#include "win32_options_dialog.hpp"
#include "dpi_helper.hpp"
#include "../core/download_queue.hpp"
#include <commctrl.h>
#include <shlobj.h>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace opendm {
namespace win32 {

namespace {

std::string get_default_downloads_dir() {
    PWSTR pPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &pPath))) {
        std::wstring ws(pPath);
        CoTaskMemFree(pPath);
        return std::string(ws.begin(), ws.end());
    }
    const char* userprofile = getenv("USERPROFILE");
    if (userprofile) {
        return (fs::path(userprofile) / "Downloads").string();
    }
    return "Downloads";
}

struct OptionsDialogState {
    HWND hwnd{nullptr};
    HWND hTab{nullptr};
    HWND hBtnOk{nullptr};
    HWND hBtnCancel{nullptr};

    // General tab controls
    HWND hChkStartup{nullptr};
    HWND hChkClipboard{nullptr};

    // Connection tab controls
    HWND hComboMaxConns{nullptr};
    HWND hChkSpeedLimit{nullptr};
    HWND hEditSpeedLimit{nullptr};

    // File Types tab controls
    HWND hEditExts{nullptr};

    // Save To tab controls
    HWND hEditSaveDir{nullptr};
    HWND hBtnBrowse{nullptr};

    std::vector<HWND> tab_controls[4];
    int current_tab{0};

    HFONT hFont{nullptr};
    HFONT hFontBold{nullptr};
    bool done{false};
};

void SwitchTab(OptionsDialogState* state, int tab) {
    state->current_tab = tab;
    for (int t = 0; t < 4; ++t) {
        BOOL show = (t == tab);
        for (HWND h : state->tab_controls[t]) {
            ShowWindow(h, show ? SW_SHOW : SW_HIDE);
        }
    }
}

