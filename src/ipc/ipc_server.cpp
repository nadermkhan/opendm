#include "ipc_server.hpp"
#include "../core/download_queue.hpp"
#include "../core/media_muxer.hpp"
#include "../core/site_logon.hpp"
#include "../core/youtube_engine.hpp"
#include "../core/win32_unicode.hpp"

#ifdef _WIN32
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include <io.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>
#include <cstring>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <unordered_set>
#include <iomanip>

namespace opendm {

using json = nlohmann::json;

namespace {

std::string json_get_str(const json& obj, const char* key, const std::string& def = "") {
    if (!obj.is_object()) return def;
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return def;
    if (it->is_string()) return it->get<std::string>();
    if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
    if (it->is_number_integer()) return std::to_string(it->get<int64_t>());
    if (it->is_number_unsigned()) return std::to_string(it->get<uint64_t>());
    return def;
}

std::string get_default_downloads_folder() {
    std::string configured = QueueManager::instance().get_default_save_dir();
    if (!configured.empty()) return configured;
    return sys::get_downloads_dir_utf8();
}

#ifdef _WIN32
constexpr DWORD PIPE_IO_BUF_SIZE = 65536;
constexpr size_t MAX_IPC_MSG_BYTES = 16 * 1024 * 1024;

bool overlapped_io_exact(HANDLE hPipe, OVERLAPPED& ov, HANDLE stop_event, bool is_read,
                         void* buf, DWORD len, DWORD* out_transferred, DWORD* out_err) {
    DWORD transferred = 0;
    BOOL ok = is_read
        ? ReadFile(hPipe, buf, len, &transferred, &ov)
        : WriteFile(hPipe, buf, len, &transferred, &ov);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    if (ok) {
        if (out_transferred) *out_transferred = transferred;
        if (out_err) *out_err = ERROR_SUCCESS;
        return true;
    }
    if (err != ERROR_IO_PENDING) {
        if (out_transferred) *out_transferred = 0;
        if (out_err) *out_err = err;
        return err == ERROR_MORE_DATA;
    }

    HANDLE waits[2] = {stop_event, ov.hEvent};
    DWORD wait_res = WaitForMultipleObjects(2, waits, FALSE, 30000);
    if (wait_res != WAIT_OBJECT_0 + 1) {
        CancelIoEx(hPipe, &ov);
        DWORD dummy = 0;
        GetOverlappedResult(hPipe, &ov, &dummy, TRUE);
        if (out_transferred) *out_transferred = 0;
        if (out_err) *out_err = (wait_res == WAIT_OBJECT_0) ? ERROR_OPERATION_ABORTED : ERROR_TIMEOUT;
        return false;
    }

    if (!GetOverlappedResult(hPipe, &ov, &transferred, FALSE)) {
        err = GetLastError();
        if (out_transferred) *out_transferred = transferred;
        if (out_err) *out_err = err;
        return err == ERROR_MORE_DATA;
    }
    if (out_transferred) *out_transferred = transferred;
    if (out_err) *out_err = ERROR_SUCCESS;
    return true;
}

std::string read_pipe_message_overlapped(HANDLE hPipe, HANDLE stop_event) {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return "";
    std::string request;
    std::vector<char> chunk(PIPE_IO_BUF_SIZE);
    while (request.size() < MAX_IPC_MSG_BYTES) {
        ResetEvent(ev);
        OVERLAPPED ov{};
        ov.hEvent = ev;
        DWORD n = 0;
        DWORD err = ERROR_SUCCESS;
        overlapped_io_exact(hPipe, ov, stop_event, true, chunk.data(), PIPE_IO_BUF_SIZE, &n, &err);
        if (n > 0) request.append(chunk.data(), n);
        if (err == ERROR_SUCCESS) break;
        if (err == ERROR_MORE_DATA) continue;
        request.clear();
        break;
    }
    CloseHandle(ev);
    return request;
}

void write_pipe_message_overlapped(HANDLE hPipe, HANDLE stop_event, const std::string& payload) {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return;
    ResetEvent(ev);
    OVERLAPPED ov{};
    ov.hEvent = ev;
    DWORD written = 0;
    DWORD err = ERROR_SUCCESS;
    overlapped_io_exact(hPipe, ov, stop_event, false,
                        const_cast<char*>(payload.data()),
                        static_cast<DWORD>(payload.size()), &written, &err);
    FlushFileBuffers(hPipe);
    CloseHandle(ev);
}
#else
bool is_complete_ipc_frame(const std::string& buf) {
    size_t i = 0;
    while (i < buf.size() && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) {
        ++i;
    }
    if (i >= buf.size()) return false;

    // Tonec protocol: header + comma-separated id=len:payload fields ending in ';'
    if (buf.compare(i, 4, "MSG#") == 0 || buf.compare(i, 5, "\"MSG#") == 0) {
        size_t pos = buf.find(':', i);
        if (pos == std::string::npos) {
            return buf.back() == '\n' && buf.find(';') != std::string::npos;
        }
        // Check whether ends with ';' (or ';"\n') after trimming trailing whitespace/newline
        size_t end = buf.size();
        while (end > i && (buf[end - 1] == '\n' || buf[end - 1] == '\r' || buf[end - 1] == ' ' || buf[end - 1] == '"')) {
            --end;
        }
        if (end <= i || buf[end - 1] != ';') return false;
        // Verify length-prefixed Tonec fields so an internal ';' before a field payload isn't misread
        size_t cur = i;
        while (cur < end) {
            size_t eq = buf.find('=', cur);
            if (eq == std::string::npos || eq >= end) break;
            size_t colon = buf.find(':', eq + 1);
            if (colon == std::string::npos || colon >= end) break;
            size_t flen = 0;
            try {
                flen = std::stoul(buf.substr(eq + 1, colon - eq - 1));
            } catch (...) {
                cur = colon + 1;
                continue;
            }
            if (colon + 1 + flen > end) {
                return false; // Incomplete length-prefixed field!
            }
            cur = colon + 1 + flen;
        }
        return true;
    }

    // JSON object frame: check balanced braces ignoring string literals
    if (buf[i] == '{') {
        int depth = 0;
        bool in_str = false;
        bool escaped = false;
        for (size_t k = i; k < buf.size(); ++k) {
            char c = buf[k];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (in_str) {
                if (c == '\\') escaped = true;
                else if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') in_str = true;
            else if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0) return true;
            }
        }
        return false;
    }

    return buf.find('\n') != std::string::npos;
}
#endif

} // namespace

