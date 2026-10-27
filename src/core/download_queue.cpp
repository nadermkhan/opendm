#include "download_queue.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <cstdlib>

namespace opendm {

DownloadQueue::DownloadQueue(const std::string& id, const std::string& name, size_t max_concurrent)
    : id_(id), name_(name), max_concurrent_(max_concurrent) {
    loop_running_ = true;
    queue_thread_ = std::thread(&DownloadQueue::queue_scheduler_loop, this);
}

DownloadQueue::~DownloadQueue() {
    loop_running_ = false;
    if (queue_thread_.joinable()) {
        queue_thread_.join();
    }
}

void DownloadQueue::set_max_concurrent(size_t max_concurrent) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_concurrent_ = std::clamp<size_t>(max_concurrent, 1, 16);
}

size_t DownloadQueue::get_max_concurrent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_concurrent_;
}

void DownloadQueue::add_session(std::shared_ptr<DownloadSession> session, bool start_now) {
    if (!session) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Check if already in queue
        auto it = std::find_if(items_.begin(), items_.end(),
            [&session](const std::shared_ptr<DownloadSession>& s) {
                return s->get_id() == session->get_id();
            });
        if (it == items_.end()) {
            items_.push_back(session);
        }
    }

    if (start_now) {
        session->start();
    }

    if (update_callback_) {
        update_callback_();
    }
}

bool DownloadQueue::remove_session(const std::string& session_id) {
    std::shared_ptr<DownloadSession> to_cancel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(items_.begin(), items_.end(),
            [&session_id](const std::shared_ptr<DownloadSession>& s) {
                return s->get_id() == session_id;
            });
        if (it == items_.end()) {
            return false;
        }
        to_cancel = *it;
        items_.erase(it);
    }

    if (to_cancel) {
        to_cancel->cancel();
    }

    if (update_callback_) {
        update_callback_();
    }
    return true;
}

std::shared_ptr<DownloadSession> DownloadQueue::get_session(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : items_) {
        if (item->get_id() == session_id) {
            return item;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<DownloadSession>> DownloadQueue::get_all_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}

void DownloadQueue::move_item_up(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 1; i < items_.size(); ++i) {
        if (items_[i]->get_id() == session_id) {
            std::swap(items_[i], items_[i - 1]);
            break;
        }
    }
    if (update_callback_) {
        update_callback_();
    }
}

void DownloadQueue::move_item_down(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i + 1 < items_.size(); ++i) {
        if (items_[i]->get_id() == session_id) {
            std::swap(items_[i], items_[i + 1]);
            break;
        }
    }
    if (update_callback_) {
        update_callback_();
    }
}

void DownloadQueue::start_queue() {
    status_ = QueueStatus::RUNNING;
    if (update_callback_) {
        update_callback_();
    }
}

void DownloadQueue::stop_queue() {
    status_ = QueueStatus::STOPPED;

    std::vector<std::shared_ptr<DownloadSession>> active_sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& s : items_) {
            if (s->get_status() == SessionStatus::DOWNLOADING ||
                s->get_status() == SessionStatus::PROBING) {
                active_sessions.push_back(s);
            }
        }
    }

    for (auto& s : active_sessions) {
        s->pause();
    }

    if (update_callback_) {
        update_callback_();
    }
}

bool DownloadQueue::is_running() const {
    return status_ == QueueStatus::RUNNING;
}

QueueStatus DownloadQueue::get_status() const {
    return status_.load();
}

void DownloadQueue::set_scheduler(const SchedulerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    scheduler_ = config;
}

SchedulerConfig DownloadQueue::get_scheduler() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return scheduler_;
}

void DownloadQueue::set_update_callback(QueueUpdatedCallback cb) {
    update_callback_ = std::move(cb);
}

void DownloadQueue::check_schedule_triggers() {
    SchedulerConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = scheduler_;
    }

    if (!cfg.enabled) return;

    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm;
    localtime_r(&now_c, &now_tm);

    // Check Start Trigger
    if (cfg.start_hour >= 0 && cfg.start_minute >= 0) {
        if (now_tm.tm_hour == cfg.start_hour && now_tm.tm_min == cfg.start_minute && now_tm.tm_sec <= 2) {
            if (status_ != QueueStatus::RUNNING) {
                start_queue();
            }
        }
    }

    // Check Stop Trigger
    if (cfg.stop_hour >= 0 && cfg.stop_minute >= 0) {
        if (now_tm.tm_hour == cfg.stop_hour && now_tm.tm_min == cfg.stop_minute && now_tm.tm_sec <= 2) {
            if (status_ == QueueStatus::RUNNING) {
                stop_queue();
            }
        }
    }
}

