#include "win32_main_window.hpp"
#include "win32_add_url_dialog.hpp"
#include "win32_download_info_dialog.hpp"
#include "win32_download_complete_dialog.hpp"
#include "win32_options_dialog.hpp"
#include "win32_about_dialog.hpp"
#include "win32_properties_dialog.hpp"
#include "win32_zip_preview_dialog.hpp"
#include "win32_youtube_format_dialog.hpp"
#include "win32_grabber_dialog.hpp"
#include "dpi_helper.hpp"
#include "win32_assets.hpp"
#include "resource.h"
#include "../core/batch_importer.hpp"
#include "../core/site_logon.hpp"
#include "../core/youtube_engine.hpp"

#include <gdiplus.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace fs = std::filesystem;

namespace opendm {
namespace win32 {

namespace {

std::string format_bytes(int64_t bytes) {
    if (bytes < 0) return "Unknown";
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

std::string format_speed(uint64_t bytes_per_sec) {
    if (bytes_per_sec == 0) return "0.00 KB/s";
    return format_bytes(bytes_per_sec) + "/s";
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

std::string get_url_domain(const std::string& url) {
    std::string s = url;
    size_t pos = s.find("://");
    if (pos != std::string::npos) s = s.substr(pos + 3);
    size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    size_t colon = s.find(':');
    if (colon != std::string::npos) s = s.substr(0, colon);
    return s;
}

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    std::vector<BYTE> memory(size);
    Gdiplus::ImageCodecInfo* pImageCodecInfo = (Gdiplus::ImageCodecInfo*)memory.data();
    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);

    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            return j;
        }
    }
    return -1;
}

// Posted by worker threads (IPC server) to show a download prompt on the UI thread.
// lParam owns a heap-allocated DownloadPromptInfo.
constexpr UINT WM_APP_PROMPT_DOWNLOAD = WM_APP + 41;

// Correct UTF-8 -> UTF-16 conversion (byte-wise widening garbles non-ASCII names)
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) {
        // Not valid UTF-8: fall back to the ANSI code page
        len = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (len <= 0) return std::wstring();
        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), &out[0], len);
        return out;
    }
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len);
    return out;
}

} // namespace

Win32MainWindow::Win32MainWindow(HINSTANCE hInstance) : hInstance_(hInstance) {
    dpi_ = DpiHelper::get_dpi(nullptr);
    sidebar_width_ = DpiHelper::scale(220, dpi_);
    hFont_ = DpiHelper::create_font(9, FW_NORMAL, false, dpi_);
    hFontBold_ = DpiHelper::create_font(9, FW_SEMIBOLD, false, dpi_);
    setup_ui();
}

Win32MainWindow::~Win32MainWindow() {
    if (timer_id_) {
        KillTimer(hwnd_, timer_id_);
        timer_id_ = 0;
    }
    if (hFont_) DeleteObject(hFont_);
    if (hFontBold_) DeleteObject(hFontBold_);
    if (himlToolbar_) {
        ImageList_Destroy(himlToolbar_);
        himlToolbar_ = nullptr;
    }
    if (himlToolbarDisabled_) {
        ImageList_Destroy(himlToolbarDisabled_);
        himlToolbarDisabled_ = nullptr;
    }
    if (himlTree_) ImageList_Destroy(himlTree_);
    if (himlList_) ImageList_Destroy(himlList_);
}

void Win32MainWindow::present() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);
        SetForegroundWindow(hwnd_);
    }
}

void Win32MainWindow::setup_ui() {
    const wchar_t* className = L"OpenDM_MainWindow";

    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance_ ? hInstance_ : GetModuleHandleW(nullptr);
    wc.hIcon = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = className;
    RegisterClassExW(&wc);

    int win_w = DpiHelper::scale(1200, dpi_);
    int win_h = DpiHelper::scale(680, dpi_);

    RECT rcDesk;
    GetWindowRect(GetDesktopWindow(), &rcDesk);
    int x = (rcDesk.right - win_w) / 2;
    int y = (rcDesk.bottom - win_h) / 2;

    hwnd_ = CreateWindowExW(
        WS_EX_WINDOWEDGE,
        className,
        L"OpenDM - High-Speed Download Accelerator",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, win_w, win_h,
        nullptr, nullptr, wc.hInstance, this
    );

    setup_menubar();
    setup_toolbar();
    setup_sidebar();
    setup_downloads_view();
    setup_statusbar();

    update_layout();
    refresh_downloads_list();

    timer_id_ = SetTimer(hwnd_, 10, 400, nullptr);
}

void Win32MainWindow::setup_menubar() {
    HMENU hMenu = CreateMenu();

    // 1. Tasks Menu
    HMENU hTasks = CreatePopupMenu();
    AppendMenuW(hTasks, MF_STRING, IDM_TASKS_ADD_URL, L"Add new download (Ctrl+N)");
    AppendMenuW(hTasks, MF_STRING, IDM_TASKS_BATCH_IMPORT, L"Add batch download (.ef2)...");
    AppendMenuW(hTasks, MF_STRING, IDM_TASKS_BATCH_EXPORT, L"Export batch download (.ef2)...");
    AppendMenuW(hTasks, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hTasks, MF_STRING, IDM_TASKS_SITE_GRABBER, L"Run site grabber...");
    AppendMenuW(hTasks, MF_STRING, IDM_TASKS_ZIP_PREVIEW, L"Preview remote ZIP archive...");
    AppendMenuW(hTasks, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hTasks, MF_STRING, IDM_TASKS_OPTIONS, L"Options...");
    AppendMenuW(hTasks, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hTasks, MF_STRING, IDM_TASKS_EXIT, L"Exit");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hTasks, L"Tasks");

    // 2. File Menu
    HMENU hFile = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, IDM_FILE_RESUME, L"Resume Download");
    AppendMenuW(hFile, MF_STRING, IDM_FILE_STOP, L"Stop Download");
    AppendMenuW(hFile, MF_STRING, IDM_FILE_DELETE, L"Remove Selected\tDel");
    AppendMenuW(hFile, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hFile, MF_STRING, IDM_FILE_OPEN, L"Open");
    AppendMenuW(hFile, MF_STRING, IDM_FILE_OPEN_FOLDER, L"Open Folder");
    AppendMenuW(hFile, MF_STRING, IDM_FILE_COPY_URL, L"Copy URL Address");
    AppendMenuW(hFile, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hFile, MF_STRING, IDM_FILE_PROPERTIES, L"Properties...");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFile, L"File");

    // 3. Downloads Menu
    HMENU hDownloads = CreatePopupMenu();
    AppendMenuW(hDownloads, MF_STRING, IDM_DOWNLOADS_STOP_ALL, L"Pause All");
    AppendMenuW(hDownloads, MF_STRING, IDM_DOWNLOADS_DELETE_COMPLETED, L"Delete All Completed");
    AppendMenuW(hDownloads, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hDownloads, MF_STRING, IDM_DOWNLOADS_SCHEDULER, L"Scheduler...");
    AppendMenuW(hDownloads, MF_STRING, IDM_DOWNLOADS_START_QUEUE, L"Start Queue");
    AppendMenuW(hDownloads, MF_STRING, IDM_DOWNLOADS_STOP_QUEUE, L"Stop Queue");
    AppendMenuW(hDownloads, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hDownloads, MF_STRING, IDM_DOWNLOADS_SPEED_LIMITER, L"Speed Limiter (Toggle)");
    AppendMenuW(hDownloads, MF_STRING, IDM_TASKS_OPTIONS, L"Options...");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hDownloads, L"Downloads");

    // 4. View Menu
    HMENU hView = CreatePopupMenu();
    AppendMenuW(hView, MF_STRING | MF_CHECKED, IDM_VIEW_SIDEBAR, L"Categories Sidebar");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hView, L"View");

    // 5. Help Menu
    HMENU hHelp = CreatePopupMenu();
    AppendMenuW(hHelp, MF_STRING, IDM_HELP_ABOUT, L"About OpenDM");
    AppendMenuW(hHelp, MF_STRING, IDM_HELP_GITHUB, L"GitHub Repository...");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hHelp, L"Help");

    SetMenu(hwnd_, hMenu);
}

