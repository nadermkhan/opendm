#include "youtube_engine.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <algorithm>
#include <unordered_set>
#include <mutex>

namespace opendm {

using json = nlohmann::json;

namespace {

static std::string s_cached_visitor_data;
static std::mutex s_visitor_mutex;

size_t curl_string_write(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total);
    return total;
}

size_t curl_discard_write(void* contents, size_t size, size_t nmemb, void* userp) {
    return size * nmemb;
}

} // namespace

bool YouTubeEngine::is_youtube_url(const std::string& url) {
    if (url.empty()) return false;
    if (url.find("youtube.com/watch") != std::string::npos ||
        url.find("youtu.be/") != std::string::npos ||
        url.find("youtube.com/shorts/") != std::string::npos ||
        url.find("youtube.com/embed/") != std::string::npos ||
        url.find("youtube.com/v/") != std::string::npos ||
        url.find("youtube.com/live/") != std::string::npos ||
        url.find("youtube.com/watch_popup") != std::string::npos) {
        return true;
    }
    // Also check if raw 11-char ID
    if (url.length() == 11) {
        std::regex id_rx("^[0-9A-Za-z_-]{11}$");
        if (std::regex_match(url, id_rx)) return true;
    }
    return false;
}

std::string YouTubeEngine::extract_video_id(const std::string& url) {
    if (url.empty()) return "";
    if (url.length() == 11) {
        std::regex id_rx("^[0-9A-Za-z_-]{11}$");
        if (std::regex_match(url, id_rx)) return url;
    }

    // 1. Check for ?v= or &v=
    {
        std::regex v_param_rx(R"((?:[?&]v=|\/shorts\/|\/embed\/|\/v\/|\/live\/|youtu\.be\/)([0-9A-Za-z_-]{11}))");
        std::smatch m;
        if (std::regex_search(url, m, v_param_rx) && m.size() > 1) {
            return m[1].str();
        }
    }

    return "";
}

