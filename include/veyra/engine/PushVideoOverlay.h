// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "present" R1 (24 Nov 2026): the interface items that sit over the picture WHILE IT PLAYS (the delay badge, the
// statistics overlay, the PS5 network panel) are drawn INSIDE the swap chain instead of being cut out of the native video
// window's region. The research report (out/research/AMD_FG_WINDOWS_PRESENT_GPU.md, R1) found that in every presentation
// run of BENCH2 present-to-vblank was 7-9 ms with variable refresh not active exactly while that region was complex, and
// about 1 ms once it was one rectangle (a correlation from his logs; the cause is not proven). With this the window keeps
// one rectangle while he plays with the badge on.
//
// The flow, and who owns what:
//   QML item (PushLatencyBadge.qml ...) -> grabToImage -> bridge pushSetVideoOverlay (UI thread) composes every item into ONE
//   straight-alpha sRGB RGBA8 picture placed in the video window's client pixels -> PushVideoOverlayBoard::publish
//   -> VideoPresenter (engine thread, every present) takes a NEW version only, uploads it once, blends it in the present
//   pass after the picture (engine/PushPresentOverlay.h), and acknowledges the version it drew -> the bridge tells the QML
//   item that its picture is in the swap chain -> the item stops being a cover (videoCover false), the region is a rectangle.
// The QML item stays a cover (the old cut-out) until the presenter has drawn its picture, and again whenever the presenter
// has presented none for up to about 2 s (1 s here plus the item's refresh period; XeSS owning Present, the switch
// VEYRA_TEST_RP_LEGACY_VIDEO_OVERLAY=1 at once): never a moment with
// neither. Recordings, comparison clips and streams are taken from the graph's frame before this pass: the badge is never in
// them (VideoPresenter.cpp, the capture hook comes before the overlay draw).
//
// Also here: the region state published by apps/veyra-qml/main.cpp for the [present-path] line, and the decision of which
// region kind a set of covers gives (unit-tested). Portable, no Windows headers.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace veyra::engine {

// One composed picture: straight alpha (not premultiplied), sRGB-coded RGBA8, rows of width*4 bytes, placed at x,y in the
// video window's client pixels.
struct PushVideoOverlayImage {
    int x = 0, y = 0;
    unsigned width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
    bool valid() const { return width > 0 && height > 0 && rgba.size() == std::size_t(width) * height * 4; }
};

