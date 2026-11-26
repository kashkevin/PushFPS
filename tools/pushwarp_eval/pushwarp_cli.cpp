// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Offline: runs the implemented PushWarp rules (include/veyra/pipeline/PushWarp.h, pushWarpReference: the CPU reference
// the GPU passes are checked against on WARP) on frames and a field written by measure.py. No GPU.
//   pushwarp_cli <job.bin> <out.rgba>
//   pushwarp_cli frames ...   two real frames in, the generated frames out, with the flow MODEL (FlowModel.h): see
//                             tools/pushwarp_eval/README.md (the quality bench's "own generator" stage)
// job.bin (little-endian): u32 W, H, gw, gh, hasPrevious; f32 t; u32 acceleration; A, B: W*H u32 (R8G8B8A8);
// field: gw*gh f32 storedX, storedY, trust; previous field (if hasPrevious): the same.
// out.rgba: W*H u32, then u32 still, blend, conflict, hole, wholeFrameNearest, cutOffCells (trusted by the flow but longer
// than maxMotionShare), trustedCells; then, with overlay rules, the mask used (W*H bytes).
// Offline variants (environment; nothing here changes what the GPU does):
//   PUSHWARP_MAX_MOTION_SHARE=<f>  maxMotionShare (the GPU's constant is 0.125)
//   PUSHWARP_TEN_BIT=1             frames are R10G10B10A2 words (HDR10 PQ codes), as the GPU's HDR10 path
//   PUSHWARP_LINEAR_COMPOSE=1      (ten-bit only) R3 redone here with the bilinear samples and the blend in LINEAR light
//                                  (PQ decoded, blended, PQ encoded) on the reference's own filled field: the comparison
//                                  with the implemented scheme, which samples and blends in PQ code values. With
//                                  PUSHWARP_LINEAR_COMPOSE=check the same re-run is done in code values and must give the
//                                  reference's words exactly (exit code 9 otherwise).
//   PUSHWARP_OVERLAY_RULES=<bits>  static-overlay rules (PushWarp.h: 1 override, 2 source exclusion) on PushWarp's own
//                                  mask (pushWarpOverlayMask of A, B and the pair's cells), or on
//   PUSHWARP_OVERLAY_MASK=<file>   a given mask (W x H bytes, the same bits): the ideal mask of a synthetic scene
#include "veyra/pipeline/PushWarp.h"
#include "FlowModel.h"
#include <string>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace veyra::pipeline;

static bool readAll(std::FILE* f, void* p, std::size_t n) { return std::fread(p, 1, n, f) == n; }