LRESULT CALLBACK OptionsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OptionsDialogState* state = reinterpret_cast<OptionsDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<OptionsDialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            state->hwnd = hwnd;
            return 0;
        }
        case WM_NOTIFY: {
            NMHDR* nmhdr = reinterpret_cast<NMHDR*>(lParam);
            if (nmhdr->hwndFrom == state->hTab && nmhdr->code == TCN_SELCHANGE) {
                int sel = TabCtrl_GetCurSel(state->hTab);
                SwitchTab(state, sel);
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 601) { // Browse save directory
                wchar_t szDir[MAX_PATH] = {0};
                BROWSEINFOW bi = {};
                bi.hwndOwner = hwnd;
                bi.pszDisplayName = szDir;
                bi.lpszTitle = L"Select Default Download Folder";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
                if (pidl) {
                    if (SHGetPathFromIDListW(pidl, szDir)) {
                        SetWindowTextW(state->hEditSaveDir, szDir);
                    }
                    CoTaskMemFree(pidl);
                }
                return 0;
            }
            if (id == IDOK) {
                // Apply settings
                if (state->hComboMaxConns) {
                    int sel = static_cast<int>(SendMessageW(state->hComboMaxConns, CB_GETCURSEL, 0, 0));
                    int conns_val[] = {1, 2, 4, 8, 16, 24, 32};
                    if (sel >= 0 && sel < 7) {
                        QueueManager::instance().get_main_queue().set_max_concurrent(conns_val[sel]);
                    }
                }
                if (state->hChkSpeedLimit) {
                    bool enable = (SendMessageW(state->hChkSpeedLimit, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    QueueManager::instance().enable_global_speed_limit(enable);
                    wchar_t buf[64] = {0};
                    GetWindowTextW(state->hEditSpeedLimit, buf, 64);
                    int kb = _wtoi(buf);
                    if (kb > 0) {
                        QueueManager::instance().set_global_speed_limit(static_cast<uint64_t>(kb) * 1024);
                    }
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

void Win32OptionsDialog::show(HWND parent, int initial_tab) {
    OptionsDialogState state;
    UINT dpi = DpiHelper::get_dpi(parent);

    state.hFont = DpiHelper::create_font(9, FW_NORMAL, false, dpi);
    state.hFontBold = DpiHelper::create_font(9, FW_SEMIBOLD, false, dpi);

    static bool s_registered = false;
    const wchar_t* className = L"OpenDM_OptionsDialog";
    if (!s_registered) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = OptionsWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        s_registered = true;
    }

    int dlg_w = DpiHelper::scale(520, dpi);
    int dlg_h = DpiHelper::scale(360, dpi);

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
        className, L"Options and Preferences",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlg_w, dlg_h,
        parent, nullptr, GetModuleHandleW(nullptr), &state
    );

    int margin_x = DpiHelper::scale(12, dpi);
    int tab_w = dlg_w - margin_x * 2 - DpiHelper::scale(16, dpi);
    int tab_h = DpiHelper::scale(250, dpi);

    state.hTab = CreateWindowExW(
        0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        margin_x, DpiHelper::scale(10, dpi), tab_w, tab_h,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(state.hTab, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    TCITEMW tie = {TCIF_TEXT};
    tie.pszText = (LPWSTR)L"General";
    TabCtrl_InsertItem(state.hTab, 0, &tie);
    tie.pszText = (LPWSTR)L"Connection";
    TabCtrl_InsertItem(state.hTab, 1, &tie);
    tie.pszText = (LPWSTR)L"File Types";
    TabCtrl_InsertItem(state.hTab, 2, &tie);
    tie.pszText = (LPWSTR)L"Save To";
    TabCtrl_InsertItem(state.hTab, 3, &tie);

    int page_x = margin_x + DpiHelper::scale(16, dpi);
    int page_y = DpiHelper::scale(45, dpi);
    int page_w = tab_w - DpiHelper::scale(32, dpi);

    // Tab 0: General
    state.hChkStartup = CreateWindowExW(0, L"BUTTON", L"Launch OpenDM on Windows startup",
        WS_CHILD | BS_AUTOCHECKBOX, page_x, page_y, page_w, DpiHelper::scale(22, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hChkStartup, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    state.tab_controls[0].push_back(state.hChkStartup);

    state.hChkClipboard = CreateWindowExW(0, L"BUTTON", L"Automatically monitor clipboard for downloadable URLs",
        WS_CHILD | BS_AUTOCHECKBOX, page_x, page_y + DpiHelper::scale(28, dpi), page_w, DpiHelper::scale(22, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hChkClipboard, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    SendMessageW(state.hChkClipboard, BM_SETCHECK, BST_CHECKED, 0);
    state.tab_controls[0].push_back(state.hChkClipboard);

    // Tab 1: Connection
    HWND hLblConns = CreateWindowExW(0, L"STATIC", L"Max concurrent connections per download:",
        WS_CHILD, page_x, page_y, page_w, DpiHelper::scale(20, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblConns, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    state.tab_controls[1].push_back(hLblConns);

    state.hComboMaxConns = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST | WS_TABSTOP,
        page_x, page_y + DpiHelper::scale(24, dpi), DpiHelper::scale(120, dpi), DpiHelper::scale(120, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hComboMaxConns, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    const wchar_t* conn_opts[] = {L"1", L"2", L"4", L"8", L"16", L"24", L"32", nullptr};
    int current_max = static_cast<int>(QueueManager::instance().get_main_queue().get_max_concurrent());
    int cur_sel = 3; // default 8
    int conns_val[] = {1, 2, 4, 8, 16, 24, 32};
    for (int i = 0; conn_opts[i]; ++i) {
        SendMessageW(state.hComboMaxConns, CB_ADDSTRING, 0, (LPARAM)conn_opts[i]);
        if (conns_val[i] == current_max) cur_sel = i;
    }
    SendMessageW(state.hComboMaxConns, CB_SETCURSEL, cur_sel, 0);
    state.tab_controls[1].push_back(state.hComboMaxConns);

    state.hChkSpeedLimit = CreateWindowExW(0, L"BUTTON", L"Enable global speed limiter (KB/s):",
        WS_CHILD | BS_AUTOCHECKBOX, page_x, page_y + DpiHelper::scale(65, dpi), DpiHelper::scale(240, dpi), DpiHelper::scale(22, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hChkSpeedLimit, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    if (QueueManager::instance().is_global_speed_limit_enabled()) {
        SendMessageW(state.hChkSpeedLimit, BM_SETCHECK, BST_CHECKED, 0);
    }
    state.tab_controls[1].push_back(state.hChkSpeedLimit);

    uint64_t cur_limit_kb = QueueManager::instance().get_global_speed_limit() / 1024;
    std::wstring wlimit = std::to_wstring(cur_limit_kb > 0 ? cur_limit_kb : 1024);
    state.hEditSpeedLimit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wlimit.c_str(),
        WS_CHILD | ES_NUMBER, page_x + DpiHelper::scale(245, dpi), page_y + DpiHelper::scale(64, dpi), DpiHelper::scale(70, dpi), DpiHelper::scale(22, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hEditSpeedLimit, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    state.tab_controls[1].push_back(state.hEditSpeedLimit);

    // Tab 2: File Types
    HWND hLblExts = CreateWindowExW(0, L"STATIC", L"Automatically intercept downloads for these file extensions:",
        WS_CHILD, page_x, page_y, page_w, DpiHelper::scale(20, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblExts, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    state.tab_controls[2].push_back(hLblExts);

    const wchar_t* default_exts = L"3GP 7Z AAC ACE AI AIF APK ARJ ASF AVI BIN BZ2 DEB DMG DOC DOCX EPUB EXE FLAC FLV GZ GZIP IMG ISO LZH M4A M4V MKV MOV MP3 MP4 MPA MPE MPEG MPG MSI MSU OGG OGV PDF PKG PLJ PPS PPT PPTX QT R0* RA RAR RM RMVB RPM SEA SIT SITX TAR TORRENT TS VTT WAV WEBM WMA WMV XLS XLSX Z ZIP";
    state.hEditExts = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", default_exts,
        WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
        page_x, page_y + DpiHelper::scale(26, dpi), page_w, DpiHelper::scale(120, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hEditExts, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    state.tab_controls[2].push_back(state.hEditExts);

    // Tab 3: Save To
    HWND hLblSave = CreateWindowExW(0, L"STATIC", L"Default download directory:",
        WS_CHILD, page_x, page_y, page_w, DpiHelper::scale(20, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblSave, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    state.tab_controls[3].push_back(hLblSave);

    std::string def_dir = get_default_downloads_dir();
    std::wstring wdef_dir(def_dir.begin(), def_dir.end());
    state.hEditSaveDir = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wdef_dir.c_str(),
        WS_CHILD | ES_AUTOHSCROLL,
        page_x, page_y + DpiHelper::scale(26, dpi), page_w - DpiHelper::scale(40, dpi), DpiHelper::scale(24, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hEditSaveDir, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    state.tab_controls[3].push_back(state.hEditSaveDir);

    state.hBtnBrowse = CreateWindowExW(0, L"BUTTON", L"...",
        WS_CHILD | BS_PUSHBUTTON,
        page_x + page_w - DpiHelper::scale(35, dpi), page_y + DpiHelper::scale(26, dpi), DpiHelper::scale(35, dpi), DpiHelper::scale(24, dpi),
        hwnd, (HMENU)601, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnBrowse, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    state.tab_controls[3].push_back(state.hBtnBrowse);

    // Select initial tab
    TabCtrl_SetCurSel(state.hTab, initial_tab);
    SwitchTab(&state, initial_tab);

    // OK and Cancel buttons
    int btn_w = DpiHelper::scale(88, dpi);
    int btn_h = DpiHelper::scale(26, dpi);
    int btn_y = tab_h + DpiHelper::scale(20, dpi);

    state.hBtnOk = CreateWindowExW(0, L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        dlg_w - margin_x - btn_w * 2 - DpiHelper::scale(24, dpi), btn_y, btn_w, btn_h,
        hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnOk, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    state.hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        dlg_w - margin_x - btn_w - DpiHelper::scale(16, dpi), btn_y, btn_w, btn_h,
        hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnCancel, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    SetFocus(state.hBtnOk);

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
}

} // namespace win32
} // namespace opendm