std::string extract_tonec_field(const std::string& msg, int field_id) {
    std::string tag = std::to_string(field_id) + "=";
    size_t pos = 0;
    while ((pos = msg.find(tag, pos)) != std::string::npos) {
        if (pos == 0 || msg[pos - 1] == ',' || msg[pos - 1] == ';' || msg[pos - 1] == ':') {
            size_t colon = msg.find(':', pos + tag.length());
            if (colon != std::string::npos) {
                std::string len_str = msg.substr(pos + tag.length(), colon - pos - tag.length());
                size_t len = 0;
                try {
                    len = std::stoul(len_str);
                } catch (...) {
                    return "";
                }
                if (colon + 1 + len <= msg.length()) {
                    return msg.substr(colon + 1, len);
                }
            }
        }
        pos += tag.length();
    }
    return "";
}

MsgHeader parse_msg_header(const std::string& msg) {
    MsgHeader hdr;
    if (msg.rfind("MSG#", 0) != 0) return hdr;

    size_t p1 = 4;
    size_t p2 = msg.find('#', p1);
    if (p2 == std::string::npos) return hdr;
    size_t p3 = msg.find('#', p2 + 1);
    if (p3 == std::string::npos) return hdr;
    size_t p4 = msg.find('#', p3 + 1);
    if (p4 == std::string::npos) return hdr;
    size_t p5 = msg.find_first_of(":,;", p4 + 1);
    if (p5 == std::string::npos) p5 = msg.length();

    try {
        hdr.id = std::stoul(msg.substr(p1, p2 - p1));
        hdr.type = std::stoul(msg.substr(p2 + 1, p3 - p2 - 1));
        hdr.subtype = std::stoul(msg.substr(p3 + 1, p4 - p3 - 1));
        hdr.flags = std::stoul(msg.substr(p4 + 1, p5 - p4 - 1));
        hdr.valid = true;
    } catch (...) {
        return hdr;
    }
    return hdr;
}

std::vector<std::string> parse_tonec_pos_args(const std::string& msg) {
    std::vector<std::string> args;
    if (msg.rfind("MSG#", 0) != 0) return args;

    // Skip MSG#id#type#subtype#flags
    size_t p = 4;
    for (int i = 0; i < 3; ++i) {
        size_t next = msg.find('#', p);
        if (next == std::string::npos) return args;
        p = next + 1;
    }
    size_t colon = msg.find(':', p);
    if (colon == std::string::npos) return args;
    size_t end_args = msg.find_first_of(",;", colon + 1);
    if (end_args == std::string::npos) end_args = msg.length();

    std::string sub = msg.substr(colon + 1, end_args - (colon + 1));
    size_t start = 0;
    while (start <= sub.length()) {
        size_t next_colon = sub.find(':', start);
        if (next_colon == std::string::npos) {
            args.push_back(sub.substr(start));
            break;
        }
        args.push_back(sub.substr(start, next_colon - start));
        start = next_colon + 1;
    }
    return args;
}

bool is_downloadable_extension(const std::string& ext) {
    if (ext.empty()) return false;
    std::string upper_ext = ext;
    if (upper_ext.front() == '.') upper_ext.erase(0, 1);
    for (char& c : upper_ext) c = static_cast<char>(std::toupper(c));

    static const std::unordered_set<std::string> downloadable_exts = {
        "3GP", "7Z", "AAC", "ACE", "AIF", "APK", "ARJ", "ASF", "AVI", "BIN", "BZ2",
        "DEB", "DMG", "DOC", "DOCX", "EPUB", "EXE", "FLAC", "FLV", "GZ", "GZIP",
        "IMG", "ISO", "LZH", "M4A", "M4V", "MKV", "MOV", "MP3", "MP4", "MPA",
        "MPE", "MPEG", "MPG", "MSI", "MSU", "OGG", "OGV", "PDF", "PKG", "PLJ",
        "PPS", "PPT", "PPTX", "QT", "RA", "RAR", "RM", "RMVB", "RPM", "SEA",
        "SIT", "SITX", "TAR", "TORRENT", "TS", "VTT", "WAV", "WEBM", "WMA", "WMV",
        "XLS", "XLSX", "Z", "ZIP"
    };

    if (downloadable_exts.count(upper_ext) > 0) return true;
    if (upper_ext.length() == 3 && (upper_ext[0] == 'R' || upper_ext[0] == 'A') &&
        std::isdigit(upper_ext[1]) && std::isdigit(upper_ext[2])) {
        return true;
    }
    return false;
}

bool is_forbidden_web_extension(const std::string& ext) {
    if (ext.empty()) return false;
    std::string upper_ext = ext;
    if (upper_ext.front() == '.') upper_ext.erase(0, 1);
    for (char& c : upper_ext) c = static_cast<char>(std::toupper(c));

    static const std::unordered_set<std::string> forbidden_web_exts = {
        // Web pages, scripts, styles
        "HTM", "HTML", "PHP", "ASP", "ASPX", "CGI", "JSP", "JSPX", "JS", "CSS", "MJS", "JSON",
        // Images & graphics (never download automatically during browsing)
        "PNG", "JPG", "JPEG", "GIF", "WEBP", "SVG", "ICO", "BMP", "APNG", "AVIF", "CUR",
        // Web fonts
        "WOFF", "WOFF2", "TTF", "EOT", "OTF"
    };

    return forbidden_web_exts.count(upper_ext) > 0;
}

bool is_web_content_type(const std::string& content_type) {
    if (content_type.empty()) return false;
    std::string ct = content_type;
    for (char& c : ct) c = static_cast<char>(std::tolower(c));
    size_t sc = ct.find(';');
    if (sc != std::string::npos) ct = ct.substr(0, sc);
    while (!ct.empty() && (ct.back() == ' ' || ct.back() == '\t')) ct.pop_back();

    if (ct == "text/html" || ct == "application/xhtml+xml" ||
        ct == "text/css" ||
        ct == "text/javascript" || ct == "application/javascript" || ct == "application/x-javascript" ||
        ct == "application/json" || ct == "text/xml" || ct == "application/xml" ||
        ct.rfind("image/", 0) == 0 ||
        ct.rfind("font/", 0) == 0) {
        return true;
    }
    return false;
}

