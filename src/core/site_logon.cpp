#include "site_logon.hpp"
#include "download_session.hpp"
#include "site_grabber.hpp"
#include "win32_unicode.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

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
    fs::path dir = sys::path_from_utf8(sys::get_appdata_dir_utf8());
    std::error_code ec;
    fs::create_directories(dir, ec);
    return sys::path_to_utf8(dir / "site_logins.json");
}

void SiteLogonManager::set_credential(const SiteCredential& cred) {
    std::lock_guard<std::mutex> lock(mutex_);
    credentials_[cred.domain] = cred;
    save_to_file_locked();
}

void SiteLogonManager::remove_credential(const std::string& domain) {
    std::lock_guard<std::mutex> lock(mutex_);
    credentials_.erase(domain);
    save_to_file_locked();
}

std::vector<SiteCredential> SiteLogonManager::get_all_credentials() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SiteCredential> list;
    list.reserve(credentials_.size());
    for (const auto& [k, v] : credentials_) {
        list.push_back(v);
    }
    return list;
}

bool SiteLogonManager::get_credential_for_url(const std::string& url, SiteCredential& out_cred) const {
    std::string host = sys::ascii_tolower(SiteGrabber::extract_host(url));
    if (host.empty()) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [domain, cred] : credentials_) {
        std::string ld = sys::ascii_tolower(domain);
        if (host == ld) {
            out_cred = cred;
            return true;
        }
    }

    for (const auto& [domain, cred] : credentials_) {
        std::string ld = sys::ascii_tolower(domain);
        if (!ld.empty() && host.length() > ld.length()) {
            if (host.rfind("." + ld) == host.length() - ld.length() - 1) {
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

    if (!cred.username.empty() && !cred.password.empty()) {
        session.set_credentials(cred.username, cred.password);
        std::string auth_token = base64_encode(cred.username + ":" + cred.password);
        session.set_header("Authorization", "Basic " + auth_token);
    }

    if (!cred.custom_cookies.empty()) {
        session.set_header("Cookie", cred.custom_cookies);
    }

    for (const auto& [k, v] : cred.custom_headers) {
        session.set_header(k, v);
    }
}

bool SiteLogonManager::save_to_file(const std::string& filepath) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return save_to_file_locked(filepath);
}

bool SiteLogonManager::save_to_file_locked(const std::string& filepath) const {
    std::string path = filepath.empty() ? default_config_path() : filepath;
    fs::path p = sys::path_from_utf8(path);
    std::error_code ec;
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
    }

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [domain, cred] : credentials_) {
        nlohmann::json item = {
            {"domain", cred.domain},
            {"username", cred.username},
            {"password", cred.password},
            {"auth_type", cred.auth_type},
            {"custom_cookies", cred.custom_cookies},
            {"custom_headers", cred.custom_headers}
        };
        arr.push_back(std::move(item));
    }

    fs::path tmp_p = sys::path_from_utf8(path + ".tmp");
    std::ofstream f(tmp_p, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f << arr.dump(2) << "\n";
    f.flush();
    bool ok = f.good();
    f.close();
    if (!ok) {
        fs::remove(tmp_p, ec);
        return false;
    }
    return sys::atomic_replace_file(tmp_p, p, ec);
}

bool SiteLogonManager::load_from_file(const std::string& filepath) {
    std::string path = filepath.empty() ? default_config_path() : filepath;
    fs::path p = sys::path_from_utf8(path);
    std::error_code ec;
    if (!fs::exists(p, ec)) return false;

    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return false;

    std::stringstream buffer;
    buffer << f.rdbuf();
    std::string content = buffer.str();
    f.close();

    std::map<std::string, SiteCredential> parsed;
    try {
        nlohmann::json arr = nlohmann::json::parse(content);
        if (arr.is_array()) {
            for (const auto& item : arr) {
                if (!item.is_object()) continue;
                std::string d = item.value("domain", "");
                if (d.empty()) continue;
                SiteCredential sc;
                sc.domain = d;
                sc.username = item.value("username", "");
                sc.password = item.value("password", "");
                sc.auth_type = item.value("auth_type", "basic");
                sc.custom_cookies = item.value("custom_cookies", "");
                if (item.contains("custom_headers") && item["custom_headers"].is_object()) {
                    for (auto& [hk, hv] : item["custom_headers"].items()) {
                        if (hv.is_string()) {
                            sc.custom_headers[hk] = hv.get<std::string>();
                        }
                    }
                }
                parsed[d] = std::move(sc);
            }
        }
    } catch (...) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    credentials_ = std::move(parsed);
    return true;
}

} // namespace opendm
