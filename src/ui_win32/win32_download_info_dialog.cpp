#include "win32_download_info_dialog.hpp"
#include "dpi_helper.hpp"
#include "win32_assets.hpp"
#include "../core/download_queue.hpp"
#include <commctrl.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <vector>

namespace fs = std::filesystem;

namespace opendm {
namespace win32 {

namespace {

static const char* kCategories[] = {
    "General",
    "Compressed",
    "Documents",
    "Music",
    "Programs",
    "Video",
    nullptr
};

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

std::string get_default_downloads_dir() {
    PWSTR pPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &pPath))) {
        std::wstring ws(pPath);
        CoTaskMemFree(pPath);
        return wide_to_utf8(ws);
    }
    const char* userprofile = getenv("USERPROFILE");
    if (userprofile) {
        return (fs::path(userprofile) / "Downloads").string();
    }
    return "Downloads";
}

std::string format_size(uint64_t bytes) {
    if (bytes == 0) return "Unknown";
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
    ss << " (" << bytes << " bytes)";
    return ss.str();
}

#define WM_PROBE_COMPLETE (WM_USER + 101)

struct InfoDialogState {
    HWND hwnd{nullptr};
    HWND hEditUrl{nullptr};
    HWND hComboCat{nullptr};
    HWND hEditSaveAs{nullptr};
    HWND hBtnBrowse{nullptr};
    HWND hStaticSize{nullptr};
    HWND hEditDesc{nullptr};
    HWND hBtnStart{nullptr};
    HWND hBtnLater{nullptr};
    HWND hBtnCancel{nullptr};
    HWND hBtnZip{nullptr};

    HFONT hFont{nullptr};
    HFONT hFontBold{nullptr};

    DownloadInfoResult result;
    bool done{false};
    std::atomic<bool> destroyed{false};
};

