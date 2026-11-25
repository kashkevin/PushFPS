// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "clarity" (round 2): lossless files of matched frames for the detail-loss question.
// With VEYRA_TEST_RP_PROBE_FRAMES=<1..4> the pipeline probe (PushPipelineProbe.cpp) writes, for the first frame of each
// of its first N pairs, three pictures of the SAME presented frame, copied in the same command list:
//   raw     the decoded picture after YUV to linear RGB, before any processing (float16, linear light)
//   output  the graph's output at its work size (what frame generation, recording and the presenter read)
//   screen  the swap chain's back buffer after the presenter's blit, before Present (what the window shows)
// as NumPy .npy arrays (height, width, 3), alpha dropped, plus one JSON per capture with the frame's timestamp, sizes,
// formats and encodings. .npy is lossless and needs no image codec in the app; tools/clarity_frames/compare.py reads
// them, writes PNG copies and computes the numbers. Portable, header-only, pure (file writing is the caller's).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace veyra::engine {

enum class PushDumpPixels : uint8_t { Rgba8, Bgra8, Rgb10a2, Rgba16f };

// The NumPy dtype of the packed samples: 8-bit -> '|u1', 10-bit -> '<u2' (values 0..1023), half -> '<f2'.
inline const char* pushNpyDescr(PushDumpPixels pixels) noexcept {
    return pixels == PushDumpPixels::Rgb10a2 ? "<u2" : pixels == PushDumpPixels::Rgba16f ? "<f2" : "|u1";
}
inline std::size_t pushNpySampleBytes(PushDumpPixels pixels) noexcept {
    return pixels == PushDumpPixels::Rgba8 || pixels == PushDumpPixels::Bgra8 ? 1u : 2u;
}

// A .npy version 1.0 header for a C-order array of shape (height, width, 3): magic, version, 2-byte little-endian
// header length, the dict text padded with spaces and ended by '\n' so that the whole header is a multiple of 64.
inline std::string pushNpyHeader(const char* descr, uint32_t height, uint32_t width) {
    char dict[160];
    const int n = std::snprintf(dict, sizeof dict, "{'descr': '%s', 'fortran_order': False, 'shape': (%lu, %lu, 3), }",
        descr, static_cast<unsigned long>(height), static_cast<unsigned long>(width));
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof dict) return {};
    std::string text(dict, static_cast<std::size_t>(n));
    const std::size_t fixed = 10;                         // magic (6) + version (2) + length (2)
    std::size_t total = fixed + text.size() + 1;          // + '\n'
    const std::size_t padded = (total + 63) / 64 * 64;
    text.append(padded - total, ' ');
    text.push_back('\n');
    const std::size_t length = text.size();
    std::string header("\x93NUMPY\x01\x00", 8);
    header.push_back(static_cast<char>(length & 0xffu));
    header.push_back(static_cast<char>((length >> 8) & 0xffu));
    header += text;
    return header;
}

// Packs a row-pitched picture into tightly packed RGB samples (little-endian), alpha dropped. pitch is in bytes.
// Returns false for an empty picture or a pitch shorter than a row.
inline bool pushPackRgb(const uint8_t* data, std::size_t pitch, uint32_t width, uint32_t height, PushDumpPixels pixels,
                        std::vector<uint8_t>& out) {
    out.clear();
    if (!data || !width || !height) return false;
    const std::size_t inBytes = pixels == PushDumpPixels::Rgba16f ? 8u : 4u;
    if (pitch < std::size_t(width) * inBytes) return false;
    const std::size_t sample = pushNpySampleBytes(pixels);
    out.resize(std::size_t(width) * height * 3u * sample);
    uint8_t* to = out.data();
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = data + std::size_t(y) * pitch;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* p = row + std::size_t(x) * inBytes;
            switch (pixels) {
            case PushDumpPixels::Rgba8: to[0] = p[0]; to[1] = p[1]; to[2] = p[2]; to += 3; break;
            case PushDumpPixels::Bgra8: to[0] = p[2]; to[1] = p[1]; to[2] = p[0]; to += 3; break;
            case PushDumpPixels::Rgb10a2: {
                uint32_t v = 0; std::memcpy(&v, p, 4);
                const uint16_t c[3] = {static_cast<uint16_t>(v & 0x3ffu), static_cast<uint16_t>((v >> 10) & 0x3ffu),
                                       static_cast<uint16_t>((v >> 20) & 0x3ffu)};
                for (int i = 0; i < 3; ++i) { to[0] = static_cast<uint8_t>(c[i] & 0xffu); to[1] = static_cast<uint8_t>(c[i] >> 8); to += 2; }
                break;
            }
            case PushDumpPixels::Rgba16f: std::memcpy(to, p, 6); to += 6; break;   // R, G, B halfs as stored
            }
        }
    }
    return true;
}

