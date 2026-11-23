#include "download_session.hpp"
#include "curl_utils.hpp"
#include "download_queue.hpp"
#include "media_muxer.hpp"
#include "post_download.hpp"
#include "win32_unicode.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#ifdef ERROR
#undef ERROR
#endif
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace opendm {

namespace {

struct AttemptHeaderState {
    long last_status{0};
    bool has_content_range{false};
    int64_t range_start{-1};
    int64_t range_end{-1};
    int64_t range_total{-1};
    std::map<std::string, std::string> headers;

    void reset_for_new_response() {
        has_content_range = false;
        range_start = -1;
        range_end = -1;
        range_total = -1;
        headers.clear();
    }
};

void parse_content_range_value(const std::string& val, AttemptHeaderState& st) {
    // Expected: "bytes <start>-<end>/<total>" or "bytes */<total>"
    std::string lower = sys::ascii_tolower(val);
    size_t bpos = lower.find("bytes");
    if (bpos == std::string::npos) return;
    size_t p = bpos + 5;
    while (p < lower.size() && (lower[p] == ' ' || lower[p] == '\t')) p++;
    size_t slash = lower.find('/', p);
    if (slash == std::string::npos) return;

    std::string span = lower.substr(p, slash - p);
    std::string tot_str = lower.substr(slash + 1);
    while (!tot_str.empty() && (tot_str.back() == ' ' || tot_str.back() == '\t' || tot_str.back() == '\r' || tot_str.back() == '\n')) {
        tot_str.pop_back();
    }

    st.has_content_range = true;
    if (tot_str != "*") {
        try { st.range_total = std::stoll(tot_str); } catch (...) {}
    }
    size_t dash = span.find('-');
    if (dash != std::string::npos) {
        try {
            st.range_start = std::stoll(span.substr(0, dash));
            st.range_end = std::stoll(span.substr(dash + 1));
        } catch (...) {
            st.range_start = -1;
            st.range_end = -1;
        }
    }
}

size_t attempt_header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total_size = size * nitems;
    auto* st = static_cast<AttemptHeaderState*>(userdata);
    if (!st) return total_size;

    std::string line(buffer, total_size);
    if (line.rfind("HTTP/", 0) == 0) {
        st->reset_for_new_response();
        size_t sp = line.find(' ');
        if (sp != std::string::npos) {
            try { st->last_status = std::stol(line.substr(sp + 1)); } catch (...) {}
        }
        return total_size;
    }

    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = sys::ascii_tolower(line.substr(0, colon));
        std::string val = line.substr(colon + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) val.pop_back();
        st->headers[key] = val;
        if (key == "content-range") {
            parse_content_range_value(val, *st);
        }
    }
    return total_size;
}

struct WorkerData {
    DownloadSession* session{nullptr};
    CURL* curl{nullptr};
    int chunk_id{0};
    int64_t expected_start_offset{0};
    int64_t expected_chunk_end{0};
    int64_t expected_total_size{-1};
    bool expect_ranged{false};
    bool sent_if_range{false};
    AttemptHeaderState hdr_state;
    bool attempt_validated{false};
    bool response_rejected{false};
    bool if_range_mismatch{false};
    long rejected_http_code{0};
    std::ofstream file;
    int64_t bytes_received{0};
    std::chrono::steady_clock::time_point last_speed_time;
    int64_t bytes_since_last_speed{0};
    uint64_t current_speed{0};
};

int xferinfo_callback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* data = static_cast<WorkerData*>(clientp);
    if (!data || !data->session) return 1;

    if (data->session->is_stop_requested() || data->session->is_pause_requested() ||
        data->session->get_status() == SessionStatus::PAUSED ||
        data->session->get_status() == SessionStatus::IDLE) {
        return 1;
    }
    return 0;
}

int probe_xferinfo_callback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* session = static_cast<DownloadSession*>(clientp);
    if (!session) return 1;

    if (session->is_stop_requested() || session->is_pause_requested() ||
        session->get_status() == SessionStatus::PAUSED ||
        session->get_status() == SessionStatus::IDLE) {
        return 1;
    }
    return 0;
}

struct ProbeWriteState {
    size_t bytes_seen{0};
};

size_t probe_write_callback(char*, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* st = static_cast<ProbeWriteState*>(userdata);
    if (!st) return 0;
    st->bytes_seen += total;
    if (st->bytes_seen > 64) {
        return 0;
    }
    return total;
}

void curl_share_lock_cb(CURL*, curl_lock_data data, curl_lock_access, void* userptr) {
    auto* mutexes = static_cast<std::array<std::mutex, 8>*>(userptr);
    (*mutexes)[static_cast<size_t>(data) % 8].lock();
}

void curl_share_unlock_cb(CURL*, curl_lock_data data, void* userptr) {
    auto* mutexes = static_cast<std::array<std::mutex, 8>*>(userptr);
    (*mutexes)[static_cast<size_t>(data) % 8].unlock();
}

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* data = static_cast<WorkerData*>(userdata);
    size_t incoming_bytes = size * nmemb;
    if (!data || !data->session) return 0;

    if (data->session->is_stop_requested() || data->session->is_pause_requested() ||
        data->session->get_status() == SessionStatus::PAUSED ||
        data->session->get_status() == SessionStatus::IDLE) {
        return 0;
    }

    if (incoming_bytes == 0) return 0;

    // Validate response status & Content-Range BEFORE committing a single byte to disk
    if (!data->attempt_validated) {
        long http_code = data->hdr_state.last_status;
        if (http_code == 0 && data->curl) {
            curl_easy_getinfo(data->curl, CURLINFO_RESPONSE_CODE, &http_code);
        }

        if (http_code >= 400 || (http_code > 0 && http_code != 200 && http_code != 206)) {
            data->response_rejected = true;
            data->rejected_http_code = http_code;
            return 0;
        }

        if (data->sent_if_range && http_code == 200) {
            // Server indicates If-Range precondition failed (resource changed)
            data->if_range_mismatch = true;
            data->response_rejected = true;
            data->rejected_http_code = http_code;
            return 0;
        }

        if (data->expect_ranged && data->expected_chunk_end >= 0) {
            if (http_code == 200 && data->expected_start_offset > 0) {
                data->response_rejected = true;
                data->rejected_http_code = http_code;
                return 0;
            }
            if (http_code == 206 && data->hdr_state.has_content_range) {
                if (data->hdr_state.range_start >= 0 &&
                    data->hdr_state.range_start != data->expected_start_offset) {
                    data->response_rejected = true;
                    data->rejected_http_code = http_code;
                    return 0;
                }
                if (data->hdr_state.range_total > 0 && data->expected_total_size > 0 &&
                    data->hdr_state.range_total != data->expected_total_size) {
                    data->if_range_mismatch = true;
                    data->response_rejected = true;
                    data->rejected_http_code = http_code;
                    return 0;
                }
            }
        }
        data->attempt_validated = true;
    }

    // Calculate speed every 250ms
    data->bytes_since_last_speed += static_cast<int64_t>(incoming_bytes);
    auto now = std::chrono::steady_clock::now();
    auto diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - data->last_speed_time).count();
    if (diff_ms >= 250) {
        data->current_speed = static_cast<uint64_t>((data->bytes_since_last_speed * 1000) / diff_ms);
        data->bytes_since_last_speed = 0;
        data->last_speed_time = now;
    } else if (data->current_speed == 0 && diff_ms > 0 && data->bytes_since_last_speed > 0) {
        data->current_speed = static_cast<uint64_t>((data->bytes_since_last_speed * 1000) / diff_ms);
    }

    int64_t pre_write_received = data->bytes_received;
    bool reached_boundary = false;
    size_t bytes_to_write = data->session->clamp_and_update_chunk(
        data->chunk_id, pre_write_received, incoming_bytes, data->current_speed, reached_boundary);

    if (data->file.is_open() && bytes_to_write > 0) {
        data->file.write(ptr, static_cast<std::streamsize>(bytes_to_write));
        if (data->file.fail()) {
            data->session->rollback_chunk_write(data->chunk_id, pre_write_received);
            return 0;
        }
        data->bytes_received += static_cast<int64_t>(bytes_to_write);
        data->session->apply_throttle(bytes_to_write);
    }

    if (reached_boundary) {
        return 0;
    }

    return incoming_bytes;
}

} // namespace

bool DownloadSession::get_chunk_bounds(int chunk_id, int64_t& start_offset, int64_t& end_offset) const {
    Chunk c;
    if (chunk_manager_.get_chunk(chunk_id, c)) {
        start_offset = c.start_offset;
        end_offset = c.end_offset;
        return true;
    }
    return false;
}

void DownloadSession::set_chunk_progress(int chunk_id, int64_t bytes, uint64_t speed) {
    chunk_manager_.update_chunk_progress(chunk_id, bytes, speed);
}

void DownloadSession::apply_throttle(size_t bytes) {
    speed_limiter_.throttle(bytes, &pause_requested_);
    SpeedLimiter* global = global_speed_limiter_.load(std::memory_order_relaxed);
    if (global && !stop_requested_ && !pause_requested_) {
        global->throttle(bytes, &pause_requested_);
    }
}

