#pragma once

#include "download_session.hpp"
#include "speed_limiter.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace opendm {

enum class QueueStatus {
    STOPPED,
    RUNNING,
    PAUSED
};

struct SchedulerConfig {
    bool enabled{false};
    int start_hour{-1};   // 0-23, -1 to disable
    int start_minute{-1}; // 0-59
    int stop_hour{-1};    // 0-23, -1 to disable
    int stop_minute{-1};  // 0-59
    bool shutdown_on_completion{false};
    std::string post_download_command;
};

class DownloadQueue {
public:
    DownloadQueue(const std::string& id, const std::string& name, size_t max_concurrent = 2);
    ~DownloadQueue();

    std::string get_id() const { return id_; }
    std::string get_name() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return name_;
    }
    void set_name(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        name_ = name;
    }

    void set_max_concurrent(size_t max_concurrent);
    size_t get_max_concurrent() const;

    // Queue items
    void add_session(std::shared_ptr<DownloadSession> session, bool start_now = false, bool persist = true);
    bool remove_session(const std::string& session_id, bool persist = true);
    std::shared_ptr<DownloadSession> get_session(const std::string& session_id) const;
    std::vector<std::shared_ptr<DownloadSession>> get_all_sessions() const;

    void move_item_up(const std::string& session_id);
    void move_item_down(const std::string& session_id);
    void clear();

    // Queue control
    void start_queue();
    void stop_queue();
    bool is_running() const;
    QueueStatus get_status() const;

    // Scheduler
    void set_scheduler(const SchedulerConfig& config);
    SchedulerConfig get_scheduler() const;

    // State notification callback
    using QueueUpdatedCallback = std::function<void()>;
    void set_update_callback(QueueUpdatedCallback cb);

private:
    void queue_scheduler_loop();
    void process_pending_items();
    void check_schedule_triggers();
    void fire_update_callback();

    std::string id_;
    std::string name_;
    size_t max_concurrent_{2};

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<DownloadSession>> items_;

    std::atomic<QueueStatus> status_{QueueStatus::STOPPED};
    std::atomic<bool> loop_running_{false};
    std::thread queue_thread_;

    SchedulerConfig scheduler_;
    int last_start_trigger_key_{-1};
    int last_stop_trigger_key_{-1};
    QueueUpdatedCallback update_callback_;
};

// Global queue and category coordinator
class QueueManager {
public:
    static QueueManager& instance();

    DownloadQueue& get_main_queue();
    std::shared_ptr<DownloadQueue> create_queue(const std::string& name, size_t max_concurrent = 2);
    std::vector<std::shared_ptr<DownloadQueue>> get_all_queues() const;

    static std::string detect_category(const std::string& filename);

    std::shared_ptr<DownloadSession> find_session(const std::string& session_id) const;

    // Global speed limiter control
    void set_global_speed_limit(uint64_t bytes_per_sec);
    void enable_global_speed_limit(bool enable);
    bool is_global_speed_limit_enabled() const;
    uint64_t get_global_speed_limit() const;
    SpeedLimiter* get_global_speed_limiter() { return &global_speed_limiter_; }

    // Default max connections per download session (1..32, default 16)
    void set_default_max_connections(size_t max_conns);
    size_t get_default_max_connections() const;

    // Default save directory (UTF-8)
    void set_default_save_dir(const std::string& dir);
    std::string get_default_save_dir() const;

    // State persistence across application restarts
    static std::string get_default_storage_path();
    void set_storage_path(const std::string& path);
    std::string get_storage_path() const;
    bool save_to_file(const std::string& filepath = "") const;
    bool load_from_file(const std::string& filepath = "");
    void clear_all();

    void mark_dirty();
    void save_if_dirty();

    // Stops & joins all active sessions, pending teardowns/scans, and persists final state on exit
    void shutdown_all_sessions();

    // Background teardown & scan tracking
    void cancel_session_async(std::shared_ptr<DownloadSession> session);
    void track_background_task(std::future<void> fut);
    void wait_for_pending_teardowns();

private:
    QueueManager();
    ~QueueManager() = default;

    mutable std::mutex mutex_;
    std::shared_ptr<DownloadQueue> main_queue_;
    std::vector<std::shared_ptr<DownloadQueue>> queues_;
    std::string storage_path_;
    std::string default_save_dir_;

    SpeedLimiter global_speed_limiter_;
    bool global_limit_enabled_{false};
    uint64_t global_limit_bytes_per_sec_{0};
    std::atomic<size_t> default_max_connections_{16};
    mutable std::atomic<bool> dirty_{false};

    std::mutex teardown_mutex_;
    std::vector<std::future<void>> teardowns_;
};

} // namespace opendm
