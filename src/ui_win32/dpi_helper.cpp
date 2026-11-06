#include "dpi_helper.hpp"

namespace opendm {
namespace win32 {

void DpiHelper::init_dpi_awareness() {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContextProc)(DPI_AWARENESS_CONTEXT);
        auto pSetProcessDpiAwarenessContext = 
            (SetProcessDpiAwarenessContextProc)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (pSetProcessDpiAwarenessContext) {
            pSetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            return;
        }
    }

    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (hShcore) {
        typedef HRESULT (WINAPI *SetProcessDpiAwarenessProc)(int);
        auto pSetProcessDpiAwareness = 
            (SetProcessDpiAwarenessProc)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (pSetProcessDpiAwareness) {
            // PROCESS_PER_MONITOR_DPI_AWARE = 2
            pSetProcessDpiAwareness(2);
            FreeLibrary(hShcore);
            return;
        }
        FreeLibrary(hShcore);
    }

    // Fallback to older Vista/7 system-wide DPI aware
    SetProcessDPIAware();
}

UINT DpiHelper::get_dpi(HWND hwnd) {
    if (hwnd) {
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            typedef UINT (WINAPI *GetDpiForWindowProc)(HWND);
            auto pGetDpiForWindow = (GetDpiForWindowProc)GetProcAddress(hUser32, "GetDpiForWindow");
            if (pGetDpiForWindow) {
                UINT dpi = pGetDpiForWindow(hwnd);
                if (dpi > 0) return dpi;
            }
        }
    }

    HDC hdc = GetDC(hwnd);
    if (hdc) {
        int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(hwnd, hdc);
        if (dpi > 0) return static_cast<UINT>(dpi);
    }
    return 96;
}

int DpiHelper::scale(int val, UINT dpi) {
    if (dpi == 96 || dpi == 0) return val;
    return MulDiv(val, static_cast<int>(dpi), 96);
}

HFONT DpiHelper::create_font(int point_size, int weight, bool italic, UINT dpi, const wchar_t* face_name) {
    int height = -MulDiv(point_size, static_cast<int>(dpi), 72);
    return CreateFontW(
        height,
        0, 0, 0,
        weight,
        italic ? TRUE : FALSE,
        FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        face_name
    );
}

} // namespace win32
} // namespace opendm