// SMPTE ST 2084 on 0..1 code values <-> 0..1 of 10 000 nits.
static float pqToLinear(float e) {
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
    const double p = std::pow(std::max(double(e), 0.0), 1.0 / m2);
    return float(std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1));
}
static float linearToPq(float y) {
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
    const double p = std::pow(std::max(double(y), 0.0), m1);
    return float(std::pow((c1 + c2 * p) / (1 + c3 * p), m2));
}
static PushWarpColor toLinear(PushWarpColor c) { return {pqToLinear(c.r), pqToLinear(c.g), pqToLinear(c.b), c.a}; }
static PushWarpColor toPq(PushWarpColor c) { return {linearToPq(c.r), linearToPq(c.g), linearToPq(c.b), c.a}; }
// pushWarpBilinear with the four taps in linear light (or in code values when linear is false: the reference's own).
static PushWarpColor bilinearIn(const PushWarpFrame& f, float qx, float qy, bool linear) {
    if (!linear) return pushWarpBilinear(f, true, qx, qy);
    const float fx = std::floor(qx), fy = std::floor(qy), tx = qx - fx, ty = qy - fy;
    const int ax = int(fx), ay = int(fy);
    const auto c00 = toLinear(pushWarpUnpack(f.at(ax, ay), true)), c10 = toLinear(pushWarpUnpack(f.at(ax + 1, ay), true));
    const auto c01 = toLinear(pushWarpUnpack(f.at(ax, ay + 1), true)), c11 = toLinear(pushWarpUnpack(f.at(ax + 1, ay + 1), true));
    const auto mix = [&](float PushWarpColor::*m) {
        const float top = c00.*m + (c10.*m - c00.*m) * tx, bottom = c01.*m + (c11.*m - c01.*m) * tx;
        return top + (bottom - top) * ty;
    };
    return {mix(&PushWarpColor::r), mix(&PushWarpColor::g), mix(&PushWarpColor::b), mix(&PushWarpColor::a)};
}
// R3 of pushWarpReference (interpolation only, t < 1) on its filled level-0 field, in code values or in linear light.
// The still test, the conflict test and the hole rule stay in code values (as the GPU decides them); only the samples
// and the blend change domain.
static std::vector<std::uint32_t> recompose(const PushWarpFrame& a, const PushWarpFrame& b, const PushWarpResult& r, const PushWarpParams& p,
                                            std::uint32_t gw, std::uint32_t gh, bool linear) {
    const std::uint32_t W = b.width, H = b.height;
    std::vector<std::uint32_t> out(std::size_t(W) * H);
    const float sx = float(gw) / float(W), sy = float(gh) / float(H), scaleX = float(W) / float(gw), scaleY = float(H) / float(gh);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const std::uint32_t aw = a.at(int(x), int(y)), bw = b.at(int(x), int(y));
            if (pushWarpMaxDelta(pushWarpUnpack(aw, true), pushWarpUnpack(bw, true)) <= p.stillDelta * pushWarpOverlayScale(true)) { out[std::size_t(y) * W + x] = aw; continue; }
            const float qx = std::max((float(x) + 0.5f) * sx - 0.5f, 0.0f), qy = std::max((float(y) + 0.5f) * sy - 0.5f, 0.0f);
            const float fx = std::floor(qx), fy = std::floor(qy), tx = qx - fx, ty = qy - fy;
            const std::uint32_t x0 = std::min(std::uint32_t(fx), gw - 1), y0 = std::min(std::uint32_t(fy), gh - 1);
            const std::uint32_t x1 = std::min(x0 + 1, gw - 1), y1 = std::min(y0 + 1, gh - 1);
            const auto& f00 = r.field[y0 * gw + x0]; const auto& f10 = r.field[y0 * gw + x1];
            const auto& f01 = r.field[y1 * gw + x0]; const auto& f11 = r.field[y1 * gw + x1];
            const auto bl = [&](float PushWarpLevelCell::*m) {
                const float top = f00.*m + (f10.*m - f00.*m) * tx, bottom = f01.*m + (f11.*m - f01.*m) * tx;
                return top + (bottom - top) * ty;
            };
            const float gx = bl(&PushWarpLevelCell::ex) * scaleX, gy = bl(&PushWarpLevelCell::ey) * scaleY;
            float vis = bl(&PushWarpLevelCell::visibility);
            if (vis < p.holeVisibility) vis = 1.0f;
            const float ax = float(x) + p.t * gx, ay = float(y) + p.t * gy, bx = float(x) - (1.0f - p.t) * gx, by = float(y) - (1.0f - p.t) * gy;
            const auto c0 = pushWarpBilinear(a, true, ax, ay), c1 = pushWarpBilinear(b, true, bx, by);   // the decisions, in codes
            const auto s0 = bilinearIn(a, ax, ay, linear), s1 = bilinearIn(b, bx, by, linear);
            const float w0 = (1.0f - p.t) * vis, w1 = p.t * vis;
            PushWarpColor o;
            if (pushWarpMaxDelta(c0, c1) > p.conflictDelta) o = w0 >= w1 ? s0 : s1;
            else {
                const float d = w0 + w1;
                o = {(s0.r * w0 + s1.r * w1) / d, (s0.g * w0 + s1.g * w1) / d, (s0.b * w0 + s1.b * w1) / d, (s0.a * w0 + s1.a * w1) / d};
            }
            out[std::size_t(y) * W + x] = pushWarpPack(linear ? toPq(o) : o, true);
        }
    if (r.wholeFrameNearest) {
        const PushWarpFrame& nearest = p.t >= 0.5f ? b : a;
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x) out[std::size_t(y) * W + x] = nearest.at(int(x), int(y));
    }
    return out;
}


