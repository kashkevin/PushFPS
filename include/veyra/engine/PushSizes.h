// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "delay90" (the owner's nineteenth message: a 30 FPS game shown at 90 FPS on a 2560x1440 180 Hz screen, "verify
// the actual processing resolution rather than assuming the 4K setting applies throughout the pipeline"). Portable and
// header-only: the C++ standard library, no Windows, no engine types. Tests: tests/remoteplay/SizesTests.inc.
//
//   pushFitVerdict      whether the size noise reduction, frame generation and the output work at stays within the box
//                       that covers the display, and why not (the fit= / fitReason= keys);
//   pushSizesLine       the `[slipstream-sizes]` line: every working size of the live graph, the swap chain's back buffer,
//                       the window's client area and the display, written when the live graph is built and when a value
//                       changes (PushSizesGate: looked at no more than once a second). Always on, like [slipstream-config]:
//                       it costs a few integer comparisons per second and holds no private data (sizes and a refresh rate).
//   pushFitDisplayTarget  VEYRA_EXP_RP_FIT_DISPLAY=1, a DIAGNOSTIC switch (what the working size itself costs; never a
//                       recommendation: the owner's rule of 23 Nov 2026 keeps 4K): the SR target live playback evaluates,
//                       the same rule as VEYRA_EXP_RP_SMOOTH_SR_DISPLAY (PushSmoothness.h pushSmoothSrTarget, shared).
//
// Nothing here has run on a GPU or on Windows; nothing here is a measurement.
#include "veyra/engine/PushSmoothness.h"
#include <cstdint>
#include <cstdio>
#include <string>

