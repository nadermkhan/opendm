#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace opendm {
namespace sys {

struct VolumeCapabilities {
    bool valid{false};
    bool is_fat32{false};
    bool supports_sparse{false};
    uint64_t free_bytes{0};
    std::string filesystem_name;
};

// UTF-8 <-> UTF-16 conversions (always use CP_UTF8 on Windows; identity on POSIX)
std::wstring utf8_to_wide(std::string_view utf8);
std::string wide_to_utf8(std::wstring_view wide);

// Filesystem path conversions preserving UTF-8 across MinGW/Windows ACP boundaries
std::filesystem::path path_from_utf8(std::string_view utf8);
std::string path_to_utf8(const std::filesystem::path& p);

// Windows extended-length path (\\?\ / \\?\UNC\) for Win32 file/volume APIs
std::wstring to_win32_long_path_wide(const std::filesystem::path& p);

// UTF-8 environment, application, and standard directory queries
std::string get_env_utf8(const char* name);
std::string get_exe_dir_utf8();
std::string get_downloads_dir_utf8();
std::string get_appdata_dir_utf8();
std::string get_local_appdata_dir_utf8();
std::vector<std::string> get_cli_args_utf8(int argc, char* argv[]);

// Safe locale-independent ASCII lowercase helper (avoids signed-char tolower UB)
std::string ascii_tolower(std::string_view s);

// Checks Windows reserved DOS device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9)
bool is_reserved_dos_name(std::string_view filename);

// Queries underlying volume filesystem type and available space (via GetVolumePathNameW on Windows)
VolumeCapabilities inspect_target_volume(const std::string& path_utf8);

// Atomically replaces dest_path with tmp_path without deleting dest_path first or falling back to slow cross-volume copy.
// On Windows, retries transiently locked destinations (e.g. Windows Defender close-time scan locks).
bool atomic_replace_file(const std::filesystem::path& tmp_path,
                         const std::filesystem::path& dest_path,
                         std::error_code& ec);

// Quotes a single command-line argument safely per Windows CommandLineToArgvW backslash rules
std::string escape_windows_arg(std::string_view arg);

// Executes an executable cleanly without spawning a console window (CreateProcessW + CREATE_NO_WINDOW on Windows).
// Streams merged stdout/stderr lines to line_cb (if non-null) and terminates child if cancel_cb returns true.
bool run_hidden_process(const std::string& exe_utf8,
                        const std::vector<std::string>& args_utf8,
                        std::function<void(std::string_view)> line_cb = nullptr,
                        std::function<bool()> cancel_cb = nullptr,
                        int* out_exit_code = nullptr);

// Shared formatting helpers
std::string format_bytes(int64_t bytes);
std::string format_speed(uint64_t bytes_per_sec);
std::string format_eta(int64_t eta_sec);

} // namespace sys
} // namespace opendm
