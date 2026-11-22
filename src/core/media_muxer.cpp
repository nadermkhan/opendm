#include "media_muxer.hpp"
#include "win32_unicode.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#ifdef ERROR
#undef ERROR
#endif
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace opendm {

namespace {

inline uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
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

struct ChildBox {
    std::string type;
    size_t offset;
    size_t size;
    size_t payload_offset;
    size_t payload_size;
};

std::vector<ChildBox> parse_children(const uint8_t* data, size_t start, size_t end) {
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
        if (sz < hdr || sz > (end - cur)) break;
        res.push_back({type, cur, static_cast<size_t>(sz), cur + hdr, static_cast<size_t>(sz - hdr)});
        cur += static_cast<size_t>(sz);
    }
    return res;
}

void append_box(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& payload) {
    size_t start = out.size();
    out.resize(start + 8 + payload.size());
    write_be32(&out[start], static_cast<uint32_t>(8 + payload.size()));
    std::memcpy(&out[start + 4], type, 4);
    if (!payload.empty()) {
        std::memcpy(&out[start + 8], payload.data(), payload.size());
    }
}

uint32_t get_tkhd_track_id(const uint8_t* data, size_t start, size_t end) {
    for (const auto& c : parse_children(data, start, end)) {
        if (c.type == "tkhd" && c.payload_size >= 16) {
            uint8_t ver = data[c.payload_offset];
            size_t id_off = (ver == 0) ? (c.payload_offset + 12) : (c.payload_offset + 20);
            if (id_off + 4 <= c.offset + c.size) {
                return read_be32(data + id_off);
            }
        }
    }
    return 0;
}

uint32_t get_mdhd_timescale(const uint8_t* data, size_t start, size_t end) {
    for (const auto& c : parse_children(data, start, end)) {
        if (c.type == "mdhd" && c.payload_size >= 16) {
            uint8_t ver = data[c.payload_offset];
            size_t ts_off = (ver == 0) ? (c.payload_offset + 12) : (c.payload_offset + 20);
            if (ts_off + 4 <= c.offset + c.size) {
                return read_be32(data + ts_off);
            }
        }
    }
    return 1;
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

#ifdef _WIN32
    std::string exe_dir = sys::get_exe_dir_utf8();
    if (!exe_dir.empty()) {
        fs::path local_ff = sys::path_from_utf8(exe_dir) / L"ffmpeg.exe";
        std::error_code ec;
        if (fs::exists(local_ff, ec)) {
            s_cached_path = sys::path_to_utf8(local_ff);
            return s_cached_path;
        }
    }
    std::wstring sp_buf(32768, L'\0');
    DWORD found = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(sp_buf.size()), sp_buf.data(), nullptr);
    if (found > 0 && found < sp_buf.size()) {
        sp_buf.resize(found);
        s_cached_path = sys::wide_to_utf8(sp_buf);
        return s_cached_path;
    }
    return "";
#else
    std::string home = sys::get_env_utf8("HOME");
    std::vector<std::string> search_paths = {
        home.empty() ? "" : home + "/.local/bin/ffmpeg",
        "/usr/local/bin/ffmpeg",
        "/usr/bin/ffmpeg",
        "/bin/ffmpeg",
        "/snap/bin/ffmpeg"
    };

    std::string path_env = sys::get_env_utf8("PATH");
    if (!path_env.empty()) {
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
#endif
}

std::vector<MediaMuxer::BoxInfo> MediaMuxer::scan_mp4_boxes(const std::string& file_path) {
    std::vector<BoxInfo> boxes;
    std::ifstream file(sys::path_from_utf8(file_path), std::ios::binary | std::ios::ate);
    if (!file.is_open()) return boxes;

    std::streamoff end_pos = file.tellg();
    if (end_pos <= 0) return boxes;
    uint64_t file_size = static_cast<uint64_t>(end_pos);
    file.seekg(0, std::ios::beg);

    uint64_t current_offset = 0;
    while (current_offset + 8 <= file_size) {
        file.seekg(static_cast<std::streamoff>(current_offset));
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

        if (box_size < header_size || box_size > (file_size - current_offset)) {
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
                        if (entry_count > (box.offset + box.size - table_start) / 4) return false;

                        std::vector<uint64_t> new_offsets(entry_count);
                        bool need_co64 = false;
                        for (uint32_t e = 0; e < entry_count; ++e) {
                            size_t pos = table_start + e * 4;
                            uint32_t old_off = read_be32(&trak_data[pos]);
                            int64_t delta = static_cast<int64_t>(old_off) - static_cast<int64_t>(old_mdat_payload_offset);
                            uint64_t new_off = static_cast<uint64_t>(static_cast<int64_t>(new_mdat_payload_offset) + delta);
                            new_offsets[e] = new_off;
                            if (new_off > 0xFFFFFFFFULL) {
                                need_co64 = true;
                            }
                        }

                        if (!need_co64) {
                            for (uint32_t e = 0; e < entry_count; ++e) {
                                write_be32(&trak_data[table_start + e * 4], static_cast<uint32_t>(new_offsets[e]));
                            }
                            return true;
                        }

                        // Upgrade stco -> co64: each entry grows by +4 bytes (4 -> 8 bytes).
                        // Propagate exact byte growth to co64, stbl, minf, mdia, and trak headers.
                        size_t growth = static_cast<size_t>(entry_count) * 4;
                        if (trak_data.size() + growth > 0xFFFFFFFFULL) return false;

                        size_t insert_at = box.offset + box.size;
                        trak_data.insert(trak_data.begin() + static_cast<std::ptrdiff_t>(insert_at), growth, 0);

                        std::memcpy(&trak_data[box.offset + 4], "co64", 4);
                        for (uint32_t e = 0; e < entry_count; ++e) {
                            write_be64(&trak_data[table_start + e * 8], new_offsets[e]);
                        }

                        write_be32(&trak_data[box.offset], static_cast<uint32_t>(box.size + growth));
                        write_be32(&trak_data[stbl.offset], static_cast<uint32_t>(stbl.size + growth));
                        write_be32(&trak_data[minf.offset], static_cast<uint32_t>(minf.size + growth));
                        write_be32(&trak_data[mdia.offset], static_cast<uint32_t>(mdia.size + growth));
                        write_be32(&trak_data[0], static_cast<uint32_t>(trak_data.size()));
                        return true;
                    } else if (box.type == "co64" && box.payload_size >= 8) {
                        uint32_t entry_count = read_be32(&trak_data[box.payload_offset + 4]);
                        size_t table_start = box.payload_offset + 8;
                        if (entry_count > (box.offset + box.size - table_start) / 8) return false;

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

    std::ifstream in(sys::path_from_utf8(input_file), std::ios::binary);
    if (!in.is_open()) return false;

    std::vector<uint8_t> ftyp_data(ftyp_box->size);
    in.seekg(static_cast<std::streamoff>(ftyp_box->offset));
    in.read(reinterpret_cast<char*>(ftyp_data.data()), static_cast<std::streamsize>(ftyp_box->size));

    std::vector<uint8_t> moov_payload(moov_box->payload_size);
    in.seekg(static_cast<std::streamoff>(moov_box->payload_offset));
    in.read(reinterpret_cast<char*>(moov_payload.data()), static_cast<std::streamsize>(moov_box->payload_size));

    struct TrexDefaults {
        uint32_t dur{0};
        uint32_t size{0};
        uint32_t flags{0};
    };
    std::map<uint32_t, TrexDefaults> trex_map;
    uint32_t mvhd_timescale = 1000;

    auto moov_children = parse_children(moov_payload.data(), 0, moov_payload.size());
    for (const auto& c : moov_children) {
        if (c.type == "mvhd" && c.payload_size >= 20) {
            uint8_t ver = moov_payload[c.payload_offset];
            size_t ts_off = (ver == 0) ? (c.payload_offset + 12) : (c.payload_offset + 20);
            if (ts_off + 4 <= c.offset + c.size) {
                uint32_t ts = read_be32(&moov_payload[ts_off]);
                if (ts > 0) mvhd_timescale = ts;
            }
        } else if (c.type == "mvex") {
            auto mvex_children = parse_children(moov_payload.data(), c.payload_offset, c.offset + c.size);
            for (const auto& mc : mvex_children) {
                if (mc.type == "trex" && mc.payload_size >= 24) {
                    uint32_t tid = read_be32(&moov_payload[mc.payload_offset + 4]);
                    trex_map[tid] = {
                        read_be32(&moov_payload[mc.payload_offset + 12]),
                        read_be32(&moov_payload[mc.payload_offset + 16]),
                        read_be32(&moov_payload[mc.payload_offset + 20])
                    };
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
    struct TrackRunState {
        std::vector<SampleRec> samples;
        std::vector<std::pair<uint64_t, uint64_t>> mdat_chunks; // (chunk_file_offset, chunk_bytes)
        bool has_cts{false};
    };
    std::map<uint32_t, TrackRunState> track_states;
    std::vector<std::pair<uint64_t, uint64_t>> all_mdat_slices;
    uint64_t total_mdat_payload = 0;

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
        in.seekg(static_cast<std::streamoff>(moof.payload_offset));
        in.read(reinterpret_cast<char*>(moof_buf.data()), static_cast<std::streamsize>(moof.payload_size));

        auto moof_ch = parse_children(moof_buf.data(), 0, moof_buf.size());
        uint64_t mdat_cursor = 0;

        for (const auto& traf : moof_ch) {
            if (traf.type != "traf") continue;
            uint32_t track_id = 1;
            TrexDefaults def{};
            if (!trex_map.empty()) def = trex_map.begin()->second;

            auto traf_ch = parse_children(moof_buf.data(), traf.payload_offset, traf.offset + traf.size);
            for (const auto& tc : traf_ch) {
                if (tc.type == "tfhd" && tc.payload_size >= 8) {
                    uint32_t tf_flags = read_be32(&moof_buf[tc.payload_offset]) & 0x00FFFFFF;
                    track_id = read_be32(&moof_buf[tc.payload_offset + 4]);
                    if (trex_map.count(track_id)) def = trex_map[track_id];

                    size_t pos = tc.payload_offset + 8;
                    if (tf_flags & 0x000001) pos += 8;
                    if (tf_flags & 0x000002) pos += 4;
                    if ((tf_flags & 0x000008) && pos + 4 <= tc.offset + tc.size) {
                        def.dur = read_be32(&moof_buf[pos]);
                        pos += 4;
                    }
                    if ((tf_flags & 0x000010) && pos + 4 <= tc.offset + tc.size) {
                        def.size = read_be32(&moof_buf[pos]);
                        pos += 4;
                    }
                    if ((tf_flags & 0x000020) && pos + 4 <= tc.offset + tc.size) {
                        def.flags = read_be32(&moof_buf[pos]);
                        pos += 4;
                    }
                } else if (tc.type == "trun" && tc.payload_size >= 8) {
                    auto& tr = track_states[track_id];
                    uint8_t tr_ver = moof_buf[tc.payload_offset];
                    (void)tr_ver;
                    uint32_t tr_flags = read_be32(&moof_buf[tc.payload_offset]) & 0x00FFFFFF;
                    uint32_t sample_count = read_be32(&moof_buf[tc.payload_offset + 4]);
                    size_t pos = tc.payload_offset + 8;
                    if (tr_flags & 0x000001) pos += 4;
                    uint32_t first_sample_flags = def.flags;
                    bool has_first_flags = (tr_flags & 0x000004) != 0;
                    if (has_first_flags && pos + 4 <= tc.offset + tc.size) {
                        first_sample_flags = read_be32(&moof_buf[pos]);
                        pos += 4;
                    }

                    uint64_t trun_bytes = 0;
                    for (uint32_t s = 0; s < sample_count; ++s) {
                        SampleRec rec{def.dur, def.size, (s == 0 && has_first_flags) ? first_sample_flags : def.flags, 0};
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
                            rec.cts_offset = static_cast<int32_t>(read_be32(&moof_buf[pos]));
                            if (rec.cts_offset != 0) tr.has_cts = true;
                            pos += 4;
                        }
                        trun_bytes += rec.size;
                        tr.samples.push_back(rec);
                    }
                    if (trun_bytes > 0 && mdat_cursor + trun_bytes <= mdat->payload_size) {
                        tr.mdat_chunks.push_back({mdat->payload_offset + mdat_cursor, trun_bytes});
                        mdat_cursor += trun_bytes;
                    }
                }
            }
        }

        all_mdat_slices.push_back({mdat->payload_offset, mdat->payload_size});
        total_mdat_payload += mdat->payload_size;
    }

    if (track_states.empty() || all_mdat_slices.empty()) {
        return false;
    }

    // Reorder mdat sequentially only for tracks > 1 when multiple tracks exist; single-track preserves slice sequence.
    bool multi_track = (track_states.size() > 1);
    std::vector<std::pair<uint64_t, uint64_t>> write_slices;
    std::map<uint32_t, std::vector<uint64_t>> track_rel_chunk_offsets;
    if (!multi_track) {
        write_slices = all_mdat_slices;
        uint32_t tid = track_states.begin()->first;
        track_rel_chunk_offsets[tid].push_back(0);
    } else {
        uint64_t rel_cursor = 0;
        for (const auto& [tid, tr] : track_states) {
            if (tr.mdat_chunks.empty()) continue;
            track_rel_chunk_offsets[tid].push_back(rel_cursor);
            for (const auto& mc : tr.mdat_chunks) {
                write_slices.push_back(mc);
                rel_cursor += mc.second;
            }
        }
        total_mdat_payload = rel_cursor;
    }

    // Build per-track updated traks (initially with stco 1-entry 32-bit placeholder)
    std::vector<uint8_t> mvhd_box_bytes;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> rebuilt_traks;
    uint64_t max_mvhd_ticks = 0;

    for (const auto& c : moov_children) {
        if (c.type == "mvhd") {
            mvhd_box_bytes.assign(moov_payload.data() + c.offset, moov_payload.data() + c.offset + c.size);
        } else if (c.type == "trak") {
            uint32_t tid = get_tkhd_track_id(moov_payload.data(), c.payload_offset, c.offset + c.size);
            auto tr_it = track_states.find(tid);
            if (tr_it == track_states.end() && track_states.size() == 1) {
                tr_it = track_states.begin();
            }
            if (tr_it == track_states.end() || tr_it->second.samples.empty()) {
                continue;
            }
            const auto& tr = tr_it->second;
            uint32_t md_ts = get_mdhd_timescale(moov_payload.data(), c.payload_offset, c.offset + c.size);

            std::vector<std::pair<uint32_t, uint32_t>> stts_runs;
            uint64_t total_media_ticks = 0;
            for (const auto& s : tr.samples) {
                total_media_ticks += s.duration;
                if (!stts_runs.empty() && stts_runs.back().second == s.duration) {
                    stts_runs.back().first++;
                } else {
                    stts_runs.push_back({1, s.duration});
                }
            }
            uint64_t scaled_mvhd = (md_ts > 0) ? ((total_media_ticks * mvhd_timescale) / md_ts) : total_media_ticks;
            if (scaled_mvhd > max_mvhd_ticks) max_mvhd_ticks = scaled_mvhd;

            std::vector<uint8_t> stts_payload(8 + stts_runs.size() * 8, 0);
            write_be32(&stts_payload[4], static_cast<uint32_t>(stts_runs.size()));
            for (size_t i = 0; i < stts_runs.size(); ++i) {
                write_be32(&stts_payload[8 + i * 8], stts_runs[i].first);
                write_be32(&stts_payload[12 + i * 8], stts_runs[i].second);
            }

            std::vector<uint8_t> stsz_payload(12 + tr.samples.size() * 4, 0);
            write_be32(&stsz_payload[8], static_cast<uint32_t>(tr.samples.size()));
            for (size_t i = 0; i < tr.samples.size(); ++i) {
                write_be32(&stsz_payload[12 + i * 4], tr.samples[i].size);
            }

            std::vector<uint8_t> stsc_payload(20, 0);
            write_be32(&stsc_payload[4], 1);
            write_be32(&stsc_payload[8], 1);
            write_be32(&stsc_payload[12], static_cast<uint32_t>(tr.samples.size()));
            write_be32(&stsc_payload[16], 1);

            std::vector<uint32_t> sync_samples;
            for (size_t i = 0; i < tr.samples.size(); ++i) {
                bool is_non_sync = ((tr.samples[i].flags & 0x00010000) != 0) ||
                                   (((tr.samples[i].flags >> 24) & 0x03) == 1);
                if (!is_non_sync) {
                    sync_samples.push_back(static_cast<uint32_t>(i + 1));
                }
            }

            std::vector<uint8_t> ctts_payload;
            if (tr.has_cts) {
                std::vector<std::pair<uint32_t, int32_t>> ctts_runs;
                for (const auto& s : tr.samples) {
                    if (!ctts_runs.empty() && ctts_runs.back().second == s.cts_offset) {
                        ctts_runs.back().first++;
                    } else {
                        ctts_runs.push_back({1, s.cts_offset});
                    }
                }
                ctts_payload.assign(8 + ctts_runs.size() * 8, 0);
                ctts_payload[0] = 1;
                write_be32(&ctts_payload[4], static_cast<uint32_t>(ctts_runs.size()));
                for (size_t i = 0; i < ctts_runs.size(); ++i) {
                    write_be32(&ctts_payload[8 + i * 8], ctts_runs[i].first);
                    write_be32(&ctts_payload[12 + i * 8], static_cast<uint32_t>(ctts_runs[i].second));
                }
            }

            std::vector<uint8_t> stco_payload(12, 0);
            write_be32(&stco_payload[4], 1);

            std::vector<uint8_t> trak_payload;
            auto trak_ch = parse_children(moov_payload.data(), c.payload_offset, c.offset + c.size);
            for (const auto& tc : trak_ch) {
                if (tc.type != "mdia") {
                    trak_payload.insert(trak_payload.end(),
                                        moov_payload.data() + tc.offset,
                                        moov_payload.data() + tc.offset + tc.size);
                    continue;
                }
                std::vector<uint8_t> mdia_payload;
                auto mdia_ch = parse_children(moov_payload.data(), tc.payload_offset, tc.offset + tc.size);
                for (const auto& mc : mdia_ch) {
                    if (mc.type == "mdhd" && mc.payload_size >= 20) {
                        std::vector<uint8_t> mdhd_data(moov_payload.data() + mc.offset, moov_payload.data() + mc.offset + mc.size);
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
                                            moov_payload.data() + mc.offset,
                                            moov_payload.data() + mc.offset + mc.size);
                        continue;
                    }
                    std::vector<uint8_t> minf_payload;
                    auto minf_ch = parse_children(moov_payload.data(), mc.payload_offset, mc.offset + mc.size);
                    for (const auto& mfc : minf_ch) {
                        if (mfc.type != "stbl") {
                            minf_payload.insert(minf_payload.end(),
                                                moov_payload.data() + mfc.offset,
                                                moov_payload.data() + mfc.offset + mfc.size);
                            continue;
                        }
                        std::vector<uint8_t> stbl_payload;
                        auto stbl_ch = parse_children(moov_payload.data(), mfc.payload_offset, mfc.offset + mfc.size);
                        for (const auto& sc : stbl_ch) {
                            if (sc.type == "stsd") {
                                stbl_payload.insert(stbl_payload.end(),
                                                    moov_payload.data() + sc.offset,
                                                    moov_payload.data() + sc.offset + sc.size);
                            }
                        }
                        append_box(stbl_payload, "stts", stts_payload);
                        if (!ctts_payload.empty()) append_box(stbl_payload, "ctts", ctts_payload);
                        if (!sync_samples.empty() && sync_samples.size() < tr.samples.size()) {
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

            std::vector<uint8_t> trak_box;
            append_box(trak_box, "trak", trak_payload);
            rebuilt_traks.emplace_back(tr_it->first, std::move(trak_box));
        }
    }

    if (rebuilt_traks.empty() || mvhd_box_bytes.empty()) {
        return false;
    }

    // Patch mvhd total movie duration
    if (mvhd_box_bytes.size() >= 28 && max_mvhd_ticks > 0) {
        uint8_t ver = mvhd_box_bytes[8];
        if (ver == 0 && mvhd_box_bytes.size() >= 28) {
            write_be32(&mvhd_box_bytes[24], static_cast<uint32_t>(max_mvhd_ticks));
        } else if (ver == 1 && mvhd_box_bytes.size() >= 36) {
            write_be64(&mvhd_box_bytes[28], max_mvhd_ticks);
        }
    }

    bool ext_mdat = (total_mdat_payload + 8 > 0xFFFFFFFFULL);
    uint64_t mdat_hdr_sz = ext_mdat ? 16 : 8;

    // Two-pass fixed point so any stco -> co64 size expansion happens BEFORE final offset & moov layout
    uint64_t prev_offsets_base = 0;
    uint64_t final_mdat_payload_offset = 0;
    for (int pass = 0; pass < 2; ++pass) {
        size_t moov_total = 8 + mvhd_box_bytes.size();
        for (const auto& [tid, tv] : rebuilt_traks) {
            moov_total += tv.size();
        }
        final_mdat_payload_offset = ftyp_data.size() + moov_total + mdat_hdr_sz;

        for (auto& [tid, tv] : rebuilt_traks) {
            uint64_t rel = (!track_rel_chunk_offsets[tid].empty()) ? track_rel_chunk_offsets[tid][0] : 0;
            uint64_t old_base = (pass == 0) ? 0 : (prev_offsets_base + rel);
            uint64_t new_base = final_mdat_payload_offset + rel;
            if (!rewrite_stco_co64(tv, old_base, new_base)) {
                return false;
            }
        }
        prev_offsets_base = final_mdat_payload_offset;
    }

    std::vector<uint8_t> new_moov_payload = mvhd_box_bytes;
    for (const auto& [tid, tv] : rebuilt_traks) {
        new_moov_payload.insert(new_moov_payload.end(), tv.begin(), tv.end());
    }
    std::vector<uint8_t> new_moov;
    append_box(new_moov, "moov", new_moov_payload);

    std::ofstream out(sys::path_from_utf8(temp_defrag_file), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    out.write(reinterpret_cast<const char*>(ftyp_data.data()), static_cast<std::streamsize>(ftyp_data.size()));
    out.write(reinterpret_cast<const char*>(new_moov.data()), static_cast<std::streamsize>(new_moov.size()));

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
    for (const auto& [off, sz] : write_slices) {
        in.seekg(static_cast<std::streamoff>(off));
        uint64_t rem = sz;
        while (rem > 0) {
            size_t chunk = static_cast<size_t>(std::min<uint64_t>(copy_buf.size(), rem));
            in.read(copy_buf.data(), static_cast<std::streamsize>(chunk));
            std::streamsize got = in.gcount();
            if (got <= 0) {
                out.close();
                std::error_code ec;
                fs::remove(sys::path_from_utf8(temp_defrag_file), ec);
                return false;
            }
            out.write(copy_buf.data(), got);
            if (!out.good()) {
                out.close();
                std::error_code ec;
                fs::remove(sys::path_from_utf8(temp_defrag_file), ec);
                return false;
            }
            rem -= static_cast<uint64_t>(got);
        }
    }

    out.flush();
    bool ok = out.good();
    out.close();
    if (!ok) {
        std::error_code ec;
        fs::remove(sys::path_from_utf8(temp_defrag_file), ec);
        return false;
    }
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

    std::error_code ec;
    fs::path v_p = sys::path_from_utf8(video_file);
    fs::path a_p = sys::path_from_utf8(audio_file);
    fs::path o_p = sys::path_from_utf8(output_file);

    uint64_t total_sz = (fs::exists(v_p, ec) ? fs::file_size(v_p, ec) : 0) +
                        (fs::exists(a_p, ec) ? fs::file_size(a_p, ec) : 0);

    std::string ext = sys::ascii_tolower(sys::path_to_utf8(o_p.extension()));
    bool small_mp4 = (ext == ".mp4" || ext == ".m4v" || ext == ".mov" || ext.empty()) &&
                     (total_sz < 512ULL * 1024 * 1024);

    // Attempt 1: lossless stream copy
    std::vector<std::string> args1 = {
        "-y", "-loglevel", "error",
        "-i", video_file,
        "-i", audio_file,
        "-map", "0:v:0", "-map", "1:a:0",
        "-c", "copy"
    };
    if (small_mp4) {
        args1.push_back("-movflags");
        args1.push_back("+faststart");
    }
    if (ext != ".mp4" && ext != ".mkv" && ext != ".webm" && ext != ".mov") {
        args1.push_back("-f");
        args1.push_back("mp4");
    }
    args1.push_back(output_file);

    if (sys::run_hidden_process(ffmpeg_bin, args1) && fs::exists(o_p, ec) && fs::file_size(o_p, ec) > 0) {
        if (progress_cb) progress_cb(100.0);
        return true;
    }

    // Attempt 2: transcode only incompatible audio stream while never adding +faststart on >=512 MB files
    std::vector<std::string> args2 = {
        "-y", "-loglevel", "error",
        "-i", video_file,
        "-i", audio_file,
        "-map", "0:v:0", "-map", "1:a:0",
        "-c:v", "copy",
        "-c:a", (ext == ".webm") ? "libopus" : "aac"
    };
    if (ext != ".webm" && ext != ".mkv") {
        if (small_mp4) {
            args2.push_back("-movflags");
            args2.push_back("+faststart");
        }
        args2.push_back("-f");
        args2.push_back("mp4");
    }
    args2.push_back(output_file);

    if (sys::run_hidden_process(ffmpeg_bin, args2) && fs::exists(o_p, ec) && fs::file_size(o_p, ec) > 0) {
        if (progress_cb) progress_cb(100.0);
        return true;
    }

    fs::remove(o_p, ec);
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

    std::error_code ec;
    fs::path out_p = sys::path_from_utf8(output_file);
    std::string v_usable, a_usable;
    std::string v_defrag_tmp = output_file + ".v_defrag.tmp";
    std::string a_defrag_tmp = output_file + ".a_defrag.tmp";

    auto cleanup_defrag = [&]() {
        fs::remove(sys::path_from_utf8(v_defrag_tmp), ec);
        fs::remove(sys::path_from_utf8(a_defrag_tmp), ec);
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
        if (out_error) *out_error = "Missing essential MP4 atoms (ftyp/moov/mdat) or WebM stream requires ffmpeg";
        return false;
    }

    std::ifstream v_in(sys::path_from_utf8(v_usable), std::ios::binary);
    std::ifstream a_in(sys::path_from_utf8(a_usable), std::ios::binary);
    if (!v_in.is_open() || !a_in.is_open()) {
        cleanup_defrag();
        if (out_error) *out_error = "Could not open source media files";
        return false;
    }

    std::vector<uint8_t> ftyp_data(v_ftyp->size);
    v_in.seekg(static_cast<std::streamoff>(v_ftyp->offset));
    v_in.read(reinterpret_cast<char*>(ftyp_data.data()), static_cast<std::streamsize>(v_ftyp->size));

    std::vector<uint8_t> v_moov_data(v_moov->payload_size);
    v_in.seekg(static_cast<std::streamoff>(v_moov->payload_offset));
    v_in.read(reinterpret_cast<char*>(v_moov_data.data()), static_cast<std::streamsize>(v_moov->payload_size));

    std::vector<uint8_t> a_moov_data(a_moov->payload_size);
    a_in.seekg(static_cast<std::streamoff>(a_moov->payload_offset));
    a_in.read(reinterpret_cast<char*>(a_moov_data.data()), static_cast<std::streamsize>(a_moov->payload_size));

    std::vector<uint8_t> v_trak, v_mvhd, a_trak;
    for (const auto& c : parse_children(v_moov_data.data(), 0, v_moov_data.size())) {
        if (c.type == "mvhd" && v_mvhd.empty()) {
            v_mvhd.assign(v_moov_data.data() + c.offset, v_moov_data.data() + c.offset + c.size);
        } else if (c.type == "trak" && v_trak.empty()) {
            v_trak.assign(v_moov_data.data() + c.offset, v_moov_data.data() + c.offset + c.size);
        }
    }
    for (const auto& c : parse_children(a_moov_data.data(), 0, a_moov_data.size())) {
        if (c.type == "trak" && a_trak.empty()) {
            a_trak.assign(a_moov_data.data() + c.offset, a_moov_data.data() + c.offset + c.size);
        }
    }

    if (v_trak.empty() || a_trak.empty() || v_mvhd.empty()) {
        cleanup_defrag();
        if (out_error) *out_error = "Could not locate tracks or movie header";
        return false;
    }

    set_tkhd_track_id(v_trak, 1);
    set_tkhd_track_id(a_trak, 2);

    if (v_mvhd.size() >= 108) {
        uint8_t version = v_mvhd[8];
        size_t vol_offset = (version == 0) ? 32 : 44;
        size_t next_id_offset = (version == 0) ? 104 : 116;
        if (vol_offset + 2 <= v_mvhd.size()) write_be16(&v_mvhd[vol_offset], 0x0100);
        if (next_id_offset + 4 <= v_mvhd.size()) write_be32(&v_mvhd[next_id_offset], 3);
    }

    uint64_t total_media_payload = v_mdat->payload_size + a_mdat->payload_size;
    bool extended_mdat = (total_media_payload + 8 > 0xFFFFFFFFULL);
    uint64_t mdat_header_size = extended_mdat ? 16 : 8;

    // Fixed-point loop: upgrade stco -> co64 on v_trak/a_trak FIRST so moov size & mdat payload offsets converge cleanly
    uint64_t prev_v_off = v_mdat->payload_offset;
    uint64_t prev_a_off = a_mdat->payload_offset;
    uint64_t final_v_off = 0;
    uint64_t final_a_off = 0;
    uint64_t moov_total_size = 0;

    for (int pass = 0; pass < 3; ++pass) {
        uint64_t moov_payload_size = static_cast<uint64_t>(v_mvhd.size()) + v_trak.size() + a_trak.size();
        moov_total_size = moov_payload_size + 8;
        if (moov_total_size > 0xFFFFFFFFULL) {
            cleanup_defrag();
            if (out_error) *out_error = "MP4 moov box exceeds 4 GB container limit";
            return false;
        }

        uint64_t new_mdat_box_offset = ftyp_data.size() + moov_total_size;
        final_v_off = new_mdat_box_offset + mdat_header_size;
        final_a_off = final_v_off + v_mdat->payload_size;

        size_t before_v_sz = v_trak.size();
        size_t before_a_sz = a_trak.size();
        if (!rewrite_stco_co64(v_trak, prev_v_off, final_v_off) ||
            !rewrite_stco_co64(a_trak, prev_a_off, final_a_off)) {
            cleanup_defrag();
            if (out_error) *out_error = "Failed to rewrite track chunk offsets";
            return false;
        }
        prev_v_off = final_v_off;
        prev_a_off = final_a_off;
        if (v_trak.size() == before_v_sz && a_trak.size() == before_a_sz && pass > 0) {
            break;
        }
    }

    // Check available disk space before writing multi-GB output
    fs::path out_dir_p = out_p.parent_path();
    sys::VolumeCapabilities vol = sys::inspect_target_volume(out_dir_p.empty() ? "." : sys::path_to_utf8(out_dir_p));
    uint64_t needed = ftyp_data.size() + moov_total_size + mdat_header_size + total_media_payload;
    if (vol.valid && vol.free_bytes < needed) {
        cleanup_defrag();
        if (out_error) *out_error = "Not enough disk space for muxed output file";
        return false;
    }

    std::vector<uint8_t> new_moov(static_cast<size_t>(moov_total_size));
    write_be32(&new_moov[0], static_cast<uint32_t>(moov_total_size));
    std::memcpy(&new_moov[4], "moov", 4);
    size_t write_pos = 8;
    std::memcpy(&new_moov[write_pos], v_mvhd.data(), v_mvhd.size());
    write_pos += v_mvhd.size();
    std::memcpy(&new_moov[write_pos], v_trak.data(), v_trak.size());
    write_pos += v_trak.size();
    std::memcpy(&new_moov[write_pos], a_trak.data(), a_trak.size());

    std::ofstream out(out_p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        cleanup_defrag();
        if (out_error) *out_error = "Could not open output destination file";
        return false;
    }

    out.write(reinterpret_cast<const char*>(ftyp_data.data()), static_cast<std::streamsize>(ftyp_data.size()));
    out.write(reinterpret_cast<const char*>(new_moov.data()), static_cast<std::streamsize>(new_moov.size()));

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

    constexpr size_t BUFFER_SIZE = 4 * 1024 * 1024;
    std::vector<char> buffer(BUFFER_SIZE);

    v_in.seekg(static_cast<std::streamoff>(v_mdat->payload_offset));
    uint64_t v_copied = 0;
    bool write_failed = !out.good();
    while (!write_failed && v_copied < v_mdat->payload_size) {
        size_t to_read = static_cast<size_t>(std::min<uint64_t>(BUFFER_SIZE, v_mdat->payload_size - v_copied));
        v_in.read(buffer.data(), static_cast<std::streamsize>(to_read));
        std::streamsize got = v_in.gcount();
        if (got <= 0) break;
        out.write(buffer.data(), got);
        if (!out.good()) { write_failed = true; break; }
        v_copied += static_cast<uint64_t>(got);
        if (progress_cb && total_media_payload > 0) {
            progress_cb((static_cast<double>(v_copied) / total_media_payload) * 95.0);
        }
    }

    a_in.seekg(static_cast<std::streamoff>(a_mdat->payload_offset));
    uint64_t a_copied = 0;
    while (!write_failed && a_copied < a_mdat->payload_size) {
        size_t to_read = static_cast<size_t>(std::min<uint64_t>(BUFFER_SIZE, a_mdat->payload_size - a_copied));
        a_in.read(buffer.data(), static_cast<std::streamsize>(to_read));
        std::streamsize got = a_in.gcount();
        if (got <= 0) break;
        out.write(buffer.data(), got);
        if (!out.good()) { write_failed = true; break; }
        a_copied += static_cast<uint64_t>(got);
        if (progress_cb && total_media_payload > 0) {
            progress_cb((static_cast<double>(v_copied + a_copied) / total_media_payload) * 95.0);
        }
    }

    out.flush();
    bool ok = !write_failed && out.good() && (v_copied == v_mdat->payload_size) && (a_copied == a_mdat->payload_size);
    out.close();
    v_in.close();
    a_in.close();
    cleanup_defrag();

    if (!ok) {
        fs::remove(out_p, ec);
        if (out_error && out_error->empty()) {
            *out_error = "Failed while writing muxed output MP4";
        }
        return false;
    }

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

    fs::path out_p = sys::path_from_utf8(output_file);
    bool remux_to_mp4 = (sys::ascii_tolower(sys::path_to_utf8(out_p.extension())) == ".mp4");
    std::string ts_target = remux_to_mp4 ? (output_file + ".concat.ts") : output_file;
    fs::path ts_target_p = sys::path_from_utf8(ts_target);

    std::ofstream out(ts_target_p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (out_error) *out_error = "Could not open output TS file";
        return false;
    }

    constexpr size_t BUFFER_SIZE = 2 * 1024 * 1024;
    std::vector<char> buffer(BUFFER_SIZE);
    size_t total_files = segment_files.size();
    std::error_code ec;

    for (size_t idx = 0; idx < total_files; ++idx) {
        std::ifstream in(sys::path_from_utf8(segment_files[idx]), std::ios::binary);
        if (!in.is_open()) {
            out.close();
            fs::remove(ts_target_p, ec);
            if (out_error) *out_error = "Missing TS segment: " + segment_files[idx];
            return false;
        }
        while (in.read(buffer.data(), static_cast<std::streamsize>(BUFFER_SIZE)) || in.gcount() > 0) {
            out.write(buffer.data(), in.gcount());
            if (out.fail()) {
                out.close();
                fs::remove(ts_target_p, ec);
                if (out_error) *out_error = "Disk write error while concatenating TS segments";
                return false;
            }
        }
        if (progress_cb) {
            progress_cb((static_cast<double>(idx + 1) / total_files) * (remux_to_mp4 ? 85.0 : 100.0));
        }
    }

    out.flush();
    bool write_ok = out.good();
    out.close();
    if (!write_ok) {
        fs::remove(ts_target_p, ec);
        return false;
    }

    if (remux_to_mp4) {
        bool conv_ok = convert_ts_to_mp4(ts_target, output_file, progress_cb, out_error);
        fs::remove(ts_target_p, ec);
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
        std::error_code ec;
        fs::path ts_p = sys::path_from_utf8(ts_file);
        fs::path out_p = sys::path_from_utf8(output_file);
        uint64_t ts_sz = fs::exists(ts_p, ec) ? fs::file_size(ts_p, ec) : 0;
        bool use_faststart = (ts_sz > 0 && ts_sz < 512ULL * 1024 * 1024);

        std::vector<std::string> args1 = {
            "-y", "-loglevel", "error", "-i", ts_file, "-c", "copy", "-bsf:a", "aac_adtstoasc"
        };
        if (use_faststart) {
            args1.push_back("-movflags");
            args1.push_back("+faststart");
        }
        args1.push_back(output_file);

        if (sys::run_hidden_process(ffmpeg_bin, args1) && fs::exists(out_p, ec) && fs::file_size(out_p, ec) > 0) {
            if (progress_cb) progress_cb(100.0);
            return true;
        }

        std::vector<std::string> args2 = {
            "-y", "-loglevel", "error", "-i", ts_file, "-c", "copy"
        };
        if (use_faststart) {
            args2.push_back("-movflags");
            args2.push_back("+faststart");
        }
        args2.push_back(output_file);

        if (sys::run_hidden_process(ffmpeg_bin, args2) && fs::exists(out_p, ec) && fs::file_size(out_p, ec) > 0) {
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
