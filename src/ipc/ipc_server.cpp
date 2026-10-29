#include "ipc_server.hpp"
#include "../core/download_queue.hpp"
#include "../core/media_muxer.hpp"
#include "../core/site_logon.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <unordered_set>
#include <cctype>

namespace opendm {

namespace {

std::string extract_json_field(const std::string& json, const std::string& field) {
    std::string key = "\"" + field + "\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + key.length());
    if (pos == std::string::npos) return "";

    // Skip whitespace
    while (pos < json.length() && (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t')) {
        pos++;
    }

    if (pos >= json.length()) return "";

    if (json[pos] == '\"') {
        size_t end = json.find('\"', pos + 1);
        if (end == std::string::npos) return "";
        return json.substr(pos + 1, end - pos - 1);
    } else {
        size_t end = json.find_first_of(",}\n\r", pos);
        if (end == std::string::npos) end = json.length();
        std::string val = json.substr(pos, end - pos);
        // Trim
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
        return val;
    }
}

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
    for (char c : title) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '\"' || c == '<' || c == '>' || c == '|') {
            clean += '_';
        } else if (static_cast<unsigned char>(c) >= 32) {
            clean += c;
        }
    }
    while (!clean.empty() && (clean.front() == ' ' || clean.front() == '.')) clean.erase(0, 1);
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
    if (clean.empty()) {
        return "video_download." + default_ext;
    }
    if (clean.find('.') == std::string::npos) {
        clean += "." + default_ext;
    }
    return clean;
}

IpcServer::IpcServer(const std::string& socket_path) : socket_path_(socket_path) {}

IpcServer::~IpcServer() {
    stop();
}

bool IpcServer::start() {
    if (running_) return true;

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
}

void IpcServer::stop() {
    if (!running_) return;

    running_ = false;
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
}

bool IpcServer::is_running() const {
    return running_;
}

