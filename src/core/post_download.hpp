#pragma once

#include <string>
#include <functional>

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

    // Sends a native desktop notification when download completes
    void notify_completion(const std::string& filename, int64_t total_size, bool success, const std::string& error_msg = "");

    // Triggers an antivirus scan on the completed file (if configured or clamscan present)
    bool scan_file(const std::string& filepath, std::string* out_scan_report = nullptr);

    // Executes post-download scheduler actions (Shut down, Suspend, or Exit)
    void execute_completion_action(CompletionAction action);

    // Configuration
    void set_av_command(const std::string& cmd) { av_scan_cmd_ = cmd; }
    std::string get_av_command() const { return av_scan_cmd_; }

    void set_default_completion_action(CompletionAction action) { default_action_ = action; }
    CompletionAction get_default_completion_action() const { return default_action_; }

private:
    PostDownloadManager() = default;
    ~PostDownloadManager() = default;

    std::string av_scan_cmd_;
    CompletionAction default_action_{CompletionAction::NONE};
};

} // namespace opendm
