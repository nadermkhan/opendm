#include "win32_grabber_dialog.hpp"
#include "dpi_helper.hpp"
#include "../core/site_grabber.hpp"
#include "../core/download_queue.hpp"
#include <commctrl.h>
#include <shlobj.h>
#include <thread>
#include <atomic>
#include <vector>

namespace opendm {
namespace win32 {

namespace {

#define WM_GRABBER_FILE (WM_USER + 301)
#define WM_GRABBER_STATUS (WM_USER + 302)
#define WM_GRABBER_DONE (WM_USER + 303)

struct GrabberDialogState {
    HWND hwnd{nullptr};
    HWND hEditUrl{nullptr};
    HWND hEditDepth{nullptr};
    HWND hChkVideo{nullptr};
    HWND hChkAudio{nullptr};
    HWND hChkComp{nullptr};
    HWND hChkDocs{nullptr};
    HWND hListView{nullptr};
    HWND hStaticStatus{nullptr};
    HWND hBtnStart{nullptr};
    HWND hBtnStop{nullptr};
    HWND hBtnDownloadAll{nullptr};
    HWND hBtnClose{nullptr};

    HFONT hFont{nullptr};
    HFONT hFontBold{nullptr};

    SiteGrabber grabber;
    std::vector<DiscoveredFile> discovered;
    std::atomic<bool> crawling{false};
    std::atomic<bool> destroyed{false};
    std::thread crawler_thread;
    bool done{false};
};

LRESULT CALLBACK GrabberWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    GrabberDialogState* state = reinterpret_cast<GrabberDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<GrabberDialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            state->hwnd = hwnd;
            return 0;
        }
        case WM_GRABBER_FILE: {
            DiscoveredFile* pdf = reinterpret_cast<DiscoveredFile*>(lParam);
            if (pdf) {
                if (state && !state->destroyed) {
                    state->discovered.push_back(*pdf);
                    int idx = static_cast<int>(state->discovered.size() - 1);
                    std::wstring wfn(pdf->filename.begin(), pdf->filename.end());
                    std::wstring wcat(pdf->category.begin(), pdf->category.end());
                    std::wstring wurl(pdf->url.begin(), pdf->url.end());

                    LVITEMW lvi = {LVIF_TEXT};
                    lvi.iItem = idx;
                    lvi.pszText = (LPWSTR)wfn.c_str();
                    ListView_InsertItem(state->hListView, &lvi);
                    ListView_SetItemText(state->hListView, idx, 1, (LPWSTR)wcat.c_str());
                    ListView_SetItemText(state->hListView, idx, 2, (LPWSTR)wurl.c_str());
                }
                delete pdf;
            }
            return 0;
        }
        case WM_GRABBER_STATUS: {
            if (state && !state->destroyed) {
                std::wstring* pmsg = reinterpret_cast<std::wstring*>(lParam);
                if (pmsg) {
                    SetWindowTextW(state->hStaticStatus, pmsg->c_str());
                    delete pmsg;
                }
            }
            return 0;
        }
        case WM_GRABBER_DONE: {
            if (state && !state->destroyed) {
                state->crawling = false;
                EnableWindow(state->hBtnStart, TRUE);
                EnableWindow(state->hBtnStop, FALSE);
                EnableWindow(state->hBtnDownloadAll, !state->discovered.empty());
                std::wstring st = L"Finished. Discovered " + std::to_wstring(state->discovered.size()) + L" files.";
                SetWindowTextW(state->hStaticStatus, st.c_str());
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 901) { // Start Crawl
                wchar_t buf[2048] = {0};
                GetWindowTextW(state->hEditUrl, buf, 2048);
                std::wstring wu(buf);
                if (wu.empty()) return 0;
                std::string start_url(wu.begin(), wu.end());

                wchar_t d_buf[16] = {0};
                GetWindowTextW(state->hEditDepth, d_buf, 16);
                int depth = _wtoi(d_buf);
                if (depth < 1) depth = 1;
                if (depth > 5) depth = 5;

                GrabberConfig cfg;
                cfg.seed_url = start_url;
                cfg.max_depth = depth;

                if (SendMessageW(state->hChkVideo, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    cfg.file_extensions.insert(cfg.file_extensions.end(), {"mp4", "mkv", "avi", "webm", "flv", "mov"});
                }
                if (SendMessageW(state->hChkAudio, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    cfg.file_extensions.insert(cfg.file_extensions.end(), {"mp3", "wav", "flac", "aac", "m4a", "ogg"});
                }
                if (SendMessageW(state->hChkComp, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    cfg.file_extensions.insert(cfg.file_extensions.end(), {"zip", "rar", "7z", "tar", "gz", "bz2"});
                }
                if (SendMessageW(state->hChkDocs, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    cfg.file_extensions.insert(cfg.file_extensions.end(), {"pdf", "doc", "docx", "xls", "xlsx", "epub"});
                }

                state->discovered.clear();
                ListView_DeleteAllItems(state->hListView);
                state->crawling = true;
                EnableWindow(state->hBtnStart, FALSE);
                EnableWindow(state->hBtnStop, TRUE);
                EnableWindow(state->hBtnDownloadAll, FALSE);
                SetWindowTextW(state->hStaticStatus, L"Connecting and crawling...");

                if (state->crawler_thread.joinable()) {
                    state->grabber.stop();
                    state->crawler_thread.join();
                }

                state->crawler_thread = std::thread([hwnd, pstate = state, cfg]() {
                    pstate->grabber.start(
                        cfg,
                        [hwnd, pstate](const DiscoveredFile& df) {
                            if (pstate->destroyed) return;
                            auto* pdf = new DiscoveredFile(df);
                            if (IsWindow(hwnd)) {
                                if (!PostMessageW(hwnd, WM_GRABBER_FILE, 0, reinterpret_cast<LPARAM>(pdf))) {
                                    delete pdf;
                                }
                            } else {
                                delete pdf;
                            }
                        },
                        [hwnd, pstate](const std::string& st, size_t, size_t) {
                            if (pstate->destroyed) return;
                            auto* wst = new std::wstring(st.begin(), st.end());
                            if (IsWindow(hwnd)) {
                                if (!PostMessageW(hwnd, WM_GRABBER_STATUS, 0, reinterpret_cast<LPARAM>(wst))) {
                                    delete wst;
                                }
                            } else {
                                delete wst;
                            }
                        }
                    );
                    if (IsWindow(hwnd)) {
                        PostMessageW(hwnd, WM_GRABBER_DONE, 0, 0);
                    }
                });
                return 0;
            }
            if (id == 902) { // Stop Crawl
                state->grabber.stop();
                return 0;
            }
            if (id == 903) { // Download All
                for (const auto& df : state->discovered) {
                    static uint64_t cnt = 1;
                    std::string id_str = "grabber_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(cnt++);
                    std::string fn = df.filename.empty() ? "grabbed_file.bin" : df.filename;
                    auto sess = std::make_shared<DownloadSession>(id_str, df.url, fn);
                    QueueManager::instance().get_main_queue().add_session(sess, true);
                }
                MessageBoxW(hwnd, L"Added all discovered files to download queue!", L"Site Grabber", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (id == IDCANCEL || id == 904) {
                if (state->crawling) state->grabber.stop();
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE: {
            if (state->crawling) state->grabber.stop();
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY: {
            state->destroyed = true;
            state->grabber.stop();
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void Win32GrabberDialog::show(HWND parent) {
    GrabberDialogState state;
    UINT dpi = DpiHelper::get_dpi(parent);

    state.hFont = DpiHelper::create_font(9, FW_NORMAL, false, dpi);
    state.hFontBold = DpiHelper::create_font(9, FW_SEMIBOLD, false, dpi);

    static bool s_registered = false;
    const wchar_t* className = L"OpenDM_GrabberDialog";
    if (!s_registered) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = GrabberWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        s_registered = true;
    }

    int dlg_w = DpiHelper::scale(650, dpi);
    int dlg_h = DpiHelper::scale(480, dpi);

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
        className, L"OpenDM Site Grabber Spider",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlg_w, dlg_h,
        parent, nullptr, GetModuleHandleW(nullptr), &state
    );

    int margin_x = DpiHelper::scale(16, dpi);
    int cur_y = DpiHelper::scale(14, dpi);
    int content_w = dlg_w - margin_x * 2 - DpiHelper::scale(16, dpi);

    // URL row
    HWND hLblUrl = CreateWindowExW(0, L"STATIC", L"Start Address:", WS_CHILD | WS_VISIBLE,
        margin_x, cur_y + 2, DpiHelper::scale(90, dpi), DpiHelper::scale(20, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblUrl, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    state.hEditUrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"https://",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        margin_x + DpiHelper::scale(95, dpi), cur_y, content_w - DpiHelper::scale(95, dpi), DpiHelper::scale(24, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hEditUrl, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    cur_y += DpiHelper::scale(30, dpi);

    // Depth & Filters row
    HWND hLblDepth = CreateWindowExW(0, L"STATIC", L"Explore Depth (1-5):", WS_CHILD | WS_VISIBLE,
        margin_x, cur_y + 2, DpiHelper::scale(130, dpi), DpiHelper::scale(20, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblDepth, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    state.hEditDepth = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"2",
        WS_CHILD | WS_VISIBLE | ES_NUMBER,
        margin_x + DpiHelper::scale(135, dpi), cur_y, DpiHelper::scale(40, dpi), DpiHelper::scale(22, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hEditDepth, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    // Filter checkboxes
    int chk_x = margin_x + DpiHelper::scale(200, dpi);
    state.hChkVideo = CreateWindowExW(0, L"BUTTON", L"Video", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        chk_x, cur_y, DpiHelper::scale(60, dpi), DpiHelper::scale(22, dpi), hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hChkVideo, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    SendMessageW(state.hChkVideo, BM_SETCHECK, BST_CHECKED, 0);

    state.hChkAudio = CreateWindowExW(0, L"BUTTON", L"Audio", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        chk_x + DpiHelper::scale(65, dpi), cur_y, DpiHelper::scale(60, dpi), DpiHelper::scale(22, dpi), hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hChkAudio, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    SendMessageW(state.hChkAudio, BM_SETCHECK, BST_CHECKED, 0);

    state.hChkComp = CreateWindowExW(0, L"BUTTON", L"Archives", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        chk_x + DpiHelper::scale(130, dpi), cur_y, DpiHelper::scale(75, dpi), DpiHelper::scale(22, dpi), hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hChkComp, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    SendMessageW(state.hChkComp, BM_SETCHECK, BST_CHECKED, 0);

    state.hChkDocs = CreateWindowExW(0, L"BUTTON", L"Documents", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        chk_x + DpiHelper::scale(210, dpi), cur_y, DpiHelper::scale(85, dpi), DpiHelper::scale(22, dpi), hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hChkDocs, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    SendMessageW(state.hChkDocs, BM_SETCHECK, BST_CHECKED, 0);

    cur_y += DpiHelper::scale(30, dpi);

    // Action buttons: Start, Stop
    int btn_w = DpiHelper::scale(100, dpi);
    int btn_h = DpiHelper::scale(26, dpi);
    state.hBtnStart = CreateWindowExW(0, L"BUTTON", L"Start Crawl", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        margin_x, cur_y, btn_w, btn_h, hwnd, (HMENU)901, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnStart, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    state.hBtnStop = CreateWindowExW(0, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
        margin_x + btn_w + DpiHelper::scale(8, dpi), cur_y, btn_w - DpiHelper::scale(20, dpi), btn_h, hwnd, (HMENU)902, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnStop, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    state.hBtnDownloadAll = CreateWindowExW(0, L"BUTTON", L"Download All Discovered...", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
        margin_x + btn_w * 2, cur_y, DpiHelper::scale(180, dpi), btn_h, hwnd, (HMENU)903, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnDownloadAll, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    cur_y += btn_h + DpiHelper::scale(12, dpi);

    // Results ListView
    int list_h = DpiHelper::scale(240, dpi);
    state.hListView = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_TABSTOP,
        margin_x, cur_y, content_w, list_h,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(state.hListView, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    ListView_SetExtendedListViewStyle(state.hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW lvc = {LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM};
    lvc.cx = DpiHelper::scale(200, dpi);
    lvc.pszText = (LPWSTR)L"Discovered File";
    ListView_InsertColumn(state.hListView, 0, &lvc);

    lvc.cx = DpiHelper::scale(100, dpi);
    lvc.pszText = (LPWSTR)L"Category";
    ListView_InsertColumn(state.hListView, 1, &lvc);

    lvc.cx = DpiHelper::scale(300, dpi);
    lvc.pszText = (LPWSTR)L"Full URL";
    ListView_InsertColumn(state.hListView, 2, &lvc);

    cur_y += list_h + DpiHelper::scale(10, dpi);

    // Status label
    state.hStaticStatus = CreateWindowExW(0, L"STATIC", L"Ready to crawl website for media and documents.",
        WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
        margin_x, cur_y, content_w - DpiHelper::scale(100, dpi), DpiHelper::scale(20, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hStaticStatus, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    int close_w = DpiHelper::scale(85, dpi);
    state.hBtnClose = CreateWindowExW(0, L"BUTTON", L"Close",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        dlg_w - margin_x - close_w - DpiHelper::scale(16, dpi), cur_y, close_w, btn_h,
        hwnd, (HMENU)904, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnClose, WM_SETFONT, (WPARAM)state.hFont, TRUE);

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

    state.grabber.stop();
    if (state.crawler_thread.joinable()) {
        state.crawler_thread.join();
    }

    if (state.hFont) DeleteObject(state.hFont);
    if (state.hFontBold) DeleteObject(state.hFontBold);
}

} // namespace win32
} // namespace opendm