size_t DownloadSession::clamp_and_update_chunk(
    int chunk_id, int64_t current_bytes_received, size_t incoming_bytes,
    uint64_t speed, bool& out_reached_boundary) {
    return chunk_manager_.clamp_and_update_chunk(
        chunk_id, current_bytes_received, incoming_bytes, speed, out_reached_boundary);
}

void DownloadSession::rollback_chunk_write(int chunk_id, int64_t committed_bytes) {
    chunk_manager_.rollback_chunk_write(chunk_id, committed_bytes);
}

void DownloadSession::set_header(const std::string& name, const std::string& value) {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    custom_headers_[name] = value;
}

void DownloadSession::set_credentials(const std::string& username, const std::string& password) {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    if (!username.empty()) {
        auth_userpwd_ = username + ":" + password;
    } else {
        auth_userpwd_.clear();
    }
}

DownloadSession::DownloadSession(const std::string& id, const std::string& url,
                                 const std::string& dest_path, const std::string& temp_dir)
    : id_(id), url_(url), destination_path_(dest_path) {
    max_connections_.store(QueueManager::instance().get_default_max_connections(), std::memory_order_relaxed);

    CURLSH* sh = curl_share_init();
    if (sh) {
        curl_share_setopt(sh, CURLSHOPT_USERDATA, &share_mutexes_);
        curl_share_setopt(sh, CURLSHOPT_LOCKFUNC, curl_share_lock_cb);
        curl_share_setopt(sh, CURLSHOPT_UNLOCKFUNC, curl_share_unlock_cb);
        curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        curl_share_ = sh;
    }

    fs::path p = sys::path_from_utf8(dest_path);
    filename_ = sys::path_to_utf8(p.filename());
    if (filename_.empty() || filename_ == "download.bin") {
        std::string url_fn = extract_filename_from_url(url_);
        if (!url_fn.empty() && url_fn != "download.bin") {
            filename_ = url_fn;
            fs::path parent = p.parent_path();
            destination_path_ = parent.empty() ? filename_ : sys::path_to_utf8(parent / sys::path_from_utf8(filename_));
        } else {
            filename_ = "download.bin";
            if (destination_path_.empty()) {
                destination_path_ = filename_;
            }
        }
    } else if (filename_.find('%') != std::string::npos) {
        std::string decoded = sanitize_filename(url_decode(filename_));
        if (!decoded.empty() && decoded != "download.bin") {
            filename_ = decoded;
            fs::path parent = p.parent_path();
            destination_path_ = parent.empty() ? filename_ : sys::path_to_utf8(parent / sys::path_from_utf8(filename_));
        }
    }

    if (temp_dir.empty()) {
        // Place chunks on the destination volume so C:\ is never exhausted when saving to D:\ / E:\.
        fs::path dest_parent = sys::path_from_utf8(destination_path_).parent_path();
        if (dest_parent.empty()) dest_parent = fs::path(".");
        temp_dir_ = sys::path_to_utf8(dest_parent / ".opendm_tmp" / sys::path_from_utf8(id_));
    } else {
        temp_dir_ = temp_dir;
    }
}

DownloadSession::~DownloadSession() {
    try {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        stop_and_join_locked();
        if (curl_share_) {
            curl_share_cleanup(static_cast<CURLSH*>(curl_share_));
            curl_share_ = nullptr;
        }
    } catch (...) {
    }
}

void DownloadSession::set_max_connections(size_t max_conns) {
    max_connections_.store(std::clamp<size_t>(max_conns, 1, 32), std::memory_order_relaxed);
}

size_t DownloadSession::get_max_connections() const {
    return max_connections_.load(std::memory_order_relaxed);
}

void DownloadSession::set_speed_limit(uint64_t bytes_per_sec) {
    speed_limiter_.set_limit_bytes_per_sec(bytes_per_sec);
}

void DownloadSession::enable_speed_limit(bool enable) {
    speed_limiter_.set_enabled(enable);
}

void DownloadSession::set_progress_callback(ProgressCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    progress_callback_ = std::move(cb);
}

void DownloadSession::set_dirty_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    dirty_callback_ = std::move(cb);
}

void DownloadSession::notify_dirty() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = dirty_callback_;
    }
    if (cb) {
        cb();
    }
}

std::string DownloadSession::get_destination_path() const {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    return destination_path_;
}

std::string DownloadSession::get_filename() const {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    return filename_;
}

std::string DownloadSession::get_category() const {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    return category_;
}

std::string DownloadSession::get_last_error() const {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    return last_error_;
}

std::string DownloadSession::get_last_modified() const {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    return last_modified_;
}

std::string DownloadSession::get_etag() const {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    return etag_;
}

void DownloadSession::set_category(const std::string& cat) {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    category_ = cat;
}

void DownloadSession::set_custom_filename(const std::string& fname) {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    if (fname.empty() || fname == "download.bin") {
        std::string url_fn = extract_filename_from_url(url_);
        filename_ = (!url_fn.empty() && url_fn != "download.bin") ? url_fn : "download.bin";
        custom_filename_set_ = false;
    } else {
        filename_ = fname;
        custom_filename_set_ = true;
    }
    fs::path parent = sys::path_from_utf8(destination_path_).parent_path();
    destination_path_ = parent.empty() ? filename_ : sys::path_to_utf8(parent / sys::path_from_utf8(filename_));
}

void DownloadSession::reset_interrupted_assembling() {
    SessionStatus st = status_.load();
    if (st == SessionStatus::ASSEMBLING || st == SessionStatus::DOWNLOADING || st == SessionStatus::PROBING) {
        status_ = SessionStatus::PAUSED;
        chunk_manager_.reset_all_speeds();
        current_speed_ = 0;
        eta_seconds_ = 0;
    }
}

std::string DownloadSession::url_decode(const std::string& encoded) {
    std::string src = encoded;
    if (src.size() > 7) {
        std::string prefix = sys::ascii_tolower(src.substr(0, 7));
        if (prefix.rfind("utf-8'", 0) == 0) {
            size_t second_quote = src.find('\'', 6);
            if (second_quote != std::string::npos && second_quote + 1 < src.size()) {
                src = src.substr(second_quote + 1);
            }
        }
    }

    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '%' && i + 2 < src.size()) {
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                char decoded = static_cast<char>((hi << 4) | lo);
                if (decoded != '\0') {
                    out.push_back(decoded);
                }
                i += 2;
                continue;
            }
        }
        out.push_back(src[i]);
    }
    return out;
}

std::string DownloadSession::sanitize_filename(const std::string& filename) {
    std::string s = filename;

    while (!s.empty()) {
        unsigned char c = static_cast<unsigned char>(s.front());
        if (c <= 32 || c == '"' || c == '\'' || c == ';' || c == ',') {
            s.erase(0, 1);
        } else {
            break;
        }
    }
    while (!s.empty()) {
        unsigned char c = static_cast<unsigned char>(s.back());
        if (c <= 32 || c == '"' || c == '\'' || c == ';' || c == ',' || c == '.') {
            s.pop_back();
        } else {
            break;
        }
    }

    size_t last_sep = s.find_last_of("/\\");
    if (last_sep != std::string::npos) {
        s = s.substr(last_sep + 1);
    }

    std::string clean;
    clean.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            clean.push_back('_');
        } else {
            clean.push_back(static_cast<char>(c));
        }
    }

    while (!clean.empty() && (clean.front() == ' ' || clean.front() == '\t')) {
        clean.erase(0, 1);
    }
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\t' || clean.back() == '.')) {
        clean.pop_back();
    }

    if (clean.empty() || clean == "." || clean == "..") {
        return "download.bin";
    }
    if (sys::is_reserved_dos_name(clean)) {
        clean = "_" + clean;
    }
    return clean;
}

