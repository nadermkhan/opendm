#include "win32_unicode.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shldisp.h>
#include <servprov.h>
#include <oleauto.h>
#include <objbase.h>
#include <future>
#ifdef ERROR
#undef ERROR
#endif
#else
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace opendm {
namespace sys {

std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) return {};
#ifdef _WIN32
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return std::wstring(utf8.begin(), utf8.end());
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
#else
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) return {};
#ifdef _WIN32
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        std::string out;
        out.reserve(wide.size());
        for (wchar_t wc : wide) {
            out.push_back(static_cast<char>(wc & 0xFF));
        }
        return out;
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    return out;
#else
    std::string out;
    out.reserve(wide.size());
    for (wchar_t wc : wide) {
        out.push_back(static_cast<char>(wc & 0xFF));
    }
    return out;
#endif
}

fs::path path_from_utf8(std::string_view utf8) {
    if (utf8.empty()) return {};
#ifdef _WIN32
    return fs::path(utf8_to_wide(utf8));
#else
    return fs::path(utf8);
#endif
}

std::string path_to_utf8(const fs::path& p) {
    if (p.empty()) return {};
#ifdef _WIN32
    return wide_to_utf8(p.wstring());
#else
    return p.string();
#endif
}

std::wstring to_win32_long_path_wide(const fs::path& p) {
#ifdef _WIN32
    std::wstring ws = p.wstring();
    if (ws.empty()) return {};
    if (ws.rfind(L"\\\\?\\", 0) == 0) {
        return ws;
    }
    DWORD full_len = GetFullPathNameW(ws.c_str(), 0, nullptr, nullptr);
    std::wstring abs_ws = ws;
    if (full_len > 0) {
        std::wstring buf(full_len, L'\0');
        DWORD written = GetFullPathNameW(ws.c_str(), full_len, buf.data(), nullptr);
        if (written > 0 && written < full_len) {
            buf.resize(written);
            abs_ws = std::move(buf);
        }
    }
    std::replace(abs_ws.begin(), abs_ws.end(), L'/', L'\\');
    if (abs_ws.rfind(L"\\\\", 0) == 0 && abs_ws.rfind(L"\\\\?\\", 0) != 0) {
        return L"\\\\?\\UNC\\" + abs_ws.substr(2);
    }
    if (abs_ws.size() >= 240 && abs_ws.size() >= 2 && abs_ws[1] == L':') {
        return L"\\\\?\\" + abs_ws;
    }
    return abs_ws;
#else
    (void)p;
    return {};
#endif
}

std::string get_env_utf8(const char* name) {
    if (!name || !*name) return {};
#ifdef _WIN32
    std::wstring wname = utf8_to_wide(name);
    const wchar_t* val = _wgetenv(wname.c_str());
    return val ? wide_to_utf8(val) : std::string{};
#else
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string{};
#endif
}

std::string get_exe_dir_utf8() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len > 0 && len < buf.size()) {
        buf.resize(len);
        return wide_to_utf8(fs::path(buf).parent_path().wstring());
    }
    return {};
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return fs::path(buf).parent_path().string();
    }
    return {};
#endif
}

std::string get_downloads_dir_utf8() {
#ifdef _WIN32
    PWSTR pPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &pPath)) && pPath) {
        std::string res = wide_to_utf8(pPath);
        CoTaskMemFree(pPath);
        if (!res.empty()) return res;
    }
    std::string up = get_env_utf8("USERPROFILE");
    if (!up.empty()) {
        return path_to_utf8(path_from_utf8(up) / L"Downloads");
    }
    return "Downloads";
#else
    std::string home = get_env_utf8("HOME");
    if (home.empty()) home = "/tmp";
    return (fs::path(home) / "Downloads").string();
#endif
}

std::string get_appdata_dir_utf8() {
#ifdef _WIN32
    PWSTR pPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pPath)) && pPath) {
        std::string res = path_to_utf8(fs::path(pPath) / L"OpenDM");
        CoTaskMemFree(pPath);
        return res;
    }
    std::string appdata = get_env_utf8("APPDATA");
    if (!appdata.empty()) {
        return path_to_utf8(path_from_utf8(appdata) / L"OpenDM");
    }
    std::string up = get_env_utf8("USERPROFILE");
    if (!up.empty()) {
        return path_to_utf8(path_from_utf8(up) / L".opendm");
    }
    return ".";