std::string normalize_stream_scheme(const std::string& raw_url) {
    std::string url = raw_url;
    while (!url.empty() && (url.front() == ' ' || url.front() == '\t' || url.front() == '\r' || url.front() == '\n' || url.front() == '\"')) {
        url.erase(0, 1);
    }
    while (!url.empty() && (url.back() == ' ' || url.back() == '\t' || url.back() == '\r' || url.back() == '\n' || url.back() == '\"' || url.back() == ';')) {
        url.pop_back();
    }

    if (url.rfind("idmfchttps://", 0) == 0) {
        url = "https://" + url.substr(13);
    } else if (url.rfind("idmfchttp://", 0) == 0) {
        url = "http://" + url.substr(12);
    } else if (url.rfind("idmfcftp://", 0) == 0) {
        url = "ftp://" + url.substr(11);
    } else if (url.rfind("idmdwnlmfv9://", 0) == 0) {
        url = url.substr(14);
    } else if (url.rfind("idmreg://", 0) == 0) {
        url = url.substr(9);
    } else if (url.rfind("mpdatoidm:///", 0) == 0) {
        url = url.substr(13);
    } else if (url.rfind("mpdatoidm://", 0) == 0) {
        url = url.substr(12);
    } else if (url.rfind("mpdtoidm:///", 0) == 0) {
        url = url.substr(12);
    } else if (url.rfind("mpdtoidm://", 0) == 0) {
        url = url.substr(11);
    } else if (url.rfind("yta2toidm://", 0) == 0) {
        url = url.substr(12);
    } else if (url.rfind("yta3toidm://", 0) == 0) {
        url = url.substr(12);
    } else if (url.rfind("yta5toidm://", 0) == 0) {
        url = url.substr(12);
    } else if (url.rfind("ytatoidm://", 0) == 0) {
        url = url.substr(11);
    } else if (url.rfind("hls2toidmtomp4://", 0) == 0) {
        url = url.substr(17);
    } else if (url.rfind("hls2toidm://", 0) == 0) {
        url = url.substr(12);
    } else if (url.rfind("idmhlstomp4v2://", 0) == 0) {
        url = url.substr(16);
    }

    if (url.rfind("//", 0) == 0) {
        url = "https:" + url;
    }
    return url;
}

std::pair<std::string, std::string> split_dual_stream_url(const std::string& raw_url) {
    std::string s = normalize_stream_scheme(raw_url);
    size_t hash_pos = s.find("##");
    if (hash_pos == std::string::npos) {
        return {s, ""};
    }
    std::string video_url = normalize_stream_scheme(s.substr(0, hash_pos));
    std::string audio_url = normalize_stream_scheme(s.substr(hash_pos + 2));
    if (audio_url.find("failure.mp3") != std::string::npos || audio_url.find("/s/search/audio/") != std::string::npos) {
        audio_url.clear();
    }
    return {video_url, audio_url};
}

std::string clean_stream_query_parameters(const std::string& url) {
    size_t qpos = url.find('?');
    if (qpos == std::string::npos) {
        return url;
    }
    std::string base = url.substr(0, qpos);
    std::string query = url.substr(qpos + 1);

    bool is_googlevideo = (url.find("googlevideo.com") != std::string::npos ||
                           url.find("videoplayback") != std::string::npos ||
                           url.find("youtube.com") != std::string::npos);
    bool is_mycdn = (url.find("mycdn.me") != std::string::npos);

    if (!is_googlevideo && !is_mycdn) {
        return url;
    }

    std::unordered_set<std::string> signed_params;
    size_t sp_pos = query.find("sparams=");
    if (sp_pos != std::string::npos) {
        size_t sp_end = query.find('&', sp_pos);
        std::string sp_val = (sp_end == std::string::npos) ? query.substr(sp_pos + 8) : query.substr(sp_pos + 8, sp_end - (sp_pos + 8));
        std::string decoded_sp;
        for (size_t i = 0; i < sp_val.length(); ++i) {
            if (sp_val[i] == '%' && i + 2 < sp_val.length()) {
                std::string hex = sp_val.substr(i + 1, 2);
                try {
                    decoded_sp += static_cast<char>(std::stoul(hex, nullptr, 16));
                    i += 2;
                    continue;
                } catch (...) {}
            }
            decoded_sp += sp_val[i];
        }
        std::istringstream sp_ss(decoded_sp);
        std::string p;
        while (std::getline(sp_ss, p, ',')) {
            while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(0, 1);
            while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
            if (!p.empty()) signed_params.insert(p);
        }
    }

    std::vector<std::string> kept_params;
    std::istringstream iss(query);
    std::string token;
    while (std::getline(iss, token, '&')) {
        if (token.empty()) continue;
        size_t eq = token.find('=');
        std::string key = (eq == std::string::npos) ? token : token.substr(0, eq);
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(0, 1);

        // Never strip any parameter that is cryptographically signed inside sparams
        if (signed_params.count(key) > 0) {
            kept_params.push_back(token);
            continue;
        }

        if (is_googlevideo) {
            if (key == "range" || key == "begin" || key == "len" || key == "ump" ||
                key == "sq" || key == "otf" || key == "rn" || key == "rbuf" || key == "bytes") {
                continue;
            }
        }
        if (is_mycdn) {
            if (key == "bytes") {
                continue;
            }
        }
        kept_params.push_back(token);
    }

    if (kept_params.empty()) {
        return base;
    }

    std::string result = base + "?";
    for (size_t i = 0; i < kept_params.size(); ++i) {
        if (i > 0) result += "&";
        result += kept_params[i];
    }
    return result;
}

std::string sanitize_media_filename(const std::string& title, const std::string& default_ext) {
    if (title.empty()) {
        return "video_download." + default_ext;
    }
    std::string clean;
    clean.reserve(title.length());
    for (unsigned char c : title) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '\"' || c == '<' || c == '>' || c == '|') {
            clean += '_';
        } else if (c >= 32) {
            clean += static_cast<char>(c);
        }
    }
    while (!clean.empty() && (clean.front() == ' ' || clean.front() == '.')) clean.erase(0, 1);
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
    if (clean.empty()) {
        return "video_download." + default_ext;
    }
    if (sys::is_reserved_dos_name(clean)) {
        clean = "_" + clean;
    }
    if (clean.find('.') == std::string::npos) {
        clean += "." + default_ext;
    }
    return clean;
}

IpcServer::IpcServer(const std::string& socket_path) : socket_path_(socket_path) {
#ifdef _WIN32
    if (socket_path_.rfind("\\\\.\\pipe\\", 0) != 0) {
        std::string stem = std::filesystem::path(socket_path_).stem().string();
        if (stem.empty()) stem = "opendm_pipe";
        socket_path_ = "\\\\.\\pipe\\" + stem;
    }
#endif
}

IpcServer::~IpcServer() {
    stop();
}

bool IpcServer::start() {
    if (running_) return true;

#ifdef _WIN32
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    running_ = true;
    server_thread_ = std::thread(&IpcServer::server_loop, this);
    return true;
#else
    // Unlink old socket file if it exists
    unlink(socket_path_.c_str());

    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        perror("IpcServer: socket creation failed");
        return false;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("IpcServer: bind failed");
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 10) < 0) {
        perror("IpcServer: listen failed");
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    running_ = true;
    server_thread_ = std::thread(&IpcServer::server_loop, this);
    return true;
#endif
}

