#pragma once

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace opendm {
namespace win32 {

class Win32Assets {
public:
    static void init_gdiplus();
    static void shutdown_gdiplus();

    static std::wstring resolve_asset_path(const std::string& filename);
    static HBITMAP load_bitmap(const std::string& filename, int target_w = 0, int target_h = 0);
    static HICON load_icon(const std::string& filename, int target_size = 0);
    static HICON load_disabled_icon(const std::string& filename, int target_size = 0);

    // Creates an ImageList from a list of asset filenames
    static HIMAGELIST create_image_list(const std::vector<std::string>& filenames, int icon_size);
    static HIMAGELIST create_disabled_image_list(const std::vector<std::string>& filenames, int icon_size);

    // Helpers to get system file type icon
    static HICON get_file_type_icon(const std::string& filename, bool small_icon = true);
};

} // namespace win32
} // namespace opendm
