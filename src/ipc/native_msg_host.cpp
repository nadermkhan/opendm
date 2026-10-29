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
    if (opendm_bin == "opendm") {
        if (access("/usr/bin/opendm", X_OK) == 0) {
            opendm_bin = "/usr/bin/opendm";
        } else if (access("/usr/local/bin/opendm", X_OK) == 0) {
            opendm_bin = "/usr/local/bin/opendm";
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
        if (opendm_bin.find('/') != std::string::npos) {
            execl(opendm_bin.c_str(), opendm_bin.c_str(), nullptr);
        } else {
            execlp(opendm_bin.c_str(), opendm_bin.c_str(), nullptr);
        }
        _exit(1);
    } else if (pid > 0) {
        // Poll for OpenDM IPC socket to become ready (up to 3 seconds)
        for (int i = 0; i < 30; ++i) {
            usleep(100000); // 100ms
            std::string check = IpcServer::send_command("{\"action\": \"ping\"}");
            if (check.find("Could not connect") == std::string::npos) {
                break; // Socket is up and responding
            }
        }
    }
}

} // namespace opendm

#include <fstream>
#include <csignal>

int main(int argc, char* argv[]) {
    signal(SIGCHLD, SIG_IGN); // Prevent zombie processes
    std::ios_base::sync_with_stdio(false);

    std::ofstream log("/tmp/opendm_native_host.log", std::ios::app);
    log << "[NATIVE_HOST] Started. argc=" << argc << std::endl;
    for (int i = 0; i < argc; ++i) {
        log << "  argv[" << i << "] = " << argv[i] << std::endl;
    }
    log.flush();

    std::string incoming;
    while (opendm::read_native_message(incoming)) {
        log << "[NATIVE_HOST] incoming: " << incoming << std::endl;
        log.flush();
        std::string raw = opendm::unquote_json_string(incoming);

        if (raw.rfind("MSG#", 0) == 0) {
            opendm::MsgHeader hdr = opendm::parse_msg_header(raw);
            log << "[NATIVE_HOST] parsed MSG header: id=" << hdr.id << " type=" << hdr.type << " subtype=" << hdr.subtype << std::endl;
            log.flush();

            // 1. Handshake request (type == 2)
            if (hdr.type == 2) {
                log << "[NATIVE_HOST] Sending handshake responses (Type 3 & Type 4)" << std::endl;
                log.flush();
                // Packet 1: Handshake confirmation (Type 3)
                opendm::write_native_json_string("MSG#1#3#0#0:113:0:0:0:96:103481600:0:0:100;");

                // Packet 2: Configuration & activation (Type 4) matching IDM defaults
                opendm::write_native_json_string(
                    "MSG#2#4#0#1918:0:0:0:0,12=2:.*,1=245:ZIP|EXE|7Z|RAR|TAR|GZ|BZ2|ISO|DMG|DEB|RPM|PKG|MP4|MKV|AVI|MP3|FLAC|WAV|PDF|EPUB|DOC|DOCX|XLS|XLSX|PPT|PPTX|APK|TORRENT|3GP|AAC|ACE|AIF|ARJ|ASF|BIN|M4A|M4V|MOV|MPA|MPE|MPEG|MPG|MSI|MSU|OGG|OGV|PLJ|PPS|QT|R0*|R1*|RA|SEA|SIT|SITX|TIF|TIFF|WMA|WMV|Z,2=55:CSS|JS|JSON|CRL|OCSP|APNG|GIF|ICO|JPEG|JPG|PNG|SVG|WEBP,3=97:3GP|ASF|AVI|F4V|FLV|M4A|M4V|MKV|MOV|MP3|MP4|MPEG|MPG|OGG|OGV|QT|RA|RM|TS|TSA|TSV|WAV|WEBM|WMA|WMV,4=30:JSON|GZ|Z|BZ2|BIN|ZIP|GZIP|LZH,10=117:application/download|application/force-download|application/x-msdownload|application/octet-stream|binary/octet-stream,20=80:TXT|HTM|HTML|PHP|ASP|ASPX|CGI|JSP|JSPX|CSS|JS|APNG|GIF|ICO|JPEG|JPG|PNG|SVG|WEBP,21=16:/*-internal-*/**;"
                );
                continue;
            }

            // 2. Download interception request (type == 13, type == 14, or has 6= url)
            if (hdr.type == 13 || hdr.type == 14 || raw.find("6=") != std::string::npos) {
                std::vector<std::string> pos_args = opendm::parse_tonec_pos_args(raw);
                uint32_t action_code = 0;
                uint32_t status_code = 0;
                if (pos_args.size() > 5) {
                    try {
                        action_code = std::stoul(pos_args[5]);
                        status_code = std::stoul(pos_args[3]);
                    } catch (...) {}
                }

                log << "[NATIVE_HOST] Interception message received: id=" << hdr.id
                    << " type=" << hdr.type << " action=" << action_code << " status=" << status_code << std::endl;
                log.flush();

                // Non-download notifications:
                // Action 6 = HTTP 3xx redirect notification
                // Action 5 = HTML page frame navigation notification
                // Action 4 = Media response started / panel tracking
                if (action_code == 6 || action_code == 5 || action_code == 4 || (status_code >= 300 && status_code < 400)) {
                    log << "[NATIVE_HOST] Acknowledging non-download notification without cancel (action=" << action_code << ")" << std::endl;
                    log.flush();
                    opendm::write_native_json_string("MSG#" + std::to_string(hdr.id) + "#1#2#0:" + std::to_string(hdr.id) + ":0;");
                    continue;
                }

                opendm::ensure_opendm_running();
                std::string resp = opendm::IpcServer::send_command(raw);
                if (resp.find("Could not connect") != std::string::npos) {
                    opendm::ensure_opendm_running();
                    resp = opendm::IpcServer::send_command(raw);
                }

                log << "[NATIVE_HOST] IPC server response: " << resp << std::endl;
                log.flush();

                // Only instruct browser to cancel internal download (e[1]=3) if IPC server accepted the download.
                // If ignored (web resource, HTML/PHP, image without attachment), send e[1]=0 so browser continues.
                if (resp.find("\"status\": \"prompting\"") != std::string::npos ||
                    resp.find("\"status\": \"downloading\"") != std::string::npos ||
                    resp.find("\"status\": \"started\"") != std::string::npos) {
                    opendm::write_native_json_string("MSG#" + std::to_string(hdr.id) + "#1#2#0:" + std::to_string(hdr.id) + ":3;");
                } else {
                    opendm::write_native_json_string("MSG#" + std::to_string(hdr.id) + "#1#2#0:" + std::to_string(hdr.id) + ":0;");
                }
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

    log << "[NATIVE_HOST] Exiting." << std::endl;
    log.flush();
    return 0;
}
