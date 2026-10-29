#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace opendm {

using MuxProgressCallback = std::function<void(double percent)>;

class MediaMuxer {
public:
    static bool mux_mp4_and_mp4(
        const std::string& video_file,
        const std::string& audio_file,
        const std::string& output_file,
        MuxProgressCallback progress_cb = nullptr,
        std::string* out_error = nullptr);

    static bool concat_ts_segments(
        const std::vector<std::string>& segment_files,
        const std::string& output_file,
        MuxProgressCallback progress_cb = nullptr,
        std::string* out_error = nullptr);

    static bool convert_ts_to_mp4(
        const std::string& ts_file,
        const std::string& output_file,
        MuxProgressCallback progress_cb = nullptr,
        std::string* out_error = nullptr);

    static bool mux_streams(
        const std::string& video_file,
        const std::string& audio_file,
        const std::string& output_file,
        MuxProgressCallback progress_cb = nullptr,
        std::string* out_error = nullptr);

    static std::string find_ffmpeg();

private:
    struct BoxInfo {
        std::string type;
        uint64_t offset;
        uint64_t size;
        uint64_t payload_offset;
        uint64_t payload_size;
    };

    static std::vector<BoxInfo> scan_mp4_boxes(const std::string& file_path);

    static bool defragment_mp4_if_needed(
        const std::string& input_file,
        const std::string& temp_defrag_file,
        std::string& out_usable_file);

    static bool rewrite_stco_co64(
        std::vector<uint8_t>& trak_data,
        uint64_t old_mdat_payload_offset,
        uint64_t new_mdat_payload_offset);

    static void set_tkhd_track_id(std::vector<uint8_t>& trak_data, uint32_t new_id);

    static bool try_external_ffmpeg(
        const std::string& video_file,
        const std::string& audio_file,
        const std::string& output_file,
        MuxProgressCallback progress_cb);
};

} // namespace opendm

