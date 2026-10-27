#include "../src/core/chunk_manager.hpp"
#include "../src/core/speed_limiter.hpp"
#include <iostream>
#include <cassert>
#include <fstream>
#include <filesystem>
#include <vector>
#include <numeric>

void test_initialization() {
    std::cout << "[TEST] Running test_initialization..." << std::endl;
    opendm::ChunkManager cm;
    std::string temp_dir = "/tmp/opendm_test";
    std::filesystem::create_directories(temp_dir);

    bool ok = cm.init_first_chunk(1024 * 1024, temp_dir, "test1");
    assert(ok);
    assert(cm.get_total_size() == 1024 * 1024);

    opendm::Chunk c0;
    assert(cm.get_chunk(0, c0));
    assert(c0.start_offset == 0);
    assert(c0.end_offset == 1024 * 1024 - 1);
    assert(c0.downloaded_bytes == 0);
    std::cout << "  ✓ Initial chunk initialized correctly [0, " << c0.end_offset << "]" << std::endl;
}

void test_idm_need_new_connection_threshold() {
    std::cout << "[TEST] Running test_idm_need_new_connection_threshold..." << std::endl;
    opendm::ChunkManager cm;
    std::string temp_dir = "/tmp/opendm_test";

    // Below IDM threshold: (2 * 102400 + 3) = 204803 bytes
    int64_t small_size = 200000;
    cm.init_first_chunk(small_size, temp_dir, "small");
    assert(!cm.need_new_connection(1, 8)); // Should NOT create new connection
    std::cout << "  ✓ Correctly rejected new connection for " << small_size << " bytes (< 204,803 bytes)" << std::endl;

    // Above IDM threshold: 500,000 bytes
    opendm::ChunkManager cm2;
    cm2.init_first_chunk(500000, temp_dir, "large");
    assert(cm2.need_new_connection(1, 8)); // SHOULD create new connection
    std::cout << "  ✓ Correctly accepted new connection for 500,000 bytes (> 204,803 bytes)" << std::endl;

    // Max connections reached check
    assert(!cm2.need_new_connection(8, 8));
    std::cout << "  ✓ Respected max_connections ceiling" << std::endl;
}

void test_idm_midpoint_splitting() {
    std::cout << "[TEST] Running test_idm_midpoint_splitting..." << std::endl;
    opendm::ChunkManager cm;
    std::string temp_dir = "/tmp/opendm_test";
    int64_t total_size = 1000000; // 1,000,000 bytes
    cm.init_first_chunk(total_size, temp_dir, "split");

    int target = cm.find_max_free_chunk();
    assert(target == 0);

    int new_id = cm.split_chunk(target, temp_dir, "split");
    assert(new_id == 1);

    opendm::Chunk c0, c1;
    assert(cm.get_chunk(0, c0));
    assert(cm.get_chunk(1, c1));

    // Midpoint: 1,000,000 bytes -> [0, 499999] and [500000, 999999]
    assert(c0.start_offset == 0);
    assert(c0.end_offset == 499999);
    assert(c0.total_size() == 500000);

    assert(c1.start_offset == 500000);
    assert(c1.end_offset == 999999);
    assert(c1.total_size() == 500000);

    std::cout << "  ✓ Chunk 0 split to [0, " << c0.end_offset << "] (" << c0.total_size() << " bytes)" << std::endl;
    std::cout << "  ✓ Chunk 1 created at [" << c1.start_offset << ", " << c1.end_offset << "] (" << c1.total_size() << " bytes)" << std::endl;
}

void test_chunk_assembly() {
    std::cout << "[TEST] Running test_chunk_assembly..." << std::endl;
    opendm::ChunkManager cm;
    std::string temp_dir = "/tmp/opendm_test";
    int64_t total_size = 600000; // 600 KB (> 204,800 bytes threshold)
    cm.init_first_chunk(total_size, temp_dir, "asm");

    int new_id = cm.split_chunk(0, temp_dir, "asm");
    assert(new_id == 1);

    opendm::Chunk c0, c1;
    assert(cm.get_chunk(0, c0));
    assert(cm.get_chunk(1, c1));

    // Write deterministic test patterns into chunk files
    std::vector<uint8_t> p0(c0.total_size(), 0xAA);
    std::vector<uint8_t> p1(c1.total_size(), 0xBB);

    {
        std::ofstream f0(c0.temp_filepath, std::ios::binary);
        f0.write(reinterpret_cast<const char*>(p0.data()), p0.size());
    }
    {
        std::ofstream f1(c1.temp_filepath, std::ios::binary);
        f1.write(reinterpret_cast<const char*>(p1.data()), p1.size());
    }

    cm.update_chunk_progress(0, c0.total_size(), 1000);
    cm.mark_chunk_completed(0);

    cm.update_chunk_progress(1, c1.total_size(), 1000);
    cm.mark_chunk_completed(1);

    assert(cm.all_chunks_completed());

    std::string output_file = temp_dir + "/assembled_result.bin";
    bool joined = cm.join_file(output_file);
    assert(joined);

    // Verify assembled file content
    std::ifstream in(output_file, std::ios::binary);
    std::vector<uint8_t> result((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(result.size() == static_cast<size_t>(total_size));

    for (size_t i = 0; i < p0.size(); ++i) {
        assert(result[i] == 0xAA);
    }
    for (size_t i = 0; i < p1.size(); ++i) {
        assert(result[p0.size() + i] == 0xBB);
    }

    std::cout << "  ✓ Multi-chunk file assembly verified byte-for-byte (" << result.size() << " bytes)" << std::endl;

    // Cleanup
    std::filesystem::remove_all(temp_dir);
}

void test_speed_limiter() {
    std::cout << "[TEST] Running test_speed_limiter..." << std::endl;
    opendm::SpeedLimiter limiter;
    limiter.set_enabled(true);
    limiter.set_limit_bytes_per_sec(100 * 1024); // 100 KB/s
    assert(limiter.is_enabled());
    assert(limiter.get_limit_bytes_per_sec() == 100 * 1024);

    auto t0 = std::chrono::steady_clock::now();
    limiter.throttle(50 * 1024);
    auto t1 = std::chrono::steady_clock::now();
    // 50KB at 100KB/s takes ~500ms
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "  ✓ SpeedLimiter pacing elapsed ms: " << elapsed_ms << " ms" << std::endl;
}

int main() {
    std::cout << "=== OpenDM Core Algorithm Tests ===" << std::endl;
    test_initialization();
    test_idm_need_new_connection_threshold();
    test_idm_midpoint_splitting();
    test_chunk_assembly();
    test_speed_limiter();
    std::cout << "=== All OpenDM Tests PASSED Successfully! ===" << std::endl;
    return 0;
}