std::string DownloadSession::extract_filename_from_content_disposition(const std::string& cd) {
    if (cd.empty()) return "";

    {
        std::regex fn_star_re(R"(filename\*\s*=\s*(?:UTF-8''|utf-8'')?["']?([^"';\r\n]+)["']?)", std::regex::icase);
        std::smatch m;
        if (std::regex_search(cd, m, fn_star_re) && m.size() > 1) {
            std::string clean = sanitize_filename(url_decode(m[1].str()));
            if (!clean.empty() && clean != "download.bin") {
                return clean;
            }
        }
    }

    {
        std::regex fn_quoted_re(R"re(filename\s*=\s*"([^"\r\n]+)")re", std::regex::icase);
        std::smatch m;
        if (std::regex_search(cd, m, fn_quoted_re) && m.size() > 1) {
            std::string clean = sanitize_filename(url_decode(m[1].str()));
            if (!clean.empty() && clean != "download.bin") {
                return clean;
            }
        }
    }

    {
        std::regex fn_unquoted_re(R"(filename\s*=\s*([^;\r\n"']+))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(cd, m, fn_unquoted_re) && m.size() > 1) {
            std::string clean = sanitize_filename(url_decode(m[1].str()));
            if (!clean.empty() && clean != "download.bin") {
                return clean;
            }
        }
    }

    return "";
}

std::string DownloadSession::extract_filename_from_url(const std::string& url) {
    if (url.empty()) return "download.bin";

    std::string trimmed = url;
    while (!trimmed.empty() && static_cast<unsigned char>(trimmed.front()) <= 32) trimmed.erase(0, 1);
    while (!trimmed.empty() && static_cast<unsigned char>(trimmed.back()) <= 32) trimmed.pop_back();
    if (trimmed.empty()) return "download.bin";

    size_t hash_pos = trimmed.find('#');
    std::string no_frag = (hash_pos != std::string::npos) ? trimmed.substr(0, hash_pos) : trimmed;

    size_t q_pos = no_frag.find('?');
    std::string path_part = (q_pos != std::string::npos) ? no_frag.substr(0, q_pos) : no_frag;
    std::string query_part = (q_pos != std::string::npos) ? no_frag.substr(q_pos + 1) : "";

    if (!query_part.empty()) {
        std::string decoded_query = url_decode(query_part);
        std::string cd_fn = extract_filename_from_content_disposition(decoded_query);
        if (!cd_fn.empty() && cd_fn != "download.bin") {
            return cd_fn;
        }

        std::regex q_fn_re(R"((?:^|[&;])(?:filename|file|name)\s*=\s*([^&;#\r\n]+))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(query_part, m, q_fn_re) && m.size() > 1) {
            std::string q_cand = sanitize_filename(url_decode(m[1].str()));
            if (!q_cand.empty() && q_cand != "download.bin" && q_cand.find('.') != std::string::npos) {
                return q_cand;
            }
        }
    }

    size_t scheme_pos = path_part.find("://");
    size_t path_start = 0;
    if (scheme_pos != std::string::npos) {
        size_t first_slash = path_part.find_first_of("/\\", scheme_pos + 3);
        if (first_slash == std::string::npos) {
            return "download.bin";
        }
        path_start = first_slash;
    }

    while (path_part.size() > path_start + 1 && (path_part.back() == '/' || path_part.back() == '\\')) {
        path_part.pop_back();
    }

    size_t last_slash = path_part.find_last_of("/\\");
    std::string raw_seg;
    if (last_slash != std::string::npos && last_slash >= path_start && last_slash + 1 < path_part.size()) {
        raw_seg = path_part.substr(last_slash + 1);
    } else if (scheme_pos == std::string::npos) {
        raw_seg = path_part;
    }

    if (raw_seg.empty()) {
        return "download.bin";
    }

    return sanitize_filename(url_decode(raw_seg));
}

void DownloadSession::stop_and_join_locked() {
    stop_requested_ = true;
    pause_requested_ = false;

    const auto self_id = std::this_thread::get_id();

    if (coordinator_thread_.joinable()) {
        if (coordinator_thread_.get_id() == self_id) {
            coordinator_thread_.detach();
        } else {
            coordinator_thread_.join();
        }
    }

    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> wlock(workers_mutex_);
        workers.swap(worker_threads_);
    }
    for (auto& t : workers) {
        if (!t.joinable()) continue;
        if (t.get_id() == self_id) {
            t.detach();
        } else {
            t.join();
        }
    }
}

void DownloadSession::wait_for_stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    stop_and_join_locked();
}

bool DownloadSession::start() {
    if (disposed_) return false;

    std::unique_lock<std::mutex> lock(lifecycle_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || disposed_) {
        return false;
    }

    SessionStatus st = status_.load();
    if (st == SessionStatus::DOWNLOADING || st == SessionStatus::PROBING || st == SessionStatus::ASSEMBLING) {
        return false;
    }

    stop_and_join_locked();

    stop_requested_ = false;
    pause_requested_ = false;
    status_ = SessionStatus::PROBING;

    coordinator_thread_ = std::thread(&DownloadSession::run_session, this);
    return true;
}

void DownloadSession::pause() {
    // Always signal pause immediately even if a start/teardown holds lifecycle_mutex_
    pause_requested_.store(true, std::memory_order_relaxed);

    std::unique_lock<std::mutex> lock(lifecycle_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }

    SessionStatus st = status_.load();
    if (st == SessionStatus::PAUSED || st == SessionStatus::COMPLETED ||
        st == SessionStatus::IDLE || st == SessionStatus::ASSEMBLING) {
        return;
    }

    chunk_manager_.reset_all_speeds();
    status_ = SessionStatus::PAUSED;
    current_speed_ = 0;
    eta_seconds_ = 0;
    update_speed_and_eta();
    notify_dirty();
}

void DownloadSession::resume() {
    SessionStatus st = status_.load();
    if (st == SessionStatus::PAUSED || st == SessionStatus::IDLE || st == SessionStatus::ERROR) {
        start();
    }
}

void DownloadSession::request_stop(bool permanent) {
    if (permanent) {
        disposed_ = true;
    }
    stop_requested_ = true;
    pause_requested_ = false;
}

void DownloadSession::cancel() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    stop_and_join_locked();

    chunk_manager_.cleanup_temp_files();

    status_ = SessionStatus::IDLE;
    current_speed_ = 0;
    eta_seconds_ = 0;
}

bool DownloadSession::redownload() {
    cancel();
    chunk_manager_.reset();
    return start();
}

bool DownloadSession::is_googlevideo_url(const std::string& target_url) {
    return (target_url.find("googlevideo.com") != std::string::npos ||
            target_url.find("videoplayback") != std::string::npos);
}

std::string DownloadSession::resolve_user_agent_for_url(const std::string& target_url) const {
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        auto ua_it = custom_headers_.find("User-Agent");
        if (ua_it != custom_headers_.end() && !ua_it->second.empty()) {
            return ua_it->second;
        }
    }
    if (target_url.find("c=VISIONOS") != std::string::npos) {
        return "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
    }
    if (target_url.find("c=ANDROID_VR") != std::string::npos) {
        return "com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip";
    }
    if (target_url.find("c=ANDROID_TESTSUITE") != std::string::npos) {
        return "com.google.android.youtube/1.9 (Linux; U; Android 11) gzip";
    }
    if (target_url.find("c=ANDROID_KIDS") != std::string::npos) {
        return "com.google.android.apps.youtube.kids/9.40.2 (Linux; U; Android 14) gzip";
    }
    if (target_url.find("c=IOS_KIDS") != std::string::npos) {
        return "com.google.ios.youtubekids/9.40.2 (iPhone16,2; U; CPU iOS 18_1_0 like Mac OS X;)";
    }
    if (target_url.find("c=IOS") != std::string::npos) {
        return "com.google.ios.youtube/21.26.4 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)";
    }
    if (target_url.find("c=ANDROID") != std::string::npos) {
        return "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip";
    }
    if (target_url.find("c=TVHTML5_SIMPLY") != std::string::npos) {
        return "Mozilla/5.0 (PlayStation; PlayStation 4/12.00) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.4 Safari/605.1.15";
    }
    if (target_url.find("c=TVHTML5") != std::string::npos) {
        return "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034943-gold (unlike Gecko), Unknown_TV_Unknown_0/Unknown (Unknown, Unknown)";
    }
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
}

