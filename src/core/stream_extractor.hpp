#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <functional>

namespace opendm {

struct StreamFormat {
    std::string format_id;
    std::string label;       // e.g. "1080p MP4 (Video + Audio)"
    std::string ext;         // "mp4", "m4a", "webm"
    uint64_t filesize{0};    // approximate bytes
    std::string video_url;   // direct stream URL for video
    std::string audio_url;   // direct stream URL for audio (empty if combined or audio-only)
    std::string vcodec;
    std::string acodec;
    int width{0};
    int height{0};
    int fps{0};
    bool is_audio_only{false};
    bool requires_muxing{false};
};

struct StreamMediaInfo {
    std::string title;
    std::string uploader;
    std::string webpage_url;
    std::string thumbnail_url;
    double duration_seconds{0.0};
    std::vector<StreamFormat> formats;
};

class StreamExtractor {
public:
    // Checks if yt-dlp executable exists on the system ($PATH, ~/.local/bin/yt-dlp, etc.)
    static bool is_available();

    // Returns the path to the yt-dlp executable
    static std::string get_extractor_binary();

    // Returns the path to the node executable for JavaScript execution
    static std::string get_node_binary();

    // Checks whether the given URL is a known streaming host (YouTube, Vimeo, Twitch, etc.)
    static bool is_streaming_url(const std::string& url);

    // Synchronously extracts format descriptors and direct stream URLs
    static bool extract_info(
        const std::string& url,
        StreamMediaInfo& out_info,
        std::string* out_error = nullptr);

    // Asynchronous helper that runs extract_info on a worker thread
    static void extract_info_async(
        const std::string& url,
        std::function<void(bool success, const StreamMediaInfo& info, const std::string& error)> callback);

    // Helper to sanitize title into a clean filename
    static std::string sanitize_filename(const std::string& title, const std::string& ext = "mp4");
};

} // namespace opendm
