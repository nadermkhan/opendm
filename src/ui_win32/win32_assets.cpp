#include "win32_assets.hpp"
#include <gdiplus.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace opendm {
namespace win32 {

static ULONG_PTR s_gdiplusToken = 0;
static bool s_gdiplusInitialized = false;

void Win32Assets::init_gdiplus() {
    if (!s_gdiplusInitialized) {
        Gdiplus::GdiplusStartupInput startupInput;
        Gdiplus::GdiplusStartup(&s_gdiplusToken, &startupInput, nullptr);
        s_gdiplusInitialized = true;
    }
}

void Win32Assets::shutdown_gdiplus() {
    if (s_gdiplusInitialized) {
        Gdiplus::GdiplusShutdown(s_gdiplusToken);
        s_gdiplusInitialized = false;
    }
}

std::wstring Win32Assets::resolve_asset_path(const std::string& filename) {
    std::wstring wfname(filename.begin(), filename.end());

    // 1. Current directory: data/ui_assets/
    std::wstring p1 = L"data\\ui_assets\\" + wfname;
    if (GetFileAttributesW(p1.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return p1;
    }

    // 2. Relative to executable module path
    wchar_t exe_path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        PathRemoveFileSpecW(exe_path); // exe directory
        std::wstring p2 = std::wstring(exe_path) + L"\\data\\ui_assets\\" + wfname;
        if (GetFileAttributesW(p2.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return p2;
        }

        PathRemoveFileSpecW(exe_path); // parent directory
        std::wstring p3 = std::wstring(exe_path) + L"\\data\\ui_assets\\" + wfname;
        if (GetFileAttributesW(p3.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return p3;
        }
    }

    return L"";
}

HBITMAP Win32Assets::load_bitmap(const std::string& filename, int target_w, int target_h) {
    init_gdiplus();
    std::wstring path = resolve_asset_path(filename);
    if (path.empty()) return nullptr;

    Gdiplus::Bitmap bmp(path.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return nullptr;

    int orig_w = bmp.GetWidth();
    int orig_h = bmp.GetHeight();
    if (target_w <= 0) target_w = orig_w;
    if (target_h <= 0) target_h = orig_h;

    HBITMAP hBmp = nullptr;
    if (target_w == orig_w && target_h == orig_h) {
        bmp.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hBmp);
    } else {
        Gdiplus::Bitmap resized(target_w, target_h, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&resized);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        float scale = std::min((float)target_w / orig_w, (float)target_h / orig_h);
        int draw_w = std::max(1, (int)(orig_w * scale + 0.5f));
        int draw_h = std::max(1, (int)(orig_h * scale + 0.5f));
        int draw_x = (target_w - draw_w) / 2;
        int draw_y = (target_h - draw_h) / 2;

        g.DrawImage(&bmp, draw_x, draw_y, draw_w, draw_h);
        resized.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hBmp);
    }

    return hBmp;
}

HICON Win32Assets::load_icon(const std::string& filename, int target_size) {
    init_gdiplus();
    std::wstring path = resolve_asset_path(filename);
    if (path.empty()) return nullptr;

    Gdiplus::Bitmap bmp(path.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return nullptr;

    int orig_w = bmp.GetWidth();
    int orig_h = bmp.GetHeight();
    if (target_size <= 0) target_size = orig_w;

    HICON hIcon = nullptr;
    if (target_size == orig_w && target_size == orig_h) {
        bmp.GetHICON(&hIcon);
    } else {
        Gdiplus::Bitmap resized(target_size, target_size, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics g(&resized);
            g.Clear(Gdiplus::Color(0, 0, 0, 0));
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

            float scale = std::min((float)target_size / orig_w, (float)target_size / orig_h);
            int draw_w = std::max(1, (int)(orig_w * scale + 0.5f));
            int draw_h = std::max(1, (int)(orig_h * scale + 0.5f));
            int draw_x = (target_size - draw_w) / 2;
            int draw_y = (target_size - draw_h) / 2;

            g.DrawImage(&bmp, draw_x, draw_y, draw_w, draw_h);
        }
        resized.GetHICON(&hIcon);
    }

    return hIcon;
}

HICON Win32Assets::load_disabled_icon(const std::string& filename, int target_size) {
    init_gdiplus();
    std::wstring path = resolve_asset_path(filename);
    if (path.empty()) return nullptr;

    Gdiplus::Bitmap bmp(path.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return nullptr;

    int orig_w = bmp.GetWidth();
    int orig_h = bmp.GetHeight();
    if (target_size <= 0) target_size = orig_w;

    Gdiplus::Bitmap resized(target_size, target_size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&resized);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        float scale = std::min((float)target_size / orig_w, (float)target_size / orig_h);
        int draw_w = std::max(1, (int)(orig_w * scale + 0.5f));
        int draw_h = std::max(1, (int)(orig_h * scale + 0.5f));
        int draw_x = (target_size - draw_w) / 2;
        int draw_y = (target_size - draw_h) / 2;

        // Desaturate to clean grayscale and reduce opacity to 40%
        Gdiplus::ColorMatrix cm = {
            0.299f * 0.7f, 0.299f * 0.7f, 0.299f * 0.7f, 0.0f, 0.0f,
            0.587f * 0.7f, 0.587f * 0.7f, 0.587f * 0.7f, 0.0f, 0.0f,
            0.114f * 0.7f, 0.114f * 0.7f, 0.114f * 0.7f, 0.0f, 0.0f,
            0.0f,          0.0f,          0.0f,          0.40f, 0.0f,
            0.0f,          0.0f,          0.0f,          0.0f,  1.0f
        };
        Gdiplus::ImageAttributes attr;
        attr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);

        g.DrawImage(&bmp, Gdiplus::Rect(draw_x, draw_y, draw_w, draw_h),
                    0, 0, orig_w, orig_h,
                    Gdiplus::UnitPixel, &attr);
    }

    HICON hIcon = nullptr;
    resized.GetHICON(&hIcon);
    return hIcon;
}

HIMAGELIST Win32Assets::create_image_list(const std::vector<std::string>& filenames, int icon_size) {
    HIMAGELIST himl = ImageList_Create(icon_size, icon_size, ILC_COLOR32 | ILC_MASK, static_cast<int>(filenames.size()), 4);
    if (!himl) return nullptr;

    for (const auto& fn : filenames) {
        HICON hIcon = load_icon(fn, icon_size);
        if (hIcon) {
            ImageList_AddIcon(himl, hIcon);
            DestroyIcon(hIcon);
        } else {
            // Add fallback generic icon
            HICON hFallback = LoadIcon(nullptr, IDI_APPLICATION);
            ImageList_AddIcon(himl, hFallback);
        }
    }

    return himl;
}

HIMAGELIST Win32Assets::create_disabled_image_list(const std::vector<std::string>& filenames, int icon_size) {
    HIMAGELIST himl = ImageList_Create(icon_size, icon_size, ILC_COLOR32 | ILC_MASK, static_cast<int>(filenames.size()), 4);
    if (!himl) return nullptr;

    for (const auto& fn : filenames) {
        HICON hIcon = load_disabled_icon(fn, icon_size);
        if (hIcon) {
            ImageList_AddIcon(himl, hIcon);
            DestroyIcon(hIcon);
        } else {
            HICON hFallback = LoadIcon(nullptr, IDI_APPLICATION);
            ImageList_AddIcon(himl, hFallback);
        }
    }

    return himl;
}

HICON Win32Assets::get_file_type_icon(const std::string& filename, bool small_icon) {
    std::wstring wname(filename.begin(), filename.end());
    SHFILEINFOW sfi = {};
    DWORD_PTR res = SHGetFileInfoW(
        wname.c_str(),
        FILE_ATTRIBUTE_NORMAL,
        &sfi,
        sizeof(sfi),
        SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | (small_icon ? SHGFI_SMALLICON : SHGFI_LARGEICON)
    );
    if (res && sfi.hIcon) {
        return sfi.hIcon;
    }
    return LoadIcon(nullptr, IDI_APPLICATION);
}

} // namespace win32
} // namespace opendm