// One capture's description (the JSON beside the three files).
struct PushFrameDumpPicture {
    std::string file;          // file name only, never a path
    uint32_t width = 0, height = 0;
    const char* dtype = "";    // pushNpyDescr
    const char* encoding = ""; // "linear-float16", "srgb8", "pq10", "scrgb-float16"
    bool present = false;
};
struct PushFrameDumpInfo {
    uint64_t probe = 0;        // the probe's run number in this process
    uint32_t pair = 0;         // pair index within the run (the first frame of that pair)
    int64_t pts100ns = 0;      // the frame's source timestamp (100 ns), 0 = unknown
    uint32_t hdrMode = 0;      // 0 SDR, 1 scRGB, 2 HDR10
    float ditherStep = 0;      // the graph's output dither step (0 = none)
    // How the presenter scaled the output to the screen: 1 = PresentBlit FineSample (Catmull-Rom clamped to the 2x2
    // texels; EnhanceGraph::highQualityPresentation), 0 = one bilinear tap, -1 = unknown. In SDR it samples the 8-bit
    // sRGB codes as they are (UNORM view, no sRGB decode): the interpolation is NOT in linear light.
    int presentFine = -1;
    std::string stamp;         // per-process stamp of the file names (pushDumpStamp)
    PushFrameDumpPicture raw, output, screen;
};

// A per-process stamp for the file names: the UTC time as yyyymmddThhmmss (civil date from days since 1970,
// H. Hinnant's days_from_civil inverse), so captures of two app starts never mix.
inline std::string pushDumpStamp(int64_t unixSeconds) {
    if (unixSeconds < 0) unixSeconds = 0;
    const int64_t days = unixSeconds / 86400, rest = unixSeconds % 86400;
    const int64_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    char text[32];
    std::snprintf(text, sizeof text, "%04lld%02lld%02lldT%02lld%02lld%02lld", static_cast<long long>(y), static_cast<long long>(m),
        static_cast<long long>(d), static_cast<long long>(rest / 3600), static_cast<long long>(rest / 60 % 60), static_cast<long long>(rest % 60));
    return text;
}

inline const char* pushDumpEncoding(PushDumpPixels pixels, bool raw) noexcept {
    if (raw) return "linear-float16";
    return pixels == PushDumpPixels::Rgb10a2 ? "pq10" : pixels == PushDumpPixels::Rgba16f ? "scrgb-float16" : "srgb8";
}

inline std::string pushFrameDumpJson(const PushFrameDumpInfo& info) {
    const auto picture = [](const char* name, const PushFrameDumpPicture& p) {
        if (!p.present) return std::string("\"") + name + "\": null";
        char text[320];
        std::snprintf(text, sizeof text, "\"%s\": {\"file\": \"%s\", \"width\": %lu, \"height\": %lu, \"dtype\": \"%s\", \"encoding\": \"%s\"}",
            name, p.file.c_str(), static_cast<unsigned long>(p.width), static_cast<unsigned long>(p.height), p.dtype, p.encoding);
        return std::string(text);
    };
    char head[384];
    const char* fine = info.presentFine < 0 ? "null" : info.presentFine ? "true" : "false";
    std::string stamp;
    for (const char c : info.stamp) if ((c >= '0' && c <= '9') || c == 'T') stamp += c;   // digits and T only
    std::snprintf(head, sizeof head, "{\"schema\": 1, \"stamp\": \"%s\", \"probe\": %llu, \"pair\": %lu, \"pts100ns\": %lld, \"hdrMode\": %lu, \"ditherStep\": %.6g, \"presentFine\": %s, ",
        stamp.c_str(), static_cast<unsigned long long>(info.probe), static_cast<unsigned long>(info.pair), static_cast<long long>(info.pts100ns),
        static_cast<unsigned long>(info.hdrMode), static_cast<double>(info.ditherStep), fine);
    return std::string(head) + picture("raw", info.raw) + ", " + picture("output", info.output) + ", " + picture("screen", info.screen) + "}\n";
}

// VEYRA_TEST_RP_PROBE_FRAMES: digits only, 1..4; anything else = 0 (no files).
inline uint32_t pushProbeFramesFromText(const wchar_t* text, std::size_t length) noexcept {
    if (!text || length == 0 || length > 1) return 0;
    const wchar_t c = text[0];
    return c >= L'1' && c <= L'4' ? static_cast<uint32_t>(c - L'0') : 0u;
}

} // namespace veyra::engine
