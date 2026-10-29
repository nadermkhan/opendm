#include "media_muxer.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include <sys/wait.h>

namespace opendm {

namespace {

inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           (static_cast<uint32_t>(p[3]));
}

inline uint64_t read_be64(const uint8_t* p) {
    return (static_cast<uint64_t>(read_be32(p)) << 32) | read_be32(p + 4);
}

inline void write_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

inline void write_be64(uint8_t* p, uint64_t v) {
    write_be32(p, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFF));
    write_be32(p + 4, static_cast<uint32_t>(v & 0xFFFFFFFF));
}

static std::string escape_shell_arg(const std::string& arg) {
    std::string escaped = "'";
    for (char c : arg) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped += c;
        }
    }
    escaped += "'";
    return escaped;
}

} // namespace

std::string MediaMuxer::find_ffmpeg() {
    const char* home = getenv("HOME");
    std::vector<std::string> search_paths = {
        home ? std::string(home) + "/.local/bin/ffmpeg" : "",
        "/usr/local/bin/ffmpeg",
        "/usr/bin/ffmpeg",
        "/bin/ffmpeg",
        "/snap/bin/ffmpeg"
    };
    for (const auto& p : search_paths) {
        if (!p.empty() && access(p.c_str(), X_OK) == 0) {
            return p;
        }
    }
    if (system("which ffmpeg >/dev/null 2>&1") == 0) {
        return "ffmpeg";
    }
    return "";
}

std::vector<MediaMuxer::BoxInfo> MediaMuxer::scan_mp4_boxes(const std::string& file_path) {
    std::vector<BoxInfo> boxes;
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return boxes;

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    uint64_t current_offset = 0;
    while (current_offset + 8 <= file_size) {
        file.seekg(current_offset);
        uint8_t header[16];
        file.read(reinterpret_cast<char*>(header), 8);
        if (file.gcount() < 8) break;

        uint64_t box_size = read_be32(header);
        std::string box_type(reinterpret_cast<char*>(header + 4), 4);
        uint64_t header_size = 8;

        if (box_size == 1) { // 64-bit size
            file.read(reinterpret_cast<char*>(header + 8), 8);
            if (file.gcount() < 8) break;
            box_size = read_be64(header + 8);
            header_size = 16;
        } else if (box_size == 0) { // extends to EOF
            box_size = file_size - current_offset;
        }

        if (box_size < header_size || current_offset + box_size > file_size) {
            break;
        }

        BoxInfo info;
        info.type = box_type;
        info.offset = current_offset;
        info.size = box_size;
        info.payload_offset = current_offset + header_size;
        info.payload_size = box_size - header_size;
        boxes.push_back(info);

        current_offset += box_size;
    }

    return boxes;
}

void MediaMuxer::set_tkhd_track_id(std::vector<uint8_t>& trak_data, uint32_t new_id) {
    // Search for 'tkhd' box inside trak
    for (size_t i = 0; i + 8 <= trak_data.size(); ++i) {
        if (std::memcmp(&trak_data[i + 4], "tkhd", 4) == 0) {
            uint32_t box_size = read_be32(&trak_data[i]);
            if (i + box_size > trak_data.size()) break;

            uint8_t version = trak_data[i + 8];
            // If version 0: track_id is at offset 12 in payload (20 from start of box)
            // If version 1: track_id is at offset 20 in payload (28 from start of box)
            size_t id_offset = (version == 0) ? (i + 20) : (i + 28);
            if (id_offset + 4 <= trak_data.size()) {
                write_be32(&trak_data[id_offset], new_id);
            }
            break;
        }
    }
}