LRESULT CALLBACK DownloadInfoWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    InfoDialogState* state = reinterpret_cast<InfoDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<InfoDialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            state->hwnd = hwnd;
            return 0;
        }
        case WM_PROBE_COMPLETE: {
            if (state && !state->destroyed) {
                uint64_t probed_sz = (static_cast<uint64_t>(lParam) << 32) | static_cast<uint32_t>(wParam);
                if (probed_sz > 0) {
                    state->result.info.file_size = probed_sz;
                    std::string sz_str = format_size(probed_sz);
                    std::wstring wsz(sz_str.begin(), sz_str.end());
                    SetWindowTextW(state->hStaticSize, wsz.c_str());
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);

            if (id == 201 && code == CBN_SELCHANGE) { // Category combobox
                int idx = static_cast<int>(SendMessageW(state->hComboCat, CB_GETCURSEL, 0, 0));
                if (idx >= 0 && idx < 6) {
                    state->result.info.category = kCategories[idx];

                    // Preserve user-edited filename from edit box if present
                    wchar_t cur_buf[2048] = {0};
                    GetWindowTextW(state->hEditSaveAs, cur_buf, 2048);
                    std::wstring cur_ws(cur_buf);
                    if (!cur_ws.empty()) {
                        fs::path cur_p(cur_ws);
                        if (!cur_p.filename().wstring().empty()) {
                            state->result.info.filename = wide_to_utf8(cur_p.filename().wstring());
                        }
                    }

                    std::string root = get_default_downloads_dir();
                    std::string new_dir = (state->result.info.category == "General") ? root : (fs::path(utf8_to_wide(root)) / utf8_to_wide(state->result.info.category)).string();
                    state->result.info.destination_dir = new_dir;
                    fs::path full_p = fs::path(utf8_to_wide(new_dir)) / utf8_to_wide(state->result.info.filename);
                    state->result.info.save_as_path = wide_to_utf8(full_p.wstring());
                    std::wstring ws = utf8_to_wide(state->result.info.save_as_path);
                    SetWindowTextW(state->hEditSaveAs, ws.c_str());
                }
                return 0;
            }
            if (id == 202) { // Browse button
                wchar_t szFile[MAX_PATH] = {0};
                std::wstring ws = utf8_to_wide(state->result.info.save_as_path);
                wcsncpy(szFile, ws.c_str(), MAX_PATH - 1);

                OPENFILENAMEW ofn = {sizeof(OPENFILENAMEW)};
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
                ofn.nFilterIndex = 1;
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

                if (GetSaveFileNameW(&ofn)) {
                    std::wstring chosen(szFile);
                    state->result.info.save_as_path = wide_to_utf8(chosen);
                    fs::path cp(chosen);
                    state->result.info.destination_dir = wide_to_utf8(cp.parent_path().wstring());
                    state->result.info.filename = wide_to_utf8(cp.filename().wstring());
                    SetWindowTextW(state->hEditSaveAs, chosen.c_str());
                }
                return 0;
            }
            if (id == 203) { // Start Download
                wchar_t buf[2048] = {0};
                GetWindowTextW(state->hEditSaveAs, buf, 2048);
                std::wstring ws(buf);
                if (!ws.empty()) {
                    state->result.info.save_as_path = wide_to_utf8(ws);
                    fs::path cp(ws);
                    state->result.info.destination_dir = wide_to_utf8(cp.parent_path().wstring());
                    state->result.info.filename = wide_to_utf8(cp.filename().wstring());
                }
                state->result.action = DownloadInfoAction::START_NOW;
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == 204) { // Download Later
                wchar_t buf[2048] = {0};
                GetWindowTextW(state->hEditSaveAs, buf, 2048);
                std::wstring ws(buf);
                if (!ws.empty()) {
                    state->result.info.save_as_path = wide_to_utf8(ws);
                    fs::path cp(ws);
                    state->result.info.destination_dir = wide_to_utf8(cp.parent_path().wstring());
                    state->result.info.filename = wide_to_utf8(cp.filename().wstring());
                }
                state->result.action = DownloadInfoAction::DOWNLOAD_LATER;
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDCANCEL || id == 205) { // Cancel
                state->result.action = DownloadInfoAction::CANCEL;
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == 206) { // ZIP Preview
                state->result.action = DownloadInfoAction::ZIP_PREVIEW;
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE: {
            state->result.action = DownloadInfoAction::CANCEL;
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY: {
            state->destroyed = true;
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

DownloadInfoResult Win32DownloadInfoDialog::show(HWND parent, const DownloadPromptInfo& initial_info) {
    InfoDialogState state;
    state.result.info = initial_info;

    if (state.result.info.filename.empty()) {
        state.result.info.filename = "download.bin";
    }
    if (state.result.info.category.empty()) {
        state.result.info.category = QueueManager::detect_category(state.result.info.filename);
    }
    if (state.result.info.destination_dir.empty()) {
        std::string root = get_default_downloads_dir();
        fs::path dp = (state.result.info.category == "General") ? fs::path(utf8_to_wide(root)) : (fs::path(utf8_to_wide(root)) / utf8_to_wide(state.result.info.category));
        state.result.info.destination_dir = wide_to_utf8(dp.wstring());
    }
    if (state.result.info.save_as_path.empty()) {
        fs::path sp = fs::path(utf8_to_wide(state.result.info.destination_dir)) / utf8_to_wide(state.result.info.filename);
        state.result.info.save_as_path = wide_to_utf8(sp.wstring());
    }

    // Check if archive
    std::string ext = "";
    size_t dot = state.result.info.filename.rfind('.');
    if (dot != std::string::npos) {
        ext = state.result.info.filename.substr(dot + 1);
        for (char& c : ext) c = static_cast<char>(std::tolower(c));
    }
    bool is_zip = (ext == "zip" || state.result.info.is_archive);

    UINT dpi = DpiHelper::get_dpi(parent);
    state.hFont = DpiHelper::create_font(9, FW_NORMAL, false, dpi);
    state.hFontBold = DpiHelper::create_font(9, FW_SEMIBOLD, false, dpi);

    static bool s_registered = false;
    const wchar_t* className = L"OpenDM_DownloadInfoDialog";
    if (!s_registered) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = DownloadInfoWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        s_registered = true;
    }

    int dlg_w = DpiHelper::scale(580, dpi);
    int dlg_h = DpiHelper::scale(330, dpi);

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
        L"Download File Info",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlg_w, dlg_h,
        parent, nullptr, GetModuleHandleW(nullptr), &state
    );

    int margin_x = DpiHelper::scale(18, dpi);
    int lbl_w = DpiHelper::scale(90, dpi);
    int input_x = margin_x + lbl_w + DpiHelper::scale(8, dpi);
    int input_w = dlg_w - input_x - margin_x - DpiHelper::scale(16, dpi);
    int row_h = DpiHelper::scale(24, dpi);
    int cur_y = DpiHelper::scale(16, dpi);

    // URL row
    HWND hLblUrl = CreateWindowExW(0, L"STATIC", L"URL:", WS_CHILD | WS_VISIBLE,
        margin_x, cur_y + 2, lbl_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblUrl, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    state.hEditUrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
        input_x, cur_y, input_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hEditUrl, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    std::wstring wurl = utf8_to_wide(state.result.info.url);
    SetWindowTextW(state.hEditUrl, wurl.c_str());

    cur_y += DpiHelper::scale(32, dpi);

    // Category row
    HWND hLblCat = CreateWindowExW(0, L"STATIC", L"Category:", WS_CHILD | WS_VISIBLE,
        margin_x, cur_y + 2, lbl_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblCat, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    state.hComboCat = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        input_x, cur_y, DpiHelper::scale(180, dpi), DpiHelper::scale(150, dpi),
        hwnd, (HMENU)201, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hComboCat, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    int cat_sel = 0;
    for (int i = 0; kCategories[i]; ++i) {
        std::wstring wc(kCategories[i], kCategories[i] + strlen(kCategories[i]));
        SendMessageW(state.hComboCat, CB_ADDSTRING, 0, (LPARAM)wc.c_str());
        if (state.result.info.category == kCategories[i]) cat_sel = i;
    }
    SendMessageW(state.hComboCat, CB_SETCURSEL, cat_sel, 0);

    cur_y += DpiHelper::scale(32, dpi);

    // Save As row
    HWND hLblSave = CreateWindowExW(0, L"STATIC", L"Save As:", WS_CHILD | WS_VISIBLE,
        margin_x, cur_y + 2, lbl_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblSave, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    int browse_btn_w = DpiHelper::scale(36, dpi);
    state.hEditSaveAs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        input_x, cur_y, input_w - browse_btn_w - DpiHelper::scale(6, dpi), row_h,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hEditSaveAs, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    std::wstring wsa = utf8_to_wide(state.result.info.save_as_path);
    SetWindowTextW(state.hEditSaveAs, wsa.c_str());

    state.hBtnBrowse = CreateWindowExW(0, L"BUTTON", L"...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        input_x + input_w - browse_btn_w, cur_y, browse_btn_w, row_h,
        hwnd, (HMENU)202, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnBrowse, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    cur_y += DpiHelper::scale(32, dpi);

    // File Size row
    HWND hLblSize = CreateWindowExW(0, L"STATIC", L"File Size:", WS_CHILD | WS_VISIBLE,
        margin_x, cur_y + 2, lbl_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblSize, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    std::string sz_str = (state.result.info.file_size > 0) ? format_size(state.result.info.file_size) : "Checking file size...";
    std::wstring wsz(sz_str.begin(), sz_str.end());
    state.hStaticSize = CreateWindowExW(0, L"STATIC", wsz.c_str(),
        WS_CHILD | WS_VISIBLE,
        input_x, cur_y + 2, input_w, row_h,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hStaticSize, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    cur_y += DpiHelper::scale(28, dpi);

    // Description row
    HWND hLblDesc = CreateWindowExW(0, L"STATIC", L"Description:", WS_CHILD | WS_VISIBLE,
        margin_x, cur_y + 2, lbl_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblDesc, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    state.hEditDesc = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        input_x, cur_y, input_w, row_h,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hEditDesc, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    cur_y += DpiHelper::scale(44, dpi);

    // Action buttons
    int btn_w = DpiHelper::scale(120, dpi);
    int btn_h = DpiHelper::scale(28, dpi);
    int cur_btn_x = margin_x;

    if (is_zip) {
        state.hBtnZip = CreateWindowExW(0, L"BUTTON", L"ZIP Preview...",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            cur_btn_x, cur_y, DpiHelper::scale(105, dpi), btn_h,
            hwnd, (HMENU)206, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(state.hBtnZip, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    }

    int right_btns_w = btn_w * 3 + DpiHelper::scale(16, dpi);
    int right_x = dlg_w - margin_x - right_btns_w - DpiHelper::scale(16, dpi);

    state.hBtnStart = CreateWindowExW(0, L"BUTTON", L"Start Download",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        right_x, cur_y, btn_w, btn_h,
        hwnd, (HMENU)203, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnStart, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    state.hBtnLater = CreateWindowExW(0, L"BUTTON", L"Download Later",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        right_x + btn_w + DpiHelper::scale(8, dpi), cur_y, btn_w, btn_h,
        hwnd, (HMENU)204, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnLater, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    state.hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        right_x + (btn_w + DpiHelper::scale(8, dpi)) * 2, cur_y, btn_w - DpiHelper::scale(20, dpi), btn_h,
        hwnd, (HMENU)205, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnCancel, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    SetFocus(state.hBtnStart);

    // Background resource probe if size is 0
    if (state.result.info.file_size == 0) {
        std::thread([hwnd, url = state.result.info.url, ua = state.result.info.user_agent]() {
            CURL* curl = curl_easy_init();
            if (!curl) return;

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
            std::string user_agent = ua.empty() ? "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36" : ua;
            curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());

            curl_off_t cl = 0;
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
            }
            curl_easy_cleanup(curl);
            if (cl > 0 && IsWindow(hwnd)) {
                PostMessageW(hwnd, WM_PROBE_COMPLETE, (WPARAM)static_cast<uint32_t>(cl), (LPARAM)(static_cast<uint64_t>(cl) >> 32));
            }
        }).detach();
    }

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

bool Win32DownloadInfoDialog::save_snapshot(HWND parent, const DownloadPromptInfo& initial_info, const std::string& filename) {
    InfoDialogState state;
    state.result.info = initial_info;

    if (state.result.info.filename.empty()) {
        state.result.info.filename = "download.bin";
    }
    if (state.result.info.category.empty()) {
        state.result.info.category = QueueManager::detect_category(state.result.info.filename);
    }
    if (state.result.info.destination_dir.empty()) {
        std::string root = get_default_downloads_dir();
        fs::path dp = (state.result.info.category == "General") ? fs::path(utf8_to_wide(root)) : (fs::path(utf8_to_wide(root)) / utf8_to_wide(state.result.info.category));
        state.result.info.destination_dir = wide_to_utf8(dp.wstring());
    }
    if (state.result.info.save_as_path.empty()) {
        fs::path sp = fs::path(utf8_to_wide(state.result.info.destination_dir)) / utf8_to_wide(state.result.info.filename);
        state.result.info.save_as_path = wide_to_utf8(sp.wstring());
    }

    std::string ext = "";
    size_t dot = state.result.info.filename.rfind('.');
    if (dot != std::string::npos) {
        ext = state.result.info.filename.substr(dot + 1);
        for (char& c : ext) c = static_cast<char>(std::tolower(c));
    }
    bool is_zip = (ext == "zip" || state.result.info.is_archive);

    UINT dpi = DpiHelper::get_dpi(parent);
    state.hFont = DpiHelper::create_font(9, FW_NORMAL, false, dpi);
    state.hFontBold = DpiHelper::create_font(9, FW_SEMIBOLD, false, dpi);

    static bool s_registered = false;
    const wchar_t* className = L"OpenDM_DownloadInfoDialog";
    if (!s_registered) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = DownloadInfoWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        s_registered = true;
    }

    int dlg_w = DpiHelper::scale(580, dpi);
    int dlg_h = DpiHelper::scale(330, dpi);
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        className, L"Download File Info",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        100, 100, dlg_w, dlg_h,
        parent, nullptr, GetModuleHandleW(nullptr), &state
    );

    int margin_x = DpiHelper::scale(18, dpi);
    int lbl_w = DpiHelper::scale(90, dpi);
    int input_x = margin_x + lbl_w + DpiHelper::scale(8, dpi);
    int input_w = dlg_w - input_x - margin_x - DpiHelper::scale(16, dpi);
    int row_h = DpiHelper::scale(24, dpi);
    int cur_y = DpiHelper::scale(16, dpi);

    HWND hLblUrl = CreateWindowExW(0, L"STATIC", L"URL:", WS_CHILD | WS_VISIBLE,
        margin_x, cur_y + 2, lbl_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblUrl, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    state.hEditUrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
        input_x, cur_y, input_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hEditUrl, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    std::wstring wurl = utf8_to_wide(state.result.info.url);
    SetWindowTextW(state.hEditUrl, wurl.c_str());

    cur_y += DpiHelper::scale(32, dpi);

    HWND hLblCat = CreateWindowExW(0, L"STATIC", L"Category:", WS_CHILD | WS_VISIBLE,
        margin_x, cur_y + 2, lbl_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblCat, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    state.hComboCat = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        input_x, cur_y, DpiHelper::scale(180, dpi), DpiHelper::scale(150, dpi),
        hwnd, (HMENU)201, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hComboCat, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    int cat_sel = 0;
    for (int i = 0; kCategories[i]; ++i) {
        std::wstring wc(kCategories[i], kCategories[i] + strlen(kCategories[i]));
        SendMessageW(state.hComboCat, CB_ADDSTRING, 0, (LPARAM)wc.c_str());
        if (state.result.info.category == kCategories[i]) cat_sel = i;
    }
    SendMessageW(state.hComboCat, CB_SETCURSEL, cat_sel, 0);

    cur_y += DpiHelper::scale(32, dpi);

    HWND hLblSave = CreateWindowExW(0, L"STATIC", L"Save As:", WS_CHILD | WS_VISIBLE,
        margin_x, cur_y + 2, lbl_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblSave, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    int browse_btn_w = DpiHelper::scale(36, dpi);
    state.hEditSaveAs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        input_x, cur_y, input_w - browse_btn_w - DpiHelper::scale(6, dpi), row_h,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hEditSaveAs, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    std::wstring wsa = utf8_to_wide(state.result.info.save_as_path);
    SetWindowTextW(state.hEditSaveAs, wsa.c_str());

    state.hBtnBrowse = CreateWindowExW(0, L"BUTTON", L"...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        input_x + input_w - browse_btn_w, cur_y, browse_btn_w, row_h,
        hwnd, (HMENU)202, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnBrowse, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    cur_y += DpiHelper::scale(32, dpi);

    HWND hLblSize = CreateWindowExW(0, L"STATIC", L"File Size:", WS_CHILD | WS_VISIBLE,
        margin_x, cur_y + 2, lbl_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblSize, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    std::string sz_str = (state.result.info.file_size > 0) ? format_size(state.result.info.file_size) : "37.75 KB (38656 bytes)";
    std::wstring wsz(sz_str.begin(), sz_str.end());
    state.hStaticSize = CreateWindowExW(0, L"STATIC", wsz.c_str(),
        WS_CHILD | WS_VISIBLE,
        input_x, cur_y + 2, input_w, row_h,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hStaticSize, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    cur_y += DpiHelper::scale(28, dpi);

    HWND hLblDesc = CreateWindowExW(0, L"STATIC", L"Description:", WS_CHILD | WS_VISIBLE,
        margin_x, cur_y + 2, lbl_w, row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblDesc, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    state.hEditDesc = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        input_x, cur_y, input_w, row_h,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hEditDesc, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    cur_y += DpiHelper::scale(44, dpi);

    int btn_w = DpiHelper::scale(120, dpi);
    int btn_h = DpiHelper::scale(28, dpi);
    int cur_btn_x = margin_x;

    if (is_zip) {
        state.hBtnZip = CreateWindowExW(0, L"BUTTON", L"ZIP Preview...",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            cur_btn_x, cur_y, DpiHelper::scale(105, dpi), btn_h,
            hwnd, (HMENU)206, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(state.hBtnZip, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    }

    int right_btns_w = btn_w * 3 + DpiHelper::scale(16, dpi);
    int right_x = dlg_w - margin_x - right_btns_w - DpiHelper::scale(16, dpi);

    state.hBtnStart = CreateWindowExW(0, L"BUTTON", L"Start Download",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        right_x, cur_y, btn_w, btn_h,
        hwnd, (HMENU)203, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnStart, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    state.hBtnLater = CreateWindowExW(0, L"BUTTON", L"Download Later",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        right_x + btn_w + DpiHelper::scale(8, dpi), cur_y, btn_w, btn_h,
        hwnd, (HMENU)204, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnLater, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    state.hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        right_x + (btn_w + DpiHelper::scale(8, dpi)) * 2, cur_y, btn_w - DpiHelper::scale(20, dpi), btn_h,
        hwnd, (HMENU)205, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnCancel, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    UpdateWindow(hwnd);
    MSG pmsg;
    for (int i = 0; i < 30 && PeekMessageW(&pmsg, nullptr, 0, 0, PM_REMOVE); ++i) {
        TranslateMessage(&pmsg);
        DispatchMessageW(&pmsg);
    }

    RECT rc;
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    HDC hdcWin = GetWindowDC(hwnd);
    HDC hdcMem = CreateCompatibleDC(hdcWin);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcWin, w, h);
    HGDIOBJ hOld = SelectObject(hdcMem, hBitmap);

    PrintWindow(hwnd, hdcMem, 2);

    Win32Assets::init_gdiplus();
    Gdiplus::Bitmap gdiBmp(hBitmap, nullptr);

    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    CLSID pngClsid{};
    if (size > 0) {
        std::vector<BYTE> memory(size);
        Gdiplus::ImageCodecInfo* pImageCodecInfo = (Gdiplus::ImageCodecInfo*)memory.data();
        Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);
        for (UINT j = 0; j < num; ++j) {
            if (wcscmp(pImageCodecInfo[j].MimeType, L"image/png") == 0) {
                pngClsid = pImageCodecInfo[j].Clsid;
                break;
            }
        }
    }

    std::wstring wpath(filename.begin(), filename.end());
    Gdiplus::Status st = gdiBmp.Save(wpath.c_str(), &pngClsid, nullptr);

    SelectObject(hdcMem, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcWin);
    DestroyWindow(hwnd);
    if (state.hFont) DeleteObject(state.hFont);
    if (state.hFontBold) DeleteObject(state.hFontBold);

    return (st == Gdiplus::Ok);
}

} // namespace win32
} // namespace opendm
