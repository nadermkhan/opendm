#pragma once

#include "chunk_manager.hpp"
#include "speed_limiter.hpp"
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include <map>

namespace opendm {

enum class SessionStatus {
    IDLE,
    PROBING,
    DOWNLOADING,
    ASSEMBLING,
    PAUSED,
    COMPLETED,
    ERROR
};

struct DownloadProgress {
    SessionStatus status{SessionStatus::IDLE};
    int64_t total_size{0};
    int64_t downloaded_bytes{0};
    double progress_fraction{0.0};
    uint64_t speed_bytes_per_sec{0};
    int64_t eta_seconds{0};
    size_t active_connections{0};
    std::string error_message;
    std::vector<Chunk> chunks;
};

using ProgressCallback = std::function<void(const DownloadProgress&)>;

class DownloadSession : public std::enable_shared_from_this<DownloadSession> {
public:
    DownloadSession(const std::string& id, const std::string& url,
                    const std::string& dest_path, const std::string& temp_dir = "");
    ~DownloadSession();

    // Configuration
    void set_max_connections(size_t max_conns);
    size_t get_max_connections() const;

    void set_header(const std::string& name, const std::string& value);
    void set_speed_limit(uint64_t bytes_per_sec);
    void enable_speed_limit(bool enable);

    // Lifecycle
    bool start();
    void pause();
    void resume();
    void cancel();

    // Query status
    std::string get_id() const { return id_; }
    std::string get_url() const { return url_; }
    std::string get_destination_path() const { return destination_path_; }
    std::string get_filename() const { return filename_; }
    SessionStatus get_status() const { return status_.load(); }
    DownloadProgress get_progress() const;

    // Listeners
    void set_progress_callback(ProgressCallback cb);

    // State persistence
    std::string serialize() const;
    static std::shared_ptr<DownloadSession> deserialize(const std::string& json_str);

    // Progress & Throttling
    void set_chunk_progress(int chunk_id, int64_t bytes, uint64_t speed);
    void apply_throttle(size_t bytes);

private:
    void run_session();
    bool probe_resource();
    void download_chunk_worker(int chunk_id);
    void dynamic_split_loop();
    void assemble_file();
    void update_speed_and_eta();

    std::string id_;
    std::string url_;
    std::string destination_path_;
    std::string temp_dir_;
    std::string filename_;
    std::map<std::string, std::string> custom_headers_;

    std::atomic<SessionStatus> status_{SessionStatus::IDLE};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> pause_requested_{false};

    size_t max_connections_{8};
    bool supports_ranges_{false};
    int64_t total_size_{-1};
    std::string etag_;
    std::string last_modified_;

    ChunkManager chunk_manager_;
    SpeedLimiter speed_limiter_;

    mutable std::mutex workers_mutex_;
    std::vector<std::thread> worker_threads_;
    std::thread coordinator_thread_;

    mutable std::mutex callback_mutex_;
    ProgressCallback progress_callback_;

    // Speed calculation
    std::atomic<uint64_t> current_speed_{0};
    std::atomic<int64_t> eta_seconds_{0};
    std::string last_error_;
};

} // namespace opendm