// ---- frames mode: picture files in and out --------------------------------------------------------------------------
namespace {
struct Image { std::uint32_t w = 0, h = 0, channels = 0, maxval = 0; std::vector<std::uint32_t> v; };   // samples, row major

bool endsWith(const std::string& s, const char* e) { const std::string x(e); return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0; }

bool readPnm(const std::string& path, Image& im) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const auto token = [&]() {
        std::string t;
        int c = std::fgetc(f);
        while (c != EOF) {
            if (c == '#') { while (c != EOF && c != '\n') c = std::fgetc(f); }
            else if (std::isspace(c)) { if (!t.empty()) break; }
            else t.push_back(char(c));
            c = std::fgetc(f);
        }
        return t;
    };
    const std::string magic = token();
    if (magic != "P5" && magic != "P6") { std::fclose(f); return false; }
    im.channels = magic == "P6" ? 3 : 1;
    im.w = std::uint32_t(std::stoul(token())); im.h = std::uint32_t(std::stoul(token())); im.maxval = std::uint32_t(std::stoul(token()));
    const std::size_t n = std::size_t(im.w) * im.h * im.channels;
    const int bytes = im.maxval > 255 ? 2 : 1;
    std::vector<std::uint8_t> raw(n * std::size_t(bytes));
    const bool ok = std::fread(raw.data(), 1, raw.size(), f) == raw.size();
    std::fclose(f);
    if (!ok) return false;
    im.v.resize(n);
    for (std::size_t i = 0; i < n; ++i) im.v[i] = bytes == 2 ? (std::uint32_t(raw[2 * i]) << 8) | raw[2 * i + 1] : raw[i];   // big-endian
    return true;
}

// .npy, version 1 or 2, C order, dtype |u1 or <u2, shape (H, W) or (H, W, 3). maxval 255 / 65535 unless --max says.
bool readNpy(const std::string& path, Image& im) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char head[10];
    if (std::fread(head, 1, 10, f) != 10 || std::memcmp(head, "\x93NUMPY", 6) != 0) { std::fclose(f); return false; }
    std::size_t hlen = head[8] | (std::size_t(head[9]) << 8);
    if (head[6] >= 2) { unsigned char more[2]; if (std::fread(more, 1, 2, f) != 2) { std::fclose(f); return false; } hlen |= (std::size_t(more[0]) << 16) | (std::size_t(more[1]) << 24); }
    std::string h(hlen, ' ');
    if (std::fread(h.data(), 1, hlen, f) != hlen) { std::fclose(f); return false; }
    const bool u2 = h.find("<u2") != std::string::npos, u1 = h.find("|u1") != std::string::npos;
    if ((!u1 && !u2) || h.find("'fortran_order': True") != std::string::npos) { std::fclose(f); return false; }
    const auto open = h.find('('), close = h.find(')');
    std::vector<std::uint32_t> dims;
    for (std::size_t i = open + 1; i < close;) {
        while (i < close && !std::isdigit(static_cast<unsigned char>(h[i]))) ++i;
        std::size_t j = i;
        while (j < close && std::isdigit(static_cast<unsigned char>(h[j]))) ++j;
        if (j > i) dims.push_back(std::uint32_t(std::stoul(h.substr(i, j - i))));
        i = j;
    }
    if (dims.size() < 2 || dims.size() > 3) { std::fclose(f); return false; }
    im.h = dims[0]; im.w = dims[1]; im.channels = dims.size() == 3 ? dims[2] : 1; im.maxval = u2 ? 65535 : 255;
    const std::size_t n = std::size_t(im.w) * im.h * im.channels;
    im.v.resize(n);
    bool ok = true;
    if (u2) { std::vector<std::uint16_t> raw(n); ok = std::fread(raw.data(), 2, n, f) == n; for (std::size_t i = 0; i < n; ++i) im.v[i] = raw[i]; }
    else { std::vector<std::uint8_t> raw(n); ok = std::fread(raw.data(), 1, n, f) == n; for (std::size_t i = 0; i < n; ++i) im.v[i] = raw[i]; }
    std::fclose(f);
    return ok && (im.channels == 1 || im.channels == 3);
}