bool DownloadSession::probe_resource(bool* out_identity_changed) {
    if (out_identity_changed) *out_identity_changed = false;

    if (is_googlevideo_url(url_) &&
        (url_.find("sabr=1") != std::string::npos || url_.find("sabr=2") != std::string::npos)) {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        last_error_ = "YouTube SABR stream URL rejected. Please reload video page to capture direct stream.";
        return false;
    }

    int64_t prev_total = total_size_.load();
    std::string prev_etag;
    std::string prev_lm;
    std::map<std::string, std::string> headers_copy;
    std::string userpwd_copy;
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        prev_etag = etag_;
        prev_lm = last_modified_;
        headers_copy = custom_headers_;
        userpwd_copy = auth_userpwd_;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        last_error_ = "Failed to initialize CURL probe";
        return false;
    }

    AttemptHeaderState hdr_state;
    ProbeWriteState probe_write_state;
    if (curl_share_) {
        curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(curl_share_));
    }
    net::configure_curl_ssl(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, probe_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &probe_write_state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, probe_xferinfo_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, attempt_header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hdr_state);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);

    std::string ua = resolve_user_agent_for_url(url_);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());

    if (!userpwd_copy.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd_copy.c_str());
    }

    bool is_mobile_yt = (url_.find("c=ANDROID_VR") != std::string::npos ||
                         url_.find("c=VISIONOS") != std::string::npos ||
                         url_.find("c=ANDROID_TESTSUITE") != std::string::npos ||
                         url_.find("c=IOS") != std::string::npos ||
                         url_.find("c=ANDROID") != std::string::npos);

    struct curl_slist* header_list = nullptr;
    for (const auto& [k, v] : headers_copy) {
        std::string lk = sys::ascii_tolower(k);
        if (lk == "user-agent") continue;
        if (lk == "referer" && is_mobile_yt) continue;
        std::string h = k + ": " + v;
        header_list = curl_slist_append(header_list, h.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    char* eff_url = nullptr;
    std::string final_eff_url;
    if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff_url) == CURLE_OK && eff_url && *eff_url) {
        final_eff_url = eff_url;
        std::lock_guard<std::mutex> lock(meta_mutex_);
        effective_url_ = final_eff_url;
    }

    if (header_list) {
        curl_slist_free_all(header_list);
    }
    curl_easy_cleanup(curl);

    bool write_aborted_on_200 = (res == CURLE_WRITE_ERROR && http_code == 200);
    if (res != CURLE_OK && res != CURLE_PARTIAL_FILE && !write_aborted_on_200) {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        last_error_ = std::string("Probe network error: ") + curl_easy_strerror(res);
        return false;
    }

    if (http_code == 206) {
        supports_ranges_ = true;
        if (hdr_state.has_content_range && hdr_state.range_total > 0) {
            total_size_ = hdr_state.range_total;
        } else {
            total_size_ = -1;
        }
    } else if (http_code == 200) {
        supports_ranges_ = false;
        auto it = hdr_state.headers.find("content-length");
        if (it != hdr_state.headers.end()) {
            try {
                total_size_ = std::stoll(it->second);
            } catch (...) {
                total_size_ = -1;
            }
        }
    } else {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        last_error_ = "HTTP error code: " + std::to_string(http_code);
        return false;
    }

    auto ct_it = hdr_state.headers.find("content-type");
    if (ct_it != hdr_state.headers.end()) {
        std::string ct = sys::ascii_tolower(ct_it->second);
        if (ct.find("yt-ump") != std::string::npos) {
            std::lock_guard<std::mutex> lock(meta_mutex_);
            last_error_ = "YouTube SABR control stream intercepted. Please reload video page to capture direct stream.";
            return false;
        }
    }

    std::string resolved_fn;
    auto cd_it = hdr_state.headers.find("content-disposition");
    if (cd_it != hdr_state.headers.end()) {
        resolved_fn = extract_filename_from_content_disposition(cd_it->second);
    }

    if (resolved_fn.empty()) {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        if (filename_.empty() || filename_ == "download.bin") {
            if (!effective_url_.empty() && !is_googlevideo_url(effective_url_)) {
                std::string eff_fn = extract_filename_from_url(effective_url_);
                if (!eff_fn.empty() && eff_fn != "download.bin") {
                    resolved_fn = eff_fn;
                }
            }
            if (resolved_fn.empty() && !is_googlevideo_url(url_)) {
                std::string u_fn = extract_filename_from_url(url_);
                if (!u_fn.empty() && u_fn != "download.bin") {
                    resolved_fn = u_fn;
                }
            }
        }
    }

    if (!resolved_fn.empty()) {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        if (!custom_filename_set_ || filename_.empty() || filename_ == "download.bin") {
            filename_ = resolved_fn;
            fs::path p = sys::path_from_utf8(destination_path_);
            fs::path parent = p.parent_path();
            destination_path_ = parent.empty() ? filename_ : sys::path_to_utf8(parent / sys::path_from_utf8(filename_));
            if (category_.empty() || category_ == "General" || category_ == "Programs") {
                std::string new_cat = QueueManager::detect_category(filename_);
                if (new_cat != "General") {
                    category_ = new_cat;
                }
            }
        }
    }

    std::string new_etag;
    std::string new_lm;
    auto lm_it = hdr_state.headers.find("last-modified");
    if (lm_it != hdr_state.headers.end()) new_lm = lm_it->second;
    auto etag_it = hdr_state.headers.find("etag");
    if (etag_it != hdr_state.headers.end()) new_etag = etag_it->second;

    int64_t new_total = total_size_.load();
    bool id_changed = false;
    if (prev_total > 0 && new_total > 0 && prev_total != new_total) {
        id_changed = true;
    } else if (!prev_etag.empty() && !new_etag.empty() && prev_etag != new_etag) {
        id_changed = true;
    } else if (prev_etag.empty() && new_etag.empty() && !prev_lm.empty() && !new_lm.empty() && prev_lm != new_lm) {
        id_changed = true;
    }
    if (out_identity_changed) {
        *out_identity_changed = id_changed;
    }

    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        if (!new_lm.empty()) last_modified_ = new_lm;
        if (!new_etag.empty()) etag_ = new_etag;
    }

    // Check target filesystem constraints (FAT32 > 4GB) & available disk space on target and temp volumes
    if (new_total > 0) {
        std::string check_dest;
        std::string audio_url_copy;
        {
            std::lock_guard<std::mutex> lock(meta_mutex_);
            check_dest = destination_path_;
            audio_url_copy = audio_stream_url_;
        }
        fs::path dest_parent = sys::path_from_utf8(check_dest).parent_path();
        std::string dest_dir = dest_parent.empty() ? "." : sys::path_to_utf8(dest_parent);

        sys::VolumeCapabilities dest_vol = sys::inspect_target_volume(dest_dir);
        if (dest_vol.valid && dest_vol.is_fat32 && new_total >= 0x100000000LL) {
            std::lock_guard<std::mutex> lock(meta_mutex_);
            last_error_ = "Target drive uses FAT32 which cannot store files >= 4 GB. Save to an NTFS or exFAT drive.";
            return false;
        }

        int64_t remaining_download = id_changed ? new_total :
            (chunk_manager_.get_chunks_snapshot().empty() ? new_total : chunk_manager_.get_remaining_bytes());
        if (remaining_download < 0) remaining_download = new_total;

        // Same-volume assembly for multi-chunk downloads requires space for remaining downloads + final file copy
        size_t planned_conns = supports_ranges_ ? std::min<size_t>(max_connections_.load(), is_googlevideo_url(url_) ? 6 : 32) : 1;
        uint64_t dest_required = static_cast<uint64_t>(remaining_download);
        if (planned_conns > 1 && !id_changed) {
            // Only add final copy headroom if not single chunk
            dest_required += static_cast<uint64_t>(new_total);
        } else if (planned_conns > 1) {
            dest_required += static_cast<uint64_t>(new_total);
        }
        if (!audio_url_copy.empty()) {
            dest_required += static_cast<uint64_t>(new_total) / 5 + 32ULL * 1024 * 1024;
        }

        if (dest_vol.valid && dest_vol.free_bytes < dest_required) {
            std::lock_guard<std::mutex> lock(meta_mutex_);
            last_error_ = "Not enough disk space on target drive. Required: " +
                          sys::format_bytes(static_cast<int64_t>(dest_required)) + ", Available: " +
                          sys::format_bytes(static_cast<int64_t>(dest_vol.free_bytes));
            return false;
        }
    }

    return true;
}

void DownloadSession::run_session() {
    std::string vid_tmp_check;
    std::string aud_tmp_check;
    std::string aud_url_check;
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        vid_tmp_check = destination_path_ + ".video.tmp";
        aud_tmp_check = destination_path_ + ".audio.tmp";
        aud_url_check = audio_stream_url_;
    }

    // If a previous run downloaded video+audio and failed only during muxing, skip network probe/download and retry mux directly
    {
        std::error_code ec;
        if (!aud_url_check.empty() &&
            chunk_manager_.all_chunks_completed() &&
            fs::exists(sys::path_from_utf8(vid_tmp_check), ec) &&
            fs::exists(sys::path_from_utf8(aud_tmp_check), ec)) {
            audio_download_complete_.store(true);
            assemble_file();
            return;
        }
    }

    bool identity_changed = false;
    if (!probe_resource(&identity_changed)) {
        if (stop_requested_ || pause_requested_) {
            return;
        }
        status_ = SessionStatus::ERROR;
        update_speed_and_eta();
        notify_dirty();
        return;
    }

    if (stop_requested_ || pause_requested_) {
        return;
    }

    if (identity_changed && !chunk_manager_.get_chunks_snapshot().empty()) {
        chunk_manager_.reset();
    }

    size_t max_conns = max_connections_.load(std::memory_order_relaxed);
    size_t effective_max_conns = is_googlevideo_url(url_)
        ? std::min<size_t>(max_conns, 6)
        : std::min<size_t>(max_conns, 32);

    auto existing_chunks = chunk_manager_.get_chunks_snapshot();
    if (!supports_ranges_) {
        chunk_manager_.init_stream_chunk(temp_dir_, id_);
    } else if (existing_chunks.empty()) {
        if (total_size_ > 0) {
            if (!chunk_manager_.init_first_chunk(total_size_, temp_dir_, id_)) {
                std::lock_guard<std::mutex> lock(meta_mutex_);
                last_error_ = "Failed to create temporary chunk directory.";
                status_ = SessionStatus::ERROR;
                update_speed_and_eta();
                return;
            }
            size_t current_chunks = 1;
            while (current_chunks < effective_max_conns &&
                   chunk_manager_.need_new_connection(current_chunks, effective_max_conns)) {
                int target_chunk = chunk_manager_.find_max_free_chunk();
                if (target_chunk == -1) break;
                int new_id = chunk_manager_.split_chunk(target_chunk, temp_dir_, id_);
                if (new_id == -1) break;
                current_chunks++;
            }
        } else {
            chunk_manager_.init_stream_chunk(temp_dir_, id_);
        }
    } else {
        // Reconcile on-disk .partN sizes BEFORE any splits can run
        chunk_manager_.reconcile_disk_sizes();
    }

    if (chunk_manager_.all_chunks_completed()) {
        assemble_file();
        return;
    }

    status_ = SessionStatus::DOWNLOADING;
    update_speed_and_eta();
    notify_dirty();

    // Launch up to effective_max_conns workers; any excess resumed chunks stay PENDING for work-stealing
    existing_chunks = chunk_manager_.get_chunks_snapshot();
    {
        std::lock_guard<std::mutex> slock(split_mutex_);
        std::lock_guard<std::mutex> lock(workers_mutex_);
        worker_threads_.clear();
        active_workers_count_.store(0, std::memory_order_relaxed);
        size_t spawned = 0;
        for (const auto& c : existing_chunks) {
            if (c.status != ChunkStatus::COMPLETED) {
                if (spawned >= effective_max_conns) {
                    chunk_manager_.set_chunk_status(c.id, ChunkStatus::PENDING);
                    continue;
                }
                chunk_manager_.set_chunk_status(c.id, ChunkStatus::DOWNLOADING);
                active_workers_count_.fetch_add(1, std::memory_order_relaxed);
                worker_threads_.emplace_back(&DownloadSession::download_chunk_worker, this, c.id);
                spawned++;
            }
        }
    }

    std::string audio_url;
    std::string audio_tmp_path;
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        audio_url = audio_stream_url_;
        if (!audio_url.empty()) {
            audio_tmp_path = destination_path_ + ".audio.tmp";
        }
    }

    audio_download_complete_.store(false);
    std::thread audio_thread;
    if (!audio_url.empty() &&
        (!is_googlevideo_url(audio_url) || (audio_url.find("sabr=1") == std::string::npos && audio_url.find("sabr=2") == std::string::npos))) {
        audio_thread = std::thread([this, audio_url, audio_tmp_path]() {
            if (download_audio_stream_to_file(audio_url, audio_tmp_path)) {
                audio_download_complete_.store(true);
            }
        });
    }

    dynamic_split_loop();

    {
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            workers.swap(worker_threads_);
        }
        for (auto& t : workers) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    if (audio_thread.joinable()) {
        audio_thread.join();
    }

    if (stop_requested_ || pause_requested_) {
        if (pause_requested_) {
            status_ = SessionStatus::PAUSED;
            current_speed_ = 0;
            eta_seconds_ = 0;
            update_speed_and_eta();
            notify_dirty();
        }
        return;
    }

    if (chunk_manager_.all_chunks_completed()) {
        assemble_file();
    } else {
        {
            std::lock_guard<std::mutex> lock(meta_mutex_);
            if (last_error_.empty()) {
                last_error_ = "One or more chunks failed to complete.";
            }
        }
        status_ = SessionStatus::ERROR;
        update_speed_and_eta();
        notify_dirty();
    }
}

