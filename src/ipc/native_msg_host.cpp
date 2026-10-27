#include "ipc_server.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>
#include <filesystem>
#include <sys/types.h>
#include <sys/wait.h>

namespace opendm {

// Reads a 32-bit length-prefixed native message from stdin
bool read_native_message(std::string& out_msg) {
    uint32_t length = 0;
    if (std::cin.read(reinterpret_cast<char*>(&length), sizeof(length))) {
        if (length == 0 || length > 10 * 1024 * 1024) { // 10MB safety cap
            return false;
        }
        std::vector<char> buffer(length);
        if (std::cin.read(buffer.data(), length)) {
            out_msg.assign(buffer.data(), length);
            return true;
        }
    }
    return false;
}

// Writes a 32-bit length-prefixed JSON string to stdout
void write_native_json_string(const std::string& raw_msg) {
    std::string json_str = "\"";
    for (char c : raw_msg) {
        if (c == '"') json_str += "\\\"";
        else if (c == '\\') json_str += "\\\\";
        else if (c == '\n') json_str += "\\n";
        else if (c == '\r') json_str += "\\r";
        else if (c == '\t') json_str += "\\t";
        else json_str += c;
    }
    json_str += "\"";

    uint32_t length = static_cast<uint32_t>(json_str.length());
    std::cout.write(reinterpret_cast<const char*>(&length), sizeof(length));
    std::cout.write(json_str.data(), length);
    std::cout.flush();
}

// Writes raw JSON to stdout (for standard JSON messages)
void write_native_raw_json(const std::string& json_obj) {
    uint32_t length = static_cast<uint32_t>(json_obj.length());
    std::cout.write(reinterpret_cast<const char*>(&length), sizeof(length));
    std::cout.write(json_obj.data(), length);
    std::cout.flush();
}

// Helper to strip surrounding JSON quotes and unescape
std::string unquote_json_string(const std::string& input) {
    if (input.size() >= 2 && input.front() == '"' && input.back() == '"') {
        std::string unescaped;
        unescaped.reserve(input.size() - 2);
        for (size_t i = 1; i < input.size() - 1; ++i) {
            if (input[i] == '\\' && i + 1 < input.size() - 1) {
                char next = input[++i];
                if (next == '"') unescaped += '"';
                else if (next == '\\') unescaped += '\\';
                else if (next == 'n') unescaped += '\n';
                else if (next == 'r') unescaped += '\r';
                else if (next == 't') unescaped += '\t';
                else unescaped += next;
            } else {
                unescaped += input[i];
            }
        }
        return unescaped;
    }
    return input;
}

struct MsgHeader {
    uint32_t id{0};
    uint32_t type{0};
    uint32_t subtype{0};
    uint32_t flags{0};
    bool valid{false};
};

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

void ensure_opendm_running() {
    std::string test_resp = IpcServer::send_command("{\"action\": \"ping\"}");
    if (test_resp.find("Could not connect") == std::string::npos) {
        return; // Already running
    }

    std::string opendm_bin = "opendm";
    const char* home = getenv("HOME");
    if (home) {
        std::string local_bin = std::string(home) + "/.local/bin/opendm";
        if (access(local_bin.c_str(), X_OK) == 0) {
            opendm_bin = local_bin;
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        setsid();
        execl(opendm_bin.c_str(), opendm_bin.c_str(), nullptr);
        _exit(1);
    } else if (pid > 0) {
        usleep(400000); // 400ms
    }
}

} // namespace opendm

int main(int argc, char* argv[]) {
    // Set standard IO to binary mode
    std::ios_base::sync_with_stdio(false);

    std::string incoming;
    while (opendm::read_native_message(incoming)) {
        std::string raw = opendm::unquote_json_string(incoming);

        if (raw.rfind("MSG#", 0) == 0) {
            opendm::MsgHeader hdr = opendm::parse_msg_header(raw);

            // 1. Handshake request (type == 2)
            if (hdr.type == 2) {
                // Packet 1: Handshake confirmation (Type 3)
                opendm::write_native_json_string("MSG#1#3#0#0:113:0:0:0:96:103481600:0:0:100;");

                // Packet 2: Configuration & activation (Type 4)
                // Enables context menu, video grabber, and auto-interception for download file types
                opendm::write_native_json_string(
                    "MSG#2#4#0#0:0:0:0:0,12=2:.*,1=127:ZIP|EXE|7Z|RAR|TAR|GZ|BZ2|ISO|DMG|DEB|RPM|PKG|MP4|MKV|AVI|MP3|FLAC|WAV|PDF|EPUB|DOC|DOCX|XLS|XLSX|PPT|PPTX|APK|TORRENT,2=7:(?!.*),3=7:(?!.*),4=2:.*,10=2:.*;"
                );
                continue;
            }

            // 2. Download interception request (type == 14 or has 6= url)
            if (hdr.type == 14 || raw.find("6=") != std::string::npos) {
                opendm::ensure_opendm_running();
                std::string resp = opendm::IpcServer::send_command(raw);
                if (resp.find("Could not connect") != std::string::npos) {
                    opendm::ensure_opendm_running();
                    resp = opendm::IpcServer::send_command(raw);
                }

                // Send ACK back to browser
                opendm::write_native_json_string("MSG#" + std::to_string(hdr.id) + "#1#2#0:" + std::to_string(hdr.id) + ";");
                continue;
            }

            // 3. Ping / keep-alive (type == 0) or any other message
            opendm::write_native_json_string("MSG#" + std::to_string(hdr.id) + "#1#2#0:" + std::to_string(hdr.id) + ";");
            continue;
        }

        // Standard JSON messaging
        opendm::ensure_opendm_running();
        std::string response = opendm::IpcServer::send_command(incoming);
        opendm::write_native_raw_json(response);
    }

    return 0;
}
