#pragma once

#include <windows.h>
#include <string>

namespace opendm {
namespace win32 {

class Win32GrabberDialog {
public:
    static void show(HWND parent);
};

} // namespace win32
} // namespace opendm
