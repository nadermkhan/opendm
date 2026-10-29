#include "core/media_muxer.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <cassert>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

static void write_be32(uint8_t* p, uint32_t val) {
    p[0] = (val >> 24) & 0xFF;
    p[1] = (val >> 16) & 0xFF;
    p[2] = (val >> 8) & 0xFF;
    p[3] = val & 0xFF;
}

// Helper to construct a synthetic valid MP4 file with minimal atoms
static void create_synthetic_mp4(
    const std::string& filename,
    uint32_t track_id,
    const std::string& handler_type, // "vide" or "soun"
    const std::vector<uint8_t>& sample_payload) {

    // 1. ftyp box (20 bytes)
    std::vector<uint8_t> ftyp = {
        0x00, 0x00, 0x00, 0x14, // size = 20
        'f', 't', 'y', 'p',
        'i', 's', 'o', 'm',
        0x00, 0x00, 0x02, 0x00,
        'i', 's', 'o', 'm'
    };

    // 2. mdat box
    uint32_t mdat_size = 8 + static_cast<uint32_t>(sample_payload.size());
    std::vector<uint8_t> mdat(mdat_size);
    write_be32(&mdat[0], mdat_size);
    mdat[4] = 'm'; mdat[5] = 'd'; mdat[6] = 'a'; mdat[7] = 't';
    std::memcpy(&mdat[8], sample_payload.data(), sample_payload.size());

    // 3. trak -> tkhd
    // tkhd version 0 is 92 bytes (size 92: 4 size, 4 'tkhd', 4 ver/flags, 8 ctime/mtime, 4 track_id, ...)
    std::vector<uint8_t> tkhd(92, 0);
    write_be32(&tkhd[0], 92);
    std::memcpy(&tkhd[4], "tkhd", 4);
    tkhd[11] = 0x01; // track enabled
    write_be32(&tkhd[20], track_id);

    // trak -> mdia -> hdlr
    std::vector<uint8_t> hdlr(32, 0);
    write_be32(&hdlr[0], 32);
    std::memcpy(&hdlr[4], "hdlr", 4);
    std::memcpy(&hdlr[16], handler_type.c_str(), 4);

    // trak -> mdia -> minf -> stbl -> stco (single chunk starting at mdat payload offset = 28)
    std::vector<uint8_t> stco(20, 0);
    write_be32(&stco[0], 20);
    std::memcpy(&stco[4], "stco", 4);
    write_be32(&stco[12], 1); // 1 entry
    write_be32(&stco[16], 28); // points to offset 28 (right after ftyp 20 + mdat header 8)

    // assemble stbl
    std::vector<uint8_t> stbl;
    uint32_t stbl_size = 8 + static_cast<uint32_t>(stco.size());
    stbl.resize(stbl_size);
    write_be32(&stbl[0], stbl_size);
    std::memcpy(&stbl[4], "stbl", 4);
    std::memcpy(&stbl[8], stco.data(), stco.size());

    // assemble minf
    std::vector<uint8_t> minf;
    uint32_t minf_size = 8 + static_cast<uint32_t>(stbl.size());
    minf.resize(minf_size);
    write_be32(&minf[0], minf_size);
    std::memcpy(&minf[4], "minf", 4);
    std::memcpy(&minf[8], stbl.data(), stbl.size());

    // assemble mdia
    std::vector<uint8_t> mdia;
    uint32_t mdia_size = 8 + static_cast<uint32_t>(hdlr.size() + minf.size());
    mdia.resize(mdia_size);
    write_be32(&mdia[0], mdia_size);
    std::memcpy(&mdia[4], "mdia", 4);
    std::memcpy(&mdia[8], hdlr.data(), hdlr.size());
    std::memcpy(&mdia[8 + hdlr.size()], minf.data(), minf.size());

    // assemble trak
    std::vector<uint8_t> trak;
    uint32_t trak_size = 8 + static_cast<uint32_t>(tkhd.size() + mdia.size());
    trak.resize(trak_size);
    write_be32(&trak[0], trak_size);
    std::memcpy(&trak[4], "trak", 4);
    std::memcpy(&trak[8], tkhd.data(), tkhd.size());
    std::memcpy(&trak[8 + tkhd.size()], mdia.data(), mdia.size());

    // mvhd (108 bytes)
    std::vector<uint8_t> mvhd(108, 0);
    write_be32(&mvhd[0], 108);
    std::memcpy(&mvhd[4], "mvhd", 4);
    write_be32(&mvhd[104], track_id + 1); // next_track_id

    // assemble moov
    std::vector<uint8_t> moov;
    uint32_t moov_size = 8 + static_cast<uint32_t>(mvhd.size() + trak.size());
    moov.resize(moov_size);
    write_be32(&moov[0], moov_size);
    std::memcpy(&moov[4], "moov", 4);
    std::memcpy(&moov[8], mvhd.data(), mvhd.size());
    std::memcpy(&moov[8 + mvhd.size()], trak.data(), trak.size());

    // Write file
    std::ofstream out(filename, std::ios::binary);
    out.write(reinterpret_cast<const char*>(ftyp.data()), ftyp.size());
    out.write(reinterpret_cast<const char*>(mdat.data()), mdat.size());
    out.write(reinterpret_cast<const char*>(moov.data()), moov.size());
}

