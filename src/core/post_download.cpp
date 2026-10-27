#include "post_download.hpp"
#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <filesystem>
#include <sstream>
#include <iomanip>

namespace opendm {

namespace {

std::string format_file_size(int64_t bytes) {
    if (bytes < 0) return "Unknown size";
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double d = static_cast<double>(bytes);
    while (d >= 1024.0 && unit_idx < 4) {
        d /= 1024.0;
        unit_idx++;
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << d << " " << units[unit_idx];
    return ss.str();
}

void async_exec(const std::string& cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child
        setsid();
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(1);
    }
}

} // namespace

PostDownloadManager& PostDownloadManager::instance() {
    static PostDownloadManager inst;
    return inst;
}

void PostDownloadManager::notify_completion(
    const std::string& filename,
    int64_t total_size,
    bool success,
    const std::string& error_msg) {

    std::string title = success ? "Download Complete" : "Download Failed";
    std::string body = filename;
    if (success) {
        body += " (" + format_file_size(total_size) + ")";
    } else if (!error_msg.empty()) {
        body += ": " + error_msg;
    }

    std::string icon = success ? "document-save" : "dialog-error";

    // Escape quotes for shell safety
    std::string safe_body;
    for (char c : body) {
        if (c == '"' || c == '\\' || c == '`' || c == '$') safe_body += '\\';
        safe_body += c;
    }

    std::string cmd = "notify-send -i " + icon + " -a OpenDM \"" + title + "\" \"" + safe_body + "\" 2>/dev/null";
    async_exec(cmd);
}

bool PostDownloadManager::scan_file(const std::string& filepath, std::string* out_scan_report) {
    std::string cmd;
    if (!av_scan_cmd_.empty()) {
        cmd = av_scan_cmd_;
        size_t pos = cmd.find("%file");
        if (pos != std::string::npos) {
            cmd.replace(pos, 5, "\"" + filepath + "\"");
        } else {
            cmd += " \"" + filepath + "\"";
        }
    } else {
        // Fallback to clamscan if present
        if (system("which clamscan >/dev/null 2>&1") == 0) {
            cmd = "clamscan --no-summary \"" + filepath + "\"";
        } else {
            return false;
        }
    }

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;

    char buffer[256];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    int status = pclose(pipe);

    if (out_scan_report) *out_scan_report = output;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void PostDownloadManager::execute_completion_action(CompletionAction action) {
    switch (action) {
        case CompletionAction::EXIT_APP:
            std::cout << "[OpenDM] Completion action: Exiting application." << std::endl;
            exit(0);
            break;
        case CompletionAction::SUSPEND_SYSTEM:
            std::cout << "[OpenDM] Completion action: Suspending system." << std::endl;
            async_exec("systemctl suspend 2>/dev/null || dbus-send --system --print-reply --dest=org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager.Suspend boolean:true");
            break;
        case CompletionAction::SHUTDOWN_SYSTEM:
            std::cout << "[OpenDM] Completion action: Shutting down system." << std::endl;
            async_exec("systemctl poweroff 2>/dev/null || dbus-send --system --print-reply --dest=org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager.PowerOff boolean:true");
            break;
        case CompletionAction::NONE:
        default:
            break;
    }
}

} // namespace opendm
