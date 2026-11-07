#include "win32_download_complete_dialog.hpp"
#include "dpi_helper.hpp"
#include "win32_assets.hpp"
#include <shellapi.h>
#include <filesystem>
#include <sstream>
#include <iomanip>

namespace fs = std::filesystem;

namespace opendm {
namespace win32 {

namespace {

std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (needed <= 0) return std::wstring(str.begin(), str.end());
    std::wstring out(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), out.data(), needed);
    return out;
}

std::string wide_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string(wstr.begin(), wstr.end());
    std::string out(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::string format_size(uint64_t bytes) {
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
        ss << bytes << " Bytes";
    }
    return ss.str();
}

struct CompleteDialogState {
    std::string filepath;
    uint64_t file_size{0};
    HFONT hFont{nullptr};
    HFONT hFontBold{nullptr};
    HICON hFileIcon{nullptr};
    bool done{false};
};

LRESULT CALLBACK CompleteWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CompleteDialogState* state = reinterpret_cast<CompleteDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<CompleteDialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 401) { // Open
                std::wstring wp = utf8_to_wide(state->filepath);
                ShellExecuteW(hwnd, L"open", wp.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == 402) { // Open Folder
                std::wstring wp = utf8_to_wide(state->filepath);
                std::error_code ec;
                if (fs::exists(fs::path(wp), ec)) {
                    std::wstring param = L"/select,\"" + wp + L"\"";
                    ShellExecuteW(hwnd, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
                } else {
                    std::wstring pdir = fs::path(wp).parent_path().wstring();
                    ShellExecuteW(hwnd, L"open", pdir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == 403 || id == IDCANCEL) { // Close
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE: {
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void Win32DownloadCompleteDialog::show(HWND parent, const std::string& filepath, uint64_t file_size) {
    CompleteDialogState state;
    state.filepath = filepath;
    state.file_size = file_size;

    UINT dpi = DpiHelper::get_dpi(parent);
    state.hFont = DpiHelper::create_font(9, FW_NORMAL, false, dpi);
    state.hFontBold = DpiHelper::create_font(9, FW_SEMIBOLD, false, dpi);
    state.hFileIcon = Win32Assets::get_file_type_icon(filepath, false);

    static bool s_registered = false;
    const wchar_t* className = L"OpenDM_CompleteDialog";
    if (!s_registered) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = CompleteWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        s_registered = true;
    }

    int dlg_w = DpiHelper::scale(480, dpi);
    int dlg_h = DpiHelper::scale(220, dpi);

    RECT rcParent;
    if (parent && GetWindowRect(parent, &rcParent)) {
        // center
    } else {
        GetWindowRect(GetDesktopWindow(), &rcParent);
    }
    int x = rcParent.left + ((rcParent.right - rcParent.left) - dlg_w) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - dlg_h) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        className,
        L"Download complete",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlg_w, dlg_h,
        parent, nullptr, GetModuleHandleW(nullptr), &state
    );

    int margin_x = DpiHelper::scale(20, dpi);
    int icon_size = DpiHelper::scale(32, dpi);

    // Icon
    HWND hIcon = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_ICON,
        margin_x, DpiHelper::scale(20, dpi), icon_size, icon_size,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hIcon, STM_SETICON, (WPARAM)state.hFileIcon, 0);

    // File info
    fs::path p(utf8_to_wide(filepath));
    std::wstring wfn = p.filename().wstring();

    int text_x = margin_x + icon_size + DpiHelper::scale(16, dpi);
    int text_w = dlg_w - text_x - margin_x - DpiHelper::scale(16, dpi);

    HWND hLblName = CreateWindowExW(0, L"STATIC", wfn.c_str(),
        WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
        text_x, DpiHelper::scale(20, dpi), text_w, DpiHelper::scale(20, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblName, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    std::string info_text = "Size: " + format_size(file_size) + "\nSaved to: " + wide_to_utf8(p.parent_path().wstring());
    std::wstring winfo = utf8_to_wide(info_text);
    HWND hLblInfo = CreateWindowExW(0, L"STATIC", winfo.c_str(),
        WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
        text_x, DpiHelper::scale(44, dpi), text_w, DpiHelper::scale(45, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblInfo, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    // Buttons: Open, Open Folder, Close
    int btn_w = DpiHelper::scale(100, dpi);
    int btn_h = DpiHelper::scale(26, dpi);
    int btn_y = DpiHelper::scale(120, dpi);
    int total_btns_w = btn_w * 3 + DpiHelper::scale(16, dpi);
    int start_btn_x = (dlg_w - total_btns_w) / 2;

    HWND hBtnOpen = CreateWindowExW(0, L"BUTTON", L"Open",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        start_btn_x, btn_y, btn_w, btn_h,
        hwnd, (HMENU)401, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hBtnOpen, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    HWND hBtnFolder = CreateWindowExW(0, L"BUTTON", L"Open Folder",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        start_btn_x + btn_w + DpiHelper::scale(8, dpi), btn_y, btn_w, btn_h,
        hwnd, (HMENU)402, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hBtnFolder, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    HWND hBtnClose = CreateWindowExW(0, L"BUTTON", L"Close",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        start_btn_x + (btn_w + DpiHelper::scale(8, dpi)) * 2, btn_y, btn_w, btn_h,
        hwnd, (HMENU)403, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hBtnClose, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    SetFocus(hBtnOpen);

    if (parent) EnableWindow(parent, FALSE);

    MSG msg;
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (parent) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }

    if (state.hFont) DeleteObject(state.hFont);
    if (state.hFontBold) DeleteObject(state.hFontBold);
    if (state.hFileIcon) DestroyIcon(state.hFileIcon);
}

} // namespace win32
} // namespace opendm