bool readImage(const std::string& path, Image& im) { return endsWith(path, ".npy") ? readNpy(path, im) : readPnm(path, im); }

// Samples -> PushWarp words: SDR R8G8B8A8 (code = round(v x 255 / maxval)), HDR10 R10G10B10A2 PQ codes (v x 1023 / maxval).
std::vector<std::uint32_t> toWords(const Image& im, bool hdr, std::uint32_t maxval) {
    std::vector<std::uint32_t> w(std::size_t(im.w) * im.h);
    const double top = hdr ? 1023.0 : 255.0;
    for (std::size_t i = 0; i < w.size(); ++i) {
        std::uint32_t c[3];
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t v = im.v[i * im.channels + (im.channels == 3 ? std::size_t(k) : 0)];
            c[k] = std::uint32_t(std::min(top, std::nearbyint(double(v) * top / double(maxval))));
        }
        w[i] = hdr ? (c[0] | (c[1] << 10) | (c[2] << 20) | (3u << 30)) : (c[0] | (c[1] << 8) | (c[2] << 16) | (255u << 24));
    }
    return w;
}

// 8-bit RGB for the flow model (HDR10: the PQ codes reduced to 8 bits, as hud_eval.py / hdr_eval.py do).
std::vector<std::uint8_t> flowRgb(const std::vector<std::uint32_t>& words, bool hdr) {
    std::vector<std::uint8_t> rgb(words.size() * 3);
    for (std::size_t i = 0; i < words.size(); ++i)
        for (int k = 0; k < 3; ++k)
            rgb[i * 3 + std::size_t(k)] = hdr ? std::uint8_t(std::nearbyint(double((words[i] >> (10 * k)) & 1023u) * 255.0 / 1023.0))
                                              : std::uint8_t((words[i] >> (8 * k)) & 255u);
    return rgb;
}

// Words -> file: PPM P6 (SDR maxval 255; HDR10 maxval 1023, two bytes big-endian) or .npy (uint8 / uint16 codes, H x W x 3).
bool writeImage(const std::string& path, const std::vector<std::uint32_t>& words, std::uint32_t W, std::uint32_t H, bool hdr) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const auto sample = [&](std::size_t i, int k) { return hdr ? (words[i] >> (10 * k)) & 1023u : (words[i] >> (8 * k)) & 255u; };
    const std::size_t n = std::size_t(W) * H;
    if (endsWith(path, ".npy")) {
        std::string d = std::string("{'descr': '") + (hdr ? "<u2" : "|u1") + "', 'fortran_order': False, 'shape': (" + std::to_string(H) + ", " + std::to_string(W) + ", 3), }";
        while ((10 + d.size() + 1) % 64 != 0) d.push_back(' ');
        d.push_back('\n');
        const unsigned char head[10] = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0, static_cast<unsigned char>(d.size() & 255u), static_cast<unsigned char>(d.size() >> 8)};
        std::fwrite(head, 1, 10, f); std::fwrite(d.data(), 1, d.size(), f);
        for (std::size_t i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k) {
                if (hdr) { const std::uint16_t v = std::uint16_t(sample(i, k)); std::fwrite(&v, 2, 1, f); }   // little-endian hosts
                else { const std::uint8_t v = std::uint8_t(sample(i, k)); std::fwrite(&v, 1, 1, f); }
            }
    } else {
        std::fprintf(f, "P6\n%u %u\n%u\n", W, H, hdr ? 1023u : 255u);
        for (std::size_t i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t v = sample(i, k);
                if (hdr) { const unsigned char b2[2] = {static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v & 255u)}; std::fwrite(b2, 1, 2, f); }
                else std::fputc(int(v), f);
            }
    }
    return std::fclose(f) == 0;
}

int framesUsage() {
    std::fprintf(stderr,
                 "usage: pushwarp_cli frames --a A.(ppm|pgm|npy) --b B.(ppm|pgm|npy) --multiplier 2|3 --out PREFIX.(ppm|npy)\n"
                 "                    [--hdr10] [--max N] [--overlay on|off] [--write-field FILE]\n");
    return 2;
}

