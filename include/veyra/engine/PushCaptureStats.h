// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Slipstream stream "capture": what a capture-card session really delivers, one `[slipstream-capture]` line per 5 s
// window while VEYRA_EXP_RP_STATS=1 (the test kit), nothing at all otherwise. Pure and portable; tests in
// tests/remoteplay/CaptureStatsTests.inc; the engine glue is a few calls in src/engine/EngineController.cpp, all on the
// engine thread (the graph owner, which also runs the presentation steps), so nothing here is shared between threads.
//   - Picture numbers of every sample the engine reads, before duplicate removal (CaptureUploadFrame.h: the capture
//     callback numbers each sample while it compares pictures; consecutive new pictures get consecutive numbers, also
//     those the engine never read): new pictures, repeats, and NEW PICTURES THE ENGINE NEVER READ (the mailbox replaced
//     every sample of them while the engine was busy: a real game frame lost, seen as judder).
//   - The processed pairs: how many were between two identical pictures, and how many generated frames those had.
//     A frame generated between two identical pictures is a copy, never motion: `stillGenFps` beside `motionGenFps`.
//   - Each real frame's capture callback to its Present return (P50 / P95 / P99): software, not photons.
//   - The share of pairs in LiveFgRejectionHold (real frames shown at once, without the generation phase).
//   - The source's own counters (callbacks, mailbox replacements) and the mode in use.
// The card line (`event=card`, written by the bridge): the card's name as one word and a summary of its formats.
// Unknown values are written as -1, never as 0 and never as another counter's value.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::engine {

// The low 56 bits of a picture number count new pictures; the high 8 bits name the numbering (PictureCompare.h).
inline constexpr std::uint64_t kCapturePictureCounter = 0x00FFFFFFFFFFFFFFull;

// What the engine knows about the source when a window ends.
struct PushCaptureSource {
    std::uint32_t width = 0, height = 0;
    double nominalFps = 0;
    std::string format;                    // one word (pushCaptureWord), "unknown" when empty
    bool compressed = false, hdr = false, dedup = false;
    std::uint64_t received = 0, mailboxDropped = 0;   // cumulative: the capture source's own counters
    std::uint64_t revision = 0;                       // the engine's settings revision: a change restarts the window
};

struct PushCaptureWindowStats {
    double seconds = 0;
    bool pictures = false;                 // every sample read in the window carried a picture number
    double transportFps = 0, readFps = 0, mailboxDropFps = 0;
    double newFps = -1, repeatFps = -1, missedNewFps = -1;
    double pairsFps = 0, genFps = 0, stillPairsFps = -1, stillGenFps = -1, motionGenFps = -1, fgHeldPct = 0;
    std::size_t latencySamples = 0;
    double realLatP50Ms = -1, realLatP95Ms = -1, realLatP99Ms = -1;
};

