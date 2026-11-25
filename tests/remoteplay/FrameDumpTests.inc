// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/engine/PushFrameDump.h: the lossless matched-frame files of the clarity retest.
#include "veyra/engine/PushFrameDump.h"
#include <cstring>
#include <string>
#include <vector>

TEST(frame_dump_npy_header_is_numpy_1_0_and_64_aligned){
    using namespace veyra::engine;
    for (const char* descr : {"|u1", "<u2", "<f2"}) {
        const std::string h = pushNpyHeader(descr, 1080, 1920);
        CHECK(h.size() % 64 == 0);
        CHECK(h.compare(0, 6, "\x93NUMPY") == 0 && h[6] == '\x01' && h[7] == '\x00');
        const std::size_t length = std::size_t(static_cast<unsigned char>(h[8])) | (std::size_t(static_cast<unsigned char>(h[9])) << 8);
        CHECK(length + 10 == h.size());
        CHECK(h.back() == '\n');
        CHECK(h.find(std::string("'descr': '") + descr + "'") != std::string::npos);
        CHECK(h.find("'shape': (1080, 1920, 3)") != std::string::npos);
        CHECK(h.find("'fortran_order': False") != std::string::npos);
    }
    CHECK(pushNpyHeader("|u1", 2560u * 100u, 4096).size() % 64 == 0);
}

TEST(frame_dump_packs_each_layout_to_plain_rgb){
    using namespace veyra::engine;
    std::vector<uint8_t> out;
    // two pixels, 10 bytes pitch (padding must be skipped), two rows
    const uint8_t rgba[20] = {1,2,3,255, 4,5,6,255, 9,9, 7,8,9,255, 10,11,12,255, 9,9};
    CHECK(pushPackRgb(rgba, 10, 2, 2, PushDumpPixels::Rgba8, out));
    CHECK((out == std::vector<uint8_t>{1,2,3, 4,5,6, 7,8,9, 10,11,12}));
    CHECK(pushPackRgb(rgba, 10, 2, 2, PushDumpPixels::Bgra8, out));
    CHECK((out == std::vector<uint8_t>{3,2,1, 6,5,4, 9,8,7, 12,11,10}));
    // R=1023, G=512, B=1, A=3
    const uint32_t word = 1023u | (512u << 10) | (1u << 20) | (3u << 30);
    uint8_t ten[4]; std::memcpy(ten, &word, 4);
    CHECK(pushPackRgb(ten, 4, 1, 1, PushDumpPixels::Rgb10a2, out));
    CHECK((out == std::vector<uint8_t>{0xff,0x03, 0x00,0x02, 0x01,0x00}));
    const uint8_t half[8] = {0x00,0x3c, 0x00,0x38, 0x00,0x00, 0x00,0x3c};   // 1.0, 0.5, 0.0, alpha 1.0
    CHECK(pushPackRgb(half, 8, 1, 1, PushDumpPixels::Rgba16f, out));
    CHECK((out == std::vector<uint8_t>{0x00,0x3c, 0x00,0x38, 0x00,0x00}));
    CHECK(!pushPackRgb(rgba, 7, 2, 1, PushDumpPixels::Rgba8, out));     // pitch shorter than a row
    CHECK(!pushPackRgb(nullptr, 8, 1, 1, PushDumpPixels::Rgba8, out));
    CHECK(std::string(pushNpyDescr(PushDumpPixels::Rgb10a2)) == "<u2" && std::string(pushNpyDescr(PushDumpPixels::Rgba16f)) == "<f2" &&
          std::string(pushNpyDescr(PushDumpPixels::Bgra8)) == "|u1");
}

TEST(frame_dump_json_names_files_only_and_marks_missing_pictures){
    using namespace veyra::engine;
    PushFrameDumpInfo info;
    info.probe = 3; info.pair = 1; info.pts100ns = 123456789; info.hdrMode = 0;
    info.raw = {"p3-f1-raw.npy", 1920, 1080, "<f2", "linear-float16", true};
    info.output = {"p3-f1-output.npy", 2560, 1440, "|u1", "srgb8", true};
    const std::string json = pushFrameDumpJson(info);
    CHECK(json.find("\"schema\": 1") != std::string::npos);
    CHECK(json.find("\"pts100ns\": 123456789") != std::string::npos);
    CHECK(json.find("\"raw\": {\"file\": \"p3-f1-raw.npy\", \"width\": 1920, \"height\": 1080, \"dtype\": \"<f2\", \"encoding\": \"linear-float16\"}") != std::string::npos);
    CHECK(json.find("\"screen\": null") != std::string::npos);
    CHECK(json.find('\\') == std::string::npos && json.find(':') != std::string::npos);
    CHECK(std::string(pushDumpEncoding(PushDumpPixels::Rgb10a2, false)) == "pq10" && std::string(pushDumpEncoding(PushDumpPixels::Rgba8, true)) == "linear-float16");
    CHECK(json.find("\"presentFine\": null") != std::string::npos && json.find("\"stamp\": \"\"") != std::string::npos);
    info.presentFine = 1; info.stamp = "20261121T183000/x";
    const std::string fine = pushFrameDumpJson(info);
    CHECK(fine.find("\"presentFine\": true") != std::string::npos && fine.find("\"stamp\": \"20261121T183000\"") != std::string::npos);
    info.presentFine = 0;
    CHECK(pushFrameDumpJson(info).find("\"presentFine\": false") != std::string::npos);
}

TEST(frame_dump_stamp_is_the_utc_civil_time){
    using namespace veyra::engine;
    CHECK(pushDumpStamp(0) == "19700101T000000");
    CHECK(pushDumpStamp(951782400) == "20000229T000000");          // a leap day
    CHECK(pushDumpStamp(1795285800) == "20261121T183000");
    CHECK(pushDumpStamp(-5) == "19700101T000000");
}

TEST(frame_dump_switch_takes_one_digit_from_1_to_4){
    using namespace veyra::engine;
    CHECK(pushProbeFramesFromText(L"1", 1) == 1 && pushProbeFramesFromText(L"4", 1) == 4);
    CHECK(pushProbeFramesFromText(L"0", 1) == 0 && pushProbeFramesFromText(L"5", 1) == 0);
    CHECK(pushProbeFramesFromText(L"12", 2) == 0 && pushProbeFramesFromText(L"", 0) == 0 && pushProbeFramesFromText(nullptr, 1) == 0);
}
