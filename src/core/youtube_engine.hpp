#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <memory>

namespace opendm {

struct YouTubeFormat {
    int itag{0};
    std::string quality_label; // e.g. "1080p", "720p", "360p"
    int width{0};
    int height{0};
    int fps{0};
    std::string mime_type;     // e.g. "video/mp4; codecs=\"avc1.640028\""
    std::string container;     // "MP4", "WEBM", "M4A"
    std::string vcodec;        // "avc1", "vp9", "av01"
    std::string acodec;        // "mp4a.40.2", "opus"
    uint64_t bitrate{0};
    uint64_t content_length{0};
    std::string url;
    std::string user_agent;
    bool is_progressive{false};
    bool is_audio{false};
};

struct YouTubeVideoInfo {
    std::string video_id;
    std::string title;
    std::string author;
    uint64_t duration_seconds{0};
    std::string thumbnail_url;
    std::string client_used;

    std::vector<YouTubeFormat> progressive_formats;
    std::vector<YouTubeFormat> video_formats;
    std::vector<YouTubeFormat> audio_formats;

    YouTubeFormat best_video;
    YouTubeFormat best_audio;
    YouTubeFormat best_progressive;

    bool has_playable_formats() const {
        return !progressive_formats.empty() || (!video_formats.empty() && !audio_formats.empty());
    }
};

class YouTubeEngine {
public:
    // URL pattern recognition & ID extraction
    static bool is_youtube_url(const std::string& url);
    static std::string extract_video_id(const std::string& url);

    // Core extraction following yt-dlp's multi-client fallback hierarchy
    static bool extract_video_info(
        const std::string& url_or_id,
        YouTubeVideoInfo& out_info,
        std::string* out_error = nullptr);

    // Range request prober: verifies stream accepts range requests and is not 403-capped
    static bool probe_stream_range(
        const std::string& url,
        const std::string& user_agent,
        uint64_t content_length = 0);

    // Helper: format duration string e.g. "03:45"
    static std::string format_duration(uint64_t seconds);

    // Helper: format bitrate / size string
    static std::string format_bitrate(uint64_t bps);

    // Visitor data session token helper (bypasses YouTube anti-bot / login_required checks)
    static std::string get_or_fetch_visitor_data(const std::string& video_id);
    static void invalidate_visitor_data();

private:
    struct InnerTubeClient {
        std::string name;
        std::string client_name;
        std::string client_version;
        std::string client_id;
        int android_sdk_version{0};
        std::string os_name;
        std::string os_version;
        std::string platform;
        std::string client_form_factor;
        std::string device_make;
        std::string device_model;
        std::string user_agent;
        std::string embed_url;
    };

    static std::vector<InnerTubeClient> get_client_hierarchy();

    static bool query_innertube_client(
        const InnerTubeClient& client,
        const std::string& video_id,
        const std::string& visitor_data,
        std::string& out_response_json,
        std::string* out_error);

    static bool parse_player_response(
        const std::string& json_str,
        const InnerTubeClient& client,
        const std::string& video_id,
        YouTubeVideoInfo& out_info,
        std::string* out_error);

    static bool is_valid_stream_url(const std::string& url);
    static void parse_codecs_from_mime(
        const std::string& mime,
        std::string& out_container,
        std::string& out_vcodec,
        std::string& out_acodec);
};

} // namespace opendm