#else
    std::string home = get_env_utf8("HOME");
    if (home.empty()) home = "/tmp";
    return (fs::path(home) / ".config" / "opendm").string();
#endif
}

std::string get_local_appdata_dir_utf8() {
#ifdef _WIN32
    PWSTR pPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &pPath)) && pPath) {
        std::string res = path_to_utf8(fs::path(pPath) / L"OpenDM");
        CoTaskMemFree(pPath);
        return res;
    }
    std::string local = get_env_utf8("LOCALAPPDATA");
    if (!local.empty()) {
        return path_to_utf8(path_from_utf8(local) / L"OpenDM");
    }
    return path_to_utf8(fs::temp_directory_path() / L"OpenDM");
#else
    std::string home = get_env_utf8("HOME");
    if (home.empty()) home = "/tmp";
    return (fs::path(home) / ".cache" / "opendm").string();
#endif
}

std::vector<std::string> get_cli_args_utf8(int argc, char* argv[]) {
    std::vector<std::string> out;
#ifdef _WIN32
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv && wargc > 0) {
        out.reserve(static_cast<size_t>(wargc));
        for (int i = 0; i < wargc; ++i) {
            out.push_back(wide_to_utf8(wargv[i]));
        }
        LocalFree(wargv);
        return out;
    }
#endif
    out.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        out.emplace_back(argv[i] ? argv[i] : "");
    }
    return out;
}

std::string ascii_tolower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c + ('a' - 'A')));
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool is_reserved_dos_name(std::string_view filename) {
    if (filename.empty()) return false;
    size_t dot = filename.find('.');
    std::string_view base = (dot == std::string_view::npos) ? filename : filename.substr(0, dot);
    while (!base.empty() && base.back() == ' ') {
        base.remove_suffix(1);
    }
    std::string upper;
    upper.reserve(base.size());
    for (unsigned char c : base) {
        if (c >= 'a' && c <= 'z') {
            upper.push_back(static_cast<char>(c - ('a' - 'A')));
        } else {
            upper.push_back(static_cast<char>(c));
        }
    }
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
        return true;
    }
    if (upper.size() == 4 &&
        (upper.rfind("COM", 0) == 0 || upper.rfind("LPT", 0) == 0) &&
        upper[3] >= '1' && upper[3] <= '9') {
        return true;
    }
    return false;
}

VolumeCapabilities inspect_target_volume(const std::string& path_utf8) {
    VolumeCapabilities caps;
    std::string probe = path_utf8.empty() ? "." : path_utf8;
#ifdef _WIN32
    fs::path p = path_from_utf8(probe);
    std::error_code ec;
    fs::path abs_p = fs::weakly_canonical(p, ec);
    if (ec || abs_p.empty()) {
        abs_p = fs::absolute(p, ec);
    }
    std::wstring wpath = abs_p.empty() ? utf8_to_wide(probe) : abs_p.wstring();
    wchar_t vol_root[MAX_PATH * 4] = {0};
    if (!GetVolumePathNameW(wpath.c_str(), vol_root, static_cast<DWORD>(std::size(vol_root)))) {
        // Fallback to parent directory if target file doesn't exist yet
        fs::path parent = p.parent_path();
        if (parent.empty()) parent = L".";
        GetVolumePathNameW(parent.wstring().c_str(), vol_root, static_cast<DWORD>(std::size(vol_root)));
    }
    if (vol_root[0] != L'\0') {
        wchar_t fs_name[128] = {0};
        DWORD flags = 0;
        if (GetVolumeInformationW(vol_root, nullptr, 0, nullptr, nullptr, &flags, fs_name, 128)) {
            caps.filesystem_name = wide_to_utf8(fs_name);
            caps.is_fat32 = (ascii_tolower(caps.filesystem_name) == "fat32");
            caps.supports_sparse = (flags & FILE_SUPPORTS_SPARSE_FILES) != 0;
        }
        ULARGE_INTEGER avail{};
        if (GetDiskFreeSpaceExW(wpath.c_str(), &avail, nullptr, nullptr) ||
            GetDiskFreeSpaceExW(vol_root, &avail, nullptr, nullptr)) {
            caps.free_bytes = avail.QuadPart;
            caps.valid = true;
        }
    }
#else
    fs::path p(probe);
    std::error_code ec;
    fs::path dir = fs::is_directory(p, ec) ? p : p.parent_path();
    if (dir.empty()) dir = ".";
    auto si = fs::space(dir, ec);
    if (!ec) {
        caps.free_bytes = static_cast<uint64_t>(si.available);
        caps.valid = true;
    }
#endif
    return caps;
}

