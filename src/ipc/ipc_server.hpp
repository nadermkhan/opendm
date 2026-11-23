#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include <mutex>
#include <condition_variable>

#ifdef _WIN32
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#endif

namespace opendm {

class DownloadQueue;

struct DownloadPromptInfo {
    std::string url;
    std::string filename;
    std::string destination_dir;
    std::string save_as_path;
    std::string category;
    uint64_t file_size{0};
    std::string description;
    std::string referer;
    std::string cookies;
    std::string user_agent;
    std::string post_data;
    std::string username;
    std::string password;
    bool is_archive{false};
    std::string audio_stream_url;
    bool is_streaming_media{false};
};

using DownloadPromptCallback = std::function<void(const DownloadPromptInfo& info)>;

class IpcServer {
public:
#ifdef _WIN32
    static constexpr const char* DEFAULT_SOCKET_PATH = "\\\\.\\pipe\\opendm_pipe";
#else
    static constexpr const char* DEFAULT_SOCKET_PATH = "/tmp/opendm.sock";
#endif

    explicit IpcServer(const std::string& socket_path = DEFAULT_SOCKET_PATH);
    ~IpcServer();

    bool start();
    void stop();
    bool is_running() const;

    void set_gui_mode(bool gui_mode);
    void set_download_prompt_callback(DownloadPromptCallback cb);

    // Send a command to a running OpenDM instance via UNIX socket / Named Pipe (client mode)
    static std::string send_command(const std::string& json_request,
                                    const std::string& socket_path = DEFAULT_SOCKET_PATH,
                                    int max_connect_attempts = 20);

    // Run local YouTube format extraction and serialize IPC JSON response
    static std::string extract_youtube_json(const std::string& url, const std::string& video_id = "");

private:
    struct InterceptRequest {
        std::string raw_url;
        std::string req_audio;
        std::string filename;
        std::string dest_dir;
        uint64_t file_size{0};
        std::string referer;
        std::string page_url;
        std::string page_title;
        std::string cookies;
        std::string user_agent;
        std::string resp_headers;
        std::string content_type;
        int status_code{0};
        bool is_attachment{false};
        bool is_explicit_user_action{false};
        bool enforce_web_filter{true};
        bool no_prompt{false};
        bool add_only{false};
        unsigned long connections{0};
        uint64_t speed_limit_kb{0};
    };

    std::string handle_intercept_download(const InterceptRequest& req_info);
    void server_loop();
    std::string handle_request(const std::string& request);

    std::string socket_path_;
#ifdef _WIN32
    HANDLE stop_event_{nullptr};
#else
    int server_fd_{-1};
#endif
    std::atomic<bool> running_{false};
    std::atomic<bool> gui_mode_{true};
    std::thread server_thread_;

    std::mutex prompt_mutex_;
    DownloadPromptCallback prompt_callback_;
    std::vector<DownloadPromptInfo> pending_prompts_;

    std::mutex workers_mutex_;
    std::condition_variable workers_cv_;
    size_t active_workers_{0};
};

struct MsgHeader {
    uint32_t id{0};
    uint32_t type{0};
    uint32_t subtype{0};
    uint32_t flags{0};
    bool valid{false};
};

// Tonec protocol parsing helpers
MsgHeader parse_msg_header(const std::string& msg);
std::vector<std::string> parse_tonec_pos_args(const std::string& msg);
std::string extract_tonec_field(const std::string& msg, int field_id);

// Content and extension filtering helpers matching IDM behavior
bool is_downloadable_extension(const std::string& ext);
bool is_forbidden_web_extension(const std::string& ext);
bool is_web_content_type(const std::string& content_type);

// Media stream scheme and query parameter normalization (IDM compatibility)
std::string normalize_stream_scheme(const std::string& raw_url);
std::pair<std::string, std::string> split_dual_stream_url(const std::string& raw_url);
std::string clean_stream_query_parameters(const std::string& url);
std::string sanitize_media_filename(const std::string& title, const std::string& default_ext = "mp4");

struct YouTubeVideoInfo;
std::string build_youtube_extract_json(const YouTubeVideoInfo& info);

} // namespace opendm
