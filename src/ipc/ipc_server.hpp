#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>

#include <vector>
#include <mutex>

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
    static constexpr const char* DEFAULT_SOCKET_PATH = "/tmp/opendm.sock";

    explicit IpcServer(const std::string& socket_path = DEFAULT_SOCKET_PATH);
    ~IpcServer();

    bool start();
    void stop();
    bool is_running() const;

    void set_gui_mode(bool gui_mode);
    void set_download_prompt_callback(DownloadPromptCallback cb);

    // Send a command to a running OpenDM instance via UNIX socket (client mode)
    static std::string send_command(const std::string& json_request,
                                    const std::string& socket_path = DEFAULT_SOCKET_PATH);

private:
    void server_loop();
    std::string handle_request(const std::string& request);

    std::string socket_path_;
    int server_fd_{-1};
    std::atomic<bool> running_{false};
    std::atomic<bool> gui_mode_{true};
    std::thread server_thread_;

    std::mutex prompt_mutex_;
    DownloadPromptCallback prompt_callback_;
    std::vector<DownloadPromptInfo> pending_prompts_;
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

} // namespace opendm
