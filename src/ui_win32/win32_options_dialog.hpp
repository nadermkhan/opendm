#pragma once

#include <windows.h>
#include <string>

namespace opendm {
namespace win32 {

class Win32OptionsDialog {
public:
    static void show(HWND parent, int initial_tab = 0);
};

} // namespace win32
} // namespace opendm
