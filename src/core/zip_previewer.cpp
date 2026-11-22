#include "zip_previewer.hpp"
#include "curl_utils.hpp"
#include "win32_unicode.hpp"

#include <curl/curl.h>
#include <zlib.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace opendm {

namespace {

size_t curl_buffer_writer(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::vector<uint8_t>*>(userdata);
    size_t total = size * nmemb;
    buf->insert(buf->end(), reinterpret_cast<uint8_t*>(ptr), reinterpret_cast<uint8_t*>(ptr) + total);
    return total;
}

uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64(const uint8_t* p) {
    return static_cast<uint64_t>(read_u32(p)) |
           (static_cast<uint64_t>(read_u32(p + 4)) << 32);
}

void parse_zip64_extra(const uint8_t* extra, uint16_t extra_len,
                       uint64_t& uncomp_sz, uint64_t& comp_sz, uint64_t& lhof) {
    size_t pos = 0;
    while (pos + 4 <= extra_len) {
        uint16_t tag = read_u16(extra + pos);
        uint16_t dsz = read_u16(extra + pos + 2);
        pos += 4;
        if (pos + dsz > extra_len) break;
        if (tag == 0x0001) {
            size_t ep = pos;
            if (uncomp_sz == 0xFFFFFFFFULL && ep + 8 <= pos + dsz) {
                uncomp_sz = read_u64(extra + ep);
                ep += 8;
            }
            if (comp_sz == 0xFFFFFFFFULL && ep + 8 <= pos + dsz) {
                comp_sz = read_u64(extra + ep);
                ep += 8;
            }
            if (lhof == 0xFFFFFFFFULL && ep + 8 <= pos + dsz) {
                lhof = read_u64(extra + ep);
                ep += 8;
            }
            break;
        }
        pos += dsz;
    }
}

struct StreamExtractCtx {
    uint16_t method{0};
    uint64_t expected_comp_size{0};
    uint64_t expected_uncomp_size{0};
    std::function<void(double)> progress_cb;
    std::ofstream* out_file{nullptr};
    z_stream strm{};
    bool z_init{false};
    uLong running_crc{0};
    uint64_t total_comp_seen{0};
    uint64_t total_uncomp_written{0};
    bool failed{false};
    std::string error_msg;
    std::vector<uint8_t> out_chunk;
};

size_t stream_extract_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamExtractCtx*>(userdata);
    size_t total = size * nmemb;
    if (!ctx || ctx->failed) return 0;
    if (total == 0) return 0;

    ctx->total_comp_seen += total;

    if (ctx->method == 0) {
        ctx->out_file->write(ptr, static_cast<std::streamsize>(total));
        if (ctx->out_file->fail()) {
            ctx->failed = true;
            ctx->error_msg = "Disk write failed while extracting ZIP entry";
            return 0;
        }
        ctx->running_crc = crc32(ctx->running_crc, reinterpret_cast<const Bytef*>(ptr), static_cast<uInt>(total));
        ctx->total_uncomp_written += total;
    } else if (ctx->method == 8) {
        ctx->strm.next_in = reinterpret_cast<Bytef*>(ptr);
        ctx->strm.avail_in = static_cast<uInt>(total);
        while (ctx->strm.avail_in > 0) {
            ctx->strm.next_out = reinterpret_cast<Bytef*>(ctx->out_chunk.data());
            ctx->strm.avail_out = static_cast<uInt>(ctx->out_chunk.size());
            int ret = inflate(&ctx->strm, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END) {
                ctx->failed = true;
                ctx->error_msg = "Deflate stream corrupted (inflate error " + std::to_string(ret) + ")";
                return 0;
            }
            size_t produced = ctx->out_chunk.size() - ctx->strm.avail_out;
            if (produced > 0) {
                ctx->out_file->write(reinterpret_cast<const char*>(ctx->out_chunk.data()), static_cast<std::streamsize>(produced));
                if (ctx->out_file->fail()) {
                    ctx->failed = true;
                    ctx->error_msg = "Disk write failed while extracting deflated ZIP entry";
                    return 0;
                }
                ctx->running_crc = crc32(ctx->running_crc, ctx->out_chunk.data(), static_cast<uInt>(produced));
                ctx->total_uncomp_written += produced;
            }
            if (ret == Z_STREAM_END) break;
        }
    }