// UI thread publishes, the engine thread takes; a version number says what changed. take() copies a shared pointer under
// the lock only when the version moved (the presenter compares version() first: one relaxed load per present).
class PushVideoOverlayBoard {
public:
    // Publishes a picture (nullptr or an invalid one clears it). Returns the new version.
    std::uint64_t publish(std::shared_ptr<const PushVideoOverlayImage> image) {
        std::lock_guard lock(mutex_);
        image_ = image && image->valid() ? std::move(image) : nullptr;
        const auto v = version_.load(std::memory_order_relaxed) + 1;
        version_.store(v, std::memory_order_release);
        return v;
    }
    std::uint64_t version() const { return version_.load(std::memory_order_acquire); }
    std::shared_ptr<const PushVideoOverlayImage> take(std::uint64_t& version) const {
        std::lock_guard lock(mutex_);
        version = version_.load(std::memory_order_relaxed);
        return image_;
    }
    // The presenter drew `version` (or a later one) in a present at nowMs (steady clock).
    void drawn(std::uint64_t version, std::int64_t nowMs) {
        drawnVersion_.store(version, std::memory_order_relaxed);
        drawnAtMs_.store(nowMs, std::memory_order_release);
    }
    // The presenter stopped drawing (closed, XeSS, switch): every item becomes a cover again at once.
    void stopped() { drawnAtMs_.store(0, std::memory_order_release); }
    // True when a present within the last freshMs drew `version` or a later one.
    bool drawnSince(std::uint64_t version, std::int64_t nowMs, std::int64_t freshMs = 1000) const {
        const auto at = drawnAtMs_.load(std::memory_order_acquire);
        return version > 0 && at > 0 && nowMs - at <= freshMs && drawnVersion_.load(std::memory_order_relaxed) >= version;
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const PushVideoOverlayImage> image_;
    std::atomic<std::uint64_t> version_{0}, drawnVersion_{0};
    std::atomic<std::int64_t> drawnAtMs_{0};
};
inline PushVideoOverlayBoard& pushVideoOverlayBoard() {
    static PushVideoOverlayBoard board;
    return board;
}

// Where the picture lands in the swap chain's buffer. The buffer normally equals the window's client size; during a deferred
// resize DXGI stretches the old buffer to the window, so the picture is placed in buffer pixels by the same ratio. right and
// bottom are exclusive; scale = buffer pixels per window pixel. Nothing to draw when the rectangle is empty.
struct PushOverlayPlacement {
    int left = 0, top = 0, right = 0, bottom = 0;
    float scaleX = 1.0f, scaleY = 1.0f;
    float originX = 0.0f, originY = 0.0f;   // the picture's top left in buffer pixels (may be outside the buffer)
    bool empty() const { return right <= left || bottom <= top; }
};
inline PushOverlayPlacement pushOverlayPlacement(int x, int y, unsigned width, unsigned height, unsigned windowW, unsigned windowH,
                                                 unsigned bufferW, unsigned bufferH) {
    PushOverlayPlacement p;
    if (!width || !height || !windowW || !windowH || !bufferW || !bufferH) return p;
    p.scaleX = float(bufferW) / float(windowW);
    p.scaleY = float(bufferH) / float(windowH);
    p.originX = float(x) * p.scaleX;
    p.originY = float(y) * p.scaleY;
    const auto lo = [](float v, unsigned limit) { return std::clamp(int(std::floor(v)), 0, int(limit)); };
    const auto hi = [](float v, unsigned limit) { return std::clamp(int(std::ceil(v)), 0, int(limit)); };
    p.left = lo(p.originX, bufferW);
    p.top = lo(p.originY, bufferH);
    p.right = hi(float(x + int(width)) * p.scaleX, bufferW);
    p.bottom = hi(float(y + int(height)) * p.scaleY, bufferH);
    return p;
}

// ---- The video window's region, for [present-path] ----------------------------------------------------------------------
// Full: no region (the whole client rectangle). Rect: a region that is one rectangle (covers only outside the picture, or
// VEYRA_EXP_PRESENT_RECT_REGION). Complex: holes or the rounded cineIn band. Empty: blanked while a new source opens.
// [present-path] also logs GetWindowRgn's own answer (windowRgn) beside this decision.
enum class PushRegionKind : unsigned { Full, Rect, Complex, Empty };
inline const char* pushRegionKindName(PushRegionKind k) {
    switch (k) {
    case PushRegionKind::Full: return "full";
    case PushRegionKind::Rect: return "rect";
    case PushRegionKind::Complex: return "complex";
    case PushRegionKind::Empty: return "empty";
    }
    return "unknown";
}
// The decision syncVideoCovers makes (apps/veyra-qml/main.cpp), in its order: the no-region probe (no region); covers
// over the picture (holes: complex, or one rectangle with the rect experiment); the cineIn band when there is no cover at
// all (complex); any cover at all, even wholly outside the picture (review S5: the retracted dock bar), installs a region
// that is one rectangle (rect); otherwise no region (full). coversInPicture counts covers that reach into the window.
struct PushRegionInput {
    std::size_t coversInPicture = 0;
    bool insetBand = false;        // the cineIn animation's rounded band
    bool rectExperiment = false;   // VEYRA_EXP_PRESENT_RECT_REGION with a complete cover list
    bool noRegionProbe = false;
    std::size_t covers = 0;        // every cover, also those wholly outside the picture
};
inline PushRegionKind pushRegionDecision(const PushRegionInput& in) {
    if (in.noRegionProbe) return PushRegionKind::Full;
    if (in.coversInPicture > 0) return in.rectExperiment ? PushRegionKind::Rect : PushRegionKind::Complex;
    if (in.covers > 0) return PushRegionKind::Rect;
    if (in.insetBand) return PushRegionKind::Complex;
    return PushRegionKind::Full;
}

// Published by the UI thread on every region change, read by the presenter for [present-path].
struct PushRegionState {
    PushRegionKind kind = PushRegionKind::Full;
    unsigned coversInPicture = 0;
    bool insetBand = false;
    std::string items;   // the covers over the picture by QML type / object name of this program's own interface ("none")
    bool operator==(const PushRegionState&) const = default;
};
class PushRegionBoard {
public:
    void set(PushRegionState s) {
        std::lock_guard lock(mutex_);
        if (s == state_) return;
        state_ = std::move(s);
        serial_.fetch_add(1, std::memory_order_release);
    }
    std::uint64_t serial() const { return serial_.load(std::memory_order_acquire); }
    PushRegionState get() const {
        std::lock_guard lock(mutex_);
        return state_;
    }

private:
    mutable std::mutex mutex_;
    PushRegionState state_;
    std::atomic<std::uint64_t> serial_{0};
};
inline PushRegionBoard& pushRegionBoard() {
    static PushRegionBoard board;
    return board;
}

}  // namespace veyra::engine