void IpcServer::stop() {
    if (!running_) return;

    running_ = false;
#ifdef _WIN32
    if (stop_event_) {
        SetEvent(stop_event_);
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    {
        std::unique_lock<std::mutex> lock(workers_mutex_);
        workers_cv_.wait(lock, [this]() { return active_workers_ == 0; });
    }
    if (stop_event_) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
#else
    if (server_fd_ >= 0) {
        // Shutdown server socket to unblock accept()
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    unlink(socket_path_.c_str());
#endif
}

bool IpcServer::is_running() const {
    return running_;
}

void IpcServer::server_loop() {
#ifdef _WIN32
    std::wstring wpipe = sys::utf8_to_wide(socket_path_);
    constexpr size_t PENDING_LISTENERS = 2;
    struct PendingPipe {
        HANDLE hPipe{INVALID_HANDLE_VALUE};
        HANDLE hEvent{nullptr};
        OVERLAPPED ov{};
        bool connecting{false};
    };

    auto create_instance = [&](PendingPipe& p) -> bool {
        p.hPipe = CreateNamedPipeW(
            wpipe.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            PIPE_IO_BUF_SIZE, PIPE_IO_BUF_SIZE, 1000, nullptr
        );
        if (p.hPipe == INVALID_HANDLE_VALUE) return false;
        if (!p.hEvent) p.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!p.hEvent) {
            CloseHandle(p.hPipe);
            p.hPipe = INVALID_HANDLE_VALUE;
            return false;
        }
        ResetEvent(p.hEvent);
        p.ov = OVERLAPPED{};
        p.ov.hEvent = p.hEvent;
        p.connecting = false;

        BOOL ok = ConnectNamedPipe(p.hPipe, &p.ov);
        DWORD err = GetLastError();
        if (!ok) {
            if (err == ERROR_IO_PENDING) {
                p.connecting = true;
            } else if (err == ERROR_PIPE_CONNECTED) {
                SetEvent(p.hEvent);
            } else {
                CloseHandle(p.hPipe);
                p.hPipe = INVALID_HANDLE_VALUE;
                return false;
            }
        } else {
            SetEvent(p.hEvent);
        }
        return true;
    };

    std::vector<PendingPipe> slots(PENDING_LISTENERS);
    for (auto& s : slots) {
        create_instance(s);
    }

    while (running_) {
        std::vector<HANDLE> wait_handles;
        std::vector<size_t> slot_indices;
        wait_handles.push_back(stop_event_);
        for (size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].hPipe != INVALID_HANDLE_VALUE && slots[i].hEvent) {
                wait_handles.push_back(slots[i].hEvent);
                slot_indices.push_back(i);
            }
        }

        DWORD wr = WaitForMultipleObjects(static_cast<DWORD>(wait_handles.size()),
                                         wait_handles.data(), FALSE, 200);
        if (!running_ || wr == WAIT_OBJECT_0) {
            break;
        }
        if (wr == WAIT_TIMEOUT) {
            for (auto& s : slots) {
                if (s.hPipe == INVALID_HANDLE_VALUE && running_) {
                    create_instance(s);
                }
            }
            continue;
        }
        if (wr > WAIT_OBJECT_0 && wr < WAIT_OBJECT_0 + wait_handles.size()) {
            size_t idx = slot_indices[wr - (WAIT_OBJECT_0 + 1)];
            PendingPipe& ready = slots[idx];
            if (ready.connecting) {
                DWORD dummy = 0;
                if (!GetOverlappedResult(ready.hPipe, &ready.ov, &dummy, FALSE)) {
                    CloseHandle(ready.hPipe);
                    ready.hPipe = INVALID_HANDLE_VALUE;
                    if (running_) create_instance(ready);
                    continue;
                }
            }

            HANDLE client_pipe = ready.hPipe;
            ready.hPipe = INVALID_HANDLE_VALUE;
            if (running_) {
                create_instance(ready);
            }

            {
                std::lock_guard<std::mutex> lock(workers_mutex_);
                ++active_workers_;
            }
            std::thread([this, client_pipe]() {
                std::string request = read_pipe_message_overlapped(client_pipe, stop_event_);
                if (!request.empty() && running_) {
                    std::string response = handle_request(request) + "\n";
                    write_pipe_message_overlapped(client_pipe, stop_event_, response);
                }
                DisconnectNamedPipe(client_pipe);
                CloseHandle(client_pipe);
                {
                    std::lock_guard<std::mutex> lock(workers_mutex_);
                    --active_workers_;
                }
                workers_cv_.notify_all();
            }).detach();
        }
    }

    for (auto& s : slots) {
        if (s.hPipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(s.hPipe, &s.ov);
            CloseHandle(s.hPipe);
            s.hPipe = INVALID_HANDLE_VALUE;
        }
        if (s.hEvent) {
            CloseHandle(s.hEvent);
            s.hEvent = nullptr;
        }
    }
#else
    while (running_) {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (!running_) break;
            continue;
        }

        // Read complete request frame from client (multi-line Tonec headers or balanced JSON)
        std::string request;
        char buffer[8192];
        ssize_t bytes_read = 0;

        while ((bytes_read = read(client_fd, buffer, sizeof(buffer))) > 0) {
            request.append(buffer, static_cast<size_t>(bytes_read));
            if (is_complete_ipc_frame(request)) {
                break;
            }
        }

        if (!request.empty()) {
            std::string response = handle_request(request) + "\n";
            size_t total_written = 0;
            while (total_written < response.size()) {
                ssize_t w = write(client_fd, response.data() + total_written, response.size() - total_written);
                if (w <= 0) break;
                total_written += static_cast<size_t>(w);
            }
        }
        close(client_fd);
    }
#endif
}

void IpcServer::set_gui_mode(bool gui_mode) {
    gui_mode_ = gui_mode;
}

void IpcServer::set_download_prompt_callback(DownloadPromptCallback cb) {
    std::lock_guard<std::mutex> lock(prompt_mutex_);
    prompt_callback_ = cb;
    if (prompt_callback_) {
        for (const auto& p : pending_prompts_) {
            prompt_callback_(p);
        }
        pending_prompts_.clear();
    }
}

static std::string format_file_size(uint64_t bytes) {
    if (bytes == 0) return "";
    constexpr double KB = 1024.0;
    constexpr double MB = 1024.0 * KB;
    constexpr double GB = 1024.0 * MB;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    if (bytes >= GB) {
        ss << (bytes / GB) << " GB";
    } else if (bytes >= MB) {
        ss << (bytes / MB) << " MB";
    } else if (bytes >= KB) {
        ss << (bytes / KB) << " KB";
    } else {
        ss << bytes << " Bytes";
    }
    return ss.str();
}