    if (ctx->progress_cb && ctx->expected_comp_size > 0) {
        double p = std::min(1.0, static_cast<double>(ctx->total_comp_seen) / ctx->expected_comp_size);
        ctx->progress_cb(p);
    }

    return total;
}

} // namespace

ZipPreviewer::ZipPreviewer() {}

bool ZipPreviewer::fetch_range(const std::string& url, const std::string& range_str,
                              std::vector<uint8_t>& out_data, std::string& out_error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        out_error = "Failed to initialize CURL";
        return false;
    }

    out_data.clear();
    net::configure_curl_ssl(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl, CURLOPT_RANGE, range_str.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_writer);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_data);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) OpenDM/1.0");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        out_error = curl_easy_strerror(res);
        return false;
    }

    if (http_code != 200 && http_code != 206) {
        out_error = "HTTP server returned code " + std::to_string(http_code);
        return false;
    }

    return true;
}

bool ZipPreviewer::fetch_entries(const std::string& url, std::vector<ZipEntry>& out_entries, std::string& out_error) {
    out_entries.clear();

    std::vector<uint8_t> tail_buffer;
    if (!fetch_range(url, "-65558", tail_buffer, out_error)) {
        return false;
    }

    if (tail_buffer.size() < 22) {
        out_error = "File is too small to be a valid ZIP archive";
        return false;
    }

    int eocd_pos = -1;
    for (int i = static_cast<int>(tail_buffer.size()) - 22; i >= 0; --i) {
        if (tail_buffer[i] == 0x50 && tail_buffer[i + 1] == 0x4b &&
            tail_buffer[i + 2] == 0x05 && tail_buffer[i + 3] == 0x06) {
            uint16_t comment_len = read_u16(tail_buffer.data() + i + 20);
            if (static_cast<size_t>(i) + 22 + comment_len == tail_buffer.size()) {
                eocd_pos = i;
                break;
            }
        }
    }

    if (eocd_pos == -1) {
        out_error = "Could not find ZIP End of Central Directory record";
        return false;
    }

    const uint8_t* eocd = tail_buffer.data() + eocd_pos;
    uint64_t total_entries = read_u16(eocd + 10);
    uint64_t cd_size = read_u32(eocd + 12);
    uint64_t cd_offset = read_u32(eocd + 16);

    // Check for ZIP64 EOCD Locator (PK\x06\x07) 20 bytes immediately before EOCD
    if (eocd_pos >= 20) {
        const uint8_t* loc = tail_buffer.data() + eocd_pos - 20;
        if (loc[0] == 0x50 && loc[1] == 0x4b && loc[2] == 0x06 && loc[3] == 0x07) {
            uint64_t z64_eocd_offset = read_u64(loc + 8);
            std::vector<uint8_t> z64_buf;
            std::string z64_range = std::to_string(z64_eocd_offset) + "-" + std::to_string(z64_eocd_offset + 55);
            if (fetch_range(url, z64_range, z64_buf, out_error) && z64_buf.size() >= 56 &&
                z64_buf[0] == 0x50 && z64_buf[1] == 0x4b && z64_buf[2] == 0x06 && z64_buf[3] == 0x06) {
                total_entries = read_u64(z64_buf.data() + 32);
                cd_size = read_u64(z64_buf.data() + 40);
                cd_offset = read_u64(z64_buf.data() + 48);
            }
        }
    }

    if (cd_size == 0 || total_entries == 0) {
        out_entries.clear();
        return true;
    }
    if (cd_size == 0xFFFFFFFFULL || cd_offset == 0xFFFFFFFFULL) {
        out_error = "ZIP64 metadata unavailable or corrupted";
        return false;
    }

    std::vector<uint8_t> cd_buffer;
    std::string cd_range = std::to_string(cd_offset) + "-" + std::to_string(cd_offset + cd_size - 1);
    if (!fetch_range(url, cd_range, cd_buffer, out_error)) {
        return false;
    }

    if (cd_buffer.size() < cd_size) {
        out_error = "Incomplete Central Directory received from server";
        return false;
    }

    size_t offset = 0;
    while (offset + 46 <= cd_buffer.size()) {
        const uint8_t* p = cd_buffer.data() + offset;
        if (p[0] != 0x50 || p[1] != 0x4b || p[2] != 0x01 || p[3] != 0x02) {
            break;
        }

        uint16_t comp_method = read_u16(p + 10);
        uint32_t crc = read_u32(p + 16);
        uint64_t comp_size = read_u32(p + 20);
        uint64_t uncomp_size = read_u32(p + 24);
        uint16_t fn_len = read_u16(p + 28);
        uint16_t extra_len = read_u16(p + 30);
        uint16_t comment_len = read_u16(p + 32);
        uint64_t local_header_off = read_u32(p + 42);

        offset += 46;
        if (offset + fn_len + extra_len + comment_len > cd_buffer.size()) break;

        std::string filename(reinterpret_cast<const char*>(cd_buffer.data() + offset), fn_len);
        const uint8_t* extra_ptr = cd_buffer.data() + offset + fn_len;
        if (extra_len > 0 &&
            (comp_size == 0xFFFFFFFFULL || uncomp_size == 0xFFFFFFFFULL || local_header_off == 0xFFFFFFFFULL)) {
            parse_zip64_extra(extra_ptr, extra_len, uncomp_size, comp_size, local_header_off);
        }
        offset += fn_len + extra_len + comment_len;

        ZipEntry entry;
        entry.filename = filename;
        entry.compressed_size = comp_size;
        entry.uncompressed_size = uncomp_size;
        entry.compression_method = comp_method;
        entry.crc32 = crc;
        entry.local_header_offset = local_header_off;
        entry.is_directory = (!filename.empty() && filename.back() == '/');

        out_entries.push_back(std::move(entry));
    }

    return true;
}