bool MediaMuxer::rewrite_stco_co64(
    std::vector<uint8_t>& trak_data,
    uint64_t old_mdat_payload_offset,
    uint64_t new_mdat_payload_offset) {
    // Scan for 'stco' or 'co64' boxes inside trak
    for (size_t i = 0; i + 8 <= trak_data.size(); ++i) {
        if (std::memcmp(&trak_data[i + 4], "stco", 4) == 0) {
            uint32_t box_size = read_be32(&trak_data[i]);
            if (i + box_size > trak_data.size() || box_size < 16) continue;

            uint32_t entry_count = read_be32(&trak_data[i + 12]);
            size_t table_start = i + 16;
            if (table_start + (entry_count * 4) > trak_data.size()) continue;

            for (uint32_t e = 0; e < entry_count; ++e) {
                size_t entry_pos = table_start + (e * 4);
                uint32_t old_off = read_be32(&trak_data[entry_pos]);
                int64_t delta = static_cast<int64_t>(old_off) - static_cast<int64_t>(old_mdat_payload_offset);
                uint64_t new_off = new_mdat_payload_offset + delta;
                if (new_off > 0xFFFFFFFFULL) {
                    // Exceeds 32-bit: stco overflow
                    return false;
                }
                write_be32(&trak_data[entry_pos], static_cast<uint32_t>(new_off));
            }
            return true;
        } else if (std::memcmp(&trak_data[i + 4], "co64", 4) == 0) {
            uint32_t box_size = read_be32(&trak_data[i]);
            if (i + box_size > trak_data.size() || box_size < 16) continue;

            uint32_t entry_count = read_be32(&trak_data[i + 12]);
            size_t table_start = i + 16;
            if (table_start + (entry_count * 8) > trak_data.size()) continue;

            for (uint32_t e = 0; e < entry_count; ++e) {
                size_t entry_pos = table_start + (e * 8);
                uint64_t old_off = read_be64(&trak_data[entry_pos]);
                int64_t delta = static_cast<int64_t>(old_off) - static_cast<int64_t>(old_mdat_payload_offset);
                uint64_t new_off = new_mdat_payload_offset + delta;
                write_be64(&trak_data[entry_pos], new_off);
            }
            return true;
        }
    }
    return false;
}

bool MediaMuxer::try_external_ffmpeg(
    const std::string& video_file,
    const std::string& audio_file,
    const std::string& output_file,
    MuxProgressCallback progress_cb) {
    std::string ffmpeg_bin = find_ffmpeg();
    if (ffmpeg_bin.empty()) {
        return false;
    }

    std::string cmd = ffmpeg_bin + " -y -i " + escape_shell_arg(video_file) +
                      " -i " + escape_shell_arg(audio_file) +
                      " -c copy -movflags +faststart " +
                      escape_shell_arg(output_file) + " >/dev/null 2>&1";
    int ret = system(cmd.c_str());
    if (ret == 0) {
        if (progress_cb) progress_cb(100.0);
        return true;
    }
    return false;
}

