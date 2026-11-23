#pragma once

#include <string>
#include <vector>

namespace opendm {

inline constexpr const char* kNativeHostName = "com.opendm.host";
inline constexpr const char* kDefaultChromiumExtensionId = "ekehfpkieofbbgakjbmefknbbabjpoog";
inline constexpr const char* kFirefoxExtensionId = "extension@opendm.org";

struct NativeHostSetupOptions {
    // Explicit path to opendm-native-host executable. Auto-detected next to opendm if empty.
    std::string host_executable_path;
    // Explicit directory for generated manifests. Defaults to %LOCALAPPDATA%\OpenDM\NativeMessagingHosts on Windows.
    std::string manifest_dir;
    // Optional additional Chromium extension origin ID (e.g. unpacked ID during local testing).
    std::string extra_chromium_extension_id;
    // If true, generates manifest JSON files in manifest_dir without mutating system registry / OS browser dirs.
    bool dry_run{false};
};

struct NativeHostSetupResult {
    bool ok{false};
    std::string host_path;
    std::string chrome_manifest_path;
    std::string firefox_manifest_path;
    std::vector<std::string> registered_targets;
    std::string error_message;
};

/**
 * Registers the OpenDM native messaging host manifests for Chromium-based browsers
 * (Chrome, Edge, Brave, Chromium, Vivaldi, Opera) and Gecko/Firefox browsers.
 */
NativeHostSetupResult register_native_messaging_hosts(const NativeHostSetupOptions& opts = {});

/**
 * Checks whether the OpenDM native messaging host manifests appear registered for
 * the current user.
 */
bool is_native_host_registered();

} // namespace opendm