bool atomic_replace_file(const fs::path& tmp_path,
                         const fs::path& dest_path,
                         std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    std::wstring src_w = to_win32_long_path_wide(tmp_path);
    std::wstring dst_w = to_win32_long_path_wide(dest_path);

    DWORD delay_ms = 25;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (MoveFileExW(src_w.c_str(), dst_w.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min<DWORD>(delay_ms * 2, 200);
            continue;
        }
        ec = std::error_code(static_cast<int>(err), std::system_category());
        return false;
    }
    DWORD last_err = GetLastError();
    ec = std::error_code(static_cast<int>(last_err ? last_err : ERROR_SHARING_VIOLATION), std::system_category());
    return false;
#else
    fs::rename(tmp_path, dest_path, ec);
    return !ec;
#endif
}

std::string escape_windows_arg(std::string_view arg) {
    if (arg.empty()) return "\"\"";
    bool needs_quotes = false;
    for (char c : arg) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return std::string(arg);
    }

    std::string out;
    out.push_back('"');
    for (size_t i = 0; i < arg.size();) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') {
            backslashes++;
            i++;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, '\\');
            break;
        } else if (arg[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            i++;
        } else {
            out.append(backslashes, '\\');
            out.push_back(arg[i]);
            i++;
        }
    }
    out.push_back('"');
    return out;
}

