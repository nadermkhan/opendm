#include "win32_progress_dialog.hpp"
#include "win32_download_complete_dialog.hpp"
#include "win32_dialogs_common.hpp"
#include "../core/download_queue.hpp"
#include "../core/win32_unicode.hpp"
#include "dpi_helper.hpp"
#include "win32_assets.hpp"
#include <gdiplus.h>
#include <algorithm>
#include <vector>

namespace opendm {
namespace win32 {

namespace {

constexpr UINT WM_APP_PROGRESS_COMPLETED = WM_APP + 201;

} // namespace

Win32ProgressDialog::Win32ProgressDialog(std::shared_ptr<DownloadSession> session, HWND parent)
    : session_(session), parent_(parent) {
    dpi_ = DpiHelper::get_dpi(parent);
    hFont_ = DpiHelper::create_font(9, FW_NORMAL, false, dpi_);
    hFontBold_ = DpiHelper::create_font(9, FW_SEMIBOLD, false, dpi_);
    setup_ui();
}

Win32ProgressDialog::~Win32ProgressDialog() {
    if (timer_id_) {
        KillTimer(hwnd_, timer_id_);
        timer_id_ = 0;
    }
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (hFont_) DeleteObject(hFont_);
    if (hFontBold_) DeleteObject(hFontBold_);
}

void Win32ProgressDialog::setup_ui() {
    static bool s_registered = false;
    const wchar_t* className = L"OpenDM_ProgressDialog";
    const wchar_t* visClassName = L"OpenDM_Visualizer";

    if (!s_registered) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);

        WNDCLASSEXW wcv = {sizeof(WNDCLASSEXW)};
        wcv.lpfnWndProc = VisualizerWndProc;
        wcv.hInstance = GetModuleHandleW(nullptr);
        wcv.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wcv.lpszClassName = visClassName;
        wcv.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wcv);

