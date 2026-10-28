#include "download_session.hpp"
#include "post_download.hpp"
#include "core/media_muxer.hpp"
#include "core/stream_extractor.hpp"
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <chrono>
#include <regex>
#include <cstring>

namespace opendm {

namespace {

size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total_size = size * nitems;
    std::string header(buffer, total_size);
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);

    size_t colon = header.find(':');
    if (colon != std::string::npos) {
        std::string key = header.substr(0, colon);
        std::string val = header.substr(colon + 1);
        // Trim leading and trailing whitespace
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) val.pop_back();
        // Lowercase key
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        (*headers)[key] = val;
    }
    return total_size;
}

struct WorkerData {
    DownloadSession* session{nullptr};
    int chunk_id{0};
    std::ofstream file;
    int64_t bytes_received{0};
    std::chrono::steady_clock::time_point last_speed_time;
    int64_t bytes_since_last_speed{0};
    uint64_t current_speed{0};
};

int xferinfo_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    auto* data = static_cast<WorkerData*>(clientp);
    if (!data || !data->session) return 1;

    if (data->session->is_stop_requested() || data->session->is_pause_requested() ||
        data->session->get_status() == SessionStatus::PAUSED ||
        data->session->get_status() == SessionStatus::IDLE) {
        return 1; // Abort immediately with CURLE_ABORTED_BY_CALLBACK
    }
    return 0;
}

int probe_xferinfo_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    auto* session = static_cast<DownloadSession*>(clientp);
    if (!session) return 1;

    if (session->is_stop_requested() || session->is_pause_requested() ||
        session->get_status() == SessionStatus::PAUSED ||
        session->get_status() == SessionStatus::IDLE) {
        return 1; // Abort probe immediately
    }
    return 0;
}

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* data = static_cast<WorkerData*>(userdata);
    size_t bytes = size * nmemb;

    if (!data || !data->session) return 0;

    if (data->session->is_stop_requested() || data->session->is_pause_requested() ||
        data->session->get_status() == SessionStatus::PAUSED ||
        data->session->get_status() == SessionStatus::IDLE) {
        return 0; // Abort transfer cleanly with CURLE_WRITE_ERROR
    }

    // Dynamic chunk boundary enforcement (OnChangeBounds in IDM)
    int64_t start_off = 0, end_off = -1;
    if (data->session->get_chunk_bounds(data->chunk_id, start_off, end_off) && end_off >= start_off) {
        int64_t max_bytes = end_off - start_off + 1;
        if (data->bytes_received >= max_bytes) {
            return 0; // Boundary reached, terminate chunk connection cleanly
        }
        if (data->bytes_received + static_cast<int64_t>(bytes) > max_bytes) {
            bytes = static_cast<size_t>(max_bytes - data->bytes_received);
        }
    }

    if (data->file.is_open() && bytes > 0) {
        data->file.write(ptr, bytes);
        data->bytes_received += bytes;
        data->bytes_since_last_speed += bytes;

        // Calculate speed every 250ms
        auto now = std::chrono::steady_clock::now();
        auto diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - data->last_speed_time).count();
        if (diff_ms >= 250) {
            data->current_speed = (data->bytes_since_last_speed * 1000) / diff_ms;
            data->bytes_since_last_speed = 0;
            data->last_speed_time = now;
        } else if (data->current_speed == 0 && diff_ms > 0 && data->bytes_since_last_speed > 0) {
            data->current_speed = (data->bytes_since_last_speed * 1000) / diff_ms;
        }

        data->session->set_chunk_progress(data->chunk_id, data->bytes_received, data->current_speed);
        data->session->apply_throttle(bytes);
    }

    if (end_off >= start_off) {
        int64_t max_bytes = end_off - start_off + 1;
        if (data->bytes_received >= max_bytes) {
            return 0; // Chunk fully downloaded up to split boundary
        }
    }

    return bytes;
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
    speed_limiter_.throttle(bytes);
}

void DownloadSession::set_header(const std::string& name, const std::string& value) {
    custom_headers_[name] = value;
}