std::string build_youtube_extract_json(const YouTubeVideoInfo& info) {
    json root;
    root["status"] = "ok";
    root["video_id"] = info.video_id;
    root["title"] = info.title;
    root["author"] = info.author;
    root["duration"] = info.duration_seconds;
    root["duration_str"] = YouTubeEngine::format_duration(info.duration_seconds);
    root["client_used"] = info.client_used;

    json options = json::array();

    // 1. Adaptive formats paired with best audio
    if (!info.video_formats.empty() && !info.audio_formats.empty()) {
        const auto& best_audio = info.best_audio;
        for (const auto& vf : info.video_formats) {
            uint64_t total = vf.content_length + (best_audio.content_length > 0 ? best_audio.content_length : 0);
            std::string size_str = format_file_size(total);

            std::string label = vf.quality_label;
            if (vf.fps > 30) {
                label += " (" + std::to_string(vf.fps) + "fps, " + vf.container + ")";
            } else {
                label += " (" + vf.container + ")";
            }
            label += " [Video + " + YouTubeEngine::format_bitrate(best_audio.bitrate) + " Audio]";
            if (!size_str.empty()) {
                label += " - " + size_str;
            }

            std::string dual_url = "yta2toidm://" + vf.url + "##" + best_audio.url;
            std::string ua = vf.user_agent.empty() ? best_audio.user_agent : vf.user_agent;

            options.push_back({
                {"label", label},
                {"quality", vf.quality_label},
                {"fps", vf.fps},
                {"container", vf.container},
                {"itag", vf.itag},
                {"size", total},
                {"size_str", size_str},
                {"url", dual_url},
                {"user_agent", ua},
                {"is_audio", false},
                {"is_muxed", true}
            });
        }
    }

    // 2. Progressive formats
    for (const auto& pf : info.progressive_formats) {
        std::string size_str = format_file_size(pf.content_length);
        std::string label = pf.quality_label + " (" + pf.container + " - Progressive)";
        if (!size_str.empty()) {
            label += " - " + size_str;
        }

        options.push_back({
            {"label", label},
            {"quality", pf.quality_label},
            {"fps", pf.fps},
            {"container", pf.container},
            {"itag", pf.itag},
            {"size", pf.content_length},
            {"size_str", size_str},
            {"url", pf.url},
            {"user_agent", pf.user_agent},
            {"is_audio", false},
            {"is_muxed", false}
        });
    }

    // 3. Audio only formats
    for (const auto& af : info.audio_formats) {
        std::string size_str = format_file_size(af.content_length);
        std::string label = "Audio Only - " + af.container + " (" + YouTubeEngine::format_bitrate(af.bitrate) + ")";
        if (!size_str.empty()) {
            label += " - " + size_str;
        }

        options.push_back({
            {"label", label},
            {"quality", YouTubeEngine::format_bitrate(af.bitrate)},
            {"fps", 0},
            {"container", af.container},
            {"itag", af.itag},
            {"size", af.content_length},
            {"size_str", size_str},
            {"url", af.url},
            {"user_agent", af.user_agent},
            {"is_audio", true},
            {"is_muxed", false}
        });
    }

    root["options"] = std::move(options);
    return root.dump(2);
}

