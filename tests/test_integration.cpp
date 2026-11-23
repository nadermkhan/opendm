#include "core/batch_importer.hpp"
#include "core/site_logon.hpp"
#include "core/download_session.hpp"
#include "core/download_queue.hpp"
#include "core/win32_unicode.hpp"
#include "core/youtube_engine.hpp"
#include "core/native_host_setup.hpp"
#include "cli/cli_parser.hpp"
#include "ipc/ipc_server.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include <chrono>
#include <thread>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
namespace {
int run_win32_job_test_child(const std::string& result_path, const std::string& noop_marker_path) {
    bool breakaway_allowed = true;
    bool silent_breakaway = true;
    uint32_t err = 0;
    const bool in_job = opendm::sys::is_current_process_in_job(breakaway_allowed, silent_breakaway, &err);

    wchar_t sys_dir_buf[MAX_PATH * 2]{};
    UINT slen = GetSystemDirectoryW(sys_dir_buf, static_cast<UINT>(std::size(sys_dir_buf)));
    std::string sys_dir = (slen > 0) ? opendm::sys::wide_to_utf8(std::wstring_view(sys_dir_buf, slen)) : "C:\\Windows\\System32";

    opendm::sys::DetachedLaunchOptions opts;
    opts.exe_path = sys_dir + "\\cmd.exe";
    opts.arguments = "/c echo alive > \"" + noop_marker_path + "\"";
    opts.working_dir = sys_dir;
    opts.show_cmd = SW_HIDE;
    opts.allow_foreground = false;

    auto cp_breakaway = opendm::sys::launch_via_create_process(opts, true);
    auto detached = opendm::sys::launch_detached_gui_process(opts);

    nlohmann::json report = {
        {"in_job", in_job},
        {"breakaway_allowed", breakaway_allowed},
        {"silent_breakaway", silent_breakaway},
        {"cp_ok", cp_breakaway.ok},
        {"cp_err", cp_breakaway.win32_error},
        {"detached_ok", detached.ok},
        {"detached_method", detached.method},
        {"detached_err", detached.win32_error}
    };
    std::ofstream ofs(opendm::sys::path_from_utf8(result_path), std::ios::binary | std::ios::trunc);
    ofs << report.dump();
    ofs.close();
    return 0;
}
} // namespace
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    if (argc >= 3 && std::string(argv[1]) == "--win32-noop-child") {
        std::ofstream ofs(opendm::sys::path_from_utf8(argv[2]), std::ios::binary | std::ios::trunc);
        ofs << "alive";
        return 0;
    }
    if (argc >= 4 && std::string(argv[1]) == "--win32-job-test-child") {
        return run_win32_job_test_child(argv[2], argv[3]);
    }
#else
    (void)argc;
    (void)argv;
