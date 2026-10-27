#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>

namespace opendm {

class DownloadQueue;

class IpcServer {
public:
    static constexpr const char* DEFAULT_SOCKET_PATH = "/tmp/opendm.sock";

    explicit IpcServer(const std::string& socket_path = DEFAULT_SOCKET_PATH);
    ~IpcServer();

    bool start();
    void stop();
    bool is_running() const;

    // Send a command to a running OpenDM instance via UNIX socket (client mode)
    static std::string send_command(const std::string& json_request,
                                    const std::string& socket_path = DEFAULT_SOCKET_PATH);

private:
    void server_loop();
    std::string handle_request(const std::string& request);

    std::string socket_path_;
    int server_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread server_thread_;
};

} // namespace opendm
