#include "download_queue.hpp"
#include "win32_unicode.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace opendm {

namespace {

// Targeted repair helper for legacy downloads.json files written with unescaped Windows backslashes
// and unescaped HTTP ETag quotes ("etag": ""abc"").
std::string sanitize_legacy_downloads_json(const std::string& raw) {
    std::istringstream in(raw);
    std::ostringstream out;
    std::string line;
    bool first = true;

    while (std::getline(in, line)) {
        if (!first) out << "\n";
        first = false;

        // Trim leading whitespace to inspect "key": "value" lines
        size_t indent_end = line.find_first_not_of(" \t\r");
        if (indent_end == std::string::npos) {
            out << line;
            continue;
        }

        size_t colon = line.find("\": \"", indent_end);
        if (colon != std::string::npos && line[indent_end] == '"') {
            std::string key = line.substr(indent_end + 1, colon - (indent_end + 1));
            size_t val_start = colon + 4;
            bool has_comma = false;
            size_t rtrim = line.find_last_not_of(" \t\r");
            if (rtrim != std::string::npos && rtrim > val_start) {
                if (line[rtrim] == ',' && line[rtrim - 1] == '"') {
                    has_comma = true;
                    rtrim--;
                }
                if (line[rtrim] == '"' && rtrim >= val_start) {
                    std::string val = line.substr(val_start, rtrim - val_start);
                    // Re-encode safely using nlohmann::json
                    nlohmann::json jv = val;
                    out << line.substr(0, indent_end) << "\"" << key << "\": " << jv.dump();
                    if (has_comma) out << ",";
                    continue;
                }
            }
        }
        out << line;
    }
    return out.str();
}

} // namespace

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

void DownloadQueue::fire_update_callback() {
    QueueUpdatedCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = update_callback_;
    }
    if (cb) {
        cb();
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

void DownloadQueue::add_session(std::shared_ptr<DownloadSession> session, bool start_now, bool persist) {
    if (!session) return;

    session->set_global_speed_limiter(QueueManager::instance().get_global_speed_limiter());
    session->set_dirty_callback([]() {
        QueueManager::instance().mark_dirty();
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
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

    if (persist) {
        QueueManager::instance().save_to_file();
    }

    fire_update_callback();
}

bool DownloadQueue::remove_session(const std::string& session_id, bool persist) {
    std::shared_ptr<DownloadSession> to_cancel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(items_.begin(), items_.end(),
            [&session_id](const std::shared_ptr<DownloadSession>& s) {
                return s && s->get_id() == session_id;
            });
        if (it == items_.end()) {
            return false;
        }
        to_cancel = *it;
        items_.erase(it);
    }

    if (to_cancel) {
        to_cancel->request_stop(true);
        QueueManager::instance().cancel_session_async(std::move(to_cancel));
    }

    if (persist) {
        QueueManager::instance().save_to_file();
    }

    fire_update_callback();
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 1; i < items_.size(); ++i) {
            if (items_[i]->get_id() == session_id) {
                std::swap(items_[i], items_[i - 1]);
                break;
            }
        }
    }
    fire_update_callback();
}

void DownloadQueue::move_item_down(const std::string& session_id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i + 1 < items_.size(); ++i) {
            if (items_[i]->get_id() == session_id) {
                std::swap(items_[i], items_[i + 1]);
                break;
            }
        }
    }
    fire_update_callback();
}

void DownloadQueue::clear() {
    std::vector<std::shared_ptr<DownloadSession>> to_cancel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        to_cancel.swap(items_);
    }
    for (auto& s : to_cancel) {
        if (s) {
            s->request_stop(true);
            QueueManager::instance().cancel_session_async(std::move(s));
        }
    }
    fire_update_callback();
}

void DownloadQueue::start_queue() {
    status_ = QueueStatus::RUNNING;
    fire_update_callback();
}

void DownloadQueue::stop_queue() {
    status_ = QueueStatus::STOPPED;

    std::vector<std::shared_ptr<DownloadSession>> active_sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& s : items_) {
            auto st = s->get_status();
            if (st == SessionStatus::DOWNLOADING || st == SessionStatus::PROBING) {
                active_sessions.push_back(s);
            }
        }
    }

    for (auto& s : active_sessions) {
        s->pause();
    }

    fire_update_callback();
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
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::tm now_tm{};
#ifdef _WIN32
    localtime_s(&now_tm, &now_c);
#else
    localtime_r(&now_c, &now_tm);
