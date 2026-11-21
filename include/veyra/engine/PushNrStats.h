// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace veyra::engine {
// The `[slipstream-nr]` statistics line (S5_CONTRACT.md interface item 4; sprint 5, stream "pipeline").
// Portable and pure: tests in tests/remoteplay/NrStatsTests.inc. The engine feeds one sample per graph
// record (one per real frame) from the GPU timestamps the graph already takes, and writes one line per
// 5 s at most, only with VEYRA_EXP_RP_STATS=1 (include/veyra/engine/PushNrStatsHook.h). Nothing here has
// run on a GPU.
//
//   frames=<n> layers=<n> internal=<WxH> output=<WxH> nrMsP50= nrMsP95= srMsP50= srMsP95= fgMsP50= fgMsP95=
//   graphMsP50= graphMsP95= copyCount=<n> vramMiB= budgetMiB= runtime=<name> version=<a.b.c.d>
//
// A value the app cannot know is written -1. What each number is:
//   nrMs     the neural layers' own spans of one real frame added up (NrLayer0..3: each layer's evaluate and
//            its decode; the shared stage GpuStage::Nr holds only the LAST layer's evaluate and is used only
//            when no layer span was measured). A frame that reused the previous inference counts as 0
//            (that is what the graph records for it). More than four layers: the first four.
//   srMs     GpuStage::Sr: the upscaler with the conversions the graph times with it (RTX Video SR: its
//            8-bit encode and decode; DLSS SR in HDR: NOT its restore pass, which is recorded after the mark).
//   fgMs     GpuStage::FgBatch: every generated frame of one pair.
//   graphMs  first to last GPU timestamp of the record (diagnostics::graphExecutionSpanMs): colour, flow,
//            SR, NR, residual, frame generation; not the presenter's blit, not waits before the record.
//   copyCount  pushNrCopyCount below: a MODEL of the graph by code reading, not instrumentation.

// One window of GPU milliseconds: nearest-rank percentiles over at most kCapacity stored samples (the same
// maths as PushSrWindow). A window of 5 s at 240 real frames a second holds 1200 samples: nothing is cut.
class PushNrSeries {
public:
    static constexpr std::size_t kCapacity = 1536;
    void add(double ms) {
        if (!(ms >= 0.0) || ms > 1.0e4) return;
        ++samples_;
        if (count_ < kCapacity) values_[count_++] = ms;
    }
    std::uint64_t samples() const { return samples_; }
    // Nearest-rank percentile (0..100) of the stored samples; -1 with none.
    double percentile(double p) const {
        if (!count_) return -1.0;
        std::array<double, kCapacity> sorted{};
        std::copy(values_.begin(), values_.begin() + std::ptrdiff_t(count_), sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + std::ptrdiff_t(count_));
        const double clamped = std::clamp(p, 0.0, 100.0);
        std::size_t rank = std::size_t(clamped / 100.0 * double(count_) + 0.999999);
        if (rank < 1) rank = 1;
        if (rank > count_) rank = count_;
        return sorted[rank - 1];
    }
    void clear() { count_ = 0; samples_ = 0; }
private:
    std::array<double, kCapacity> values_{};
    std::size_t count_ = 0;
    std::uint64_t samples_ = 0;
};

// One graph record. A stage that did not run (or was not measured) is -1 and is left out of its series.
struct PushNrFrame {
    double nrMs = -1.0, srMs = -1.0, fgMs = -1.0, graphMs = -1.0;
};

class PushNrWindow {
public:
    void add(const PushNrFrame& f) {
        ++frames_;
        nr_.add(f.nrMs);
        sr_.add(f.srMs);
        fg_.add(f.fgMs);
        graph_.add(f.graphMs);
    }
    std::uint64_t frames() const { return frames_; }
    const PushNrSeries& nr() const { return nr_; }
    const PushNrSeries& sr() const { return sr_; }
    const PushNrSeries& fg() const { return fg_; }
    const PushNrSeries& graph() const { return graph_; }
    void clear() { frames_ = 0; nr_.clear(); sr_.clear(); fg_.clear(); graph_.clear(); }
    // "At most one line per 5 s": true once the window is 5 s old (host clock, 100 ns units). The first
    // call only starts the window.
    bool due(std::int64_t now100ns) {
        if (!started_) { started_ = true; start100ns_ = now100ns; return false; }
        return now100ns - start100ns_ >= kWindow100ns;
    }
    void restart(std::int64_t now100ns) { clear(); started_ = true; start100ns_ = now100ns; }
    static constexpr std::int64_t kWindow100ns = 50000000;  // 5 s
private:
    std::uint64_t frames_ = 0;
    PushNrSeries nr_, sr_, fg_, graph_;
    bool started_ = false;
    std::int64_t start100ns_ = 0;
};