bool run_hidden_process(const std::string& exe_utf8,
                        const std::vector<std::string>& args_utf8,
                        std::function<void(std::string_view)> line_cb,
                        std::function<bool()> cancel_cb,
                        int* out_exit_code) {
    if (out_exit_code) *out_exit_code = -1;
    if (exe_utf8.empty()) return false;

#ifdef _WIN32
    std::string cmd_utf8 = escape_windows_arg(exe_utf8);
    for (const auto& a : args_utf8) {
        cmd_utf8.push_back(' ');
        cmd_utf8 += escape_windows_arg(a);
    }
    std::wstring cmd_w = utf8_to_wide(cmd_utf8);
    if (cmd_w.size() >= 32767) return false;
    std::vector<wchar_t> cmd_buf(cmd_w.begin(), cmd_w.end());
    cmd_buf.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe = nullptr;
    HANDLE hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return false;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE hNullIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = (hNullIn != INVALID_HANDLE_VALUE) ? hNullIn : nullptr;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(
        nullptr,
        cmd_buf.data(),
        nullptr, nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi
    );

    CloseHandle(hWritePipe);
    if (hNullIn != INVALID_HANDLE_VALUE) CloseHandle(hNullIn);

    if (!ok) {
        CloseHandle(hReadPipe);
        return false;
    }

    std::string pending;
    char buf[4096];
    bool cancelled = false;

    for (;;) {
        if (cancel_cb && cancel_cb()) {
            cancelled = true;
            TerminateProcess(pi.hProcess, 1);
            break;
        }

        DWORD avail = 0;
        if (PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD to_read = std::min<DWORD>(avail, sizeof(buf));
            DWORD bytes_read = 0;
            if (ReadFile(hReadPipe, buf, to_read, &bytes_read, nullptr) && bytes_read > 0) {
                if (line_cb) {
                    pending.append(buf, bytes_read);
                    size_t pos = 0;
                    while (true) {
                        size_t nl = pending.find_first_of("\r\n", pos);
                        if (nl == std::string::npos) {
                            if (pos > 0) pending.erase(0, pos);
                            break;
                        }
                        if (nl > pos) {
                            line_cb(std::string_view(pending.data() + pos, nl - pos));
                        }
                        pos = nl + 1;
                    }
                }
                continue;
            }
        }

        DWORD wait_res = WaitForSingleObject(pi.hProcess, 50);
        if (wait_res == WAIT_OBJECT_0) {
            // Drain any remaining bytes in pipe before exiting
            DWORD bytes_read = 0;
            while (PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0 &&
                   ReadFile(hReadPipe, buf, std::min<DWORD>(avail, sizeof(buf)), &bytes_read, nullptr) && bytes_read > 0) {
                if (line_cb) {
                    pending.append(buf, bytes_read);
                }
            }
            if (line_cb && !pending.empty()) {
                size_t pos = 0;
                while (pos < pending.size()) {
                    size_t nl = pending.find_first_of("\r\n", pos);
                    if (nl == std::string::npos) {
                        line_cb(std::string_view(pending.data() + pos, pending.size() - pos));
                        break;
                    }
                    if (nl > pos) {
                        line_cb(std::string_view(pending.data() + pos, nl - pos));
                    }
                    pos = nl + 1;
                }
            }
            break;
        }
    }

    if (cancelled) {
        WaitForSingleObject(pi.hProcess, 2000);
    }

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(hReadPipe);

    if (out_exit_code) *out_exit_code = static_cast<int>(exit_code);
    return !cancelled && exit_code == 0;
#else
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }

    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);

        std::vector<char*> c_args;
        c_args.reserve(args_utf8.size() + 2);
        c_args.push_back(const_cast<char*>(exe_utf8.c_str()));
        for (const auto& a : args_utf8) {
            c_args.push_back(const_cast<char*>(a.c_str()));
        }
        c_args.push_back(nullptr);
        execvp(exe_utf8.c_str(), c_args.data());
        _exit(127);
    }

    close(pipe_fds[1]);
    int flags = fcntl(pipe_fds[0], F_GETFL, 0);
    if (flags >= 0) fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);

    std::string pending;
    char buf[4096];
    bool cancelled = false;
    bool pipe_open = true;
    int status = 0;
    bool reaped = false;

    while (pipe_open || !reaped) {
        if (cancel_cb && cancel_cb()) {
            cancelled = true;
            kill(pid, SIGTERM);
            break;
        }
        if (pipe_open) {
            struct pollfd pfd{pipe_fds[0], POLLIN, 0};
            if (poll(&pfd, 1, 50) > 0) {
                ssize_t n = read(pipe_fds[0], buf, sizeof(buf));
                if (n > 0) {
                    if (line_cb) {
                        pending.append(buf, static_cast<size_t>(n));
                        size_t pos = 0;
                        while (true) {
                            size_t nl = pending.find_first_of("\r\n", pos);
                            if (nl == std::string::npos) {
                                if (pos > 0) pending.erase(0, pos);
                                break;
                            }
                            if (nl > pos) {
                                line_cb(std::string_view(pending.data() + pos, nl - pos));
                            }
                            pos = nl + 1;
                        }
                    }
                } else if (n == 0) {
                    pipe_open = false;
                }
            }
        }
        if (!reaped) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                reaped = true;
            } else if (!pipe_open) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }

    if (cancelled && !reaped) {
        waitpid(pid, &status, 0);
    }
    close(pipe_fds[0]);

    if (line_cb && !pending.empty()) {
        size_t pos = 0;
        while (pos < pending.size()) {
            size_t nl = pending.find_first_of("\r\n", pos);
            if (nl == std::string::npos) {
                line_cb(std::string_view(pending.data() + pos, pending.size() - pos));
                break;
            }
            if (nl > pos) {
                line_cb(std::string_view(pending.data() + pos, nl - pos));
            }
            pos = nl + 1;
        }
    }

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (out_exit_code) *out_exit_code = code;
    return !cancelled && WIFEXITED(status) && code == 0;
#endif
}

bool is_current_process_in_job(bool& out_breakaway_allowed,
                               bool& out_silent_breakaway,
                               uint32_t* out_error) {
    out_breakaway_allowed = false;
    out_silent_breakaway = false;
    if (out_error) *out_error = 0;
#ifdef _WIN32
    BOOL in_job = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &in_job)) {
        if (out_error) *out_error = static_cast<uint32_t>(GetLastError());
        return false;
    }
    if (!in_job) {
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    DWORD returned = 0;
    if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli), &returned)) {
        DWORD flags = jeli.BasicLimitInformation.LimitFlags;
        out_breakaway_allowed = (flags & JOB_OBJECT_LIMIT_BREAKAWAY_OK) != 0;
        out_silent_breakaway = (flags & JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK) != 0;
    } else if (out_error) {
        *out_error = static_cast<uint32_t>(GetLastError());
    }
    return true;
#else
    return false;
#endif
}

