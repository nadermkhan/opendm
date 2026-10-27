#include "ipc_server.hpp"
#include "../core/download_queue.hpp"
#include "../core/media_muxer.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include <vector>
#include <filesystem>

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

} // namespace

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
            if (request.find('\n') != std::string::npos || bytes_read < (ssize_t)sizeof(buffer) - 1) {
                break;
            }
        }

        std::string response = handle_request(request);
        response += "\n";
        (void)write(client_fd, response.c_str(), response.length());
        close(client_fd);
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
        std::string url = extract_tonec_field(req, 6);
        if (!url.empty()) {
            std::string filename = extract_tonec_field(req, 100);
            std::string referer = extract_tonec_field(req, 50);
            std::string cookies = extract_tonec_field(req, 51);
            std::string user_agent = extract_tonec_field(req, 54);

            std::string path = (std::filesystem::path(getenv("HOME") ? getenv("HOME") : "/home/nader") / "Downloads").string();
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

            std::string dest_file = path + "/" + filename;
            static uint64_t counter = 1;
            std::string id = "ext_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter++);

            auto session = std::make_shared<DownloadSession>(id, url, dest_file);
            if (!referer.empty()) session->set_header("Referer", referer);
            if (!cookies.empty()) session->set_header("Cookie", cookies);
            if (!user_agent.empty()) session->set_header("User-Agent", user_agent);

            QueueManager::instance().get_main_queue().add_session(session, true);
            std::cout << "OpenDM IPC: Intercepted download from browser: " << url << " -> " << dest_file << std::endl;
            return "{\"status\": \"ok\", \"id\": \"" + id + "\", \"filename\": \"" + filename + "\"}";
        }
    }

    std::string action = extract_json_field(req, "action");
    if (action == "ping") {
        return "{\"status\": \"ok\", \"message\": \"pong\"}";
    }

    if (action == "download") {
        std::string url = extract_json_field(request, "url");
        std::string path = extract_json_field(request, "path");
        std::string filename = extract_json_field(request, "filename");
        std::string queue_name = extract_json_field(request, "queue");
        std::string add_only_str = extract_json_field(request, "add_only");

        if (url.empty()) {
            return "{\"status\": \"error\", \"message\": \"Missing URL\"}";
        }

        if (path.empty()) {
            path = "/home/nader/Downloads";
        }

        if (filename.empty()) {
            size_t slash = url.find_last_of('/');
            if (slash != std::string::npos && slash + 1 < url.length()) {
                filename = url.substr(slash + 1);
            } else {
                filename = "download.bin";
            }
        }

        std::string dest_file = path + "/" + filename;
        static uint64_t counter = 1;
        std::string id = "task_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter++);

        auto session = std::make_shared<DownloadSession>(id, url, dest_file);
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
