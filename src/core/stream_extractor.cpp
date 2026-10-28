#include "core/stream_extractor.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <thread>
#include <filesystem>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace opendm {

std::string StreamExtractor::get_extractor_binary() {
    const char* home = getenv("HOME");
    if (home) {
        std::string local_bin = std::string(home) + "/.local/bin/yt-dlp";
        if (access(local_bin.c_str(), X_OK) == 0) {
            return local_bin;
        }
    }

    const char* standard_paths[] = {
        "/usr/local/bin/yt-dlp",
        "/usr/bin/yt-dlp",
        "/bin/yt-dlp"
    };
    for (const char* p : standard_paths) {
        if (access(p, X_OK) == 0) {
            return p;
        }
    }

    // Check PATH via which
    FILE* fp = popen("which yt-dlp 2>/dev/null", "r");
    if (fp) {
        char buf[512];
        if (fgets(buf, sizeof(buf), fp)) {
            std::string res(buf);
            while (!res.empty() && (res.back() == '\n' || res.back() == '\r' || res.back() == ' ')) {
                res.pop_back();
            }
            pclose(fp);
            if (!res.empty() && access(res.c_str(), X_OK) == 0) {
                return res;
            }
        } else {
            pclose(fp);
        }
    }

    return "";
}

std::string StreamExtractor::get_node_binary() {
    const char* home = getenv("HOME");
    if (home) {
        std::string local_node = std::string(home) + "/.local/bin/node";
        if (access(local_node.c_str(), X_OK) == 0) return local_node;

        std::string nvm_dir = std::string(home) + "/.nvm/versions/node";
        std::error_code ec;
        if (fs::exists(nvm_dir, ec)) {
            for (const auto& entry : fs::directory_iterator(nvm_dir, ec)) {
                std::string candidate = (entry.path() / "bin" / "node").string();
                if (access(candidate.c_str(), X_OK) == 0) return candidate;
            }
        }
    }

    const char* standard_paths[] = {
        "/usr/local/bin/node",
        "/usr/bin/node",
        "/bin/node"
    };
    for (const char* p : standard_paths) {
        if (access(p, X_OK) == 0) return p;
    }

    FILE* fp = popen("which node 2>/dev/null", "r");
    if (fp) {
        char buf[512];
        if (fgets(buf, sizeof(buf), fp)) {
            std::string res(buf);
            while (!res.empty() && (res.back() == '\n' || res.back() == '\r' || res.back() == ' ')) res.pop_back();
            pclose(fp);
            if (!res.empty() && access(res.c_str(), X_OK) == 0) return res;
        } else {
            pclose(fp);
        }
    }
    return "";
}

bool StreamExtractor::is_available() {
    return !get_extractor_binary().empty();
}

bool StreamExtractor::is_streaming_url(const std::string& url) {
    if (url.empty()) return false;

    // Convert to lowercase for checking
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    const char* streaming_patterns[] = {
        "youtube.com/",
        "youtu.be/",
        "vimeo.com/",
        "dailymotion.com/",
        "twitch.tv/",
        "tiktok.com/",
        "twitter.com/",
        "x.com/",
        "facebook.com/watch",
        "facebook.com/reel",
        "instagram.com/reel",
        "instagram.com/p/",
        "reddit.com/r/"
    };

    for (const char* pat : streaming_patterns) {
        if (lower.find(pat) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string StreamExtractor::sanitize_filename(const std::string& title, const std::string& ext) {
    std::string safe = title;
    for (char& c : safe) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c < 32) {
            c = '_';
        }
    }
    while (!safe.empty() && (safe.front() == ' ' || safe.front() == '.')) {
        safe.erase(0, 1);
    }
    while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.')) {
        safe.pop_back();
    }
    if (safe.empty()) {
        safe = "video";
    }
    if (!ext.empty()) {
        safe += "." + ext;
    }
    return safe;
}

