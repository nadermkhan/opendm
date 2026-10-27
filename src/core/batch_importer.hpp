#pragma once

#include <string>
#include <vector>

namespace opendm {

struct BatchItem {
    std::string url;
    std::string filename;
    std::string path;
    std::string referer;
    std::string cookies;
    std::string user_agent;
};

class BatchImporter {
public:
    // Imports batch download items from an IDM .ef2 or plain text URL list
    static std::vector<BatchItem> import_file(const std::string& filepath, std::string& out_error);

    // Exports a list of batch download items to an IDM-compatible .ef2 file
    static bool export_ef2(const std::string& filepath, const std::vector<BatchItem>& items, std::string& out_error);
};

} // namespace opendm