void DownloadSession::set_credentials(const std::string& username, const std::string& password) {
    if (!username.empty()) {
        auth_userpwd_ = username + ":" + password;
    } else {
        auth_userpwd_.clear();
    }
}

DownloadSession::DownloadSession(const std::string& id, const std::string& url,
                                 const std::string& dest_path, const std::string& temp_dir)
    : id_(id), url_(url), destination_path_(dest_path) {
    
    if (temp_dir.empty()) {
        const char* home = getenv("HOME");
        std::filesystem::path base_cache = home ? (std::filesystem::path(home) / ".cache" / "opendm" / "chunks")
                                                : (std::filesystem::temp_directory_path() / "opendm_chunks");
        temp_dir_ = (base_cache / id_).string();
    } else {
        temp_dir_ = temp_dir;
    }

    // Extract filename from dest_path or URL
    std::filesystem::path p(dest_path);
    filename_ = p.filename().string();
    if (filename_.empty()) {
        size_t last_slash = url_.find_last_of("/\\?#");
        if (last_slash != std::string::npos && last_slash + 1 < url_.length()) {
            filename_ = url_.substr(last_slash + 1);
        } else {
            filename_ = "download.bin";
        }
    }
}

DownloadSession::~DownloadSession() {
    cancel();
}

void DownloadSession::set_max_connections(size_t max_conns) {
    max_connections_ = std::clamp<size_t>(max_conns, 1, 32);
}

size_t DownloadSession::get_max_connections() const {
    return max_connections_;
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
    filename_ = fname;
    custom_filename_set_ = true;
    std::filesystem::path p(destination_path_);
    destination_path_ = (p.parent_path() / fname).string();
}

bool DownloadSession::start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    SessionStatus st = status_.load();
    if (st == SessionStatus::DOWNLOADING || st == SessionStatus::PROBING || st == SessionStatus::ASSEMBLING) {
        return false;
    }

    if (coordinator_thread_.joinable()) {
        coordinator_thread_.join();
    }

    stop_requested_ = false;
    pause_requested_ = false;
    status_ = SessionStatus::PROBING;

    coordinator_thread_ = std::thread(&DownloadSession::run_session, this);
    return true;
}

void DownloadSession::pause() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    SessionStatus st = status_.load();
    if (st == SessionStatus::PAUSED || st == SessionStatus::COMPLETED || st == SessionStatus::IDLE) {
        return;
    }
    pause_requested_ = true;

    // Reset speeds on chunks for instant feedback
    auto chunks = chunk_manager_.get_chunks_snapshot();
    for (const auto& c : chunks) {
        chunk_manager_.update_chunk_progress(c.id, c.downloaded_bytes, 0);
    }

    if (coordinator_thread_.joinable()) {
        coordinator_thread_.join();
    }

    status_ = SessionStatus::PAUSED;
    current_speed_ = 0;
    eta_seconds_ = 0;
    update_speed_and_eta();
}

void DownloadSession::resume() {
    if (status_.load() == SessionStatus::PAUSED) {
        start();
    }
}

