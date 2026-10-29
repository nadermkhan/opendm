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

// Helper to construct a synthetic valid fragmented MP4 (fMP4) with moof/traf/trun
static void create_synthetic_fmp4(
    const std::string& filename,
    uint32_t track_id,
    const std::string& handler_type,
    const std::vector<uint8_t>& sample_payload) {

    // 1. ftyp box (20 bytes)
    std::vector<uint8_t> ftyp = {
        0x00, 0x00, 0x00, 0x14,
        'f', 't', 'y', 'p',
        'i', 's', 'o', 'm',
        0x00, 0x00, 0x02, 0x00,
        'i', 's', 'o', 'm'
    };

    // 2. moov with trak -> mdia -> minf -> stbl -> stsd
    std::vector<uint8_t> tkhd(92, 0);
    write_be32(&tkhd[0], 92);
    std::memcpy(&tkhd[4], "tkhd", 4);
    tkhd[11] = 0x01;
    write_be32(&tkhd[20], track_id);

    std::vector<uint8_t> mdhd(32, 0);
    write_be32(&mdhd[0], 32);
    std::memcpy(&mdhd[4], "mdhd", 4);

    std::vector<uint8_t> hdlr(32, 0);
    write_be32(&hdlr[0], 32);
    std::memcpy(&hdlr[4], "hdlr", 4);
    std::memcpy(&hdlr[16], handler_type.c_str(), 4);

    std::vector<uint8_t> stsd(16, 0);
    write_be32(&stsd[0], 16);
    std::memcpy(&stsd[4], "stsd", 4);

    std::vector<uint8_t> stbl(8 + stsd.size(), 0);
    write_be32(&stbl[0], static_cast<uint32_t>(stbl.size()));
    std::memcpy(&stbl[4], "stbl", 4);
    std::memcpy(&stbl[8], stsd.data(), stsd.size());

    std::vector<uint8_t> minf(8 + stbl.size(), 0);
    write_be32(&minf[0], static_cast<uint32_t>(minf.size()));
    std::memcpy(&minf[4], "minf", 4);
    std::memcpy(&minf[8], stbl.data(), stbl.size());

    std::vector<uint8_t> mdia(8 + mdhd.size() + hdlr.size() + minf.size(), 0);
    write_be32(&mdia[0], static_cast<uint32_t>(mdia.size()));
    std::memcpy(&mdia[4], "mdia", 4);
    size_t mp = 8;
    std::memcpy(&mdia[mp], mdhd.data(), mdhd.size()); mp += mdhd.size();
    std::memcpy(&mdia[mp], hdlr.data(), hdlr.size()); mp += hdlr.size();
    std::memcpy(&mdia[mp], minf.data(), minf.size());

    std::vector<uint8_t> trak(8 + tkhd.size() + mdia.size(), 0);
    write_be32(&trak[0], static_cast<uint32_t>(trak.size()));
    std::memcpy(&trak[4], "trak", 4);
    std::memcpy(&trak[8], tkhd.data(), tkhd.size());
    std::memcpy(&trak[8 + tkhd.size()], mdia.data(), mdia.size());

    std::vector<uint8_t> mvhd(108, 0);
    write_be32(&mvhd[0], 108);
    std::memcpy(&mvhd[4], "mvhd", 4);
    write_be32(&mvhd[104], track_id + 1);

    std::vector<uint8_t> moov(8 + mvhd.size() + trak.size(), 0);
    write_be32(&moov[0], static_cast<uint32_t>(moov.size()));
    std::memcpy(&moov[4], "moov", 4);
    std::memcpy(&moov[8], mvhd.data(), mvhd.size());
    std::memcpy(&moov[8 + mvhd.size()], trak.data(), trak.size());

    // 3. moof with traf -> tfhd & trun
    std::vector<uint8_t> tfhd(16, 0);
    write_be32(&tfhd[0], 16);
    std::memcpy(&tfhd[4], "tfhd", 4);
    write_be32(&tfhd[8], 0);
    write_be32(&tfhd[12], track_id);

    std::vector<uint8_t> trun(28, 0);
    write_be32(&trun[0], 28);
    std::memcpy(&trun[4], "trun", 4);
    trun[8] = 0;
    trun[9] = 0x00;
    trun[10] = 0x03;
    trun[11] = 0x01; // data-offset (0x1) | sample-duration (0x100) | sample-size (0x200)
    write_be32(&trun[12], 1); // 1 sample
    write_be32(&trun[16], 0); // data_offset
    write_be32(&trun[20], 1000); // sample duration
    write_be32(&trun[24], static_cast<uint32_t>(sample_payload.size()));

    std::vector<uint8_t> traf(8 + tfhd.size() + trun.size(), 0);
    write_be32(&traf[0], static_cast<uint32_t>(traf.size()));
    std::memcpy(&traf[4], "traf", 4);
    std::memcpy(&traf[8], tfhd.data(), tfhd.size());
    std::memcpy(&traf[8 + tfhd.size()], trun.data(), trun.size());

    std::vector<uint8_t> mfhd(16, 0);
    write_be32(&mfhd[0], 16);
    std::memcpy(&mfhd[4], "mfhd", 4);
    write_be32(&mfhd[12], 1);

    std::vector<uint8_t> moof(8 + mfhd.size() + traf.size(), 0);
    write_be32(&moof[0], static_cast<uint32_t>(moof.size()));
    std::memcpy(&moof[4], "moof", 4);
    std::memcpy(&moof[8], mfhd.data(), mfhd.size());
    std::memcpy(&moof[8 + mfhd.size()], traf.data(), traf.size());

    // 4. mdat
    uint32_t mdat_size = 8 + static_cast<uint32_t>(sample_payload.size());
    std::vector<uint8_t> mdat(mdat_size);
    write_be32(&mdat[0], mdat_size);
    mdat[4] = 'm'; mdat[5] = 'd'; mdat[6] = 'a'; mdat[7] = 't';
    std::memcpy(&mdat[8], sample_payload.data(), sample_payload.size());

    std::ofstream out(filename, std::ios::binary);
    out.write(reinterpret_cast<const char*>(ftyp.data()), ftyp.size());
    out.write(reinterpret_cast<const char*>(moov.data()), moov.size());
    out.write(reinterpret_cast<const char*>(moof.data()), moof.size());
    out.write(reinterpret_cast<const char*>(mdat.data()), mdat.size());
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

    std::cout << "[Test 1] Muxing video.mp4 and audio.m4a (Fast-Start [ftyp][moov][mdat])..." << std::endl;
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

    // Verify output contents: fast-start layout [ftyp] [moov] [mdat]
    std::ifstream muxed_in(muxed_test, std::ios::binary);
    std::vector<uint8_t> file_bytes((std::istreambuf_iterator<char>(muxed_in)),
                                     std::istreambuf_iterator<char>());

    // Verify ftyp exists
    assert(file_bytes.size() > 20);
    assert(std::memcmp(&file_bytes[4], "ftyp", 4) == 0);

    // Verify fast-start: moov immediately follows ftyp (at offset 20)
    uint32_t moov_sz = (static_cast<uint32_t>(file_bytes[20]) << 24) |
                       (static_cast<uint32_t>(file_bytes[21]) << 16) |
                       (static_cast<uint32_t>(file_bytes[22]) << 8) |
                       static_cast<uint32_t>(file_bytes[23]);
    assert(std::memcmp(&file_bytes[24], "moov", 4) == 0);

    // Verify mdat comes immediately after moov
    size_t mdat_offset = 20 + moov_sz;
    assert(mdat_offset + 8 <= file_bytes.size());
    assert(std::memcmp(&file_bytes[mdat_offset], "\x00\x00\x00\xC8mdat", 8) == 0); // 8 header + 128 + 64 = 200 = 0xC8
    size_t mdat_payload = mdat_offset + 8;
    assert(file_bytes[mdat_payload] == 0xAA);
    assert(file_bytes[mdat_payload + 127] == 0xAA);
    assert(file_bytes[mdat_payload + 128] == 0xBB);
    assert(file_bytes[mdat_payload + 128 + 63] == 0xBB);
    std::cout << "  ✓ Fast-start [ftyp][moov][mdat] verified and media payloads combined accurately." << std::endl;

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

    // Test 3: Fragmented MP4 (fMP4 / DASH) defragmentation & muxing
    std::cout << "[Test 3] Verifying fragmented MP4 (fMP4) defragmentation & muxing..." << std::endl;
    std::string fmp4_test = "/tmp/test_fmp4.mp4";
    std::string fmp4_muxed = "/tmp/test_fmp4_muxed.mp4";
    fs::remove(fmp4_test);
    fs::remove(fmp4_muxed);

    std::vector<uint8_t> fmp4_video_payload(128, 0xCC);
    create_synthetic_fmp4(fmp4_test, 1, "vide", fmp4_video_payload);

    std::string fmp4_err;
    bool fmp4_ok = opendm::MediaMuxer::mux_mp4_and_mp4(fmp4_test, audio_test, fmp4_muxed, nullptr, &fmp4_err);
    if (!fmp4_ok) {
        std::cerr << "fMP4 muxing failed: " << fmp4_err << std::endl;
    }
    assert(fmp4_ok);
    assert(fs::exists(fmp4_muxed));
    assert(fs::file_size(fmp4_muxed) > 0);
    std::cout << "  ✓ Fragmented MP4 successfully defragmented and muxed into fast-start MP4." << std::endl;

    // Verify malformed fMP4 rejection
    std::string bad_fmp4 = "/tmp/test_bad_fmp4.mp4";
    {
        std::ofstream f(bad_fmp4, std::ios::binary);
        uint8_t ftyp_hdr[8] = {0x00, 0x00, 0x00, 0x14, 'f', 't', 'y', 'p'};
        f.write(reinterpret_cast<char*>(ftyp_hdr), 8);
        f.write("isom\0\0\x02\0isom", 12);
        uint8_t moov_hdr[8] = {0x00, 0x00, 0x00, 0x10, 'm', 'o', 'o', 'v'};
        f.write(reinterpret_cast<char*>(moov_hdr), 8);
        f.write("\0\0\0\x08mvhd", 8);
        uint8_t moof_hdr[8] = {0x00, 0x00, 0x00, 0x10, 'm', 'o', 'o', 'f'};
        f.write(reinterpret_cast<char*>(moof_hdr), 8);
        f.write("\0\0\0\x08mfhd", 8);
        uint8_t mdat_hdr[8] = {0x00, 0x00, 0x00, 0x10, 'm', 'd', 'a', 't'};
        f.write(reinterpret_cast<char*>(mdat_hdr), 8);
        f.write("12345678", 8);
    }
    std::string bad_err;
    bool bad_ok = opendm::MediaMuxer::mux_mp4_and_mp4(bad_fmp4, audio_test, muxed_test, nullptr, &bad_err);
    assert(!bad_ok);
    assert(bad_err.find("defragment") != std::string::npos || bad_err.find("Fragmented") != std::string::npos);
    std::cout << "  ✓ Corrupted fMP4 correctly rejected with error." << std::endl;

    // Clean up temporary test files
    fs::remove(video_test);
    fs::remove(audio_test);
    fs::remove(muxed_test);
    fs::remove(ts1);
    fs::remove(ts2);
    fs::remove(ts_out);
    fs::remove(fmp4_test);
    fs::remove(fmp4_muxed);
    fs::remove(bad_fmp4);

    std::cout << "All Media Muxer tests passed!" << std::endl;
    return 0;
}
