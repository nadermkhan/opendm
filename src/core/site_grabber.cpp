#include "site_grabber.hpp"
#include "download_queue.hpp"
#include <curl/curl.h>
#include <regex>
#include <iostream>
#include <algorithm>

namespace opendm {

namespace {

size_t curl_string_writer(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
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

    size_t end = url.find_first_of("/:?#", start);
    if (end == std::string::npos) {
        end = url.length();
    }

    std::string host = url.substr(start, end - start);
    std::transform(host.begin(), host.end(), host.begin(), ::tolower);
    return host;
}

std::string SiteGrabber::resolve_url(const std::string& base_url, const std::string& rel_url) {
    if (rel_url.empty()) return "";
    if (rel_url.rfind("http://", 0) == 0 || rel_url.rfind("https://", 0) == 0 || rel_url.rfind("ftp://", 0) == 0) {
        return rel_url;
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
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_string_writer);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &html_body);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) OpenDM/1.0 SiteGrabber");

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        pages_crawled++;

        if (res != CURLE_OK || html_body.empty()) {
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

            // Domain filter check
            if (config_.domain_scope == DomainScope::SAME_DOMAIN_ONLY) {
                if (host != base_host_) continue;
            } else if (config_.domain_scope == DomainScope::INCLUDE_SUBDOMAINS) {
                if (host != base_host_ && host.rfind("." + base_host_) == std::string::npos) continue;
            }

            // Extract extension
            size_t dot_pos = full_url.find_last_of('.');
            size_t slash_pos = full_url.find_last_of('/');
            std::string ext;
            if (dot_pos != std::string::npos && dot_pos > slash_pos) {
                size_t q_pos = full_url.find('?', dot_pos);
                if (q_pos != std::string::npos) {
                    ext = full_url.substr(dot_pos + 1, q_pos - dot_pos - 1);
                } else {
                    ext = full_url.substr(dot_pos + 1);
                }
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            }

            // Check if matches requested file filters
            bool matches_filter = false;
            if (config_.file_extensions.empty()) {
                // If empty, match non-html media/docs
                matches_filter = (ext != "html" && ext != "htm" && ext != "php" && ext != "asp" && !ext.empty());
            } else {
                for (const auto& f : config_.file_extensions) {
                    if (ext == f) {
                        matches_filter = true;
                        break;
                    }
                }
            }

            if (matches_filter) {
                // Discovered downloadable asset
                DiscoveredFile df;
                df.url = full_url;
                size_t fn_slash = full_url.find_last_of('/');
                df.filename = (fn_slash != std::string::npos) ? full_url.substr(fn_slash + 1) : "file.bin";
                // Strip query string from filename
                size_t qmark = df.filename.find('?');
                if (qmark != std::string::npos) df.filename = df.filename.substr(0, qmark);
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