void DownloadSession::cancel() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    stop_requested_ = true;
    pause_requested_ = false;

    if (coordinator_thread_.joinable()) {
        coordinator_thread_.join();
    }

    {
        std::lock_guard<std::mutex> wlock(workers_mutex_);
        for (auto& t : worker_threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        worker_threads_.clear();
    }

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

bool DownloadSession::probe_resource() {
    // If url_ is a web streaming URL (e.g. YouTube, Vimeo, TikTok) and has not been resolved,
    // invoke StreamExtractor to obtain the direct video and audio streams.
    if (StreamExtractor::is_streaming_url(url_) && audio_stream_url_.empty() && StreamExtractor::is_available()) {
        StreamMediaInfo info;
        std::string err;
        if (StreamExtractor::extract_info(url_, info, &err) && !info.formats.empty()) {
            const auto& best = info.formats[0];
            url_ = best.video_url;
            audio_stream_url_ = best.audio_url;
            if (best.filesize > 0) total_size_ = best.filesize;

            std::lock_guard<std::mutex> lock(meta_mutex_);
            if (!custom_filename_set_ || filename_ == "download.bin" || filename_.find("watch?") != std::string::npos || filename_.find("video_download") != std::string::npos) {
                filename_ = StreamExtractor::sanitize_filename(info.title, best.ext);
                std::filesystem::path p(destination_path_);
                destination_path_ = (p.parent_path() / filename_).string();
            }
        }
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        last_error_ = "Failed to initialize CURL probe";
        return false;
    }

    std::map<std::string, std::string> resp_headers;
    curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, probe_xferinfo_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-0"); // Probe range support with single byte
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp_headers);

    const std::string default_ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
    std::string ua = default_ua;
    auto ua_it = custom_headers_.find("User-Agent");
    if (ua_it != custom_headers_.end()) {
        ua = ua_it->second;
    }
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());

    if (!auth_userpwd_.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth_userpwd_.c_str());
    }

    // Add custom headers if specified
    struct curl_slist* header_list = nullptr;
    for (const auto& [k, v] : custom_headers_) {
        std::string h = k + ": " + v;
        header_list = curl_slist_append(header_list, h.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (header_list) {
        curl_slist_free_all(header_list);
    }
    curl_easy_cleanup(curl);

    if (res != CURLE_OK && res != CURLE_PARTIAL_FILE) {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        last_error_ = std::string("Probe network error: ") + curl_easy_strerror(res);
        return false;
    }

    // Check response code
    if (http_code == 206) {
        supports_ranges_ = true;
        // Parse Content-Range: bytes 0-0/1234567
        auto it = resp_headers.find("content-range");
        if (it != resp_headers.end()) {
            size_t slash = it->second.find('/');
            if (slash != std::string::npos) {
                try {
                    total_size_ = std::stoll(it->second.substr(slash + 1));
                } catch (...) {
                    total_size_ = -1;
                }
            }
        }
    } else if (http_code == 200) {
        supports_ranges_ = false;
        auto it = resp_headers.find("content-length");
        if (it != resp_headers.end()) {
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

    // Extract filename from Content-Disposition if present
    auto cd_it = resp_headers.find("content-disposition");
    if (cd_it != resp_headers.end()) {
        std::regex fn_regex(R"(filename\*?=(?:UTF-8'')?["']?([^"';]+)["']?)", std::regex::icase);
        std::smatch match;
        if (std::regex_search(cd_it->second, match, fn_regex) && match.size() > 1) {
            std::string extracted_fn = match[1].str();
            if (!extracted_fn.empty()) {
                // Strip directory components to prevent path traversal (e.g. "../../etc/passwd")
                extracted_fn = std::filesystem::path(extracted_fn).filename().string();
                extracted_fn.erase(std::remove_if(extracted_fn.begin(), extracted_fn.end(),
                    [](char c) { return c == '\0' || c == '/' || c == '\\'; }), extracted_fn.end());

                if (!extracted_fn.empty()) {
                    std::lock_guard<std::mutex> lock(meta_mutex_);
                    if (!custom_filename_set_) {
                        filename_ = extracted_fn;
                        std::filesystem::path p(destination_path_);
                        destination_path_ = (p.parent_path() / filename_).string();
                    }
                }
            }
        }
    }

    // Capture server timestamp (Last-Modified) and ETag
    auto lm_it = resp_headers.find("last-modified");
    if (lm_it != resp_headers.end()) {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        last_modified_ = lm_it->second;
    }
    auto etag_it = resp_headers.find("etag");
    if (etag_it != resp_headers.end()) {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        etag_ = etag_it->second;
    }

    // Check available disk space before downloading
    if (total_size_.load() > 0) {
        std::string check_dest;
        {
            std::lock_guard<std::mutex> lock(meta_mutex_);
            check_dest = destination_path_;
        }
        std::filesystem::path parent = std::filesystem::path(check_dest).parent_path();
        if (parent.empty()) parent = ".";
        std::error_code ec;
        auto space_info = std::filesystem::space(parent, ec);
        if (!ec) {
            if (space_info.available < static_cast<uintmax_t>(total_size_.load())) {
                std::lock_guard<std::mutex> lock(meta_mutex_);
                last_error_ = "Not enough disk space on target drive. Required: " +
                              std::to_string(total_size_.load() / (1024 * 1024)) + " MB, Available: " +
                              std::to_string(space_info.available / (1024 * 1024)) + " MB";
                return false;
            }
        }
    }

    return true;
}

void DownloadSession::run_session() {
    if (!probe_resource()) {
        if (stop_requested_ || pause_requested_) {
            return;
        }
        status_ = SessionStatus::ERROR;
        update_speed_and_eta();
        return;
    }

    if (stop_requested_ || pause_requested_) {
        return;
    }

    // Initialize or resume chunks
    auto existing_chunks = chunk_manager_.get_chunks_snapshot();
    if (!supports_ranges_) {
        // Non-resumable stream: always restart fresh chunk
        chunk_manager_.init_stream_chunk(temp_dir_, id_);
    } else if (existing_chunks.empty()) {
        if (total_size_ > 0) {
            chunk_manager_.init_first_chunk(total_size_, temp_dir_, id_);
        } else {
            chunk_manager_.init_stream_chunk(temp_dir_, id_);
        }
    }

    if (chunk_manager_.all_chunks_completed()) {
        assemble_file();
        return;
    }

    status_ = SessionStatus::DOWNLOADING;
    update_speed_and_eta();

    // Launch worker thread(s) for all incomplete chunks
    existing_chunks = chunk_manager_.get_chunks_snapshot();
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        worker_threads_.clear();
        for (const auto& c : existing_chunks) {
            if (c.status != ChunkStatus::COMPLETED) {
                chunk_manager_.set_chunk_status(c.id, ChunkStatus::DOWNLOADING);
                worker_threads_.emplace_back(&DownloadSession::download_chunk_worker, this, c.id);
            }
        }
    }

    // Coordinator loop: monitor progress, ETA, and dynamically split chunks as in IDM
    dynamic_split_loop();

    // Wait for all workers to finish or abort
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        for (auto& t : worker_threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        worker_threads_.clear();
    }

    if (stop_requested_ || pause_requested_) {
        if (pause_requested_) {
            status_ = SessionStatus::PAUSED;
            current_speed_ = 0;
            eta_seconds_ = 0;
            update_speed_and_eta();
        }
        return;
    }

    if (chunk_manager_.all_chunks_completed()) {
        assemble_file();
    } else {
        status_ = SessionStatus::ERROR;
        last_error_ = "One or more chunks failed to complete.";
        update_speed_and_eta();
    }
}

void DownloadSession::download_chunk_worker(int chunk_id) {
    Chunk chunk;
    if (!chunk_manager_.get_chunk(chunk_id, chunk)) {
        return;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        chunk_manager_.set_chunk_status(chunk_id, ChunkStatus::FAILED);
        return;
    }

    WorkerData data;
    data.session = this;
    data.chunk_id = chunk_id;
    data.last_speed_time = std::chrono::steady_clock::now();

    // Open file in append or binary mode
    std::ios_base::openmode mode = std::ios::binary | std::ios::out;
    if (chunk.downloaded_bytes > 0) {
        mode |= std::ios::app;
        data.bytes_received = chunk.downloaded_bytes;
    } else {
        mode |= std::ios::trunc;
    }
    data.file.open(chunk.temp_filepath, mode);

    if (!data.file.is_open()) {
        chunk_manager_.set_chunk_status(chunk_id, ChunkStatus::FAILED);
        curl_easy_cleanup(curl);
        return;
    }

    chunk_manager_.set_chunk_status(chunk_id, ChunkStatus::DOWNLOADING);

    curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &data);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);

    const std::string default_ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
    std::string ua = default_ua;
    auto ua_it = custom_headers_.find("User-Agent");
    if (ua_it != custom_headers_.end()) {
        ua = ua_it->second;
    }
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());

    // Configure range header if supported
    if (supports_ranges_ && chunk.end_offset >= 0) {
        int64_t start_pos = chunk.start_offset + chunk.downloaded_bytes;
        std::string range_header = std::to_string(start_pos) + "-" + std::to_string(chunk.end_offset);
        curl_easy_setopt(curl, CURLOPT_RANGE, range_header.c_str());
    }

    if (!auth_userpwd_.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth_userpwd_.c_str());
    }

    struct curl_slist* header_list = nullptr;
    for (const auto& [k, v] : custom_headers_) {
        std::string h = k + ": " + v;
        header_list = curl_slist_append(header_list, h.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (header_list) {
        curl_slist_free_all(header_list);
    }
    curl_easy_cleanup(curl);
    data.file.flush();
    data.file.close();

    if (stop_requested_ || pause_requested_) {
        if (chunk.status != ChunkStatus::COMPLETED) {
            chunk_manager_.set_chunk_status(chunk_id, ChunkStatus::PENDING);
        }
        return;
    }

    Chunk latest_chunk;
    bool has_c = chunk_manager_.get_chunk(chunk_id, latest_chunk);
    bool reached_boundary = has_c && (latest_chunk.end_offset >= latest_chunk.start_offset) &&
                            (latest_chunk.downloaded_bytes >= (latest_chunk.end_offset - latest_chunk.start_offset + 1));

    // Reset chunk speed on finish/failure so speed doesn't get stuck
    chunk_manager_.update_chunk_progress(chunk_id, has_c ? latest_chunk.downloaded_bytes : 0, 0);

    bool http_ok = (http_code >= 200 && http_code < 300);
    if (http_ok && (res == CURLE_OK || (res == CURLE_PARTIAL_FILE && http_code == 206) ||
        (res == CURLE_WRITE_ERROR && reached_boundary))) {
        chunk_manager_.mark_chunk_completed(chunk_id);
    } else {
        chunk_manager_.set_chunk_status(chunk_id, ChunkStatus::FAILED);
    }
}

void DownloadSession::dynamic_split_loop() {
    while (!stop_requested_ && !pause_requested_) {
        update_speed_and_eta();

        if (chunk_manager_.all_chunks_completed()) {
            break;
        }

        // Count active workers
        size_t active_workers = 0;
        auto snapshot = chunk_manager_.get_chunks_snapshot();
        for (const auto& c : snapshot) {
            if (c.status == ChunkStatus::DOWNLOADING) {
                active_workers++;
            }
        }

        // Check IDM condition: NeedNewConnection()
        if (supports_ranges_.load() && chunk_manager_.need_new_connection(active_workers, max_connections_)) {
            int target_chunk = chunk_manager_.find_max_free_chunk();
            if (target_chunk != -1) {
                int new_id = chunk_manager_.split_chunk(target_chunk, temp_dir_, id_);
                if (new_id != -1) {
                    chunk_manager_.set_chunk_status(new_id, ChunkStatus::DOWNLOADING);
                    std::lock_guard<std::mutex> lock(workers_mutex_);
                    worker_threads_.emplace_back(&DownloadSession::download_chunk_worker, this, new_id);
                }
            }
        }

        // Fast sleep in 10ms intervals for rapid pause/cancel response
        for (int i = 0; i < 10 && !stop_requested_ && !pause_requested_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void DownloadSession::assemble_file() {
    status_ = SessionStatus::ASSEMBLING;
    update_speed_and_eta();

    std::string dest_path;
    std::string fn;
    int64_t sz = total_size_.load();
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        dest_path = destination_path_;
        fn = filename_;
    }

    bool ok = chunk_manager_.join_file(dest_path, [this](double p) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (progress_callback_) {
            DownloadProgress prog = get_progress();
            prog.status = SessionStatus::ASSEMBLING;
            prog.progress_fraction = p;
            progress_callback_(prog);
        }
    });

    if (ok) {
        // If an audio stream URL is present (adaptive streaming / separate tracks),
        // download the audio stream and multiplex into the final destination_path.
        std::string audio_url;
        {
            std::lock_guard<std::mutex> lock(meta_mutex_);
            audio_url = audio_stream_url_;
        }
        if (!audio_url.empty()) {
            namespace fs = std::filesystem;
            std::string video_tmp = dest_path + ".video.tmp";
            std::string audio_tmp = dest_path + ".audio.tmp";
            std::error_code ec;
            fs::rename(dest_path, video_tmp, ec);
            if (!ec) {
                CURL* acurl = curl_easy_init();
                if (acurl) {
                    FILE* afp = fopen(audio_tmp.c_str(), "wb");
                    if (afp) {
                        curl_easy_setopt(acurl, CURLOPT_URL, audio_url.c_str());
                        curl_easy_setopt(acurl, CURLOPT_FOLLOWLOCATION, 1L);
                        curl_easy_setopt(acurl, CURLOPT_WRITEDATA, afp);
                        const std::string default_ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
                        std::string ua = default_ua;
                        auto ua_it = custom_headers_.find("User-Agent");
                        if (ua_it != custom_headers_.end()) {
                            ua = ua_it->second;
                        }
                        curl_easy_setopt(acurl, CURLOPT_USERAGENT, ua.c_str());
                        curl_easy_setopt(acurl, CURLOPT_CONNECTTIMEOUT, 15L);
                        CURLcode ares = curl_easy_perform(acurl);
                        fclose(afp);
                        if (ares == CURLE_OK) {
                            std::string mux_err;
                            bool mux_ok = MediaMuxer::mux_streams(video_tmp, audio_tmp, dest_path, nullptr, &mux_err);
                            if (mux_ok) {
                                fs::remove(video_tmp, ec);
                                fs::remove(audio_tmp, ec);
                            } else {
                                fs::rename(video_tmp, dest_path, ec);
                            }
                        } else {
                            fs::rename(video_tmp, dest_path, ec);
                        }
                    } else {
                        fs::rename(video_tmp, dest_path, ec);
                    }
                    curl_easy_cleanup(acurl);
                } else {
                    fs::rename(video_tmp, dest_path, ec);
                }
            }
        }

        status_ = SessionStatus::COMPLETED;

        // Preserve remote server modification timestamp if captured
        std::string lm_str;
        {
            std::lock_guard<std::mutex> lock(meta_mutex_);
            lm_str = last_modified_;
        }
        if (!lm_str.empty()) {
            std::tm tm{};
            if (strptime(lm_str.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm) != nullptr) {
                time_t t = timegm(&tm);
                if (t != -1) {
                    std::error_code ec;
                    auto ftime = std::chrono::file_clock::from_sys(std::chrono::system_clock::from_time_t(t));
                    std::filesystem::last_write_time(dest_path, ftime, ec);
                }
            }
        }

        PostDownloadManager::instance().notify_completion(fn, sz, true);
        PostDownloadManager::instance().scan_file(dest_path);
    } else {
        status_ = SessionStatus::ERROR;
        {
            std::lock_guard<std::mutex> lock(meta_mutex_);
            last_error_ = "Failed to assemble file parts.";
        }
        PostDownloadManager::instance().notify_completion(fn, sz, false, "Failed to assemble file parts.");
    }

    update_speed_and_eta();
}

void DownloadSession::update_speed_and_eta() {
    auto snapshot = chunk_manager_.get_chunks_snapshot();
    uint64_t total_spd = 0;
    size_t active_conns = 0;

    for (const auto& c : snapshot) {
        if (c.status == ChunkStatus::DOWNLOADING) {
            total_spd += c.current_speed;
            active_conns++;
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
            eta_seconds_ = (remaining > 0) ? (remaining / total_spd) : 0;
        } else {
            eta_seconds_ = 0;
        }
    }

    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (progress_callback_) {
        progress_callback_(get_progress());
    }
}

DownloadProgress DownloadSession::get_progress() const {
    DownloadProgress p;
    p.status = status_.load();
    p.total_size = total_size_.load();
    p.downloaded_bytes = chunk_manager_.get_total_downloaded_bytes();
    p.speed_bytes_per_sec = current_speed_.load();
    p.eta_seconds = eta_seconds_.load();
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
    std::string dest, fn, cat, lm, et, aud;
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        dest = destination_path_;
        fn = filename_;
        cat = category_;
        lm = last_modified_;
        et = etag_;
        aud = audio_stream_url_;
    }

    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"id\": \"" << id_ << "\",\n";
    ss << "  \"url\": \"" << url_ << "\",\n";
    ss << "  \"audio_stream_url\": \"" << aud << "\",\n";
    ss << "  \"destination_path\": \"" << dest << "\",\n";
    ss << "  \"temp_dir\": \"" << temp_dir_ << "\",\n";
    ss << "  \"filename\": \"" << fn << "\",\n";
    ss << "  \"category\": \"" << cat << "\",\n";
    ss << "  \"last_modified\": \"" << lm << "\",\n";
    ss << "  \"etag\": \"" << et << "\",\n";
    ss << "  \"status\": " << static_cast<int>(status_.load()) << ",\n";
    ss << "  \"max_connections\": " << max_connections_ << ",\n";
    ss << "  \"supports_ranges\": " << (supports_ranges_.load() ? "true" : "false") << ",\n";
    ss << "  \"total_size\": " << total_size_.load() << ",\n";
    ss << "  \"chunk_manager\": " << chunk_manager_.serialize_state() << "\n";
    ss << "}\n";
    return ss.str();
}

std::shared_ptr<DownloadSession> DownloadSession::deserialize(const std::string& json_str) {
    auto get_str = [&json_str](const std::string& key) -> std::string {
        size_t pos = json_str.find("\"" + key + "\":");
        if (pos == std::string::npos) return "";
        pos = json_str.find('\"', pos + key.length() + 2);
        if (pos == std::string::npos) return "";
        size_t end = json_str.find('\"', pos + 1);
        if (end == std::string::npos) return "";
        return json_str.substr(pos + 1, end - pos - 1);
    };

    std::string id = get_str("id");
    std::string url = get_str("url");
    std::string dest = get_str("destination_path");
    std::string temp = get_str("temp_dir");
    std::string cat = get_str("category");
    std::string fn = get_str("filename");
    std::string lm = get_str("last_modified");
    std::string et = get_str("etag");
    std::string aud = get_str("audio_stream_url");

    if (id.empty() || url.empty()) return nullptr;

    auto session = std::make_shared<DownloadSession>(id, url, dest, temp);
    if (!aud.empty()) {
        session->set_audio_stream_url(aud);
    }
    if (!cat.empty()) {
        session->set_category(cat);
    }
    if (!fn.empty()) {
        session->set_custom_filename(fn);
    }
    {
        std::lock_guard<std::mutex> lock(session->meta_mutex_);
        session->last_modified_ = lm;
        session->etag_ = et;
    }

    size_t sz_pos = json_str.find("\"total_size\":");
    if (sz_pos != std::string::npos) {
        try {
            session->total_size_ = std::stoll(json_str.substr(sz_pos + 13));
        } catch (...) {}
    }

    size_t st_pos = json_str.find("\"status\":");
    if (st_pos != std::string::npos) {
        try {
            int st_val = std::stoi(json_str.substr(st_pos + 9));
            session->status_ = static_cast<SessionStatus>(st_val);
        } catch (...) {}
    }

    size_t rng_pos = json_str.find("\"supports_ranges\":");
    if (rng_pos != std::string::npos) {
        if (json_str.substr(rng_pos + 18, 4) == "true") {
            session->supports_ranges_ = true;
        }
    }

    size_t cm_pos = json_str.find("\"chunk_manager\":");
    if (cm_pos != std::string::npos) {
        session->chunk_manager_.deserialize_state(json_str.substr(cm_pos + 16));
    }
    return session;
}

} // namespace opendm
