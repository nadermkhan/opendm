#include "core/youtube_engine.hpp"
#include <iostream>
#include <cassert>

int main() {
    std::cout << "=== Running OpenDM Native YouTube Processing Engine Tests ===" << std::endl;

    // 1. Test URL recognition and Video ID extraction
    std::cout << "[Test 1] Testing YouTube URL pattern recognition and Video ID extraction..." << std::endl;
    {
        const std::string id = "83lRs8jmKpo";
        std::vector<std::string> valid_urls = {
            "https://www.youtube.com/watch?v=83lRs8jmKpo",
            "https://youtube.com/watch?v=83lRs8jmKpo&feature=shared",
            "https://youtu.be/83lRs8jmKpo",
            "https://youtu.be/83lRs8jmKpo?t=42",
            "https://www.youtube.com/shorts/83lRs8jmKpo",
            "https://www.youtube.com/embed/83lRs8jmKpo",
            "https://www.youtube.com/v/83lRs8jmKpo",
            "https://www.youtube.com/live/83lRs8jmKpo",
            "83lRs8jmKpo"
        };

        for (const auto& u : valid_urls) {
            assert(opendm::YouTubeEngine::is_youtube_url(u));
            std::string extracted = opendm::YouTubeEngine::extract_video_id(u);
            assert(extracted == id);
        }

        // Invalid URLs
        assert(!opendm::YouTubeEngine::is_youtube_url("https://example.com/video.mp4"));
        assert(!opendm::YouTubeEngine::is_youtube_url("https://vimeo.com/123456789"));
        assert(opendm::YouTubeEngine::extract_video_id("https://example.com/").empty());
        std::cout << "  Passed URL recognition across watch, youtu.be, shorts, embed, live, and raw ID." << std::endl;
    }

    // 2. Test Format Helper functions
    std::cout << "[Test 2] Testing duration and bitrate formatting..." << std::endl;
    {
        assert(opendm::YouTubeEngine::format_duration(65) == "01:05");
        assert(opendm::YouTubeEngine::format_duration(3665) == "1:01:05");
        assert(opendm::YouTubeEngine::format_bitrate(2500000) == "2.5 Mbps");
        assert(opendm::YouTubeEngine::format_bitrate(128000) == "128 kbps");
        std::cout << "  Passed duration and bitrate formatting." << std::endl;
    }

    // 3. Test Live Video Extraction following yt-dlp's workflow
    std::cout << "[Test 3] Testing live InnerTube extraction on video 83lRs8jmKpo..." << std::endl;
    {
        opendm::YouTubeVideoInfo info;
        std::string err;
        bool ok = opendm::YouTubeEngine::extract_video_info("https://www.youtube.com/watch?v=83lRs8jmKpo", info, &err);
        if (!ok) {
            std::cerr << "Extraction failed: " << err << std::endl;
            return 1;
        }

        assert(ok);
        assert(!info.title.empty());
        assert(info.duration_seconds > 0);
        assert(!info.client_used.empty());
        assert(info.has_playable_formats());

        std::cout << "  Title: " << info.title << std::endl;
        std::cout << "  Author: " << info.author << std::endl;
        std::cout << "  Duration: " << opendm::YouTubeEngine::format_duration(info.duration_seconds) << std::endl;
        std::cout << "  Client used: " << info.client_used << std::endl;
        std::cout << "  Progressive formats: " << info.progressive_formats.size() << std::endl;
        std::cout << "  Adaptive video formats: " << info.video_formats.size() << std::endl;
        std::cout << "  Adaptive audio formats: " << info.audio_formats.size() << std::endl;

        if (!info.progressive_formats.empty()) {
            const auto& prog = info.progressive_formats.front();
            assert(prog.itag == 18);
            assert(!prog.url.empty());
            assert(prog.url.find("sabr=1") == std::string::npos);
            std::cout << "  Best progressive format: itag " << prog.itag << " (" << prog.quality_label << ", " << prog.container << ")" << std::endl;
        }

        if (!info.video_formats.empty() && !info.audio_formats.empty()) {
            std::cout << "  Best adaptive video: itag " << info.best_video.itag << " (" << info.best_video.quality_label << ", " << info.best_video.container << ")" << std::endl;
            std::cout << "  Best adaptive audio: itag " << info.best_audio.itag << " (" << info.best_audio.container << ", " << opendm::YouTubeEngine::format_bitrate(info.best_audio.bitrate) << ")" << std::endl;
        }
    }

    std::cout << "=== All OpenDM Native YouTube Processing Engine Tests Passed 100% ===" << std::endl;
    return 0;
}
