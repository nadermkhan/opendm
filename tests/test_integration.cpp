#include "core/batch_importer.hpp"
#include "core/site_logon.hpp"
#include "cli/cli_parser.hpp"
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
    std::cout << "  ✓ Site logon URL matching passed." << std::endl;

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
    std::cout << "  ✓ IDM EF2 batch parser & serializer passed." << std::endl;

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
    std::cout << "  ✓ IDM CLI suite argument parser passed." << std::endl;

    std::cout << "All Subsystem Integration tests passed!" << std::endl;
    return 0;
}