// --- copyCount: full-picture passes per real frame that only convert, resize or copy a picture on its way
// into or out of an AI stage. Counted from the code of src/pipeline/EnhanceGraph.cpp (docs/NEURAL_RENDERING.md
// lists each with its line): it follows the configuration, it does not observe the GPU.
enum class PushNrSrKind : std::uint8_t {
    None,        // no upscaler, work size = source size: nothing
    Resize,      // no upscaler but a larger work size: the graph's own resize (1)
    Temporal,    // DLSS SR or FSR: read the graph's FP16 colour and write the work texture (0; HDR: 1 restore pass)
    VideoNgx,    // RTX Video SR standard family: 8-bit encode + decode / HDR restore (2)
    VideoVfx,    // RTX Video SR through Video Effects: encode, copy in, copy out, decode (4)
    Nis          // NVIDIA Image Scaling: its encode and decode around the scaler (2)
};
struct PushNrCopyConfig {
    std::uint32_t nrLayers = 0;        // neural layers that evaluate
    std::uint32_t nrLayersResized = 0; // of those, the layers whose inference size differs from their input (a downsample each)
    PushNrSrKind sr = PushNrSrKind::None;
    bool hdr = false;                  // HDR working colour
    bool supersampleShrink = false;    // the SR output is area-reduced into a smaller work size (replaces the decode: +0)
    std::uint32_t referenceCopies = 0; // raw / base references kept for a recording or comparison (0..2)
};
constexpr int pushNrCopyCount(const PushNrCopyConfig& c) {
    // Per neural layer: encode to the 8-bit proxy, decode back to FP16, residual composite at full size;
    // plus the downsample when the layer does not run at its input's size.
    int n = int(c.nrLayers) * 3 + int(std::min(c.nrLayersResized, c.nrLayers));
    switch (c.sr) {
    case PushNrSrKind::None: break;
    case PushNrSrKind::Resize: n += 1; break;
    case PushNrSrKind::Temporal: n += c.hdr ? 1 : 0; break;
    case PushNrSrKind::VideoNgx: n += 2; break;
    case PushNrSrKind::VideoVfx: n += 4; break;
    case PushNrSrKind::Nis: n += 2; break;
    }
    return n + int(std::min<std::uint32_t>(c.referenceCopies, 2u));
}
// EnhancementSettings::videoSrQuality (0 DLSS SR, 1..4 RTX Video SR, 5 FSR, 6 NIS, 16..19 / 21 / 23 Video
// Effects) to the kind above. `enlarges` = the work size differs from the source size.
constexpr PushNrSrKind pushNrSrKind(bool srEnabled, std::uint32_t videoSrQuality, bool enlarges) {
    if (!srEnabled) return enlarges ? PushNrSrKind::Resize : PushNrSrKind::None;
    if (videoSrQuality == 0 || videoSrQuality == 5) return PushNrSrKind::Temporal;
    if (videoSrQuality == 6) return PushNrSrKind::Nis;
    if (videoSrQuality >= 1 && videoSrQuality <= 4) return PushNrSrKind::VideoNgx;
    if ((videoSrQuality >= 16 && videoSrQuality <= 19) || videoSrQuality == 21 || videoSrQuality == 23) return PushNrSrKind::VideoVfx;
    return PushNrSrKind::None;
}

// The runtime slot's name as `[nr-runtime-status]` writes it (ids of engine::NrRuntime: 0 and 1 are retired
// and mean 2); "none" while no neural layer runs.
constexpr std::string_view pushNrStatsRuntimeName(int runtimeId, bool layerRuns) {
    if (!layerRuns) return "none";
    return runtimeId == 3 ? "original" : runtimeId == 4 ? "amd" : "sf";
}

struct PushNrStatsInfo {
    std::uint32_t layers = 0;
    std::uint32_t internalW = 0, internalH = 0;  // the first layer's inference size; 0x0 without a layer
    std::uint32_t outputW = 0, outputH = 0;      // the graph's work size (what frame generation and the presenter get)
    int copyCount = -1;
    std::int64_t vramMiB = -1, budgetMiB = -1;   // this process's video memory use and the system's budget for it
    std::string_view runtime = "none";
    std::uint32_t versionMs = 0, versionLs = 0;  // the loaded runtime file's version resource; 0 = unknown

    constexpr bool sameConfiguration(const PushNrStatsInfo& o) const {
        return layers == o.layers && internalW == o.internalW && internalH == o.internalH && outputW == o.outputW && outputH == o.outputH &&
               copyCount == o.copyCount && runtime == o.runtime && versionMs == o.versionMs && versionLs == o.versionLs;
    }
};

inline std::string pushNrStatsLine(const PushNrWindow& w, const PushNrStatsInfo& i) {
    char version[48];
    if (i.versionMs || i.versionLs)
        std::snprintf(version, sizeof version, "%u.%u.%u.%u", unsigned(i.versionMs >> 16), unsigned(i.versionMs & 0xFFFFu),
                      unsigned(i.versionLs >> 16), unsigned(i.versionLs & 0xFFFFu));
    else
        std::snprintf(version, sizeof version, "-1");
    const std::string_view runtime = i.runtime.empty() ? std::string_view("none") : i.runtime;
    char text[640];
    std::snprintf(text, sizeof text,
                  "frames=%llu layers=%u internal=%ux%u output=%ux%u nrMsP50=%.3f nrMsP95=%.3f srMsP50=%.3f srMsP95=%.3f "
                  "fgMsP50=%.3f fgMsP95=%.3f graphMsP50=%.3f graphMsP95=%.3f copyCount=%d vramMiB=%lld budgetMiB=%lld "
                  "runtime=%.*s version=%s",
                  static_cast<unsigned long long>(w.frames()), unsigned(i.layers), unsigned(i.internalW), unsigned(i.internalH),
                  unsigned(i.outputW), unsigned(i.outputH), w.nr().percentile(50.0), w.nr().percentile(95.0), w.sr().percentile(50.0),
                  w.sr().percentile(95.0), w.fg().percentile(50.0), w.fg().percentile(95.0), w.graph().percentile(50.0),
                  w.graph().percentile(95.0), i.copyCount, static_cast<long long>(i.vramMiB), static_cast<long long>(i.budgetMiB),
                  int(std::min<std::size_t>(runtime.size(), 24)), runtime.data(), version);
    return text;
}
}  // namespace veyra::engine
