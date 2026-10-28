#include "core/stream_extractor.hpp"
#include <iostream>
#include <cassert>
#include <future>

using namespace opendm;

void test_streaming_url_patterns() {
    std::cout << "[Test 1] Testing is_streaming_url pattern matching..." << std::endl;
    assert(StreamExtractor::is_streaming_url("https://www.youtube.com/watch?v=jNQXAC9IVRw"));
    assert(StreamExtractor::is_streaming_url("https://youtu.be/jNQXAC9IVRw"));
    assert(StreamExtractor::is_streaming_url("https://vimeo.com/12345678"));
    assert(StreamExtractor::is_streaming_url("https://www.twitch.tv/videos/123456789"));
    assert(StreamExtractor::is_streaming_url("https://www.tiktok.com/@user/video/123456789"));
    assert(StreamExtractor::is_streaming_url("https://twitter.com/user/status/123456789"));
    assert(StreamExtractor::is_streaming_url("https://x.com/user/status/123456789"));
    assert(StreamExtractor::is_streaming_url("https://www.facebook.com/watch/?v=123456789"));
    assert(StreamExtractor::is_streaming_url("https://www.reddit.com/r/videos/comments/xyz/test/"));

    assert(!StreamExtractor::is_streaming_url("https://example.com/file.zip"));
    assert(!StreamExtractor::is_streaming_url("https://download.mozilla.org/firefox.tar.bz2"));
    assert(!StreamExtractor::is_streaming_url("http://kernel.org/pub/linux/kernel/v6.x/linux-6.1.tar.xz"));
    assert(!StreamExtractor::is_streaming_url(""));
    std::cout << "  -> PASSED" << std::endl;
}

void test_sanitize_filename() {
    std::cout << "[Test 2] Testing filename sanitization..." << std::endl;
    std::string safe = StreamExtractor::sanitize_filename("Great: Video / Clip * 1080p? <Final>", "mp4");
    assert(safe == "Great_ Video _ Clip _ 1080p_ _Final_.mp4");

    std::string safe_empty = StreamExtractor::sanitize_filename("   ", "mp4");
    assert(safe_empty == "video.mp4");

    std::string safe_custom = StreamExtractor::sanitize_filename("Sample.Audio", "m4a");
    assert(safe_custom == "Sample.Audio.m4a");
    std::cout << "  -> PASSED" << std::endl;
}

void test_extractor_detection() {
    std::cout << "[Test 3] Testing extractor binary detection..." << std::endl;
    bool avail = StreamExtractor::is_available();
    std::string bin = StreamExtractor::get_extractor_binary();
    std::cout << "  -> Extractor binary: " << bin << " (Available: " << (avail ? "true" : "false") << ")" << std::endl;
    assert(avail);
    assert(!bin.empty());
    std::cout << "  -> PASSED" << std::endl;
}

void test_extract_info_live() {
    std::cout << "[Test 4] Testing live video format extraction..." << std::endl;
    StreamMediaInfo info;
    std::string err;
    std::string test_url = "https://www.youtube.com/watch?v=jNQXAC9IVRw";

    bool ok = StreamExtractor::extract_info(test_url, info, &err);
    if (!ok) {
        std::cerr << "Extraction failed: " << err << std::endl;
    }
    assert(ok);
    assert(info.title == "Me at the zoo");
    assert(!info.formats.empty());

    bool found_video = false;
    bool found_audio = false;
    for (const auto& fmt : info.formats) {
        if (!fmt.is_audio_only) {
            found_video = true;
            assert(!fmt.video_url.empty());
        } else {
            found_audio = true;
            assert(!fmt.video_url.empty());
        }
    }
    assert(found_video);
    assert(found_audio);
    std::cout << "  -> Extracted title: \"" << info.title << "\", total formats: " << info.formats.size() << std::endl;
    std::cout << "  -> PASSED" << std::endl;
}

void test_extract_info_async() {
    std::cout << "[Test 5] Testing asynchronous extraction..." << std::endl;
    std::promise<bool> prom;
    auto fut = prom.get_future();

    StreamExtractor::extract_info_async("https://www.youtube.com/watch?v=jNQXAC9IVRw",
        [&prom](bool ok, const StreamMediaInfo& info, const std::string&) {
            prom.set_value(ok && !info.formats.empty());
        });

    assert(fut.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
    assert(fut.get() == true);
    std::cout << "  -> PASSED" << std::endl;
}

int main() {
    std::cout << "=== Running StreamExtractor Unit Tests ===" << std::endl;
    test_streaming_url_patterns();
    test_sanitize_filename();
    test_extractor_detection();
    test_extract_info_live();
    test_extract_info_async();
    std::cout << "=== All StreamExtractor Tests Passed 100% ===" << std::endl;
    return 0;
}