namespace veyra::engine {

struct PushWxH {
    std::uint32_t w = 0, h = 0;
    bool known() const { return w != 0 && h != 0; }
    bool within(const PushWxH& box) const { return w <= box.w && h <= box.h; }
    bool operator==(const PushWxH&) const = default;
};

// --- VEYRA_EXP_RP_FIT_DISPLAY (diagnostic only): the SR target -----------------------------------------------------------
// Either switch (VEYRA_EXP_RP_SMOOTH_SR_DISPLAY=1 or VEYRA_EXP_RP_FIT_DISPLAY=1) lowers the SR target with the one rule of
// pushSmoothSrTarget: the smallest target box that still covers the display on both sides, never larger than the user's.
// The engine passes the display size to runningSrTarget (GraphDescription.h) when either switch is on; this function is
// what the tests and the sizes line use to say which target that is.
constexpr std::uint32_t pushFitDisplayTarget(bool followDisplay, std::uint32_t userTarget, std::uint32_t displayWidth, std::uint32_t displayHeight) {
    return followDisplay ? pushSmoothSrTarget(userTarget, displayWidth, displayHeight) : userTarget;
}

// --- fit= and fitReason= --------------------------------------------------------------------------------------------------
enum class PushFitReason : std::uint8_t {
    WithinDisplay,    // fit=1: the working size is not larger than the display on either side
    SrLowered,        // fit=1: super resolution evaluates into a lowered target (the smallest box that covers the display)
    SrSmallestBox,    // fit=1: super resolution's target is already the smallest box that covers the display (a 21:9 panel)
    AsSet,            // fit=0: working at the size that was set (super resolution's target is larger than the display)
    DoubleSr,         // fit=0: double SR runs (its second pass keeps its own target; the switches leave it alone)
    SourceSize,       // fit=0: working at the source's own size, which is larger than the display (a 4K capture on a
                      //        1440p panel); a fact for the reader, not advice: 4K is the owner's choice
    DisplayUnknown,   // fit=0: the display's size was not known
};
constexpr const char* pushFitReasonName(PushFitReason reason) {
    switch (reason) {
    case PushFitReason::WithinDisplay: return "within-display";
    case PushFitReason::SrLowered: return "sr-lowered";
    case PushFitReason::SrSmallestBox: return "sr-smallest-box";
    case PushFitReason::AsSet: return "as-set";
    case PushFitReason::DoubleSr: return "double-sr";
    case PushFitReason::SourceSize: return "source-size";
    case PushFitReason::DisplayUnknown: return "display-unknown";
    }
    return "unknown";
}

struct PushFitInputs {
    PushWxH srInput, srEvaluated, work, display;
    bool sr = false;            // super resolution is on for this graph
    bool dualSr = false;        // double SR runs
    bool followDisplay = false; // VEYRA_EXP_RP_SMOOTH_SR_DISPLAY=1 or VEYRA_EXP_RP_FIT_DISPLAY=1
    int targetAsked = -1, targetUsed = -1;   // pipeline::SrTarget values; -1 without SR
};
struct PushFitVerdict {
    bool fit = false;
    PushFitReason reason = PushFitReason::DisplayUnknown;
};
// work: the size noise reduction (its composite), frame generation and the output work at (ResolutionPlan: fg = output =
// base). The display: the monitor the video window is on, in pixels (a smaller window on that monitor still counts the
// monitor: full screen is where the owner plays, and the presenter scales the work size into whatever the window is).
inline PushFitVerdict pushFitVerdict(const PushFitInputs& in) {
    PushFitVerdict v;
    if (!in.display.known() || !in.work.known()) { v.reason = PushFitReason::DisplayUnknown; return v; }
    const bool lowered = in.sr && in.targetUsed >= 0 && in.targetAsked >= 0 && in.targetUsed != in.targetAsked;
    if (in.work.within(in.display)) { v.fit = true; v.reason = lowered ? PushFitReason::SrLowered : PushFitReason::WithinDisplay; return v; }
    const bool srEnlarges = in.sr && in.srEvaluated.known() && !(in.srEvaluated == in.srInput);
    if (srEnlarges) {
        if (in.dualSr) { v.reason = PushFitReason::DoubleSr; return v; }
        if (!in.followDisplay) { v.reason = PushFitReason::AsSet; return v; }
        v.fit = true;
        v.reason = lowered ? PushFitReason::SrLowered : PushFitReason::SrSmallestBox;
        return v;
    }
    v.reason = PushFitReason::SourceSize;
    return v;
}

// --- the [slipstream-sizes] line ------------------------------------------------------------------------------------------
struct PushSizesFacts {
    PushWxH source, srInput, srEvaluated, base, nr, flow, fg, output;   // the live graph (base after any shrink)
    PushWxH backBuffer, client, display, mode;                          // swap chain, window client area, monitor, display mode
    unsigned modeHz = 0;                                                // the display mode's refresh (whole Hz, Windows' own)
    bool sr = false, hdrOutput = false;
    int srTarget = -1, srTargetAsked = -1;
    bool srDisplaySwitch = false, fitSwitch = false;                    // VEYRA_EXP_RP_SMOOTH_SR_DISPLAY, VEYRA_EXP_RP_FIT_DISPLAY
    PushFitVerdict verdict;
};

inline std::string pushSizesLine(const PushSizesFacts& f) {
    std::string s;
    const auto size = [&s](const char* name, const PushWxH& v) {
        char buffer[48];
        const int n = v.known() ? std::snprintf(buffer, sizeof buffer, "%s%s=%ux%u", s.empty() ? "" : " ", name, unsigned(v.w), unsigned(v.h))
                                : std::snprintf(buffer, sizeof buffer, "%s%s=-", s.empty() ? "" : " ", name);
        if (n > 0) s.append(buffer, static_cast<std::size_t>(n < int(sizeof buffer) ? n : int(sizeof buffer) - 1));
    };
    const auto text = [&s](const char* name, const char* value) { s += ' '; s += name; s += '='; s += value; };
    const auto number = [&s](const char* name, long long value) { s += ' '; s += name; s += '='; s += std::to_string(value); };
    size("source", f.source);
    size("srInput", f.srInput);
    size("srEvaluated", f.srEvaluated);
    size("base", f.base);
    size("nr", f.nr);
    size("flow", f.flow);
    size("fg", f.fg);
    size("output", f.output);
    size("backBuffer", f.backBuffer);
    size("client", f.client);
    size("display", f.display);
    size("mode", f.mode);
    number("modeHz", static_cast<long long>(f.modeHz));
    text("sr", f.sr ? "1" : "0");
    number("srTarget", f.srTarget);
    number("srTargetAsked", f.srTargetAsked);
    text("hdr", f.hdrOutput ? "1" : "0");
    text("srDisplay", f.srDisplaySwitch ? "1" : "0");
    text("fitDisplay", f.fitSwitch ? "1" : "0");
    text("fit", f.verdict.fit ? "1" : "0");
    text("fitReason", pushFitReasonName(f.verdict.reason));
    return s;
}

// When to write it: the facts are looked at no more than once a second (due), and the line is written only when its
// text differs from the last one written (changed). The first call writes (the live graph was just built).
class PushSizesGate {
public:
    static constexpr std::int64_t kPeriod100ns = 10000000;   // 1 s
    bool due(std::int64_t now100ns) {
        if (checked_ && now100ns - last_ < kPeriod100ns && now100ns >= last_) return false;
        checked_ = true;
        last_ = now100ns;
        return true;
    }
    bool changed(const std::string& line) {
        if (written_ && line == text_) return false;
        written_ = true;
        text_ = line;
        return true;
    }
    void reset() { checked_ = written_ = false; text_.clear(); }

private:
    std::string text_;
    std::int64_t last_ = 0;
    bool checked_ = false, written_ = false;
};

}  // namespace veyra::engine
