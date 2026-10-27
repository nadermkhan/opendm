#include "ipc_server.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/types.h>
#include <sys/wait.h>
#endif

#include <filesystem>

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

// Writes a 32-bit length-prefixed native message to stdout
void write_native_message(const std::string& msg) {
    uint32_t length = static_cast<uint32_t>(msg.length());
    std::cout.write(reinterpret_cast<const char*>(&length), sizeof(length));
    std::cout.write(msg.data(), length);
    std::cout.flush();
}

} // namespace opendm

int main(int argc, char* argv[]) {
    // Set standard IO to binary mode
    std::ios_base::sync_with_stdio(false);

    std::string incoming;
    while (opendm::read_native_message(incoming)) {
        // Forward message to OpenDM via unix domain socket
        std::string response = opendm::IpcServer::send_command(incoming);

        // If OpenDM daemon is not running, attempt to spawn it in the background
        if (response.find("Could not connect") != std::string::npos) {
            std::string opendm_bin = "opendm";
            char self_path[4096];
            ssize_t s_len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
            if (s_len > 0) {
                self_path[s_len] = '\0';
                std::string self_dir = std::filesystem::path(self_path).parent_path().string();
                std::string adjacent = self_dir + "/opendm";
                if (access(adjacent.c_str(), X_OK) == 0) {
                    opendm_bin = adjacent;
                }
            }

            pid_t pid = fork();
            if (pid == 0) {
                // Child process: detach completely from standard IO and start opendm daemon
                int devnull = open("/dev/null", O_RDWR);
                if (devnull >= 0) {
                    dup2(devnull, STDIN_FILENO);
                    dup2(devnull, STDOUT_FILENO);
                    dup2(devnull, STDERR_FILENO);
                    if (devnull > 2) close(devnull);
                }
                setsid();
                execl(opendm_bin.c_str(), opendm_bin.c_str(), "--daemon", nullptr);
                _exit(1);
            } else if (pid > 0) {
                // Wait briefly for daemon socket to become available
                usleep(400000); // 400ms
                response = opendm::IpcServer::send_command(incoming);
            }
        }

        opendm::write_native_message(response);
    }

    return 0;
}
