#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace opendm {

// Progress callback receiving completion percentage (0.0 to 100.0)
using MuxProgressCallback = std::function<void(double percent)>;

class MediaMuxer {
public:
    // Merges separate video and audio streams (MP4/M4V + M4A/AAC) into a single MP4 container
    // without re-encoding (stream copying, identical to IDM idmvconv.dll).
    static bool mux_mp4_and_mp4(
        const std::string& video_file,
        const std::string& audio_file,
        const std::string& output_file,
        MuxProgressCallback progress_cb = nullptr,
        std::string* out_error = nullptr);

    // Concatenates multiple MPEG-TS segment files into a single unified TS or MP4 stream
    static bool concat_ts_segments(
        const std::vector<std::string>& segment_files,
        const std::string& output_file,
        MuxProgressCallback progress_cb = nullptr,
        std::string* out_error = nullptr);

    // Converts MPEG-TS stream to MP4 container (ConvertTsToMp4 in idmvconv.dll)
    static bool convert_ts_to_mp4(
        const std::string& ts_file,
        const std::string& output_file,
        MuxProgressCallback progress_cb = nullptr,
        std::string* out_error = nullptr);

    // General stream muxer: inspects containers and muxes accordingly
    static bool mux_streams(
        const std::string& video_file,
        const std::string& audio_file,
        const std::string& output_file,
        MuxProgressCallback progress_cb = nullptr,
        std::string* out_error = nullptr);

    // Resolves available ffmpeg executable from system paths and user directories
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
    static bool parse_box_recursive(
        const std::vector<uint8_t>& data,
        uint64_t offset,
        uint64_t limit,
        std::vector<BoxInfo>& boxes);

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