// A name as one log word: letters, digits, '.', '-' kept, spaces and '_' become '_', the rest dropped, at most 40
// characters; "unknown" when nothing is left. Used for the card's friendly name and pixel format names only.
inline std::string pushCaptureWord(std::wstring_view text) {
    std::string out;
    for (const wchar_t c : text) {
        if (out.size() >= 40) break;
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-')
            out.push_back(static_cast<char>(c));
        else if ((c == L' ' || c == L'_') && !out.empty() && out.back() != '_')
            out.push_back('_');
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? std::string("unknown") : out;
}
inline std::string pushCaptureWord(std::string_view text) {
    std::wstring wide;
    for (const char c : text) wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return pushCaptureWord(std::wstring_view(wide));
}

class PushCaptureWindow {
public:
    static constexpr std::int64_t kWindow100ns = 50000000;     // 5 s
    static constexpr std::size_t kMaxLatency = 4096;           // more than a 5 s window of real frames at 360 FPS
    static constexpr std::uint64_t kMaxGap = 600;              // a longer jump in numbers is a restart, not misses

    explicit PushCaptureWindow(bool enabled = false) : enabled_(enabled) {}
    bool enabled() const { return enabled_; }

    // Every sample the engine reads, before duplicate removal. pictured: the sample carries a picture number.
    void sample(bool pictured, std::uint64_t id) {
        if (!enabled_) return;
        ++read_;
        if (!pictured) { havePicture_ = false; return; }       // the chain of numbers is broken: nothing is inferred across it
        ++pictured_;
        if (havePicture_ && id == lastPicture_) { ++repeats_; return; }
        ++new_;
        if (havePicture_ && (id >> 56) == (lastPicture_ >> 56)) {
            const std::uint64_t now = id & kCapturePictureCounter, before = lastPicture_ & kCapturePictureCounter;
            if (now > before && now - before <= kMaxGap) missed_ += now - before - 1;
        }
        lastPicture_ = id;
        havePicture_ = true;
    }
    // Every processed frame B of a pair (A, B): its picture number, whether its history was reset (then there is no
    // pair), the generated frames evaluated for it, and whether LiveFgRejectionHold held the pair.
    void pair(bool pictured, std::uint64_t id, bool reset, std::uint32_t generated, bool held) {
        if (!enabled_) return;
        ++pairs_;
        generated_ += generated;
        if (held) ++held_;
        if (!pictured) ++unknownPairs_;
        else if (!reset && havePair_ && pairPictured_ && id == pairPicture_) { ++stillPairs_; stillGenerated_ += generated; }
        havePair_ = true;
        pairPictured_ = pictured;
        pairPicture_ = id;
    }
    // A real frame's capture callback to its Present return, in milliseconds.
    void realPresented(double ms) {
        if (!enabled_ || !std::isfinite(ms) || ms < 0 || ms > 10000 || latency_.size() >= kMaxLatency) return;
        latency_.push_back(ms);
    }
    // About once a second. The first call starts the window. Returns true with `out` filled when a window of at least
    // 5 s has ended; the next window starts at once. A source whose counters went backwards (reconnect) or a new settings
    // revision (another backend, multiplier, tier, dedup mode: the graph is rebuilt) restarts it and drops the part
    // already counted, so no window mixes two configurations.
    bool due(std::int64_t now100ns, const PushCaptureSource& source, PushCaptureWindowStats& out) {
        if (!enabled_) return false;
        if (!started_ || source.received < baseReceived_ || source.mailboxDropped < baseDropped_ || now100ns < start_ ||
            source.revision != baseRevision_) {
            restart(now100ns, source);
            return false;
        }
        if (now100ns - start_ < kWindow100ns) return false;
        out = stats(now100ns, source);
        restart(now100ns, source);
        return true;
    }

private:
    PushCaptureWindowStats stats(std::int64_t now100ns, const PushCaptureSource& source) {
        PushCaptureWindowStats s;
        s.seconds = double(now100ns - start_) / 1e7;
        const double t = s.seconds > 0 ? s.seconds : 1.0;
        s.transportFps = double(source.received - baseReceived_) / t;
        s.mailboxDropFps = double(source.mailboxDropped - baseDropped_) / t;
        s.readFps = double(read_) / t;
        s.pictures = pictured_ > 0 && pictured_ == read_;
        if (s.pictures) {
            s.newFps = double(new_) / t;
            s.repeatFps = double(repeats_) / t;
            s.missedNewFps = double(missed_) / t;
        }
        s.pairsFps = double(pairs_) / t;
        s.genFps = double(generated_) / t;
        if (unknownPairs_ == 0) {
            s.stillPairsFps = double(stillPairs_) / t;
            s.stillGenFps = double(stillGenerated_) / t;
            s.motionGenFps = double(generated_ - stillGenerated_) / t;
        }
        s.fgHeldPct = pairs_ ? 100.0 * double(held_) / double(pairs_) : 0.0;
        s.latencySamples = latency_.size();
        if (!latency_.empty()) {
            std::sort(latency_.begin(), latency_.end());
            const auto at = [&](double q) {
                const std::size_t i = std::min(latency_.size() - 1, static_cast<std::size_t>(std::ceil(q * double(latency_.size()))) - 1);
                return latency_[i];
            };
            s.realLatP50Ms = at(0.50);
            s.realLatP95Ms = at(0.95);
            s.realLatP99Ms = at(0.99);
        }
        return s;
    }
    void restart(std::int64_t now100ns, const PushCaptureSource& source) {
        started_ = true;
        start_ = now100ns;
        baseReceived_ = source.received;
        baseDropped_ = source.mailboxDropped;
        baseRevision_ = source.revision;
        read_ = pictured_ = new_ = repeats_ = missed_ = 0;
        pairs_ = generated_ = held_ = unknownPairs_ = stillPairs_ = stillGenerated_ = 0;
        latency_.clear();
        // The chains of numbers carry over: a picture read just before the boundary still decides the next one.
    }

    bool enabled_ = false, started_ = false;
    std::int64_t start_ = 0;
    std::uint64_t baseReceived_ = 0, baseDropped_ = 0, baseRevision_ = 0;
    std::uint64_t read_ = 0, pictured_ = 0, new_ = 0, repeats_ = 0, missed_ = 0;
    std::uint64_t pairs_ = 0, generated_ = 0, held_ = 0, unknownPairs_ = 0, stillPairs_ = 0, stillGenerated_ = 0;
    bool havePicture_ = false, havePair_ = false, pairPictured_ = false;
    std::uint64_t lastPicture_ = 0, pairPicture_ = 0;
    std::vector<double> latency_;
};

// "%.2f", or "-1" for an unknown (negative) value.
inline std::string pushCaptureValue(double value) {
    if (!std::isfinite(value) || value < 0) return "-1";
    return std::format("{:.2f}", value);
}

inline std::string pushCaptureLine(const PushCaptureWindowStats& s, const PushCaptureSource& source) {
    return std::format(
        "revision={} window={:.1f}s mode={}x{}@{:.2f} format={} compressed={} hdr={} dedup={} pictures={} transportFps={:.2f} readFps={:.2f} "
        "newFps={} repeatFps={} missedNewFps={} mailboxDropFps={:.2f} pairsFps={:.2f} stillPairsFps={} genFps={:.2f} stillGenFps={} "
        "motionGenFps={} fgHeldPct={:.1f} realLatN={} realLatP50Ms={} realLatP95Ms={} realLatP99Ms={} "
        "(software counters: new = a picture that differs from the sample before it, missedNew = new pictures the engine never "
        "read, gen = generated frames evaluated (shown ones: [slipstream-cadence]), stillGen = generated between two identical "
        "pictures (copies, never motion), realLat = a real frame's capture callback to its Present return, not photons; -1 = unknown)",
        source.revision, s.seconds, source.width, source.height, source.nominalFps, source.format.empty() ? std::string("unknown") : source.format,
        source.compressed ? 1 : 0, source.hdr ? 1 : 0, source.dedup ? 1 : 0, s.pictures ? 1 : 0, s.transportFps, s.readFps,
        pushCaptureValue(s.newFps), pushCaptureValue(s.repeatFps), pushCaptureValue(s.missedNewFps), s.mailboxDropFps, s.pairsFps,
        pushCaptureValue(s.stillPairsFps), s.genFps, pushCaptureValue(s.stillGenFps), pushCaptureValue(s.motionGenFps), s.fgHeldPct,
        s.latencySamples, pushCaptureValue(s.realLatP50Ms), pushCaptureValue(s.realLatP95Ms), pushCaptureValue(s.realLatP99Ms));
}

// --- the card line ----------------------------------------------------------------------------------------------
// What the card offers, from the format list the capture dialog reads (source::CaptureFormat: width, height, fps,
// pixel, interlaced, supported, tier (3 = needs a decoder: compressed or unknown), automatic). A template so that
// this header does not depend on the DirectShow headers; the tests pass their own struct with the same fields.
struct PushCaptureFormatSummary {
    std::size_t modes = 0;                                // progressive modes the player supports
    std::uint32_t w60 = 0, h60 = 0, w120 = 0, h120 = 0;   // largest uncompressed mode at >= 59 / >= 119 FPS
    std::uint32_t w60any = 0, h60any = 0, w120any = 0, h120any = 0;   // the same, compressed modes included
    bool tenBit = false;                                  // an uncompressed 10-bit format (P010, P016, v210, R10l)
    double maxFps = 0;
    std::string pixels;                                   // distinct pixel formats, '+' between them, at most 8
};

inline bool pushCaptureTenBit(std::string_view word) {
    std::string lower;
    for (const char c : word) lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    return lower == "p010" || lower == "p016" || lower == "v210" || lower == "r10l";
}

template <class Range>
PushCaptureFormatSummary pushCaptureFormatSummary(const Range& formats) {
    PushCaptureFormatSummary s;
    std::vector<std::string> pixels;
    const auto bigger = [](std::uint32_t w, std::uint32_t h, std::uint32_t& bw, std::uint32_t& bh) {
        if (std::uint64_t(w) * h > std::uint64_t(bw) * bh) { bw = w; bh = h; }
    };
    for (const auto& f : formats) {
        if (f.automatic || f.interlaced || !f.supported || f.width == 0 || f.height == 0) continue;
        ++s.modes;
        const bool uncompressed = f.tier < 3;
        const std::string pixel = pushCaptureWord(std::wstring_view(f.pixel));
        if (std::find(pixels.begin(), pixels.end(), pixel) == pixels.end() && pixels.size() < 8) pixels.push_back(pixel);
        if (uncompressed && pushCaptureTenBit(pixel)) s.tenBit = true;
        if (std::isfinite(f.fps) && f.fps > s.maxFps) s.maxFps = f.fps;
        if (f.fps >= 59.0) { bigger(f.width, f.height, s.w60any, s.h60any); if (uncompressed) bigger(f.width, f.height, s.w60, s.h60); }
        if (f.fps >= 119.0) { bigger(f.width, f.height, s.w120any, s.h120any); if (uncompressed) bigger(f.width, f.height, s.w120, s.h120); }
    }
    for (const auto& p : pixels) s.pixels += (s.pixels.empty() ? "" : "+") + p;
    if (s.pixels.empty()) s.pixels = "none";
    return s;
}

inline std::string pushCaptureSize(std::uint32_t w, std::uint32_t h) {
    return w && h ? std::format("{}x{}", w, h) : std::string("none");
}

// A device name such as "Alex's iPhone Camera" or "James' Capture Card" carries a person's name: everything up to the
// last apostrophe-like mark (', `, and in UTF-8 U+00B4, U+2018, U+2019, U+02BC, U+FF07) is dropped, and with it the rest
// of that word ("s", or whatever letters follow the mark), so only the product part is written ("iPhone_Camera",
// "Capture_Card"). A name without such a mark is written as it is.
inline std::string pushCaptureCardName(std::string_view card) {
    std::size_t cut = std::string_view::npos;
    for (const std::string_view mark : {std::string_view("'"), std::string_view("`"), std::string_view("\xC2\xB4"),
                                        std::string_view("\xE2\x80\x98"), std::string_view("\xE2\x80\x99"),
                                        std::string_view("\xCA\xBC"), std::string_view("\xEF\xBC\x87")}) {
        const std::size_t at = card.rfind(mark);
        if (at != std::string_view::npos && (cut == std::string_view::npos || at + mark.size() > cut)) cut = at + mark.size();
    }
    if (cut != std::string_view::npos) {
        card.remove_prefix(cut);
        while (!card.empty() && ((card.front() >= 'a' && card.front() <= 'z') || (card.front() >= 'A' && card.front() <= 'Z') ||
                                 (card.front() >= '0' && card.front() <= '9')))
            card.remove_prefix(1);
    }
    return pushCaptureWord(card);
}

// `[slipstream-capture] event=card ...`: once per capture session, test kit only. `card` is the driver's friendly name
// of the video device as one word (a product name such as Game_Capture_4K_X; never a path or serial, and a possessive
// owner's name is dropped: pushCaptureCardName).
inline std::string pushCaptureCardLine(std::string_view card, const PushCaptureFormatSummary& s) {
    return std::format("event=card card={} modes={} max60={} max120={} max60Any={} max120Any={} tenBit={} maxFps={:.2f} pixels={} "
                       "(what the driver lists; Any = compressed modes included; the mode in use is mode= of the window lines)",
                       pushCaptureCardName(card), s.modes, pushCaptureSize(s.w60, s.h60), pushCaptureSize(s.w120, s.h120),
                       pushCaptureSize(s.w60any, s.h60any), pushCaptureSize(s.w120any, s.h120any), s.tenBit ? 1 : 0, s.maxFps, s.pixels);
}

}  // namespace veyra::engine
