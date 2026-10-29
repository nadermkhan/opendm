#include "download_session.hpp"
#include "post_download.hpp"
#include "core/media_muxer.hpp"
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
    size_t incoming_bytes = size * nmemb;
    if (!data || !data->session) return 0;

    if (data->session->is_stop_requested() || data->session->is_pause_requested() ||
        data->session->get_status() == SessionStatus::PAUSED ||
        data->session->get_status() == SessionStatus::IDLE) {
        return 0; // Abort transfer cleanly with CURLE_WRITE_ERROR
    }

    if (incoming_bytes == 0) return 0;

    // Calculate speed every 250ms
    data->bytes_since_last_speed += static_cast<int64_t>(incoming_bytes);
    auto now = std::chrono::steady_clock::now();
    auto diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - data->last_speed_time).count();
    if (diff_ms >= 250) {
        data->current_speed = (data->bytes_since_last_speed * 1000) / diff_ms;
        data->bytes_since_last_speed = 0;
        data->last_speed_time = now;
    } else if (data->current_speed == 0 && diff_ms > 0 && data->bytes_since_last_speed > 0) {
        data->current_speed = (data->bytes_since_last_speed * 1000) / diff_ms;
    }

    // Atomically clamp incoming write to dynamic split boundary and update chunk progress
    bool reached_boundary = false;
    size_t bytes_to_write = data->session->clamp_and_update_chunk(
        data->chunk_id, data->bytes_received, incoming_bytes, data->current_speed, reached_boundary);

    if (data->file.is_open() && bytes_to_write > 0) {
        data->file.write(ptr, static_cast<std::streamsize>(bytes_to_write));
        if (data->file.fail()) {
            return 0; // Disk error: abort transfer immediately
        }
        data->bytes_received += static_cast<int64_t>(bytes_to_write);
        data->session->apply_throttle(bytes_to_write);
    }

    if (reached_boundary) {
        return 0; // Terminate connection cleanly at exact split boundary
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
    speed_limiter_.throttle(bytes);
}

