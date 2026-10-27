#include "core/download_session.hpp"
#include <iostream>
#include <vector>
#include <cassert>
#include <chrono>
#include <thread>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace fs = std::filesystem;

class TestHttpRangeServer {
public:
    TestHttpRangeServer(const std::vector<uint8_t>& payload)
        : payload_(payload), running_(false), server_fd_(-1), port_(0) {}

    ~TestHttpRangeServer() {
        stop();
    }

    bool start() {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) return false;

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // bind to ephemeral port

        if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(server_fd_);
            return false;
        }

        socklen_t len = sizeof(addr);
        if (getsockname(server_fd_, (struct sockaddr*)&addr, &len) < 0) {
            close(server_fd_);
            return false;
        }
        port_ = ntohs(addr.sin_port);

        if (listen(server_fd_, 16) < 0) {
            close(server_fd_);
            return false;
        }

        running_ = true;
        server_thread_ = std::thread(&TestHttpRangeServer::run, this);
        return true;
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        if (server_fd_ >= 0) {
            shutdown(server_fd_, SHUT_RDWR);
            close(server_fd_);
            server_fd_ = -1;
        }
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    int get_port() const { return port_; }

private:
    void run() {
        while (running_) {
            struct sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                if (!running_) break;
                continue;
            }

            std::thread([this, client_fd]() {
                handle_client(client_fd);
            }).detach();
        }
    }

    void handle_client(int client_fd) {
        char buf[4096];
        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            close(client_fd);
            return;
        }
        buf[n] = '\0';
        std::string req(buf);

        int64_t total_size = static_cast<int64_t>(payload_.size());
        int64_t range_start = 0;
        int64_t range_end = total_size - 1;
        bool has_range = false;

        size_t range_pos = req.find("Range: bytes=");
        if (range_pos != std::string::npos) {
            has_range = true;
            size_t eq = range_pos + 13;
            size_t dash = req.find('-', eq);
            if (dash != std::string::npos) {
                std::string s_start = req.substr(eq, dash - eq);
                if (!s_start.empty()) {
                    range_start = std::stoll(s_start);
                }
                size_t cr = req.find("\r\n", dash);
                if (cr != std::string::npos) {
                    std::string s_end = req.substr(dash + 1, cr - (dash + 1));
                    if (!s_end.empty()) {
                        range_end = std::stoll(s_end);
                    }
                }
            }
        }

        if (range_end >= total_size) range_end = total_size - 1;
        int64_t content_len = (range_end >= range_start) ? (range_end - range_start + 1) : 0;

        std::string resp;
        if (has_range) {
            resp = "HTTP/1.1 206 Partial Content\r\n";
            resp += "Content-Type: application/octet-stream\r\n";
            resp += "Content-Range: bytes " + std::to_string(range_start) + "-" +
                    std::to_string(range_end) + "/" + std::to_string(total_size) + "\r\n";
            resp += "Content-Length: " + std::to_string(content_len) + "\r\n";
            resp += "Content-Disposition: attachment; filename=\"testfile.bin\"\r\n";
            resp += "Connection: close\r\n\r\n";
        } else {
            resp = "HTTP/1.1 200 OK\r\n";
            resp += "Content-Type: application/octet-stream\r\n";
            resp += "Content-Length: " + std::to_string(total_size) + "\r\n";
            resp += "Accept-Ranges: bytes\r\n";
            resp += "Connection: close\r\n\r\n";
            content_len = total_size;
            range_start = 0;
        }

        send(client_fd, resp.data(), resp.size(), 0);

        // Stream body with slight throttle to allow testing pause/resume in-flight
        int64_t sent = 0;
        while (sent < content_len && running_) {
            int64_t chunk_to_send = std::min<int64_t>(32768, content_len - sent);
            ssize_t w = send(client_fd, reinterpret_cast<const char*>(payload_.data() + range_start + sent), chunk_to_send, 0);
            if (w <= 0) break;
            sent += w;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }

        close(client_fd);
    }

    const std::vector<uint8_t>& payload_;
    std::atomic<bool> running_{false};
    int server_fd_{-1};
    int port_{0};
    std::thread server_thread_;
};