std::vector<YouTubeEngine::InnerTubeClient> YouTubeEngine::get_client_hierarchy() {
    std::vector<InnerTubeClient> clients;

    // 1. VISIONOS: Client ID 101, Version 1.02 (Apple Vision Pro RealityDevice17,1).
    // Primary high-definition client: delivers unthrottled 4K (2160p), 1440p, 1080p, 720p streams without PO token.
    {
        InnerTubeClient c;
        c.name = "VISIONOS";
        c.client_name = "VISIONOS";
        c.client_version = "1.02";
        c.client_id = "101";
        c.device_make = "Apple";
        c.device_model = "RealityDevice17,1";
        c.os_name = "visionOS";
        c.os_version = "26.5.23O471";
        c.user_agent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
        clients.push_back(c);
    }

    // 2. ANDROID_VR: Client ID 28, Version 1.65.10 (Oculus Quest 3, Android 12L SDK 32).
    // Secondary high-definition client: provides unthrottled 4K, 1440p, 1080p, 720p streams with direct signed URLs.
    {
        InnerTubeClient c;
        c.name = "ANDROID_VR";
        c.client_name = "ANDROID_VR";
        c.client_version = "1.65.10";
        c.client_id = "28";
        c.device_make = "Oculus";
        c.device_model = "Quest 3";
        c.android_sdk_version = 32;
        c.os_name = "Android";
        c.os_version = "12L";
        c.user_agent = "com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip";
        clients.push_back(c);
    }

    // 3. ANDROID: Client ID 3, Version 21.26.364 (Android 11, SDK 30).
    // High-compatibility mobile client: delivers progressive itag 18 (360p) with ratebypass=yes.
    {
        InnerTubeClient c;
        c.name = "ANDROID";
        c.client_name = "ANDROID";
        c.client_version = "21.26.364";
        c.client_id = "3";
        c.android_sdk_version = 30;
        c.os_name = "Android";
        c.os_version = "11";
        c.user_agent = "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip";
        clients.push_back(c);
    }

    // 4. IOS: Client ID 5, Version 21.26.4 (iPhone16,2, iOS 18.3.2).
    // Native Apple mobile client: fallback mobile endpoint.
    {
        InnerTubeClient c;
        c.name = "IOS";
        c.client_name = "IOS";
        c.client_version = "21.26.4";
        c.client_id = "5";
        c.device_make = "Apple";
        c.device_model = "iPhone16,2";
        c.os_name = "iPhone";
        c.os_version = "18.3.2.22D82";
        c.user_agent = "com.google.ios.youtube/21.26.4 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)";
        clients.push_back(c);
    }

    // 5. TVHTML5 (Cobalt 25.lts): Client ID 7, Version 7.20260707.07.00.
    // Cobalt LTS TV client, returns clean direct streams without n-signature challenge.
    {
        InnerTubeClient c;
        c.name = "TVHTML5";
        c.client_name = "TVHTML5";
        c.client_version = "7.20260707.07.00";
        c.client_id = "7";
        c.os_name = "ChromiumStylePlatform";
        c.user_agent = "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034943-gold (unlike Gecko), Unknown_TV_Unknown_0/Unknown (Unknown, Unknown)";
        clients.push_back(c);
    }

    // 6. TV_SAMSUNG: Client ID 7, Version 5.20260707 (Tizen 2.4.0 SmartTV).
    // Samsung SmartTV client with direct playback URLs.
    {
        InnerTubeClient c;
        c.name = "TV_SAMSUNG";
        c.client_name = "TVHTML5";
        c.client_version = "5.20260707";
        c.client_id = "7";
        c.device_make = "Samsung";
        c.device_model = "SmartTV";
        c.os_name = "Tizen";
        c.os_version = "2.4.0";
        c.user_agent = "Mozilla/5.0 (SMART-TV; Linux; Tizen 2.4.0) AppleWebKit/538.1 (KHTML, like Gecko) Version/2.4.0 TV Safari/538.1";
        clients.push_back(c);
    }

    // 7. TVHTML5_SIMPLY: Client ID 75, Version 1.0 (PlayStation/Embedded TV).
    // Lightweight TV fallback client.
    {
        InnerTubeClient c;
        c.name = "TVHTML5_SIMPLY";
        c.client_name = "TVHTML5_SIMPLY";
        c.client_version = "1.0";
        c.client_id = "75";
        c.user_agent = "Mozilla/5.0 (PlayStation; PlayStation 4/12.00) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.4 Safari/605.1.15";
        c.embed_url = "https://www.youtube.com/";
        clients.push_back(c);
    }

    // 8. WEB_EMBEDDED_PLAYER: Client ID 56, Version 2.20260708.00.00.
    // Embedded desktop player context bypassing geo-restrictions.
    {
        InnerTubeClient c;
        c.name = "WEB_EMBEDDED_PLAYER";
        c.client_name = "WEB_EMBEDDED_PLAYER";
        c.client_version = "2.20260708.00.00";
        c.client_id = "56";
        c.user_agent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.5 Safari/605.1.15,gzip(gfe)";
        c.embed_url = "https://www.youtube.com";
        clients.push_back(c);
    }

    return clients;
}

bool YouTubeEngine::is_valid_stream_url(const std::string& url) {
    if (url.empty()) return false;
    if (url.find("sabr=1") != std::string::npos ||
        url.find("sabr=2") != std::string::npos) {
        return false;
    }
    // Reject c=WEB URLs with encrypted n= challenge
    if ((url.find("c=WEB") != std::string::npos || url.find("c=web") != std::string::npos) &&
        (url.find("?n=") != std::string::npos || url.find("&n=") != std::string::npos)) {
        return false;
    }
    return true;
}

void YouTubeEngine::parse_codecs_from_mime(
    const std::string& mime,
    std::string& out_container,
    std::string& out_vcodec,
    std::string& out_acodec) {

    out_container = "MP4";
    out_vcodec = "";
    out_acodec = "";

    if (mime.find("video/webm") != std::string::npos || mime.find("audio/webm") != std::string::npos) {
        out_container = "WEBM";
    } else if (mime.find("audio/mp4") != std::string::npos) {
        out_container = "M4A";
    } else if (mime.find("video/mp4") != std::string::npos) {
        out_container = "MP4";
    }

    // Extract codecs parameter: codecs="avc1.4d401f, mp4a.40.2"
    size_t c_pos = mime.find("codecs=\"");
    if (c_pos != std::string::npos) {
        size_t end_pos = mime.find("\"", c_pos + 8);
        if (end_pos != std::string::npos) {
            std::string codecs_str = mime.substr(c_pos + 8, end_pos - (c_pos + 8));
            size_t comma = codecs_str.find(",");
            if (comma != std::string::npos) {
                out_vcodec = codecs_str.substr(0, comma);
                out_acodec = codecs_str.substr(comma + 1);
                while (!out_acodec.empty() && out_acodec.front() == ' ') out_acodec.erase(0, 1);
            } else {
                if (mime.find("audio") != std::string::npos) {
                    out_acodec = codecs_str;
                } else {
                    out_vcodec = codecs_str;
                }
            }
        }
    }
}