void DownloadQueue::process_pending_items() {
    if (status_ != QueueStatus::RUNNING) {
        return;
    }

    std::vector<std::shared_ptr<DownloadSession>> snapshot;
    size_t max_conns = 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = items_;
        max_conns = max_concurrent_;
    }

    size_t active_count = 0;
    std::shared_ptr<DownloadSession> next_candidate = nullptr;
    bool all_finished = true;

    for (const auto& s : snapshot) {
        auto st = s->get_status();
        if (st == SessionStatus::DOWNLOADING || st == SessionStatus::PROBING || st == SessionStatus::ASSEMBLING) {
            active_count++;
            all_finished = false;
        } else if (st == SessionStatus::IDLE) {
            all_finished = false;
            if (!next_candidate && active_count < max_conns) {
                next_candidate = s;
            }
        } else if (st == SessionStatus::PAUSED) {
            // Paused downloads are explicitly held; do not auto-start
            all_finished = false;
        }
    }

    if (next_candidate && active_count < max_conns) {
        next_candidate->start();
    }

    // Handle post-completion action if everything finished while queue was running
    if (all_finished && !snapshot.empty() && status_ == QueueStatus::RUNNING) {
        SchedulerConfig cfg;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cfg = scheduler_;
        }

        if (cfg.enabled) {
            if (!cfg.post_download_command.empty()) {
                std::system(cfg.post_download_command.c_str());
            }
            if (cfg.shutdown_on_completion) {
                std::system("shutdown -h now");
            }
        }
    }
}

void DownloadQueue::queue_scheduler_loop() {
    while (loop_running_) {
        check_schedule_triggers();
        process_pending_items();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// QueueManager singleton
QueueManager& QueueManager::instance() {
    static QueueManager mgr;
    return mgr;
}

QueueManager::QueueManager() {
    main_queue_ = std::make_shared<DownloadQueue>("main_queue", "Main Download Queue", 2);
    queues_.push_back(main_queue_);
}

DownloadQueue& QueueManager::get_main_queue() {
    return *main_queue_;
}

std::shared_ptr<DownloadQueue> QueueManager::create_queue(const std::string& name, size_t max_concurrent) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "queue_" + std::to_string(queues_.size() + 1);
    auto q = std::make_shared<DownloadQueue>(id, name, max_concurrent);
    queues_.push_back(q);
    return q;
}

std::vector<std::shared_ptr<DownloadQueue>> QueueManager::get_all_queues() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queues_;
}

std::shared_ptr<DownloadSession> QueueManager::find_session(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& q : queues_) {
        auto s = q->get_session(session_id);
        if (s) return s;
    }
    return nullptr;
}

std::string QueueManager::detect_category(const std::string& filename) {
    size_t dot_pos = filename.find_last_of('.');
    if (dot_pos == std::string::npos) {
        return "General";
    }

    std::string ext = filename.substr(dot_pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Compressed
    if (ext == "zip" || ext == "rar" || ext == "7z" || ext == "tar" ||
        ext == "gz" || ext == "bz2" || ext == "xz" || ext == "tgz" ||
        ext == "tbz2" || ext == "iso") {
        return "Compressed";
    }

    // Documents
    if (ext == "pdf" || ext == "doc" || ext == "docx" || ext == "xls" ||
        ext == "xlsx" || ext == "ppt" || ext == "pptx" || ext == "txt" ||
        ext == "odt" || ext == "ods" || ext == "odp" || ext == "rtf" ||
        ext == "epub" || ext == "md") {
        return "Documents";
    }

    // Music
    if (ext == "mp3" || ext == "flac" || ext == "wav" || ext == "aac" ||
        ext == "ogg" || ext == "m4a" || ext == "wma" || ext == "opus") {
        return "Music";
    }

    // Programs
    if (ext == "exe" || ext == "msi" || ext == "deb" || ext == "rpm" ||
        ext == "appimage" || ext == "dmg" || ext == "pkg" || ext == "sh" ||
        ext == "bin") {
        return "Programs";
    }

    // Video
    if (ext == "mp4" || ext == "mkv" || ext == "avi" || ext == "webm" ||
        ext == "flv" || ext == "mov" || ext == "wmv" || ext == "ts" ||
        ext == "m4v" || ext == "mpg" || ext == "mpeg" || ext == "3gp") {
        return "Video";
    }

    return "General";
}

void QueueManager::set_global_speed_limit(uint64_t bytes_per_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    global_limit_bytes_per_sec_ = bytes_per_sec;
    for (auto& q : queues_) {
        for (auto& s : q->get_all_sessions()) {
            s->set_speed_limit(bytes_per_sec);
        }
    }
}

void QueueManager::enable_global_speed_limit(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    global_limit_enabled_ = enable;
    for (auto& q : queues_) {
        for (auto& s : q->get_all_sessions()) {
            s->enable_speed_limit(enable);
        }
    }
}

bool QueueManager::is_global_speed_limit_enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return global_limit_enabled_;
}

uint64_t QueueManager::get_global_speed_limit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return global_limit_bytes_per_sec_;
}

} // namespace opendm