int main() {
    std::cout << "=== Running OpenDM Media Muxer Tests ===" << std::endl;

    std::string video_test = "/tmp/test_video.mp4";
    std::string audio_test = "/tmp/test_audio.m4a";
    std::string muxed_test = "/tmp/test_muxed.mp4";

    // Clean up
    fs::remove(video_test);
    fs::remove(audio_test);
    fs::remove(muxed_test);

    // Video sample data: 128 bytes
    std::vector<uint8_t> video_payload(128, 0xAA);
    create_synthetic_mp4(video_test, 1, "vide", video_payload);

    // Audio sample data: 64 bytes
    std::vector<uint8_t> audio_payload(64, 0xBB);
    create_synthetic_mp4(audio_test, 1, "soun", audio_payload);

    std::cout << "[Test 1] Muxing video.mp4 and audio.m4a..." << std::endl;
    std::string err;
    double reported_pct = 0.0;
    bool ok = opendm::MediaMuxer::mux_mp4_and_mp4(
        video_test, audio_test, muxed_test,
        [&](double pct) { reported_pct = pct; },
        &err);

    if (!ok) {
        std::cerr << "Muxing failed: " << err << std::endl;
        return 1;
    }

    assert(ok);
    assert(reported_pct >= 99.0);
    assert(fs::exists(muxed_test));
    assert(fs::file_size(muxed_test) > 0);
    std::cout << "  ✓ Muxing succeeded! Output file size: " << fs::file_size(muxed_test) << " bytes." << std::endl;

    // Verify output contents
    std::ifstream muxed_in(muxed_test, std::ios::binary);
    std::vector<uint8_t> file_bytes((std::istreambuf_iterator<char>(muxed_in)),
                                     std::istreambuf_iterator<char>());

    // Verify ftyp exists
    assert(file_bytes.size() > 20);
    assert(std::memcmp(&file_bytes[4], "ftyp", 4) == 0);

    // Verify mdat contains both video payload (0xAA) and audio payload (0xBB)
    assert(std::memcmp(&file_bytes[20], "\x00\x00\x00\xC8mdat", 8) == 0); // 8 header + 128 + 64 = 200 = 0xC8
    assert(file_bytes[28] == 0xAA);
    assert(file_bytes[28 + 127] == 0xAA);
    assert(file_bytes[28 + 128] == 0xBB);
    assert(file_bytes[28 + 128 + 63] == 0xBB);
    std::cout << "  ✓ Media payloads combined accurately in mdat." << std::endl;

    // Test 2: TS segments concatenation
    std::cout << "[Test 2] Concatenating MPEG-TS segments..." << std::endl;
    std::string ts1 = "/tmp/seg1.ts";
    std::string ts2 = "/tmp/seg2.ts";
    std::string ts_out = "/tmp/combined.ts";

    std::vector<uint8_t> seg1(188, 0x47);
    seg1[1] = 0x01;
    std::vector<uint8_t> seg2(188, 0x47);
    seg2[1] = 0x02;

    {
        std::ofstream f1(ts1, std::ios::binary);
        f1.write(reinterpret_cast<const char*>(seg1.data()), 188);
        std::ofstream f2(ts2, std::ios::binary);
        f2.write(reinterpret_cast<const char*>(seg2.data()), 188);
    }

    ok = opendm::MediaMuxer::concat_ts_segments({ts1, ts2}, ts_out, nullptr, &err);
    assert(ok);
    assert(fs::file_size(ts_out) == 376);
    std::cout << "  ✓ TS segments concatenated successfully (376 bytes)." << std::endl;

    // Test 3: Fragmented MP4 rejection guard
    std::cout << "[Test 3] Verifying fragmented MP4 guard..." << std::endl;
    std::string fmp4_test = "/tmp/test_fmp4.mp4";
    {
        std::ofstream f(fmp4_test, std::ios::binary);
        // ftyp
        uint8_t ftyp_hdr[8] = {0x00, 0x00, 0x00, 0x14, 'f', 't', 'y', 'p'};
        f.write(reinterpret_cast<char*>(ftyp_hdr), 8);
        f.write("isom\0\0\x02\0isom", 12);
        // moov
        uint8_t moov_hdr[8] = {0x00, 0x00, 0x00, 0x10, 'm', 'o', 'o', 'v'};
        f.write(reinterpret_cast<char*>(moov_hdr), 8);
        f.write("\0\0\0\x08mvhd", 8);
        // moof
        uint8_t moof_hdr[8] = {0x00, 0x00, 0x00, 0x10, 'm', 'o', 'o', 'f'};
        f.write(reinterpret_cast<char*>(moof_hdr), 8);
        f.write("\0\0\0\x08mfhd", 8);
        // mdat
        uint8_t mdat_hdr[8] = {0x00, 0x00, 0x00, 0x10, 'm', 'd', 'a', 't'};
        f.write(reinterpret_cast<char*>(mdat_hdr), 8);
        f.write("12345678", 8);
    }

    std::string fmp4_err;
    bool fmp4_ok = opendm::MediaMuxer::mux_mp4_and_mp4(fmp4_test, audio_test, muxed_test, nullptr, &fmp4_err);
    assert(!fmp4_ok);
    assert(fmp4_err.find("Fragmented MP4") != std::string::npos);
    std::cout << "  ✓ Fragmented MP4 correctly rejected without truncation." << std::endl;

    // Clean up temporary test files
    fs::remove(video_test);
    fs::remove(audio_test);
    fs::remove(muxed_test);
    fs::remove(ts1);
    fs::remove(ts2);
    fs::remove(ts_out);
    fs::remove(fmp4_test);

    std::cout << "All Media Muxer tests passed!" << std::endl;
    return 0;
}