DetachedLaunchResult launch_via_create_process(const DetachedLaunchOptions& opts,
                                               bool use_breakaway,
                                               void** out_process_handle) {
    DetachedLaunchResult res;
    if (out_process_handle) *out_process_handle = nullptr;
#ifdef _WIN32
    if (opts.exe_path.empty()) {
        res.error_message = "Empty executable path";
        return res;
    }
    if (opts.allow_foreground) {
        AllowSetForegroundWindow(ASFW_ANY);
    }

    std::wstring wexe = utf8_to_wide(opts.exe_path);
    std::wstring wcmd = L"\"" + wexe + L"\"";
    if (!opts.arguments.empty()) {
        wcmd += L" " + utf8_to_wide(opts.arguments);
    }
    std::vector<wchar_t> cmd_buf(wcmd.begin(), wcmd.end());
    cmd_buf.push_back(L'\0');

    std::wstring wdir;
    const wchar_t* cwd_ptr = nullptr;
    if (!opts.working_dir.empty()) {
        wdir = utf8_to_wide(opts.working_dir);
        cwd_ptr = wdir.c_str();
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = static_cast<WORD>(opts.show_cmd);

    DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    if (use_breakaway) {
        flags |= CREATE_BREAKAWAY_FROM_JOB;
    }

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(
            wexe.c_str(),
            cmd_buf.data(),
            nullptr,
            nullptr,
            FALSE,
            flags,
            nullptr,
            cwd_ptr,
            &si,
            &pi)) {
        res.win32_error = static_cast<uint32_t>(GetLastError());
        res.error_message = "CreateProcessW failed with error " + std::to_string(res.win32_error);
        return res;
    }

    CloseHandle(pi.hThread);
    if (out_process_handle) {
        *out_process_handle = static_cast<void*>(pi.hProcess);
    } else {
        CloseHandle(pi.hProcess);
    }
    res.ok = true;
    res.method = use_breakaway ? "breakaway" : "direct";
    return res;
#else
    res.error_message = "Not supported on non-Windows platform";
    return res;
#endif
}