void DownloadSession::download_chunk_worker(int initial_chunk_id) {
    struct ActiveWorkerGuard {
        std::atomic<size_t>& counter;
        ~ActiveWorkerGuard() { counter.fetch_sub(1, std::memory_order_relaxed); }
    } worker_guard{active_workers_count_};

    CURL* curl = curl_easy_init();
    if (!curl) {
        chunk_manager_.set_chunk_status(initial_chunk_id, ChunkStatus::FAILED);
        return;
    }

    if (curl_share_) {
        curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(curl_share_));
    }
    net::configure_curl_ssl(curl);

    std::string target_url;
    std::string if_range_val;
    std::map<std::string, std::string> headers_copy;
    std::string userpwd_copy;
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        target_url = !effective_url_.empty() ? effective_url_ : url_;
        headers_copy = custom_headers_;
        userpwd_copy = auth_userpwd_;
        if_range_val = !etag_.empty() ? etag_ : last_modified_;
    }

    std::string ua = resolve_user_agent_for_url(url_);
    bool is_yt = is_googlevideo_url(url_);
    bool is_mobile_yt = (url_.find("c=ANDROID_VR") != std::string::npos ||
                         url_.find("c=VISIONOS") != std::string::npos ||
                         url_.find("c=ANDROID_TESTSUITE") != std::string::npos ||
                         url_.find("c=IOS") != std::string::npos ||
                         url_.find("c=ANDROID") != std::string::npos);

    curl_easy_setopt(curl, CURLOPT_URL, target_url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, attempt_header_callback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_callback);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

    // Adjust low-speed abort threshold if user explicitly throttled below 4 KB/s per worker
    long low_speed_limit = 512L;
    SpeedLimiter* global = global_speed_limiter_.load(std::memory_order_relaxed);
    if ((speed_limiter_.is_enabled() && speed_limiter_.get_limit_bytes_per_sec() < 65536) ||
        (global && global->is_enabled() && global->get_limit_bytes_per_sec() < 65536)) {
        low_speed_limit = 1L;
    }
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, low_speed_limit);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 45L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                     is_yt ? CURL_HTTP_VERSION_2TLS : CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 2L * 1024L * 1024L);

    if (!userpwd_copy.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd_copy.c_str());
    }

    struct curl_slist* header_list = nullptr;
    for (const auto& [k, v] : headers_copy) {
        std::string lk = sys::ascii_tolower(k);
        if (lk == "user-agent" || lk == "if-range") continue;
        if (lk == "referer" && is_mobile_yt) continue;
        std::string h = k + ": " + v;
        header_list = curl_slist_append(header_list, h.c_str());
    }
    bool has_if_range = false;
    if (!if_range_val.empty() && supports_ranges_.load()) {
        std::string h = "If-Range: " + if_range_val;
        header_list = curl_slist_append(header_list, h.c_str());
        has_if_range = true;
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    std::vector<char> file_buf(2 * 1024 * 1024);
    constexpr int64_t YT_SEGMENT_SIZE = 8 * 1024 * 1024;
    constexpr int MAX_CHUNK_RETRIES = 12;
    bool using_effective_url = (target_url != url_);

    int chunk_id = initial_chunk_id;
    while (chunk_id != -1 && !stop_requested_ && !pause_requested_) {
        Chunk chunk;
        if (!chunk_manager_.get_chunk(chunk_id, chunk)) {
            break;
        }

        WorkerData data;
        data.session = this;
        data.curl = curl;
        data.chunk_id = chunk_id;
        data.expected_total_size = total_size_.load();
        data.expect_ranged = supports_ranges_.load();
        data.sent_if_range = has_if_range;
        data.last_speed_time = std::chrono::steady_clock::now();

        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &data);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &data.hdr_state);

        fs::path part_p = sys::path_from_utf8(chunk.temp_filepath);
        std::error_code ec;
        int64_t disk_bytes = 0;
        if (fs::exists(part_p, ec)) {
            uint64_t fsz = fs::file_size(part_p, ec);
            if (!ec) disk_bytes = static_cast<int64_t>(fsz);
        }

        if (!supports_ranges_.load()) {
            disk_bytes = 0;
        } else if (chunk.end_offset >= chunk.start_offset) {
            int64_t max_chunk_sz = chunk.end_offset - chunk.start_offset + 1;
            if (disk_bytes > max_chunk_sz) {
                fs::resize_file(part_p, static_cast<uintmax_t>(max_chunk_sz), ec);
                if (!ec) disk_bytes = max_chunk_sz;
            }
        }

        chunk_manager_.update_chunk_progress(chunk_id, disk_bytes, 0);
        data.bytes_received = disk_bytes;

        std::ios_base::openmode mode = std::ios::binary | std::ios::out | (disk_bytes > 0 ? std::ios::app : std::ios::trunc);
        data.file.open(part_p, mode);
        if (!data.file.is_open()) {
            chunk_manager_.set_chunk_status(chunk_id, ChunkStatus::FAILED);
            break;
        }
        data.file.rdbuf()->pubsetbuf(file_buf.data(), static_cast<std::streamsize>(file_buf.size()));
        chunk_manager_.set_chunk_status(chunk_id, ChunkStatus::DOWNLOADING);

        int64_t start_pos = chunk.start_offset + disk_bytes;
        if (supports_ranges_.load() && chunk.end_offset >= chunk.start_offset && start_pos > chunk.end_offset) {
            data.file.close();
            chunk_manager_.mark_chunk_completed(chunk_id);
        } else {
            CURLcode res = CURLE_OK;
            long http_code = 0;
            int retry_count = 0;
            bool non_range_restart = false;

            while (!stop_requested_ && !pause_requested_) {
                Chunk cur_chunk;
                if (!chunk_manager_.get_chunk(chunk_id, cur_chunk)) break;

                if (non_range_restart) {
                    data.file.close();
                    data.file.open(part_p, std::ios::binary | std::ios::out | std::ios::trunc);
                    if (!data.file.is_open()) {
                        res = CURLE_WRITE_ERROR;
                        break;
                    }
                    data.file.rdbuf()->pubsetbuf(file_buf.data(), static_cast<std::streamsize>(file_buf.size()));
                    data.bytes_received = 0;
                    chunk_manager_.update_chunk_progress(chunk_id, 0, 0);
                    non_range_restart = false;
                }

                int64_t cur_start = cur_chunk.start_offset + data.bytes_received;
                int64_t cur_end = cur_chunk.end_offset;
                if (supports_ranges_.load() && cur_end >= cur_chunk.start_offset && cur_start > cur_end) {
                    res = CURLE_OK;
                    http_code = 206;
                    break;
                }

                data.expected_start_offset = cur_start;
                data.expected_chunk_end = cur_end;
                data.attempt_validated = false;
                data.response_rejected = false;
                data.if_range_mismatch = false;
                data.rejected_http_code = 0;
                data.hdr_state.last_status = 0;
                data.hdr_state.reset_for_new_response();

                if (supports_ranges_.load() && cur_end >= 0) {
                    int64_t seg_end = is_yt ? std::min<int64_t>(cur_start + YT_SEGMENT_SIZE - 1, cur_end) : cur_end;
                    std::string range_header = std::to_string(cur_start) + "-" + std::to_string(seg_end);
                    curl_easy_setopt(curl, CURLOPT_RANGE, range_header.c_str());
                } else {
                    curl_easy_setopt(curl, CURLOPT_RANGE, nullptr);
                }

                int64_t bytes_before_attempt = data.bytes_received;
                res = curl_easy_perform(curl);
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                if (data.rejected_http_code > 0) {
                    http_code = data.rejected_http_code;
                }

                if (stop_requested_ || pause_requested_) {
                    break;
                }

                if (data.if_range_mismatch) {
                    std::lock_guard<std::mutex> lock(meta_mutex_);
                    last_error_ = "Remote file changed during download (ETag/size mismatch).";
                    break;
                }

                Chunk after_chunk;
                bool has_after = chunk_manager_.get_chunk(chunk_id, after_chunk);
                bool hit_split_bound = !data.response_rejected && has_after &&
                    (after_chunk.end_offset >= after_chunk.start_offset) &&
                    (after_chunk.downloaded_bytes >= (after_chunk.end_offset - after_chunk.start_offset + 1));

                if (hit_split_bound) {
                    res = CURLE_OK;
                    http_code = 206;
                    break;
                }

                if (!supports_ranges_.load() || cur_end < 0) {
                    bool complete_stream = (res == CURLE_OK) &&
                        (total_size_.load() <= 0 || data.bytes_received >= total_size_.load());
                    if (complete_stream || retry_count >= 4) {
                        break;
                    }
                    retry_count++;
                    non_range_restart = true;
                    for (int w = 0; w < 25 * retry_count && !stop_requested_ && !pause_requested_; ++w) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    continue;
                }

                if (is_yt && res == CURLE_OK && !data.response_rejected &&
                    (http_code == 206 || (cur_start == 0 && http_code == 200))) {
                    retry_count = 0;
                    continue;
                }

                if (!data.response_rejected && data.bytes_received > bytes_before_attempt) {
                    retry_count = 0;
                } else {
                    retry_count++;
                }

                if (using_effective_url && (http_code == 400 || http_code == 403 || http_code == 404 || http_code == 410)) {
                    using_effective_url = false;
                    {
                        std::lock_guard<std::mutex> lock(meta_mutex_);
                        effective_url_.clear();
                    }
                    curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
                    if (retry_count > 1) retry_count = 1;
                } else if (!using_effective_url && (http_code == 403 || http_code == 404 || http_code == 410) && retry_count >= 2) {
                    std::lock_guard<std::mutex> lock(meta_mutex_);
                    last_error_ = "HTTP error code: " + std::to_string(http_code);
                    break;
                }

                if (retry_count > MAX_CHUNK_RETRIES) {
                    break;
                }

                data.file.flush();
                // Exponential backoff capped at ~15s (checked every 25ms for fast pause/stop)
                int delay_ms = std::min(15000, 200 * (1 << std::min(retry_count - 1, 6)));
                for (int waited = 0; waited < delay_ms && !stop_requested_ && !pause_requested_; waited += 25) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
            }

            data.file.flush();
            bool file_ok = !data.file.fail();
            data.file.close();

            if (stop_requested_ || pause_requested_) {
                Chunk cur_c;
                if (chunk_manager_.get_chunk(chunk_id, cur_c) && cur_c.status != ChunkStatus::COMPLETED) {
                    chunk_manager_.set_chunk_status(chunk_id, ChunkStatus::PENDING);
                }
                break;
            }

            Chunk latest_chunk;
            bool has_c = chunk_manager_.get_chunk(chunk_id, latest_chunk);
            bool reached_boundary = has_c && (latest_chunk.end_offset >= latest_chunk.start_offset) &&
                                    (latest_chunk.downloaded_bytes >= (latest_chunk.end_offset - latest_chunk.start_offset + 1));
            int64_t expected_total = total_size_.load();
            bool stream_completed = has_c && (latest_chunk.end_offset < 0) && (res == CURLE_OK) &&
                                    (expected_total <= 0 || latest_chunk.downloaded_bytes >= expected_total);

            chunk_manager_.update_chunk_progress(chunk_id, has_c ? latest_chunk.downloaded_bytes : 0, 0);

            bool range_honored = !data.response_rejected &&
                (!supports_ranges_.load() || !has_if_range ? (http_code == 200 || http_code == 206)
                                                           : (start_pos == 0 ? (http_code == 200 || http_code == 206) : (http_code == 206)));
            if (file_ok && range_honored && (reached_boundary || stream_completed)) {
                chunk_manager_.mark_chunk_completed(chunk_id);
            } else {
                {
                    std::lock_guard<std::mutex> lock(meta_mutex_);
                    if (last_error_.empty()) {
                        if (!file_ok) {
                            last_error_ = "Disk write failed while downloading chunk.";
                        } else if (http_code >= 400) {
                            last_error_ = "HTTP error code: " + std::to_string(http_code);
                        } else {
                            last_error_ = std::string("Network transfer failed: ") + curl_easy_strerror(res);
                        }
                    }
                }
                chunk_manager_.set_chunk_status(chunk_id, ChunkStatus::FAILED);
                break;
            }
        }

        // Work-stealing: first pick up any queued PENDING chunks, then dynamically split the largest remaining chunk
        int next_chunk_id = -1;
        if (!stop_requested_ && !pause_requested_ && supports_ranges_.load()) {
            std::lock_guard<std::mutex> slock(split_mutex_);
            next_chunk_id = chunk_manager_.claim_next_pending_chunk();
            if (next_chunk_id == -1) {
                size_t max_c = max_connections_.load(std::memory_order_relaxed);
                size_t eff_max = is_yt ? std::min<size_t>(max_c, 6) : std::min<size_t>(max_c, 32);
                size_t other_active = active_workers_count_.load(std::memory_order_relaxed);
                if (other_active > 0) other_active--;
                if (other_active < eff_max && chunk_manager_.need_new_connection(other_active, eff_max)) {
                    int target_chunk = chunk_manager_.find_max_free_chunk();
                    if (target_chunk != -1) {
                        next_chunk_id = chunk_manager_.split_chunk(target_chunk, temp_dir_, id_);
                        if (next_chunk_id != -1) {
                            chunk_manager_.set_chunk_status(next_chunk_id, ChunkStatus::DOWNLOADING);
                        }
                    }
                }
            }
        }
        chunk_id = next_chunk_id;
    }

    if (header_list) {
        curl_slist_free_all(header_list);
    }
    curl_easy_cleanup(curl);
}

