#include "zip_previewer.hpp"
#include <curl/curl.h>
#include <zlib.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <filesystem>

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
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_RANGE, range_str.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_writer);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_data);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) OpenDM/1.0");

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

    // IDM algorithm: Fetch the last 65,536 bytes of the archive
    std::vector<uint8_t> tail_buffer;
    if (!fetch_range(url, "-65536", tail_buffer, out_error)) {
        return false;
    }

    if (tail_buffer.size() < 22) {
        out_error = "File is too small to be a valid ZIP archive";
        return false;
    }

    // Scan backwards for End of Central Directory signature: 0x06054b50 ("PK\x05\x06")
    int eocd_pos = -1;
    for (int i = static_cast<int>(tail_buffer.size()) - 22; i >= 0; --i) {
        if (tail_buffer[i] == 0x50 && tail_buffer[i + 1] == 0x4b &&
            tail_buffer[i + 2] == 0x05 && tail_buffer[i + 3] == 0x06) {
            eocd_pos = i;
            break;
        }
    }

    if (eocd_pos == -1) {
        out_error = "Could not find ZIP End of Central Directory record";
        return false;
    }

    const uint8_t* eocd = tail_buffer.data() + eocd_pos;
    uint16_t total_entries = read_u16(eocd + 10);
    uint32_t cd_size = read_u32(eocd + 12);
    uint32_t cd_offset = read_u32(eocd + 16);

    std::vector<uint8_t> cd_buffer;
    // Check if Central Directory was fully contained in our tail buffer
    int64_t tail_start_in_file = -1; // If we know total file size
    // Alternatively, fetch the exact byte range for Central Directory:
    std::string cd_range = std::to_string(cd_offset) + "-" + std::to_string(cd_offset + cd_size - 1);
    if (!fetch_range(url, cd_range, cd_buffer, out_error)) {
        return false;
    }

    if (cd_buffer.size() < cd_size) {
        out_error = "Incomplete Central Directory received from server";
        return false;
    }

    // Parse Central Directory entries ("PK\x01\x02" = 0x02014b50)
    size_t offset = 0;
    while (offset + 46 <= cd_buffer.size()) {
        const uint8_t* p = cd_buffer.data() + offset;
        if (p[0] != 0x50 || p[1] != 0x4b || p[2] != 0x01 || p[3] != 0x02) {
            break; // End of Central Directory records
        }

        uint16_t comp_method = read_u16(p + 10);
        uint32_t crc = read_u32(p + 16);
        uint32_t comp_size = read_u32(p + 20);
        uint32_t uncomp_size = read_u32(p + 24);
        uint16_t fn_len = read_u16(p + 28);
        uint16_t extra_len = read_u16(p + 30);
        uint16_t comment_len = read_u16(p + 32);
        uint32_t local_header_off = read_u32(p + 42);

        offset += 46;
        if (offset + fn_len > cd_buffer.size()) break;

        std::string filename(reinterpret_cast<const char*>(cd_buffer.data() + offset), fn_len);
        offset += fn_len + extra_len + comment_len;

        ZipEntry entry;
        entry.filename = filename;
        entry.compressed_size = comp_size;
        entry.uncompressed_size = uncomp_size;
        entry.compression_method = comp_method;
        entry.crc32 = crc;
        entry.local_header_offset = local_header_off;
        entry.is_directory = (!filename.empty() && filename.back() == '/');

        out_entries.push_back(entry);
    }

    return true;
}

bool ZipPreviewer::decompress_deflate(const uint8_t* in_data, size_t in_len,
                                     std::vector<uint8_t>& out_data, size_t expected_size) {
    out_data.resize(expected_size);

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    strm.next_in = const_cast<Bytef*>(in_data);
    strm.avail_in = static_cast<uInt>(in_len);
    strm.next_out = reinterpret_cast<Bytef*>(out_data.data());
    strm.avail_out = static_cast<uInt>(expected_size);

    // Negative windowBits (-MAX_WBITS) tells zlib to expect raw DEFLATE stream without header/checksum
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        return false;
    }

    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);

    return (ret == Z_STREAM_END || ret == Z_OK);
}

bool ZipPreviewer::extract_entry(const std::string& url, const ZipEntry& entry,
                                 const std::string& destination_filepath,
                                 std::function<void(double)> progress_cb,
                                 std::string& out_error) {
    if (entry.is_directory) {
        std::filesystem::create_directories(destination_filepath);
        return true;
    }

    // Fetch the local file header (30 bytes minimum)
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

    // Calculate exact byte range of the compressed payload:
    uint32_t payload_start = entry.local_header_offset + 30 + fn_len + extra_len;
    uint32_t payload_end = payload_start + entry.compressed_size - 1;

    std::string payload_range = std::to_string(payload_start) + "-" + std::to_string(payload_end);
    std::vector<uint8_t> payload_buf;
    if (!fetch_range(url, payload_range, payload_buf, out_error)) {
        return false;
    }

    if (payload_buf.size() < entry.compressed_size) {
        out_error = "Incomplete payload received";
        return false;
    }

    // Ensure parent directory exists
    std::filesystem::path dest_p(destination_filepath);
    if (dest_p.has_parent_path()) {
        std::filesystem::create_directories(dest_p.parent_path());
    }

    std::ofstream out_file(destination_filepath, std::ios::binary);
    if (!out_file.is_open()) {
        out_error = "Failed to open destination file for writing";
        return false;
    }

    if (entry.compression_method == 0) {
        // Stored (no compression)
        out_file.write(reinterpret_cast<const char*>(payload_buf.data()), payload_buf.size());
    } else if (entry.compression_method == 8) {
        // Deflated
        std::vector<uint8_t> decompressed;
        if (!decompress_deflate(payload_buf.data(), payload_buf.size(), decompressed, entry.uncompressed_size)) {
            out_error = "Deflate decompression failed";
            return false;
        }
        out_file.write(reinterpret_cast<const char*>(decompressed.data()), decompressed.size());
    } else {
        out_error = "Unsupported ZIP compression method: " + std::to_string(entry.compression_method);
        return false;
    }

    out_file.close();
    return true;
}

bool ZipPreviewer::extract_entry(const std::string& url, const ZipEntry& entry,
                                 const std::string& destination_filepath,
                                 std::string& out_error) {
    return extract_entry(url, entry, destination_filepath, nullptr, out_error);
}

} // namespace opendm
