#pragma once

#include <windows.h>
#include <string>
#include <cstdint>

namespace opendm {
namespace win32 {

class Win32DownloadCompleteDialog {
public:
    static void show(HWND parent, const std::string& filepath, uint64_t file_size);
};

} // namespace win32
} // namespace opendm