DetachedLaunchResult launch_via_explorer_com(const DetachedLaunchOptions& opts, int timeout_ms) {
#ifdef _WIN32
    auto run_com_once = [](DetachedLaunchOptions local_opts) -> DetachedLaunchResult {
        DetachedLaunchResult r;
        r.method = "shell_com";

        if (local_opts.allow_foreground) {
            AllowSetForegroundWindow(ASFW_ANY);
        }

        HRESULT hr_com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(hr_com) && hr_com != RPC_E_CHANGED_MODE) {
            r.win32_error = static_cast<uint32_t>(hr_com);
            r.error_message = "CoInitializeEx failed";
            return r;
        }
        const bool should_uninit = (hr_com == S_OK || hr_com == S_FALSE);

        struct ComCleanup {
            bool uninit{false};
            ~ComCleanup() {
                if (uninit) CoUninitialize();
            }
        } com_guard{should_uninit};

        auto format_hr = [](const char* stage, HRESULT hr) -> std::string {
            std::ostringstream oss;
            oss << stage << " failed (0x" << std::hex << static_cast<uint32_t>(hr) << ")";
            return oss.str();
        };

        IShellWindows* psw = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(ShellWindows), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&psw));
        if (FAILED(hr) || !psw) {
            r.win32_error = static_cast<uint32_t>(hr);
            r.error_message = format_hr("CoCreateInstance(ShellWindows)", hr);
            return r;
        }

        struct VariantGuard {
            VARIANT v{};
            VariantGuard() { VariantInit(&v); }
            ~VariantGuard() { VariantClear(&v); }
        };

        VariantGuard v_empty;
        VariantGuard v_empty_root;
        LONG desktop_hwnd = 0;
        IDispatch* pdisp_shell = nullptr;
        hr = psw->FindWindowSW(&v_empty.v, &v_empty_root.v, SWC_DESKTOP, &desktop_hwnd, SWFO_NEEDDISPATCH, &pdisp_shell);
        if (FAILED(hr) || hr == S_FALSE || !pdisp_shell) {
            psw->Release();
            r.win32_error = static_cast<uint32_t>(hr == S_FALSE ? ERROR_FILE_NOT_FOUND : hr);
            r.error_message = (hr == S_FALSE || !pdisp_shell)
                ? "Explorer desktop shell window not present"
                : format_hr("FindWindowSW", hr);
            return r;
        }

        IServiceProvider* psp = nullptr;
        hr = pdisp_shell->QueryInterface(IID_PPV_ARGS(&psp));
        pdisp_shell->Release();
        psw->Release();
        if (FAILED(hr) || !psp) {
            r.win32_error = static_cast<uint32_t>(hr);
            r.error_message = format_hr("QueryInterface(IServiceProvider)", hr);
            return r;
        }

        // Define locally so MinGW builds do not depend on libuuid.a exporting SID_STopLevelBrowser
        static constexpr GUID kSidTopLevelBrowser = {
            0x4c96be40, 0x915c, 0x11cf, {0x99, 0xd3, 0x00, 0xaa, 0x00, 0x4a, 0xe8, 0x37}
        };
        IShellBrowser* psb = nullptr;
        hr = psp->QueryService(kSidTopLevelBrowser, IID_PPV_ARGS(&psb));
        psp->Release();
        if (FAILED(hr) || !psb) {
            r.win32_error = static_cast<uint32_t>(hr);
            r.error_message = format_hr("QueryService(IShellBrowser)", hr);
            return r;
        }

        IShellView* psv = nullptr;
        hr = psb->QueryActiveShellView(&psv);
        psb->Release();
        if (FAILED(hr) || !psv) {
            r.win32_error = static_cast<uint32_t>(hr);
            r.error_message = format_hr("QueryActiveShellView", hr);
            return r;
        }

        IDispatch* pdisp_view = nullptr;
        hr = psv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&pdisp_view));
        psv->Release();
        if (FAILED(hr) || !pdisp_view) {
            r.win32_error = static_cast<uint32_t>(hr);
            r.error_message = format_hr("GetItemObject(SVGIO_BACKGROUND)", hr);
            return r;
        }

        IShellFolderViewDual* psfv = nullptr;
        hr = pdisp_view->QueryInterface(IID_PPV_ARGS(&psfv));
        pdisp_view->Release();
        if (FAILED(hr) || !psfv) {
            r.win32_error = static_cast<uint32_t>(hr);
            r.error_message = format_hr("QueryInterface(IShellFolderViewDual)", hr);
            return r;
        }

        IDispatch* pdisp_app = nullptr;
        hr = psfv->get_Application(&pdisp_app);
        psfv->Release();
        if (FAILED(hr) || !pdisp_app) {
            r.win32_error = static_cast<uint32_t>(hr);
            r.error_message = format_hr("get_Application", hr);
            return r;
        }

        IShellDispatch2* psd = nullptr;
        hr = pdisp_app->QueryInterface(IID_PPV_ARGS(&psd));
        pdisp_app->Release();
        if (FAILED(hr) || !psd) {
            r.win32_error = static_cast<uint32_t>(hr);
            r.error_message = format_hr("QueryInterface(IShellDispatch2)", hr);
            return r;
        }

        std::wstring wexe = utf8_to_wide(local_opts.exe_path);
        BSTR bstr_file = SysAllocString(wexe.c_str());
        if (!bstr_file) {
            psd->Release();
            r.win32_error = ERROR_OUTOFMEMORY;
            r.error_message = "SysAllocString failed";
            return r;
        }

        VariantGuard v_args;
        if (!local_opts.arguments.empty()) {
            std::wstring wargs = utf8_to_wide(local_opts.arguments);
            v_args.v.vt = VT_BSTR;
            v_args.v.bstrVal = SysAllocString(wargs.c_str());
        }

        VariantGuard v_dir;
        if (!local_opts.working_dir.empty()) {
            std::wstring wdir = utf8_to_wide(local_opts.working_dir);
            v_dir.v.vt = VT_BSTR;
            v_dir.v.bstrVal = SysAllocString(wdir.c_str());
        }

        VariantGuard v_op;
        v_op.v.vt = VT_BSTR;
        v_op.v.bstrVal = SysAllocString(L"open");

        VariantGuard v_show;
        v_show.v.vt = VT_I4;
        v_show.v.lVal = local_opts.show_cmd;

        hr = psd->ShellExecute(bstr_file, v_args.v, v_dir.v, v_op.v, v_show.v);
        SysFreeString(bstr_file);
        psd->Release();

        if (FAILED(hr)) {
            r.win32_error = static_cast<uint32_t>(hr);
            r.error_message = format_hr("IShellDispatch2::ShellExecute", hr);
            return r;
        }

        r.ok = true;
        return r;
    };

    auto prom = std::make_shared<std::promise<DetachedLaunchResult>>();
    std::future<DetachedLaunchResult> fut = prom->get_future();
    std::thread([opts, prom, run_com_once]() {
        prom->set_value(run_com_once(opts));
    }).detach();

    if (fut.wait_for(std::chrono::milliseconds(std::max(100, timeout_ms))) == std::future_status::timeout) {
        DetachedLaunchResult timeout_res;
        timeout_res.method = "shell_com";
        timeout_res.win32_error = WAIT_TIMEOUT;
        timeout_res.error_message = "Explorer COM ShellExecute timed out";
        return timeout_res;
    }
    return fut.get();