bool MediaMuxer::mux_mp4_and_mp4(
    const std::string& video_file,
    const std::string& audio_file,
    const std::string& output_file,
    MuxProgressCallback progress_cb,
    std::string* out_error) {

    // First try external ffmpeg if present for fastest native trans-muxing
    if (try_external_ffmpeg(video_file, audio_file, output_file, progress_cb)) {
        return true;
    }

    // Direct clean-room ISOBMFF atom muxer matching IDM idmvconv.dll
    auto v_boxes = scan_mp4_boxes(video_file);
    auto a_boxes = scan_mp4_boxes(audio_file);

    if (v_boxes.empty() || a_boxes.empty()) {
        if (out_error) *out_error = "Failed to scan MP4 boxes in video or audio stream";
        return false;
    }

    // Guard against fragmented MP4 (fMP4/DASH) streams:
    // Fragmented MP4 stores media data across multiple moof+mdat fragments,
    // which cannot be multiplexed as a single monolithic mdat without ffmpeg.
    for (const auto& b : v_boxes) {
        if (b.type == "moof") {
            if (out_error) *out_error = "Fragmented MP4 (DASH) multiplexing requires external ffmpeg";
            return false;
        }
    }
    for (const auto& b : a_boxes) {
        if (b.type == "moof") {
            if (out_error) *out_error = "Fragmented MP4 (DASH) multiplexing requires external ffmpeg";
            return false;
        }
    }

    const BoxInfo* v_ftyp = nullptr;
    const BoxInfo* v_moov = nullptr;
    const BoxInfo* v_mdat = nullptr;
    for (const auto& b : v_boxes) {
        if (b.type == "ftyp" && !v_ftyp) v_ftyp = &b;
        else if (b.type == "moov" && !v_moov) v_moov = &b;
        else if (b.type == "mdat" && !v_mdat) v_mdat = &b;
    }

    const BoxInfo* a_moov = nullptr;
    const BoxInfo* a_mdat = nullptr;
    for (const auto& b : a_boxes) {
        if (b.type == "moov" && !a_moov) a_moov = &b;
        else if (b.type == "mdat" && !a_mdat) a_mdat = &b;
    }

    if (!v_ftyp || !v_moov || !v_mdat || !a_moov || !a_mdat) {
        if (out_error) *out_error = "Missing essential MP4 atoms (ftyp/moov/mdat)";
        return false;
    }

    // Verify that the video mdat actually contains the media payload,
    // rather than just an initial fragment.
    std::error_code ec;
    uint64_t v_file_size = std::filesystem::file_size(video_file, ec);
    if (!ec && v_file_size > 1024 * 1024 && v_mdat->payload_size < v_file_size / 2) {
        if (out_error) *out_error = "Single mdat does not contain main video payload (fragmented or corrupted stream)";
        return false;
    }

    // Read video ftyp
    std::ifstream v_in(video_file, std::ios::binary);
    std::ifstream a_in(audio_file, std::ios::binary);
    if (!v_in.is_open() || !a_in.is_open()) {
        if (out_error) *out_error = "Could not open source media files";
        return false;
    }

    std::vector<uint8_t> ftyp_data(v_ftyp->size);
    v_in.seekg(v_ftyp->offset);
    v_in.read(reinterpret_cast<char*>(ftyp_data.data()), v_ftyp->size);

    // Read moov boxes
    std::vector<uint8_t> v_moov_data(v_moov->payload_size);
    v_in.seekg(v_moov->payload_offset);
    v_in.read(reinterpret_cast<char*>(v_moov_data.data()), v_moov->payload_size);

    std::vector<uint8_t> a_moov_data(a_moov->payload_size);
    a_in.seekg(a_moov->payload_offset);
    a_in.read(reinterpret_cast<char*>(a_moov_data.data()), a_moov->payload_size);

    // Extract trak from video moov
    std::vector<uint8_t> v_trak;
    std::vector<uint8_t> v_mvhd;
    for (size_t i = 0; i + 8 <= v_moov_data.size();) {
        uint32_t b_size = read_be32(&v_moov_data[i]);
        if (b_size < 8 || i + b_size > v_moov_data.size()) break;
        if (std::memcmp(&v_moov_data[i + 4], "mvhd", 4) == 0) {
            v_mvhd.assign(&v_moov_data[i], &v_moov_data[i + b_size]);
        } else if (std::memcmp(&v_moov_data[i + 4], "trak", 4) == 0 && v_trak.empty()) {
            v_trak.assign(&v_moov_data[i], &v_moov_data[i + b_size]);
        }
        i += b_size;
    }

    // Extract trak from audio moov
    std::vector<uint8_t> a_trak;
    for (size_t i = 0; i + 8 <= a_moov_data.size();) {
        uint32_t b_size = read_be32(&a_moov_data[i]);
        if (b_size < 8 || i + b_size > a_moov_data.size()) break;
        if (std::memcmp(&a_moov_data[i + 4], "trak", 4) == 0 && a_trak.empty()) {
            a_trak.assign(&a_moov_data[i], &a_moov_data[i + b_size]);
        }
        i += b_size;
    }

    if (v_trak.empty() || a_trak.empty() || v_mvhd.empty()) {
        if (out_error) *out_error = "Could not locate tracks or movie header";
        return false;
    }

    // Set audio track ID to 2 to avoid collision with video track ID 1
    set_tkhd_track_id(a_trak, 2);

    // Update next_track_id in mvhd to 3
    if (v_mvhd.size() >= 108) {
        uint8_t version = v_mvhd[8];
        size_t next_id_offset = (version == 0) ? (8 + 96) : (8 + 108);
        if (next_id_offset + 4 <= v_mvhd.size()) {
            write_be32(&v_mvhd[next_id_offset], 3);
        }
    }

    // Calculate new mdat layout
    uint64_t total_media_payload = v_mdat->payload_size + a_mdat->payload_size;
    bool extended_mdat = (total_media_payload + 8 > 0xFFFFFFFFULL);
    uint64_t mdat_header_size = extended_mdat ? 16 : 8;

    uint64_t new_mdat_box_offset = ftyp_data.size();
    uint64_t new_v_mdat_payload_offset = new_mdat_box_offset + mdat_header_size;
    uint64_t new_a_mdat_payload_offset = new_v_mdat_payload_offset + v_mdat->payload_size;

    // Rewrite chunk offsets
    if (!rewrite_stco_co64(v_trak, v_mdat->payload_offset, new_v_mdat_payload_offset)) {
        if (out_error) *out_error = "Failed to rewrite video chunk offsets";
        return false;
    }
    if (!rewrite_stco_co64(a_trak, a_mdat->payload_offset, new_a_mdat_payload_offset)) {
        if (out_error) *out_error = "Failed to rewrite audio chunk offsets";
        return false;
    }

    // Assemble new moov
    uint32_t moov_payload_size = static_cast<uint32_t>(v_mvhd.size() + v_trak.size() + a_trak.size());
    uint32_t moov_total_size = moov_payload_size + 8;
    std::vector<uint8_t> new_moov(moov_total_size);
    write_be32(&new_moov[0], moov_total_size);
    std::memcpy(&new_moov[4], "moov", 4);
    size_t write_pos = 8;
    std::memcpy(&new_moov[write_pos], v_mvhd.data(), v_mvhd.size());
    write_pos += v_mvhd.size();
    std::memcpy(&new_moov[write_pos], v_trak.data(), v_trak.size());
    write_pos += v_trak.size();
    std::memcpy(&new_moov[write_pos], a_trak.data(), a_trak.size());

    // Write output file
    std::ofstream out(output_file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (out_error) *out_error = "Could not open output destination file";
        return false;
    }

    // 1. Write ftyp
    out.write(reinterpret_cast<const char*>(ftyp_data.data()), ftyp_data.size());

    // 2. Write mdat header
    if (extended_mdat) {
        uint8_t mdat_hdr[16];
        write_be32(&mdat_hdr[0], 1);
        std::memcpy(&mdat_hdr[4], "mdat", 4);
        write_be64(&mdat_hdr[8], total_media_payload + 16);
        out.write(reinterpret_cast<const char*>(mdat_hdr), 16);
    } else {
        uint8_t mdat_hdr[8];
        write_be32(&mdat_hdr[0], static_cast<uint32_t>(total_media_payload + 8));
        std::memcpy(&mdat_hdr[4], "mdat", 4);
        out.write(reinterpret_cast<const char*>(mdat_hdr), 8);
    }

    // 3. Stream copy video mdat payload
    const size_t BUFFER_SIZE = 128 * 1024;
    std::vector<char> buffer(BUFFER_SIZE);

    v_in.seekg(v_mdat->payload_offset);
    uint64_t v_copied = 0;
    while (v_copied < v_mdat->payload_size) {
        size_t to_read = static_cast<size_t>(std::min<uint64_t>(BUFFER_SIZE, v_mdat->payload_size - v_copied));
        v_in.read(buffer.data(), to_read);
        out.write(buffer.data(), to_read);
        v_copied += to_read;

        if (progress_cb && total_media_payload > 0) {
            double pct = (static_cast<double>(v_copied) / total_media_payload) * 90.0;
            progress_cb(pct);
        }
    }

    // 4. Stream copy audio mdat payload
    a_in.seekg(a_mdat->payload_offset);
    uint64_t a_copied = 0;
    while (a_copied < a_mdat->payload_size) {
        size_t to_read = static_cast<size_t>(std::min<uint64_t>(BUFFER_SIZE, a_mdat->payload_size - a_copied));
        a_in.read(buffer.data(), to_read);
        out.write(buffer.data(), to_read);
        a_copied += to_read;

        if (progress_cb && total_media_payload > 0) {
            double pct = (static_cast<double>(v_copied + a_copied) / total_media_payload) * 90.0;
            progress_cb(pct);
        }
    }

    // 5. Write moov box
    out.write(reinterpret_cast<const char*>(new_moov.data()), new_moov.size());
    out.flush();

    if (progress_cb) progress_cb(100.0);
    return true;
}

