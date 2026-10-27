#pragma once

#include "download_session.hpp"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>

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
    std::string get_name() const { return name_; }
    void set_name(const std::string& name) { name_ = name; }

    void set_max_concurrent(size_t max_concurrent);
    size_t get_max_concurrent() const;

    // Queue items
    void add_session(std::shared_ptr<DownloadSession> session, bool start_now = false);
    bool remove_session(const std::string& session_id);
    std::shared_ptr<DownloadSession> get_session(const std::string& session_id) const;
    std::vector<std::shared_ptr<DownloadSession>> get_all_sessions() const;

    void move_item_up(const std::string& session_id);
    void move_item_down(const std::string& session_id);

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

    std::string id_;
    std::string name_;
    size_t max_concurrent_{2};

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<DownloadSession>> items_;

    std::atomic<QueueStatus> status_{QueueStatus::STOPPED};
    std::atomic<bool> loop_running_{false};
    std::thread queue_thread_;

    SchedulerConfig scheduler_;
    QueueUpdatedCallback update_callback_;
};

// Global queue and category coordinator
class QueueManager {
public:
    static QueueManager& instance();

    DownloadQueue& get_main_queue();
    std::shared_ptr<DownloadQueue> create_queue(const std::string& name, size_t max_concurrent = 2);
    std::vector<std::shared_ptr<DownloadQueue>> get_all_queues() const;

    // Helper: auto-detect category based on filename extension
    static std::string detect_category(const std::string& filename);

    // Find session across any queue
    std::shared_ptr<DownloadSession> find_session(const std::string& session_id) const;

    // Global speed limiter control
    void set_global_speed_limit(uint64_t bytes_per_sec);
    void enable_global_speed_limit(bool enable);
    bool is_global_speed_limit_enabled() const;
    uint64_t get_global_speed_limit() const;

private:
    QueueManager();
    ~QueueManager() = default;

    mutable std::mutex mutex_;
    std::shared_ptr<DownloadQueue> main_queue_;
    std::vector<std::shared_ptr<DownloadQueue>> queues_;

    bool global_limit_enabled_{false};
    uint64_t global_limit_bytes_per_sec_{0};
};

} // namespace opendm
