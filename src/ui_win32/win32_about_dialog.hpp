#pragma once

#include <windows.h>
#include <string>

namespace opendm {
namespace win32 {

class Win32AboutDialog {
public:
    static void show(HWND parent);
    static bool save_snapshot(HWND parent, const std::string& filename);
};

} // namespace win32
} // namespace opendm