std::string IpcServer::handle_request(const std::string& request) {
    std::string req = request;
    while (!req.empty() && (req.front() == '\"' || req.front() == ' ' || req.front() == '\t' || req.front() == '\r' || req.front() == '\n')) {
        req.erase(0, 1);
    }
    while (!req.empty() && (req.back() == '\"' || req.back() == ' ' || req.back() == '\t' || req.back() == '\r' || req.back() == '\n')) {
        req.pop_back();
    }

    // Check if message is Tonec wire protocol format (from Chrome/Firefox IDM extension)
    if (req.rfind("MSG#", 0) == 0 || req.find("6=") != std::string::npos) {
        MsgHeader hdr = parse_msg_header(req);
        bool is_explicit_user_action = (hdr.valid && hdr.type == 14) || (req.find("#14#") != std::string::npos);

        std::string raw_field6 = extract_tonec_field(req, 6);
        if (!raw_field6.empty()) {
            auto [video_url, audio_url] = split_dual_stream_url(raw_field6);
            video_url = clean_stream_query_parameters(video_url);
            if (!audio_url.empty()) {
                audio_url = clean_stream_query_parameters(audio_url);
            }
            if ((video_url.find("googlevideo.com") != std::string::npos || video_url.find("videoplayback") != std::string::npos) &&
                (video_url.find("sabr=1") != std::string::npos || video_url.find("sabr=2") != std::string::npos)) {
                std::cout << "OpenDM IPC: Ignored YouTube SABR video stream: " << video_url << std::endl;
                return "{\"status\": \"ignored\", \"reason\": \"youtube_sabr_stream_not_downloadable\"}";
            }
            if (!audio_url.empty() && (audio_url.find("googlevideo.com") != std::string::npos || audio_url.find("videoplayback") != std::string::npos) &&
                (audio_url.find("sabr=1") != std::string::npos || audio_url.find("sabr=2") != std::string::npos)) {
                std::cout << "OpenDM IPC: Ignored YouTube SABR audio stream: " << audio_url << std::endl;
                return "{\"status\": \"ignored\", \"reason\": \"youtube_sabr_stream_not_downloadable\"}";
            }
            std::string url = video_url;
            std::string filename = extract_tonec_field(req, 100);
            if (filename.empty()) {
                filename = extract_tonec_field(req, 4);
            }
            std::string referer = extract_tonec_field(req, 50);
            std::string page_url = extract_tonec_field(req, 7);
            std::string media_title = extract_tonec_field(req, 102);

            bool is_mobile_yt = (url.find("c=ANDROID_VR") != std::string::npos ||
                                 url.find("c=VISIONOS") != std::string::npos ||
                                 url.find("c=ANDROID_TESTSUITE") != std::string::npos ||
                                 url.find("c=IOS") != std::string::npos ||
                                 url.find("c=ANDROID") != std::string::npos);
            if (is_mobile_yt) {
                referer.clear();
            } else if (referer.empty() && page_url.rfind("http", 0) == 0) {
                referer = page_url;
            }
            std::string page_title = !media_title.empty() ? media_title : (page_url.rfind("http", 0) == 0 ? "" : page_url);
            std::string cookies = extract_tonec_field(req, 51);
            std::string user_agent = extract_tonec_field(req, 54);

            std::string req_headers = extract_tonec_field(req, 11);
            if (!req_headers.empty()) {
                if (user_agent.empty()) {
                    size_t ua = req_headers.find("User-Agent: ");
                    if (ua != std::string::npos) {
                        size_t end = req_headers.find("\r\n", ua);
                        user_agent = req_headers.substr(ua + 12, end - (ua + 12));
                    }
                }
                if (referer.empty()) {
                    size_t ref = req_headers.find("Referer: ");
                    if (ref != std::string::npos) {
                        size_t end = req_headers.find("\r\n", ref);
                        referer = req_headers.substr(ref + 9, end - (ref + 9));
                    }
                }
                if (cookies.empty()) {
                    size_t ck = req_headers.find("Cookie: ");
                    if (ck != std::string::npos) {
                        size_t end = req_headers.find("\r\n", ck);
                        cookies = req_headers.substr(ck + 8, end - (ck + 8));
                    }
                }
            }

            // Extract Content-Length & Content-Disposition filename from response headers (field 13)
            uint64_t file_size = 0;
            std::string resp_headers = extract_tonec_field(req, 13);
            if (!resp_headers.empty()) {
                std::string lower_h = sys::ascii_tolower(resp_headers);
                size_t cl_pos = lower_h.find("content-length:");
                if (cl_pos != std::string::npos) {
                    size_t val_start = cl_pos + 15;
                    while (val_start < resp_headers.size() && (resp_headers[val_start] == ' ' || resp_headers[val_start] == '\t')) val_start++;
                    size_t val_end = resp_headers.find_first_of("\r\n;", val_start);
                    try {
                        file_size = std::stoull(resp_headers.substr(val_start, val_end - val_start));
                    } catch (...) {}
                }

                if (filename.empty() || filename == "ZIP") {
                    std::string cd_fn = DownloadSession::extract_filename_from_content_disposition(resp_headers);
                    if (!cd_fn.empty()) {
                        filename = cd_fn;
                    }
                }
            }

            if (filename.empty() || filename == "ZIP" || filename == "videoplayback" || filename == "videoplayback.mp4" || filename == "download.bin") {
                std::string url_fn = DownloadSession::extract_filename_from_url(url);
                bool is_generic_stream = (url_fn.empty() || url_fn == "download.bin" ||
                                          url_fn.rfind("videoplayback", 0) == 0 ||
                                          url_fn == "master.m3u8" || url_fn == "index.m3u8");
                if (!is_generic_stream) {
                    filename = url_fn;
                } else if (!page_title.empty()) {
                    filename = sanitize_media_filename(page_title, "mp4");
                } else {
                    filename = url_fn.empty() ? "download.bin" : url_fn;
                }
            } else {
                filename = DownloadSession::sanitize_filename(DownloadSession::url_decode(filename));
            }

            // Clean filename from any directory separators and trailing delimiters
            size_t slash = filename.find_last_of("/\\");
            if (slash != std::string::npos) {
                filename = filename.substr(slash + 1);
            }
            while (!filename.empty() && (filename.back() == ',' || filename.back() == ';' ||
                                         filename.back() == '\"' || filename.back() == ' ' ||
                                         filename.back() == '\t' || filename.back() == '\r' ||
                                         filename.back() == '\n')) {
                filename.pop_back();
            }

            // 1. Check message action code and status code
            std::vector<std::string> pos_args = parse_tonec_pos_args(req);
            if (pos_args.size() > 5 && !is_explicit_user_action) {
                try {
                    uint32_t action_code = std::stoul(pos_args[5]);
                    // Action 6 is redirect notification, 5 is HTML frame navigation, 4 is media info
                    if (action_code == 6 || action_code == 5 || action_code == 4) {
                        return "{\"status\": \"ignored\", \"reason\": \"non_download_action\"}";
                    }
                    uint32_t status_code = std::stoul(pos_args[3]);
                    if (status_code >= 300 && status_code < 400) {
                        return "{\"status\": \"ignored\", \"reason\": \"redirect_status\"}";
                    }
                } catch (...) {}
            }

            // 2. Check field 12 (status line, e.g. "HTTP/2.0 302")
            std::string status_line = extract_tonec_field(req, 12);
            if (!status_line.empty()) {
                size_t sp = status_line.find(' ');
                if (sp != std::string::npos && sp + 4 <= status_line.length()) {
                    try {
                        int sc = std::stoi(status_line.substr(sp + 1, 3));
                        if (sc >= 300 && sc < 400) {
                            return "{\"status\": \"ignored\", \"reason\": \"redirect_status\"}";
                        }
                    } catch (...) {}
                }
            }

            // 3. Check response headers (Content-Type & Content-Disposition)
            bool is_attachment = false;
            std::string content_type;
            if (!resp_headers.empty()) {
                std::string lower_h = sys::ascii_tolower(resp_headers);

                size_t cd_pos = lower_h.find("content-disposition:");
                if (cd_pos != std::string::npos) {
                    size_t cd_end = lower_h.find_first_of("\r\n", cd_pos);
                    std::string cd_val = lower_h.substr(cd_pos, cd_end - cd_pos);
                    if (cd_val.find("attachment") != std::string::npos) {
                        is_attachment = true;
                    }
                }

                size_t ct_pos = lower_h.find("content-type:");
                if (ct_pos != std::string::npos) {
                    size_t ct_start = ct_pos + 13;
                    while (ct_start < resp_headers.size() && (resp_headers[ct_start] == ' ' || resp_headers[ct_start] == '\t')) ct_start++;
                    size_t ct_end = resp_headers.find_first_of("\r\n;", ct_start);
                    content_type = resp_headers.substr(ct_start, ct_end - ct_start);
                }
            }

            // 4. Extract extension from filename or URL
            std::string ext;
            size_t dot = filename.find_last_of('.');
            if (dot != std::string::npos) {
                ext = filename.substr(dot + 1);
            } else {
                size_t q = url.find('?');
                std::string path_part = (q != std::string::npos) ? url.substr(0, q) : url;
                size_t url_dot = path_part.find_last_of('.');
                if (url_dot != std::string::npos && url_dot > path_part.find_last_of('/')) {
                    ext = path_part.substr(url_dot + 1);
                }
            }
            while (!ext.empty() && (ext.back() == ',' || ext.back() == ';' || ext.back() == '\"' ||
                                    ext.back() == ' ' || ext.back() == '\t' || ext.back() == '\r' ||
                                    ext.back() == '\n')) {
                ext.pop_back();
            }

            // 5. Apply IDM content & extension filtering rules
            if (!is_attachment && !is_explicit_user_action && !YouTubeEngine::is_youtube_url(url)) {
                if (is_web_content_type(content_type)) {
                    std::cout << "OpenDM IPC: Ignored web content type '" << content_type << "' for " << url << std::endl;
                    return "{\"status\": \"ignored\", \"reason\": \"web_content_type\"}";
                }
                if (is_forbidden_web_extension(ext)) {
                    std::cout << "OpenDM IPC: Ignored web resource extension '" << ext << "' for " << url << std::endl;
                    return "{\"status\": \"ignored\", \"reason\": \"web_resource_extension\"}";
                }
                if (!is_downloadable_extension(ext)) {
                    std::cout << "OpenDM IPC: Ignored non-downloadable extension '" << ext << "' for " << url << std::endl;
                    return "{\"status\": \"ignored\", \"reason\": \"not_downloadable_type\"}";
                }
            }

            if (is_explicit_user_action || YouTubeEngine::is_youtube_url(url)) {
                if (!page_title.empty() && (filename.empty() || filename == "download.bin" || filename.rfind("videoplayback", 0) == 0)) {
                    filename = sanitize_media_filename(page_title, "mp4");
                } else if (filename.find('.') == std::string::npos || filename == "download.bin") {
                    if (url.find("audio") != std::string::npos || url.find(".mp3") != std::string::npos || url.find(".m4a") != std::string::npos) {
                        filename = (filename == "download.bin" ? "audio_download.mp3" : (filename + ".mp3"));
                    } else {
                        filename = (filename == "download.bin" ? "video_download.mp4" : (filename + ".mp4"));
                    }
                }
            }

            std::string category = QueueManager::detect_category(filename);

            if (gui_mode_) {
                DownloadPromptInfo prompt_info;
                prompt_info.url = url;
                prompt_info.audio_stream_url = audio_url;
                prompt_info.is_streaming_media = !audio_url.empty();
                prompt_info.filename = filename;
                prompt_info.category = category;
                prompt_info.file_size = file_size;
                prompt_info.referer = referer;
                prompt_info.cookies = cookies;
                prompt_info.user_agent = user_agent;

                std::lock_guard<std::mutex> lock(prompt_mutex_);
                if (prompt_callback_) {
                    prompt_callback_(prompt_info);
                } else {
                    pending_prompts_.push_back(prompt_info);
                }
                std::cout << "OpenDM IPC: Intercepted download from browser, prompting user: " << url << " (" << filename << ", size: " << file_size << ")" << std::endl;
                return json({{"status", "prompting"}, {"filename", filename}}).dump(2);
            } else {
                std::string path = get_default_downloads_folder();
                std::string dest_file = sys::path_to_utf8(sys::path_from_utf8(path) / sys::path_from_utf8(filename));
                static std::atomic<uint64_t> counter{1};
                std::string id = "ext_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter.fetch_add(1));

                auto session = std::make_shared<DownloadSession>(id, url, dest_file);
                if (!audio_url.empty()) {
                    session->set_audio_stream_url(audio_url);
                }
                SiteLogonManager::instance().apply_to_session(*session, url);
                if (!referer.empty()) session->set_header("Referer", referer);
                if (!cookies.empty()) session->set_header("Cookie", cookies);
                if (!user_agent.empty()) session->set_header("User-Agent", user_agent);

                QueueManager::instance().get_main_queue().add_session(session, true);
                std::cout << "OpenDM IPC: Headless intercepted download from browser: " << url << " -> " << dest_file << std::endl;
                return json({{"status", "ok"}, {"id", id}, {"filename", filename}}).dump(2);
            }
        }
    }

    json jreq;
    try {
        jreq = json::parse(req);
    } catch (...) {
        try {
            jreq = json::parse(request);
        } catch (...) {
            return json({{"status", "error"}, {"message", "Invalid JSON request"}}).dump(2);
        }
    }
    if (!jreq.is_object()) {
        return json({{"status", "error"}, {"message", "JSON request must be an object"}}).dump(2);
    }

    std::string action = json_get_str(jreq, "action");
    if (action == "ping") {
        return json({{"status", "ok"}, {"message", "pong"}}).dump(2);
    }

    if (action == "download") {
        std::string raw_url = json_get_str(jreq, "url");
        std::string path = json_get_str(jreq, "path");
        std::string filename = json_get_str(jreq, "filename");
        std::string queue_name = json_get_str(jreq, "queue");
        (void)queue_name;
        std::string add_only_str = json_get_str(jreq, "add_only");
        std::string no_prompt_str = json_get_str(jreq, "no_prompt");
        std::string req_audio = json_get_str(jreq, "audio_url");
        std::string user_agent = json_get_str(jreq, "user_agent");
        std::string referer = json_get_str(jreq, "referer");
        std::string cookies = json_get_str(jreq, "cookies");

        if (raw_url.empty()) {
            return json({{"status", "error"}, {"message", "Missing URL"}}).dump(2);
        }

        auto [video_url, audio_url] = split_dual_stream_url(raw_url);
        video_url = clean_stream_query_parameters(video_url);
        if (!req_audio.empty()) {
            audio_url = clean_stream_query_parameters(normalize_stream_scheme(req_audio));
        } else if (!audio_url.empty()) {
            audio_url = clean_stream_query_parameters(audio_url);
        }
        if ((video_url.find("googlevideo.com") != std::string::npos || video_url.find("videoplayback") != std::string::npos) &&
            (video_url.find("sabr=1") != std::string::npos || video_url.find("sabr=2") != std::string::npos)) {
            std::cout << "OpenDM IPC: Ignored YouTube SABR video stream: " << video_url << std::endl;
            return json({{"status", "ignored"}, {"reason", "youtube_sabr_stream_not_downloadable"}}).dump(2);
        }
        if (!audio_url.empty() && (audio_url.find("googlevideo.com") != std::string::npos || audio_url.find("videoplayback") != std::string::npos) &&
            (audio_url.find("sabr=1") != std::string::npos || audio_url.find("sabr=2") != std::string::npos)) {
            std::cout << "OpenDM IPC: Ignored YouTube SABR audio stream: " << audio_url << std::endl;
            return json({{"status", "ignored"}, {"reason", "youtube_sabr_stream_not_downloadable"}}).dump(2);
        }
        std::string url = video_url;

        if (filename.empty() || filename == "download.bin") {
            filename = DownloadSession::extract_filename_from_url(url);
        } else {
            filename = DownloadSession::sanitize_filename(DownloadSession::url_decode(filename));
        }

        if (gui_mode_ && no_prompt_str != "true" && add_only_str != "true") {
            DownloadPromptInfo prompt_info;
            prompt_info.url = url;
            prompt_info.audio_stream_url = audio_url;
            prompt_info.is_streaming_media = !audio_url.empty();
            prompt_info.filename = filename;
            prompt_info.destination_dir = path;
            prompt_info.category = QueueManager::detect_category(filename);
            prompt_info.user_agent = user_agent;
            prompt_info.referer = referer;
            prompt_info.cookies = cookies;

            std::lock_guard<std::mutex> lock(prompt_mutex_);
            if (prompt_callback_) {
                prompt_callback_(prompt_info);
            } else {
                pending_prompts_.push_back(prompt_info);
            }
            return json({{"status", "prompting"}, {"filename", filename}}).dump(2);
        }

        if (path.empty()) {
            path = get_default_downloads_folder();
        }

        std::string dest_file = sys::path_to_utf8(sys::path_from_utf8(path) / sys::path_from_utf8(filename));
        static std::atomic<uint64_t> counter{1};
        std::string id = "task_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter.fetch_add(1));

        auto session = std::make_shared<DownloadSession>(id, url, dest_file);
        if (!audio_url.empty()) {
            session->set_audio_stream_url(audio_url);
        }
        if (!user_agent.empty()) {
            session->set_header("User-Agent", user_agent);
        }
        if (!referer.empty()) {
            session->set_header("Referer", referer);
        }
        if (!cookies.empty()) {
            session->set_header("Cookie", cookies);
        }
        SiteLogonManager::instance().apply_to_session(*session, url);

        std::string connections_str = json_get_str(jreq, "connections");
        if (!connections_str.empty()) {
            try {
                session->set_max_connections(std::stoul(connections_str));
            } catch (...) {}
        }
        std::string speed_limit_str = json_get_str(jreq, "speed_limit");
        if (!speed_limit_str.empty()) {
            try {
                uint64_t limit_kb = std::stoull(speed_limit_str);
                if (limit_kb > 0) {
                    session->set_speed_limit(limit_kb * 1024);
                    session->enable_speed_limit(true);
                }
            } catch (...) {}
        }

        bool start_now = (add_only_str != "true");
        QueueManager::instance().get_main_queue().add_session(session, start_now);

        return json({{"status", "ok"}, {"id", id}, {"filename", filename}}).dump(2);
    } else if (action == "start_queue") {
        QueueManager::instance().get_main_queue().start_queue();
        return json({{"status", "ok"}, {"message", "Queue started"}}).dump(2);
    } else if (action == "stop_queue") {
        QueueManager::instance().get_main_queue().stop_queue();
        return json({{"status", "ok"}, {"message", "Queue stopped"}}).dump(2);
    } else if (action == "pause") {
        std::string id = json_get_str(jreq, "id");
        auto s = QueueManager::instance().find_session(id);
        if (s) {
            s->pause();
            return json({{"status", "ok"}, {"message", "Session paused"}}).dump(2);
        }
        return json({{"status", "error"}, {"message", "Session not found"}}).dump(2);
    } else if (action == "resume") {
        std::string id = json_get_str(jreq, "id");
        auto s = QueueManager::instance().find_session(id);
        if (s) {
            s->resume();
            return json({{"status", "ok"}, {"message", "Session resumed"}}).dump(2);
        }
        return json({{"status", "error"}, {"message", "Session not found"}}).dump(2);
    } else if (action == "cancel") {
        std::string id = json_get_str(jreq, "id");
        if (QueueManager::instance().get_main_queue().remove_session(id)) {
            return json({{"status", "ok"}, {"message", "Session cancelled"}}).dump(2);
        }
        return json({{"status", "error"}, {"message", "Session not found"}}).dump(2);
    } else if (action == "mux") {
        std::string video = json_get_str(jreq, "video");
        std::string audio = json_get_str(jreq, "audio");
        std::string output = json_get_str(jreq, "output");
        if (video.empty() || audio.empty() || output.empty()) {
            return json({{"status", "error"}, {"message", "Missing video, audio, or output parameters"}}).dump(2);
        }
        std::string mux_err;
        if (MediaMuxer::mux_mp4_and_mp4(video, audio, output, nullptr, &mux_err)) {
            return json({{"status", "ok"}, {"output", output}}).dump(2);
        } else {
            return json({{"status", "error"}, {"message", mux_err}}).dump(2);
        }
    } else if (action == "status") {
        auto sessions = QueueManager::instance().get_main_queue().get_all_sessions();
        json dl_arr = json::array();
        for (const auto& s : sessions) {
            auto prog = s->get_progress();
            dl_arr.push_back({
                {"id", s->get_id()},
                {"filename", s->get_filename()},
                {"total_size", prog.total_size},
                {"downloaded", prog.downloaded_bytes},
                {"speed", prog.speed_bytes_per_sec},
                {"progress", prog.progress_fraction}
            });
        }
        return json({{"status", "ok"}, {"count", sessions.size()}, {"downloads", dl_arr}}).dump(2);
    } else if (action == "extract_youtube") {
        std::string url = json_get_str(jreq, "url");
        if (url.empty()) {
            std::string video_id = json_get_str(jreq, "videoId");
            if (!video_id.empty()) {
                url = "https://www.youtube.com/watch?v=" + video_id;
            }
        }
        if (url.empty()) {
            return json({{"status", "error"}, {"message", "Missing url parameter"}}).dump(2);
        }

        YouTubeVideoInfo info;
        std::string err;
        bool ok = YouTubeEngine::extract_video_info(url, info, &err);
        if (!ok || !info.has_playable_formats()) {
            return json({{"status", "error"}, {"message", err.empty() ? "No playable formats found" : err}}).dump(2);
        }
        return build_youtube_extract_json(info);
    }

    return json({{"status", "error"}, {"message", "Unknown action"}}).dump(2);
}

