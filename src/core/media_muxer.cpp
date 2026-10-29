#include "media_muxer.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>

namespace opendm {

namespace {

inline uint16_t read_be16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           (static_cast<uint32_t>(p[3]));
}

inline uint64_t read_be64(const uint8_t* p) {
    return (static_cast<uint64_t>(read_be32(p)) << 32) | read_be32(p + 4);
}

inline void write_be16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
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

struct ChildBox {
    std::string type;
    size_t offset;
    size_t size;
    size_t payload_offset;
    size_t payload_size;
};

static std::vector<ChildBox> parse_children(const uint8_t* data, size_t start, size_t end) {
    std::vector<ChildBox> res;
    size_t cur = start;
    while (cur + 8 <= end) {
        uint64_t sz = read_be32(data + cur);
        std::string type(reinterpret_cast<const char*>(data + cur + 4), 4);
        size_t hdr = 8;
        if (sz == 1) {
            if (cur + 16 > end) break;
            sz = read_be64(data + cur + 8);
            hdr = 16;
        } else if (sz == 0) {
            sz = end - cur;
        }
        if (sz < hdr || cur + sz > end) break;
        res.push_back({type, cur, static_cast<size_t>(sz), cur + hdr, static_cast<size_t>(sz - hdr)});
        cur += static_cast<size_t>(sz);
    }
    return res;
}

static void append_box(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& payload) {
    size_t start = out.size();
    out.resize(start + 8 + payload.size());
    write_be32(&out[start], static_cast<uint32_t>(8 + payload.size()));
    std::memcpy(&out[start + 4], type, 4);
    if (!payload.empty()) {
        std::memcpy(&out[start + 8], payload.data(), payload.size());
    }
}

} // namespace

std::string MediaMuxer::find_ffmpeg() {
    static std::mutex s_ffmpeg_mutex;
    static bool s_checked = false;
    static std::string s_cached_path;

    std::lock_guard<std::mutex> lock(s_ffmpeg_mutex);
    if (s_checked) {
        return s_cached_path;
    }
    s_checked = true;

    const char* home = getenv("HOME");
    std::vector<std::string> search_paths = {
        home ? std::string(home) + "/.local/bin/ffmpeg" : "",
        "/usr/local/bin/ffmpeg",
        "/usr/bin/ffmpeg",
        "/bin/ffmpeg",
        "/snap/bin/ffmpeg"
    };

    const char* path_env = getenv("PATH");
    if (path_env) {
        std::stringstream ss(path_env);
        std::string dir;
        while (std::getline(ss, dir, ':')) {
            if (!dir.empty()) {
                search_paths.push_back(dir + "/ffmpeg");
            }
        }
    }

    for (const auto& p : search_paths) {
        if (!p.empty() && access(p.c_str(), X_OK) == 0) {
            s_cached_path = p;
            return s_cached_path;
        }
    }
    return "";
}

std::vector<MediaMuxer::BoxInfo> MediaMuxer::scan_mp4_boxes(const std::string& file_path) {
    std::vector<BoxInfo> boxes;
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return boxes;

    uint64_t file_size = static_cast<uint64_t>(file.tellg());
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

        if (box_size == 1) {
            file.read(reinterpret_cast<char*>(header + 8), 8);
            if (file.gcount() < 8) break;
            box_size = read_be64(header + 8);
            header_size = 16;
        } else if (box_size == 0) {
            box_size = file_size - current_offset;
        }

        if (box_size < header_size || current_offset + box_size > file_size) {
            break;
        }

        boxes.push_back({box_type, current_offset, box_size, current_offset + header_size, box_size - header_size});
        current_offset += box_size;
    }
    return boxes;
}

void MediaMuxer::set_tkhd_track_id(std::vector<uint8_t>& trak_data, uint32_t new_id) {
    if (trak_data.size() < 8) return;
    auto children = parse_children(trak_data.data(), 8, trak_data.size());
    for (const auto& c : children) {
        if (c.type == "tkhd" && c.payload_size >= 16) {
            uint8_t version = trak_data[c.payload_offset];
            size_t id_offset = (version == 0) ? (c.payload_offset + 12) : (c.payload_offset + 20);
            if (id_offset + 4 <= c.offset + c.size) {
                write_be32(&trak_data[id_offset], new_id);
            }
            return;
        }
    }
}

