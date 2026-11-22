#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace opendm {

enum class CompletionAction {
    NONE = 0,
    EXIT_APP = 1,
    SUSPEND_SYSTEM = 2,
    SHUTDOWN_SYSTEM = 3
};

class PostDownloadManager {
public:
    static PostDownloadManager& instance();

    // Sends a desktop notification when a download finishes (callback on Windows, notify-send on Linux)
    void notify_completion(const std::string& filename, int64_t total_size, bool success, const std::string& error_msg = "");

    // Optional UI notification hook
    using NotificationCallback = std::function<void(const std::string& filename, int64_t total_size, bool success, const std::string& error_msg)>;
    void set_notification_callback(NotificationCallback cb);

    // Triggers an antivirus scan on the completed file without shell injection or console flash
    bool scan_file(const std::string& filepath, std::string* out_scan_report = nullptr);

    void execute_completion_action(CompletionAction action);

    void set_av_command(const std::string& cmd);
    std::string get_av_command() const;

    void set_default_completion_action(CompletionAction action) { default_action_.store(action); }
    CompletionAction get_default_completion_action() const { return default_action_.load(); }

private:
    PostDownloadManager() = default;
    ~PostDownloadManager() = default;

    mutable std::mutex mutex_;
    std::string av_scan_cmd_;
    NotificationCallback notify_cb_;
    std::atomic<CompletionAction> default_action_{CompletionAction::NONE};
};

} // namespace opendm