#endif

    const int minute_key = now_tm.tm_yday * 1440 + now_tm.tm_hour * 60 + now_tm.tm_min;

    if (cfg.start_hour >= 0 && cfg.start_minute >= 0) {
        if (now_tm.tm_hour == cfg.start_hour && now_tm.tm_min == cfg.start_minute &&
            last_start_trigger_key_ != minute_key) {
            last_start_trigger_key_ = minute_key;
            if (status_ != QueueStatus::RUNNING) {
                start_queue();
            }
        }
    }

    if (cfg.stop_hour >= 0 && cfg.stop_minute >= 0) {
        if (now_tm.tm_hour == cfg.stop_hour && now_tm.tm_min == cfg.stop_minute &&
            last_stop_trigger_key_ != minute_key) {
            last_stop_trigger_key_ = minute_key;
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
        } else if (st == SessionStatus::PAUSED || st == SessionStatus::ERROR) {
            all_finished = false;
        }
    }

    if (next_candidate && active_count < max_conns) {
        next_candidate->start();
    }

    if (all_finished && !snapshot.empty() && status_ == QueueStatus::RUNNING) {
        status_ = QueueStatus::STOPPED;
        SchedulerConfig cfg;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cfg = scheduler_;
        }

        if (cfg.enabled) {
            if (!cfg.post_download_command.empty()) {
                (void)std::system(cfg.post_download_command.c_str());
            }
            if (cfg.shutdown_on_completion) {
#ifdef _WIN32
                sys::run_hidden_process("shutdown.exe", {"/s", "/t", "5"});
#else
                (void)std::system("shutdown -h now");
#endif
            }
        }

        fire_update_callback();
    }
}

void DownloadQueue::queue_scheduler_loop() {
    while (loop_running_) {
        check_schedule_triggers();
        process_pending_items();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

QueueManager& QueueManager::instance() {
    static QueueManager mgr;
    return mgr;
}

QueueManager::QueueManager() {
    main_queue_ = std::make_shared<DownloadQueue>("main_queue", "Main Download Queue", 2);
    queues_.push_back(main_queue_);
    storage_path_ = get_default_storage_path();
    default_save_dir_ = sys::get_downloads_dir_utf8();
}

DownloadQueue& QueueManager::get_main_queue() {
    return *main_queue_;
}

void QueueManager::track_background_task(std::future<void> fut) {
    std::lock_guard<std::mutex> lock(teardown_mutex_);
    teardowns_.erase(std::remove_if(teardowns_.begin(), teardowns_.end(),
        [](std::future<void>& f) {
            return !f.valid() || f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }), teardowns_.end());
    teardowns_.push_back(std::move(fut));
}

void QueueManager::cancel_session_async(std::shared_ptr<DownloadSession> session) {
    if (!session) return;
    session->request_stop(false);

    try {
        track_background_task(std::async(std::launch::async, [s = std::move(session)]() mutable {
            try {
                s->cancel();
            } catch (...) {
            }
            s.reset();
        }));
    } catch (...) {
        if (session) session->cancel();
    }
}

void QueueManager::wait_for_pending_teardowns() {
    std::vector<std::future<void>> pending;
    {
        std::lock_guard<std::mutex> lock(teardown_mutex_);
        pending.swap(teardowns_);
    }
    for (auto& f : pending) {
        if (f.valid()) f.wait();
    }
}

void QueueManager::shutdown_all_sessions() {
    std::vector<std::shared_ptr<DownloadSession>> all_sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& q : queues_) {
            if (!q) continue;
            auto q_items = q->get_all_sessions();
            all_sessions.insert(all_sessions.end(), q_items.begin(), q_items.end());
        }
    }

    for (auto& s : all_sessions) {
        if (!s) continue;
        auto st = s->get_status();
        if (st == SessionStatus::DOWNLOADING || st == SessionStatus::PROBING || st == SessionStatus::ASSEMBLING) {
            s->pause();
        }
    }

    for (auto& s : all_sessions) {
        if (s) {
            s->wait_for_stop();
        }
    }

    wait_for_pending_teardowns();
    save_to_file();
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
    if (filename.empty() || filename == "download.bin") {
        return "General";
    }
    size_t dot_pos = filename.find_last_of('.');
    if (dot_pos == std::string::npos) {
        return "General";
    }

    std::string ext = sys::ascii_tolower(filename.substr(dot_pos + 1));

    if (ext == "zip" || ext == "rar" || ext == "7z" || ext == "tar" ||
        ext == "gz" || ext == "bz2" || ext == "xz" || ext == "tgz" ||
        ext == "tbz2" || ext == "iso") {
        return "Compressed";
    }

    if (ext == "pdf" || ext == "doc" || ext == "docx" || ext == "xls" ||
        ext == "xlsx" || ext == "ppt" || ext == "pptx" || ext == "txt" ||
        ext == "odt" || ext == "ods" || ext == "odp" || ext == "rtf" ||
        ext == "epub" || ext == "md") {
        return "Documents";
    }

    if (ext == "mp3" || ext == "flac" || ext == "wav" || ext == "aac" ||
        ext == "ogg" || ext == "m4a" || ext == "wma" || ext == "opus") {
        return "Music";
    }

    if (ext == "exe" || ext == "msi" || ext == "deb" || ext == "rpm" ||
        ext == "appimage" || ext == "dmg" || ext == "pkg" || ext == "sh" ||
        ext == "bin") {
        return "Programs";
    }

    if (ext == "mp4" || ext == "mkv" || ext == "avi" || ext == "webm" ||
        ext == "flv" || ext == "mov" || ext == "wmv" || ext == "ts" ||
        ext == "m4v" || ext == "mpg" || ext == "mpeg" || ext == "3gp") {
        return "Video";
    }

    return "General";
}

