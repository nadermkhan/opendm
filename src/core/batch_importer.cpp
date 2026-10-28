#include "batch_importer.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace opendm {

namespace {

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

} // namespace

std::vector<BatchItem> BatchImporter::import_file(const std::string& filepath, std::string& out_error) {
    std::vector<BatchItem> items;
    std::ifstream file(filepath);
    if (!file.is_open()) {
        out_error = "Could not open file: " + filepath;
        return items;
    }

    std::string line;
    bool in_ef2_block = false;
    BatchItem current_item;
    int line_in_block = 0;

    while (std::getline(file, line)) {
        std::string t = trim(line);
        if (t.empty()) continue;

        if (t == "<") {
            in_ef2_block = true;
            current_item = BatchItem{};
            line_in_block = 0;
            continue;
        } else if (t == ">") {
            if (in_ef2_block && !current_item.url.empty()) {
                items.push_back(current_item);
            }
            in_ef2_block = false;
            line_in_block = 0;
            continue;
        }

        if (in_ef2_block) {
            line_in_block++;
            // Check for IDM tag prefixes (CUrlExporter::ReadFromIEFFile / WriteInELFFile)
            if (t.rfind("referer:", 0) == 0 || t.rfind("Referer:", 0) == 0) {
                current_item.referer = trim(t.substr(8));
            } else if (t.rfind("cookie:", 0) == 0 || t.rfind("Cookie:", 0) == 0) {
                current_item.cookies = trim(t.substr(7));
            } else if (t.rfind("User-Agent:", 0) == 0 || t.rfind("user-agent:", 0) == 0) {
                current_item.user_agent = trim(t.substr(11));
            } else {
                // Positional fallback
                if (line_in_block == 1) {
                    current_item.url = t;
                } else if (line_in_block == 2) {
                    current_item.filename = t;
                } else if (line_in_block == 3) {
                    current_item.path = t;
                } else if (line_in_block == 4) {
                    current_item.referer = t;
                } else if (line_in_block == 5) {
                    current_item.cookies = t;
                } else if (line_in_block == 6) {
                    current_item.user_agent = t;
                }
            }
        } else {
            // Plain text URL list (skip comments)
            if (t[0] == '#' || t.rfind("//", 0) == 0) continue;
            if (t.rfind("http://", 0) == 0 || t.rfind("https://", 0) == 0 || t.rfind("ftp://", 0) == 0) {
                BatchItem bi;
                bi.url = t;
                size_t slash = t.find_last_of('/');
                if (slash != std::string::npos && slash + 1 < t.length()) {
                    bi.filename = t.substr(slash + 1);
                    size_t q = bi.filename.find('?');
                    if (q != std::string::npos) bi.filename = bi.filename.substr(0, q);
                } else {
                    bi.filename = "download.bin";
                }
                items.push_back(bi);
            }
        }
    }

    if (in_ef2_block && !current_item.url.empty()) {
        items.push_back(current_item);
    }

    return items;
}

bool BatchImporter::export_ef2(const std::string& filepath, const std::vector<BatchItem>& items, std::string& out_error) {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        out_error = "Could not create file for export: " + filepath;
        return false;
    }

    for (const auto& item : items) {
        file << "<\n";
        file << item.url << "\n";
        file << item.filename << "\n";
        file << item.path << "\n";
        file << item.referer << "\n";
        file << item.cookies << "\n";
        file << item.user_agent << "\n";
        file << ">\n";
    }

    return true;
}

} // namespace opendm
