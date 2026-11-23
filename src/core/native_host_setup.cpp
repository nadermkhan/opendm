#include "core/native_host_setup.hpp"
#include "core/win32_unicode.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#endif

namespace opendm {

namespace {

namespace fs = std::filesystem;

std::string build_chrome_manifest_json(const std::string& host_path,
                                       const std::string& extra_chromium_id) {
    nlohmann::json origins = nlohmann::json::array();
    origins.push_back(std::string("chrome-extension://") + kDefaultChromiumExtensionId + "/");
    if (!extra_chromium_id.empty() && extra_chromium_id != kDefaultChromiumExtensionId) {
        origins.push_back(std::string("chrome-extension://") + extra_chromium_id + "/");
    }

    nlohmann::json doc = {
        {"name", kNativeHostName},
        {"description", "OpenDM Native Messaging Host"},
        {"path", host_path},
        {"type", "stdio"},
        {"allowed_origins", std::move(origins)}
    };
    return doc.dump(2) + "\n";
}

std::string build_firefox_manifest_json(const std::string& host_path) {
    nlohmann::json doc = {
        {"name", kNativeHostName},
        {"description", "OpenDM Native Messaging Host"},
        {"path", host_path},
        {"type", "stdio"},
        {"allowed_extensions", nlohmann::json::array({kFirefoxExtensionId})}
    };
    return doc.dump(2) + "\n";
}

bool write_text_file(const fs::path& dest, const std::string& content, std::string& err) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        err = "Failed to create directory " + sys::path_to_utf8(dest.parent_path()) + ": " + ec.message();
        return false;
    }

    std::ofstream ofs(dest, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        err = "Failed to write manifest file: " + sys::path_to_utf8(dest);
        return false;
    }
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    ofs.close();
    if (!ofs) {
        err = "I/O error writing manifest file: " + sys::path_to_utf8(dest);
        return false;
    }
    return true;
}

std::string resolve_host_executable(const NativeHostSetupOptions& opts) {
    if (!opts.host_executable_path.empty()) {
        return opts.host_executable_path;
    }
    std::string exe_dir = sys::get_exe_dir_utf8();
#ifdef _WIN32
    fs::path candidate = sys::path_from_utf8(exe_dir) / "opendm-native-host.exe";
#else
    fs::path candidate = sys::path_from_utf8(exe_dir) / "opendm-native-host";
#endif
    std::error_code ec;
    if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
        return sys::path_to_utf8(candidate);
    }
    return "";
}

#ifdef _WIN32
fs::path default_win32_manifest_dir() {
    std::string local_app_data = sys::get_env_utf8("LOCALAPPDATA");
    if (!local_app_data.empty()) {
        return sys::path_from_utf8(local_app_data) / "OpenDM" / "NativeMessagingHosts";
    }
    std::string app_data = sys::get_env_utf8("APPDATA");
    if (!app_data.empty()) {
        return sys::path_from_utf8(app_data) / "OpenDM" / "NativeMessagingHosts";
    }
    return sys::path_from_utf8(sys::get_exe_dir_utf8()) / "NativeMessagingHosts";
}

bool write_hkcu_registry_default_value(const std::wstring& subkey,
                                       const std::wstring& value_utf16,
                                       REGSAM extra_sam,
                                       std::string& err) {
    HKEY hKey = nullptr;
    LONG rc = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        subkey.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE | extra_sam,
        nullptr,
        &hKey,
        nullptr);
    if (rc != ERROR_SUCCESS) {
        err = "RegCreateKeyExW failed (" + std::to_string(rc) + ") for " + sys::wide_to_utf8(subkey);
        return false;
    }

    DWORD bytes = static_cast<DWORD>((value_utf16.size() + 1) * sizeof(wchar_t));
    rc = RegSetValueExW(
        hKey,
        nullptr,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value_utf16.c_str()),
        bytes);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) {
        err = "RegSetValueExW failed (" + std::to_string(rc) + ") for " + sys::wide_to_utf8(subkey);
        return false;
    }
    return true;
}

