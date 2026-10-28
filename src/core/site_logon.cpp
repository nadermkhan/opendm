#include "site_logon.hpp"
#include "download_session.hpp"
#include "site_grabber.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

namespace opendm {

namespace {

std::string base64_encode(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (uint8_t c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(tbl[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

} // namespace

SiteLogonManager& SiteLogonManager::instance() {
    static SiteLogonManager inst;
    return inst;
}

SiteLogonManager::SiteLogonManager() {
    load_from_file();
}

std::string SiteLogonManager::default_config_path() const {
    const char* home = getenv("HOME");
    std::string base = home ? home : "/home/nader";
    return (std::filesystem::path(base) / ".config" / "opendm" / "site_logins.json").string();
}

void SiteLogonManager::set_credential(const SiteCredential& cred) {
    std::lock_guard<std::mutex> lock(mutex_);
    credentials_[cred.domain] = cred;
    save_to_file();
}

void SiteLogonManager::remove_credential(const std::string& domain) {
    std::lock_guard<std::mutex> lock(mutex_);
    credentials_.erase(domain);
    save_to_file();
}

std::vector<SiteCredential> SiteLogonManager::get_all_credentials() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SiteCredential> list;
    for (const auto& [k, v] : credentials_) {
        list.push_back(v);
    }
    return list;
}

bool SiteLogonManager::get_credential_for_url(const std::string& url, SiteCredential& out_cred) const {
    std::string host = SiteGrabber::extract_host(url);
    if (host.empty()) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    // Exact match
    auto it = credentials_.find(host);
    if (it != credentials_.end()) {
        out_cred = it->second;
        return true;
    }

    // Subdomain matching
    for (const auto& [domain, cred] : credentials_) {
        if (host.length() > domain.length()) {
            if (host.rfind("." + domain) == host.length() - domain.length() - 1) {
                out_cred = cred;
                return true;
            }
        }
    }

    return false;
}

void SiteLogonManager::apply_to_session(DownloadSession& session, const std::string& url) const {
    SiteCredential cred;
    if (!get_credential_for_url(url, cred)) {
        return;
    }

    // Apply Basic Auth and Userpwd
    if (!cred.username.empty() && !cred.password.empty()) {
        session.set_credentials(cred.username, cred.password);
        std::string auth_token = base64_encode(cred.username + ":" + cred.password);
        session.set_header("Authorization", "Basic " + auth_token);
    }

    // Apply Cookies
    if (!cred.custom_cookies.empty()) {
        session.set_header("Cookie", cred.custom_cookies);
    }

    // Apply custom headers
    for (const auto& [k, v] : cred.custom_headers) {
        session.set_header(k, v);
    }
}

bool SiteLogonManager::save_to_file(const std::string& filepath) const {
    std::string path = filepath.empty() ? default_config_path() : filepath;
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }

    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << "[\n";
    bool first = true;
    for (const auto& [domain, cred] : credentials_) {
        if (!first) f << ",\n";
        first = false;
        f << "  {\n"
          << "    \"domain\": \"" << cred.domain << "\",\n"
          << "    \"username\": \"" << cred.username << "\",\n"
          << "    \"password\": \"" << cred.password << "\",\n"
          << "    \"auth_type\": \"" << cred.auth_type << "\",\n"
          << "    \"custom_cookies\": \"" << cred.custom_cookies << "\"\n"
          << "  }";
    }
    f << "\n]\n";
    return true;
}

bool SiteLogonManager::load_from_file(const std::string& filepath) {
    std::string path = filepath.empty() ? default_config_path() : filepath;
    if (!std::filesystem::exists(path)) return false;

    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::stringstream buffer;
    buffer << f.rdbuf();
    std::string content = buffer.str();

    // Simple line/token parser for credentials
    std::lock_guard<std::mutex> lock(mutex_);
    credentials_.clear();

    size_t pos = 0;
    while ((pos = content.find('{', pos)) != std::string::npos) {
        size_t end = content.find('}', pos);
        if (end == std::string::npos) break;
        std::string obj = content.substr(pos, end - pos + 1);

        auto get_field = [&obj](const std::string& key) -> std::string {
            size_t kpos = obj.find("\"" + key + "\":");
            if (kpos == std::string::npos) return "";
            size_t start = obj.find('\"', kpos + key.length() + 2);
            if (start == std::string::npos) return "";
            size_t finish = obj.find('\"', start + 1);
            if (finish == std::string::npos) return "";
            return obj.substr(start + 1, finish - start - 1);
        };

        std::string d = get_field("domain");
        if (!d.empty()) {
            SiteCredential sc;
            sc.domain = d;
            sc.username = get_field("username");
            sc.password = get_field("password");
            sc.auth_type = get_field("auth_type");
            sc.custom_cookies = get_field("custom_cookies");
            credentials_[d] = sc;
        }

        pos = end + 1;
    }

    return true;
}

} // namespace opendm
