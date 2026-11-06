#pragma once

#include <windows.h>
#include <string>

namespace opendm {
namespace win32 {

struct AddUrlResult {
    bool accepted{false};
    std::string url;
    std::string username;
    std::string password;
};

class Win32AddUrlDialog {
public:
    static AddUrlResult show(HWND parent, const std::string& default_url = "");
};

} // namespace win32
} // namespace opendm
