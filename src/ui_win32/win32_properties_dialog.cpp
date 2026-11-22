#include "win32_properties_dialog.hpp"
#include "win32_dialogs_common.hpp"
#include "dpi_helper.hpp"
#include "../core/win32_unicode.hpp"

namespace opendm {
namespace win32 {

namespace {

inline std::wstring utf8_to_wide(const std::string& str) { return sys::utf8_to_wide(str); }
inline std::string format_bytes(int64_t bytes) {
    return sys::format_bytes(bytes) + (bytes >= 0 ? (" (" + std::to_string(bytes) + " bytes)") : "");
}

struct PropDialogState {
    HFONT hFont{nullptr};
    HFONT hFontBold{nullptr};
    bool done{false};
};

LRESULT CALLBACK PropWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PropDialogState* state = reinterpret_cast<PropDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<PropDialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK || id == IDCANCEL) {
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

void Win32PropertiesDialog::show(HWND parent, std::shared_ptr<DownloadSession> session) {
    if (!session) return;

    PropDialogState state;
    UINT dpi = DpiHelper::get_dpi(parent);
    state.hFont = DpiHelper::create_font(9, FW_NORMAL, false, dpi);
    state.hFontBold = DpiHelper::create_font(9, FW_SEMIBOLD, false, dpi);

    static bool s_registered = false;
    const wchar_t* className = L"OpenDM_PropDialog";
    if (!s_registered) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = PropWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        s_registered = true;
    }

    int dlg_w = DpiHelper::scale(540, dpi);
    int dlg_h = DpiHelper::scale(380, dpi);

    RECT rcParent;
    if (parent && GetWindowRect(parent, &rcParent)) {
        // center
    } else {
        GetWindowRect(GetDesktopWindow(), &rcParent);
    }
    int x = rcParent.left + ((rcParent.right - rcParent.left) - dlg_w) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - dlg_h) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        className, L"Download Properties",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlg_w, dlg_h,
        parent, nullptr, GetModuleHandleW(nullptr), &state
    );

    int margin_x = DpiHelper::scale(18, dpi);
    int lbl_w = DpiHelper::scale(110, dpi);
    int val_x = margin_x + lbl_w + DpiHelper::scale(8, dpi);
    int val_w = dlg_w - val_x - margin_x - DpiHelper::scale(16, dpi);
    int row_h = DpiHelper::scale(24, dpi);
    int cur_y = DpiHelper::scale(16, dpi);

    auto prog = session->get_progress();

    auto add_row = [&](const wchar_t* lbl, const std::string& text, bool is_edit = false) {
        HWND hL = CreateWindowExW(0, L"STATIC", lbl, WS_CHILD | WS_VISIBLE,
            margin_x, cur_y + 2, lbl_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hL, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

        std::wstring wt = utf8_to_wide(text);
        HWND hV = nullptr;
        if (is_edit) {
            hV = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wt.c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                val_x, cur_y, val_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        } else {
            hV = CreateWindowExW(0, L"STATIC", wt.c_str(),
                WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                val_x, cur_y + 2, val_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
        SendMessageW(hV, WM_SETFONT, (WPARAM)state.hFont, TRUE);

        cur_y += row_h + DpiHelper::scale(6, dpi);
    };

    add_row(L"File Name:", session->get_filename());
    add_row(L"URL Address:", session->get_url(), true);
    add_row(L"Saved Location:", session->get_destination_path(), true);
    add_row(L"File Size:", format_bytes(prog.total_size));
    add_row(L"Downloaded:", format_bytes(prog.downloaded_bytes));

    std::string st = "Idle";
    if (prog.status == SessionStatus::DOWNLOADING) st = "Downloading";
    else if (prog.status == SessionStatus::PAUSED) st = "Paused";
    else if (prog.status == SessionStatus::COMPLETED) st = "Completed";
    else if (prog.status == SessionStatus::ERROR) st = "Error: " + prog.error_message;
    add_row(L"Status:", st);

    add_row(L"Resume Support:", session->supports_ranges() ? "Yes" : "No");
    add_row(L"Category:", session->get_category());

    // Close button
    int btn_w = DpiHelper::scale(88, dpi);
    int btn_h = DpiHelper::scale(26, dpi);
    int btn_y = dlg_h - btn_h - DpiHelper::scale(50, dpi);

    HWND hBtnOk = CreateWindowExW(0, L"BUTTON", L"Close",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        (dlg_w - btn_w) / 2, btn_y, btn_w, btn_h,
        hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hBtnOk, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    SetFocus(hBtnOk);

    run_modal_loop(hwnd, parent, state.done);

    if (state.hFont) DeleteObject(state.hFont);
    if (state.hFontBold) DeleteObject(state.hFontBold);
}

} // namespace win32
} // namespace opendm