std::string YouTubeEngine::get_or_fetch_visitor_data(const std::string& video_id) {
    {
        std::lock_guard<std::mutex> lock(s_visitor_mutex);
        if (!s_cached_visitor_data.empty()) {
            return s_cached_visitor_data;
        }
    }

    // Fetch lightweight InnerTube guide/sw.js_data or compressed watch page
    std::string url = "https://www.youtube.com/sw.js_data";
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string html_resp;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.5");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // Enable gzip/br decompression
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_string_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &html_resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    auto extract_vd = [](const std::string& body) -> std::string {
        static const std::regex vd_rx(R"lit((?:\"VISITOR_DATA\":\s*\"([^\"]+)\"|\"(Cgt[0-9A-Za-z_-]+)\"))lit");
        std::smatch m;
        if (std::regex_search(body, m, vd_rx)) {
            return (m[1].matched && !m[1].str().empty()) ? m[1].str() : m[2].str();
        }
        return "";
    };

    if (res == CURLE_OK && !html_resp.empty()) {
        std::string token = extract_vd(html_resp);
        if (!token.empty()) {
            std::lock_guard<std::mutex> lock(s_visitor_mutex);
            s_cached_visitor_data = token;
            return s_cached_visitor_data;
        }
    }

    // Fallback to compressed watch page if sw.js_data did not contain visitor data
    if (!video_id.empty()) {
        std::string watch_url = "https://www.youtube.com/watch?v=" + video_id;
        CURL* curl2 = curl_easy_init();
        if (curl2) {
            std::string watch_resp;
            struct curl_slist* h2 = nullptr;
            h2 = curl_slist_append(h2, "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
            h2 = curl_slist_append(h2, "Accept-Language: en-US,en;q=0.5");

            curl_easy_setopt(curl2, CURLOPT_URL, watch_url.c_str());
            curl_easy_setopt(curl2, CURLOPT_HTTPHEADER, h2);
            curl_easy_setopt(curl2, CURLOPT_ACCEPT_ENCODING, "");
            curl_easy_setopt(curl2, CURLOPT_WRITEFUNCTION, curl_string_write);
            curl_easy_setopt(curl2, CURLOPT_WRITEDATA, &watch_resp);
            curl_easy_setopt(curl2, CURLOPT_TIMEOUT, 5L);
            curl_easy_setopt(curl2, CURLOPT_CONNECTTIMEOUT, 3L);
            curl_easy_setopt(curl2, CURLOPT_FOLLOWLOCATION, 1L);

            CURLcode res2 = curl_easy_perform(curl2);
            curl_slist_free_all(h2);
            curl_easy_cleanup(curl2);

            if (res2 == CURLE_OK && !watch_resp.empty()) {
                std::string token = extract_vd(watch_resp);
                if (!token.empty()) {
                    std::lock_guard<std::mutex> lock(s_visitor_mutex);
                    s_cached_visitor_data = token;
                    return s_cached_visitor_data;
                }
            }
        }
    }

    return "";
}

void YouTubeEngine::invalidate_visitor_data() {
    std::lock_guard<std::mutex> lock(s_visitor_mutex);
    s_cached_visitor_data.clear();
}

bool YouTubeEngine::query_innertube_client(
    const InnerTubeClient& client,
    const std::string& video_id,
    const std::string& visitor_data,
    std::string& out_response_json,
    std::string* out_error) {

    CURL* curl = curl_easy_init();
    if (!curl) {
        if (out_error) *out_error = "Failed to initialize libcurl";
        return false;
    }

    // Build JSON context
    json req;
    req["videoId"] = video_id;

    json client_obj;
    client_obj["clientName"] = client.client_name;
    client_obj["clientVersion"] = client.client_version;
    client_obj["hl"] = "en";
    client_obj["gl"] = "US";
    client_obj["timeZone"] = "UTC";
    client_obj["utcOffsetMinutes"] = 0;

    if (!visitor_data.empty()) {
        client_obj["visitorData"] = visitor_data;
    }

    if (client.android_sdk_version > 0) client_obj["androidSdkVersion"] = client.android_sdk_version;
    if (!client.os_name.empty()) client_obj["osName"] = client.os_name;
    if (!client.os_version.empty()) client_obj["osVersion"] = client.os_version;
    if (!client.device_make.empty()) client_obj["deviceMake"] = client.device_make;
    if (!client.device_model.empty()) client_obj["deviceModel"] = client.device_model;
    if (!client.user_agent.empty()) client_obj["userAgent"] = client.user_agent;

    req["context"] = json::object();
    req["context"]["client"] = client_obj;

    if (!client.embed_url.empty()) {
        req["context"]["thirdParty"] = { {"embedUrl", client.embed_url} };
    }

    req["contentPlaybackContext"] = { {"html5Preference", "HTML5_PREF_WANTS"} };
    req["playbackContext"] = {
        {"contentPlaybackContext", {
            {"html5Preference", "HTML5_PREF_WANTS"}
        }}
    };
    req["racyCheckOk"] = true;

    std::string req_body = req.dump();
    std::string response_data;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string ua_hdr = "User-Agent: " + client.user_agent;
    headers = curl_slist_append(headers, ua_hdr.c_str());
    std::string client_name_hdr = "X-YouTube-Client-Name: " + client.client_id;
    headers = curl_slist_append(headers, client_name_hdr.c_str());
    std::string client_ver_hdr = "X-YouTube-Client-Version: " + client.client_version;
    headers = curl_slist_append(headers, client_ver_hdr.c_str());

    const char* api_url = "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";
    curl_easy_setopt(curl, CURLOPT_URL, api_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req_body.length()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // Enable gzip/br compression for InnerTube JSON
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_string_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        if (out_error) *out_error = "InnerTube request failed (HTTP " + std::to_string(http_code) + ", curl " + std::to_string(res) + ")";
        return false;
    }

    out_response_json = std::move(response_data);
    return true;
}

bool YouTubeEngine::probe_stream_range(
    const std::string& url,
    const std::string& user_agent,
    uint64_t content_length) {

    if (url.empty()) return false;

    // Check near EOF or 2MB past 60-second cap
    uint64_t range_start = content_length > 65536 ? (content_length - 4096) : 2097152;
    uint64_t range_end = content_length > 65536 ? (content_length - 1) : 2098175;

    auto do_probe = [&](uint64_t start, uint64_t end) -> long {
        CURL* curl = curl_easy_init();
        if (!curl) return 0;

        std::string range_hdr = std::to_string(start) + "-" + std::to_string(end);
        struct curl_slist* headers = nullptr;
        if (!user_agent.empty()) {
            std::string ua = "User-Agent: " + user_agent;
            headers = curl_slist_append(headers, ua.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_RANGE, range_hdr.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_discard_write);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        CURLcode res = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return (res == CURLE_OK) ? code : 0;
    };

    long code = do_probe(range_start, range_end);
    if (content_length <= 65536 && code == 416) {
        // Fallback to start for clips shorter than 2MB
        code = do_probe(0, 1023);
    }

    return (code == 200 || code == 206);
}

bool YouTubeEngine::parse_player_response(
    const std::string& json_str,
    const InnerTubeClient& client,
    const std::string& video_id,
    YouTubeVideoInfo& out_info,
    std::string* out_error) {

    json root;
    try {
        root = json::parse(json_str);
    } catch (const std::exception& e) {
        if (out_error) *out_error = std::string("JSON parse error: ") + e.what();
        return false;
    }

    // Verify playability status
    if (root.contains("playabilityStatus")) {
        std::string status = root["playabilityStatus"].value("status", "");
        if (status != "OK") {
            std::string reason = root["playabilityStatus"].value("reason", status);
            if (out_error) *out_error = "Playability status: " + status + " (" + reason + ")";
            return false;
        }
    }

    if (!root.contains("streamingData")) {
        if (out_error) *out_error = "Response contains no streamingData";
        return false;
    }

    out_info.video_id = video_id;
    out_info.client_used = client.name;

    // Parse videoDetails
    if (root.contains("videoDetails")) {
        const auto& vd = root["videoDetails"];
        out_info.title = vd.value("title", "");
        out_info.author = vd.value("author", "");
        std::string len_str = vd.value("lengthSeconds", "0");
        try { out_info.duration_seconds = std::stoull(len_str); } catch (...) {}

        if (vd.contains("thumbnail") && vd["thumbnail"].contains("thumbnails") && vd["thumbnail"]["thumbnails"].is_array()) {
            const auto& thumbs = vd["thumbnail"]["thumbnails"];
            if (!thumbs.empty()) {
                out_info.thumbnail_url = thumbs.back().value("url", "");
            }
        }
    }

    const auto& sd = root["streamingData"];

    auto parse_fmt = [&](const json& j, bool is_prog, bool is_aud) -> YouTubeFormat {
        YouTubeFormat f;
        f.itag = j.value("itag", 0);
        f.quality_label = j.value("qualityLabel", "");
        f.width = j.value("width", 0);
        f.height = j.value("height", 0);
        f.fps = j.value("fps", 0);
        f.mime_type = j.value("mimeType", "");
        f.bitrate = j.value("bitrate", 0ULL);
        f.url = j.value("url", "");
        f.user_agent = client.user_agent;
        f.is_progressive = is_prog;
        f.is_audio = is_aud;

        std::string clen_str = j.value("contentLength", "");
        if (!clen_str.empty()) {
            try { f.content_length = std::stoull(clen_str); } catch (...) {}
        }

        parse_codecs_from_mime(f.mime_type, f.container, f.vcodec, f.acodec);
        if (f.quality_label.empty() && f.height > 0) {
            f.quality_label = std::to_string(f.height) + "p";
        }
        return f;
    };

    int probe_attempts = 0;
    bool probe_ok = false;

    auto check_stream = [&](const YouTubeFormat& fmt) -> bool {
        if (probe_ok) return true;
        if (probe_attempts < 2) {
            probe_attempts++;
            if (probe_stream_range(fmt.url, fmt.user_agent, fmt.content_length)) {
                probe_ok = true;
                return true;
            }
        }
        return false;
    };

    // 1. Progressive Formats
    if (sd.contains("formats") && sd["formats"].is_array()) {
        for (const auto& item : sd["formats"]) {
            std::string u = item.value("url", "");
            if (!is_valid_stream_url(u)) continue;

            YouTubeFormat fmt = parse_fmt(item, true, false);
            // Verify stream with range probe
            if (check_stream(fmt)) {
                out_info.progressive_formats.push_back(fmt);
            }
        }
    }

    // 2. Adaptive Formats
    if (sd.contains("adaptiveFormats") && sd["adaptiveFormats"].is_array()) {
        for (const auto& item : sd["adaptiveFormats"]) {
            std::string u = item.value("url", "");
            if (!is_valid_stream_url(u)) continue;

            std::string mime = item.value("mimeType", "");
            bool is_aud = (mime.find("audio") != std::string::npos);

            YouTubeFormat fmt = parse_fmt(item, false, is_aud);
            // Verify stream with range probe
            if (check_stream(fmt)) {
                if (is_aud) {
                    out_info.audio_formats.push_back(fmt);
                } else {
                    out_info.video_formats.push_back(fmt);
                }
            }
        }
    }

    if (out_info.progressive_formats.empty() && out_info.video_formats.empty() && out_info.audio_formats.empty()) {
        if (out_error) *out_error = "Client " + client.name + " returned formats but none passed stream range verification";
        return false;
    }

    // Sort progressive formats by height descending
    std::sort(out_info.progressive_formats.begin(), out_info.progressive_formats.end(),
        [](const YouTubeFormat& a, const YouTubeFormat& b) {
            if (a.height != b.height) return a.height > b.height;
            return a.bitrate > b.bitrate;
        });

    // Sort video formats by height descending, then fps, then bitrate
    std::sort(out_info.video_formats.begin(), out_info.video_formats.end(),
        [](const YouTubeFormat& a, const YouTubeFormat& b) {
            if (a.height != b.height) return a.height > b.height;
            if (a.fps != b.fps) return a.fps > b.fps;
            return a.bitrate > b.bitrate;
        });

    // Sort audio formats: prioritize AAC (itag 140) or highest bitrate
    std::sort(out_info.audio_formats.begin(), out_info.audio_formats.end(),
        [](const YouTubeFormat& a, const YouTubeFormat& b) {
            int a_score = (a.itag == 140 || a.container == "M4A") ? 10 : 0;
            int b_score = (b.itag == 140 || b.container == "M4A") ? 10 : 0;
            if (a_score != b_score) return a_score > b_score;
            return a.bitrate > b.bitrate;
        });

    if (!out_info.progressive_formats.empty()) {
        out_info.best_progressive = out_info.progressive_formats.front();
    }
    if (!out_info.video_formats.empty()) {
        out_info.best_video = out_info.video_formats.front();
    }
    if (!out_info.audio_formats.empty()) {
        out_info.best_audio = out_info.audio_formats.front();
    }

    return true;
}

bool YouTubeEngine::extract_video_info(
    const std::string& url_or_id,
    YouTubeVideoInfo& out_info,
    std::string* out_error) {

    std::string vid = extract_video_id(url_or_id);
    if (vid.empty()) {
        if (out_error) *out_error = "Could not parse YouTube video ID from URL: " + url_or_id;
        return false;
    }

    std::vector<InnerTubeClient> clients = get_client_hierarchy();
    std::string primary_error;
    std::string last_error;

    std::string visitor_data;
    {
        std::lock_guard<std::mutex> lock(s_visitor_mutex);
        visitor_data = s_cached_visitor_data;
    }

    YouTubeVideoInfo merged_info;
    std::unordered_set<int> seen_prog_itags;
    std::unordered_set<int> seen_video_itags;
    std::unordered_set<int> seen_audio_itags;
    std::vector<std::string> clients_used;

    auto run_extraction_pass = [&](const std::string& v_data) -> bool {
        merged_info = YouTubeVideoInfo();
        merged_info.video_id = vid;
        seen_prog_itags.clear();
        seen_video_itags.clear();
        seen_audio_itags.clear();
        clients_used.clear();

        for (const auto& client : clients) {
            std::string resp_json;
            std::string err;
            if (!query_innertube_client(client, vid, v_data, resp_json, &err)) {
                last_error = client.name + ": " + err;
                if (primary_error.empty()) primary_error = last_error;
                continue;
            }

            YouTubeVideoInfo client_info;
            if (!parse_player_response(resp_json, client, vid, client_info, &err)) {
                last_error = client.name + ": " + err;
                if (primary_error.empty()) primary_error = last_error;
                continue;
            }

            if (merged_info.title.empty()) merged_info.title = client_info.title;
            if (merged_info.author.empty()) merged_info.author = client_info.author;
            if (merged_info.thumbnail_url.empty()) merged_info.thumbnail_url = client_info.thumbnail_url;
            if (merged_info.duration_seconds == 0) merged_info.duration_seconds = client_info.duration_seconds;

            bool added_any = false;

            // Merge progressive formats
            for (const auto& fmt : client_info.progressive_formats) {
                if (seen_prog_itags.find(fmt.itag) == seen_prog_itags.end()) {
                    seen_prog_itags.insert(fmt.itag);
                    merged_info.progressive_formats.push_back(fmt);
                    added_any = true;
                }
            }

            // Merge adaptive video formats
            for (const auto& fmt : client_info.video_formats) {
                if (seen_video_itags.find(fmt.itag) == seen_video_itags.end()) {
                    seen_video_itags.insert(fmt.itag);
                    merged_info.video_formats.push_back(fmt);
                    added_any = true;
                }
            }

            // Merge audio formats
            for (const auto& fmt : client_info.audio_formats) {
                if (seen_audio_itags.find(fmt.itag) == seen_audio_itags.end()) {
                    seen_audio_itags.insert(fmt.itag);
                    merged_info.audio_formats.push_back(fmt);
                    added_any = true;
                }
            }

            if (added_any) {
                clients_used.push_back(client.name);
            }

            // Break immediately once HD adaptive video (>= 720p) and audio formats are retrieved
            bool has_hd = false;
            for (const auto& vf : merged_info.video_formats) {
                if (vf.height >= 720) {
                    has_hd = true;
                    break;
                }
            }

            if (has_hd && !merged_info.audio_formats.empty()) {
                break;
            }
        }
        return merged_info.has_playable_formats();
    };

    bool pass_ok = run_extraction_pass(visitor_data);
    if (!pass_ok) {
        // If initial pass failed, invalidate cached visitor data and re-fetch freshly
        invalidate_visitor_data();
        std::string fresh_vd = get_or_fetch_visitor_data(vid);
        if (!fresh_vd.empty() && fresh_vd != visitor_data) {
            pass_ok = run_extraction_pass(fresh_vd);
        }
    }

    if (!merged_info.has_playable_formats()) {
        if (out_error) {
            if (primary_error == last_error || primary_error.empty()) {
                *out_error = "Failed to extract video using all fallback clients (" + last_error + ")";
            } else {
                *out_error = "Failed to extract video using all fallback clients (" + primary_error + "; " + last_error + ")";
            }
        }
        return false;
    }

    // Set client_used summary
    std::string client_summary;
    for (size_t i = 0; i < clients_used.size(); ++i) {
        if (i > 0) client_summary += "+";
        client_summary += clients_used[i];
    }
    merged_info.client_used = client_summary;

    // Sort progressive formats by height descending
    std::sort(merged_info.progressive_formats.begin(), merged_info.progressive_formats.end(),
        [](const YouTubeFormat& a, const YouTubeFormat& b) {
            if (a.height != b.height) return a.height > b.height;
            return a.bitrate > b.bitrate;
        });

    // Sort video formats by height descending, then fps, then bitrate
    std::sort(merged_info.video_formats.begin(), merged_info.video_formats.end(),
        [](const YouTubeFormat& a, const YouTubeFormat& b) {
            if (a.height != b.height) return a.height > b.height;
            if (a.fps != b.fps) return a.fps > b.fps;
            return a.bitrate > b.bitrate;
        });

    // Sort audio formats: prioritize AAC (itag 140) or highest bitrate
    std::sort(merged_info.audio_formats.begin(), merged_info.audio_formats.end(),
        [](const YouTubeFormat& a, const YouTubeFormat& b) {
            int a_score = (a.itag == 140 || a.container == "M4A") ? 10 : 0;
            int b_score = (b.itag == 140 || b.container == "M4A") ? 10 : 0;
            if (a_score != b_score) return a_score > b_score;
            return a.bitrate > b.bitrate;
        });

    if (!merged_info.progressive_formats.empty()) {
        merged_info.best_progressive = merged_info.progressive_formats.front();
    }
    if (!merged_info.video_formats.empty()) {
        merged_info.best_video = merged_info.video_formats.front();
    }
    if (!merged_info.audio_formats.empty()) {
        merged_info.best_audio = merged_info.audio_formats.front();
    }

    out_info = std::move(merged_info);
    return true;
}

std::string YouTubeEngine::format_duration(uint64_t seconds) {
    uint64_t hrs = seconds / 3600;
    uint64_t mins = (seconds % 3600) / 60;
    uint64_t secs = seconds % 60;

    std::ostringstream ss;
    if (hrs > 0) {
        ss << hrs << ":" << std::setfill('0') << std::setw(2) << mins << ":" << std::setfill('0') << std::setw(2) << secs;
    } else {
        ss << std::setfill('0') << std::setw(2) << mins << ":" << std::setfill('0') << std::setw(2) << secs;
    }
    return ss.str();
}

std::string YouTubeEngine::format_bitrate(uint64_t bps) {
    if (bps >= 1000000) {
        double mbps = static_cast<double>(bps) / 1000000.0;
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << mbps << " Mbps";
        return ss.str();
    } else {
        uint64_t kbps = bps / 1000;
        return std::to_string(kbps) + " kbps";
    }
}

} // namespace opendm