#else
    (void)opts;
    (void)timeout_ms;
    DetachedLaunchResult res;
    res.error_message = "Not supported on non-Windows platform";
    return res;
#endif
}

DetachedLaunchResult launch_detached_gui_process(const DetachedLaunchOptions& opts,
                                                 void** out_process_handle) {
    if (out_process_handle) *out_process_handle = nullptr;
#ifdef _WIN32
    bool breakaway_ok = false;
    bool silent_breakaway = false;
    uint32_t job_err = 0;
    const bool in_job = is_current_process_in_job(breakaway_ok, silent_breakaway, &job_err);

    if (!in_job || breakaway_ok || silent_breakaway) {
        const bool try_breakaway_flag = in_job && breakaway_ok && !silent_breakaway;
        DetachedLaunchResult cp = launch_via_create_process(opts, try_breakaway_flag, out_process_handle);
        cp.in_job = in_job;
        cp.breakaway_allowed = breakaway_ok;
        cp.silent_breakaway = silent_breakaway;
        if (cp.ok) {
            return cp;
        }
        if (!in_job) {
            return cp;
        }
        // Nested Win8+ jobs may report BREAKAWAY_OK locally while an outer job denies it -> fall through to Explorer COM.
    }

    DetachedLaunchResult com = launch_via_explorer_com(opts, 1500);
    com.in_job = in_job;
    com.breakaway_allowed = breakaway_ok;
    com.silent_breakaway = silent_breakaway;
    return com;
#else
    (void)opts;
    DetachedLaunchResult res;
    res.error_message = "Not supported on non-Windows platform";
    return res;
#endif
}

uint32_t get_process_integrity_rid() {
#ifdef _WIN32
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return 0;
    }
    DWORD len = 0;
    GetTokenInformation(hToken, TokenIntegrityLevel, nullptr, 0, &len);
    if (len == 0) {
        CloseHandle(hToken);
        return 0;
    }
    std::vector<uint8_t> buf(len);
    uint32_t rid = 0;
    if (GetTokenInformation(hToken, TokenIntegrityLevel, buf.data(), len, &len)) {
        auto* til = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
        if (til && til->Label.Sid && IsValidSid(til->Label.Sid)) {
            DWORD count = *GetSidSubAuthorityCount(til->Label.Sid);
            if (count > 0) {
                rid = *GetSidSubAuthority(til->Label.Sid, count - 1);
            }
        }
    }
    CloseHandle(hToken);
    return rid;
#else
    return 0;
#endif
}

std::string format_bytes(int64_t bytes) {
    if (bytes < 0) return "Unknown";
    if (bytes == 0) return "0 Bytes";
    constexpr double KB = 1024.0;
    constexpr double MB = 1024.0 * KB;
    constexpr double GB = 1024.0 * MB;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    if (bytes >= GB) {
        ss << (bytes / GB) << " GB";
    } else if (bytes >= MB) {
        ss << (bytes / MB) << " MB";
    } else if (bytes >= KB) {
        ss << (bytes / KB) << " KB";
    } else {
        return std::to_string(bytes) + " Bytes";
    }
    return ss.str();
}

std::string format_speed(uint64_t bytes_per_sec) {
    if (bytes_per_sec == 0) return "0.00 KB/s";
    return format_bytes(static_cast<int64_t>(bytes_per_sec)) + "/s";
}

std::string format_eta(int64_t eta_sec) {
    if (eta_sec <= 0) return "--:--";
    int64_t hours = eta_sec / 3600;
    int64_t mins = (eta_sec % 3600) / 60;
    int64_t secs = eta_sec % 60;

    std::ostringstream ss;
    if (hours > 0) {
        ss << hours << "h " << mins << "m " << secs << "s";
    } else if (mins > 0) {
        ss << mins << "m " << secs << "s";
    } else {
        ss << secs << "s";
    }
    return ss.str();
}

} // namespace sys
} // namespace opendm
