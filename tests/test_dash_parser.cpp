#include "core/dash_parser.hpp"
#include "core/post_download.hpp"
#include <iostream>
#include <cassert>

int main() {
    std::cout << "=== Running OpenDM DASH Parser & Post-Download Tests ===" << std::endl;

    // 1. Synthetic MPEG-DASH MPD XML
    std::string sample_mpd = R"(<?xml version="1.0" encoding="utf-8"?>
<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" minBufferTime="PT1.5S" type="static" mediaPresentationDuration="PT0H5M12.00S">
  <BaseURL>https://media.example.com/dash/</BaseURL>
  <Period>
    <AdaptationSet id="0" contentType="video" mimeType="video/mp4">
      <Representation id="vid_720p" bandwidth="2500000" width="1280" height="720" codecs="avc1.4d401f">
        <BaseURL>video_720.mp4</BaseURL>
      </Representation>
      <Representation id="vid_1080p" bandwidth="4500000" width="1920" height="1080" codecs="avc1.640028">
        <BaseURL>video_1080.mp4</BaseURL>
      </Representation>
      <Representation id="vid_480p" bandwidth="1000000" width="854" height="480" codecs="avc1.4d401e">
        <BaseURL>video_480.mp4</BaseURL>
      </Representation>
    </AdaptationSet>
    <AdaptationSet id="1" contentType="audio" mimeType="audio/mp4">
      <Representation id="audio_low" bandwidth="64000" codecs="mp4a.40.2">
        <BaseURL>audio_64k.m4a</BaseURL>
      </Representation>
      <Representation id="audio_high" bandwidth="192000" codecs="mp4a.40.2">
        <BaseURL>audio_192k.m4a</BaseURL>
      </Representation>
    </AdaptationSet>
  </Period>
</MPD>)";

    std::cout << "[Test 1] Parsing MPEG-DASH MPD XML Manifest..." << std::endl;
    opendm::DashManifest manifest;
    std::string err;
    bool ok = opendm::DashParser::parse_mpd_xml(sample_mpd, "https://media.example.com/dash/manifest.mpd", manifest, &err);
    if (!ok) {
        std::cerr << "DASH parse error: " << err << std::endl;
        return 1;
    }

    assert(ok);
    assert(manifest.media_presentation_duration == "PT0H5M12.00S");
    assert(manifest.video_streams.size() == 3);
    assert(manifest.audio_streams.size() == 2);
    std::cout << "  ✓ Manifest parsed 3 video streams and 2 audio streams." << std::endl;

    // Verify stream selection
    const auto* best_video = opendm::DashParser::select_best_video(manifest);
    assert(best_video != nullptr);
    assert(best_video->id == "vid_1080p");
    assert(best_video->width == 1920);
    assert(best_video->height == 1080);
    assert(best_video->base_url == "https://media.example.com/dash/video_1080.mp4");
    std::cout << "  ✓ Selected best video representation (1080p @ 4.5 Mbps): " << best_video->base_url << std::endl;

    const auto* best_audio = opendm::DashParser::select_best_audio(manifest);
    assert(best_audio != nullptr);
    assert(best_audio->id == "audio_high");
    assert(best_audio->bandwidth == 192000);
    assert(best_audio->base_url == "https://media.example.com/dash/audio_192k.m4a");
    std::cout << "  ✓ Selected best audio representation (192 kbps): " << best_audio->base_url << std::endl;

    // 2. Test PostDownloadManager notification
    std::cout << "[Test 2] Testing PostDownloadManager desktop notification..." << std::endl;
    opendm::PostDownloadManager::instance().notify_completion("test_video.mp4", 104857600, true);
    std::cout << "  ✓ Desktop notification sent asynchronously." << std::endl;

    std::cout << "All DASH Parser and Post-Download tests passed!" << std::endl;
    return 0;
}