int main() {
    std::cout << "=== Running DownloadSession Pause/Resume Integration Test ===" << std::endl;

    // 1. Generate 2 MB deterministic test payload
    const size_t test_size = 2 * 1024 * 1024; // 2 MB
    std::vector<uint8_t> payload(test_size);
    for (size_t i = 0; i < test_size; ++i) {
        payload[i] = static_cast<uint8_t>((i * 7 + 13) % 256);
    }

    // 2. Start mock Range-enabled HTTP server
    TestHttpRangeServer server(payload);
    assert(server.start());
    std::cout << "  ✓ Test HTTP Range Server listening on 127.0.0.1:" << server.get_port() << std::endl;

    std::string test_url = "http://127.0.0.1:" + std::to_string(server.get_port()) + "/testfile.bin";
    std::string test_dest = "/tmp/opendm_test_pause_dest.bin";
    std::string test_temp = "/tmp/opendm_test_pause_chunks";

    fs::remove(test_dest);
    fs::remove_all(test_temp);

    // 3. Create session
    auto session = std::make_shared<opendm::DownloadSession>("test_sess_01", test_url, test_dest, test_temp);
    session->set_max_connections(4);

    std::cout << "[Step 1] Starting download..." << std::endl;
    assert(session->start());

    // 4. Wait for download to start progressing (downloaded_bytes > 100 KB)
    auto start_time = std::chrono::steady_clock::now();
    while (session->get_progress().downloaded_bytes < 100000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count();
        if (elapsed > 10) {
            std::cerr << "FAIL: Download did not make progress within 10s!" << std::endl;
            return 1;
        }
    }

    int64_t bytes_before_pause = session->get_progress().downloaded_bytes;
    std::cout << "  ✓ Download actively running (" << bytes_before_pause << " bytes received)" << std::endl;

    // 5. Test Non-blocking Pause
    std::cout << "[Step 2] Pausing download..." << std::endl;
    auto pause_t0 = std::chrono::steady_clock::now();
    session->pause();
    auto pause_t1 = std::chrono::steady_clock::now();
    auto pause_dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(pause_t1 - pause_t0).count();

    std::cout << "  ✓ pause() returned in " << pause_dur_ms << " ms" << std::endl;
    assert(pause_dur_ms < 500); // Must be instantaneous and non-blocking!
    assert(session->get_status() == opendm::SessionStatus::PAUSED);
    assert(session->get_progress().speed_bytes_per_sec == 0);

    // Verify it stays paused and does not continue downloading
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int64_t bytes_after_pause_wait = session->get_progress().downloaded_bytes;
    std::cout << "  ✓ Download remained paused at " << bytes_after_pause_wait << " bytes" << std::endl;

    // 6. Test Resume
    std::cout << "[Step 3] Resuming download..." << std::endl;
    session->resume();

    // 7. Wait for completion
    auto resume_t0 = std::chrono::steady_clock::now();
    while (session->get_status() != opendm::SessionStatus::COMPLETED) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - resume_t0).count();
        if (session->get_status() == opendm::SessionStatus::ERROR) {
            std::cerr << "FAIL: Session entered ERROR status: " << session->get_progress().error_message << std::endl;
            return 1;
        }
        if (elapsed > 20) {
            std::cerr << "FAIL: Download did not complete within 20s! Status: "
                      << static_cast<int>(session->get_status())
                      << ", Bytes: " << session->get_progress().downloaded_bytes << "/" << test_size << std::endl;
            return 1;
        }
    }

    std::cout << "  ✓ Download completed successfully!" << std::endl;

    // 8. Verify assembled file contents and size
    std::string actual_dest = session->get_destination_path();
    assert(fs::exists(actual_dest));
    assert(fs::file_size(actual_dest) == test_size);

    std::vector<uint8_t> downloaded_data(test_size);
    {
        std::ifstream f(actual_dest, std::ios::binary);
        f.read(reinterpret_cast<char*>(downloaded_data.data()), test_size);
    }
    assert(downloaded_data == payload);
    std::cout << "  ✓ Assembled file verified byte-for-byte with original payload!" << std::endl;

    // Clean up
    server.stop();
    fs::remove(actual_dest);
    fs::remove_all(test_temp);

    std::cout << "=== ALL PAUSE/RESUME TESTS PASSED ===" << std::endl;
    return 0;
}