size_t DownloadSession::clamp_and_update_chunk(
    int chunk_id, int64_t current_bytes_received, size_t incoming_bytes,
    uint64_t speed, bool& out_reached_boundary) {
    return chunk_manager_.clamp_and_update_chunk(
        chunk_id, current_bytes_received, incoming_bytes, speed, out_reached_boundary);
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

bool DownloadSession::is_googlevideo_url(const std::string& target_url) {
    return (target_url.find("googlevideo.com") != std::string::npos ||
            target_url.find("videoplayback") != std::string::npos);
}

std::string DownloadSession::resolve_user_agent_for_url(const std::string& target_url) const {
    auto ua_it = custom_headers_.find("User-Agent");
    if (ua_it != custom_headers_.end() && !ua_it->second.empty()) {
        return ua_it->second;
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

bool DownloadSession::probe_resource() {
    if (is_googlevideo_url(url_) &&
        (url_.find("sabr=1") != std::string::npos || url_.find("sabr=2") != std::string::npos)) {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        last_error_ = "YouTube SABR stream URL rejected. Please reload video page to capture direct stream.";
        return false;
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
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

    std::string ua = resolve_user_agent_for_url(url_);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());

    if (!auth_userpwd_.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth_userpwd_.c_str());
    }

    bool is_mobile_yt = (url_.find("c=ANDROID_VR") != std::string::npos ||
                         url_.find("c=VISIONOS") != std::string::npos ||
                         url_.find("c=ANDROID_TESTSUITE") != std::string::npos ||
                         url_.find("c=IOS") != std::string::npos ||
                         url_.find("c=ANDROID") != std::string::npos);

    // Add custom headers if specified
    struct curl_slist* header_list = nullptr;
    for (const auto& [k, v] : custom_headers_) {
        std::string lk = k;
        std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
        if (lk == "user-agent") {
            continue;
        }
        if (lk == "referer" && is_mobile_yt) {
            continue;
        }
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

    // Check Content-Type for SABR control streams
    auto ct_it = resp_headers.find("content-type");
    if (ct_it != resp_headers.end()) {
        std::string ct = ct_it->second;
        std::transform(ct.begin(), ct.end(), ct.begin(), ::tolower);
        if (ct.find("yt-ump") != std::string::npos) {
            std::lock_guard<std::mutex> lock(meta_mutex_);
            last_error_ = "YouTube SABR control stream intercepted. Please reload video page to capture direct stream.";
            return false;
        }
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

    // Launch parallel audio download if separate audio stream is present
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

    if (audio_thread.joinable()) {
        audio_thread.join();
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

    // Synchronize downloaded_bytes with actual physical file size on disk to prevent resume corruption
    namespace fs = std::filesystem;
    std::error_code ec;
    int64_t disk_bytes = 0;
    if (fs::exists(chunk.temp_filepath, ec)) {
        uint64_t fsz = fs::file_size(chunk.temp_filepath, ec);
        if (!ec) disk_bytes = static_cast<int64_t>(fsz);
    }

    if (!supports_ranges_.load()) {
        disk_bytes = 0;
    } else if (chunk.end_offset >= chunk.start_offset) {
        int64_t max_chunk_sz = chunk.end_offset - chunk.start_offset + 1;
        if (disk_bytes > max_chunk_sz) {
            fs::resize_file(chunk.temp_filepath, static_cast<uintmax_t>(max_chunk_sz), ec);
            disk_bytes = max_chunk_sz;
        }
    }

    chunk_manager_.update_chunk_progress(chunk_id, disk_bytes, 0);
    data.bytes_received = disk_bytes;

    std::ios_base::openmode mode = std::ios::binary | std::ios::out;
    if (disk_bytes > 0) {
        mode |= std::ios::app;
    } else {
        mode |= std::ios::trunc;
    }

    data.file.open(chunk.temp_filepath, mode);
    if (!data.file.is_open()) {
        chunk_manager_.set_chunk_status(chunk_id, ChunkStatus::FAILED);
        curl_easy_cleanup(curl);
        return;
    }

    // Use a 1 MB file stream buffer per worker to reduce syscall overhead
    std::vector<char> file_buf(1024 * 1024);
    data.file.rdbuf()->pubsetbuf(file_buf.data(), file_buf.size());

    chunk_manager_.set_chunk_status(chunk_id, ChunkStatus::DOWNLOADING);

    int64_t start_pos = chunk.start_offset + disk_bytes;
    if (supports_ranges_.load() && chunk.end_offset >= chunk.start_offset && start_pos > chunk.end_offset) {
        data.file.close();
        curl_easy_cleanup(curl);
        chunk_manager_.mark_chunk_completed(chunk_id);
        return;
    }

    std::string ua = resolve_user_agent_for_url(url_);
    bool is_yt = is_googlevideo_url(url_);
    bool is_mobile_yt = (url_.find("c=ANDROID_VR") != std::string::npos ||
                         url_.find("c=VISIONOS") != std::string::npos ||
                         url_.find("c=ANDROID_TESTSUITE") != std::string::npos ||
                         url_.find("c=IOS") != std::string::npos ||
                         url_.find("c=ANDROID") != std::string::npos);

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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512L * 1024L);

    if (!auth_userpwd_.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth_userpwd_.c_str());
    }

    struct curl_slist* header_list = nullptr;
    for (const auto& [k, v] : custom_headers_) {
        std::string lk = k;
        std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
        if (lk == "user-agent") continue;
        if (lk == "referer" && is_mobile_yt) continue;
        std::string h = k + ": " + v;
        header_list = curl_slist_append(header_list, h.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    CURLcode res = CURLE_OK;
    long http_code = 0;

    // FIX: For YouTube / googlevideo.com ranged streams, fetch in 8 MB segments
    // on the SAME persistent CURL* handle to bypass CDN bandwidth throttling.
    constexpr int64_t YT_SEGMENT_SIZE = 8 * 1024 * 1024; // 8 MB

    if (is_yt && supports_ranges_.load() && chunk.end_offset >= 0) {
        while (!stop_requested_ && !pause_requested_) {
            Chunk cur_chunk;
            if (!chunk_manager_.get_chunk(chunk_id, cur_chunk)) break;

            int64_t cur_start = cur_chunk.start_offset + data.bytes_received;
            int64_t cur_end = cur_chunk.end_offset;
            if (cur_start > cur_end) {
                res = CURLE_OK;
                http_code = 206;
                break;
            }

            int64_t seg_end = std::min<int64_t>(cur_start + YT_SEGMENT_SIZE - 1, cur_end);
            std::string range_header = std::to_string(cur_start) + "-" + std::to_string(seg_end);
            curl_easy_setopt(curl, CURLOPT_RANGE, range_header.c_str());

            res = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            Chunk after_chunk;
            bool has_after = chunk_manager_.get_chunk(chunk_id, after_chunk);
            bool hit_split_bound = has_after &&
                (after_chunk.downloaded_bytes >= (after_chunk.end_offset - after_chunk.start_offset + 1));

            if (hit_split_bound) {
                res = CURLE_OK;
                http_code = 206;
                break;
            }
            if (res != CURLE_OK || (http_code != 206 && !(cur_start == 0 && http_code == 200))) {
                break;
            }
        }
    } else {
        if (supports_ranges_.load() && chunk.end_offset >= 0) {
            std::string range_header = std::to_string(start_pos) + "-" + std::to_string(chunk.end_offset);
            curl_easy_setopt(curl, CURLOPT_RANGE, range_header.c_str());
        }
        res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }

    if (header_list) {
        curl_slist_free_all(header_list);
    }
    curl_easy_cleanup(curl);

    data.file.flush();
    bool file_ok = !data.file.fail();
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

    chunk_manager_.update_chunk_progress(chunk_id, has_c ? latest_chunk.downloaded_bytes : 0, 0);

    // Reject HTTP 200 if we requested a non-zero offset (prevents appending full file from byte 0)
    bool range_honored = (start_pos == 0) ? (http_code == 200 || http_code == 206) : (http_code == 206);
    if (file_ok && range_honored &&
        (res == CURLE_OK || (res == CURLE_PARTIAL_FILE && http_code == 206) ||
         (res == CURLE_WRITE_ERROR && reached_boundary))) {
        chunk_manager_.mark_chunk_completed(chunk_id);
    } else {
        std::cerr << "OpenDM Worker " << chunk_id << " failed: HTTP " << http_code
                  << ", curl_res: " << curl_easy_strerror(res) << std::endl;
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

        // Allow up to 6 concurrent segmented connections on Google Video (instead of 2)
        size_t effective_max_conns = max_connections_;
        if (is_googlevideo_url(url_)) {
            effective_max_conns = std::min<size_t>(max_connections_, 6);
        }

        // Check IDM condition: NeedNewConnection()
        if (supports_ranges_.load() && chunk_manager_.need_new_connection(active_workers, effective_max_conns)) {
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

bool DownloadSession::download_audio_stream_to_file(const std::string& audio_url, const std::string& audio_tmp_path) {
    if (audio_url.empty()) return false;

    std::string ua = resolve_user_agent_for_url(audio_url);
    bool is_audio_mobile_yt = (audio_url.find("c=ANDROID_VR") != std::string::npos ||
                               audio_url.find("c=VISIONOS") != std::string::npos ||
                               audio_url.find("c=ANDROID_TESTSUITE") != std::string::npos ||
                               audio_url.find("c=IOS") != std::string::npos ||
                               audio_url.find("c=ANDROID") != std::string::npos);

    std::ofstream afp(audio_tmp_path, std::ios::binary | std::ios::trunc);
    if (!afp.is_open()) return false;

    std::vector<char> file_buf(512 * 1024);
    afp.rdbuf()->pubsetbuf(file_buf.data(), file_buf.size());

    CURL* acurl = curl_easy_init();
    if (!acurl) return false;

    constexpr int64_t SEG_SIZE = 8 * 1024 * 1024;
    int64_t cur_off = 0;
    int64_t total_audio_sz = -1;
    bool audio_dl_ok = true;

    curl_easy_setopt(acurl, CURLOPT_URL, audio_url.c_str());
    curl_easy_setopt(acurl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(acurl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(acurl, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(acurl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(acurl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(acurl, CURLOPT_BUFFERSIZE, 512L * 1024L);
    curl_easy_setopt(acurl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(acurl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(acurl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(acurl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(acurl, CURLOPT_XFERINFOFUNCTION, probe_xferinfo_callback);
    curl_easy_setopt(acurl, CURLOPT_XFERINFODATA, this);

    if (!auth_userpwd_.empty()) {
        curl_easy_setopt(acurl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
        curl_easy_setopt(acurl, CURLOPT_USERPWD, auth_userpwd_.c_str());
    }

    struct curl_slist* aheaders = nullptr;
    for (const auto& [k, v] : custom_headers_) {
        std::string lk = k;
        std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
        if (lk == "user-agent") continue;
        if (lk == "referer" && is_audio_mobile_yt) continue;
        aheaders = curl_slist_append(aheaders, (k + ": " + v).c_str());
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

    std::map<std::string, std::string> a_resp_headers;
    curl_easy_setopt(acurl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(acurl, CURLOPT_HEADERDATA, &a_resp_headers);

    while (!stop_requested_ && !pause_requested_) {
        a_resp_headers.clear();
        int64_t end_off = (total_audio_sz > 0)
            ? std::min<int64_t>(cur_off + SEG_SIZE - 1, total_audio_sz - 1)
            : (cur_off + SEG_SIZE - 1);
        std::string rng = std::to_string(cur_off) + "-" + std::to_string(end_off);
        curl_easy_setopt(acurl, CURLOPT_RANGE, rng.c_str());

        int64_t seg_start = cur_off;
        CURLcode ares = curl_easy_perform(acurl);
        long http_code = 0;
        curl_easy_getinfo(acurl, CURLINFO_RESPONSE_CODE, &http_code);

        if (ares != CURLE_OK || (http_code != 200 && http_code != 206)) {
            audio_dl_ok = false;
            break;
        }

        if (total_audio_sz < 0) {
            auto cr_it = a_resp_headers.find("content-range");
            if (cr_it != a_resp_headers.end()) {
                size_t sl = cr_it->second.find('/');
                if (sl != std::string::npos) {
                    try { total_audio_sz = std::stoll(cr_it->second.substr(sl + 1)); } catch (...) {}
                }
            }
        }

        cur_off = static_cast<int64_t>(afp.tellp());
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

    if (stop_requested_ || pause_requested_) {
        std::error_code ec;
        std::filesystem::remove(audio_tmp_path, ec);
        return false;
    }

    return audio_dl_ok && (total_audio_sz <= 0 || cur_off >= total_audio_sz);
}

void DownloadSession::assemble_file() {
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

    double join_weight = audio_url.empty() ? 1.0 : 0.70;
    bool ok = chunk_manager_.join_file(dest_path, [this, join_weight](double p) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (progress_callback_) {
            DownloadProgress prog = get_progress();
            prog.status = SessionStatus::ASSEMBLING;
            prog.progress_fraction = p * join_weight;
            progress_callback_(prog);
        }
    });

    if (!ok) {
        status_ = SessionStatus::ERROR;
        {
            std::lock_guard<std::mutex> lock(meta_mutex_);
            last_error_ = "Failed to assemble file parts.";
        }
        PostDownloadManager::instance().notify_completion(fn, sz, false, "Failed to assemble file parts.");
        update_speed_and_eta();
        return;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(dest_path, ec) && fs::file_size(dest_path, ec) <= 64) {
        std::ifstream chk(dest_path, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(chk)), std::istreambuf_iterator<char>());
        if (body.find("sabr.malformed_config") != std::string::npos) {
            fs::remove(dest_path, ec);
            status_ = SessionStatus::ERROR;
            {
                std::lock_guard<std::mutex> lock(meta_mutex_);
                last_error_ = "YouTube SABR control response intercepted instead of media stream";
            }
            update_speed_and_eta();
            return;
        }
    }

    // Mux separate adaptive audio track if present
    if (!audio_url.empty()) {
        std::string video_tmp = dest_path + ".video.tmp";
        std::string audio_tmp = dest_path + ".audio.tmp";

        bool audio_dl_ok = audio_download_complete_.load();
        if (!audio_dl_ok) {
            audio_dl_ok = download_audio_stream_to_file(audio_url, audio_tmp);
        }

        if (stop_requested_ || pause_requested_) {
            fs::remove(audio_tmp, ec);
            return;
        }

        if (audio_dl_ok) {
            fs::rename(dest_path, video_tmp, ec);
            if (!ec) {
                std::string mux_err;
                bool mux_ok = MediaMuxer::mux_streams(
                    video_tmp, audio_tmp, dest_path,
                    [this](double pct) {
                        std::lock_guard<std::mutex> lock(callback_mutex_);
                        if (progress_callback_) {
                            DownloadProgress prog = get_progress();
                            prog.status = SessionStatus::ASSEMBLING;
                            prog.progress_fraction = 0.70 + 0.30 * (pct / 100.0);
                            progress_callback_(prog);
                        }
                    },
                    &mux_err);

                uint64_t v_sz = fs::exists(video_tmp, ec) ? fs::file_size(video_tmp, ec) : 0;
                uint64_t out_sz = fs::exists(dest_path, ec) ? fs::file_size(dest_path, ec) : 0;

                if (mux_ok && out_sz >= static_cast<uint64_t>(v_sz * 0.90)) {
                    fs::remove(video_tmp, ec);
                    fs::remove(audio_tmp, ec);
                } else {
                    fs::remove(dest_path, ec);
                    fs::rename(video_tmp, dest_path, ec);
                    fs::remove(audio_tmp, ec);
                    std::cerr << "[OpenDM] MediaMuxer warning: " << mux_err << std::endl;
                }
            }
        } else {
            fs::remove(audio_tmp, ec);
        }
    }

    status_ = SessionStatus::COMPLETED;

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
                auto ftime = std::chrono::file_clock::from_sys(std::chrono::system_clock::from_time_t(t));
                std::filesystem::last_write_time(dest_path, ftime, ec);
            }
        }
    }

    update_speed_and_eta();
    PostDownloadManager::instance().notify_completion(fn, sz, true);
    std::thread([dest_path]() {
        PostDownloadManager::instance().scan_file(dest_path);
    }).detach();
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
    std::string dest, fn, cat, lm, et, aud, ua;
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        dest = destination_path_;
        fn = filename_;
        cat = category_;
        lm = last_modified_;
        et = etag_;
        aud = audio_stream_url_;
        auto ua_it = custom_headers_.find("User-Agent");
        if (ua_it != custom_headers_.end()) {
            ua = ua_it->second;
        }
    }

    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"id\": \"" << id_ << "\",\n";
    ss << "  \"url\": \"" << url_ << "\",\n";
    ss << "  \"user_agent\": \"" << ua << "\",\n";
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
    std::string ua = get_str("user_agent");
    std::string dest = get_str("destination_path");
    std::string temp = get_str("temp_dir");
    std::string cat = get_str("category");
    std::string fn = get_str("filename");
    std::string lm = get_str("last_modified");
    std::string et = get_str("etag");
    std::string aud = get_str("audio_stream_url");

    if (id.empty() || url.empty()) return nullptr;

    auto session = std::make_shared<DownloadSession>(id, url, dest, temp);
    if (!ua.empty()) {
        session->set_header("User-Agent", ua);
    }
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
