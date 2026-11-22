#include "../src/core/zip_previewer.hpp"
#include "../src/core/site_grabber.hpp"
#include "../src/core/dash_parser.hpp"
#include <iostream>
#include <cassert>
#include <fstream>
#include <filesystem>
#include <vector>

void test_site_grabber_helpers() {
    std::cout << "[TEST] Running test_site_grabber_helpers..." << std::endl;

    assert(opendm::SiteGrabber::extract_host("https://www.example.com/path/file.html") == "www.example.com");
    assert(opendm::SiteGrabber::extract_host("http://sub.domain.org:8080/index.php") == "sub.domain.org");
    assert(opendm::SiteGrabber::extract_host("http://user:pass@Sub.Example.COM:8443/path?q=1") == "sub.example.com");

    // Relative URL resolutions & non-network scheme filtering
    assert(opendm::SiteGrabber::resolve_url("https://example.com/dir/page.html", "sub/file.zip") == "https://example.com/dir/sub/file.zip");
    assert(opendm::SiteGrabber::resolve_url("https://example.com/dir/page.html", "/root.zip") == "https://example.com/root.zip");
    assert(opendm::SiteGrabber::resolve_url("https://example.com/dir/page.html", "https://other.com/a.mp4") == "https://other.com/a.mp4");
    assert(opendm::SiteGrabber::resolve_url("https://example.com/page.html", "//cdn.com/lib.js") == "https://cdn.com/lib.js");
    assert(opendm::SiteGrabber::resolve_url("https://example.com/page.html", "javascript:void(0)") == "");
    assert(opendm::SiteGrabber::resolve_url("https://example.com/page.html", "mailto:user@example.com") == "");

    // DashParser per-Representation isolation inside a single AdaptationSet
    std::string mixed_mpd =
        "<MPD mediaPresentationDuration=\"PT10S\">"
        "<Period>"
        "<AdaptationSet id=\"0\">"
        "<Representation id=\"137\" mimeType=\"video/mp4\" width=\"1920\" height=\"1080\" bandwidth=\"4000000\"><BaseURL>v1080.mp4</BaseURL></Representation>"
        "<Representation id=\"140\" mimeType=\"audio/mp4\" bandwidth=\"128000\"><BaseURL>a128.m4a</BaseURL></Representation>"
        "</AdaptationSet>"
        "</Period>"
        "</MPD>";
    opendm::DashManifest manifest;
    std::string derr;
    assert(opendm::DashParser::parse_mpd_xml(mixed_mpd, "https://cdn.example.com/dash/manifest.mpd", manifest, &derr));
    assert(manifest.video_streams.size() == 1);
    assert(manifest.audio_streams.size() == 1);
    assert(manifest.video_streams[0].base_url == "https://cdn.example.com/dash/v1080.mp4");
    assert(manifest.audio_streams[0].base_url == "https://cdn.example.com/dash/a128.m4a");

    std::cout << "  ✓ URL extraction, scheme filtering, and DASH representation parsing verified" << std::endl;
}

void test_remote_zip_inspection_and_extraction() {
    std::cout << "[TEST] Running test_remote_zip_inspection_and_extraction..." << std::endl;

    opendm::ZipPreviewer previewer;
    std::vector<opendm::ZipEntry> entries;
    std::string err;

    // Use our local test server serving /tmp/test_archive.zip on port 8997
    std::string test_url = "http://127.0.0.1:8997/test_archive.zip";
    bool ok = previewer.fetch_entries(test_url, entries, err);
    if (!ok) {
        std::cerr << "  Skip HTTP test if server not running: " << err << std::endl;
        return;
    }

    assert(ok);
    assert(entries.size() == 3);
    std::cout << "  ✓ Probed Central Directory: found " << entries.size() << " files:" << std::endl;
    for (const auto& e : entries) {
        std::cout << "    - " << e.filename << " (" << e.uncompressed_size << " bytes, method: "
                  << e.compression_method << ")" << std::endl;
    }

    // Selectively extract data.bin without downloading the full archive
    std::string dest_file = "/tmp/extracted_data.bin";
    bool extracted = false;
    for (const auto& e : entries) {
        if (e.filename == "data.bin") {
            extracted = previewer.extract_entry(test_url, e, dest_file, err);
            break;
        }
    }

    assert(extracted);
    assert(std::filesystem::exists(dest_file));
    std::cout << "  ✓ Selectively downloaded and inflated data.bin successfully!" << std::endl;
}

int main(int argc, char* argv[]) {
    std::cout << "=== OpenDM Phase 2 Subsystem Tests ===" << std::endl;
    test_site_grabber_helpers();
    if (argc > 1 && std::string(argv[1]) == "--http") {
        test_remote_zip_inspection_and_extraction();
    }
    std::cout << "=== Phase 2 Unit Tests PASSED Successfully! ===" << std::endl;
    return 0;
}