bool MediaMuxer::rewrite_stco_co64(
    std::vector<uint8_t>& trak_data,
    uint64_t old_mdat_payload_offset,
    uint64_t new_mdat_payload_offset) {

    if (trak_data.size() < 8) return false;

    // Walk trak -> mdia -> minf -> stbl hierarchically
    auto trak_children = parse_children(trak_data.data(), 8, trak_data.size());
    for (const auto& mdia : trak_children) {
        if (mdia.type != "mdia") continue;
        auto mdia_children = parse_children(trak_data.data(), mdia.payload_offset, mdia.offset + mdia.size);
        for (const auto& minf : mdia_children) {
            if (minf.type != "minf") continue;
            auto minf_children = parse_children(trak_data.data(), minf.payload_offset, minf.offset + minf.size);
            for (const auto& stbl : minf_children) {
                if (stbl.type != "stbl") continue;
                auto stbl_children = parse_children(trak_data.data(), stbl.payload_offset, stbl.offset + stbl.size);
                for (const auto& box : stbl_children) {
                    if (box.type == "stco" && box.payload_size >= 8) {
                        uint32_t entry_count = read_be32(&trak_data[box.payload_offset + 4]);
                        size_t table_start = box.payload_offset + 8;
                        if (table_start + static_cast<size_t>(entry_count) * 4 > box.offset + box.size) return false;

                        for (uint32_t e = 0; e < entry_count; ++e) {
                            size_t pos = table_start + e * 4;
                            uint32_t old_off = read_be32(&trak_data[pos]);
                            int64_t delta = static_cast<int64_t>(old_off) - static_cast<int64_t>(old_mdat_payload_offset);
                            uint64_t new_off = static_cast<uint64_t>(static_cast<int64_t>(new_mdat_payload_offset) + delta);
                            if (new_off > 0xFFFFFFFFULL) {
                                return false;
                            }
                            write_be32(&trak_data[pos], static_cast<uint32_t>(new_off));
                        }
                        return true;
                    } else if (box.type == "co64" && box.payload_size >= 8) {
                        uint32_t entry_count = read_be32(&trak_data[box.payload_offset + 4]);
                        size_t table_start = box.payload_offset + 8;
                        if (table_start + static_cast<size_t>(entry_count) * 8 > box.offset + box.size) return false;

                        for (uint32_t e = 0; e < entry_count; ++e) {
                            size_t pos = table_start + e * 8;
                            uint64_t old_off = read_be64(&trak_data[pos]);
                            int64_t delta = static_cast<int64_t>(old_off) - static_cast<int64_t>(old_mdat_payload_offset);
                            uint64_t new_off = static_cast<uint64_t>(static_cast<int64_t>(new_mdat_payload_offset) + delta);
                            write_be64(&trak_data[pos], new_off);
                        }
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool MediaMuxer::defragment_mp4_if_needed(
    const std::string& input_file,
    const std::string& temp_defrag_file,
    std::string& out_usable_file) {

    auto boxes = scan_mp4_boxes(input_file);
    bool has_moof = false;
    const BoxInfo* ftyp_box = nullptr;
    const BoxInfo* moov_box = nullptr;

    for (const auto& b : boxes) {
        if (b.type == "moof") has_moof = true;
        else if (b.type == "ftyp" && !ftyp_box) ftyp_box = &b;
        else if (b.type == "moov" && !moov_box) moov_box = &b;
    }

    if (!has_moof) {
        out_usable_file = input_file;
        return true;
    }
    if (!ftyp_box || !moov_box) {
        return false;
    }

    std::ifstream in(input_file, std::ios::binary);
    if (!in.is_open()) return false;

    std::vector<uint8_t> ftyp_data(ftyp_box->size);
    in.seekg(ftyp_box->offset);
    in.read(reinterpret_cast<char*>(ftyp_data.data()), ftyp_box->size);

    std::vector<uint8_t> moov_payload(moov_box->payload_size);
    in.seekg(moov_box->payload_offset);
    in.read(reinterpret_cast<char*>(moov_payload.data()), moov_box->payload_size);

    // Parse default sample values from mvex -> trex if present
    uint32_t default_sample_duration = 0;
    uint32_t default_sample_size = 0;
    uint32_t default_sample_flags = 0;

    auto moov_children = parse_children(moov_payload.data(), 0, moov_payload.size());
    for (const auto& c : moov_children) {
        if (c.type == "mvex") {
            auto mvex_children = parse_children(moov_payload.data(), c.payload_offset, c.offset + c.size);
            for (const auto& mc : mvex_children) {
                if (mc.type == "trex" && mc.payload_size >= 24) {
                    default_sample_duration = read_be32(&moov_payload[mc.payload_offset + 12]);
                    default_sample_size = read_be32(&moov_payload[mc.payload_offset + 16]);
                    default_sample_flags = read_be32(&moov_payload[mc.payload_offset + 20]);
                }
            }
        }
    }

    struct SampleRec {
        uint32_t duration;
        uint32_t size;
        uint32_t flags;
        int32_t cts_offset;
    };
    std::vector<SampleRec> samples;
    std::vector<std::pair<uint64_t, uint64_t>> mdat_slices; // (file_offset, size)
    uint64_t total_mdat_payload = 0;
    bool has_cts = false;

    // Walk all moof + mdat pairs in sequence
    for (size_t bi = 0; bi < boxes.size(); ++bi) {
        if (boxes[bi].type != "moof") continue;
        const BoxInfo& moof = boxes[bi];
        const BoxInfo* mdat = nullptr;
        for (size_t nj = bi + 1; nj < boxes.size(); ++nj) {
            if (boxes[nj].type == "mdat") {
                mdat = &boxes[nj];
                break;
            }
            if (boxes[nj].type == "moof") break;
        }
        if (!mdat) continue;

        std::vector<uint8_t> moof_buf(moof.payload_size);
        in.seekg(moof.payload_offset);
        in.read(reinterpret_cast<char*>(moof_buf.data()), moof.payload_size);

        uint32_t tfhd_dur = default_sample_duration;
        uint32_t tfhd_sz = default_sample_size;
        uint32_t tfhd_flags_val = default_sample_flags;

        auto moof_ch = parse_children(moof_buf.data(), 0, moof_buf.size());
        for (const auto& traf : moof_ch) {
            if (traf.type != "traf") continue;
            auto traf_ch = parse_children(moof_buf.data(), traf.payload_offset, traf.offset + traf.size);
            for (const auto& tc : traf_ch) {
                if (tc.type == "tfhd" && tc.payload_size >= 8) {
                    uint32_t tf_flags = read_be32(&moof_buf[tc.payload_offset]) & 0x00FFFFFF;
                    size_t pos = tc.payload_offset + 8;
                    if (tf_flags & 0x000001) pos += 8; // base-data-offset
                    if (tf_flags & 0x000002) pos += 4; // sample-description-index
                    if ((tf_flags & 0x000008) && pos + 4 <= tc.offset + tc.size) {
                        tfhd_dur = read_be32(&moof_buf[pos]);
                        pos += 4;
                    }
                    if ((tf_flags & 0x000010) && pos + 4 <= tc.offset + tc.size) {
                        tfhd_sz = read_be32(&moof_buf[pos]);
                        pos += 4;
                    }
                    if ((tf_flags & 0x000020) && pos + 4 <= tc.offset + tc.size) {
                        tfhd_flags_val = read_be32(&moof_buf[pos]);
                        pos += 4;
                    }
                } else if (tc.type == "trun" && tc.payload_size >= 8) {
                    uint8_t tr_ver = moof_buf[tc.payload_offset];
                    uint32_t tr_flags = read_be32(&moof_buf[tc.payload_offset]) & 0x00FFFFFF;
                    uint32_t sample_count = read_be32(&moof_buf[tc.payload_offset + 4]);
                    size_t pos = tc.payload_offset + 8;
                    if (tr_flags & 0x000001) pos += 4; // data-offset
                    uint32_t first_sample_flags = tfhd_flags_val;
                    bool has_first_flags = (tr_flags & 0x000004) != 0;
                    if (has_first_flags && pos + 4 <= tc.offset + tc.size) {
                        first_sample_flags = read_be32(&moof_buf[pos]);
                        pos += 4;
                    }

                    for (uint32_t s = 0; s < sample_count; ++s) {
                        SampleRec rec{tfhd_dur, tfhd_sz, (s == 0 && has_first_flags) ? first_sample_flags : tfhd_flags_val, 0};
                        if ((tr_flags & 0x000100) && pos + 4 <= tc.offset + tc.size) {
                            rec.duration = read_be32(&moof_buf[pos]);
                            pos += 4;
                        }
                        if ((tr_flags & 0x000200) && pos + 4 <= tc.offset + tc.size) {
                            rec.size = read_be32(&moof_buf[pos]);
                            pos += 4;
                        }
                        if ((tr_flags & 0x000400) && pos + 4 <= tc.offset + tc.size) {
                            rec.flags = read_be32(&moof_buf[pos]);
                            pos += 4;
                        }
                        if ((tr_flags & 0x000800) && pos + 4 <= tc.offset + tc.size) {
                            uint32_t raw_cts = read_be32(&moof_buf[pos]);
                            rec.cts_offset = (tr_ver == 0) ? static_cast<int32_t>(raw_cts) : static_cast<int32_t>(raw_cts);
                            if (rec.cts_offset != 0) has_cts = true;
                            pos += 4;
                        }
                        samples.push_back(rec);
                    }
                }
            }
        }

        mdat_slices.push_back({mdat->payload_offset, mdat->payload_size});
        total_mdat_payload += mdat->payload_size;
    }

    if (samples.empty() || mdat_slices.empty()) {
        return false;
    }

    // Build stts payload
    std::vector<std::pair<uint32_t, uint32_t>> stts_runs;
    uint64_t total_media_ticks = 0;
    for (const auto& s : samples) {
        total_media_ticks += s.duration;
        if (!stts_runs.empty() && stts_runs.back().second == s.duration) {
            stts_runs.back().first++;
        } else {
            stts_runs.push_back({1, s.duration});
        }
    }
    std::vector<uint8_t> stts_payload(8 + stts_runs.size() * 8, 0);
    write_be32(&stts_payload[4], static_cast<uint32_t>(stts_runs.size()));
    for (size_t i = 0; i < stts_runs.size(); ++i) {
        write_be32(&stts_payload[8 + i * 8], stts_runs[i].first);
        write_be32(&stts_payload[12 + i * 8], stts_runs[i].second);
    }

    // Build stsz payload
    std::vector<uint8_t> stsz_payload(12 + samples.size() * 4, 0);
    write_be32(&stsz_payload[8], static_cast<uint32_t>(samples.size()));
    for (size_t i = 0; i < samples.size(); ++i) {
        write_be32(&stsz_payload[12 + i * 4], samples[i].size);
    }

    // Build stsc payload (1 single chunk containing all samples)
    std::vector<uint8_t> stsc_payload(20, 0);
    write_be32(&stsc_payload[4], 1);
    write_be32(&stsc_payload[8], 1);
    write_be32(&stsc_payload[12], static_cast<uint32_t>(samples.size()));
    write_be32(&stsc_payload[16], 1);

    // Build stss (sync sample table) if non-all-sync
    std::vector<uint32_t> sync_samples;
    for (size_t i = 0; i < samples.size(); ++i) {
        bool is_non_sync = ((samples[i].flags & 0x00010000) != 0) ||
                           (((samples[i].flags >> 24) & 0x03) == 1);
        if (!is_non_sync) {
            sync_samples.push_back(static_cast<uint32_t>(i + 1));
        }
    }

    // Build ctts payload if composition offsets exist
    std::vector<uint8_t> ctts_payload;
    if (has_cts) {
        std::vector<std::pair<uint32_t, int32_t>> ctts_runs;
        for (const auto& s : samples) {
            if (!ctts_runs.empty() && ctts_runs.back().second == s.cts_offset) {
                ctts_runs.back().first++;
            } else {
                ctts_runs.push_back({1, s.cts_offset});
            }
        }
        ctts_payload.assign(8 + ctts_runs.size() * 8, 0);
        ctts_payload[0] = 1; // version 1 for signed offsets
        write_be32(&ctts_payload[4], static_cast<uint32_t>(ctts_runs.size()));
        for (size_t i = 0; i < ctts_runs.size(); ++i) {
            write_be32(&ctts_payload[8 + i * 8], ctts_runs[i].first);
            write_be32(&ctts_payload[12 + i * 8], static_cast<uint32_t>(ctts_runs[i].second));
        }
    }

    // Build placeholder stco (1 entry, filled once moov size is known)
    std::vector<uint8_t> stco_payload(12, 0);
    write_be32(&stco_payload[4], 1);

    // Reconstruct moov (keep mvhd and trak with updated stbl; strip mvex)
    std::vector<uint8_t> new_moov_payload;
    for (const auto& c : moov_children) {
        if (c.type == "mvhd") {
            new_moov_payload.insert(new_moov_payload.end(),
                                    &moov_payload[c.offset],
                                    &moov_payload[c.offset + c.size]);
        } else if (c.type == "trak") {
            std::vector<uint8_t> trak_payload;
            auto trak_ch = parse_children(moov_payload.data(), c.payload_offset, c.offset + c.size);
            for (const auto& tc : trak_ch) {
                if (tc.type != "mdia") {
                    trak_payload.insert(trak_payload.end(),
                                        &moov_payload[tc.offset],
                                        &moov_payload[tc.offset + tc.size]);
                    continue;
                }
                std::vector<uint8_t> mdia_payload;
                auto mdia_ch = parse_children(moov_payload.data(), tc.payload_offset, tc.offset + tc.size);
                for (const auto& mc : mdia_ch) {
                    if (mc.type == "mdhd" && mc.payload_size >= 20) {
                        std::vector<uint8_t> mdhd_data(&moov_payload[mc.offset], &moov_payload[mc.offset + mc.size]);
                        uint8_t ver = mdhd_data[8];
                        if (ver == 0 && mdhd_data.size() >= 28) {
                            write_be32(&mdhd_data[24], static_cast<uint32_t>(total_media_ticks));
                        } else if (ver == 1 && mdhd_data.size() >= 36) {
                            write_be64(&mdhd_data[28], total_media_ticks);
                        }
                        mdia_payload.insert(mdia_payload.end(), mdhd_data.begin(), mdhd_data.end());
                        continue;
                    }
                    if (mc.type != "minf") {
                        mdia_payload.insert(mdia_payload.end(),
                                            &moov_payload[mc.offset],
                                            &moov_payload[mc.offset + mc.size]);
                        continue;
                    }
                    std::vector<uint8_t> minf_payload;
                    auto minf_ch = parse_children(moov_payload.data(), mc.payload_offset, mc.offset + mc.size);
                    for (const auto& mfc : minf_ch) {
                        if (mfc.type != "stbl") {
                            minf_payload.insert(minf_payload.end(),
                                                &moov_payload[mfc.offset],
                                                &moov_payload[mfc.offset + mfc.size]);
                            continue;
                        }
                        std::vector<uint8_t> stbl_payload;
                        auto stbl_ch = parse_children(moov_payload.data(), mfc.payload_offset, mfc.offset + mfc.size);
                        for (const auto& sc : stbl_ch) {
                            if (sc.type == "stsd") {
                                stbl_payload.insert(stbl_payload.end(),
                                                    &moov_payload[sc.offset],
                                                    &moov_payload[sc.offset + sc.size]);
                            }
                        }
                        append_box(stbl_payload, "stts", stts_payload);
                        if (!ctts_payload.empty()) {
                            append_box(stbl_payload, "ctts", ctts_payload);
                        }
                        if (!sync_samples.empty() && sync_samples.size() < samples.size()) {
                            std::vector<uint8_t> stss_payload(8 + sync_samples.size() * 4, 0);
                            write_be32(&stss_payload[4], static_cast<uint32_t>(sync_samples.size()));
                            for (size_t si = 0; si < sync_samples.size(); ++si) {
                                write_be32(&stss_payload[8 + si * 4], sync_samples[si]);
                            }
                            append_box(stbl_payload, "stss", stss_payload);
                        }
                        append_box(stbl_payload, "stsc", stsc_payload);
                        append_box(stbl_payload, "stsz", stsz_payload);
                        append_box(stbl_payload, "stco", stco_payload);
                        append_box(minf_payload, "stbl", stbl_payload);
                    }
                    append_box(mdia_payload, "minf", minf_payload);
                }
                append_box(trak_payload, "mdia", mdia_payload);
            }
            append_box(new_moov_payload, "trak", trak_payload);
        }
    }

    std::vector<uint8_t> new_moov;
    append_box(new_moov, "moov", new_moov_payload);

    bool ext_mdat = (total_mdat_payload + 8 > 0xFFFFFFFFULL);
    uint64_t mdat_hdr_sz = ext_mdat ? 16 : 8;
    uint64_t mdat_payload_offset = ftyp_data.size() + new_moov.size() + mdat_hdr_sz;

    // Update stco inside new_moov with the actual mdat_payload_offset
    auto top_ch = parse_children(new_moov.data(), 8, new_moov.size());
    for (const auto& tc : top_ch) {
        if (tc.type == "trak") {
            std::vector<uint8_t> trak_vec(&new_moov[tc.offset], &new_moov[tc.offset + tc.size]);
            rewrite_stco_co64(trak_vec, 0, mdat_payload_offset);
            std::memcpy(&new_moov[tc.offset], trak_vec.data(), trak_vec.size());
        }
    }

    std::ofstream out(temp_defrag_file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    out.write(reinterpret_cast<const char*>(ftyp_data.data()), ftyp_data.size());
    out.write(reinterpret_cast<const char*>(new_moov.data()), new_moov.size());

    if (ext_mdat) {
        uint8_t hdr[16];
        write_be32(&hdr[0], 1);
        std::memcpy(&hdr[4], "mdat", 4);
        write_be64(&hdr[8], total_mdat_payload + 16);
        out.write(reinterpret_cast<const char*>(hdr), 16);
    } else {
        uint8_t hdr[8];
        write_be32(&hdr[0], static_cast<uint32_t>(total_mdat_payload + 8));
        std::memcpy(&hdr[4], "mdat", 4);
        out.write(reinterpret_cast<const char*>(hdr), 8);
    }

    std::vector<char> copy_buf(2 * 1024 * 1024);
    for (const auto& [off, sz] : mdat_slices) {
        in.seekg(off);
        uint64_t rem = sz;
        while (rem > 0) {
            size_t chunk = static_cast<size_t>(std::min<uint64_t>(copy_buf.size(), rem));
            in.read(copy_buf.data(), chunk);
            std::streamsize got = in.gcount();
            if (got <= 0) return false;
            out.write(copy_buf.data(), got);
            rem -= static_cast<uint64_t>(got);
        }
    }

    out.flush();
    if (!out.good()) return false;
    out_usable_file = temp_defrag_file;
    return true;
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

    namespace fs = std::filesystem;
    std::error_code ec;
    uint64_t total_sz = (fs::exists(video_file, ec) ? fs::file_size(video_file, ec) : 0) +
                        (fs::exists(audio_file, ec) ? fs::file_size(audio_file, ec) : 0);

    std::string ext = fs::path(output_file).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Only apply +faststart second-pass rewrite on MP4/M4V files under 512 MB to avoid doubling disk I/O
    std::string extra_flags;
    if ((ext == ".mp4" || ext == ".m4v" || ext == ".mov" || ext.empty()) && total_sz < 512ULL * 1024 * 1024) {
        extra_flags = " -movflags +faststart";
    }
    if (ext != ".mp4" && ext != ".mkv" && ext != ".webm" && ext != ".mov") {
        extra_flags += " -f mp4";
    }

    // Attempt 1: Pure lossless stream copy of both video and audio
    std::string cmd = escape_shell_arg(ffmpeg_bin) + " -y -loglevel error -i " +
                      escape_shell_arg(video_file) + " -i " + escape_shell_arg(audio_file) +
                      " -map 0:v:0 -map 1:a:0 -c copy" + extra_flags + " " +
                      escape_shell_arg(output_file) + " >/dev/null 2>&1";

    if (system(cmd.c_str()) == 0 && fs::exists(output_file, ec) && fs::file_size(output_file, ec) > 0) {
        if (progress_cb) progress_cb(100.0);
        return true;
    }

    // Attempt 2: If container rejected audio codec (e.g. AAC audio into .webm or Opus into strict MP4),
    // copy video losslessly and transcode only audio or force MP4/Matroska muxer
    std::string fallback_flags = (ext == ".webm") ? "-c:v copy -c:a libopus" : "-c:v copy -c:a aac -f mp4";
    std::string cmd2 = escape_shell_arg(ffmpeg_bin) + " -y -loglevel error -i " +
                       escape_shell_arg(video_file) + " -i " + escape_shell_arg(audio_file) +
                       " -map 0:v:0 -map 1:a:0 " + fallback_flags + " " +
                       escape_shell_arg(output_file) + " >/dev/null 2>&1";

    if (system(cmd2.c_str()) == 0 && fs::exists(output_file, ec) && fs::file_size(output_file, ec) > 0) {
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

    if (try_external_ffmpeg(video_file, audio_file, output_file, progress_cb)) {
        return true;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    std::string v_usable, a_usable;
    std::string v_defrag_tmp = output_file + ".v_defrag.tmp";
    std::string a_defrag_tmp = output_file + ".a_defrag.tmp";

    auto cleanup_defrag = [&]() {
        fs::remove(v_defrag_tmp, ec);
        fs::remove(a_defrag_tmp, ec);
    };

    if (!defragment_mp4_if_needed(video_file, v_defrag_tmp, v_usable) ||
        !defragment_mp4_if_needed(audio_file, a_defrag_tmp, a_usable)) {
        cleanup_defrag();
        if (out_error) *out_error = "Failed to defragment DASH/MP4 media streams";
        return false;
    }

    auto v_boxes = scan_mp4_boxes(v_usable);
    auto a_boxes = scan_mp4_boxes(a_usable);

    const BoxInfo* v_ftyp = nullptr;
    const BoxInfo* v_moov = nullptr;
    const BoxInfo* v_mdat = nullptr;
    for (const auto& b : v_boxes) {
        if (b.type == "ftyp" && !v_ftyp) v_ftyp = &b;
        else if (b.type == "moov" && !v_moov) v_moov = &b;
        else if (b.type == "mdat" && (!v_mdat || b.payload_size > v_mdat->payload_size)) v_mdat = &b;
    }

    const BoxInfo* a_moov = nullptr;
    const BoxInfo* a_mdat = nullptr;
    for (const auto& b : a_boxes) {
        if (b.type == "moov" && !a_moov) a_moov = &b;
        else if (b.type == "mdat" && (!a_mdat || b.payload_size > a_mdat->payload_size)) a_mdat = &b;
    }

    if (!v_ftyp || !v_moov || !v_mdat || !a_moov || !a_mdat) {
        cleanup_defrag();
        if (out_error) *out_error = "Missing essential MP4 atoms (ftyp/moov/mdat)";
        return false;
    }

    std::ifstream v_in(v_usable, std::ios::binary);
    std::ifstream a_in(a_usable, std::ios::binary);
    if (!v_in.is_open() || !a_in.is_open()) {
        cleanup_defrag();
        if (out_error) *out_error = "Could not open source media files";
        return false;
    }

    std::vector<uint8_t> ftyp_data(v_ftyp->size);
    v_in.seekg(v_ftyp->offset);
    v_in.read(reinterpret_cast<char*>(ftyp_data.data()), v_ftyp->size);

    std::vector<uint8_t> v_moov_data(v_moov->payload_size);
    v_in.seekg(v_moov->payload_offset);
    v_in.read(reinterpret_cast<char*>(v_moov_data.data()), v_moov->payload_size);

    std::vector<uint8_t> a_moov_data(a_moov->payload_size);
    a_in.seekg(a_moov->payload_offset);
    a_in.read(reinterpret_cast<char*>(a_moov_data.data()), a_moov->payload_size);

    std::vector<uint8_t> v_trak, v_mvhd, a_trak;
    for (const auto& c : parse_children(v_moov_data.data(), 0, v_moov_data.size())) {
        if (c.type == "mvhd" && v_mvhd.empty()) {
            v_mvhd.assign(&v_moov_data[c.offset], &v_moov_data[c.offset + c.size]);
        } else if (c.type == "trak" && v_trak.empty()) {
            v_trak.assign(&v_moov_data[c.offset], &v_moov_data[c.offset + c.size]);
        }
    }
    for (const auto& c : parse_children(a_moov_data.data(), 0, a_moov_data.size())) {
        if (c.type == "trak" && a_trak.empty()) {
            a_trak.assign(&a_moov_data[c.offset], &a_moov_data[c.offset + c.size]);
        }
    }

    if (v_trak.empty() || a_trak.empty() || v_mvhd.empty()) {
        cleanup_defrag();
        if (out_error) *out_error = "Could not locate tracks or movie header";
        return false;
    }

    // Explicitly assign non-colliding Track IDs 1 (video) and 2 (audio)
    set_tkhd_track_id(v_trak, 1);
    set_tkhd_track_id(a_trak, 2);

    // Ensure mvhd has full playback volume (0x0100 = 1.0) and next_track_id = 3
    if (v_mvhd.size() >= 108) {
        uint8_t version = v_mvhd[8];
        size_t vol_offset = (version == 0) ? 32 : 44;
        size_t next_id_offset = (version == 0) ? 104 : 116;
        if (vol_offset + 2 <= v_mvhd.size()) {
            write_be16(&v_mvhd[vol_offset], 0x0100);
        }
        if (next_id_offset + 4 <= v_mvhd.size()) {
            write_be32(&v_mvhd[next_id_offset], 3);
        }
    }

    uint32_t moov_payload_size = static_cast<uint32_t>(v_mvhd.size() + v_trak.size() + a_trak.size());
    uint32_t moov_total_size = moov_payload_size + 8;

    uint64_t total_media_payload = v_mdat->payload_size + a_mdat->payload_size;
    bool extended_mdat = (total_media_payload + 8 > 0xFFFFFFFFULL);
    uint64_t mdat_header_size = extended_mdat ? 16 : 8;

    // Fast-start layout: [ftyp] [moov] [mdat]
    uint64_t new_mdat_box_offset = ftyp_data.size() + moov_total_size;
    uint64_t new_v_mdat_payload_offset = new_mdat_box_offset + mdat_header_size;
    uint64_t new_a_mdat_payload_offset = new_v_mdat_payload_offset + v_mdat->payload_size;

    if (!rewrite_stco_co64(v_trak, v_mdat->payload_offset, new_v_mdat_payload_offset) ||
        !rewrite_stco_co64(a_trak, a_mdat->payload_offset, new_a_mdat_payload_offset)) {
        cleanup_defrag();
        if (out_error) *out_error = "Failed to rewrite track chunk offsets";
        return false;
    }

    std::vector<uint8_t> new_moov(moov_total_size);
    write_be32(&new_moov[0], moov_total_size);
    std::memcpy(&new_moov[4], "moov", 4);
    size_t write_pos = 8;
    std::memcpy(&new_moov[write_pos], v_mvhd.data(), v_mvhd.size());
    write_pos += v_mvhd.size();
    std::memcpy(&new_moov[write_pos], v_trak.data(), v_trak.size());
    write_pos += v_trak.size();
    std::memcpy(&new_moov[write_pos], a_trak.data(), a_trak.size());

    std::ofstream out(output_file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        cleanup_defrag();
        if (out_error) *out_error = "Could not open output destination file";
        return false;
    }

    // 1. Write ftyp & fast-start moov at front of file
    out.write(reinterpret_cast<const char*>(ftyp_data.data()), ftyp_data.size());
    out.write(reinterpret_cast<const char*>(new_moov.data()), new_moov.size());

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

    // 3. Stream copy video and audio mdat payloads with 4 MB buffer
    constexpr size_t BUFFER_SIZE = 4 * 1024 * 1024;
    std::vector<char> buffer(BUFFER_SIZE);

    v_in.seekg(v_mdat->payload_offset);
    uint64_t v_copied = 0;
    while (v_copied < v_mdat->payload_size) {
        size_t to_read = static_cast<size_t>(std::min<uint64_t>(BUFFER_SIZE, v_mdat->payload_size - v_copied));
        v_in.read(buffer.data(), to_read);
        std::streamsize got = v_in.gcount();
        if (got <= 0) break;
        out.write(buffer.data(), got);
        v_copied += static_cast<uint64_t>(got);
        if (progress_cb && total_media_payload > 0) {
            progress_cb((static_cast<double>(v_copied) / total_media_payload) * 95.0);
        }
    }

    a_in.seekg(a_mdat->payload_offset);
    uint64_t a_copied = 0;
    while (a_copied < a_mdat->payload_size) {
        size_t to_read = static_cast<size_t>(std::min<uint64_t>(BUFFER_SIZE, a_mdat->payload_size - a_copied));
        a_in.read(buffer.data(), to_read);
        std::streamsize got = a_in.gcount();
        if (got <= 0) break;
        out.write(buffer.data(), got);
        a_copied += static_cast<uint64_t>(got);
        if (progress_cb && total_media_payload > 0) {
            progress_cb((static_cast<double>(v_copied + a_copied) / total_media_payload) * 95.0);
        }
    }

    out.flush();
    bool ok = out.good() && (v_copied == v_mdat->payload_size) && (a_copied == a_mdat->payload_size);
    out.close();
    v_in.close();
    a_in.close();
    cleanup_defrag();

    if (ok && progress_cb) progress_cb(100.0);
    return ok;
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

    std::string ts_target = output_file;
    bool remux_to_mp4 = (output_file.size() >= 4 &&
                         output_file.substr(output_file.size() - 4) == ".mp4");
    if (remux_to_mp4) {
        ts_target = output_file + ".concat.ts";
    }

    std::ofstream out(ts_target, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (out_error) *out_error = "Could not open output TS file";
        return false;
    }

    constexpr size_t BUFFER_SIZE = 2 * 1024 * 1024;
    std::vector<char> buffer(BUFFER_SIZE);
    size_t total_files = segment_files.size();

    for (size_t idx = 0; idx < total_files; ++idx) {
        std::ifstream in(segment_files[idx], std::ios::binary);
        if (!in.is_open()) {
            if (out_error) *out_error = "Missing TS segment: " + segment_files[idx];
            return false;
        }
        while (in.read(buffer.data(), BUFFER_SIZE) || in.gcount() > 0) {
            out.write(buffer.data(), in.gcount());
            if (out.fail()) {
                if (out_error) *out_error = "Disk write error while concatenating TS segments";
                return false;
            }
        }
        if (progress_cb) {
            progress_cb((static_cast<double>(idx + 1) / total_files) * (remux_to_mp4 ? 85.0 : 100.0));
        }
    }

    out.flush();
    out.close();

    if (remux_to_mp4) {
        bool conv_ok = convert_ts_to_mp4(ts_target, output_file, progress_cb, out_error);
        std::error_code ec;
        std::filesystem::remove(ts_target, ec);
        return conv_ok;
    }
    return true;
}

bool MediaMuxer::convert_ts_to_mp4(
    const std::string& ts_file,
    const std::string& output_file,
    MuxProgressCallback progress_cb,
    std::string* out_error) {

    std::string ffmpeg_bin = find_ffmpeg();
    if (!ffmpeg_bin.empty()) {
        std::string cmd = escape_shell_arg(ffmpeg_bin) + " -y -loglevel error -i " +
                          escape_shell_arg(ts_file) +
                          " -c copy -bsf:a aac_adtstoasc -movflags +faststart " +
                          escape_shell_arg(output_file) + " >/dev/null 2>&1";
        if (system(cmd.c_str()) == 0) {
            if (progress_cb) progress_cb(100.0);
            return true;
        }
        // Fallback without aac_adtstoasc for non-AAC audio tracks (e.g. MP3/AC3/Opus in TS)
        std::string cmd2 = escape_shell_arg(ffmpeg_bin) + " -y -loglevel error -i " +
                           escape_shell_arg(ts_file) + " -c copy -movflags +faststart " +
                           escape_shell_arg(output_file) + " >/dev/null 2>&1";
        if (system(cmd2.c_str()) == 0) {
            if (progress_cb) progress_cb(100.0);
            return true;
        }
    }

    if (out_error) *out_error = "TS to MP4 conversion requires ffmpeg";
    return false;
}

bool MediaMuxer::mux_streams(
    const std::string& video_file,
    const std::string& audio_file,
    const std::string& output_file,
    MuxProgressCallback progress_cb,
    std::string* out_error) {
    return mux_mp4_and_mp4(video_file, audio_file, output_file, progress_cb, out_error);
}

} // namespace opendm

