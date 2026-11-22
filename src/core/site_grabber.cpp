#include "site_grabber.hpp"
#include "download_queue.hpp"
#include "curl_utils.hpp"
#include "win32_unicode.hpp"
#include <curl/curl.h>
#include <regex>
#include <iostream>
#include <algorithm>

namespace opendm {

namespace {

constexpr size_t MAX_GRABBER_HTML_BYTES = 4 * 1024 * 1024;

size_t curl_string_writer(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* s = static_cast<std::string*>(userdata);
    if (s->size() + total > MAX_GRABBER_HTML_BYTES) {
        return 0; // Abort oversized non-HTML or huge bodies cleanly
    }
    s->append(ptr, total);
    return total;
}

int grabber_xfer_cb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* running = static_cast<std::atomic<bool>*>(clientp);
    return (running && !running->load()) ? 1 : 0;
}

} // namespace

SiteGrabber::SiteGrabber() {}

SiteGrabber::~SiteGrabber() {
    stop();
}

void SiteGrabber::start(const GrabberConfig& config, FileFoundCallback on_file_found, StatusCallback on_status) {
    stop();

    config_ = config;
    on_file_found_ = std::move(on_file_found);
    on_status_ = std::move(on_status);
    base_host_ = extract_host(config_.seed_url);

    visited_urls_.clear();
    discovered_files_.clear();
    running_ = true;

    worker_thread_ = std::thread(&SiteGrabber::crawl_loop, this);
}

void SiteGrabber::stop() {
    running_ = false;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool SiteGrabber::is_running() const {
    return running_;
}

std::vector<DiscoveredFile> SiteGrabber::get_discovered_files() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return discovered_files_;
}

std::string SiteGrabber::extract_host(const std::string& url) {
    size_t start = url.find("://");
    if (start == std::string::npos) {
        start = 0;
    } else {
        start += 3;
    }

    size_t path_end = url.find_first_of("/?#", start);
    if (path_end == std::string::npos) {
        path_end = url.length();
    }

    std::string authority = url.substr(start, path_end - start);
    size_t at_pos = authority.rfind('@');
    if (at_pos != std::string::npos) {
        authority = authority.substr(at_pos + 1);
    }

    std::string host;
    if (!authority.empty() && authority.front() == '[') {
        size_t close_bracket = authority.find(']');
        host = (close_bracket != std::string::npos)
            ? authority.substr(1, close_bracket - 1)
            : authority;
    } else {
        size_t colon = authority.find(':');
        host = (colon != std::string::npos) ? authority.substr(0, colon) : authority;
    }
    return sys::ascii_tolower(host);
}

std::string SiteGrabber::resolve_url(const std::string& base_url, const std::string& rel_url) {
    if (rel_url.empty()) return "";
    if (rel_url.rfind("http://", 0) == 0 || rel_url.rfind("https://", 0) == 0 || rel_url.rfind("ftp://", 0) == 0) {
        return rel_url;
    }

    std::string lower_prefix = sys::ascii_tolower(rel_url.substr(0, std::min<size_t>(rel_url.size(), 16)));
    if (lower_prefix.rfind("javascript:", 0) == 0 ||
        lower_prefix.rfind("mailto:", 0) == 0 ||
        lower_prefix.rfind("data:", 0) == 0 ||
        lower_prefix.rfind("tel:", 0) == 0 ||
        lower_prefix.rfind("blob:", 0) == 0 ||
        rel_url[0] == '#') {
        return "";
    }

    if (rel_url.rfind("//", 0) == 0) {
        // Protocol relative
        size_t proto_end = base_url.find("://");
        std::string proto = (proto_end != std::string::npos) ? base_url.substr(0, proto_end + 1) : "https:";
        return proto + rel_url;
    }

    size_t proto_end = base_url.find("://");
    if (proto_end == std::string::npos) return rel_url;

    std::string proto = base_url.substr(0, proto_end + 3);
    size_t host_end = base_url.find('/', proto.length());
    std::string host = (host_end != std::string::npos) ? base_url.substr(0, host_end) : base_url;

    if (rel_url[0] == '/') {
        // Absolute path from host root
        return host + rel_url;
    }

    // Relative to directory of base_url
    size_t last_slash = base_url.find_last_of('/');
    if (last_slash < proto.length()) {
        return base_url + "/" + rel_url;
    }

    return base_url.substr(0, last_slash + 1) + rel_url;
}