void IpcServer::server_loop() {
    while (running_) {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (!running_) break;
            continue;
        }

        // Read message from client
        std::string request;
        char buffer[4096];
        ssize_t bytes_read = 0;

        while ((bytes_read = read(client_fd, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            request.append(buffer, bytes_read);
            if (request.find('\n') != std::string::npos) {
                break;
            }
        }

        std::string response = handle_request(request);
        response += "\n";
        (void)write(client_fd, response.c_str(), response.length());
        close(client_fd);
    }
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
            std::string page_title = media_title.empty() ? page_url : media_title;
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
                std::string lower_h = resp_headers;
                std::transform(lower_h.begin(), lower_h.end(), lower_h.begin(), ::tolower);
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
                    size_t cd_pos = lower_h.find("filename=");
                    if (cd_pos != std::string::npos) {
                        size_t fn_start = cd_pos + 9;
                        size_t fn_end = resp_headers.find_first_of("\r\n;", fn_start);
                        std::string cd_name = resp_headers.substr(fn_start, fn_end - fn_start);
                        while (!cd_name.empty() && (cd_name.front() == '"' || cd_name.front() == ' ')) cd_name.erase(0, 1);
                        while (!cd_name.empty() && (cd_name.back() == '"' || cd_name.back() == ' ')) cd_name.pop_back();
                        if (!cd_name.empty()) {
                            size_t slash = cd_name.find_last_of("/\\");
                            if (slash != std::string::npos) cd_name = cd_name.substr(slash + 1);
                            filename = cd_name;
                        }
                    }
                }
            }

            if (filename.empty() || filename == "ZIP" || filename == "videoplayback" || filename == "videoplayback.mp4" || filename == "download.bin") {
                if (!page_title.empty()) {
                    filename = sanitize_media_filename(page_title, "mp4");
                } else {
                    size_t slash = url.find_last_of('/');
                    if (slash != std::string::npos && slash + 1 < url.length()) {
                        filename = url.substr(slash + 1);
                        size_t q = filename.find('?');
                        if (q != std::string::npos) filename = filename.substr(0, q);
                    } else {
                        filename = "download.bin";
                    }
                }
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
                std::string lower_h = resp_headers;
                std::transform(lower_h.begin(), lower_h.end(), lower_h.begin(), ::tolower);

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
            if (!is_attachment && !is_explicit_user_action) {
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

            if (is_explicit_user_action) {
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
                return "{\"status\": \"prompting\", \"filename\": \"" + filename + "\"}";
            } else {
                std::string path = (std::filesystem::path(getenv("HOME") ? getenv("HOME") : "/home/nader") / "Downloads").string();
                std::string dest_file = path + "/" + filename;
                static uint64_t counter = 1;
                std::string id = "ext_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter++);

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
                return "{\"status\": \"ok\", \"id\": \"" + id + "\", \"filename\": \"" + filename + "\"}";
            }
        }
    }

    std::string action = extract_json_field(req, "action");
    if (action == "ping") {
        return "{\"status\": \"ok\", \"message\": \"pong\"}";
    }

    if (action == "download") {
        std::string raw_url = extract_json_field(request, "url");
        std::string path = extract_json_field(request, "path");
        std::string filename = extract_json_field(request, "filename");
        std::string queue_name = extract_json_field(request, "queue");
        std::string add_only_str = extract_json_field(request, "add_only");
        std::string no_prompt_str = extract_json_field(request, "no_prompt");
        std::string req_audio = extract_json_field(request, "audio_url");

        if (raw_url.empty()) {
            return "{\"status\": \"error\", \"message\": \"Missing URL\"}";
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
            return "{\"status\": \"ignored\", \"reason\": \"youtube_sabr_stream_not_downloadable\"}";
        }
        if (!audio_url.empty() && (audio_url.find("googlevideo.com") != std::string::npos || audio_url.find("videoplayback") != std::string::npos) &&
            (audio_url.find("sabr=1") != std::string::npos || audio_url.find("sabr=2") != std::string::npos)) {
            std::cout << "OpenDM IPC: Ignored YouTube SABR audio stream: " << audio_url << std::endl;
            return "{\"status\": \"ignored\", \"reason\": \"youtube_sabr_stream_not_downloadable\"}";
        }
        std::string url = video_url;

        if (filename.empty()) {
            size_t slash = url.find_last_of('/');
            if (slash != std::string::npos && slash + 1 < url.length()) {
                filename = url.substr(slash + 1);
                size_t q = filename.find('?');
                if (q != std::string::npos) filename = filename.substr(0, q);
            } else {
                filename = "download.bin";
            }
        }

        if (gui_mode_ && no_prompt_str != "true" && add_only_str != "true") {
            DownloadPromptInfo prompt_info;
            prompt_info.url = url;
            prompt_info.audio_stream_url = audio_url;
            prompt_info.is_streaming_media = !audio_url.empty();
            prompt_info.filename = filename;
            prompt_info.destination_dir = path;
            prompt_info.category = QueueManager::detect_category(filename);

            std::lock_guard<std::mutex> lock(prompt_mutex_);
            if (prompt_callback_) {
                prompt_callback_(prompt_info);
            } else {
                pending_prompts_.push_back(prompt_info);
            }
            return "{\"status\": \"prompting\", \"filename\": \"" + filename + "\"}";
        }

        if (path.empty()) {
            path = (std::filesystem::path(getenv("HOME") ? getenv("HOME") : "/home/nader") / "Downloads").string();
        }

        std::string dest_file = path + "/" + filename;
        static uint64_t counter = 1;
        std::string id = "task_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter++);

        auto session = std::make_shared<DownloadSession>(id, url, dest_file);
        if (!audio_url.empty()) {
            session->set_audio_stream_url(audio_url);
        }
        SiteLogonManager::instance().apply_to_session(*session, url);

        std::string connections_str = extract_json_field(request, "connections");
        if (!connections_str.empty()) {
            try {
                session->set_max_connections(std::stoul(connections_str));
            } catch (...) {}
        }
        std::string speed_limit_str = extract_json_field(request, "speed_limit");
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

        return "{\"status\": \"ok\", \"id\": \"" + id + "\", \"filename\": \"" + filename + "\"}";
    } else if (action == "start_queue") {
        QueueManager::instance().get_main_queue().start_queue();
        return "{\"status\": \"ok\", \"message\": \"Queue started\"}";
    } else if (action == "stop_queue") {
        QueueManager::instance().get_main_queue().stop_queue();
        return "{\"status\": \"ok\", \"message\": \"Queue stopped\"}";
    } else if (action == "pause") {
        std::string id = extract_json_field(request, "id");
        auto s = QueueManager::instance().find_session(id);
        if (s) {
            s->pause();
            return "{\"status\": \"ok\", \"message\": \"Session paused\"}";
        }
        return "{\"status\": \"error\", \"message\": \"Session not found\"}";
    } else if (action == "resume") {
        std::string id = extract_json_field(request, "id");
        auto s = QueueManager::instance().find_session(id);
        if (s) {
            s->resume();
            return "{\"status\": \"ok\", \"message\": \"Session resumed\"}";
        }
        return "{\"status\": \"error\", \"message\": \"Session not found\"}";
    } else if (action == "cancel") {
        std::string id = extract_json_field(request, "id");
        if (QueueManager::instance().get_main_queue().remove_session(id)) {
            return "{\"status\": \"ok\", \"message\": \"Session cancelled\"}";
        }
        return "{\"status\": \"error\", \"message\": \"Session not found\"}";
    } else if (action == "mux") {
        std::string video = extract_json_field(request, "video");
        std::string audio = extract_json_field(request, "audio");
        std::string output = extract_json_field(request, "output");
        if (video.empty() || audio.empty() || output.empty()) {
            return "{\"status\": \"error\", \"message\": \"Missing video, audio, or output parameters\"}";
        }
        std::string mux_err;
        if (MediaMuxer::mux_mp4_and_mp4(video, audio, output, nullptr, &mux_err)) {
            return "{\"status\": \"ok\", \"output\": \"" + output + "\"}";
        } else {
            return "{\"status\": \"error\", \"message\": \"" + mux_err + "\"}";
        }
    } else if (action == "status") {
        auto sessions = QueueManager::instance().get_main_queue().get_all_sessions();
        std::ostringstream ss;
        ss << "{\"status\": \"ok\", \"count\": " << sessions.size() << ", \"downloads\": [";
        for (size_t i = 0; i < sessions.size(); ++i) {
            if (i > 0) ss << ", ";
            auto prog = sessions[i]->get_progress();
            ss << "{\"id\": \"" << sessions[i]->get_id() << "\", "
               << "\"filename\": \"" << sessions[i]->get_filename() << "\", "
               << "\"total_size\": " << prog.total_size << ", "
               << "\"downloaded\": " << prog.downloaded_bytes << ", "
               << "\"speed\": " << prog.speed_bytes_per_sec << ", "
               << "\"progress\": " << prog.progress_fraction << "}";
        }
        ss << "]}";
        return ss.str();
    }

    return "{\"status\": \"error\", \"message\": \"Unknown action\"}";
}

std::string IpcServer::send_command(const std::string& json_request, const std::string& socket_path) {
    int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd < 0) {
        return "{\"status\": \"error\", \"message\": \"Failed to create client socket\"}";
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(client_fd);
        return "{\"status\": \"error\", \"message\": \"Could not connect to OpenDM socket\"}";
    }

    std::string req = json_request + "\n";
    (void)write(client_fd, req.c_str(), req.length());

    std::string response;
    char buffer[4096];
    ssize_t bytes_read = 0;
    while ((bytes_read = read(client_fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        response.append(buffer, bytes_read);
        if (response.find('\n') != std::string::npos) break;
    }

    close(client_fd);
    return response;
}

} // namespace opendm