void DownloadSession::dynamic_split_loop() {
    auto last_save_time = std::chrono::steady_clock::now();

    while (!stop_requested_ && !pause_requested_) {
        update_speed_and_eta();

        if (chunk_manager_.all_chunks_completed()) {
            break;
        }

        if (supports_ranges_.load()) {
            std::lock_guard<std::mutex> slock(split_mutex_);
            size_t max_conns = max_connections_.load(std::memory_order_relaxed);
            size_t effective_max_conns = is_googlevideo_url(url_)
                ? std::min<size_t>(max_conns, 6)
                : std::min<size_t>(max_conns, 32);

            while (!stop_requested_ && !pause_requested_ &&
                   active_workers_count_.load(std::memory_order_relaxed) < effective_max_conns) {
                int new_id = chunk_manager_.claim_next_pending_chunk();
                if (new_id == -1) {
                    if (!chunk_manager_.need_new_connection(active_workers_count_.load(std::memory_order_relaxed), effective_max_conns)) {
                        break;
                    }
                    int target_chunk = chunk_manager_.find_max_free_chunk();
                    if (target_chunk == -1) break;
                    new_id = chunk_manager_.split_chunk(target_chunk, temp_dir_, id_);
                    if (new_id == -1) break;
                    chunk_manager_.set_chunk_status(new_id, ChunkStatus::DOWNLOADING);
                }
                active_workers_count_.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(workers_mutex_);
                worker_threads_.emplace_back(&DownloadSession::download_chunk_worker, this, new_id);
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_save_time).count() >= 5) {
            notify_dirty();
            last_save_time = now;
        }

        if (active_workers_count_.load(std::memory_order_relaxed) == 0 && !chunk_manager_.all_chunks_completed()) {
            break;
        }

        for (int i = 0; i < 10 && !stop_requested_ && !pause_requested_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

bool DownloadSession::download_audio_stream_to_file(const std::string& audio_url, const std::string& audio_tmp_path) {
    if (audio_url.empty()) return false;

    fs::path audio_p = sys::path_from_utf8(audio_tmp_path);
    std::error_code ec;
    if (audio_p.has_parent_path()) {
        fs::create_directories(audio_p.parent_path(), ec);
    }

    int64_t existing_sz = 0;
    if (fs::exists(audio_p, ec)) {
        uint64_t fsz = fs::file_size(audio_p, ec);
        if (!ec) existing_sz = static_cast<int64_t>(fsz);
    }

    // Declare buffer BEFORE ofstream so ofstream flushes/closes BEFORE buffer frees on early return
    std::vector<char> file_buf(2 * 1024 * 1024);
    std::ios_base::openmode mode = std::ios::binary | std::ios::out | (existing_sz > 0 ? std::ios::app : std::ios::trunc);
    std::ofstream afp(audio_p, mode);
    if (!afp.is_open()) return false;
    afp.rdbuf()->pubsetbuf(file_buf.data(), static_cast<std::streamsize>(file_buf.size()));

    CURL* acurl = curl_easy_init();
    if (!acurl) return false;

    if (curl_share_) {
        curl_easy_setopt(acurl, CURLOPT_SHARE, static_cast<CURLSH*>(curl_share_));
    }
    net::configure_curl_ssl(acurl);

    constexpr int64_t SEG_SIZE = 4 * 1024 * 1024;
    int64_t cur_off = existing_sz;
    int64_t total_audio_sz = -1;
    bool audio_dl_ok = true;

    std::string aua = resolve_user_agent_for_url(audio_url);
    curl_easy_setopt(acurl, CURLOPT_URL, audio_url.c_str());
    curl_easy_setopt(acurl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(acurl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(acurl, CURLOPT_USERAGENT, aua.c_str());
    curl_easy_setopt(acurl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(acurl, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(acurl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(acurl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(acurl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(acurl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(acurl, CURLOPT_XFERINFOFUNCTION, probe_xferinfo_callback);
    curl_easy_setopt(acurl, CURLOPT_XFERINFODATA, this);

    struct curl_slist* aheaders = nullptr;
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        auto ref_it = custom_headers_.find("Referer");
        if (ref_it != custom_headers_.end() && !ref_it->second.empty() && audio_url.find("googlevideo.com") == std::string::npos) {
            std::string h = "Referer: " + ref_it->second;
            aheaders = curl_slist_append(aheaders, h.c_str());
        }
    }
    if (aheaders) {
        curl_easy_setopt(acurl, CURLOPT_HTTPHEADER, aheaders);
    }

    curl_easy_setopt(acurl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t s, size_t n, void* ud) -> size_t {
            auto* out = static_cast<std::ofstream*>(ud);
            size_t total = s * n;
            out->write(ptr, static_cast<std::streamsize>(total));
            return out->fail() ? 0 : total;
        });
    curl_easy_setopt(acurl, CURLOPT_WRITEDATA, &afp);

    AttemptHeaderState a_hdr_state;
    curl_easy_setopt(acurl, CURLOPT_HEADERFUNCTION, attempt_header_callback);
    curl_easy_setopt(acurl, CURLOPT_HEADERDATA, &a_hdr_state);

    int retries = 0;
    while (!stop_requested_ && !pause_requested_) {
        a_hdr_state.last_status = 0;
        a_hdr_state.reset_for_new_response();
        int64_t end_off = (total_audio_sz > 0)
            ? std::min<int64_t>(cur_off + SEG_SIZE - 1, total_audio_sz - 1)
            : (cur_off + SEG_SIZE - 1);
        std::string rng = std::to_string(cur_off) + "-" + std::to_string(end_off);
        curl_easy_setopt(acurl, CURLOPT_RANGE, rng.c_str());

        int64_t seg_start = cur_off;
        CURLcode ares = curl_easy_perform(acurl);
        long http_code = 0;
        curl_easy_getinfo(acurl, CURLINFO_RESPONSE_CODE, &http_code);

        if (stop_requested_ || pause_requested_) {
            break;
        }

        if (ares != CURLE_OK || (http_code != 200 && http_code != 206)) {
            if (retries < 6 && http_code < 400) {
                retries++;
                std::this_thread::sleep_for(std::chrono::milliseconds(300 * retries));
                continue;
            }
            audio_dl_ok = false;
            break;
        }

        if (total_audio_sz < 0 && a_hdr_state.has_content_range && a_hdr_state.range_total > 0) {
            total_audio_sz = a_hdr_state.range_total;
        }

        std::streamoff pos = afp.tellp();
        if (pos < 0 || afp.fail()) {
            audio_dl_ok = false;
            break;
        }
        cur_off = static_cast<int64_t>(pos);
        retries = 0;

        int64_t expected_bytes = end_off - seg_start + 1;
        if (http_code == 200 || (total_audio_sz > 0 && cur_off >= total_audio_sz) || (cur_off - seg_start < expected_bytes)) {
            break;
        }
    }

    afp.flush();
    afp.close();

    if (aheaders) {
        curl_slist_free_all(aheaders);
    }
    curl_easy_cleanup(acurl);

    // Only delete temp audio file on explicit cancel, NEVER on pause!
    if (stop_requested_ && !pause_requested_) {
        fs::remove(audio_p, ec);
        return false;
    }
    if (pause_requested_) {
        return false;
    }

    return audio_dl_ok && (total_audio_sz <= 0 || cur_off >= total_audio_sz);
}

void DownloadSession::assemble_file() {
    if (stop_requested_ || pause_requested_) {
        return;
    }

    status_ = SessionStatus::ASSEMBLING;
    update_speed_and_eta();

    std::string dest_path;
    std::string fn;
    std::string audio_url;
    int64_t sz = total_size_.load();
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        dest_path = destination_path_;
        fn = filename_;
        audio_url = audio_stream_url_;
    }

    if (!audio_url.empty() &&
        (audio_url.find("googlevideo.com") != std::string::npos || audio_url.find("videoplayback") != std::string::npos) &&
        (audio_url.find("sabr=1") != std::string::npos || audio_url.find("sabr=2") != std::string::npos)) {
        audio_url.clear();
    }

    std::error_code ec;
    std::string video_tmp = dest_path + ".video.tmp";
    std::string audio_tmp = dest_path + ".audio.tmp";
    fs::path dest_p = sys::path_from_utf8(dest_path);
    fs::path video_tmp_p = sys::path_from_utf8(video_tmp);
    fs::path audio_tmp_p = sys::path_from_utf8(audio_tmp);

    bool already_joined_for_mux_retry = !audio_url.empty() &&
        fs::exists(video_tmp_p, ec) && fs::exists(audio_tmp_p, ec);

    double join_weight = audio_url.empty() ? 1.0 : 0.70;
    if (!already_joined_for_mux_retry) {
        bool ok = chunk_manager_.join_file(dest_path, [this, join_weight](double p) {
            std::function<void(const DownloadProgress&)> cb;
            DownloadProgress prog = get_progress();
            prog.status = SessionStatus::ASSEMBLING;
            prog.progress_fraction = p * join_weight;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                cb = progress_callback_;
            }
            if (cb) cb(prog);
        }, &stop_requested_);

        if (stop_requested_) {
            return;
        }

        if (!ok) {
            status_ = SessionStatus::ERROR;
            {
                std::lock_guard<std::mutex> lock(meta_mutex_);
                last_error_ = "Failed to assemble file parts.";
            }
            PostDownloadManager::instance().notify_completion(fn, sz, false, "Failed to assemble file parts.");
            update_speed_and_eta();
            notify_dirty();
            return;
        }

        if (fs::exists(dest_p, ec) && fs::file_size(dest_p, ec) <= 64) {
            bool sabr_err = false;
            {
                std::ifstream chk(dest_p, std::ios::binary);
                std::string body((std::istreambuf_iterator<char>(chk)), std::istreambuf_iterator<char>());
                sabr_err = (body.find("sabr.malformed_config") != std::string::npos);
            }
            if (sabr_err) {
                fs::remove(dest_p, ec);
                status_ = SessionStatus::ERROR;
                {
                    std::lock_guard<std::mutex> lock(meta_mutex_);
                    last_error_ = "YouTube SABR control response intercepted instead of media stream";
                }
                update_speed_and_eta();
                notify_dirty();
                return;
            }
        }
    }

    // Mux separate adaptive audio track if present
    if (!audio_url.empty()) {
        bool audio_dl_ok = audio_download_complete_.load();
        if (!audio_dl_ok) {
            audio_dl_ok = download_audio_stream_to_file(audio_url, audio_tmp);
        }

        if (stop_requested_ || pause_requested_) {
            return;
        }

        if (!audio_dl_ok) {
            if (!already_joined_for_mux_retry && fs::exists(dest_p, ec)) {
                sys::atomic_replace_file(dest_p, video_tmp_p, ec);
            }
            status_ = SessionStatus::ERROR;
            {
                std::lock_guard<std::mutex> lock(meta_mutex_);
                last_error_ = "Failed to download separate audio stream (tracks preserved for retry).";
            }
            PostDownloadManager::instance().notify_completion(fn, sz, false, "Audio download failed.");
            update_speed_and_eta();
            notify_dirty();
            return;
        }

        if (!already_joined_for_mux_retry) {
            if (!sys::atomic_replace_file(dest_p, video_tmp_p, ec)) {
                status_ = SessionStatus::ERROR;
                {
                    std::lock_guard<std::mutex> lock(meta_mutex_);
                    last_error_ = "Failed to stage video file for audio muxing.";
                }
                update_speed_and_eta();
                notify_dirty();
                return;
            }
        }

        std::string mux_err;
        bool mux_ok = MediaMuxer::mux_streams(
            video_tmp, audio_tmp, dest_path,
            [this](double pct) {
                std::function<void(const DownloadProgress&)> cb;
                DownloadProgress prog = get_progress();
                prog.status = SessionStatus::ASSEMBLING;
                prog.progress_fraction = 0.70 + 0.30 * (pct / 100.0);
                {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    cb = progress_callback_;
                }
                if (cb) cb(prog);
            },
            &mux_err);

        uint64_t v_sz = fs::exists(video_tmp_p, ec) ? fs::file_size(video_tmp_p, ec) : 0;
        uint64_t out_sz = fs::exists(dest_p, ec) ? fs::file_size(dest_p, ec) : 0;

        if (mux_ok && out_sz >= static_cast<uint64_t>(v_sz * 0.90)) {
            fs::remove(video_tmp_p, ec);
            fs::remove(audio_tmp_p, ec);
        } else {
            fs::remove(dest_p, ec);
            status_ = SessionStatus::ERROR;
            {
                std::lock_guard<std::mutex> lock(meta_mutex_);
                last_error_ = mux_err.empty() ? "Audio/video muxing failed (tracks preserved for retry)." : mux_err;
            }
            std::cerr << "[OpenDM] MediaMuxer error: " << mux_err << std::endl;
            PostDownloadManager::instance().notify_completion(fn, sz, false, mux_err);
            update_speed_and_eta();
            notify_dirty();
            return;
        }
    }

    status_ = SessionStatus::COMPLETED;

    std::string lm_str;
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        lm_str = last_modified_;
    }
    if (!lm_str.empty()) {
        time_t t = curl_getdate(lm_str.c_str(), nullptr);
        if (t != -1) {
            auto ftime = std::chrono::file_clock::from_sys(std::chrono::system_clock::from_time_t(t));
            fs::last_write_time(dest_p, ftime, ec);
        }
    }

    update_speed_and_eta();
    notify_dirty();
    PostDownloadManager::instance().notify_completion(fn, sz, true);
    QueueManager::instance().track_background_task(std::async(std::launch::async, [dest_path]() {
        PostDownloadManager::instance().scan_file(dest_path);
    }));
}

void DownloadSession::update_speed_and_eta() {
    auto snapshot = chunk_manager_.get_chunks_snapshot();
    uint64_t total_spd = 0;

    for (const auto& c : snapshot) {
        if (c.status == ChunkStatus::DOWNLOADING) {
            total_spd += c.current_speed;
        }
    }

    if (status_ == SessionStatus::PAUSED || status_ == SessionStatus::IDLE || status_ == SessionStatus::COMPLETED) {
        current_speed_ = 0;
        eta_seconds_ = 0;
    } else {
        current_speed_ = total_spd;
        int64_t downloaded = chunk_manager_.get_total_downloaded_bytes();
        int64_t t_size = total_size_.load();

        if (t_size > 0 && total_spd > 0) {
            int64_t remaining = t_size - downloaded;
            eta_seconds_ = (remaining > 0) ? (remaining / static_cast<int64_t>(total_spd)) : 0;
        } else {
            eta_seconds_ = 0;
        }
    }

    ProgressCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = progress_callback_;
    }
    if (cb) {
        cb(get_progress());
    }
}

DownloadProgress DownloadSession::get_progress() const {
    DownloadProgress p;
    p.status = status_.load();
    p.total_size = total_size_.load();
    p.downloaded_bytes = chunk_manager_.get_total_downloaded_bytes();
    const bool halting = pause_requested_.load() || stop_requested_.load();
    p.speed_bytes_per_sec = halting ? 0 : current_speed_.load();
    p.eta_seconds = halting ? 0 : eta_seconds_.load();
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        p.error_message = last_error_;
    }
    p.chunks = chunk_manager_.get_chunks_snapshot();

    size_t active = 0;
    for (const auto& c : p.chunks) {
        if (c.status == ChunkStatus::DOWNLOADING) active++;
    }
    p.active_connections = active;

    int64_t t_size = total_size_.load();
    if (t_size > 0) {
        p.progress_fraction = std::clamp(static_cast<double>(p.downloaded_bytes) / t_size, 0.0, 1.0);
    } else {
        p.progress_fraction = 0.0;
    }

    return p;
}

std::string DownloadSession::serialize() const {
    std::string dest, eff, fn, cat, lm, et, aud, ua;
    bool custom_fn = false;
    std::map<std::string, std::string> hdrs;
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        dest = destination_path_;
        eff = effective_url_;
        fn = filename_;
        cat = category_;
        lm = last_modified_;
        et = etag_;
        aud = audio_stream_url_;
        custom_fn = custom_filename_set_;
        hdrs = custom_headers_;
        auto ua_it = custom_headers_.find("User-Agent");
        if (ua_it != custom_headers_.end()) {
            ua = ua_it->second;
        }
    }

    nlohmann::json cm_json;
    try {
        cm_json = nlohmann::json::parse(chunk_manager_.serialize_state());
    } catch (...) {
        cm_json = nlohmann::json::object();
    }

    nlohmann::json root = {
        {"id", id_},
        {"url", url_},
        {"effective_url", eff},
        {"user_agent", ua},
        {"audio_stream_url", aud},
        {"custom_headers", hdrs},
        {"destination_path", dest},
        {"temp_dir", temp_dir_},
        {"filename", fn},
        {"custom_filename_set", custom_fn},
        {"category", cat},
        {"last_modified", lm},
        {"etag", et},
        {"status", static_cast<int>(status_.load())},
        {"max_connections", max_connections_.load(std::memory_order_relaxed)},
        {"supports_ranges", supports_ranges_.load()},
        {"total_size", total_size_.load()},
        {"chunk_manager", std::move(cm_json)}
    };
    return root.dump(2);
}

std::shared_ptr<DownloadSession> DownloadSession::deserialize(const std::string& json_str) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_str);
    } catch (...) {
        return nullptr;
    }
    if (!j.is_object()) return nullptr;

    std::string id = j.value("id", "");
    std::string url = j.value("url", "");
    if (id.empty() || url.empty()) return nullptr;

    std::string dest = j.value("destination_path", "");
    std::string temp = j.value("temp_dir", "");

    auto session = std::make_shared<DownloadSession>(id, url, dest, temp);

    if (j.contains("custom_headers") && j["custom_headers"].is_object()) {
        for (auto& [k, v] : j["custom_headers"].items()) {
            if (v.is_string()) {
                session->set_header(k, v.get<std::string>());
            }
        }
    }
    std::string ua = j.value("user_agent", "");
    if (!ua.empty()) session->set_header("User-Agent", ua);

    std::string aud = j.value("audio_stream_url", "");
    if (!aud.empty()) session->set_audio_stream_url(aud);

    std::string cat = j.value("category", "");
    if (!cat.empty()) session->set_category(cat);

    std::string fn = j.value("filename", "");
    bool custom_fn_set = j.value("custom_filename_set", false);
    if (!fn.empty()) {
        std::lock_guard<std::mutex> lock(session->meta_mutex_);
        session->filename_ = fn;
        session->custom_filename_set_ = custom_fn_set;
        const std::string& dp = session->destination_path_;
        bool ends_with_fn = (dp == fn) ||
            (dp.size() > fn.size() &&
             (dp[dp.size() - fn.size() - 1] == '/' || dp[dp.size() - fn.size() - 1] == '\\') &&
             dp.compare(dp.size() - fn.size(), fn.size(), fn) == 0);
        if (!ends_with_fn) {
            fs::path parent = sys::path_from_utf8(dp).parent_path();
            session->destination_path_ = parent.empty() ? fn : sys::path_to_utf8(parent / sys::path_from_utf8(fn));
        }
    }

    {
        std::lock_guard<std::mutex> lock(session->meta_mutex_);
        session->last_modified_ = j.value("last_modified", "");
        session->etag_ = j.value("etag", "");
        session->effective_url_ = j.value("effective_url", "");
    }

    session->total_size_ = j.value("total_size", int64_t{-1});
    if (j.contains("max_connections") && j["max_connections"].is_number_unsigned()) {
        session->set_max_connections(j["max_connections"].get<size_t>());
    }
    int st_val = j.value("status", 0);
    if (st_val >= 0 && st_val <= static_cast<int>(SessionStatus::ERROR)) {
        session->status_ = static_cast<SessionStatus>(st_val);
    }
    session->supports_ranges_ = j.value("supports_ranges", false);

    if (j.contains("chunk_manager") && j["chunk_manager"].is_object()) {
        session->chunk_manager_.deserialize_state(j["chunk_manager"].dump());
    }

    return session;
}

} // namespace opendm