int runFrames(int argc, char** argv) {
    std::string pathA, pathB, out, fieldOut;
    unsigned multiplier = 0, maxOverride = 0;
    bool hdr = false, overlayOn = true;
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        const auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (k == "--a") pathA = next();
        else if (k == "--b") pathB = next();
        else if (k == "--out") out = next();
        else if (k == "--multiplier") multiplier = unsigned(std::stoul(next()));
        else if (k == "--max") maxOverride = unsigned(std::stoul(next()));
        else if (k == "--hdr10") hdr = true;
        else if (k == "--overlay") overlayOn = next() != "off";
        else if (k == "--write-field") fieldOut = next();
        else return framesUsage();
    }
    if (pathA.empty() || pathB.empty() || out.empty() || (multiplier != 2 && multiplier != 3)) return framesUsage();
    Image ia, ib;
    if (!readImage(pathA, ia) || !readImage(pathB, ib)) { std::fprintf(stderr, "frames: cannot read the input pictures\n"); return 3; }
    if (ia.w != ib.w || ia.h != ib.h || ia.w % 4 || ia.h % 4 || ia.w < 64 || ia.h < 64) { std::fprintf(stderr, "frames: A and B must have the same size, a multiple of 4\n"); return 3; }
    const std::uint32_t W = ia.w, H = ia.h;
    const auto a = toWords(ia, hdr, maxOverride ? maxOverride : ia.maxval), b = toWords(ib, hdr, maxOverride ? maxOverride : ib.maxval);
    // The pair's ONE field (B -> A, a quarter of the size: the play ratio), as the GPU path gets one per pair.
    const auto la = pushwarp_eval::modelLumaQuarter(flowRgb(a, hdr), W, H), lb = pushwarp_eval::modelLumaQuarter(flowRgb(b, hdr), W, H);
    const auto field = pushwarp_eval::modelDensify(pushwarp_eval::modelFlow(lb, la), lb, la);
    if (!fieldOut.empty()) {
        std::FILE* ff = std::fopen(fieldOut.c_str(), "wb");
        if (!ff) return 4;
        const std::uint32_t dims[2] = {field.w, field.h};
        std::fwrite(dims, 4, 2, ff); std::fwrite(field.x.data(), 4, field.x.size(), ff); std::fwrite(field.y.data(), 4, field.y.size(), ff); std::fwrite(field.trust.data(), 4, field.trust.size(), ff);
        std::fclose(ff);
    }
    const PushWarpFrame A{a.data(), W, H, W}, B{b.data(), W, H, W};
    const PushWarpField pf{field.x.data(), field.y.data(), field.trust.data(), field.w, field.h};
    const std::string stem = out.substr(0, out.rfind('.')), ext = out.substr(out.rfind('.'));
    for (unsigned k = 1; k < multiplier; ++k) {
        PushWarpParams p;   // the GPU path's parameters (EnhanceGraphOwnFrame.cpp recordPushWarpGenerated): defaults, t, rules
        p.t = float(k) / float(multiplier);
        p.overlayRules = overlayOn ? 3u : 0u;
        const auto r = pushWarpReference(A, B, hdr, pf, p);
        const std::string path = stem + "_" + std::to_string(k) + "of" + std::to_string(multiplier) + ext;
        if (!writeImage(path, r.words, W, H, hdr)) { std::fprintf(stderr, "frames: cannot write %s\n", path.c_str()); return 5; }
        unsigned overlay = 0;
        for (auto m : r.overlayMask) overlay += (m & 1u) ? 1u : 0u;
        std::printf("{\"file\": \"%s\", \"t\": %.6f, \"still\": %u, \"blend\": %u, \"conflict\": %u, \"hole\": %u, \"wholeFrameNearest\": %s, \"overlayPx\": %u}\n",
                    path.c_str(), double(p.t), r.counters.still, r.counters.blend, r.counters.conflict, r.counters.hole, r.wholeFrameNearest ? "true" : "false", overlay);
    }
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "frames") == 0) return runFrames(argc, argv);
    if (argc != 3) { std::fprintf(stderr, "usage: pushwarp_cli job.bin out.rgba\n"); return 2; }
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 3;
    std::uint32_t h[5]; float t = 0; std::uint32_t accel = 0;
    if (!readAll(f, h, sizeof h) || !readAll(f, &t, 4) || !readAll(f, &accel, 4)) return 4;
    const std::uint32_t W = h[0], H = h[1], gw = h[2], gh = h[3];
    std::vector<std::uint32_t> a(std::size_t(W) * H), b(a.size());
    std::vector<float> fx(std::size_t(gw) * gh), fy(fx.size()), ft(fx.size()), px(fx.size()), py(fx.size()), pt(fx.size());
    if (!readAll(f, a.data(), a.size() * 4) || !readAll(f, b.data(), b.size() * 4)) return 5;
    if (!readAll(f, fx.data(), fx.size() * 4) || !readAll(f, fy.data(), fy.size() * 4) || !readAll(f, ft.data(), ft.size() * 4)) return 6;
    if (h[4] && (!readAll(f, px.data(), px.size() * 4) || !readAll(f, py.data(), py.size() * 4) || !readAll(f, pt.data(), pt.size() * 4))) return 7;
    std::fclose(f);
    const PushWarpFrame A{a.data(), W, H, W}, B{b.data(), W, H, W};
    const PushWarpField field{fx.data(), fy.data(), ft.data(), gw, gh}, previous{px.data(), py.data(), pt.data(), gw, gh};
    PushWarpParams p;
    p.t = t;
    p.acceleration = accel != 0;
    if (const char* share = std::getenv("PUSHWARP_ACCEL_SHARE")) p.accelShare = float(std::atof(share));   // offline variants only
    if (const char* share = std::getenv("PUSHWARP_MAX_MOTION_SHARE")) p.maxMotionShare = float(std::atof(share));
    const char* ten = std::getenv("PUSHWARP_TEN_BIT");
    const bool tenBit = ten && ten[0] == '1';
    std::vector<std::uint8_t> mask;
    PushWarpOverlay overlay;
    if (const char* rules = std::getenv("PUSHWARP_OVERLAY_RULES"); rules && std::atoi(rules) > 0) {
        p.overlayRules = unsigned(std::atoi(rules));
        if (const char* file = std::getenv("PUSHWARP_OVERLAY_MASK")) {
            mask.resize(std::size_t(W) * H);
            std::FILE* m = std::fopen(file, "rb");
            if (!m || std::fread(mask.data(), 1, mask.size(), m) != mask.size()) return 10;
            std::fclose(m);
            overlay.given = mask.data();
        }
    }
    const auto r = pushWarpReference(A, B, tenBit, field, p, h[4] ? &previous : nullptr, overlay.given ? &overlay : nullptr);
    std::vector<std::uint32_t> words = r.words;
    if (const char* lin = std::getenv("PUSHWARP_LINEAR_COMPOSE"); lin && tenBit && t < 1.0f) {
        const bool check = std::strcmp(lin, "check") == 0;
        words = recompose(A, B, r, p, gw, gh, !check);
        if (check) {
            std::size_t differing = 0;
            for (std::size_t i = 0; i < words.size(); ++i) differing += words[i] != r.words[i];
            if (differing) { std::fprintf(stderr, "recompose in code values differs from the reference at %zu pixels\n", differing); return 9; }
        }
    }
    std::uint32_t cutOff = 0, trusted = 0;
    for (std::uint32_t y = 0; y < gh; ++y)
        for (std::uint32_t x = 0; x < gw; ++x) {
            float vx = 0, vy = 0;
            if (!pushWarpVectorAt(field, int(x), int(y), p.minConfidence, vx, vy)) continue;
            ++trusted;
            if (std::hypot(vx, vy) > p.maxMotionShare * float(gw)) ++cutOff;
        }
    std::FILE* o = std::fopen(argv[2], "wb");
    if (!o) return 8;
    std::fwrite(words.data(), 4, words.size(), o);
    const std::uint32_t c[7] = {r.counters.still, r.counters.blend, r.counters.conflict, r.counters.hole, r.wholeFrameNearest ? 1u : 0u, cutOff, trusted};
    std::fwrite(c, 4, 7, o);
    if (!r.overlayMask.empty()) std::fwrite(r.overlayMask.data(), 1, r.overlayMask.size(), o);
    std::fclose(o);
    return 0;
}