std::string IpcServer::send_command(const std::string& json_request, const std::string& socket_path) {
#ifdef _WIN32
    std::string pipe_name = socket_path;
    if (pipe_name.rfind("\\\\.\\pipe\\", 0) != 0) {
        std::string stem = sys::path_from_utf8(pipe_name).stem().string();
        if (stem.empty()) stem = "opendm_pipe";
        pipe_name = "\\\\.\\pipe\\" + stem;
    }

    std::wstring wpipe = sys::utf8_to_wide(pipe_name);
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    for (int retry = 0; retry < 20; ++retry) {
        hPipe = CreateFileW(wpipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe != INVALID_HANDLE_VALUE) break;
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(wpipe.c_str(), 500);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    if (hPipe == INVALID_HANDLE_VALUE) {
        return json({{"status", "error"}, {"message", "Could not connect to OpenDM socket"}}).dump(2);
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    std::string req = json_request + "\n";
    DWORD total_written = 0;
    while (total_written < req.size()) {
        DWORD chunk_written = 0;
        if (!WriteFile(hPipe, req.data() + total_written,
                       static_cast<DWORD>(req.size() - total_written), &chunk_written, nullptr)) {
            CloseHandle(hPipe);
            return json({{"status", "error"}, {"message", "Failed writing to OpenDM pipe"}}).dump(2);
        }
        total_written += chunk_written;
    }

    std::string response;
    std::vector<char> chunk(PIPE_IO_BUF_SIZE);
    while (response.size() < MAX_IPC_MSG_BYTES) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hPipe, chunk.data(), PIPE_IO_BUF_SIZE, &bytesRead, nullptr);
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (bytesRead > 0) {
            response.append(chunk.data(), bytesRead);
        }
        if (ok || err != ERROR_MORE_DATA) {
            break;
        }
    }

    CloseHandle(hPipe);
    if (response.empty()) {
        return json({{"status", "error"}, {"message", "Empty response from OpenDM pipe"}}).dump(2);
    }
    return response;
#else
    int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd < 0) {
        return json({{"status", "error"}, {"message", "Failed to create client socket"}}).dump(2);
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(client_fd);
        return json({{"status", "error"}, {"message", "Could not connect to OpenDM socket"}}).dump(2);
    }

    std::string req = json_request + "\n";
    size_t total_written = 0;
    while (total_written < req.size()) {
        ssize_t w = write(client_fd, req.data() + total_written, req.size() - total_written);
        if (w <= 0) break;
        total_written += static_cast<size_t>(w);
    }
    shutdown(client_fd, SHUT_WR);

    std::string response;
    char buffer[8192];
    ssize_t bytes_read = 0;
    while ((bytes_read = read(client_fd, buffer, sizeof(buffer))) > 0) {
        response.append(buffer, static_cast<size_t>(bytes_read));
        if (is_complete_ipc_frame(response)) break;
    }

    close(client_fd);
    return response;
#endif
}

} // namespace opendm