bool StreamExtractor::extract_info(
    const std::string& url,
    StreamMediaInfo& out_info,
    std::string* out_error) {

    std::string bin = get_extractor_binary();
    if (bin.empty()) {
        if (out_error) *out_error = "yt-dlp executable not found on system";
        return false;
    }

    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        if (out_error) *out_error = "Failed to create IPC pipe for extractor process";
        return false;
    }

    std::string node_bin = get_node_binary();

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        if (out_error) *out_error = "Failed to fork extractor process";
        return false;
    }

    if (pid == 0) {
        // Child process
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        close(pipe_fd[1]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        if (!node_bin.empty()) {
            std::string js_arg = "node:" + node_bin;
            execlp(bin.c_str(), bin.c_str(), "--js-runtimes", js_arg.c_str(), "-J",
                   "--no-playlist", "--no-warnings", url.c_str(), nullptr);
        } else {
            execlp(bin.c_str(), bin.c_str(), "-J",
                   "--no-playlist", "--no-warnings", url.c_str(), nullptr);
        }
        _exit(127);
    }

    // Parent process
    close(pipe_fd[1]);

    std::string json_output;
    char buffer[4096];
    ssize_t bytes_read;
    while ((bytes_read = read(pipe_fd[0], buffer, sizeof(buffer))) > 0) {
        json_output.append(buffer, bytes_read);
    }
    close(pipe_fd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (json_output.empty()) {
        if (out_error) *out_error = "Extractor produced no output";
        return false;
    }

    try {
        json data = json::parse(json_output);

        out_info.title = data.value("title", "video");
        out_info.uploader = data.value("uploader", "");
        out_info.webpage_url = data.value("webpage_url", url);
        out_info.thumbnail_url = data.value("thumbnail", "");
        out_info.duration_seconds = data.value("duration", 0.0);

        if (!data.contains("formats") || !data["formats"].is_array()) {
            if (out_error) *out_error = "No formats found in stream response";
            return false;
        }

        // 1. Identify best audio stream
        struct AudioCandidate {
            std::string format_id;
            std::string url;
            std::string ext;
            uint64_t filesize{0};
            int tbr{0};
        };
        AudioCandidate best_m4a;
        AudioCandidate best_audio;

        for (const auto& f : data["formats"]) {
            std::string stream_url = f.value("url", "");
            if (stream_url.empty()) continue;

            std::string proto = f.value("protocol", "");
            if (proto.find("m3u8") != std::string::npos || stream_url.find(".m3u8") != std::string::npos) {
                continue;
            }

            std::string vcodec = f.value("vcodec", "none");
            std::string acodec = f.value("acodec", "none");
            std::string ext = f.value("ext", "");

            if (vcodec == "none" && acodec != "none") {
                uint64_t sz = f.value("filesize", 0ULL);
                if (sz == 0) sz = f.value("filesize_approx", 0ULL);
                int tbr = f.value("tbr", 0);

                if (ext == "m4a" && (tbr > best_m4a.tbr || best_m4a.url.empty())) {
                    best_m4a = {f.value("format_id", ""), stream_url, ext, sz, tbr};
                }
                if (tbr > best_audio.tbr || best_audio.url.empty()) {
                    best_audio = {f.value("format_id", ""), stream_url, ext, sz, tbr};
                }
            }
        }

        // Primary audio stream for muxing into MP4
        AudioCandidate selected_audio = !best_m4a.url.empty() ? best_m4a : best_audio;

        // 2. Collect video streams and pair with audio
        struct HeightGroup {
            int height{0};
            json best_fmt;
        };
        std::vector<HeightGroup> height_groups;

        for (const auto& f : data["formats"]) {
            std::string stream_url = f.value("url", "");
            if (stream_url.empty()) continue;

            std::string proto = f.value("protocol", "");
            if (proto.find("m3u8") != std::string::npos || stream_url.find(".m3u8") != std::string::npos) {
                continue;
            }

            std::string vcodec = f.value("vcodec", "none");
            if (vcodec == "none") continue;

            int height = f.value("height", 0);
            if (height <= 0) continue;

            auto it = std::find_if(height_groups.begin(), height_groups.end(),
                                   [height](const HeightGroup& g) { return g.height == height; });
            if (it == height_groups.end()) {
                height_groups.push_back({height, f});
            } else {
                // Prefer MP4 container or higher bitrate
                std::string current_ext = it->best_fmt.value("ext", "");
                std::string new_ext = f.value("ext", "");
                int current_tbr = it->best_fmt.value("tbr", 0);
                int new_tbr = f.value("tbr", 0);

                if ((new_ext == "mp4" && current_ext != "mp4") ||
                    (new_ext == current_ext && new_tbr > current_tbr)) {
                    it->best_fmt = f;
                }
            }
        }

        // Sort video heights descending (e.g. 2160, 1440, 1080, 720, 480, 360)
        std::sort(height_groups.begin(), height_groups.end(),
                  [](const HeightGroup& a, const HeightGroup& b) { return a.height > b.height; });

        for (const auto& g : height_groups) {
            const auto& f = g.best_fmt;
            StreamFormat fmt;
            fmt.format_id = f.value("format_id", "");
            fmt.width = f.value("width", 0);
            fmt.height = g.height;
            fmt.fps = f.value("fps", 30);
            fmt.vcodec = f.value("vcodec", "");
            fmt.acodec = f.value("acodec", "none");
            fmt.ext = "mp4";
            fmt.video_url = f.value("url", "");
            fmt.is_audio_only = false;

            uint64_t v_size = f.value("filesize", 0ULL);
            if (v_size == 0) v_size = f.value("filesize_approx", 0ULL);

            if (fmt.acodec == "none" && !selected_audio.url.empty()) {
                fmt.audio_url = selected_audio.url;
                fmt.requires_muxing = true;
                fmt.filesize = v_size + selected_audio.filesize;
                fmt.label = std::to_string(g.height) + "p MP4 (Video + Audio)";
            } else {
                fmt.audio_url = "";
                fmt.requires_muxing = false;
                fmt.filesize = v_size;
                fmt.label = std::to_string(g.height) + "p MP4";
            }

            out_info.formats.push_back(fmt);
        }

        // 3. Add audio-only option
        if (!selected_audio.url.empty()) {
            StreamFormat afmt;
            afmt.format_id = selected_audio.format_id;
            afmt.label = "Audio Only (" + selected_audio.ext + ")";
            afmt.ext = selected_audio.ext;
            afmt.filesize = selected_audio.filesize;
            afmt.video_url = selected_audio.url;
            afmt.audio_url = "";
            afmt.vcodec = "none";
            afmt.acodec = "audio";
            afmt.is_audio_only = true;
            afmt.requires_muxing = false;
            out_info.formats.push_back(afmt);
        }

        return !out_info.formats.empty();

    } catch (const std::exception& ex) {
        if (out_error) *out_error = std::string("JSON parse error: ") + ex.what();
        return false;
    }
}

void StreamExtractor::extract_info_async(
    const std::string& url,
    std::function<void(bool success, const StreamMediaInfo& info, const std::string& error)> callback) {

    std::thread([url, callback]() {
        StreamMediaInfo info;
        std::string err;
        bool ok = extract_info(url, info, &err);
        if (callback) {
            callback(ok, info, err);
        }
    }).detach();
}

} // namespace opendm