void SiteGrabber::crawl_loop() {
    std::queue<std::pair<std::string, int>> q;
    q.push({config_.seed_url, 0});
    visited_urls_.insert(config_.seed_url);

    size_t pages_crawled = 0;

    while (running_ && !q.empty() && pages_crawled < config_.max_pages) {
        auto [current_url, depth] = q.front();
        q.pop();

        if (on_status_) {
            on_status_("Crawling: " + current_url, pages_crawled, discovered_files_.size());
        }

        // Fetch page HTML
        CURL* curl = curl_easy_init();
        if (!curl) break;

        std::string html_body;
        curl_easy_setopt(curl, CURLOPT_URL, current_url.c_str());
        net::configure_curl_ssl(curl);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(MAX_GRABBER_HTML_BYTES));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_string_writer);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &html_body);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, grabber_xfer_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &running_);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) OpenDM/1.0 SiteGrabber");

        CURLcode res = curl_easy_perform(curl);
        char* content_type_raw = nullptr;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type_raw);
        std::string content_type = content_type_raw ? sys::ascii_tolower(content_type_raw) : "";
        curl_easy_cleanup(curl);

        pages_crawled++;

        if (res != CURLE_OK || html_body.empty()) {
            continue;
        }
        if (!content_type.empty() &&
            content_type.find("html") == std::string::npos &&
            content_type.find("xml") == std::string::npos &&
            content_type.find("text/plain") == std::string::npos) {
            continue;
        }

        // Extract href and src attributes
        static const std::regex url_regex(R"((?:href|src)=["']([^"'#\s>]+)["'])", std::regex::icase);
        auto words_begin = std::sregex_iterator(html_body.begin(), html_body.end(), url_regex);
        auto words_end = std::sregex_iterator();

        for (std::sregex_iterator i = words_begin; i != words_end && running_; ++i) {
            std::smatch match = *i;
            std::string raw_url = match[1].str();
            std::string full_url = resolve_url(current_url, raw_url);
            if (full_url.empty()) continue;

            std::string host = extract_host(full_url);
            if (host.empty()) continue;

            // Domain filter check
            if (config_.domain_scope == DomainScope::SAME_DOMAIN_ONLY) {
                if (host != base_host_) continue;
            } else if (config_.domain_scope == DomainScope::INCLUDE_SUBDOMAINS) {
                std::string suffix = "." + base_host_;
                bool is_sub = host.size() > suffix.size() &&
                              host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
                if (host != base_host_ && !is_sub) continue;
            }

            // Strip query/fragment before computing path and extension
            size_t q_or_f = full_url.find_first_of("?#");
            std::string clean_path = (q_or_f != std::string::npos) ? full_url.substr(0, q_or_f) : full_url;
            size_t proto_sep = clean_path.find("://");
            size_t path_start = (proto_sep != std::string::npos) ? clean_path.find('/', proto_sep + 3) : 0;
            std::string ext;
            if (path_start != std::string::npos) {
                size_t dot_pos = clean_path.find_last_of('.');
                size_t slash_pos = clean_path.find_last_of('/');
                if (dot_pos != std::string::npos && (slash_pos == std::string::npos || dot_pos > slash_pos)) {
                    ext = sys::ascii_tolower(clean_path.substr(dot_pos + 1));
                }
            }

            // Check if matches requested file filters
            bool matches_filter = false;
            if (config_.file_extensions.empty()) {
                // If empty, match non-html media/docs
                matches_filter = (ext != "html" && ext != "htm" && ext != "php" && ext != "asp" && ext != "aspx" && !ext.empty());
            } else {
                for (const auto& f : config_.file_extensions) {
                    if (ext == sys::ascii_tolower(f)) {
                        matches_filter = true;
                        break;
                    }
                }
            }

            if (matches_filter) {
                // Discovered downloadable asset
                DiscoveredFile df;
                df.url = full_url;
                size_t fn_slash = clean_path.find_last_of('/');
                std::string raw_fn = (fn_slash != std::string::npos) ? clean_path.substr(fn_slash + 1) : "file.bin";
                df.filename = DownloadSession::sanitize_filename(raw_fn.empty() ? "file.bin" : raw_fn);
                df.category = QueueManager::detect_category(df.filename);
                df.parent_page = current_url;

                bool already_found = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (const auto& existing : discovered_files_) {
                        if (existing.url == df.url) {
                            already_found = true;
                            break;
                        }
                    }
                    if (!already_found) {
                        discovered_files_.push_back(df);
                    }
                }

                if (!already_found && on_file_found_) {
                    on_file_found_(df);
                }
            } else if (depth + 1 <= config_.max_depth) {
                // Potential HTML page to crawl further
                if (ext.empty() || ext == "html" || ext == "htm" || ext == "php" || ext == "asp") {
                    if (visited_urls_.find(full_url) == visited_urls_.end()) {
                        visited_urls_.insert(full_url);
                        q.push({full_url, depth + 1});
                    }
                }
            }
        }
    }

    running_ = false;
    if (on_status_) {
        on_status_("Crawl complete", pages_crawled, discovered_files_.size());
    }
}

} // namespace opendm