void Win32MainWindow::setup_toolbar() {
    int icon_size = DpiHelper::scale(32, dpi_);
    int btn_w = DpiHelper::scale(68, dpi_);
    int btn_h = DpiHelper::scale(58, dpi_);
    int pad_x = DpiHelper::scale(4, dpi_);
    int pad_y = DpiHelper::scale(4, dpi_);
    int sep_w = DpiHelper::scale(10, dpi_);

    std::vector<std::string> tb_icon_files = {
        "toolbar_add_url.png",          // 0: IDM_TASKS_ADD_URL
        "toolbar_resume.png",           // 1: IDM_FILE_RESUME
        "toolbar_stop.png",             // 2: IDM_FILE_STOP
        "toolbar_stop_all.png",         // 3: IDM_DOWNLOADS_STOP_ALL
        "toolbar_delete.png",           // 4: IDM_FILE_DELETE
        "toolbar_delete_completed.png", // 5: IDM_DOWNLOADS_DELETE_COMPLETED
        "toolbar_start_queue.png",      // 6: IDM_DOWNLOADS_START_QUEUE
        "toolbar_stop_queue.png",       // 7: IDM_DOWNLOADS_STOP_QUEUE
        "toolbar_scheduler.png",        // 8: IDM_DOWNLOADS_SCHEDULER
        "toolbar_options.png",          // 9: IDM_TASKS_OPTIONS
        "toolbar_grabber.png"           // 10: IDM_TASKS_SITE_GRABBER
    };

    if (himlToolbar_) {
        ImageList_Destroy(himlToolbar_);
        himlToolbar_ = nullptr;
    }
    if (himlToolbarDisabled_) {
        ImageList_Destroy(himlToolbarDisabled_);
        himlToolbarDisabled_ = nullptr;
    }

    himlToolbar_ = Win32Assets::create_image_list(tb_icon_files, icon_size);
    himlToolbarDisabled_ = Win32Assets::create_disabled_image_list(tb_icon_files, icon_size);

    if (!hToolbar_) {
        hToolbar_ = CreateWindowExW(
            0, TOOLBARCLASSNAMEW, nullptr,
            WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NODIVIDER | CCS_TOP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 0, 0,
            hwnd_, (HMENU)10, hInstance_, nullptr
        );
        SendMessageW(hToolbar_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DOUBLEBUFFER);
    } else {
        int count = (int)SendMessageW(hToolbar_, TB_BUTTONCOUNT, 0, 0);
        for (int i = count - 1; i >= 0; --i) {
            SendMessageW(hToolbar_, TB_DELETEBUTTON, i, 0);
        }
    }

    SendMessageW(hToolbar_, WM_SETFONT, (WPARAM)hFont_, TRUE);
    SendMessageW(hToolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(hToolbar_, TB_SETIMAGELIST, 0, (LPARAM)himlToolbar_);
    SendMessageW(hToolbar_, TB_SETDISABLEDIMAGELIST, 0, (LPARAM)himlToolbarDisabled_);
    SendMessageW(hToolbar_, TB_SETBITMAPSIZE, 0, MAKELPARAM(icon_size, icon_size));
    SendMessageW(hToolbar_, TB_SETPADDING, 0, MAKELPARAM(pad_x, pad_y));
    SendMessageW(hToolbar_, TB_SETBUTTONSIZE, 0, MAKELPARAM(btn_w, btn_h));
    SendMessageW(hToolbar_, TB_SETMAXTEXTROWS, 1, 0);

    // Toolbar Buttons matching authentic IDM with uniform width and height
    TBBUTTON buttons[] = {
        {0, IDM_TASKS_ADD_URL, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Add URL"},
        {1, IDM_FILE_RESUME, 0, BTNS_BUTTON | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Resume"},
        {2, IDM_FILE_STOP, 0, BTNS_BUTTON | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Pause"},
        {3, IDM_DOWNLOADS_STOP_ALL, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Stop All"},
        {sep_w, 0, TBSTATE_ENABLED, BTNS_SEP, {0}, 0, 0},
        {4, IDM_FILE_DELETE, 0, BTNS_BUTTON | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Delete"},
        {5, IDM_DOWNLOADS_DELETE_COMPLETED, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Clean"},
        {sep_w, 0, TBSTATE_ENABLED, BTNS_SEP, {0}, 0, 0},
        {6, IDM_DOWNLOADS_START_QUEUE, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Start Queue"},
        {7, IDM_DOWNLOADS_STOP_QUEUE, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Stop Queue"},
        {8, IDM_DOWNLOADS_SCHEDULER, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Scheduler"},
        {sep_w, 0, TBSTATE_ENABLED, BTNS_SEP, {0}, 0, 0},
        {9, IDM_TASKS_OPTIONS, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Options"},
        {10, IDM_TASKS_SITE_GRABBER, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Grabber"}
    };

    SendMessageW(hToolbar_, TB_ADDBUTTONSW, sizeof(buttons) / sizeof(buttons[0]), (LPARAM)buttons);
    SendMessageW(hToolbar_, TB_SETBUTTONSIZE, 0, MAKELPARAM(btn_w, btn_h));
    SendMessageW(hToolbar_, TB_AUTOSIZE, 0, 0);
}

void Win32MainWindow::setup_sidebar() {
    int icon_sz = DpiHelper::scale(16, dpi_);
    std::vector<std::string> tree_icons = {
        "cat_all_16.png",         // 0: All Downloads
        "cat_compressed_16.png",  // 1: Compressed
        "cat_documents_16.png",   // 2: Documents
        "cat_music_16.png",       // 3: Music
        "cat_programs_16.png",    // 4: Programs
        "cat_video_16.png",       // 5: Video
        "cat_queues_16.png"       // 6: Queues
    };
    himlTree_ = Win32Assets::create_image_list(tree_icons, icon_sz);

    hTreeView_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_TREEVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | WS_TABSTOP,
        0, 0, sidebar_width_, 200,
        hwnd_, (HMENU)20, hInstance_, nullptr
    );
    SendMessageW(hTreeView_, WM_SETFONT, (WPARAM)hFont_, TRUE);
    TreeView_SetImageList(hTreeView_, himlTree_, TVSIL_NORMAL);

    auto add_item = [this](HTREEITEM hParent, const wchar_t* text, int icon_idx) -> HTREEITEM {
        TVINSERTSTRUCTW tvis = {};
        tvis.hParent = hParent;
        tvis.hInsertAfter = TVI_LAST;
        tvis.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        tvis.item.pszText = (LPWSTR)text;
        tvis.item.iImage = icon_idx;
        tvis.item.iSelectedImage = icon_idx;
        return TreeView_InsertItem(hTreeView_, &tvis);
    };

    HTREEITEM hAll = add_item(TVI_ROOT, L"All Downloads", 0);
    tree_items_["All Downloads"] = hAll;

    tree_items_["Compressed"] = add_item(hAll, L"Compressed", 1);
    tree_items_["Documents"] = add_item(hAll, L"Documents", 2);
    tree_items_["Music"] = add_item(hAll, L"Music", 3);
    tree_items_["Programs"] = add_item(hAll, L"Programs", 4);
    tree_items_["Video"] = add_item(hAll, L"Video", 5);

    tree_items_["Unfinished"] = add_item(TVI_ROOT, L"Unfinished", 0);
    tree_items_["Finished"] = add_item(TVI_ROOT, L"Finished", 0);
    tree_items_["Grabber projects"] = add_item(TVI_ROOT, L"Grabber projects", 0);

    HTREEITEM hQueues = add_item(TVI_ROOT, L"Queues", 6);
    tree_items_["Queues"] = hQueues;
    tree_items_["Main download queue"] = add_item(hQueues, L"Main download queue", 6);

    TreeView_Expand(hTreeView_, hAll, TVE_EXPAND);
    TreeView_SelectItem(hTreeView_, hAll);
}

void Win32MainWindow::setup_downloads_view() {
    int icon_sz = DpiHelper::scale(16, dpi_);
    himlList_ = ImageList_Create(icon_sz, icon_sz, ILC_COLOR32 | ILC_MASK, 10, 10);

    hListView_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
        sidebar_width_, 0, 600, 400,
        hwnd_, (HMENU)30, hInstance_, nullptr
    );
    SendMessageW(hListView_, WM_SETFONT, (WPARAM)hFont_, TRUE);
    ListView_SetExtendedListViewStyle(hListView_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
    ListView_SetImageList(hListView_, himlList_, LVSIL_SMALL);

    // Columns matching authentic IDM:
    // File Name, Size, Status, Time Left, Speed, Date Added, Domain
    LVCOLUMNW lvc = {LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM};

    lvc.cx = DpiHelper::scale(240, dpi_);
    lvc.pszText = (LPWSTR)L"File Name";
    ListView_InsertColumn(hListView_, 0, &lvc);

    lvc.cx = DpiHelper::scale(95, dpi_);
    lvc.pszText = (LPWSTR)L"Size";
    ListView_InsertColumn(hListView_, 1, &lvc);

    lvc.cx = DpiHelper::scale(105, dpi_);
    lvc.pszText = (LPWSTR)L"Status";
    ListView_InsertColumn(hListView_, 2, &lvc);

    lvc.cx = DpiHelper::scale(90, dpi_);
    lvc.pszText = (LPWSTR)L"Time Left";
    ListView_InsertColumn(hListView_, 3, &lvc);

    lvc.cx = DpiHelper::scale(95, dpi_);
    lvc.pszText = (LPWSTR)L"Speed";
    ListView_InsertColumn(hListView_, 4, &lvc);

    lvc.cx = DpiHelper::scale(110, dpi_);
    lvc.pszText = (LPWSTR)L"Date Added";
    ListView_InsertColumn(hListView_, 5, &lvc);

    lvc.cx = DpiHelper::scale(130, dpi_);
    lvc.pszText = (LPWSTR)L"Domain";
    ListView_InsertColumn(hListView_, 6, &lvc);
}

void Win32MainWindow::setup_statusbar() {
    hStatusbar_ = CreateWindowExW(
        0, STATUSCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0,
        hwnd_, (HMENU)40, hInstance_, nullptr
    );
    SendMessageW(hStatusbar_, WM_SETFONT, (WPARAM)hFont_, TRUE);

    int parts[] = {
        DpiHelper::scale(250, dpi_),
        DpiHelper::scale(500, dpi_),
        -1
    };
    SendMessageW(hStatusbar_, SB_SETPARTS, 3, (LPARAM)parts);
    SendMessageW(hStatusbar_, SB_SETTEXTW, 0, (LPARAM)L"Ready");
    SendMessageW(hStatusbar_, SB_SETTEXTW, 1, (LPARAM)L"Total Speed: 0.00 KB/s");
    SendMessageW(hStatusbar_, SB_SETTEXTW, 2, (LPARAM)L"Speed Limiter: OFF");
}

void Win32MainWindow::update_layout() {
    if (!hwnd_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);

    RECT rcTb = {0};
    if (hToolbar_ && IsWindowVisible(hToolbar_)) {
        SendMessageW(hToolbar_, TB_AUTOSIZE, 0, 0);
        GetWindowRect(hToolbar_, &rcTb);
    }
    int tb_h = rcTb.bottom - rcTb.top;

    RECT rcSb = {0};
    if (hStatusbar_ && IsWindowVisible(hStatusbar_)) {
        SendMessageW(hStatusbar_, WM_SIZE, 0, 0);
        GetWindowRect(hStatusbar_, &rcSb);
    }
    int sb_h = rcSb.bottom - rcSb.top;

    int content_y = tb_h;
    int content_h = rc.bottom - tb_h - sb_h;
    if (content_h < 0) content_h = 0;

    int splitter_w = DpiHelper::scale(4, dpi_);

    if (sidebar_visible_) {
        ShowWindow(hTreeView_, SW_SHOW);
        SetWindowPos(hTreeView_, nullptr, 0, content_y, sidebar_width_, content_h, SWP_NOZORDER);

        int list_x = sidebar_width_ + splitter_w;
        int list_w = rc.right - list_x;
        if (list_w < 0) list_w = 0;
        SetWindowPos(hListView_, nullptr, list_x, content_y, list_w, content_h, SWP_NOZORDER);
    } else {
        ShowWindow(hTreeView_, SW_HIDE);
        SetWindowPos(hListView_, nullptr, 0, content_y, rc.right, content_h, SWP_NOZORDER);
    }

    // Update statusbar parts
    int parts[] = {
        DpiHelper::scale(250, dpi_),
        DpiHelper::scale(500, dpi_),
        rc.right
    };
    SendMessageW(hStatusbar_, SB_SETPARTS, 3, (LPARAM)parts);
}

void Win32MainWindow::apply_dpi_scaling(UINT new_dpi) {
    dpi_ = new_dpi;
    if (hFont_) DeleteObject(hFont_);
    if (hFontBold_) DeleteObject(hFontBold_);
    hFont_ = DpiHelper::create_font(9, FW_NORMAL, false, dpi_);
    hFontBold_ = DpiHelper::create_font(9, FW_SEMIBOLD, false, dpi_);

    SendMessageW(hTreeView_, WM_SETFONT, (WPARAM)hFont_, TRUE);
    SendMessageW(hListView_, WM_SETFONT, (WPARAM)hFont_, TRUE);
    SendMessageW(hStatusbar_, WM_SETFONT, (WPARAM)hFont_, TRUE);

    setup_toolbar();

    sidebar_width_ = DpiHelper::scale(220, dpi_);

    // Rescale list columns
    ListView_SetColumnWidth(hListView_, 0, DpiHelper::scale(240, dpi_));
    ListView_SetColumnWidth(hListView_, 1, DpiHelper::scale(95, dpi_));
    ListView_SetColumnWidth(hListView_, 2, DpiHelper::scale(105, dpi_));
    ListView_SetColumnWidth(hListView_, 3, DpiHelper::scale(90, dpi_));
    ListView_SetColumnWidth(hListView_, 4, DpiHelper::scale(95, dpi_));
    ListView_SetColumnWidth(hListView_, 5, DpiHelper::scale(110, dpi_));
    ListView_SetColumnWidth(hListView_, 6, DpiHelper::scale(130, dpi_));

    update_layout();
    refresh_downloads_list();
}

void Win32MainWindow::refresh_downloads_list() {
    // ListView notifications and modal loops can re-enter this function; a nested
    // refresh would mutate visible_session_ids_ while the outer one iterates it.
    if (refreshing_list_ || !hListView_) return;
    struct RefreshGuard {
        bool& flag;
        explicit RefreshGuard(bool& f) : flag(f) { flag = true; }
        ~RefreshGuard() { flag = false; }
    } guard(refreshing_list_);

    auto all_sessions = QueueManager::instance().get_main_queue().get_all_sessions();

    std::vector<std::shared_ptr<DownloadSession>> visible_sessions;
    for (const auto& s : all_sessions) {
        if (!s) continue;
        if (current_category_ == "All Downloads" || current_category_ == "Queues" || current_category_ == "Main download queue") {
            visible_sessions.push_back(s);
        } else if (current_category_ == "Unfinished") {
            if (s->get_status() != SessionStatus::COMPLETED) visible_sessions.push_back(s);
        } else if (current_category_ == "Finished") {
            if (s->get_status() == SessionStatus::COMPLETED) visible_sessions.push_back(s);
        } else if (s->get_category() == current_category_) {
            visible_sessions.push_back(s);
        }
    }

    visible_session_ids_.clear();
    for (const auto& s : visible_sessions) {
        visible_session_ids_.push_back(s->get_id());
    }

    // Prune closed dialogs in active_progress_dialogs_
    for (auto it = active_progress_dialogs_.begin(); it != active_progress_dialogs_.end(); ) {
        if (!it->second || !IsWindow(it->second->get_hwnd())) {
            it = active_progress_dialogs_.erase(it);
        } else {
            ++it;
        }
    }

    int cur_count = ListView_GetItemCount(hListView_);
    while (cur_count > static_cast<int>(visible_sessions.size())) {
        ListView_DeleteItem(hListView_, cur_count - 1);
        cur_count--;
    }
    // Keep the render cache in sync with the actual ListView rows
    row_cache_.resize(static_cast<size_t>(cur_count));
    row_icon_cache_.resize(static_cast<size_t>(cur_count), -1);

    // Prevent flicker / repaint storms while updating many cells
    SendMessageW(hListView_, WM_SETREDRAW, FALSE, 0);
    bool rows_changed = false;

    uint64_t total_speed = 0;
    int downloading_count = 0;

    for (size_t i = 0; i < visible_sessions.size(); ++i) {
        const auto& s = visible_sessions[i];
        auto prog = s->get_progress();

        total_speed += prog.speed_bytes_per_sec;
        if (prog.status == SessionStatus::DOWNLOADING || prog.status == SessionStatus::PROBING) {
            downloading_count++;
        }

        std::string fname = s->get_filename();
        std::wstring wfn = utf8_to_wide(fname);

        std::string sz = format_bytes(prog.total_size);
        std::wstring wsz(sz.begin(), sz.end());

        std::string st = "Idle";
        if (prog.status == SessionStatus::DOWNLOADING) {
            int pct = static_cast<int>(prog.progress_fraction * 100);
            st = "Downloading " + std::to_string(pct) + "%";
        } else if (prog.status == SessionStatus::PAUSED) {
            st = "Paused";
        } else if (prog.status == SessionStatus::COMPLETED) {
            st = "Complete";
        } else if (prog.status == SessionStatus::ASSEMBLING) {
            st = "Assembling...";
        } else if (prog.status == SessionStatus::ERROR) {
            st = "Error: " + prog.error_message;
        }
        std::wstring wst = utf8_to_wide(st);

        std::string eta = format_eta(prog.eta_seconds);
        std::wstring weta(eta.begin(), eta.end());

        std::string spd = format_speed(prog.speed_bytes_per_sec);
        std::wstring wspd(spd.begin(), spd.end());

        std::string date_added = "Today";
        std::wstring wdate(date_added.begin(), date_added.end());

        std::string dom = get_url_domain(s->get_url());
        std::wstring wdom(dom.begin(), dom.end());

        // Cached file icon by extension to avoid GDI leak
        std::string ext = "";
        size_t dot = fname.rfind('.');
        if (dot != std::string::npos) {
            ext = fname.substr(dot + 1);
            for (char& c : ext) c = static_cast<char>(std::tolower(c));
        }
        int icon_idx = 0;
        auto icon_it = ext_icon_map_.find(ext);
        if (icon_it != ext_icon_map_.end()) {
            icon_idx = icon_it->second;
        } else {
            HICON hFileIcon = Win32Assets::get_file_type_icon(fname, true);
            icon_idx = ImageList_AddIcon(himlList_, hFileIcon);
            DestroyIcon(hFileIcon);
            ext_icon_map_[ext] = icon_idx;
        }

        const int row = static_cast<int>(i);
        std::vector<std::wstring> cols = {wfn, wsz, wst, weta, wspd, wdate, wdom};

        if (row >= cur_count) {
            LVITEMW lvi = {LVIF_TEXT | LVIF_IMAGE};
            lvi.iItem = row;
            lvi.iImage = icon_idx;
            lvi.pszText = (LPWSTR)wfn.c_str();
            ListView_InsertItem(hListView_, &lvi);
            for (int c = 1; c < static_cast<int>(cols.size()); ++c) {
                ListView_SetItemText(hListView_, row, c, (LPWSTR)cols[c].c_str());
            }
            row_cache_.push_back(cols);
            row_icon_cache_.push_back(icon_idx);
            cur_count++;
            rows_changed = true;
            continue;
        }

        // Existing row: only touch cells whose text actually changed
        auto& cached = row_cache_[i];
        if (cached.size() != cols.size()) cached.assign(cols.size(), std::wstring(L"\x01"));

        if (cached[0] != wfn || row_icon_cache_[i] != icon_idx) {
            LVITEMW lvi = {LVIF_TEXT | LVIF_IMAGE};
            lvi.iItem = row;
            lvi.iSubItem = 0;
            lvi.iImage = icon_idx;
            lvi.pszText = (LPWSTR)wfn.c_str();
            ListView_SetItem(hListView_, &lvi);
            cached[0] = wfn;
            row_icon_cache_[i] = icon_idx;
            rows_changed = true;
        }
        for (size_t c = 1; c < cols.size(); ++c) {
            if (cached[c] != cols[c]) {
                ListView_SetItemText(hListView_, row, static_cast<int>(c), (LPWSTR)cols[c].c_str());
                cached[c] = cols[c];
                rows_changed = true;
            }
        }
    }

    SendMessageW(hListView_, WM_SETREDRAW, TRUE, 0);
    if (rows_changed) {
        InvalidateRect(hListView_, nullptr, FALSE);
    }

    // Restore selection if matching
    if (!selected_session_id_.empty()) {
        for (size_t i = 0; i < visible_session_ids_.size(); ++i) {
            if (visible_session_ids_[i] == selected_session_id_) {
                int cur_sel = ListView_GetNextItem(hListView_, -1, LVNI_SELECTED);
                if (cur_sel != static_cast<int>(i)) {
                    ListView_SetItemState(hListView_, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
                break;
            }
        }
    }

    // Update status bar
    std::wstring count_txt = std::to_wstring(visible_sessions.size()) + L" items (" + std::to_wstring(downloading_count) + L" active)";
    SendMessageW(hStatusbar_, SB_SETTEXTW, 0, (LPARAM)count_txt.c_str());

    std::string spd_txt = "Total Speed: " + format_speed(total_speed);
    std::wstring wspd_txt(spd_txt.begin(), spd_txt.end());
    SendMessageW(hStatusbar_, SB_SETTEXTW, 1, (LPARAM)wspd_txt.c_str());

    bool lim_on = QueueManager::instance().is_global_speed_limit_enabled();
    std::wstring lim_txt = lim_on ? (L"Speed Limiter: ON (" + std::to_wstring(QueueManager::instance().get_global_speed_limit() / 1024) + L" KB/s)") : L"Speed Limiter: OFF";
    SendMessageW(hStatusbar_, SB_SETTEXTW, 2, (LPARAM)lim_txt.c_str());

    update_toolbar_sensitivity();
}

std::shared_ptr<DownloadSession> Win32MainWindow::get_selected_session() {
    int sel = ListView_GetNextItem(hListView_, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < static_cast<int>(visible_session_ids_.size())) {
        return QueueManager::instance().find_session(visible_session_ids_[sel]);
    }
    if (!selected_session_id_.empty()) {
        return QueueManager::instance().find_session(selected_session_id_);
    }
    return nullptr;
}

void Win32MainWindow::show_progress_dialog(std::shared_ptr<DownloadSession> session) {
    if (!session) return;
    std::string id = session->get_id();
    auto it = active_progress_dialogs_.find(id);
    if (it != active_progress_dialogs_.end() && it->second && IsWindow(it->second->get_hwnd())) {
        ShowWindow(it->second->get_hwnd(), SW_SHOWNORMAL);
        SetForegroundWindow(it->second->get_hwnd());
        return;
    }
    auto prog_dlg = std::make_shared<Win32ProgressDialog>(session, hwnd_);
    prog_dlg->show();
    active_progress_dialogs_[id] = prog_dlg;
}

void Win32MainWindow::update_toolbar_sensitivity() {
    auto s = get_selected_session();
    BOOL has_sel = (s != nullptr);
    BOOL can_resume = FALSE;
    BOOL can_stop = FALSE;

    if (has_sel) {
        auto st = s->get_status();
        if (st == SessionStatus::PAUSED || st == SessionStatus::IDLE || st == SessionStatus::ERROR) {
            can_resume = TRUE;
        } else if (st == SessionStatus::DOWNLOADING || st == SessionStatus::PROBING || st == SessionStatus::ASSEMBLING) {
            can_stop = TRUE;
        }
    }

    SendMessageW(hToolbar_, TB_ENABLEBUTTON, IDM_FILE_RESUME, MAKELPARAM(can_resume, 0));
    SendMessageW(hToolbar_, TB_ENABLEBUTTON, IDM_FILE_STOP, MAKELPARAM(can_stop, 0));
    SendMessageW(hToolbar_, TB_ENABLEBUTTON, IDM_FILE_DELETE, MAKELPARAM(has_sel, 0));

    bool q_running = QueueManager::instance().get_main_queue().is_running();
    SendMessageW(hToolbar_, TB_ENABLEBUTTON, IDM_DOWNLOADS_START_QUEUE, MAKELPARAM(!q_running, 0));
    SendMessageW(hToolbar_, TB_ENABLEBUTTON, IDM_DOWNLOADS_STOP_QUEUE, MAKELPARAM(q_running, 0));
}

void Win32MainWindow::add_download_url(const std::string& url, const std::string& path,
                                       const std::string& filename, bool start_now) {
    if (url.empty()) return;

    DownloadPromptInfo info;
    info.url = url;
    info.filename = filename.empty() ? "download.bin" : filename;
    info.destination_dir = path;
    info.category = QueueManager::detect_category(info.filename);

    prompt_download(info);
}

void Win32MainWindow::prompt_download(const DownloadPromptInfo& info) {
    DownloadPromptInfo target_info = info;

    if (YouTubeEngine::is_youtube_url(target_info.url) && target_info.audio_stream_url.empty()) {
        YouTubeVideoInfo yt_info;
        std::string err;
        if (YouTubeEngine::extract_video_info(target_info.url, yt_info, &err) && yt_info.has_playable_formats()) {
            auto yt_res = Win32YoutubeFormatDialog::show(hwnd_, yt_info);
            if (!yt_res.accepted) {
                return;
            }
            target_info.url = yt_res.video_url;
            target_info.filename = yt_res.filename;
            target_info.audio_stream_url = yt_res.audio_url;
            target_info.user_agent = yt_res.user_agent;
            target_info.is_streaming_media = yt_res.is_muxed;
            target_info.category = "Video";
            target_info.referer = "https://www.youtube.com/";
        }
    }

    auto res = Win32DownloadInfoDialog::show(hwnd_, target_info);
    if (res.action == DownloadInfoAction::CANCEL) {
        return;
    }
    if (res.action == DownloadInfoAction::ZIP_PREVIEW) {
        Win32ZipPreviewDialog::show(hwnd_, res.info.url);
        return;
    }

    std::error_code ec;
    fs::create_directories(fs::path(res.info.save_as_path).parent_path(), ec);

    static uint64_t counter = 1;
    std::string sess_id = "task_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter++);
    auto session = std::make_shared<DownloadSession>(sess_id, res.info.url, res.info.save_as_path);
    session->set_category(res.info.category);
    if (!res.info.username.empty()) {
        session->set_credentials(res.info.username, res.info.password);
    }
    if (!res.info.referer.empty()) session->set_header("Referer", res.info.referer);
    if (!res.info.cookies.empty()) session->set_header("Cookie", res.info.cookies);
    if (!res.info.user_agent.empty()) session->set_header("User-Agent", res.info.user_agent);
    if (!res.info.audio_stream_url.empty()) {
        session->set_audio_stream_url(res.info.audio_stream_url);
    }
    SiteLogonManager::instance().apply_to_session(*session, res.info.url);

    bool start = (res.action == DownloadInfoAction::START_NOW);
    QueueManager::instance().get_main_queue().add_session(session, start);

    if (start) {
        show_progress_dialog(session);
    }

    refresh_downloads_list();
}

void Win32MainWindow::post_prompt_download(const DownloadPromptInfo& info) {
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    auto* heap_info = new DownloadPromptInfo(info);
    if (!PostMessageW(hwnd_, WM_APP_PROMPT_DOWNLOAD, 0, reinterpret_cast<LPARAM>(heap_info))) {
        delete heap_info;
    }
}

void Win32MainWindow::on_add_url() {
    auto res = Win32AddUrlDialog::show(hwnd_);
    if (res.accepted && !res.url.empty()) {
        DownloadPromptInfo info;
        info.url = res.url;
        info.username = res.username;
        info.password = res.password;

        if (YouTubeEngine::is_youtube_url(res.url)) {
            YouTubeVideoInfo yt_info;
            std::string err;
            if (YouTubeEngine::extract_video_info(res.url, yt_info, &err) && yt_info.has_playable_formats()) {
                auto yt_res = Win32YoutubeFormatDialog::show(hwnd_, yt_info);
                if (yt_res.accepted) {
                    info.url = yt_res.video_url;
                    info.filename = yt_res.filename;
                    info.audio_stream_url = yt_res.audio_url;
                    info.user_agent = yt_res.user_agent;
                    info.is_streaming_media = yt_res.is_muxed;
                    info.category = "Video";
                    info.referer = "https://www.youtube.com/";
                } else {
                    return;
                }
            }
        }

        prompt_download(info);
    }
}

void Win32MainWindow::on_resume() {
    auto s = get_selected_session();
    if (s) {
        auto st = s->get_status();
        if (st == SessionStatus::PAUSED) {
            s->resume();
        } else if (st == SessionStatus::IDLE || st == SessionStatus::ERROR) {
            s->start();
        }
        refresh_downloads_list();
    }
}

void Win32MainWindow::on_stop() {
    auto s = get_selected_session();
    if (s) {
        s->pause();
        refresh_downloads_list();
    }
}

void Win32MainWindow::on_stop_all() {
    auto sessions = QueueManager::instance().get_main_queue().get_all_sessions();
    for (auto& s : sessions) {
        if (s) s->pause();
    }
    refresh_downloads_list();
}

void Win32MainWindow::on_delete() {
    // The confirmation MessageBox runs a modal loop that keeps dispatching
    // accelerators / LVN_KEYDOWN, so a second Delete press would re-enter here.
    if (delete_in_progress_) return;

    auto s = get_selected_session();
    if (!s) return;

    delete_in_progress_ = true;
    const std::string id = s->get_id();

    // BUGFIX: get_filename() returns by value. The old code built the wstring from
    // begin() of one temporary and end() of *another* temporary -> undefined
    // behaviour (garbage-length copy => freeze, then access-violation crash).
    const std::wstring wfn = utf8_to_wide(s->get_filename());
    const std::wstring prompt = L"Do you want to delete \"" + wfn + L"\" from the download list?";
    s.reset(); // don't keep the session alive across the modal loop

    int res = MessageBoxW(hwnd_, prompt.c_str(), L"Confirm Delete", MB_YESNO | MB_ICONQUESTION);
    if (res == IDYES) {
        auto dlg_it = active_progress_dialogs_.find(id);
        if (dlg_it != active_progress_dialogs_.end()) {
            auto dlg = dlg_it->second; // keep alive until close() returns
            active_progress_dialogs_.erase(dlg_it);
            if (dlg) dlg->close();
        }

        // Non-blocking: threads are joined and temp files removed in the background
        QueueManager::instance().get_main_queue().remove_session(id);

        if (selected_session_id_ == id) {
            selected_session_id_.clear();
        }
        refresh_downloads_list();
    }

    delete_in_progress_ = false;
}

void Win32MainWindow::on_delete_completed() {
    if (delete_in_progress_) return;
    delete_in_progress_ = true;

    int res = MessageBoxW(hwnd_, L"Delete all completed downloads from the list?", L"Confirm Clean", MB_YESNO | MB_ICONQUESTION);
    if (res == IDYES) {
        auto sessions = QueueManager::instance().get_main_queue().get_all_sessions();
        bool removed_any = false;
        for (auto& s : sessions) {
            if (s && s->get_status() == SessionStatus::COMPLETED) {
                std::string id = s->get_id();
                auto dlg_it = active_progress_dialogs_.find(id);
                if (dlg_it != active_progress_dialogs_.end()) {
                    auto dlg = dlg_it->second;
                    active_progress_dialogs_.erase(dlg_it);
                    if (dlg) dlg->close();
                }
                // Batch: persist once after the loop instead of once per item
                if (QueueManager::instance().get_main_queue().remove_session(id, false)) {
                    removed_any = true;
                    if (selected_session_id_ == id) selected_session_id_.clear();
                }
            }
        }
        if (removed_any) {
            QueueManager::instance().save_to_file();
        }
        refresh_downloads_list();
    }

    delete_in_progress_ = false;
}

void Win32MainWindow::on_start_queue() {
    QueueManager::instance().get_main_queue().start_queue();
    refresh_downloads_list();
}

void Win32MainWindow::on_stop_queue() {
    QueueManager::instance().get_main_queue().stop_queue();
    refresh_downloads_list();
}

void Win32MainWindow::on_speed_limiter() {
    bool current = QueueManager::instance().is_global_speed_limit_enabled();
    QueueManager::instance().enable_global_speed_limit(!current);
    refresh_downloads_list();
}

void Win32MainWindow::on_scheduler() {
    on_options(1); // open connection tab
}

void Win32MainWindow::on_options(int initial_tab) {
    Win32OptionsDialog::show(hwnd_, initial_tab);
    refresh_downloads_list();
}

void Win32MainWindow::on_site_grabber() {
    Win32GrabberDialog::show(hwnd_);
}

void Win32MainWindow::on_zip_preview() {
    Win32ZipPreviewDialog::show(hwnd_);
}

void Win32MainWindow::on_batch_import() {
    wchar_t szFile[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {sizeof(OPENFILENAMEW)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Batch Files (*.ef2;*.txt)\0*.ef2;*.txt\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameW(&ofn)) {
        std::wstring chosen(szFile);
        std::string imp_path(chosen.begin(), chosen.end());
        std::string err;
        auto items = BatchImporter::import_file(imp_path, err);
        if (items.empty()) {
            MessageBoxW(hwnd_, L"Failed to import batch file or no URLs found.", L"Import Batch", MB_OK | MB_ICONERROR);
        } else {
            for (const auto& it : items) {
                static uint64_t cnt = 1;
                std::string sid = "import_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(cnt++);
                std::string fn = it.filename.empty() ? "download.bin" : it.filename;
                auto sess = std::make_shared<DownloadSession>(sid, it.url, fn);
                QueueManager::instance().get_main_queue().add_session(sess, false);
            }
            std::wstring msg = L"Successfully imported " + std::to_wstring(items.size()) + L" downloads.";
            MessageBoxW(hwnd_, msg.c_str(), L"Import Batch", MB_OK | MB_ICONINFORMATION);
            refresh_downloads_list();
        }
    }
}

void Win32MainWindow::on_batch_export() {
    wchar_t szFile[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {sizeof(OPENFILENAMEW)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"OpenDM / IDM Batch File (*.ef2)\0*.ef2\0Text File (*.txt)\0*.txt\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

    if (GetSaveFileNameW(&ofn)) {
        std::wstring chosen(szFile);
        std::string exp_path(chosen.begin(), chosen.end());
        auto sessions = QueueManager::instance().get_main_queue().get_all_sessions();
        std::vector<BatchItem> items;
        for (const auto& s : sessions) {
            BatchItem bi;
            bi.url = s->get_url();
            bi.filename = s->get_filename();
            bi.path = s->get_destination_path();
            items.push_back(bi);
        }
        std::string err;
        if (BatchImporter::export_ef2(exp_path, items, err)) {
            MessageBoxW(hwnd_, L"Batch exported successfully!", L"Export Batch", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(hwnd_, L"Failed to export batch file.", L"Export Batch", MB_OK | MB_ICONERROR);
        }
    }
}

void Win32MainWindow::on_properties() {
    auto s = get_selected_session();
    if (s) {
        Win32PropertiesDialog::show(hwnd_, s);
    }
}

void Win32MainWindow::on_open_file() {
    auto s = get_selected_session();
    if (s) {
        std::string dest = s->get_destination_path();
        std::error_code ec;
        if (std::filesystem::exists(dest, ec)) {
            std::wstring wdest(dest.begin(), dest.end());
            ShellExecuteW(hwnd_, L"open", wdest.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            MessageBoxW(hwnd_, L"The downloaded file does not exist on disk.", L"Open File", MB_OK | MB_ICONWARNING);
        }
    }
}

void Win32MainWindow::on_open_folder() {
    auto s = get_selected_session();
    if (s) {
        std::string dest = s->get_destination_path();
        std::error_code ec;
        std::wstring wdest(dest.begin(), dest.end());
        if (std::filesystem::exists(dest, ec)) {
            std::wstring param = L"/select,\"" + wdest + L"\"";
            ShellExecuteW(hwnd_, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            std::wstring pdir = std::filesystem::path(dest).parent_path().wstring();
            ShellExecuteW(hwnd_, L"open", pdir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
}

void Win32MainWindow::on_copy_url() {
    auto s = get_selected_session();
    if (s) {
        std::string url = s->get_url();
        std::wstring wurl(url.begin(), url.end());
        if (OpenClipboard(hwnd_)) {
            EmptyClipboard();
            size_t len = (wurl.size() + 1) * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
            if (hMem) {
                memcpy(GlobalLock(hMem), wurl.c_str(), len);
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
            CloseClipboard();
        }
    }
}

bool Win32MainWindow::save_snapshot(const std::string& filename) {
    if (!hwnd_ || !IsWindow(hwnd_)) return false;

    RECT rc;
    GetWindowRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    HDC hdcWin = GetWindowDC(hwnd_);
    HDC hdcMem = CreateCompatibleDC(hdcWin);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcWin, w, h);
    HGDIOBJ hOld = SelectObject(hdcMem, hBitmap);

    PrintWindow(hwnd_, hdcMem, 2);

    Win32Assets::init_gdiplus();
    Gdiplus::Bitmap gdiBmp(hBitmap, nullptr);
    CLSID pngClsid;
    GetEncoderClsid(L"image/png", &pngClsid);
    std::wstring wpath(filename.begin(), filename.end());
    Gdiplus::Status st = gdiBmp.Save(wpath.c_str(), &pngClsid, nullptr);

    SelectObject(hdcMem, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd_, hdcWin);

    return (st == Gdiplus::Ok);
}

LRESULT CALLBACK Win32MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Win32MainWindow* self = reinterpret_cast<Win32MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<Win32MainWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
            return 0;
        }
        case WM_SIZE: {
            if (self) self->update_layout();
            return 0;
        }
        case WM_DPICHANGED: {
            if (self) {
                UINT newDpi = HIWORD(wParam);
                RECT* prc = reinterpret_cast<RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top, SWP_NOZORDER | SWP_NOACTIVATE);
                self->apply_dpi_scaling(newDpi);
            }
            return 0;
        }
        case WM_TIMER: {
            if (self) self->refresh_downloads_list();
            return 0;
        }
        case WM_APP_PROMPT_DOWNLOAD: {
            std::unique_ptr<DownloadPromptInfo> info(reinterpret_cast<DownloadPromptInfo*>(lParam));
            if (self && info) self->prompt_download(*info);
            return 0;
        }
        case WM_INITMENUPOPUP: {
            if (self) {
                HMENU hMenu = reinterpret_cast<HMENU>(wParam);
                auto s = self->get_selected_session();
                BOOL has_sel = (s != nullptr);
                UINT flag = has_sel ? MF_ENABLED : (MF_DISABLED | MF_GRAYED);

                EnableMenuItem(hMenu, IDM_FILE_RESUME, MF_BYCOMMAND | flag);
                EnableMenuItem(hMenu, IDM_FILE_STOP, MF_BYCOMMAND | flag);
                EnableMenuItem(hMenu, IDM_FILE_DELETE, MF_BYCOMMAND | flag);
                EnableMenuItem(hMenu, IDM_FILE_OPEN, MF_BYCOMMAND | flag);
                EnableMenuItem(hMenu, IDM_FILE_OPEN_FOLDER, MF_BYCOMMAND | flag);
                EnableMenuItem(hMenu, IDM_FILE_COPY_URL, MF_BYCOMMAND | flag);
                EnableMenuItem(hMenu, IDM_FILE_PROPERTIES, MF_BYCOMMAND | flag);

                CheckMenuItem(hMenu, IDM_VIEW_SIDEBAR, MF_BYCOMMAND | (self->sidebar_visible_ ? MF_CHECKED : MF_UNCHECKED));
            }
            return 0;
        }
        case WM_NOTIFY: {
            NMHDR* nmhdr = reinterpret_cast<NMHDR*>(lParam);
            if (!self) break;

            if (nmhdr->hwndFrom == self->hTreeView_ && nmhdr->code == TVN_SELCHANGEDW) {
                NMTREEVIEWW* nmtv = reinterpret_cast<NMTREEVIEWW*>(lParam);
                wchar_t buf[128] = {0};
                TVITEMW tvi = {};
                tvi.hItem = nmtv->itemNew.hItem;
                tvi.mask = TVIF_TEXT;
                tvi.pszText = buf;
                tvi.cchTextMax = 128;
                TreeView_GetItem(self->hTreeView_, &tvi);
                std::wstring wt(buf);
                self->current_category_.assign(wt.begin(), wt.end());
                self->refresh_downloads_list();
                return 0;
            }

            if (nmhdr->hwndFrom == self->hListView_) {
                if (nmhdr->code == NM_DBLCLK) {
                    auto s = self->get_selected_session();
                    if (s) {
                        if (s->get_status() == SessionStatus::COMPLETED) {
                            self->on_open_file();
                        } else {
                            if (s->get_status() == SessionStatus::PAUSED) {
                                s->resume();
                            } else if (s->get_status() == SessionStatus::IDLE || s->get_status() == SessionStatus::ERROR) {
                                s->start();
                            }
                            self->show_progress_dialog(s);
                            self->refresh_downloads_list();
                        }
                    }
                    return 0;
                }
                if (nmhdr->code == NM_RCLICK) {
                    NMITEMACTIVATE* nmia = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                    int item_idx = nmia->iItem;
                    if (item_idx >= 0 && item_idx < static_cast<int>(self->visible_session_ids_.size())) {
                        ListView_SetItemState(self->hListView_, item_idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                        self->selected_session_id_ = self->visible_session_ids_[item_idx];
                        self->update_toolbar_sensitivity();
                    }
                    auto s = self->get_selected_session();
                    if (s) {
                        POINT pt;
                        GetCursorPos(&pt);
                        HMENU hPopup = CreatePopupMenu();
                        AppendMenuW(hPopup, MF_STRING, IDM_FILE_RESUME, L"Resume Download");
                        AppendMenuW(hPopup, MF_STRING, IDM_FILE_STOP, L"Stop Download");
                        AppendMenuW(hPopup, MF_STRING, IDM_FILE_DELETE, L"Delete");
                        AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);
                        AppendMenuW(hPopup, MF_STRING, IDM_FILE_OPEN, L"Open File");
                        AppendMenuW(hPopup, MF_STRING, IDM_FILE_OPEN_FOLDER, L"Open Folder");
                        AppendMenuW(hPopup, MF_STRING, IDM_FILE_COPY_URL, L"Copy URL");
                        AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);
                        AppendMenuW(hPopup, MF_STRING, IDM_FILE_PROPERTIES, L"Properties...");
                        TrackPopupMenu(hPopup, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
                        DestroyMenu(hPopup);
                    }
                    return 0;
                }
                if (nmhdr->code == LVN_ITEMCHANGED) {
                    NMLISTVIEW* nmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
                    if ((nmlv->uChanged & LVIF_STATE) && (nmlv->uNewState & LVIS_SELECTED)) {
                        if (nmlv->iItem >= 0 && nmlv->iItem < static_cast<int>(self->visible_session_ids_.size())) {
                            self->selected_session_id_ = self->visible_session_ids_[nmlv->iItem];
                        }
                    }
                    self->update_toolbar_sensitivity();
                }
                if (nmhdr->code == LVN_KEYDOWN) {
                    NMLVKEYDOWN* nmkd = reinterpret_cast<NMLVKEYDOWN*>(lParam);
                    if (nmkd->wVKey == VK_DELETE) {
                        self->on_delete();
                        return 0;
                    }
                    if (nmkd->wVKey == VK_RETURN) {
                        auto s = self->get_selected_session();
                        if (s) {
                            if (s->get_status() == SessionStatus::COMPLETED) {
                                self->on_open_file();
                            } else {
                                if (s->get_status() == SessionStatus::PAUSED) {
                                    s->resume();
                                } else if (s->get_status() == SessionStatus::IDLE || s->get_status() == SessionStatus::ERROR) {
                                    s->start();
                                }
                                self->show_progress_dialog(s);
                                self->refresh_downloads_list();
                            }
                        }
                        return 0;
                    }
                }
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (!self) break;

            switch (id) {
                case IDM_TASKS_ADD_URL: self->on_add_url(); return 0;
                case IDM_FILE_RESUME: self->on_resume(); return 0;
                case IDM_FILE_STOP: self->on_stop(); return 0;
                case IDM_DOWNLOADS_STOP_ALL: self->on_stop_all(); return 0;
                case IDM_FILE_DELETE: self->on_delete(); return 0;
                case IDM_DOWNLOADS_DELETE_COMPLETED: self->on_delete_completed(); return 0;
                case IDM_DOWNLOADS_START_QUEUE: self->on_start_queue(); return 0;
                case IDM_DOWNLOADS_STOP_QUEUE: self->on_stop_queue(); return 0;
                case IDM_DOWNLOADS_SPEED_LIMITER: self->on_speed_limiter(); return 0;
                case IDM_DOWNLOADS_SCHEDULER: self->on_scheduler(); return 0;
                case IDM_TASKS_OPTIONS: self->on_options(0); return 0;
                case IDM_TASKS_SITE_GRABBER: self->on_site_grabber(); return 0;
                case IDM_TASKS_ZIP_PREVIEW: self->on_zip_preview(); return 0;
                case IDM_TASKS_BATCH_IMPORT: self->on_batch_import(); return 0;
                case IDM_TASKS_BATCH_EXPORT: self->on_batch_export(); return 0;
                case IDM_FILE_PROPERTIES: self->on_properties(); return 0;
                case IDM_FILE_OPEN: self->on_open_file(); return 0;
                case IDM_FILE_OPEN_FOLDER: self->on_open_folder(); return 0;
                case IDM_FILE_COPY_URL: self->on_copy_url(); return 0;
                case IDM_VIEW_SIDEBAR: {
                    self->sidebar_visible_ = !self->sidebar_visible_;
                    self->update_layout();
                    HMENU hMenu = GetMenu(hwnd);
                    if (hMenu) {
                        CheckMenuItem(hMenu, IDM_VIEW_SIDEBAR, MF_BYCOMMAND | (self->sidebar_visible_ ? MF_CHECKED : MF_UNCHECKED));
                    }
                    return 0;
                }
                case IDM_HELP_ABOUT: {
                    Win32AboutDialog::show(hwnd);
                    return 0;
                }
                case IDM_HELP_GITHUB: {
                    ShellExecuteW(hwnd, L"open", L"https://github.com/nadermkhan/", nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                }
                case IDM_TASKS_EXIT: {
                    PostQuitMessage(0);
                    return 0;
                }
                default:
                    break;
            }
            break;
        }
        case WM_SETCURSOR: {
            if (self && self->sidebar_visible_) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                int sp_x = self->sidebar_width_;
                if (abs(pt.x - sp_x) <= 4) {
                    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            if (self && self->sidebar_visible_) {
                int x = LOWORD(lParam);
                if (abs(x - self->sidebar_width_) <= 6) {
                    self->dragging_splitter_ = true;
                    SetCapture(hwnd);
                    return 0;
                }
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (self && self->dragging_splitter_) {
                int x = LOWORD(lParam);
                if (x >= DpiHelper::scale(120, self->dpi_) && x <= DpiHelper::scale(500, self->dpi_)) {
                    self->sidebar_width_ = x;
                    self->update_layout();
                }
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP: {
            if (self && self->dragging_splitter_) {
                self->dragging_splitter_ = false;
                ReleaseCapture();
                return 0;
            }
            break;
        }
        case WM_DESTROY: {
            if (self) {
                if (self->timer_id_) {
                    KillTimer(hwnd, self->timer_id_);
                    self->timer_id_ = 0;
                }
                // Close any modeless progress windows before the owner goes away
                auto dialogs = std::move(self->active_progress_dialogs_);
                self->active_progress_dialogs_.clear();
                for (auto& kv : dialogs) {
                    if (kv.second) kv.second->close();
                }
            }
            PostQuitMessage(0);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace win32
} // namespace opendm
