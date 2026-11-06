#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include "../core/download_session.hpp"

namespace opendm {
namespace win32 {

class Win32PropertiesDialog {
public:
    static void show(HWND parent, std::shared_ptr<DownloadSession> session);
};

} // namespace win32
} // namespace opendm
