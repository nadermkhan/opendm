#include "win32_add_url_dialog.hpp"
#include "dpi_helper.hpp"
#include <commctrl.h>
#include <vector>

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

std::string get_clipboard_url() {
    if (!OpenClipboard(nullptr)) return "";
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) {
        CloseClipboard();
        return "";
    }
    const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(hData));
    std::string res;
    if (text) {
        std::wstring wstr(text);
        GlobalUnlock(hData);
        CloseClipboard();
        while (!wstr.empty() && (wstr.front() == L' ' || wstr.front() == L'\r' || wstr.front() == L'\n' || wstr.front() == L'\t')) {
            wstr.erase(0, 1);
        }
        // Check if starts with http://, https://, or ftp://
        if (wstr.rfind(L"http://", 0) == 0 || wstr.rfind(L"https://", 0) == 0 || wstr.rfind(L"ftp://", 0) == 0) {
            // Trim whitespace/newlines
            while (!wstr.empty() && (wstr.back() == L' ' || wstr.back() == L'\r' || wstr.back() == L'\n' || wstr.back() == L'\t')) {
                wstr.pop_back();
            }
            res = wide_to_utf8(wstr);
        }
    } else {
        CloseClipboard();
    }
    return res;
}

struct DialogState {
    HWND hwnd{nullptr};
    HWND hEditUrl{nullptr};
    HWND hChkAuth{nullptr};
    HWND hEditUser{nullptr};
    HWND hEditPass{nullptr};
    HWND hBtnOk{nullptr};
    HWND hBtnCancel{nullptr};
    HFONT hFont{nullptr};
    HFONT hFontBold{nullptr};
    AddUrlResult result;
    bool done{false};
};

LRESULT CALLBACK AddUrlWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogState* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<DialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            state->hwnd = hwnd;
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if (id == 103 && code == BN_CLICKED) { // Auth Checkbox
                BOOL checked = (SendMessageW(state->hChkAuth, BM_GETCHECK, 0, 0) == BST_CHECKED);
                EnableWindow(state->hEditUser, checked);
                EnableWindow(state->hEditPass, checked);
                if (checked) SetFocus(state->hEditUser);
                return 0;
            }
            if (id == IDOK) {
                wchar_t buf[2048] = {0};
                GetWindowTextW(state->hEditUrl, buf, 2048);
                std::wstring wurl(buf);
                // Trim leading whitespace
                size_t start = wurl.find_first_not_of(L" \t\r\n");
                if (start != std::wstring::npos) {
                    size_t end = wurl.find_last_not_of(L" \t\r\n");
                    wurl = wurl.substr(start, end - start + 1);
                } else {
                    wurl.clear();
                }
                if (!wurl.empty()) {
                    state->result.url = wide_to_utf8(wurl);
                    if (SendMessageW(state->hChkAuth, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                        GetWindowTextW(state->hEditUser, buf, 2048);
                        std::wstring wu(buf);
                        state->result.username = wide_to_utf8(wu);

                        GetWindowTextW(state->hEditPass, buf, 2048);
                        std::wstring wp(buf);
                        state->result.password = wide_to_utf8(wp);
                    }
                    state->result.accepted = true;
                }
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDCANCEL) {
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

AddUrlResult Win32AddUrlDialog::show(HWND parent, const std::string& default_url) {
    DialogState state;

    UINT dpi = DpiHelper::get_dpi(parent);
    state.hFont = DpiHelper::create_font(9, FW_NORMAL, false, dpi);
    state.hFontBold = DpiHelper::create_font(9, FW_SEMIBOLD, false, dpi);

    // Register class
    static bool s_registered = false;
    const wchar_t* className = L"OpenDM_AddUrlDialog";
    if (!s_registered) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = AddUrlWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        s_registered = true;
    }

    int dlg_w = DpiHelper::scale(520, dpi);
    int dlg_h = DpiHelper::scale(240, dpi);

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
        L"Enter new address to download",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlg_w, dlg_h,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        &state
    );

    int margin_x = DpiHelper::scale(20, dpi);
    int content_w = dlg_w - margin_x * 2 - DpiHelper::scale(16, dpi);

    // Address label
    HWND hLbl = CreateWindowExW(
        0, L"STATIC", L"Address (URL):",
        WS_CHILD | WS_VISIBLE,
        margin_x, DpiHelper::scale(15, dpi), content_w, DpiHelper::scale(18, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(hLbl, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    // Address edit
    state.hEditUrl = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        margin_x, DpiHelper::scale(36, dpi), content_w, DpiHelper::scale(24, dpi),
        hwnd, (HMENU)101, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(state.hEditUrl, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    // Initial URL
    std::string initial_url = default_url;
    if (initial_url.empty()) {
        initial_url = get_clipboard_url();
    }
    if (!initial_url.empty()) {
        std::wstring wurl = utf8_to_wide(initial_url);
        SetWindowTextW(state.hEditUrl, wurl.c_str());
        SendMessageW(state.hEditUrl, EM_SETSEL, 0, -1);
    }

    // Checkbox for auth
    state.hChkAuth = CreateWindowExW(
        0, L"BUTTON", L"Use HTTP / FTP Authorization",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        margin_x, DpiHelper::scale(70, dpi), content_w, DpiHelper::scale(20, dpi),
        hwnd, (HMENU)103, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(state.hChkAuth, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    int auth_y = DpiHelper::scale(98, dpi);
    int col_w = (content_w - DpiHelper::scale(10, dpi)) / 2;

    // Username label + edit
    HWND hLblUser = CreateWindowExW(
        0, L"STATIC", L"User:",
        WS_CHILD | WS_VISIBLE,
        margin_x, auth_y, DpiHelper::scale(40, dpi), DpiHelper::scale(20, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(hLblUser, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    state.hEditUser = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED | ES_AUTOHSCROLL,
        margin_x + DpiHelper::scale(45, dpi), auth_y, col_w - DpiHelper::scale(45, dpi), DpiHelper::scale(24, dpi),
        hwnd, (HMENU)104, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(state.hEditUser, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    // Password label + edit
    int pass_x = margin_x + col_w + DpiHelper::scale(10, dpi);
    HWND hLblPass = CreateWindowExW(
        0, L"STATIC", L"Password:",
        WS_CHILD | WS_VISIBLE,
        pass_x, auth_y, DpiHelper::scale(65, dpi), DpiHelper::scale(20, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(hLblPass, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    state.hEditPass = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED | ES_PASSWORD | ES_AUTOHSCROLL,
        pass_x + DpiHelper::scale(70, dpi), auth_y, col_w - DpiHelper::scale(70, dpi), DpiHelper::scale(24, dpi),
        hwnd, (HMENU)105, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(state.hEditPass, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    // Buttons: OK and Cancel
    int btn_w = DpiHelper::scale(88, dpi);
    int btn_h = DpiHelper::scale(26, dpi);
    int btn_y = DpiHelper::scale(145, dpi);

    state.hBtnOk = CreateWindowExW(
        0, L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        dlg_w - margin_x - btn_w * 2 - DpiHelper::scale(18, dpi), btn_y, btn_w, btn_h,
        hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(state.hBtnOk, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    state.hBtnCancel = CreateWindowExW(
        0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        dlg_w - margin_x - btn_w - DpiHelper::scale(10, dpi), btn_y, btn_w, btn_h,
        hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(state.hBtnCancel, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    SetFocus(state.hEditUrl);

    // Modal message loop
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

    return state.result;
}

} // namespace win32
} // namespace opendm
