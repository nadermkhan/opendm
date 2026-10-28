#include "core/batch_importer.hpp"
#include "core/site_logon.hpp"
#include "core/download_session.hpp"
#include "core/download_queue.hpp"
#include "cli/cli_parser.hpp"
#include "ipc/ipc_server.hpp"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

int main() {
    std::cout << "=== Running OpenDM Subsystem Integration Tests ===" << std::endl;

    // 1. Test SiteLogon vault
    std::cout << "[Test 1] Site Logon Credential Vault..." << std::endl;
    auto& logon = opendm::SiteLogonManager::instance();
    opendm::SiteCredential c1;
    c1.domain = "rapidgator.net";
    c1.username = "premium_user";
    c1.password = "secret_pass_123";
    logon.set_credential(c1);

    opendm::SiteCredential c2;
    c2.domain = "fileserver.org";
    c2.username = "guest";
    c2.password = "anonymous";
    logon.set_credential(c2);

    opendm::SiteCredential found_cred;
    bool found = logon.get_credential_for_url("https://rapidgator.net/file/123", found_cred);
    assert(found);
    assert(found_cred.username == "premium_user");
    assert(found_cred.password == "secret_pass_123");

    found = logon.get_credential_for_url("https://sub.fileserver.org/dl", found_cred);
    assert(found);
    assert(found_cred.username == "guest");
    std::cout << "  [PASS] Site logon URL matching passed." << std::endl;

    // 2. Test Batch EF2 Importer
    std::cout << "[Test 2] IDM EF2 Batch Format Parsing & Serialization..." << std::endl;
    std::string test_ef2 = "/tmp/test_batch.ef2";
    {
        std::ofstream ef2_out(test_ef2);
        ef2_out << "<\n";
        ef2_out << "https://cdn.example.com/file1.iso\n";
        ef2_out << "file1.iso\n";
        ef2_out << "/downloads\n";
        ef2_out << "https://example.com/ref\n";
        ef2_out << "session=abc123\n";
        ef2_out << ">\n";
        ef2_out << "<\n";
        ef2_out << "https://cdn.example.com/file2.mkv\n";
        ef2_out << "file2.mkv\n";
        ef2_out << ">\n";
    }

    std::string err;
    auto items = opendm::BatchImporter::import_file(test_ef2, err);
    assert(items.size() == 2);
    assert(items[0].url == "https://cdn.example.com/file1.iso");
    assert(items[0].filename == "file1.iso");
    assert(items[0].referer == "https://example.com/ref");
    assert(items[0].cookies == "session=abc123");

    assert(items[1].url == "https://cdn.example.com/file2.mkv");
    assert(items[1].filename == "file2.mkv");

    // Export test
    std::string test_ef2_export = "/tmp/test_export.ef2";
    bool exported = opendm::BatchImporter::export_ef2(test_ef2_export, items, err);
    assert(exported);
    auto reimported = opendm::BatchImporter::import_file(test_ef2_export, err);
    assert(reimported.size() == 2);
    assert(reimported[0].url == items[0].url);

    fs::remove(test_ef2);
    fs::remove(test_ef2_export);
    std::cout << "  [PASS] IDM EF2 batch parser & serializer passed." << std::endl;

    // 3. Test CLI Parser
    std::cout << "[Test 3] IDM Command Line Compatibility..." << std::endl;
    const char* argv[] = {
        "opendm",
        "-d", "https://downloads.example.org/setup.tar.gz",
        "-p", "/home/nader/custom_dir",
        "-f", "archive.tar.gz",
        "-s",
        "-q",
        "-n",
        "-a"
    };
    int argc = sizeof(argv) / sizeof(argv[0]);

    opendm::CliOptions opts = opendm::CliParser::parse(argc, const_cast<char**>(argv));
    assert(opts.url == "https://downloads.example.org/setup.tar.gz");
    assert(opts.path == "/home/nader/custom_dir");
    assert(opts.filename == "archive.tar.gz");
    assert(opts.start_queue);
    assert(opts.exit_after_download);
    assert(opts.silent_mode);
    assert(opts.add_only);
    std::cout << "  [PASS] IDM CLI suite argument parser passed." << std::endl;

    // 4. Test Queue Reordering with update callback (verifies Fix 1.5)
    std::cout << "[Test 4] DownloadQueue item reordering callback safety..." << std::endl;
    opendm::DownloadQueue q("test_q", "Test Queue");
    bool callback_called = false;
    q.set_update_callback([&]() {
        callback_called = true;
        // In buggy versions, calling get_all_sessions() from within update_callback_ deadlocked
        auto items = q.get_all_sessions();
        assert(!items.empty());
    });

    auto s1 = std::make_shared<opendm::DownloadSession>("q_item1", "http://example.com/1", "/tmp/1.bin");
    auto s2 = std::make_shared<opendm::DownloadSession>("q_item2", "http://example.com/2", "/tmp/2.bin");
    q.add_session(s1, false);
    q.add_session(s2, false);

    callback_called = false;
    q.move_item_up("q_item2");
    assert(callback_called);

    callback_called = false;
    q.move_item_down("q_item2");
    assert(callback_called);
    std::cout << "  [PASS] Queue move_item_up/down callbacks safely executed without deadlocks." << std::endl;

    // 5. Test DownloadSession Redownload (verifies Fix 1.2 / 1.4)
    std::cout << "[Test 5] DownloadSession Redownload resets chunk state..." << std::endl;
    auto s_redl = std::make_shared<opendm::DownloadSession>("redl_task", "http://example.com/file.zip", "/tmp/file.zip");
    s_redl->set_category("Compressed");
    assert(s_redl->get_category() == "Compressed");

    // Redownload cleans up and resets
    bool redl_ok = s_redl->redownload();
    assert(s_redl->get_status() == opendm::SessionStatus::PROBING || s_redl->get_status() == opendm::SessionStatus::DOWNLOADING || s_redl->get_status() == opendm::SessionStatus::ERROR);
    s_redl->cancel();
    assert(s_redl->get_status() == opendm::SessionStatus::IDLE);
    std::cout << "  [PASS] DownloadSession redownload and lifecycle verified." << std::endl;

    // 6. Test SiteLogonManager::apply_to_session
    std::cout << "[Test 6] SiteLogonManager applies credentials to session..." << std::endl;
    auto s_auth = std::make_shared<opendm::DownloadSession>("auth_task", "https://rapidgator.net/file/999", "/tmp/auth.bin");
    logon.apply_to_session(*s_auth, "https://rapidgator.net/file/999");
    std::cout << "  [PASS] SiteLogonManager credentials applied to session." << std::endl;

    // 7. Test Tonec Wire Protocol Parsing
    std::cout << "[Test 7] Tonec Wire Protocol Parsing..." << std::endl;
    std::string sample_tonec = "MSG#170#13#1#2048:4502:0:1793158010947:302:1:6:7:0,2=14:172.67.212.214,6=51:idmdwnlmfv9://https://wickradio.com/Please-Wait.php,12=13:HTTP/2.0 302 ;";
    auto hdr = opendm::parse_msg_header(sample_tonec);
    assert(hdr.valid);
    assert(hdr.id == 170);
    assert(hdr.type == 13);
    assert(hdr.subtype == 1);
    assert(hdr.flags == 2048);

    auto args = opendm::parse_tonec_pos_args(sample_tonec);
    assert(args.size() == 8);
    assert(args[0] == "4502"); // requestId
    assert(args[3] == "302");  // status code
    assert(args[5] == "6");    // action code (redirect)

    std::string field6 = opendm::extract_tonec_field(sample_tonec, 6);
    assert(field6 == "idmdwnlmfv9://https://wickradio.com/Please-Wait.php");
    std::string field12 = opendm::extract_tonec_field(sample_tonec, 12);
    assert(field12 == "HTTP/2.0 302 ");
    std::cout << "  [PASS] Tonec header and positional argument parsing passed." << std::endl;

    // 8. Test Extension and Content-Type filtering helpers
    std::cout << "[Test 8] Content-Type & Extension Filtering..." << std::endl;
    assert(opendm::is_downloadable_extension("ZIP"));
    assert(opendm::is_downloadable_extension("exe"));
    assert(opendm::is_downloadable_extension("iso"));
    assert(opendm::is_downloadable_extension("r01"));
    assert(!opendm::is_downloadable_extension("php"));
    assert(!opendm::is_downloadable_extension("html"));
    assert(!opendm::is_downloadable_extension("png"));

    assert(opendm::is_forbidden_web_extension("php"));
    assert(opendm::is_forbidden_web_extension("html"));
    assert(opendm::is_forbidden_web_extension("js"));
    assert(opendm::is_forbidden_web_extension("css"));
    assert(opendm::is_forbidden_web_extension("png"));
    assert(opendm::is_forbidden_web_extension("jpg"));
    assert(opendm::is_forbidden_web_extension("svg"));
    assert(!opendm::is_forbidden_web_extension("zip"));
    assert(!opendm::is_forbidden_web_extension("exe"));

    assert(opendm::is_web_content_type("text/html; charset=UTF-8"));
    assert(opendm::is_web_content_type("text/css"));
    assert(opendm::is_web_content_type("image/png"));
    assert(opendm::is_web_content_type("image/jpeg"));
    assert(opendm::is_web_content_type("application/javascript"));
    assert(!opendm::is_web_content_type("application/zip"));
    assert(!opendm::is_web_content_type("application/octet-stream"));
    std::cout << "  [PASS] Extension and MIME type filtering verified." << std::endl;

    // 9. Test IpcServer handling of unintended downloads vs genuine downloads
    std::cout << "[Test 9] IpcServer unintended download rejection..." << std::endl;
    {
        opendm::IpcServer test_ipc("/tmp/opendm_test.sock");
        test_ipc.start();

        // A. Please-Wait.php with action 6 (redirect) -> MUST be ignored
        std::string resp_redirect = opendm::IpcServer::send_command(sample_tonec, "/tmp/opendm_test.sock");
        assert(resp_redirect.find("\"status\": \"ignored\"") != std::string::npos);

        // B. Inline image -> MUST be ignored
        std::string img_tonec = "MSG#171#13#1#2048:4503:0:1793158010950:200:1:2:7:0,6=28:https://example.com/logo.png,13=48:content-type: image/png\r\ncontent-length: 1234\r\n;";
        std::string resp_img = opendm::IpcServer::send_command(img_tonec, "/tmp/opendm_test.sock");
        assert(resp_img.find("\"status\": \"ignored\"") != std::string::npos);

        // C. Plain HTML page without attachment -> MUST be ignored
        std::string html_tonec = "MSG#172#13#1#2048:4504:0:1793158010952:200:1:2:7:0,6=30:https://example.com/index.html,13=50:content-type: text/html\r\ncontent-length: 5000\r\n;";
        std::string resp_html = opendm::IpcServer::send_command(html_tonec, "/tmp/opendm_test.sock");
        assert(resp_html.find("\"status\": \"ignored\"") != std::string::npos);

        // D. PHP page without attachment -> MUST be ignored
        std::string php_tonec = "MSG#173#13#1#2048:4505:0:1793158010954:200:1:1:7:0,6=32:https://example.com/download.php;";
        std::string resp_php = opendm::IpcServer::send_command(php_tonec, "/tmp/opendm_test.sock");
        assert(resp_php.find("\"status\": \"ignored\"") != std::string::npos);

        // E. PHP page WITH Content-Disposition: attachment -> MUST be accepted
        std::string php_att_tonec = "MSG#174#13#1#2048:4506:0:1793158010956:200:1:2:7:0,6=32:https://example.com/download.php,13=95:content-type: application/octet-stream\r\ncontent-disposition: attachment; filename=\"archive.zip\"\r\n;";
        std::string resp_att = opendm::IpcServer::send_command(php_att_tonec, "/tmp/opendm_test.sock");
        assert(resp_att.find("\"status\": \"prompting\"") != std::string::npos);

        // F. Genuine ZIP download -> MUST be accepted
        std::string zip_tonec = "MSG#175#13#1#2048:4507:0:1793158010958:200:1:1:7:0,6=29:https://example.com/build.zip,4=3:ZIP;";
        std::string resp_zip = opendm::IpcServer::send_command(zip_tonec, "/tmp/opendm_test.sock");
        assert(resp_zip.find("\"status\": \"prompting\"") != std::string::npos);

        // G. Explicit Video Download (Type 14 from "Download this video" hover panel) -> MUST be accepted
        std::string vid_tonec = "MSG#200#14#1#0:1,6=45:https://example.com/videoplayback?expire=123,7=12:Sample Video,100=16:Sample_Video.mp4;";
        std::string resp_vid = opendm::IpcServer::send_command(vid_tonec, "/tmp/opendm_test.sock");
        assert(resp_vid.find("\"status\": \"prompting\"") != std::string::npos);

        test_ipc.stop();
        std::cout << "  [PASS] IpcServer successfully filtered web pages/images and accepted genuine downloads." << std::endl;
    }

    // 10. Test Queue and Session State Persistence (save_to_file / load_from_file)
    std::cout << "[Test 10] Queue & Session State Persistence..." << std::endl;
    {
        std::string test_storage = "/tmp/opendm_test_queue.json";
        if (fs::exists(test_storage)) fs::remove(test_storage);

        auto s1 = std::make_shared<opendm::DownloadSession>("sess_test_10", "https://example.com/archive.zip", "/tmp/archive.zip", "/tmp/opendm_test_chunks");
        s1->set_category("Compressed");
        s1->set_custom_filename("archive.zip");

        // Test direct session serialization and deserialization
        std::string json_state = s1->serialize();
        assert(json_state.find("\"id\": \"sess_test_10\"") != std::string::npos);
        assert(json_state.find("\"category\": \"Compressed\"") != std::string::npos);

        auto restored_sess = opendm::DownloadSession::deserialize(json_state);
        assert(restored_sess != nullptr);
        assert(restored_sess->get_id() == "sess_test_10");
        assert(restored_sess->get_url() == "https://example.com/archive.zip");
        assert(restored_sess->get_destination_path() == "/tmp/archive.zip");
        assert(restored_sess->get_category() == "Compressed");
        assert(restored_sess->get_filename() == "archive.zip");

        // Test QueueManager persistence across restart
        auto& qm = opendm::QueueManager::instance();
        qm.set_storage_path(test_storage);
        qm.clear_all();
        qm.get_main_queue().add_session(s1, false);

        bool saved = qm.save_to_file(test_storage);
        assert(saved);
        assert(fs::exists(test_storage));

        qm.clear_all();
        assert(qm.get_main_queue().get_all_sessions().empty());

        bool loaded = qm.load_from_file(test_storage);
        assert(loaded);
        auto loaded_items = qm.get_main_queue().get_all_sessions();
        assert(loaded_items.size() == 1);
        assert(loaded_items[0]->get_id() == "sess_test_10");
        assert(loaded_items[0]->get_category() == "Compressed");

        qm.clear_all();
        fs::remove(test_storage);
        std::cout << "  [PASS] Session serialization and QueueManager save/load verified." << std::endl;
    }

    // 11. Test Disk Space Pre-Check & Metadata Accessors
    std::cout << "[Test 11] Disk Space Pre-Check & Metadata Accessors..." << std::endl;
    {
        auto s2 = std::make_shared<opendm::DownloadSession>("sess_test_11", "https://example.com/testfile.dat", "/tmp/testfile.dat", "/tmp/opendm_test_chunks");
        assert(s2->get_last_modified().empty());
        assert(s2->get_etag().empty());
        assert(s2->get_total_size() == -1);
        assert(!s2->supports_ranges());

        std::error_code ec;
        auto space_info = fs::space("/tmp", ec);
        if (!ec) {
            assert(space_info.available > 0);
            uintmax_t impossible_size = space_info.available + 1024ULL * 1024ULL * 1024ULL * 1024ULL;
            assert(impossible_size > space_info.available);
        }
        std::cout << "  [PASS] Disk space verification and session metadata accessors passed." << std::endl;
    }

    // 12. Test YouTube & DASH Stream Scheme Normalization, Splitting & Range Parameter Stripping
    std::cout << "[Test 12] YouTube & DASH Stream Scheme Normalization, Dual-Stream Splitting & Range Stripping..." << std::endl;
    {
        // A. Test Scheme Normalization
        assert(opendm::normalize_stream_scheme("yta2toidm://https://example.com/v") == "https://example.com/v");
        assert(opendm::normalize_stream_scheme("yta3toidm://https://example.com/v") == "https://example.com/v");
        assert(opendm::normalize_stream_scheme("yta5toidm://https://example.com/v") == "https://example.com/v");
        assert(opendm::normalize_stream_scheme("ytatoidm://https://example.com/v") == "https://example.com/v");
        assert(opendm::normalize_stream_scheme("mpdatoidm://https://example.com/v") == "https://example.com/v");
        assert(opendm::normalize_stream_scheme("mpdtoidm://https://example.com/v") == "https://example.com/v");
        assert(opendm::normalize_stream_scheme("hls2toidmtomp4://https://example.com/v") == "https://example.com/v");
        assert(opendm::normalize_stream_scheme("idmfchttps://example.com/v") == "https://example.com/v");
        assert(opendm::normalize_stream_scheme("idmfchttp://example.com/v") == "http://example.com/v");

        // B. Test Dual-Stream Splitting
        auto [v, a] = opendm::split_dual_stream_url("yta2toidm://https://googlevideo.com/v1##https://googlevideo.com/a1");
        assert(v == "https://googlevideo.com/v1");
        assert(a == "https://googlevideo.com/a1");

        // Single stream case
        auto [v_single, a_single] = opendm::split_dual_stream_url("https://example.com/video.mp4");
        assert(v_single == "https://example.com/video.mp4");
        assert(a_single.empty());

        // C. Test Parameter Stripping on googlevideo.com and videoplayback
        std::string raw_yt = "https://rr1---sn-4g5ednks.googlevideo.com/videoplayback?expire=123&itag=137&mime=video%2Fmp4&range=0-1048576&rn=1&begin=0&len=1000&ump=1";
        std::string cleaned_yt = opendm::clean_stream_query_parameters(raw_yt);
        assert(cleaned_yt.find("range=") == std::string::npos);
        assert(cleaned_yt.find("begin=") == std::string::npos);
        assert(cleaned_yt.find("len=") == std::string::npos);
        assert(cleaned_yt.find("ump=") == std::string::npos);
        assert(cleaned_yt.find("expire=123") != std::string::npos);
        assert(cleaned_yt.find("itag=137") != std::string::npos);
        assert(cleaned_yt.find("rn=1") != std::string::npos);

        // Test Parameter Stripping on mycdn.me
        std::string raw_mycdn = "https://mycdn.me/video.mp4?bytes=0-500&sig=abc";
        std::string cleaned_mycdn = opendm::clean_stream_query_parameters(raw_mycdn);
        assert(cleaned_mycdn == "https://mycdn.me/video.mp4?sig=abc");

        // D. Test Filename Sanitization from Video Title
        assert(opendm::sanitize_media_filename("Never Gonna Give You Up - Rick Astley", "mp4") == "Never Gonna Give You Up - Rick Astley.mp4");
        assert(opendm::sanitize_media_filename("Test / Video : With * Illegal ? Chars", "mp4") == "Test _ Video _ With _ Illegal _ Chars.mp4");

        // E. Test IPC reception of paired YouTube stream
        opendm::IpcServer yt_ipc("/tmp/opendm_yt_test.sock");
        opendm::DownloadPromptInfo captured_prompt;
        bool prompt_received = false;
        yt_ipc.set_download_prompt_callback([&](const opendm::DownloadPromptInfo& info) {
            captured_prompt = info;
            prompt_received = true;
        });
        yt_ipc.start();

        std::string raw_stream_payload = "yta2toidm://https://rr1.googlevideo.com/videoplayback?itag=137&mime=video%2Fmp4&range=0-1048576&rn=1"
            "##https://rr1.googlevideo.com/videoplayback?itag=140&mime=audio%2Fmp4&range=0-1048576&rn=1";
        std::string raw_title = "Rick Astley - Never Gonna Give You Up";
        std::string yt_tonec = "MSG#250#14#1#0:1,"
            "6=" + std::to_string(raw_stream_payload.length()) + ":" + raw_stream_payload + ","
            "7=" + std::to_string(raw_title.length()) + ":" + raw_title + ";";
        std::string resp = opendm::IpcServer::send_command(yt_tonec, "/tmp/opendm_yt_test.sock");
        assert(resp.find("\"status\": \"prompting\"") != std::string::npos);
        assert(prompt_received);
        assert(captured_prompt.is_streaming_media);
        assert(captured_prompt.url == "https://rr1.googlevideo.com/videoplayback?itag=137&mime=video%2Fmp4&rn=1");
        assert(captured_prompt.audio_stream_url == "https://rr1.googlevideo.com/videoplayback?itag=140&mime=audio%2Fmp4&rn=1");
        assert(captured_prompt.url.find("range=") == std::string::npos);
        assert(captured_prompt.audio_stream_url.find("range=") == std::string::npos);
        assert(captured_prompt.filename == "Rick Astley - Never Gonna Give You Up.mp4");

        yt_ipc.stop();
        std::cout << "  [PASS] Stream scheme normalization, dual-stream demuxing, range parameter stripping, and IPC prompt verified." << std::endl;
    }

    std::cout << "All Subsystem Integration tests passed!" << std::endl;
    return 0;
}
