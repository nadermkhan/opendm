#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <functional>

namespace opendm {

struct ZipEntry {
    std::string filename;
    uint32_t uncompressed_size{0};
    uint32_t compressed_size{0};
    uint16_t compression_method{0}; // 0 = Stored, 8 = Deflated
    uint32_t crc32{0};
    uint32_t local_header_offset{0};
    bool is_directory{false};
};

class ZipPreviewer {
public:
    ZipPreviewer();
    ~ZipPreviewer() = default;

    // Fetches the remote ZIP archive's central directory via HTTP Range
    // Returns true on success and populates entries
    bool fetch_entries(const std::string& url, std::vector<ZipEntry>& out_entries, std::string& out_error);

    // Selectively downloads and extracts a single file from the remote ZIP archive
    bool extract_entry(const std::string& url, const ZipEntry& entry, const std::string& destination_filepath,
                       std::function<void(double)> progress_cb, std::string& out_error);
    bool extract_entry(const std::string& url, const ZipEntry& entry, const std::string& destination_filepath,
                       std::string& out_error);

private:
    static bool fetch_range(const std::string& url, const std::string& range_str,
                            std::vector<uint8_t>& out_data, std::string& out_error);

    static bool decompress_deflate(const uint8_t* in_data, size_t in_len,
                                   std::vector<uint8_t>& out_data, size_t expected_size);
};

} // namespace opendm