bool ZipPreviewer::decompress_deflate(const uint8_t* in_data, size_t in_len,
                                      std::vector<uint8_t>& out_data, size_t expected_size) {
    out_data.resize(expected_size);

    z_stream strm{};
    strm.next_in = const_cast<Bytef*>(in_data);
    strm.avail_in = static_cast<uInt>(in_len);
    strm.next_out = reinterpret_cast<Bytef*>(out_data.data());
    strm.avail_out = static_cast<uInt>(expected_size);

    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        return false;
    }

    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);

    return (ret == Z_STREAM_END);
}

bool ZipPreviewer::extract_entry(const std::string& url, const ZipEntry& entry,
                                 const std::string& destination_filepath,
                                 std::function<void(double)> progress_cb,
                                 std::string& out_error) {
    std::error_code ec;
    fs::path dest_p = sys::path_from_utf8(destination_filepath);
    if (entry.is_directory) {
        fs::create_directories(dest_p, ec);
        if (ec) {
            out_error = "Failed to create directory: " + ec.message();
            return false;
        }
        return true;
    }

    std::string header_range = std::to_string(entry.local_header_offset) + "-" +
                               std::to_string(entry.local_header_offset + 29);
    std::vector<uint8_t> header_buf;
    if (!fetch_range(url, header_range, header_buf, out_error)) {
        return false;
    }

    if (header_buf.size() < 30 ||
        header_buf[0] != 0x50 || header_buf[1] != 0x4b || header_buf[2] != 0x03 || header_buf[3] != 0x04) {
        out_error = "Invalid local file header signature";
        return false;
    }

    uint16_t fn_len = read_u16(header_buf.data() + 26);
    uint16_t extra_len = read_u16(header_buf.data() + 28);

    if (dest_p.has_parent_path()) {
        fs::create_directories(dest_p.parent_path(), ec);
    }

    if (entry.compressed_size == 0) {
        std::ofstream out_file(dest_p, std::ios::binary | std::ios::trunc);
        if (!out_file.is_open()) {
            out_error = "Failed to create 0-byte file";
            return false;
        }
        out_file.close();
        if (progress_cb) progress_cb(1.0);
        return true;
    }

    if (entry.compression_method != 0 && entry.compression_method != 8) {
        out_error = "Unsupported ZIP compression method: " + std::to_string(entry.compression_method);
        return false;
    }

    uint64_t payload_start = entry.local_header_offset + 30ULL + fn_len + extra_len;
    uint64_t payload_end = payload_start + entry.compressed_size - 1;
    std::string payload_range = std::to_string(payload_start) + "-" + std::to_string(payload_end);

    std::string tmp_dest = destination_filepath + ".zippart.tmp";
    fs::path tmp_p = sys::path_from_utf8(tmp_dest);
    std::ofstream out_file(tmp_p, std::ios::binary | std::ios::trunc);
    if (!out_file.is_open()) {
        out_error = "Failed to open destination file for writing";
        return false;
    }

    StreamExtractCtx ctx;
    ctx.method = entry.compression_method;
    ctx.expected_comp_size = entry.compressed_size;
    ctx.expected_uncomp_size = entry.uncompressed_size;
    ctx.progress_cb = progress_cb;
    ctx.out_file = &out_file;
    ctx.running_crc = crc32(0L, Z_NULL, 0);
    ctx.out_chunk.resize(65536);

    if (ctx.method == 8) {
        if (inflateInit2(&ctx.strm, -MAX_WBITS) != Z_OK) {
            out_file.close();
            fs::remove(tmp_p, ec);
            out_error = "Failed to initialize deflate decoder";
            return false;
        }
        ctx.z_init = true;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        if (ctx.z_init) inflateEnd(&ctx.strm);
        out_file.close();
        fs::remove(tmp_p, ec);
        out_error = "Failed to initialize CURL";
        return false;
    }

    net::configure_curl_ssl(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_RANGE, payload_range.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_extract_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) OpenDM/1.0");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (ctx.z_init) {
        inflateEnd(&ctx.strm);
        ctx.z_init = false;
    }

    out_file.flush();
    bool write_ok = out_file.good();
    out_file.close();

    if (ctx.failed || res != CURLE_OK || (http_code != 200 && http_code != 206) || !write_ok) {
        fs::remove(tmp_p, ec);
        out_error = !ctx.error_msg.empty() ? ctx.error_msg :
            ((res != CURLE_OK) ? curl_easy_strerror(res) : "HTTP error " + std::to_string(http_code));
        return false;
    }

    if (ctx.total_comp_seen < entry.compressed_size || ctx.total_uncomp_written != entry.uncompressed_size) {
        fs::remove(tmp_p, ec);
        out_error = "Incomplete ZIP entry size mismatch";
        return false;
    }

    if (entry.crc32 != 0 && static_cast<uint32_t>(ctx.running_crc) != entry.crc32) {
        fs::remove(tmp_p, ec);
        out_error = "ZIP entry CRC32 verification failed";
        return false;
    }

    if (!sys::atomic_replace_file(tmp_p, dest_p, ec)) {
        fs::remove(tmp_p, ec);
        out_error = "Failed to move extracted file to destination";
        return false;
    }

    if (progress_cb) progress_cb(1.0);
    return true;
}

bool ZipPreviewer::extract_entry(const std::string& url, const ZipEntry& entry,
                                 const std::string& destination_filepath,
                                 std::string& out_error) {
    return extract_entry(url, entry, destination_filepath, nullptr, out_error);
}

} // namespace opendm
