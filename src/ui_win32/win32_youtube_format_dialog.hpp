#pragma once

#include <windows.h>
#include <string>
#include "../core/youtube_engine.hpp"

namespace opendm {
namespace win32 {

struct YoutubeFormatResult {
    bool accepted{false};
    std::string video_url;
    std::string audio_url;
    std::string filename;
    std::string user_agent;
    bool is_muxed{false};
};

class Win32YoutubeFormatDialog {
public:
    static YoutubeFormatResult show(HWND parent, const YouTubeVideoInfo& info);
};

} // namespace win32
} // namespace opendm
