#include "dash_parser.hpp"
#include <curl/curl.h>
#include <iostream>
#include <sstream>
#include <algorithm>

namespace opendm {

namespace {

size_t curl_string_write(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::string* s = static_cast<std::string*>(userp);
    s->append(static_cast<char*>(contents), total);
    return total;
}

std::string trim_spaces(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

} // namespace

std::string DashParser::resolve_url(const std::string& base_url, const std::string& relative_or_absolute) {
    if (relative_or_absolute.rfind("http://", 0) == 0 ||
        relative_or_absolute.rfind("https://", 0) == 0 ||
        relative_or_absolute.rfind("//", 0) == 0) {
        return relative_or_absolute;
    }

    if (base_url.empty()) return relative_or_absolute;

    size_t last_slash = base_url.find_last_of('/');
    if (last_slash != std::string::npos) {
        std::string base_dir = base_url.substr(0, last_slash + 1);
        if (!relative_or_absolute.empty() && relative_or_absolute[0] == '/') {
            // Root-relative
            size_t proto_end = base_url.find("://");
            if (proto_end != std::string::npos) {
                size_t host_slash = base_url.find('/', proto_end + 3);
                if (host_slash != std::string::npos) {
                    return base_url.substr(0, host_slash) + relative_or_absolute;
                }
            }
        }
        return base_dir + relative_or_absolute;
    }
    return base_url + "/" + relative_or_absolute;
}

std::string DashParser::extract_xml_attr(const std::string& tag_header, const std::string& attr_name) {
    auto match_attr = [&](const std::string& quote_str) -> std::string {
        std::string pattern = attr_name + quote_str;
        size_t pos = 0;
        while ((pos = tag_header.find(pattern, pos)) != std::string::npos) {
            if (pos == 0 || tag_header[pos - 1] == ' ' || tag_header[pos - 1] == '\t' ||
                tag_header[pos - 1] == '\n' || tag_header[pos - 1] == '\r') {
                size_t val_start = pos + pattern.length();
                char quote = quote_str[1];
                size_t val_end = tag_header.find(quote, val_start);
                if (val_end != std::string::npos) {
                    return tag_header.substr(val_start, val_end - val_start);
                }
            }
            pos += pattern.length();
        }
        return "";
    };

    std::string val = match_attr("=\"");
    if (!val.empty()) return val;
    return match_attr("='");
}

std::string DashParser::extract_xml_tag(const std::string& xml, const std::string& tag, size_t start_pos, size_t* out_end_pos) {
    std::string open_tag = "<" + tag;
    size_t open_pos = xml.find(open_tag, start_pos);
    if (open_pos == std::string::npos) return "";

    size_t header_end = xml.find('>', open_pos);
    if (header_end == std::string::npos) return "";

    if (xml[header_end - 1] == '/') {
        // Self-closing tag <tag ... />
        if (out_end_pos) *out_end_pos = header_end + 1;
        return xml.substr(open_pos, (header_end + 1) - open_pos);
    }

    std::string close_tag = "</" + tag + ">";
    size_t close_pos = xml.find(close_tag, header_end);
    if (close_pos == std::string::npos) {
        if (out_end_pos) *out_end_pos = header_end + 1;
        return xml.substr(open_pos, (header_end + 1) - open_pos);
    }

    if (out_end_pos) *out_end_pos = close_pos + close_tag.length();
    return xml.substr(open_pos, (close_pos + close_tag.length()) - open_pos);
}

bool DashParser::parse_mpd_xml(
    const std::string& xml_content,
    const std::string& manifest_base_url,
    DashManifest& out_manifest,
    std::string* out_error) {

    out_manifest.video_streams.clear();
    out_manifest.audio_streams.clear();

    if (xml_content.empty()) {
        if (out_error) *out_error = "MPD XML content is empty";
        return false;
    }

    // Check mediaPresentationDuration
    size_t mpd_tag_end = 0;
    std::string mpd_tag = extract_xml_tag(xml_content, "MPD", 0, &mpd_tag_end);
    if (!mpd_tag.empty()) {
        out_manifest.media_presentation_duration = extract_xml_attr(mpd_tag, "mediaPresentationDuration");
    }

    // Scan for top-level BaseURL inside MPD
    std::string root_base_url = manifest_base_url;
    size_t root_base_pos = xml_content.find("<BaseURL>");
    if (root_base_pos != std::string::npos) {
        size_t close = xml_content.find("</BaseURL>", root_base_pos);
        if (close != std::string::npos) {
            std::string u = trim_spaces(xml_content.substr(root_base_pos + 9, close - root_base_pos - 9));
            if (!u.empty()) root_base_url = resolve_url(manifest_base_url, u);
        }
    }

    // Scan for <AdaptationSet> tags
    size_t cursor = 0;
    while (cursor < xml_content.length()) {
        size_t next_end = 0;
        std::string adapt_block = extract_xml_tag(xml_content, "AdaptationSet", cursor, &next_end);
        if (adapt_block.empty()) break;
        cursor = next_end;

        size_t first_gt = adapt_block.find('>');
        std::string adapt_hdr = adapt_block.substr(0, first_gt);

        std::string mime_type = extract_xml_attr(adapt_hdr, "mimeType");
        std::string content_type = extract_xml_attr(adapt_hdr, "contentType");
        bool is_video = (content_type == "video" || mime_type.find("video") != std::string::npos);
        bool is_audio = (content_type == "audio" || mime_type.find("audio") != std::string::npos);

        // Scan <Representation> tags inside this AdaptationSet
        size_t rep_cursor = 0;
        while (rep_cursor < adapt_block.length()) {
            size_t rep_end = 0;
            std::string rep_block = extract_xml_tag(adapt_block, "Representation", rep_cursor, &rep_end);
            if (rep_block.empty()) break;
            rep_cursor = rep_end;

            size_t rep_first_gt = rep_block.find('>');
            std::string rep_hdr = rep_block.substr(0, rep_first_gt);

            DashRepresentation rep;
            rep.id = extract_xml_attr(rep_hdr, "id");
            rep.codecs = extract_xml_attr(rep_hdr, "codecs");
            if (rep.codecs.empty()) rep.codecs = extract_xml_attr(adapt_hdr, "codecs");

            std::string rep_mime = extract_xml_attr(rep_hdr, "mimeType");
            rep.mime_type = rep_mime.empty() ? mime_type : rep_mime;

            std::string bw_str = extract_xml_attr(rep_hdr, "bandwidth");
            if (!bw_str.empty()) {
                try { rep.bandwidth = std::stoull(bw_str); } catch (...) {}
            }

            std::string w_str = extract_xml_attr(rep_hdr, "width");
            if (!w_str.empty()) {
                try { rep.width = std::stoul(w_str); } catch (...) {}
            }

            std::string h_str = extract_xml_attr(rep_hdr, "height");
            if (!h_str.empty()) {
                try { rep.height = std::stoul(h_str); } catch (...) {}
            }

            // Look for <BaseURL> inside Representation
            size_t base_tag_pos = rep_block.find("<BaseURL>");
            if (base_tag_pos != std::string::npos) {
                size_t close = rep_block.find("</BaseURL>", base_tag_pos);
                if (close != std::string::npos) {
                    std::string u = trim_spaces(rep_block.substr(base_tag_pos + 9, close - base_tag_pos - 9));
                    rep.base_url = resolve_url(root_base_url, u);
                }
            } else {
                rep.base_url = root_base_url;
            }

            // Determine if video or audio from representation attributes if not set at adaptation level
            if (!is_video && !is_audio) {
                if (rep.width > 0 || rep.height > 0 || rep.mime_type.find("video") != std::string::npos) {
                    is_video = true;
                } else if (rep.mime_type.find("audio") != std::string::npos) {
                    is_audio = true;
                }
            }

            if (is_video) {
                out_manifest.video_streams.push_back(rep);
            } else if (is_audio) {
                out_manifest.audio_streams.push_back(rep);
            }
        }
    }

    if (out_manifest.video_streams.empty() && out_manifest.audio_streams.empty()) {
        if (out_error) *out_error = "No video or audio representations found in MPD manifest";
        return false;
    }

    return true;
}

bool DashParser::fetch_and_parse(
    const std::string& mpd_url,
    DashManifest& out_manifest,
    std::string* out_error) {

    CURL* curl = curl_easy_init();
    if (!curl) {
        if (out_error) *out_error = "Failed to initialize curl";
        return false;
    }

    std::string response_data;
    curl_easy_setopt(curl, CURLOPT_URL, mpd_url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_string_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) OpenDM/1.0");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        if (out_error) *out_error = "HTTP error fetching MPD manifest: " + std::to_string(http_code);
        return false;
    }

    return parse_mpd_xml(response_data, mpd_url, out_manifest, out_error);
}

const DashRepresentation* DashParser::select_best_video(const DashManifest& manifest) {
    if (manifest.video_streams.empty()) return nullptr;

    const DashRepresentation* best = &manifest.video_streams[0];
    for (const auto& v : manifest.video_streams) {
        uint64_t current_pixels = static_cast<uint64_t>(v.width) * v.height;
        uint64_t best_pixels = static_cast<uint64_t>(best->width) * best->height;

        if (current_pixels > best_pixels) {
            best = &v;
        } else if (current_pixels == best_pixels && v.bandwidth > best->bandwidth) {
            best = &v;
        }
    }
    return best;
}

const DashRepresentation* DashParser::select_best_audio(const DashManifest& manifest) {
    if (manifest.audio_streams.empty()) return nullptr;

    const DashRepresentation* best = &manifest.audio_streams[0];
    for (const auto& a : manifest.audio_streams) {
        if (a.bandwidth > best->bandwidth) {
            best = &a;
        }
    }
    return best;
}

} // namespace opendm