        s_registered = true;
    }

    int dlg_w = DpiHelper::scale(560, dpi_);
    int dlg_h = DpiHelper::scale(420, dpi_);

    int x = 0, y = 0;
    compute_centered_pos(parent_, dlg_w, dlg_h, x, y);

    std::string title = session_ ? (std::to_string(static_cast<int>(session_->get_progress().progress_fraction * 100)) + "% of " + session_->get_filename()) : "Download Progress";
    std::wstring wtitle = sys::utf8_to_wide(title);

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        className,
        wtitle.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        x, y, dlg_w, dlg_h,
        parent_, nullptr, GetModuleHandleW(nullptr), this
    );

    int margin_x = DpiHelper::scale(16, dpi_);
    int lbl_w = DpiHelper::scale(110, dpi_);
    int val_x = margin_x + lbl_w + DpiHelper::scale(8, dpi_);
    int val_w = dlg_w - val_x - margin_x - DpiHelper::scale(16, dpi_);
    int row_h = DpiHelper::scale(20, dpi_);
    int cur_y = DpiHelper::scale(14, dpi_);

    auto add_row = [&](const wchar_t* lbl_text, HWND& out_static) {
        HWND hL = CreateWindowExW(0, L"STATIC", lbl_text, WS_CHILD | WS_VISIBLE,
            margin_x, cur_y, lbl_w, row_h, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hL, WM_SETFONT, (WPARAM)hFontBold_, TRUE);

        out_static = CreateWindowExW(0, L"STATIC", L"-", WS_CHILD | WS_VISIBLE,
            val_x, cur_y, val_w, row_h, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(out_static, WM_SETFONT, (WPARAM)hFont_, TRUE);

        cur_y += row_h + DpiHelper::scale(4, dpi_);
    };

    add_row(L"Status:", hStaticStatus_);
    add_row(L"File Size:", hStaticSize_);
    add_row(L"Downloaded:", hStaticDownloaded_);
    add_row(L"Transfer Rate:", hStaticSpeed_);
    add_row(L"Time Left:", hStaticEta_);
    add_row(L"Resume Support:", hStaticResume_);

    cur_y += DpiHelper::scale(6, dpi_);

    // Primary Progress Bar
    int pbar_w = dlg_w - margin_x * 2 - DpiHelper::scale(16, dpi_);
    int pbar_h = DpiHelper::scale(22, dpi_);
    hProgressBar_ = CreateWindowExW(
        0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        margin_x, cur_y, pbar_w, pbar_h,
        hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(hProgressBar_, PBM_SETRANGE32, 0, 1000);

    cur_y += pbar_h + DpiHelper::scale(12, dpi_);

    // Connection visualizer custom bar
    int vis_h = DpiHelper::scale(28, dpi_);
    hVisualizer_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"OpenDM_Visualizer", L"",
        WS_CHILD | WS_VISIBLE,
        margin_x, cur_y, pbar_w, vis_h,
        hwnd_, nullptr, GetModuleHandleW(nullptr), this
    );

    cur_y += vis_h + DpiHelper::scale(10, dpi_);

    // Connections List
    int list_h = DpiHelper::scale(95, dpi_);
    hListConns_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER,
        margin_x, cur_y, pbar_w, list_h,
        hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(hListConns_, WM_SETFONT, (WPARAM)hFont_, TRUE);
    ListView_SetExtendedListViewStyle(hListConns_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    // Columns
    LVCOLUMNW lvc = {LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM};
    lvc.cx = DpiHelper::scale(110, dpi_);
    lvc.pszText = (LPWSTR)L"Connection";
    ListView_InsertColumn(hListConns_, 0, &lvc);

    lvc.cx = DpiHelper::scale(130, dpi_);
    lvc.pszText = (LPWSTR)L"Downloaded";
    ListView_InsertColumn(hListConns_, 1, &lvc);

    lvc.cx = pbar_w - DpiHelper::scale(245, dpi_);
    lvc.pszText = (LPWSTR)L"Status / Byte Range";
    ListView_InsertColumn(hListConns_, 2, &lvc);

    cur_y += list_h + DpiHelper::scale(12, dpi_);

    // Buttons
    int btn_w = DpiHelper::scale(95, dpi_);
    int btn_h = DpiHelper::scale(26, dpi_);
    int btn_x = dlg_w - margin_x - btn_w * 2 - DpiHelper::scale(24, dpi_);

    hBtnPauseResume_ = CreateWindowExW(
        0, L"BUTTON", L"Pause",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        btn_x, cur_y, btn_w, btn_h,
        hwnd_, (HMENU)301, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(hBtnPauseResume_, WM_SETFONT, (WPARAM)hFont_, TRUE);

    hBtnCancel_ = CreateWindowExW(
        0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        btn_x + btn_w + DpiHelper::scale(8, dpi_), cur_y, btn_w, btn_h,
        hwnd_, (HMENU)302, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(hBtnCancel_, WM_SETFONT, (WPARAM)hFont_, TRUE);

    // Start 200ms refresh timer
    timer_id_ = SetTimer(hwnd_, 1, 200, nullptr);

    update_ui();
}

void Win32ProgressDialog::show() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
    }
}

void Win32ProgressDialog::close() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void Win32ProgressDialog::update_ui() {
    if (!session_ || !hwnd_ || !IsWindow(hwnd_)) return;
    auto prog = session_->get_progress();

    // Check completion: stop timer immediately and let parent show FIFO completion modal safely
    if (prog.status == SessionStatus::COMPLETED) {
        if (!completed_notified_) {
            completed_notified_ = true;
            if (timer_id_) {
                KillTimer(hwnd_, timer_id_);
                timer_id_ = 0;
            }
            std::string sid = session_->get_id();
            std::string dest_path = session_->get_destination_path();
            uint64_t total_sz = prog.total_size > 0 ? static_cast<uint64_t>(prog.total_size) : 0;
            HWND parent_wnd = parent_;
            close();
            if (parent_wnd && IsWindow(parent_wnd)) {
                auto* pid = new std::string(sid);
                if (!PostMessageW(parent_wnd, WM_APP_PROGRESS_COMPLETED, 0, reinterpret_cast<LPARAM>(pid))) {
                    delete pid;
                }
            } else {
                Win32DownloadCompleteDialog::show(GetDesktopWindow(), dest_path, total_sz);
            }
            return;
        }
    }

    auto set_if_changed = [](HWND h, std::string& cache, const std::string& val) {
        if (cache != val) {
            cache = val;
            std::wstring wv = sys::utf8_to_wide(val);
            SetWindowTextW(h, wv.c_str());
        }
    };

    // Title
    int pct = std::clamp(static_cast<int>(prog.progress_fraction * 100.0 + 0.5), 0, 100);
    set_if_changed(hwnd_, last_title_, std::to_string(pct) + "% of " + session_->get_filename());

    // Status text
    std::string st_str = "Downloading...";
    if (prog.status == SessionStatus::PAUSED) st_str = "Paused";
    else if (prog.status == SessionStatus::IDLE) st_str = "Stopped";
    else if (prog.status == SessionStatus::PROBING) st_str = "Connecting...";
    else if (prog.status == SessionStatus::COMPLETED) st_str = "Complete";
    else if (prog.status == SessionStatus::ASSEMBLING) st_str = "Assembling file segments...";
    else if (prog.status == SessionStatus::ERROR) st_str = "Error: " + prog.error_message;
    set_if_changed(hStaticStatus_, last_status_, st_str);

    // Size, Downloaded, Speed, ETA
    set_if_changed(hStaticSize_, last_size_, sys::format_bytes(prog.total_size));
    set_if_changed(hStaticDownloaded_, last_dl_, sys::format_bytes(prog.downloaded_bytes) + " (" + std::to_string(pct) + "%)");
    set_if_changed(hStaticSpeed_, last_speed_, sys::format_speed(prog.speed_bytes_per_sec));
    set_if_changed(hStaticEta_, last_eta_, sys::format_eta(prog.eta_seconds));

    // Resume support
    int res_flag = session_->supports_ranges() ? 1 : 0;
    if (last_resume_ != res_flag) {
        last_resume_ = res_flag;
        SetWindowTextW(hStaticResume_, res_flag ? L"Yes" : L"No");
    }

    // 0.1% Progress bar resolution (0..1000)
    int pbar_pos = std::clamp(static_cast<int>(prog.progress_fraction * 1000.0 + 0.5), 0, 1000);
    if (last_pbar_pos_ != pbar_pos) {
        last_pbar_pos_ = pbar_pos;
        SendMessageW(hProgressBar_, PBM_SETPOS, static_cast<WPARAM>(pbar_pos), 0);
    }

    // Pause / Resume / Retry button state & label
    std::string btn_label = "Pause";
    BOOL btn_enabled = TRUE;
    if (prog.status == SessionStatus::PAUSED || prog.status == SessionStatus::IDLE) {
        btn_label = "Resume";
    } else if (prog.status == SessionStatus::ERROR) {
        btn_label = "Retry";
    } else if (prog.status == SessionStatus::ASSEMBLING || prog.status == SessionStatus::COMPLETED) {
        btn_enabled = FALSE;
    }
    EnableWindow(hBtnPauseResume_, btn_enabled);
    set_if_changed(hBtnPauseResume_, last_btn_label_, btn_label);

    // Refresh visualizer
    if (hVisualizer_) {
        InvalidateRect(hVisualizer_, nullptr, FALSE);
    }

    // Update connection table
    auto chunks = prog.chunks;
    int cur_count = ListView_GetItemCount(hListConns_);
    while (cur_count > static_cast<int>(chunks.size())) {
        ListView_DeleteItem(hListConns_, cur_count - 1);
        cur_count--;
    }
    for (size_t i = 0; i < chunks.size(); ++i) {
        std::wstring cname = L"Connection " + std::to_wstring(i + 1);
        std::wstring wdl_c = sys::utf8_to_wide(sys::format_bytes(chunks[i].downloaded_bytes));

        std::string c_st = "Downloading";
        if (chunks[i].status == ChunkStatus::COMPLETED) c_st = "Completed";
        else if (chunks[i].status == ChunkStatus::PENDING) c_st = "Pending";
        c_st += " [" + std::to_string(chunks[i].start_offset) + " - " + std::to_string(chunks[i].end_offset) + "]";
        std::wstring wc_st = sys::utf8_to_wide(c_st);

        if (static_cast<int>(i) >= cur_count) {
            LVITEMW lvi = {LVIF_TEXT};
            lvi.iItem = static_cast<int>(i);
            lvi.pszText = const_cast<LPWSTR>(cname.c_str());
            ListView_InsertItem(hListConns_, &lvi);
        } else {
            ListView_SetItemText(hListConns_, static_cast<int>(i), 0, const_cast<LPWSTR>(cname.c_str()));
        }
        ListView_SetItemText(hListConns_, static_cast<int>(i), 1, const_cast<LPWSTR>(wdl_c.c_str()));
        ListView_SetItemText(hListConns_, static_cast<int>(i), 2, const_cast<LPWSTR>(wc_st.c_str()));
    }
}

void Win32ProgressDialog::on_pause_resume() {
    if (!session_) return;
    auto prog = session_->get_progress();
    if (prog.status == SessionStatus::DOWNLOADING || prog.status == SessionStatus::PROBING) {
        session_->pause();
    } else {
        session_->resume();
    }
    update_ui();
}

void Win32ProgressDialog::on_cancel() {
    if (session_) {
        // Stop threads + delete partial files on a background thread so the UI never
        // waits on network I/O (DNS / TLS / stalled sockets / file assembly).
        QueueManager::instance().cancel_session_async(session_);
    }
    close();
}

bool Win32ProgressDialog::save_snapshot(const std::string& filename) {
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

    PrintWindow(hwnd_, hdcMem, 2 /*PW_RENDERFULLCONTENT*/);

    Win32Assets::init_gdiplus();
    Gdiplus::Bitmap gdiBmp(hBitmap, nullptr);

    CLSID pngClsid{};
    if (get_gdiplus_encoder_clsid(L"image/png", &pngClsid) < 0) {
        SelectObject(hdcMem, hOld);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(hwnd_, hdcWin);
        return false;
    }

    std::wstring wpath = sys::path_from_utf8(filename).wstring();
    Gdiplus::Status st = gdiBmp.Save(wpath.c_str(), &pngClsid, nullptr);

    SelectObject(hdcMem, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd_, hdcWin);

    return (st == Gdiplus::Ok);
}

LRESULT CALLBACK Win32ProgressDialog::VisualizerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Win32ProgressDialog* self = reinterpret_cast<Win32ProgressDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<Win32ProgressDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return 0;
    }

    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        // Background dark slate
        HBRUSH bgBrush = CreateSolidBrush(RGB(24, 28, 36));
        FillRect(memDC, &rc, bgBrush);
        DeleteObject(bgBrush);

        if (self && self->session_) {
            auto prog = self->session_->get_progress();
            auto chunks = prog.chunks;
            int64_t total = prog.total_size;

            if (total > 0 && !chunks.empty()) {
                HBRUSH brDownloaded = CreateSolidBrush(RGB(0, 180, 90));
                HBRUSH brPending = CreateSolidBrush(RGB(45, 60, 80));
                HPEN penSep = CreatePen(PS_SOLID, 1, RGB(15, 18, 24));
                HGDIOBJ oldPen = SelectObject(memDC, penSep);

                for (const auto& c : chunks) {
                    int x1 = static_cast<int>((static_cast<double>(c.start_offset) / total) * w);
                    int x2 = static_cast<int>((static_cast<double>(c.end_offset + 1) / total) * w);
                    if (x2 <= x1) x2 = x1 + 1;

                    // Pending span
                    RECT rcChunk = {x1, 0, x2, h};
                    FillRect(memDC, &rcChunk, brPending);

                    // Downloaded span
                    int64_t dl_bytes = std::min<int64_t>(c.downloaded_bytes, c.end_offset - c.start_offset + 1);
                    int dl_w = static_cast<int>((static_cast<double>(dl_bytes) / total) * w);
                    RECT rcDl = {x1, 0, std::min(x1 + dl_w, x2), h};
                    FillRect(memDC, &rcDl, brDownloaded);

                    // Separator line
                    MoveToEx(memDC, x2, 0, nullptr);
                    LineTo(memDC, x2, h);
                }

                SelectObject(memDC, oldPen);
                DeleteObject(penSep);
                DeleteObject(brDownloaded);
                DeleteObject(brPending);
            }
        }

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK Win32ProgressDialog::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Win32ProgressDialog* self = reinterpret_cast<Win32ProgressDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<Win32ProgressDialog*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
            return 0;
        }
        case WM_TIMER: {
            if (self) self->update_ui();
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 301) { // Pause / Resume
                if (self) self->on_pause_resume();
                return 0;
            }
            if (id == 302 || id == IDCANCEL) { // Cancel
                if (self) self->on_cancel();
                return 0;
            }
            break;
        }
        case WM_CLOSE: {
            if (self) self->close();
            return 0;
        }
        case WM_DESTROY: {
            if (self) {
                if (self->timer_id_) {
                    KillTimer(hwnd, self->timer_id_);
                    self->timer_id_ = 0;
                }
                self->hwnd_ = nullptr;
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace win32
} // namespace opendm
