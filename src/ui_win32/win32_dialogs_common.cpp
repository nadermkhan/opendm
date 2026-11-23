#include "win32_dialogs_common.hpp"
#include "../core/win32_unicode.hpp"
#include <gdiplus.h>
#include <vector>

namespace opendm {
namespace win32 {

std::string get_window_text_utf8(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return "";
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return "";
    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
    int copied = GetWindowTextW(hwnd, buf.data(), len + 1);
    if (copied <= 0) return "";
    buf.resize(static_cast<size_t>(copied));
    return sys::wide_to_utf8(buf);
}

void compute_centered_pos(HWND parent, int w, int h, int& out_x, int& out_y) {
    RECT rcAnchor{};
    bool valid_visible_parent = parent && IsWindow(parent) && IsWindowVisible(parent) && !IsIconic(parent);
    HMONITOR hMon = nullptr;
    if (valid_visible_parent) {
        hMon = MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST);
    } else {
        HWND fg = GetForegroundWindow();
        if (fg && IsWindow(fg)) {
            hMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        } else {
            POINT pt{};
            GetCursorPos(&pt);
            hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        }
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT rcWork{};
    if (GetMonitorInfoW(hMon, &mi)) {
        rcWork = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    }

    if (valid_visible_parent && GetWindowRect(parent, &rcAnchor)) {
        out_x = rcAnchor.left + ((rcAnchor.right - rcAnchor.left) - w) / 2;
        out_y = rcAnchor.top + ((rcAnchor.bottom - rcAnchor.top) - h) / 2;
    } else {
        out_x = rcWork.left + ((rcWork.right - rcWork.left) - w) / 2;
        out_y = rcWork.top + ((rcWork.bottom - rcWork.top) - h) / 2;
    }

    if (out_x + w > rcWork.right) out_x = rcWork.right - w;
    if (out_y + h > rcWork.bottom) out_y = rcWork.bottom - h;
    if (out_x < rcWork.left) out_x = rcWork.left;
    if (out_y < rcWork.top) out_y = rcWork.top;
}

void bring_popup_to_foreground(HWND hwnd, bool make_topmost) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    HWND fg = GetForegroundWindow();
    DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD cur_tid = GetCurrentThreadId();
    bool attached = (fg_tid != 0 && fg_tid != cur_tid && AttachThreadInput(cur_tid, fg_tid, TRUE) != FALSE);

    SetWindowPos(
        hwnd,
        make_topmost ? HWND_TOPMOST : HWND_TOP,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
    );
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);

    if (attached) {
        AttachThreadInput(cur_tid, fg_tid, FALSE);
    }
}

void run_modal_loop(HWND hwnd, HWND parent, bool& done) {
    if (parent && IsWindow(parent)) {
        EnableWindow(parent, FALSE);
    }

    MSG msg;
    while (!done && hwnd && IsWindow(hwnd)) {
        BOOL gm = GetMessageW(&msg, nullptr, 0, 0);
        if (gm == 0) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            done = true;
            break;
        }
        if (gm < 0) {
            done = true;
            break;
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (parent && IsWindow(parent)) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }
}

int get_gdiplus_encoder_clsid(const WCHAR* format, CLSID* pClsid) {
    if (!pClsid) return -1;
    *pClsid = CLSID{};
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    std::vector<BYTE> memory(size);
    auto* pImageCodecInfo = reinterpret_cast<Gdiplus::ImageCodecInfo*>(memory.data());
    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);

    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            return static_cast<int>(j);
        }
    }
    return -1;
}

} // namespace win32
} // namespace opendm