bool hkcu_default_value_exists(const std::wstring& subkey, const std::wstring& expected_path) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buf[2048]{};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(hKey, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }
    std::wstring current(buf);
    return !current.empty() && current == expected_path;
}
#endif

} // namespace

NativeHostSetupResult register_native_messaging_hosts(const NativeHostSetupOptions& opts) {
    NativeHostSetupResult res;
    res.host_path = resolve_host_executable(opts);
    if (res.host_path.empty()) {
        res.error_message = "Could not locate opendm-native-host executable next to opendm";
        return res;
    }

    const std::string chrome_json = build_chrome_manifest_json(res.host_path, opts.extra_chromium_extension_id);
    const std::string firefox_json = build_firefox_manifest_json(res.host_path);

#ifdef _WIN32
    fs::path manifest_dir = opts.manifest_dir.empty()
        ? default_win32_manifest_dir()
        : sys::path_from_utf8(opts.manifest_dir);
    fs::path chrome_manifest = manifest_dir / (std::string(kNativeHostName) + ".chrome.json");
    fs::path firefox_manifest = manifest_dir / (std::string(kNativeHostName) + ".firefox.json");

    if (!write_text_file(chrome_manifest, chrome_json, res.error_message) ||
        !write_text_file(firefox_manifest, firefox_json, res.error_message)) {
        return res;
    }

    res.chrome_manifest_path = sys::path_to_utf8(chrome_manifest);
    res.firefox_manifest_path = sys::path_to_utf8(firefox_manifest);

    if (opts.dry_run) {
        res.registered_targets.push_back("dry-run:" + res.chrome_manifest_path);
        res.registered_targets.push_back("dry-run:" + res.firefox_manifest_path);
        res.ok = true;
        return res;
    }

    const std::wstring wchrome = sys::utf8_to_wide(res.chrome_manifest_path);
    const std::wstring wfirefox = sys::utf8_to_wide(res.firefox_manifest_path);

    struct RegTarget {
        const char* name;
        const wchar_t* subkey;
        bool is_firefox;
        REGSAM extra_sam;
    };
    const RegTarget targets[] = {
        {"Google Chrome",   L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.opendm.host",              false, 0},
        {"Microsoft Edge",  L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\com.opendm.host",             false, 0},
        {"Brave",           L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\com.opendm.host", false, 0},
        {"Chromium",        L"Software\\Chromium\\NativeMessagingHosts\\com.opendm.host",                    false, 0},
        {"Vivaldi",         L"Software\\Vivaldi\\NativeMessagingHosts\\com.opendm.host",                     false, 0},
        {"Opera",           L"Software\\Opera Software\\NativeMessagingHosts\\com.opendm.host",              false, 0},
        {"Firefox (64-bit)",L"Software\\Mozilla\\NativeMessagingHosts\\com.opendm.host",                     true,  KEY_WOW64_64KEY},
        {"Firefox (32-bit)",L"Software\\Mozilla\\NativeMessagingHosts\\com.opendm.host",                     true,  KEY_WOW64_32KEY},
    };

    for (const auto& t : targets) {
        std::string reg_err;
        const std::wstring& target_manifest = t.is_firefox ? wfirefox : wchrome;
        if (write_hkcu_registry_default_value(t.subkey, target_manifest, t.extra_sam, reg_err)) {
            res.registered_targets.push_back(t.name);
        } else if (res.error_message.empty()) {
            res.error_message = reg_err;
        }
    }

    res.ok = !res.registered_targets.empty();
    return res;
#else
    if (!opts.manifest_dir.empty()) {
        fs::path base_dir = sys::path_from_utf8(opts.manifest_dir);
        fs::path chrome_manifest = base_dir / (std::string(kNativeHostName) + ".chrome.json");
        fs::path firefox_manifest = base_dir / (std::string(kNativeHostName) + ".firefox.json");
        if (!write_text_file(chrome_manifest, chrome_json, res.error_message) ||
            !write_text_file(firefox_manifest, firefox_json, res.error_message)) {
            return res;
        }
        res.chrome_manifest_path = sys::path_to_utf8(chrome_manifest);
        res.firefox_manifest_path = sys::path_to_utf8(firefox_manifest);
        res.registered_targets.push_back("custom-dir");
        res.ok = true;
        return res;
    }

    std::string home = sys::get_env_utf8("HOME");
    if (home.empty()) {
        res.error_message = "HOME environment variable is unset";
        return res;
    }
    fs::path home_path = sys::path_from_utf8(home);
    fs::path xdg_cfg = sys::get_env_utf8("XDG_CONFIG_HOME").empty()
        ? (home_path / ".config")
        : sys::path_from_utf8(sys::get_env_utf8("XDG_CONFIG_HOME"));

    struct PosixTarget {
        const char* name;
        fs::path dir;
        bool is_firefox;
    };
    const PosixTarget targets[] = {
        {"Google Chrome",  xdg_cfg / "google-chrome" / "NativeMessagingHosts",                false},
        {"Chromium",       xdg_cfg / "chromium" / "NativeMessagingHosts",                     false},
        {"Brave",          xdg_cfg / "BraveSoftware" / "Brave-Browser" / "NativeMessagingHosts", false},
        {"Microsoft Edge", xdg_cfg / "microsoft-edge" / "NativeMessagingHosts",               false},
        {"Vivaldi",        xdg_cfg / "vivaldi" / "NativeMessagingHosts",                      false},
        {"Opera",          xdg_cfg / "opera" / "NativeMessagingHosts",                        false},
        {"Firefox",        home_path / ".mozilla" / "native-messaging-hosts",                 true},
        {"LibreWolf",      home_path / ".librewolf" / "native-messaging-hosts",               true},
    };

    for (const auto& t : targets) {
        fs::path dest = t.dir / (std::string(kNativeHostName) + ".json");
        if (opts.dry_run) {
            res.registered_targets.push_back(std::string("dry-run:") + t.name);
            continue;
        }
        std::string write_err;
        if (write_text_file(dest, t.is_firefox ? firefox_json : chrome_json, write_err)) {
            res.registered_targets.push_back(t.name);
            if (t.is_firefox && res.firefox_manifest_path.empty()) {
                res.firefox_manifest_path = sys::path_to_utf8(dest);
            } else if (!t.is_firefox && res.chrome_manifest_path.empty()) {
                res.chrome_manifest_path = sys::path_to_utf8(dest);
            }
        } else if (res.error_message.empty()) {
            res.error_message = write_err;
        }
    }

    res.ok = !res.registered_targets.empty();
    return res;
#endif
}

bool is_native_host_registered() {
#ifdef _WIN32
    fs::path dir = default_win32_manifest_dir();
    fs::path chrome_manifest = dir / (std::string(kNativeHostName) + ".chrome.json");
    fs::path firefox_manifest = dir / (std::string(kNativeHostName) + ".firefox.json");
    std::error_code ec;
    if (!fs::exists(chrome_manifest, ec) || !fs::exists(firefox_manifest, ec)) {
        return false;
    }
    return hkcu_default_value_exists(
               L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.opendm.host",
               chrome_manifest.wstring()) &&
           hkcu_default_value_exists(
               L"Software\\Mozilla\\NativeMessagingHosts\\com.opendm.host",
               firefox_manifest.wstring());
#else
    std::string home = sys::get_env_utf8("HOME");
    std::error_code ec;
    if (!home.empty()) {
        fs::path firefox_user = sys::path_from_utf8(home) / ".mozilla" / "native-messaging-hosts" / "com.opendm.host.json";
        if (fs::exists(firefox_user, ec)) return true;
    }
    return fs::exists("/usr/lib/mozilla/native-messaging-hosts/com.opendm.host.json", ec) ||
           fs::exists("/etc/opt/chrome/native-messaging-hosts/com.opendm.host.json", ec);
#endif
}

} // namespace opendm
