#pragma once

#include <string>
#include <vector>
#include <set>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

namespace opendm {

enum class DomainScope {
    SAME_DOMAIN_ONLY,
    INCLUDE_SUBDOMAINS,
    ANY_DOMAIN
};

struct GrabberConfig {
    std::string seed_url;
    int max_depth{1};
    DomainScope domain_scope{DomainScope::SAME_DOMAIN_ONLY};
    std::vector<std::string> file_extensions; // e.g. {"mp4", "jpg", "zip", "pdf"}
    size_t max_pages{50};
};

struct DiscoveredFile {
    std::string url;
    std::string filename;
    std::string category;
    std::string parent_page;
};

class SiteGrabber {
public:
    using FileFoundCallback = std::function<void(const DiscoveredFile&)>;
    using StatusCallback = std::function<void(const std::string& status, size_t pages_crawled, size_t files_found)>;

    SiteGrabber();
    ~SiteGrabber();

    void start(const GrabberConfig& config, FileFoundCallback on_file_found, StatusCallback on_status);
    void stop();
    bool is_running() const;

    std::vector<DiscoveredFile> get_discovered_files() const;

    static std::string resolve_url(const std::string& base_url, const std::string& relative_url);
    static std::string extract_host(const std::string& url);

private:
    void crawl_loop();
    void process_page(const std::string& url, int depth);

    GrabberConfig config_;
    FileFoundCallback on_file_found_;
    StatusCallback on_status_;

    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;

    std::set<std::string> visited_urls_;
    std::vector<DiscoveredFile> discovered_files_;
    std::string base_host_;
};

} // namespace opendm
