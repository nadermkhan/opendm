#pragma once

#include <windows.h>
#include <string>

namespace opendm {
namespace win32 {

class DpiHelper {
public:
    static void init_dpi_awareness();
    static UINT get_dpi(HWND hwnd = nullptr);
    static int scale(int val, UINT dpi);
    static HFONT create_font(int point_size, int weight = FW_NORMAL, bool italic = false, UINT dpi = 96, const wchar_t* face_name = L"Segoe UI");
};

} // namespace win32
} // namespace opendm