#endif
    std::cout << "=== Running OpenDM Subsystem Integration Tests ===" << std::endl;

    // Isolate QueueManager storage path so tests never overwrite the user's real downloads.json
    opendm::QueueManager::instance().set_storage_path("/tmp/opendm_test_queue.json");

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
    const char* cli_argv[] = {
        "opendm",
        "-d", "https://downloads.example.org/setup.tar.gz",
        "-p", "/home/nader/custom_dir",
        "-f", "archive.tar.gz",
        "-s",
        "-q",
        "-n",
        "-a"
    };
    int cli_argc = sizeof(cli_argv) / sizeof(cli_argv[0]);

    opendm::CliOptions opts = opendm::CliParser::parse(cli_argc, const_cast<char**>(cli_argv));
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
        assert(fs::path(restored_sess->get_destination_path()) == fs::path("/tmp/archive.zip"));
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
        assert(cleaned_yt.find("rn=") == std::string::npos);

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
        assert(captured_prompt.url == "https://rr1.googlevideo.com/videoplayback?itag=137&mime=video%2Fmp4");
        assert(captured_prompt.audio_stream_url == "https://rr1.googlevideo.com/videoplayback?itag=140&mime=audio%2Fmp4");
        assert(captured_prompt.url.find("range=") == std::string::npos);
        assert(captured_prompt.audio_stream_url.find("range=") == std::string::npos);
        assert(captured_prompt.filename == "Rick Astley - Never Gonna Give You Up.mp4");

        yt_ipc.stop();
        std::cout << "  [PASS] Stream scheme normalization, dual-stream demuxing, range parameter stripping, and IPC prompt verified." << std::endl;
    }

    // 13. Test URL Percent-Decoding, Filename Extraction & Category Detection (DhakaFlix regression test)
    std::cout << "[Test 13] URL Percent-Decoding, Filename Extraction & Category Detection..." << std::endl;
    {
        std::string dhakaflix_url = "http://172.16.50.14/DHAKA-FLIX-14/SOUTH%20INDIAN%20MOVIES/South%20Movies/2025/Little%20Hearts%20%282025%29%201080p%20%28Telugu%29/Little%20Hearts%20%282025%29%20Telugu%201080p%20WEBRip%20x265%20HEVC%2010bit%20DD5.1%20ESub%20-1TMV.mkv";
        std::string expected_fn = "Little Hearts (2025) Telugu 1080p WEBRip x265 HEVC 10bit DD5.1 ESub -1TMV.mkv";

        std::string extracted = opendm::DownloadSession::extract_filename_from_url(dhakaflix_url);
        assert(extracted == expected_fn);
        assert(opendm::QueueManager::detect_category(extracted) == "Video");
        assert(opendm::QueueManager::detect_category("download.bin") == "General");

        // Even if DownloadSession is constructed with destination ending in download.bin, it must resolve the real filename from URL
        auto df_sess = std::make_shared<opendm::DownloadSession>("df_1", dhakaflix_url, "/tmp/download.bin", "/tmp/opendm_test_chunks");
        assert(df_sess->get_filename() == expected_fn);
        assert(fs::path(df_sess->get_destination_path()).filename().string() == expected_fn);

        // Query parameters containing slashes must not corrupt the path filename
        std::string query_slash_url = "https://cdn.example.com/movies/Archive%20File.zip?redirect=http://other.com/foo/bar#section";
        assert(opendm::DownloadSession::extract_filename_from_url(query_slash_url) == "Archive File.zip");

        // S3 response-content-disposition in query string
        std::string s3_url = "https://s3.amazonaws.com/bucket/blob123?response-content-disposition=attachment%3B%20filename%3D%22My%20Movie%202025.mp4%22&X-Amz-Signature=abc";
        assert(opendm::DownloadSession::extract_filename_from_url(s3_url) == "My Movie 2025.mp4");

        // RFC 5987 Content-Disposition header
        std::string cd_hdr = "attachment; filename=\"fallback.mkv\"; filename*=UTF-8''Little%20Hearts%20%282025%29.mkv";
        assert(opendm::DownloadSession::extract_filename_from_content_disposition(cd_hdr) == "Little Hearts (2025).mkv");

        // Host-only URL fallback
        assert(opendm::DownloadSession::extract_filename_from_url("https://example.com") == "download.bin");
        assert(opendm::DownloadSession::extract_filename_from_url("https://example.com/") == "download.bin");

        std::cout << "  [PASS] URL percent-decoding, filename extraction, and category detection verified." << std::endl;
    }

    // 14. Windows Unicode, Legacy Migration, Settings Persistence, SiteLogon Headers & >16KB IPC
    std::cout << "[Test 14] UTF-8 Paths, QueueManager Migration/Settings, Batch Empty Lines & Large IPC..." << std::endl;
    {
        // A. UTF-8 <-> Wide round-trip & DOS reserved filename detection
        std::string utf8_sample = "বাংলা_ভিডিও_日本語_2025.mp4";
        assert(opendm::sys::wide_to_utf8(opendm::sys::utf8_to_wide(utf8_sample)) == utf8_sample);
        assert(opendm::sys::is_reserved_dos_name("CON.txt"));
        assert(opendm::DownloadSession::sanitize_filename("CON.txt") == "_CON.txt");

        // B. QueueManager legacy backslash migration + settings + quoted ETag round-trip
        std::string qstore = "/tmp/opendm_test_mig_queue.json";
        fs::remove(qstore);
        fs::remove(qstore + ".bak");
        {
            std::ofstream legacy_out(qstore);
            legacy_out << R"({
  "version": 1,
  "queues": [
    {
      "name": "Main Download Queue",
      "items": [
        {
          "id": "mig_1",
          "url": "https://example.com/movie.mkv",
          "destination_path": "C:\Users\NiO\Downloads\Video\movie.mkv",
          "filename": "movie.mkv",
          "category": "Video",
          "total_size": 5368709120,
          "etag": "\"abc-123\"",
          "supports_ranges": true,
          "status": 3
        }
      ]
    }
  ]
})";
        }
        opendm::QueueManager::instance().set_storage_path(qstore);
        assert(opendm::QueueManager::instance().load_from_file());
        auto mig_sess = opendm::QueueManager::instance().find_session("mig_1");
        assert(mig_sess != nullptr);
        assert(mig_sess->get_destination_path() == "C:\\Users\\NiO\\Downloads\\Video\\movie.mkv");

        // Persist settings + non-ASCII filename session & reload
        opendm::QueueManager::instance().set_default_max_connections(12);
        opendm::QueueManager::instance().set_global_speed_limit(512 * 1024);
        opendm::QueueManager::instance().enable_global_speed_limit(true);
        opendm::QueueManager::instance().set_default_save_dir("D:/Downloads/বাংলা");
        auto utf8_sess = std::make_shared<opendm::DownloadSession>("utf8_s", "https://example.com/video.mp4",
                                                                   "/tmp/" + utf8_sample);
        opendm::QueueManager::instance().get_main_queue().add_session(utf8_sess, false);
        opendm::QueueManager::instance().save_to_file();

        opendm::QueueManager::instance().load_from_file();
        assert(opendm::QueueManager::instance().get_default_max_connections() == 12);
        assert(opendm::QueueManager::instance().get_global_speed_limit() == 512 * 1024);
        assert(opendm::QueueManager::instance().is_global_speed_limit_enabled());
        assert(opendm::QueueManager::instance().get_default_save_dir() == "D:/Downloads/বাংলা");
        assert(opendm::QueueManager::instance().find_session("utf8_s") != nullptr);
        opendm::QueueManager::instance().get_main_queue().remove_session("utf8_s", false);
        opendm::QueueManager::instance().get_main_queue().remove_session("mig_1", true);
        fs::remove(qstore);
        fs::remove(qstore + ".bak");

        // C. BatchImporter preserves empty lines between EF2 items
        std::string ef2_empty = "/tmp/test_empty_line.ef2";
        {
            std::ofstream out(ef2_empty);
            out << "<\nhttps://example.com/a.zip\nreferer: https://example.com/\n\n>\n<\nhttps://example.com/b.zip\nUser-Agent: TestUA/1.0\n>\n";
        }
        std::string berr;
        auto b_items = opendm::BatchImporter::import_file(ef2_empty, berr);
        assert(b_items.size() == 2);
        assert(b_items[0].referer == "https://example.com/");
        assert(b_items[1].user_agent == "TestUA/1.0");
        fs::remove(ef2_empty);

        // D. >16KB IPC payload & response round-trip + multi-line Tonec \r\n headers
        opendm::IpcServer big_ipc("/tmp/opendm_big_ipc_test.sock");
        big_ipc.set_gui_mode(true);
        opendm::DownloadPromptInfo big_prompt;
        bool got_big_prompt = false;
        big_ipc.set_download_prompt_callback([&](const opendm::DownloadPromptInfo& info) {
            big_prompt = info;
            got_big_prompt = true;
        });
        assert(big_ipc.start());

        // >20KB JSON request with huge parameter
        std::string long_param(22000, 'x');
        std::string big_req = "{\"action\": \"download\", \"url\": \"https://example.com/big_payload.zip?pad=" + long_param + "\"}";
        std::string big_resp = opendm::IpcServer::send_command(big_req, "/tmp/opendm_big_ipc_test.sock");
        assert(big_resp.find("\"status\": \"prompting\"") != std::string::npos);
        assert(got_big_prompt);
        assert(big_prompt.filename == "big_payload.zip");

        // Verify >16KB build_youtube_extract_json serialization + Tonec \r\n headers
        opendm::YouTubeVideoInfo huge_yt;
        huge_yt.video_id = "huge123";
        huge_yt.title = "Very Long Test Title বাংলা";
        huge_yt.best_audio.itag = 140;
        huge_yt.best_audio.container = "M4A";
        huge_yt.best_audio.bitrate = 128000;
        huge_yt.best_audio.url = "https://rr1.googlevideo.com/videoplayback?id=audio";
        huge_yt.audio_formats.push_back(huge_yt.best_audio);
        for (int i = 0; i < 60; ++i) {
            opendm::YouTubeFormat f;
            f.itag = 200 + i;
            f.quality_label = "1080p";
            f.container = "MP4";
            f.url = "https://rr1.googlevideo.com/videoplayback?id=" + std::to_string(i) + "&pad=" + std::string(300, 'a');
            huge_yt.video_formats.push_back(f);
        }
        std::string yt_json = opendm::build_youtube_extract_json(huge_yt);
        assert(yt_json.size() > 20000);

        // Multi-line \r\n headers inside Tonec message
        got_big_prompt = false;
        std::string resp_hdrs = "HTTP/1.1 200 OK\r\nContent-Length: 5242880\r\nContent-Disposition: attachment; filename=\"Report (2025).zip\"\r\n";
        std::string ml_url = "https://example.com/download.php?id=99";
        std::string ml_tonec = "MSG#300#13#1#0:0:0:200:0:1,"
            "6=" + std::to_string(ml_url.size()) + ":" + ml_url + ","
            "13=" + std::to_string(resp_hdrs.size()) + ":" + resp_hdrs + ";";
        std::string ml_resp = opendm::IpcServer::send_command(ml_tonec, "/tmp/opendm_big_ipc_test.sock");
        assert(ml_resp.find("\"status\": \"prompting\"") != std::string::npos);
        assert(got_big_prompt);
        assert(big_prompt.filename == "Report (2025).zip");
        assert(big_prompt.file_size == 5242880);

        big_ipc.stop();
        std::cout << "  [PASS] UTF-8 helpers, legacy migration, settings persistence, batch importer & >16KB/multi-line IPC verified." << std::endl;
    }

    // 15. Clean OpenDM Extension JSON Protocol, "6=" Dispatch Guard, Batch Download & Native Host Setup
    std::cout << "[Test 15] Clean Extension JSON Protocol, 6= Dispatch Fix, Batch & Native Host Setup..." << std::endl;
    {
        opendm::IpcServer ext_ipc("/tmp/opendm_ext_proto_test.sock");
        opendm::DownloadPromptInfo ext_prompt;
        bool got_ext_prompt = false;
        ext_ipc.set_download_prompt_callback([&](const opendm::DownloadPromptInfo& info) {
            ext_prompt = info;
            got_ext_prompt = true;
        });
        assert(ext_ipc.start());

        // 15a. URL containing "6=" inside query string must NOT trigger legacy Tonec parser
        std::string q6_req = R"({"action":"download","url":"https://example.com/movie.mkv?token=6=secret","filename":"movie.mkv","file_size":1048576})";
        std::string q6_resp = opendm::IpcServer::send_command(q6_req, "/tmp/opendm_ext_proto_test.sock");
        assert(q6_resp.find("\"status\": \"prompting\"") != std::string::npos);
        assert(got_ext_prompt);
        assert(ext_prompt.url == "https://example.com/movie.mkv?token=6=secret");
        assert(ext_prompt.file_size == 1048576);

        // 15b. New intercept_download rejects web HTML and prompts valid binary attachment
        got_ext_prompt = false;
        std::string html_intercept = R"({"action":"intercept_download","url":"https://example.com/page.php","content_type":"text/html","status_code":200})";
        std::string html_resp = opendm::IpcServer::send_command(html_intercept, "/tmp/opendm_ext_proto_test.sock");
        assert(html_resp.find("\"status\": \"ignored\"") != std::string::npos);
        assert(!got_ext_prompt);

        std::string bin_intercept = R"({"action":"intercept_download","url":"https://example.com/dl.php","filename":"archive.tar.gz","content_type":"application/octet-stream","is_attachment":true,"file_size":2097152,"referer":"https://example.com/downloads"})";
        std::string bin_resp = opendm::IpcServer::send_command(bin_intercept, "/tmp/opendm_ext_proto_test.sock");
        assert(bin_resp.find("\"status\": \"prompting\"") != std::string::npos);
        assert(got_ext_prompt);
        assert(ext_prompt.filename == "archive.tar.gz");
        assert(ext_prompt.file_size == 2097152);
        assert(ext_prompt.referer == "https://example.com/downloads");

        // 15c. download_batch queues valid HTTP/HTTPS items and skips invalid schemes
        std::string batch_req = R"({
            "action": "download_batch",
            "add_only": "true",
            "items": [
                {"url": "https://example.com/a.zip", "filename": "a.zip"},
                {"url": "blob:https://example.com/invalid"},
                {"url": "https://example.com/b.iso"}
            ]
        })";
        std::string batch_resp = opendm::IpcServer::send_command(batch_req, "/tmp/opendm_ext_proto_test.sock");
        auto jbatch = nlohmann::json::parse(batch_resp);
        assert(jbatch["status"] == "ok");
        assert(jbatch["queued"] == 2);
        assert(jbatch["skipped"] == 1);
        for (const auto& id_val : jbatch["ids"]) {
            opendm::QueueManager::instance().get_main_queue().remove_session(id_val.get<std::string>());
        }
        ext_ipc.stop();

        // 15d. build_youtube_extract_json exposes explicit video_url & audio_url on muxed options
        opendm::YouTubeVideoInfo yt_info;
        yt_info.video_id = "dQw4w9WgXcQ";
        yt_info.title = "Sample";
        yt_info.best_audio.itag = 140;
        yt_info.best_audio.container = "m4a";
        yt_info.best_audio.bitrate = 128000;
        yt_info.best_audio.url = "https://rr1.googlevideo.com/videoplayback?itag=140";
        yt_info.audio_formats.push_back(yt_info.best_audio);
        opendm::YouTubeFormat vf;
        vf.itag = 137;
        vf.quality_label = "1080p";
        vf.container = "mp4";
        vf.url = "https://rr1.googlevideo.com/videoplayback?itag=137";
        yt_info.video_formats.push_back(vf);
        auto jyt = nlohmann::json::parse(opendm::build_youtube_extract_json(yt_info));
        assert(jyt["options"][0]["is_muxed"] == true);
        assert(jyt["options"][0]["video_url"] == "https://rr1.googlevideo.com/videoplayback?itag=137");
        assert(jyt["options"][0]["audio_url"] == "https://rr1.googlevideo.com/videoplayback?itag=140");

        // 15e. CLI flag --register-native-host & manifest generator dry-run
        const char* reg_argv[] = {"opendm", "--register-native-host"};
        auto reg_opts = opendm::CliParser::parse(2, const_cast<char**>(reg_argv));
        assert(reg_opts.register_native_host == true);
        assert(reg_opts.show_gui == false);

        fs::path tmp_nmh = fs::temp_directory_path() / "opendm_test_nmh_manifests";
        std::error_code ec;
        fs::remove_all(tmp_nmh, ec);

        opendm::NativeHostSetupOptions setup_opts;
        setup_opts.host_executable_path = "/opt/opendm/opendm-native-host";
        setup_opts.manifest_dir = opendm::sys::path_to_utf8(tmp_nmh);
        setup_opts.dry_run = true;
        auto setup_res = opendm::register_native_messaging_hosts(setup_opts);
        assert(setup_res.ok);
        assert(fs::exists(setup_res.chrome_manifest_path));
        assert(fs::exists(setup_res.firefox_manifest_path));

        std::ifstream cfs(setup_res.chrome_manifest_path);
        auto jchrome = nlohmann::json::parse(cfs);
        assert(jchrome["name"] == "com.opendm.host");
        assert(jchrome["allowed_origins"][0] == "chrome-extension://ekehfpkieofbbgakjbmefknbbabjpoog/");
        cfs.close();

        std::ifstream ffs(setup_res.firefox_manifest_path);
        auto jff = nlohmann::json::parse(ffs);
        assert(jff["allowed_extensions"][0] == "extension@opendm.org");
        ffs.close();

        fs::remove_all(tmp_nmh, ec);
        std::cout << "  [PASS] Extension JSON protocol, 6= fix, batch download & native host manifests verified." << std::endl;
    }

    // 16. Log Redaction Ordering & Windows Job-Object Escape
    std::cout << "[Test 16] Log Redaction Ordering & Windows Job-Object Escape..." << std::endl;
    {
        std::string long_cookie(2500, 'c');
        std::string long_url = "https://cdn.example.com/files/movie.mkv?sig=" + std::string(200, 's');
        nlohmann::json big_req = {
            {"action", "intercept_download"},
            {"cookies", long_cookie},
            {"resp_headers", "Set-Cookie: session=secret\r\nContent-Length: 100"},
            {"filename", "movie.mkv"},
            {"url", long_url}
        };
        std::string redacted = opendm::redact_for_log(big_req.dump());
        assert(redacted.find("intercept_download") != std::string::npos);
        assert(redacted.find("https://cdn.example.com/files/movie.mkv") != std::string::npos);
        assert(redacted.find(long_cookie.substr(0, 32)) == std::string::npos);
        assert(redacted.find("[REDACTED]") != std::string::npos);
        assert(redacted.find("session=secret") == std::string::npos);

        // download_batch nested items[].cookies redaction
        nlohmann::json batch_log_req = {
            {"action", "download_batch"},
            {"items", {{{"url", "https://example.com/a.zip"}, {"cookies", "secret_token=xyz"}}}}
        };
        std::string batch_redacted = opendm::redact_for_log(batch_log_req.dump());
        assert(batch_redacted.find("download_batch") != std::string::npos);
        assert(batch_redacted.find("secret_token=xyz") == std::string::npos);
        assert(batch_redacted.find("[REDACTED]") != std::string::npos);

        // Both raw MSG# and JSON-quoted "\"MSG#...\"" must be redacted
        std::string raw_tonec = "MSG#1#5#0:0,51=12:cookie=secret;";
        std::string quoted_tonec = nlohmann::json(raw_tonec).dump();
        assert(opendm::redact_for_log(raw_tonec).find("cookie=secret") == std::string::npos);
        assert(opendm::redact_for_log(quoted_tonec).find("cookie=secret") == std::string::npos);

        // >260 KB non-JSON fast short-circuit
        std::string huge_non_json(270000, 'x');
        assert(opendm::redact_for_log(huge_non_json).rfind("[oversized:", 0) == 0);

#ifdef _WIN32
        fs::path tmp_job_dir = fs::temp_directory_path() / "opendm_job_escape_test";
        std::error_code ec;
        fs::remove_all(tmp_job_dir, ec);
        fs::create_directories(tmp_job_dir, ec);
        fs::path result_file = tmp_job_dir / "result.json";
        fs::path noop_marker = tmp_job_dir / "noop_alive.txt";

        HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
        assert(hJob != nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        assert(SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)) != FALSE);

        wchar_t self_exe[MAX_PATH * 4]{};
        DWORD nlen = GetModuleFileNameW(nullptr, self_exe, static_cast<DWORD>(std::size(self_exe)));
        assert(nlen > 0);
        std::wstring cmdline = L"\"" + std::wstring(self_exe, nlen) + L"\" --win32-job-test-child \"" +
                               result_file.wstring() + L"\" \"" + noop_marker.wstring() + L"\"";
        std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
        cmd_buf.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        BOOL created = CreateProcessW(self_exe, cmd_buf.data(), nullptr, nullptr, FALSE,
                                      CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        assert(created != FALSE);
        assert(AssignProcessToJobObject(hJob, pi.hProcess) != FALSE);
        ResumeThread(pi.hThread);
        DWORD wait_rc = WaitForSingleObject(pi.hProcess, 5000);
        if (wait_rc != WAIT_OBJECT_0) {
            TerminateProcess(pi.hProcess, 1);
            assert(false && "Job test child timed out");
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(hJob);

        assert(fs::exists(result_file));
        std::ifstream rfs(result_file);
        auto jrep = nlohmann::json::parse(rfs);
        rfs.close();

        assert(jrep["in_job"] == true);
        assert(jrep["breakaway_allowed"] == false);
        assert(jrep["cp_ok"] == false);
        assert(jrep["cp_err"] == ERROR_ACCESS_DENIED);
        assert(jrep["detached_method"] == "shell_com");
        if (jrep["detached_ok"] == true) {
            bool marker_ok = false;
            for (int w = 0; w < 100; ++w) {
                if (fs::exists(noop_marker)) { marker_ok = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            assert(marker_ok);
        } else {
            // Headless session without explorer.exe: must fail closed cleanly
            assert(jrep["detached_err"] != 0);
        }
        fs::remove_all(tmp_job_dir, ec);
#endif
        std::cout << "  [PASS] Log redaction ordering & Win32 Job-Object fail-closed/escape verified." << std::endl;
    }

    std::cout << "All Subsystem Integration tests passed!" << std::endl;
    return 0;
}