void QueueManager::set_global_speed_limit(uint64_t bytes_per_sec) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        global_limit_bytes_per_sec_ = bytes_per_sec;
    }
    global_speed_limiter_.set_limit_bytes_per_sec(bytes_per_sec);
}

void QueueManager::enable_global_speed_limit(bool enable) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        global_limit_enabled_ = enable;
    }
    global_speed_limiter_.set_enabled(enable);
}

bool QueueManager::is_global_speed_limit_enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return global_limit_enabled_;
}

uint64_t QueueManager::get_global_speed_limit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return global_limit_bytes_per_sec_;
}

void QueueManager::set_default_max_connections(size_t max_conns) {
    size_t clamped = std::clamp<size_t>(max_conns, 1, 32);
    default_max_connections_.store(clamped, std::memory_order_relaxed);
    std::vector<std::shared_ptr<DownloadQueue>> queues_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queues_copy = queues_;
    }
    for (auto& q : queues_copy) {
        if (!q) continue;
        for (auto& s : q->get_all_sessions()) {
            if (s) s->set_max_connections(clamped);
        }
    }
}

size_t QueueManager::get_default_max_connections() const {
    return default_max_connections_.load(std::memory_order_relaxed);
}

void QueueManager::set_default_save_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_save_dir_ = dir.empty() ? sys::get_downloads_dir_utf8() : dir;
}

std::string QueueManager::get_default_save_dir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return default_save_dir_.empty() ? sys::get_downloads_dir_utf8() : default_save_dir_;
}

std::string QueueManager::get_default_storage_path() {
    fs::path dir = sys::path_from_utf8(sys::get_appdata_dir_utf8());
    std::error_code ec;
    fs::create_directories(dir, ec);
    return sys::path_to_utf8(dir / "downloads.json");
}

void QueueManager::set_storage_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    storage_path_ = path;
}

std::string QueueManager::get_storage_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return storage_path_;
}

void QueueManager::clear_all() {
    std::vector<std::shared_ptr<DownloadQueue>> queues_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queues_copy = queues_;
    }
    for (auto& q : queues_copy) {
        if (q) q->clear();
    }
}

void QueueManager::mark_dirty() {
    dirty_.store(true, std::memory_order_relaxed);
}

void QueueManager::save_if_dirty() {
    if (dirty_.exchange(false, std::memory_order_relaxed)) {
        save_to_file();
    }
}

