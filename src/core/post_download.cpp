#include "post_download.hpp"
#include "win32_unicode.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace opendm {

namespace {

std::vector<std::string> split_command_tokens(const std::string& cmd) {
    std::vector<std::string> tokens;
    std::string cur;
    bool in_quotes = false;
    char quote_char = 0;

    for (size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (in_quotes) {
            if (c == quote_char) {
                in_quotes = false;
            } else {
                cur.push_back(c);
            }
        } else if (c == '"' || c == '\'') {
            in_quotes = true;
            quote_char = c;
        } else if (c == ' ' || c == '\t') {
            if (!cur.empty()) {
                tokens.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));
    return tokens;
}

std::string find_windows_defender_cli() {
#ifdef _WIN32
    std::error_code ec;
    std::string prog_data = sys::get_env_utf8("ProgramData");
    if (!prog_data.empty()) {
        fs::path plat = sys::path_from_utf8(prog_data) / "Microsoft" / "Windows Defender" / "Platform";
        if (fs::exists(plat, ec) && fs::is_directory(plat, ec)) {
            std::vector<fs::path> versions;
            for (const auto& entry : fs::directory_iterator(plat, ec)) {
                if (entry.is_directory(ec)) {
                    fs::path cand = entry.path() / "MpCmdRun.exe";
                    if (fs::exists(cand, ec)) {
                        versions.push_back(cand);
                    }
                }
            }
            if (!versions.empty()) {
                std::sort(versions.begin(), versions.end());
                return sys::path_to_utf8(versions.back());
            }
        }
    }
    std::string prog_files = sys::get_env_utf8("ProgramFiles");
    if (!prog_files.empty()) {
        fs::path cand = sys::path_from_utf8(prog_files) / "Windows Defender" / "MpCmdRun.exe";
        if (fs::exists(cand, ec)) {
            return sys::path_to_utf8(cand);
        }
    }
#endif
    return {};
}

} // namespace

PostDownloadManager& PostDownloadManager::instance() {
    static PostDownloadManager inst;
    return inst;
}

void PostDownloadManager::set_av_command(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    av_scan_cmd_ = cmd;
}

std::string PostDownloadManager::get_av_command() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return av_scan_cmd_;
}

void PostDownloadManager::set_notification_callback(NotificationCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    notify_cb_ = std::move(cb);
}

void PostDownloadManager::notify_completion(
    const std::string& filename,
    int64_t total_size,
    bool success,
    const std::string& error_msg) {

    NotificationCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = notify_cb_;
    }
    if (cb) {
        cb(filename, total_size, success, error_msg);
        return;
    }

#ifndef _WIN32
    std::string title = success ? "Download Complete" : "Download Failed";
    std::string body = filename;
    if (success) {
        body += " (" + sys::format_bytes(total_size) + ")";
    } else if (!error_msg.empty()) {
        body += ": " + error_msg;
    }
    std::string icon = success ? "document-save" : "dialog-error";
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp("notify-send", "notify-send", "-i", icon.c_str(), "-a", "OpenDM",
               title.c_str(), body.c_str(), nullptr);
        _exit(1);
    }
#else
    (void)filename;
    (void)total_size;
    (void)success;
    (void)error_msg;
#endif
}

bool PostDownloadManager::scan_file(const std::string& filepath, std::string* out_scan_report) {
    if (filepath.empty()) return false;

    std::string custom_cmd = get_av_command();
    std::string exe;
    std::vector<std::string> args;

    if (!custom_cmd.empty()) {
        auto tokens = split_command_tokens(custom_cmd);
        if (tokens.empty()) return false;
        exe = tokens[0];
        bool replaced = false;
        for (size_t i = 1; i < tokens.size(); ++i) {
            std::string tok = tokens[i];
            size_t pos = tok.find("%file");
            if (pos != std::string::npos) {
                tok.replace(pos, 5, filepath);
                replaced = true;
            }
            args.push_back(std::move(tok));
        }
        if (!replaced) {
            args.push_back(filepath);
        }
    } else {
#ifdef _WIN32
        exe = find_windows_defender_cli();
        if (exe.empty()) {
            return false;
        }
        args = {"-Scan", "-ScanType", "3", "-File", filepath};
#else
        exe = "clamscan";
        args = {"--no-summary", filepath};
#endif
    }

    std::string output;
    int exit_code = -1;
    bool ok = sys::run_hidden_process(
        exe, args,
        [&output](std::string_view line) {
            if (!output.empty()) output.push_back('\n');
            output.append(line);
        },
        nullptr, &exit_code);

    if (out_scan_report) {
        *out_scan_report = output;
    }
    return ok && exit_code == 0;
}

void PostDownloadManager::execute_completion_action(CompletionAction action) {
    switch (action) {
        case CompletionAction::EXIT_APP:
            std::cout << "[OpenDM] Completion action: Exiting application." << std::endl;
            std::exit(0);
        case CompletionAction::SUSPEND_SYSTEM:
#ifdef _WIN32
            std::thread([]() {
                sys::run_hidden_process("rundll32.exe", {"powrprof.dll,SetSuspendState", "0,1,0"});
            }).detach();
#else
            {
                pid_t pid = fork();
                if (pid == 0) {
                    setsid();
                    execlp("systemctl", "systemctl", "suspend", nullptr);
                    _exit(1);
                }
            }
#endif
            break;
        case CompletionAction::SHUTDOWN_SYSTEM:
#ifdef _WIN32
            std::thread([]() {
                sys::run_hidden_process("shutdown.exe", {"/s", "/t", "5"});
            }).detach();
#else
            {
                pid_t pid = fork();
                if (pid == 0) {
                    setsid();
                    execlp("systemctl", "systemctl", "poweroff", nullptr);
                    _exit(1);
                }
            }
#endif
            break;
        case CompletionAction::NONE:
        default:
            break;
    }
}

} // namespace opendm
