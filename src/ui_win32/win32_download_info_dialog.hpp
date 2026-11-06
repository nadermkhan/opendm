#pragma once

#include <windows.h>
#include <string>
#include "../ipc/ipc_server.hpp"

namespace opendm {
namespace win32 {

enum class DownloadInfoAction {
    START_NOW,
    DOWNLOAD_LATER,
    CANCEL,
    ZIP_PREVIEW
};

struct DownloadInfoResult {
    DownloadInfoAction action{DownloadInfoAction::CANCEL};
    DownloadPromptInfo info;
};

class Win32DownloadInfoDialog {
public:
    static DownloadInfoResult show(HWND parent, const DownloadPromptInfo& initial_info);
    static bool save_snapshot(HWND parent, const DownloadPromptInfo& initial_info, const std::string& filename);
};

} // namespace win32
} // namespace opendm
