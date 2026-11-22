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
    HMONITOR hMon = parent ? MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST)
                           : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT rcWork{};
    if (GetMonitorInfoW(hMon, &mi)) {
        rcWork = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    }

    if (parent && IsWindow(parent) && GetWindowRect(parent, &rcAnchor)) {
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
