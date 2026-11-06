#include "win32_about_dialog.hpp"
#include "dpi_helper.hpp"
#include "win32_assets.hpp"
#include "resource.h"
#include <shellapi.h>
#include <gdiplus.h>
#include <vector>

namespace opendm {
namespace win32 {

namespace {

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

struct AboutDialogState {
    HWND hwnd{nullptr};
    HFONT hFont{nullptr};
    HFONT hFontBold{nullptr};
    HFONT hFontTitle{nullptr};
    HICON hIcon{nullptr};
    bool done{false};
};

LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AboutDialogState* state = reinterpret_cast<AboutDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<AboutDialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            state->hwnd = hwnd;
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 501) { // GitHub link
                ShellExecuteW(hwnd, L"open", L"https://github.com/nadermkhan/", nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
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

void Win32AboutDialog::show(HWND parent) {
    AboutDialogState state;
    UINT dpi = DpiHelper::get_dpi(parent);

    state.hFont = DpiHelper::create_font(9, FW_NORMAL, false, dpi);
    state.hFontBold = DpiHelper::create_font(9, FW_SEMIBOLD, false, dpi);
    state.hFontTitle = DpiHelper::create_font(13, FW_BOLD, false, dpi);
    state.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, DpiHelper::scale(48, dpi), DpiHelper::scale(48, dpi), LR_DEFAULTCOLOR);
    if (!state.hIcon) {
        state.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    }

    static bool s_registered = false;
    const wchar_t* className = L"OpenDM_AboutDialog";
    if (!s_registered) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = AboutWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        s_registered = true;
    }

    int dlg_w = DpiHelper::scale(440, dpi);
    int dlg_h = DpiHelper::scale(260, dpi);

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
        L"About OpenDM",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlg_w, dlg_h,
        parent, nullptr, GetModuleHandleW(nullptr), &state
    );

    int margin_x = DpiHelper::scale(20, dpi);
    int icon_sz = DpiHelper::scale(48, dpi);

    // App Icon
    HWND hIcon = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_ICON,
        margin_x, DpiHelper::scale(24, dpi), icon_sz, icon_sz,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hIcon, STM_SETICON, (WPARAM)state.hIcon, 0);

    int text_x = margin_x + icon_sz + DpiHelper::scale(16, dpi);
    int text_w = dlg_w - text_x - margin_x - DpiHelper::scale(16, dpi);

    // App Title
    HWND hTitle = CreateWindowExW(0, L"STATIC", L"OpenDM 1.0.0",
        WS_CHILD | WS_VISIBLE,
        text_x, DpiHelper::scale(22, dpi), text_w, DpiHelper::scale(26, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hTitle, WM_SETFONT, (WPARAM)state.hFontTitle, TRUE);

    // Description & Author
    const wchar_t* desc = 
        L"High-Speed Multipart Download Accelerator\n"
        L"Native Win32API recreation of IDM Architecture\n\n"
        L"Created by Nader Mahbub Khan\n"
        L"License: GNU GPL v3.0";
    HWND hDesc = CreateWindowExW(0, L"STATIC", desc,
        WS_CHILD | WS_VISIBLE,
        text_x, DpiHelper::scale(52, dpi), text_w, DpiHelper::scale(80, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hDesc, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    // GitHub Button & OK Button
    int btn_w = DpiHelper::scale(95, dpi);
    int btn_h = DpiHelper::scale(26, dpi);
    int btn_y = DpiHelper::scale(165, dpi);

    HWND hBtnGithub = CreateWindowExW(0, L"BUTTON", L"GitHub...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        text_x, btn_y, btn_w, btn_h,
        hwnd, (HMENU)501, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hBtnGithub, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    HWND hBtnOk = CreateWindowExW(0, L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        dlg_w - margin_x - btn_w - DpiHelper::scale(16, dpi), btn_y, btn_w, btn_h,
        hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hBtnOk, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    SetFocus(hBtnOk);

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
    if (state.hFontTitle) DeleteObject(state.hFontTitle);
    if (state.hIcon) DestroyIcon(state.hIcon);
}

bool Win32AboutDialog::save_snapshot(HWND parent, const std::string& filename) {
    // For automated test verification
    AboutDialogState state;
    UINT dpi = DpiHelper::get_dpi(parent);
    state.hFont = DpiHelper::create_font(9, FW_NORMAL, false, dpi);
    state.hFontBold = DpiHelper::create_font(9, FW_SEMIBOLD, false, dpi);
    state.hFontTitle = DpiHelper::create_font(13, FW_BOLD, false, dpi);
    state.hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    static bool s_registered = false;
    const wchar_t* className = L"OpenDM_AboutDialog";
    if (!s_registered) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = AboutWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        s_registered = true;
    }

    int dlg_w = DpiHelper::scale(440, dpi);
    int dlg_h = DpiHelper::scale(260, dpi);
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        className, L"About OpenDM",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        100, 100, dlg_w, dlg_h,
        parent, nullptr, GetModuleHandleW(nullptr), &state
    );

    UpdateWindow(hwnd);

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
    CLSID pngClsid;
    GetEncoderClsid(L"image/png", &pngClsid);
    std::wstring wpath(filename.begin(), filename.end());
    Gdiplus::Status st = gdiBmp.Save(wpath.c_str(), &pngClsid, nullptr);

    SelectObject(hdcMem, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcWin);
    DestroyWindow(hwnd);

    return (st == Gdiplus::Ok);
}

} // namespace win32
} // namespace opendm
