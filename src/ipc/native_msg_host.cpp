#include "ipc_server.hpp"
#include "../core/win32_unicode.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
#include <csignal>
#include <chrono>
#include <thread>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

namespace opendm {

using json = nlohmann::json;

namespace {

// Chrome Native Messaging caps host -> extension messages at 1 MiB; stay safely below.
constexpr size_t kMaxNativeResponseBytes = 950 * 1024;

std::mutex g_log_mutex;
std::filesystem::path g_log_path;

void write_host_log(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_path.empty()) return;
    std::ofstream log(g_log_path, std::ios::app);
    if (log.is_open()) {
        log << line << "\n";
        log.flush();
    }
}

bool read_native_message(std::string& out_msg) {
    uint32_t length = 0;
    if (!std::cin.read(reinterpret_cast<char*>(&length), sizeof(length))) {
        return false;
    }
    if (length == 0 || length > 10 * 1024 * 1024) {
        return false;
    }
    out_msg.resize(length);
    if (!std::cin.read(&out_msg[0], length)) {
        return false;
    }
    return true;
}

void write_native_frame(const std::string& serialized) {
    std::string payload = serialized;
    if (payload.size() > kMaxNativeResponseBytes) {
        try {
            json parsed = json::parse(payload);
            if (parsed.is_object() && parsed.contains("options") && parsed["options"].is_array()) {
                auto& opts = parsed["options"];
                while (!opts.empty() && payload.size() > kMaxNativeResponseBytes) {
                    opts.erase(opts.end() - 1);
                    payload = parsed.dump();
                }
            }
        } catch (...) {}

        if (payload.size() > kMaxNativeResponseBytes) {
            payload = json({
                {"status", "error"},
                {"message", "Native host response exceeded 950 KB limit"}
            }).dump();
        }
    }

    uint32_t length = static_cast<uint32_t>(payload.size());
    std::cout.write(reinterpret_cast<const char*>(&length), sizeof(length));
    std::cout.write(payload.data(), static_cast<std::streamsize>(length));
    std::cout.flush();
}

void write_native_json_string(const std::string& raw_msg) {
    write_native_frame(json(raw_msg).dump());
}

void write_native_raw_json(const std::string& json_obj) {
    write_native_frame(json_obj);
}

bool is_ipc_connect_error(const std::string& resp) {
    return resp.empty() || resp.find("Could not connect") != std::string::npos;
}

bool ping_running_app(bool require_ui_ready = false) {
    std::string resp = IpcServer::send_command(
        "{\"action\":\"ping\"}",
        IpcServer::DEFAULT_SOCKET_PATH,
        1);
    if (is_ipc_connect_error(resp)) {
        return false;
    }
    try {
        json j = json::parse(resp);
        if (j.value("status", "") != "ok") return false;
        if (require_ui_ready && j.contains("ui_ready")) {
            return j.value("ui_ready", true);
        }
        return true;
    } catch (...) {
        return resp.find("\"pong\"") != std::string::npos;
    }
}

bool ensure_opendm_running() {
    if (ping_running_app(true)) {
        return true;
    }

#ifdef _WIN32
    struct LaunchMutexGuard {
        HANDLE h{nullptr};
        bool owned{false};
        ~LaunchMutexGuard() {
            if (owned && h) ReleaseMutex(h);
            if (h) CloseHandle(h);
        }
    } mutex_guard;

    mutex_guard.h = CreateMutexW(nullptr, FALSE, L"Local\\OpenDM_LaunchMutex");
    if (mutex_guard.h) {
        DWORD wait_rc = WaitForSingleObject(mutex_guard.h, 8000);
        if (wait_rc == WAIT_OBJECT_0 || wait_rc == WAIT_ABANDONED) {
            mutex_guard.owned = true;
        } else if (wait_rc == WAIT_TIMEOUT) {
            write_host_log("[NATIVE_HOST] ensure_opendm_running: launch_mutex_timeout");
            return ping_running_app(true);
        }
    } else {
        write_host_log("[NATIVE_HOST] CreateMutexW(OpenDM_LaunchMutex) failed err=" + std::to_string(GetLastError()));
    }

    if (ping_running_app(true)) {
        return true;
    }

    std::string exe_dir = sys::get_exe_dir_utf8();
    std::filesystem::path app_exe = sys::path_from_utf8(exe_dir) / "opendm.exe";
    std::error_code ec;
    if (!std::filesystem::exists(app_exe, ec)) {
        write_host_log("[NATIVE_HOST] opendm.exe missing at " + sys::path_to_utf8(app_exe));
        return false;
    }

    sys::DetachedLaunchOptions launch_opts;
    launch_opts.exe_path = sys::path_to_utf8(app_exe);
    launch_opts.arguments = "--background";
    launch_opts.working_dir = exe_dir;
    launch_opts.show_cmd = SW_SHOWMINNOACTIVE;
    launch_opts.allow_foreground = true;

    void* raw_child_handle = nullptr;
    auto launch_res = sys::launch_detached_gui_process(launch_opts, &raw_child_handle);
    HANDLE hChild = static_cast<HANDLE>(raw_child_handle);

    const uint32_t il_rid = sys::get_process_integrity_rid();
    write_host_log(
        "[NATIVE_HOST] launch attempt method=" + launch_res.method +
        " ok=" + std::string(launch_res.ok ? "true" : "false") +
        " in_job=" + std::string(launch_res.in_job ? "true" : "false") +
        " breakaway_ok=" + std::string(launch_res.breakaway_allowed ? "true" : "false") +
        " il_rid=0x" + ([&]() {
            std::ostringstream oss;
            oss << std::hex << il_rid;
            return oss.str();
        })() +
        " err=" + std::to_string(launch_res.win32_error) +
        (launch_res.error_message.empty() ? "" : (" msg=" + launch_res.error_message)));

    if (!launch_res.ok) {
        if (hChild) CloseHandle(hChild);
        return false;
    }

    bool ready = false;
    for (int i = 0; i < 120; ++i) {
        if (hChild && WaitForSingleObject(hChild, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(hChild, &code);
            write_host_log("[NATIVE_HOST] spawned_process_exited_early code=" + std::to_string(code));
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (ping_running_app(true)) {
            ready = true;
            break;
        }
    }
    if (hChild) CloseHandle(hChild);
    if (!ready) {
        write_host_log("[NATIVE_HOST] ensure_opendm_running: ready ping timeout after 6000ms");
    }
    return ready;
#else
    std::string exe_dir = sys::get_exe_dir_utf8();
    std::vector<std::string> candidates;
    if (!exe_dir.empty()) {
        candidates.push_back(exe_dir + "/opendm");
    }
    std::string home = sys::get_env_utf8("HOME");
    if (!home.empty()) {
        candidates.push_back(home + "/.local/bin/opendm");
    }
    candidates.push_back("/usr/local/bin/opendm");
    candidates.push_back("/usr/bin/opendm");

    std::string opendm_bin = "opendm";
    std::error_code ec;
    for (const auto& c : candidates) {
        if (std::filesystem::exists(c, ec)) {
            opendm_bin = c;
            break;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        write_host_log("[NATIVE_HOST] fork() failed");
        return false;
    }
    if (pid == 0) {
        setsid();
        // Detach stdio from Chromium/Firefox native-messaging pipes cleanly
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        const char* argv[] = {opendm_bin.c_str(), nullptr};
        execvp(opendm_bin.c_str(), const_cast<char* const*>(argv));
        _exit(127);
    }

    for (int i = 0; i < 120; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (ping_running_app(true)) {
            return true;
        }
    }
    write_host_log("[NATIVE_HOST] POSIX ensure_opendm_running timeout");
    return false;
#endif
}

std::string send_command_spawning_if_needed(const std::string& payload) {
#ifdef _WIN32
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    std::string resp = IpcServer::send_command(payload, IpcServer::DEFAULT_SOCKET_PATH, 3);
    if (!is_ipc_connect_error(resp)) {
        return resp;
    }
    if (ensure_opendm_running()) {
#ifdef _WIN32
        AllowSetForegroundWindow(ASFW_ANY);
#endif
        return IpcServer::send_command(payload, IpcServer::DEFAULT_SOCKET_PATH, 5);
    }
    return resp;
}

} // namespace

} // namespace opendm

int main(int argc, char* argv[]) {
    curl_global_init(CURL_GLOBAL_ALL);
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::string tmp_dir = opendm::sys::get_env_utf8("TEMP");
    opendm::g_log_path = tmp_dir.empty()
        ? std::filesystem::path("opendm_native_host.log")
        : (opendm::sys::path_from_utf8(tmp_dir) / "opendm_native_host.log");
#else
    signal(SIGCHLD, SIG_IGN);
    opendm::g_log_path = "/tmp/opendm_native_host.log";
#endif
    std::ios_base::sync_with_stdio(false);

    std::vector<std::string> args = opendm::sys::get_cli_args_utf8(argc, argv);
    opendm::write_host_log("[NATIVE_HOST] Started. argc=" + std::to_string(args.size()));

    std::string incoming;
    while (opendm::read_native_message(incoming)) {
        if (incoming.empty()) continue;
        opendm::write_host_log("[NATIVE_HOST] incoming: " + opendm::redact_for_log(incoming));

        std::string raw = opendm::unquote_json_string(incoming);

        // Legacy Tonec MSG# handshake & interception support (kept ONLY for legacy callers)
        if (raw.rfind("MSG#", 0) == 0) {
            opendm::MsgHeader hdr = opendm::parse_msg_header(raw);
            if (hdr.type == 2) {
                opendm::write_native_json_string("MSG#1#3#0#0:113:0:0:0:96:103481600:0:0:100;");
                opendm::write_native_json_string(
                    "MSG#2#4#0#1918:0:0:0:0,12=2:.*,1=245:ZIP|EXE|7Z|RAR|TAR|GZ|BZ2|ISO|DMG|DEB|RPM|PKG|MP4|MKV|AVI|MP3|FLAC|WAV|PDF|EPUB|DOC|DOCX|XLS|XLSX|PPT|PPTX|APK|TORRENT|3GP|AAC|ACE|AIF|ARJ|ASF|BIN|M4A|M4V|MOV|MPA|MPE|MPEG|MPG|MSI|MSU|OGG|OGV|PLJ|PPS|QT|R0*|R1*|RA|SEA|SIT|SITX|TIF|TIFF|WMA|WMV|Z,2=55:CSS|JS|JSON|CRL|OCSP|APNG|GIF|ICO|JPEG|JPG|PNG|SVG|WEBP,3=97:3GP|ASF|AVI|F4V|FLV|M4A|M4V|MKV|MOV|MP3|MP4|MPEG|MPG|OGG|OGV|QT|RA|RM|TS|TSA|TSV|WAV|WEBM|WMA|WMV,4=30:JSON|GZ|Z|BZ2|BIN|ZIP|GZIP|LZH,10=117:application/download|application/force-download|application/x-msdownload|application/octet-stream|binary/octet-stream,20=80:TXT|HTM|HTML|PHP|ASP|ASPX|CGI|JSP|JSPX|CSS|JS|APNG|GIF|ICO|JPEG|JPG|PNG|SVG|WEBP,21=16:/*-internal-*/**;"
                );
                continue;
            }
            if (hdr.type == 5 || hdr.type == 14) {
                std::vector<std::string> pos = opendm::parse_tonec_pos_args(raw);
                uint32_t status_code = (pos.size() > 3) ? static_cast<uint32_t>(std::strtoul(pos[3].c_str(), nullptr, 10)) : 0;
                uint32_t action_code = (pos.size() > 5) ? static_cast<uint32_t>(std::strtoul(pos[5].c_str(), nullptr, 10)) : 0;
                if (action_code == 6 || action_code == 5 || action_code == 4 || (status_code >= 300 && status_code < 400)) {
                    opendm::write_native_json_string("MSG#" + std::to_string(hdr.id) + "#1#2#0:" + std::to_string(hdr.id) + ":0;");
                    continue;
                }
                std::string resp = opendm::send_command_spawning_if_needed(raw);
                std::string status;
                try {
                    auto jresp = opendm::json::parse(resp);
                    status = jresp.value("status", "");
                } catch (...) {}
                if (status == "prompting" || status == "downloading" || status == "started" || status == "ok") {
                    opendm::write_native_json_string("MSG#" + std::to_string(hdr.id) + "#1#2#0:" + std::to_string(hdr.id) + ":3;");
                } else {
                    opendm::write_native_json_string("MSG#" + std::to_string(hdr.id) + "#1#2#0:" + std::to_string(hdr.id) + ":0;");
                }
                continue;
            }
            opendm::write_native_json_string("MSG#" + std::to_string(hdr.id) + "#1#2#0:" + std::to_string(hdr.id) + ";");
            continue;
        }

        // Standard OpenDM JSON Protocol
        opendm::json jreq;
        try {
            jreq = opendm::json::parse(raw);
        } catch (...) {
            opendm::write_native_raw_json(opendm::json({
                {"status", "error"},
                {"message", "Invalid JSON request"}
            }).dump());
            continue;
        }

        if (!jreq.is_object()) {
            opendm::write_native_raw_json(opendm::json({
                {"status", "error"},
                {"message", "JSON request must be an object"}
            }).dump());
            continue;
        }

        const std::string action = jreq.value("action", "");

        if (action == "host_ping") {
            const bool running = opendm::ping_running_app(false);
            opendm::write_native_raw_json(opendm::json({
                {"status", "ok"},
                {"host_version", "1.0.0"},
                {"app_running", running}
            }).dump());
            continue;
        }

        if (action == "launch_app") {
            const bool launched = opendm::ensure_opendm_running();
            if (launched) {
                opendm::IpcServer::send_command("{\"action\":\"present\"}", opendm::IpcServer::DEFAULT_SOCKET_PATH, 3);
            }
            opendm::write_host_log("[NATIVE_HOST] launch_app app_running=" + std::string(launched ? "true" : "false"));
            opendm::write_native_raw_json(opendm::json({
                {"status", launched ? "ok" : "error"},
                {"app_running", launched},
                {"message", launched ? "OpenDM is running" : "Could not launch OpenDM"}
            }).dump());
            continue;
        }

        if (action == "extract_youtube") {
            std::string url = jreq.value("url", "");
            std::string vid = jreq.value("videoId", "");
            // Try running daemon first without spawning GUI, else run in-process extractor
            std::string response = opendm::IpcServer::send_command(raw, opendm::IpcServer::DEFAULT_SOCKET_PATH, 1);
            if (opendm::is_ipc_connect_error(response)) {
                response = opendm::IpcServer::extract_youtube_json(url, vid);
            }
            opendm::write_native_raw_json(response);
            continue;
        }

        // Downloads, intercept_download, download_batch, ping, status, queue controls
        std::string response = opendm::send_command_spawning_if_needed(raw);
        if (opendm::is_ipc_connect_error(response)) {
            response = opendm::json({
                {"status", "error"},
                {"message", "Could not connect to or start OpenDM"}
            }).dump();
        }
        try {
            auto jresp = opendm::json::parse(response);
            opendm::write_host_log("[NATIVE_HOST] action=" + action + " resp_status=" + jresp.value("status", "unknown"));
        } catch (...) {}
        opendm::write_native_raw_json(response);
    }

    opendm::write_host_log("[NATIVE_HOST] Exiting.");
    curl_global_cleanup();
    return 0;
}