bool MediaMuxer::concat_ts_segments(
    const std::vector<std::string>& segment_files,
    const std::string& output_file,
    MuxProgressCallback progress_cb,
    std::string* out_error) {

    if (segment_files.empty()) {
        if (out_error) *out_error = "No TS segments provided";
        return false;
    }

    std::ofstream out(output_file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (out_error) *out_error = "Could not open output TS file";
        return false;
    }

    const size_t BUFFER_SIZE = 64 * 1024;
    std::vector<char> buffer(BUFFER_SIZE);

    size_t total_files = segment_files.size();
    for (size_t idx = 0; idx < total_files; ++idx) {
        std::ifstream in(segment_files[idx], std::ios::binary);
        if (!in.is_open()) continue;

        while (in.read(buffer.data(), BUFFER_SIZE) || in.gcount() > 0) {
            out.write(buffer.data(), in.gcount());
        }

        if (progress_cb) {
            double pct = (static_cast<double>(idx + 1) / total_files) * 100.0;
            progress_cb(pct);
        }
    }

    out.flush();
    return true;
}

bool MediaMuxer::convert_ts_to_mp4(
    const std::string& ts_file,
    const std::string& output_file,
    MuxProgressCallback progress_cb,
    std::string* out_error) {

    // First try ffmpeg if installed
    std::string ffmpeg_bin = find_ffmpeg();
    if (!ffmpeg_bin.empty()) {
        std::string cmd = ffmpeg_bin + " -y -i " + escape_shell_arg(ts_file) +
                          " -c copy -movflags +faststart " +
                          escape_shell_arg(output_file) + " >/dev/null 2>&1";
        if (system(cmd.c_str()) == 0) {
            if (progress_cb) progress_cb(100.0);
            return true;
        }
    }

    // Try VLC if installed
    if (system("which vlc >/dev/null 2>&1") == 0) {
        std::string cmd = "vlc -I dummy " + escape_shell_arg(ts_file) +
                          " --sout \"#std{access=file,mux=mp4,dst=" +
                          escape_shell_arg(output_file) + "}\" vlc://quit >/dev/null 2>&1";
        if (system(cmd.c_str()) == 0) {
            if (progress_cb) progress_cb(100.0);
            return true;
        }
    }

    if (out_error) *out_error = "TS to MP4 conversion requires ffmpeg or VLC command-line tools";
    return false;
}

bool MediaMuxer::mux_streams(
    const std::string& video_file,
    const std::string& audio_file,
    const std::string& output_file,
    MuxProgressCallback progress_cb,
    std::string* out_error) {

    // If both files are MP4/M4V/M4A, mux_mp4_and_mp4 tries ffmpeg first and falls back to ISOBMFF
    return mux_mp4_and_mp4(video_file, audio_file, output_file, progress_cb, out_error);
}

} // namespace opendm
