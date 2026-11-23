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

struct DetachedLaunchOptions {
    std::string exe_path;
    std::string arguments;
    std::string working_dir;
    int show_cmd{7}; // SW_SHOWMINNOACTIVE on Windows
    bool allow_foreground{true};
};

struct DetachedLaunchResult {
    bool ok{false};
    std::string method;
    bool in_job{false};
    bool breakaway_allowed{false};
    bool silent_breakaway{false};
    uint32_t win32_error{0};
    std::string error_message;
};

// Queries whether current process is assigned to a Windows Job Object and whether breakaway flags allow escape.
bool is_current_process_in_job(bool& out_breakaway_allowed,
                               bool& out_silent_breakaway,
                               uint32_t* out_error = nullptr);

// Launches a Windows GUI process directly via CreateProcessW (with optional CREATE_BREAKAWAY_FROM_JOB).
DetachedLaunchResult launch_via_create_process(const DetachedLaunchOptions& opts,
                                               bool use_breakaway,
                                               void** out_process_handle = nullptr);

// Delegates Windows GUI process creation to the active Explorer desktop shell via COM (IShellWindows / IShellDispatch2).
DetachedLaunchResult launch_via_explorer_com(const DetachedLaunchOptions& opts,
                                             int timeout_ms = 1500);

// Launches a Windows GUI child outside any browser Job Object, failing closed if escape is blocked.
DetachedLaunchResult launch_detached_gui_process(const DetachedLaunchOptions& opts,
                                                 void** out_process_handle = nullptr);

// Returns current Windows process mandatory integrity RID (e.g. 0x2000 Medium, 0x3000 High), or 0 on POSIX/failure.
uint32_t get_process_integrity_rid();

// Shared formatting helpers
std::string format_bytes(int64_t bytes);
std::string format_speed(uint64_t bytes_per_sec);
std::string format_eta(int64_t eta_sec);

} // namespace sys
} // namespace opendm
