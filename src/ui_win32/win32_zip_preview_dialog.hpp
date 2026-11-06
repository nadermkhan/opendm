#pragma once

#include <windows.h>
#include <string>

namespace opendm {
namespace win32 {

class Win32ZipPreviewDialog {
public:
    static void show(HWND parent, const std::string& initial_url = "");
};

} // namespace win32
} // namespace opendm
