#include "batch_importer.hpp"
#include "download_session.hpp"
#include "win32_unicode.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

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
    std::ifstream file(sys::path_from_utf8(filepath), std::ios::binary);
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

        if (!in_ef2_block) {
            if (t.empty()) continue;
            if (t == "<") {
                in_ef2_block = true;
                current_item = BatchItem{};
                line_in_block = 0;
                continue;
            }
            if (t[0] == '#' || t.rfind("//", 0) == 0) continue;
            if (t.rfind("http://", 0) == 0 || t.rfind("https://", 0) == 0 || t.rfind("ftp://", 0) == 0) {
                BatchItem bi;
                bi.url = t;
                bi.filename = DownloadSession::extract_filename_from_url(t);
                items.push_back(std::move(bi));
            }
            continue;
        }

        if (t == ">") {
            if (!current_item.url.empty()) {
                if (current_item.filename.empty()) {
                    current_item.filename = DownloadSession::extract_filename_from_url(current_item.url);
                } else {
                    current_item.filename = DownloadSession::sanitize_filename(
                        DownloadSession::url_decode(current_item.filename));
                }
                items.push_back(current_item);
            }
            in_ef2_block = false;
            line_in_block = 0;
            continue;
        }

        // Handle tagged EF2 lines without shifting positional slots
        std::string lower_t = sys::ascii_tolower(t);
        if (lower_t.rfind("referer:", 0) == 0) {
            current_item.referer = trim(t.substr(8));
        } else if (lower_t.rfind("cookie:", 0) == 0) {
            current_item.cookies = trim(t.substr(7));
        } else if (lower_t.rfind("user-agent:", 0) == 0) {
            current_item.user_agent = trim(t.substr(11));
        } else {
            line_in_block++;
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
    }

    if (in_ef2_block && !current_item.url.empty()) {
        if (current_item.filename.empty()) {
            current_item.filename = DownloadSession::extract_filename_from_url(current_item.url);
        } else {
            current_item.filename = DownloadSession::sanitize_filename(
                DownloadSession::url_decode(current_item.filename));
        }
        items.push_back(current_item);
    }

    return items;
}

bool BatchImporter::export_ef2(const std::string& filepath, const std::vector<BatchItem>& items, std::string& out_error) {
    std::ofstream file(sys::path_from_utf8(filepath), std::ios::binary | std::ios::trunc);
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

    file.flush();
    if (!file.good()) {
        out_error = "Failed while writing export file: " + filepath;
        return false;
    }
    return true;
}

} // namespace opendm