bool QueueManager::save_to_file(const std::string& filepath) const {
    std::string target;
    bool gl_enabled = false;
    uint64_t gl_bps = 0;
    std::string save_dir;
    std::vector<std::shared_ptr<DownloadQueue>> queues_copy;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        target = filepath.empty() ? (storage_path_.empty() ? get_default_storage_path() : storage_path_) : filepath;
        gl_enabled = global_limit_enabled_;
        gl_bps = global_limit_bytes_per_sec_;
        save_dir = default_save_dir_;
        queues_copy = queues_;
    }

    size_t max_conc = main_queue_ ? main_queue_->get_max_concurrent() : 2;

    // Serialize all sessions OUTSIDE mutex_ so slow JSON/disk work does not stall the UI
    nlohmann::json queues_arr = nlohmann::json::array();
    for (const auto& q : queues_copy) {
        if (!q) continue;
        nlohmann::json items_arr = nlohmann::json::array();
        for (const auto& s : q->get_all_sessions()) {
            if (!s) continue;
            try {
                items_arr.push_back(nlohmann::json::parse(s->serialize()));
            } catch (...) {}
        }
        queues_arr.push_back({
            {"name", q->get_name()},
            {"items", std::move(items_arr)}
        });
    }

    nlohmann::json root = {
        {"version", 1},
        {"default_max_connections", default_max_connections_.load(std::memory_order_relaxed)},
        {"settings", {
            {"default_max_connections", default_max_connections_.load(std::memory_order_relaxed)},
            {"max_concurrent_downloads", max_conc},
            {"global_limit_enabled", gl_enabled},
            {"global_limit_bytes_per_sec", gl_bps},
            {"default_save_dir", save_dir}
        }},
        {"queues", std::move(queues_arr)}
    };

    fs::path target_p = sys::path_from_utf8(target);
    std::error_code ec;
    if (target_p.has_parent_path()) {
        fs::create_directories(target_p.parent_path(), ec);
    }

    fs::path tmp_p = sys::path_from_utf8(target + ".tmp");
    std::ofstream out(tmp_p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << root.dump(2) << "\n";
    out.flush();
    bool write_ok = out.good();
    out.close();

    if (!write_ok) {
        fs::remove(tmp_p, ec);
        return false;
    }

    bool replaced = sys::atomic_replace_file(tmp_p, target_p, ec);
    if (!replaced) {
        fs::remove(tmp_p, ec);
    } else {
        dirty_.store(false, std::memory_order_relaxed);
    }
    return replaced;
}

bool QueueManager::load_from_file(const std::string& filepath) {
    std::string target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!filepath.empty()) {
            storage_path_ = filepath;
        }
        target = storage_path_.empty() ? get_default_storage_path() : storage_path_;
    }

    fs::path target_p = sys::path_from_utf8(target);
    std::error_code ec;
    if (!fs::exists(target_p, ec)) return true;

    std::ifstream in(target_p, std::ios::binary);
    if (!in.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    if (content.empty()) return true;

    nlohmann::json root;
    bool migrated = false;
    try {
        root = nlohmann::json::parse(content);
    } catch (...) {
        try {
            std::string repaired = sanitize_legacy_downloads_json(content);
            root = nlohmann::json::parse(repaired);
            migrated = true;
            std::error_code bak_ec;
            fs::copy_file(target_p, sys::path_from_utf8(target + ".bak"), fs::copy_options::overwrite_existing, bak_ec);
        } catch (...) {
            return false;
        }
    }

    if (!root.is_object()) return false;

    size_t dmc = root.value("default_max_connections", default_max_connections_.load(std::memory_order_relaxed));
    if (root.contains("settings") && root["settings"].is_object()) {
        const auto& s = root["settings"];
        dmc = s.value("default_max_connections", dmc);
        size_t mconc = s.value("max_concurrent_downloads", size_t{2});
        bool gl_en = s.value("global_limit_enabled", false);
        uint64_t gl_bps = s.value("global_limit_bytes_per_sec", uint64_t{0});
        std::string sdir = s.value("default_save_dir", std::string{});

        if (main_queue_) main_queue_->set_max_concurrent(mconc);
        enable_global_speed_limit(gl_en);
        set_global_speed_limit(gl_bps);
        if (!sdir.empty()) set_default_save_dir(sdir);
    }
    default_max_connections_.store(std::clamp<size_t>(dmc, 1, 32), std::memory_order_relaxed);

    if (root.contains("queues") && root["queues"].is_array()) {
        for (const auto& qobj : root["queues"]) {
            if (!qobj.is_object() || !qobj.contains("items") || !qobj["items"].is_array()) continue;
            for (const auto& item : qobj["items"]) {
                if (!item.is_object()) continue;
                auto session = DownloadSession::deserialize(item.dump());
                if (session) {
                    session->reset_interrupted_assembling();
                    if (!find_session(session->get_id())) {
                        main_queue_->add_session(session, false, false);
                    }
                }
            }
        }
    }

    if (migrated) {
        save_to_file(target);
    }

    return true;
}

} // namespace opendm
