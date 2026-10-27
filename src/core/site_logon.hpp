#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>

namespace opendm {

class DownloadSession;

struct SiteCredential {
    std::string domain;
    std::string username;
    std::string password;
    std::string auth_type{"basic"}; // "basic", "digest", "bearer"
    std::string custom_cookies;
    std::map<std::string, std::string> custom_headers;
};

class SiteLogonManager {
public:
    static SiteLogonManager& instance();

    void set_credential(const SiteCredential& cred);
    bool get_credential_for_url(const std::string& url, SiteCredential& out_cred) const;
    void remove_credential(const std::string& domain);
    std::vector<SiteCredential> get_all_credentials() const;

    // Automatically applies credentials, auth, and custom headers to session
    void apply_to_session(DownloadSession& session, const std::string& url) const;

    // Persistence
    bool save_to_file(const std::string& filepath = "") const;
    bool load_from_file(const std::string& filepath = "");

private:
    SiteLogonManager();
    ~SiteLogonManager() = default;

    std::string default_config_path() const;

    mutable std::mutex mutex_;
    std::map<std::string, SiteCredential> credentials_;
};

} // namespace opendm
