#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace opendm {

struct DashRepresentation {
    std::string id;
    std::string mime_type;      // "video/mp4", "audio/mp4", etc.
    std::string codecs;
    uint32_t width{0};
    uint32_t height{0};
    uint64_t bandwidth{0};
    std::string base_url;
    std::vector<std::string> segment_urls;
};

struct DashManifest {
    std::string media_presentation_duration;
    std::vector<DashRepresentation> video_streams;
    std::vector<DashRepresentation> audio_streams;
};

class DashParser {
public:
    // Parses raw XML content of an MPEG-DASH .mpd manifest file (modeled after IDM IDMVMPrs.dll)
    static bool parse_mpd_xml(
        const std::string& xml_content,
        const std::string& manifest_base_url,
        DashManifest& out_manifest,
        std::string* out_error = nullptr);

    // Downloads and parses a remote MPEG-DASH .mpd manifest
    static bool fetch_and_parse(
        const std::string& mpd_url,
        DashManifest& out_manifest,
        std::string* out_error = nullptr);

    // Selects the highest quality video representation (by resolution, then bandwidth)
    static const DashRepresentation* select_best_video(const DashManifest& manifest);

    // Selects the highest quality audio representation (by bandwidth)
    static const DashRepresentation* select_best_audio(const DashManifest& manifest);

private:
    static std::string resolve_url(const std::string& base_url, const std::string& relative_or_absolute);
    static std::string extract_xml_tag(const std::string& xml, const std::string& tag, size_t start_pos, size_t* out_end_pos = nullptr);
    static std::string extract_xml_attr(const std::string& tag_header, const std::string& attr_name);
};

} // namespace opendm
