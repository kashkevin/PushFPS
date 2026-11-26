// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "s12-pace": the maths of the frame probe (out/research/SMOOTHNESS_60_VS_90.md section 5): "are all generated frames
// genuinely unique" answered from the pictures. Every frame handed to Present (real and generated) is reduced on the GPU to a
// kWidth x kHeight luma thumbnail (engine/PushFrameThumb.h, shaders/PushFrameThumb.hlsl: 4 x 4 point taps per cell, the same
// sampling as pushFrameThumbCpu below) and read back asynchronously. One group as presented: A (real), I1 .. I(M-1)
// (generated), B (real):
//   distance(x, y)       mean absolute thumbnail difference
//   steps                d(A, I1), d(I1, I2), ..., d(I(M-1), B); evenness = min step / max step (1 = equal steps); stepShare =
//                        step / sum of steps (ideal 1/M each: thirds at 3X, halves at 2X)
//   nearDuplicate        a generated frame within kDuplicateShare of d(A, B) of a neighbour (a copy, not a new picture)
//   blendLikeness(I, t)  d(I, (1 - t) A + t B) / d(A, B): near 0 = a cross-fade of the two real frames (ghosting instead of
//                        motion); a motion-correct frame differs from the cross-fade wherever something moves
// Everything is also kept per generated POSITION (1 = first generated frame of the group, 2 = second, ...): at his VFG
// quality 5 the frame at 1/3 comes from the Low model and the one at 2/3 from the Medium model.
// Only groups with motion (d(A, B) above kMotionFloor) count. Honest limits: 16 point taps per cell sample the picture, they do
// not average it, so very small moving things (a crosshair) weigh little; L1 distance is not linear in displacement; the probe
// ranks frames, it does not judge quality.
// Pure, fixed memory except the assembler's frame store; tests: tests/remoteplay/OnScreenCadenceTests.inc.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace veyra::engine {

inline constexpr unsigned kPushThumbWidth = 128, kPushThumbHeight = 72, kPushThumbTaps = 4;

// The luma a thumbnail tap stores: Rec. 709 weights on the values the frame holds (PQ codes for HDR10, gamma-encoded SDR, both
// already perceptual); a float (scRGB, linear) frame is compressed by y / (1 + y) so the values stay within 0..1.
inline float pushThumbLuma(float r, float g, float b, bool floatFormat) {
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (floatFormat) return y > 0 ? y / (1.0f + y) : 0.0f;
    return std::clamp(y, 0.0f, 1.0f);
}
// The tap of sub-cell (sx, sy) of the whole picture (sx < kPushThumbWidth x kPushThumbTaps): the centre of the sub-cell, in
// integer arithmetic exactly as the shader does it.
inline unsigned pushThumbTapX(unsigned sx, unsigned width) { return std::min(width - 1, (sx * 2 + 1) * width / (kPushThumbWidth * kPushThumbTaps * 2)); }
inline unsigned pushThumbTapY(unsigned sy, unsigned height) { return std::min(height - 1, (sy * 2 + 1) * height / (kPushThumbHeight * kPushThumbTaps * 2)); }
// CPU reference of shaders/PushFrameThumb.hlsl. pixel(x, y) returns the luma of one pixel (pushThumbLuma of its value).
template <class Pixel>
void pushFrameThumbCpu(unsigned width, unsigned height, Pixel&& pixel, float* out) {
    for (unsigned cy = 0; cy < kPushThumbHeight; ++cy)
        for (unsigned cx = 0; cx < kPushThumbWidth; ++cx) {
            float sum = 0;
            for (unsigned j = 0; j < kPushThumbTaps; ++j)
                for (unsigned i = 0; i < kPushThumbTaps; ++i)
                    sum += pixel(pushThumbTapX(cx * kPushThumbTaps + i, width), pushThumbTapY(cy * kPushThumbTaps + j, height));
            out[cy * kPushThumbWidth + cx] = sum / float(kPushThumbTaps * kPushThumbTaps);
        }
}

class PushFrameProbe {
public:
    static constexpr std::size_t kWidth = kPushThumbWidth, kHeight = kPushThumbHeight, kCells = kWidth * kHeight, kMaxGroup = 9, kRing = 512;
    static constexpr std::size_t kPositions = kMaxGroup - 2;   // generated positions 1 .. kMaxGroup - 2
    // Review 2 SHOULD 3: a group counts as motion only above max(kMotionFloor, 3 x the 10th percentile of recent real-pair
    // distances) (capture noise, grain); motion groups are also counted by size (small < kLargeMotion <= large).
    static constexpr double kMotionFloor = 0.01, kLargeMotion = 0.03, kDuplicateShare = 0.10;
    static constexpr std::size_t kNoiseRing = 256;

    static double distance(const float* x, const float* y) {
        if (!x || !y) return -1.0;
        double sum = 0;
        for (std::size_t i = 0; i < kCells; ++i) sum += std::abs(double(x[i]) - double(y[i]));
        return sum / double(kCells);
    }
    // -1 when A and B do not differ (nothing to compare against).
    static double blendLikeness(const float* frame, const float* a, const float* b, double t) {
        if (!frame || !a || !b) return -1.0;
        const double d = distance(a, b);
        if (!(d > 0)) return -1.0;
        double sum = 0;
        for (std::size_t i = 0; i < kCells; ++i) sum += std::abs(double(frame[i]) - ((1.0 - t) * double(a[i]) + t * double(b[i])));
        return sum / double(kCells) / d;
    }
    // One group in presentation order: frames[0] = A, frames[count - 1] = B, the others generated (count = multiplier + 1).
    void group(const float* const* frames, std::size_t count) {
        if (!frames || count < 3 || count > kMaxGroup) return;
        ++groups_;
        const double d = distance(frames[0], frames[count - 1]);
        const double floor = motionFloor();
        noise_[noiseNext_++ % kNoiseRing] = d;
        if (noiseCount_ < kNoiseRing) ++noiseCount_;
        if (!(d > floor)) { ++stillGroups_; return; }
        ++motionGroups_;
        ++(d > kLargeMotion ? largeMotionGroups_ : smallMotionGroups_);
        const std::size_t multiplier = count - 1;
        if (multiplier != lastMultiplier_) {   // shares of another multiplier are not comparable
            for (auto& s : stepShare_) s.count = 0;
            lastMultiplier_ = multiplier;
        }
        std::array<double, kMaxGroup> steps{};
        double total = 0;
        for (std::size_t i = 0; i + 1 < count; ++i) total += (steps[i] = distance(frames[i], frames[i + 1]));
        const auto first = steps.begin(), last = steps.begin() + static_cast<std::ptrdiff_t>(count - 1);
        const double hi = *std::max_element(first, last), lo = *std::min_element(first, last);
        evenness_.push(hi > 0 ? lo / hi : 1.0);
        if (total > 0)
            for (std::size_t i = 0; i + 1 < count; ++i) stepShare_[i].push(steps[i] / total);
        for (std::size_t i = 1; i + 1 < count; ++i) {
            ++generated_;
            auto& at = positions_[i - 1];
            ++at.generated;
            if (std::min(steps[i - 1], steps[i]) < kDuplicateShare * d) { ++nearDuplicates_; ++at.nearDuplicates; }
            const double blend = blendLikeness(frames[i], frames[0], frames[count - 1], double(i) / double(count - 1));
            blend_.push(blend);
            at.blend.push(blend);
        }
    }
    // A frame that could not be probed (no readback slot, a skipped Present, another queue), or a group that broke.
    // The motion floor in force: max(kMotionFloor, 3 x the 10th percentile of the last kNoiseRing real-pair distances).
    double motionFloor() {
        if (noiseCount_ < 32) return kMotionFloor;
        scratch_.assign(noise_.begin(), noise_.begin() + static_cast<std::ptrdiff_t>(noiseCount_));
        const std::size_t rank = noiseCount_ / 10;
        std::nth_element(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(rank), scratch_.end());
        return std::max(kMotionFloor, 3.0 * scratch_[rank]);
    }
    void skipped(std::uint64_t frames = 1) { skipped_ += frames; }
    void broken() { ++brokenGroups_; }

    struct Position { std::uint64_t generated = 0, nearDuplicates = 0; double blendLikeP50 = -1; };
    struct Summary {
        std::uint64_t groups = 0, motionGroups = 0, generated = 0, nearDuplicates = 0, skipped = 0, brokenGroups = 0;
        std::uint64_t stillGroups = 0, smallMotionGroups = 0, largeMotionGroups = 0;
        double motionFloor = -1;
        double nearDuplicatePct = -1, evennessP50 = -1, evennessMin = -1, blendLikeP50 = -1;
        std::size_t multiplier = 0;
        std::array<double, kMaxGroup - 1> stepShareP50{};   // -1 unknown
        std::array<Position, kPositions> positions{};
    };
    Summary take() {
        Summary s;
        s.groups = groups_;
        s.motionGroups = motionGroups_;
        s.generated = generated_;
        s.nearDuplicates = nearDuplicates_;
        s.skipped = skipped_;
        s.brokenGroups = brokenGroups_;
        s.stillGroups = stillGroups_;
        s.smallMotionGroups = smallMotionGroups_;
        s.largeMotionGroups = largeMotionGroups_;
        s.motionFloor = motionFloor();
        s.multiplier = lastMultiplier_;
        s.nearDuplicatePct = generated_ ? 100.0 * double(nearDuplicates_) / double(generated_) : -1.0;
        s.evennessP50 = evenness_.median(scratch_);
        s.evennessMin = evenness_.min();
        s.blendLikeP50 = blend_.median(scratch_);
        for (std::size_t i = 0; i < s.stepShareP50.size(); ++i) s.stepShareP50[i] = stepShare_[i].median(scratch_);
        for (std::size_t i = 0; i < kPositions; ++i) {
            s.positions[i].generated = positions_[i].generated;
            s.positions[i].nearDuplicates = positions_[i].nearDuplicates;
            s.positions[i].blendLikeP50 = positions_[i].blend.median(scratch_);
        }
        groups_ = motionGroups_ = generated_ = nearDuplicates_ = skipped_ = brokenGroups_ = 0;
        stillGroups_ = smallMotionGroups_ = largeMotionGroups_ = 0;
        evenness_.count = blend_.count = 0;
        for (auto& r : stepShare_) r.count = 0;
        for (auto& p : positions_) { p.generated = p.nearDuplicates = 0; p.blend.count = 0; }
        return s;
    }

private:
    struct Ring {
        std::array<double, kRing> values{};
        std::size_t count = 0;
        void push(double v) {
            if (count < kRing && v >= 0 && std::isfinite(v)) values[count++] = v;
        }
        double median(std::vector<double>& scratch) const {
            if (!count) return -1.0;
            scratch.assign(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
            std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(count / 2), scratch.end());
            return scratch[count / 2];
        }
        double min() const { return count ? *std::min_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count)) : -1.0; }
    };
    struct PositionRing { std::uint64_t generated = 0, nearDuplicates = 0; Ring blend; };
    Ring evenness_, blend_;
    std::array<Ring, kMaxGroup - 1> stepShare_{};
    std::array<PositionRing, kPositions> positions_{};
    std::size_t lastMultiplier_ = 0;
    std::uint64_t groups_ = 0, motionGroups_ = 0, generated_ = 0, nearDuplicates_ = 0, skipped_ = 0, brokenGroups_ = 0;
    std::uint64_t stillGroups_ = 0, smallMotionGroups_ = 0, largeMotionGroups_ = 0;
    std::array<double, kNoiseRing> noise_{};
    std::size_t noiseNext_ = 0, noiseCount_ = 0;
    std::vector<double> scratch_ = std::vector<double>(kRing);
};

// Puts thumbnails back into groups (review 2 SHOULD 4: by subframe index, never by group size): frames arrive in presentation
// order with a running sequence number, the generated frame's subframe (1 .. M-1) and the group's frame count M (M - 1 generated
// + the real frame). A real frame closes the group that began with the previous real frame when exactly M - 1 generated frames
// with subframes 1, 2, ... came between; a gap in the sequence or a missing / out-of-order subframe breaks the group.
class PushFrameGroups {
public:
    explicit PushFrameGroups(PushFrameProbe& probe) : probe_(probe) {}
    void frame(std::uint64_t sequence, bool generated, unsigned subframe, unsigned groupFrames, const float* thumb) {
        if (!thumb) return;
        if (haveLast_ && sequence != lastSequence_ + 1) {
            probe_.skipped(sequence > lastSequence_ ? sequence - lastSequence_ - 1 : 1);
            breakGroup();
        }
        haveLast_ = true;
        lastSequence_ = sequence;
        if (generated) {
            if (!haveA_) return;   // no real frame before it in this run: cannot be grouped
            if (subframe != used_ + 1 || used_ + 2 >= PushFrameProbe::kMaxGroup) { breakGroup(); return; }
            store(1 + used_++, thumb);
            return;
        }
        if (haveA_ && used_ > 0 && groupFrames >= 2 && used_ + 1 == groupFrames) {
            store(1 + used_, thumb);
            std::array<const float*, PushFrameProbe::kMaxGroup> frames{};
            for (std::size_t i = 0; i < used_ + 2; ++i) frames[i] = frames_[i].data();
            probe_.group(frames.data(), used_ + 2);
            frames_[0].swap(frames_[1 + used_]);
        } else {
            if (haveA_ && used_ > 0) probe_.broken();   // a generated frame missing (dropped): not judged as another multiplier
            store(0, thumb);
        }
        haveA_ = true;
        used_ = 0;
    }
    void reset() { haveA_ = haveLast_ = false; used_ = 0; }

private:
    void breakGroup() {
        if (haveA_) probe_.broken();
        haveA_ = false;
        used_ = 0;
    }
    void store(std::size_t index, const float* thumb) {
        auto& f = frames_[index];
        f.assign(thumb, thumb + PushFrameProbe::kCells);
    }
    PushFrameProbe& probe_;
    std::array<std::vector<float>, PushFrameProbe::kMaxGroup> frames_{};
    std::size_t used_ = 0;
    std::uint64_t lastSequence_ = 0;
    bool haveA_ = false, haveLast_ = false;
};

inline std::string formatPushFrameProbe(const PushFrameProbe::Summary& s) {
    const auto num = [](double v, int digits) {
        char b[32];
        if (v < 0 || !std::isfinite(v)) return std::string("-1");
        std::snprintf(b, sizeof b, "%.*f", digits, v);
        return std::string(b);
    };
    std::string shares, positions;
    for (std::size_t i = 0; s.multiplier && i < s.multiplier && i < s.stepShareP50.size(); ++i) shares += (i ? "," : "") + num(s.stepShareP50[i], 3);
    for (std::size_t i = 0; i < s.positions.size(); ++i) {
        const auto& p = s.positions[i];
        if (!p.generated) continue;
        positions += (positions.empty() ? "" : ";") + std::to_string(i + 1) + ":n=" + std::to_string(p.generated) + ",dupPct=" +
                     num(100.0 * double(p.nearDuplicates) / double(p.generated), 1) + ",blendP50=" + num(p.blendLikeP50, 3);
    }
    char line[900];
    std::snprintf(line, sizeof line,
        "groups=%llu stillGroups=%llu motionGroups=%llu smallMotion=%llu largeMotion=%llu motionFloor=%s multiplier=%zu generated=%llu nearDuplicatePct=%s evennessP50=%s evennessMin=%s stepShareP50=%s "
        "blendLikeP50=%s byPosition=%s skippedFrames=%llu brokenGroups=%llu",
        static_cast<unsigned long long>(s.groups), static_cast<unsigned long long>(s.stillGroups), static_cast<unsigned long long>(s.motionGroups),
        static_cast<unsigned long long>(s.smallMotionGroups), static_cast<unsigned long long>(s.largeMotionGroups), num(s.motionFloor, 4).c_str(), s.multiplier,
        static_cast<unsigned long long>(s.generated), num(s.nearDuplicatePct, 1).c_str(), num(s.evennessP50, 3).c_str(), num(s.evennessMin, 3).c_str(),
        shares.empty() ? "-1" : shares.c_str(), num(s.blendLikeP50, 3).c_str(), positions.empty() ? "none" : positions.c_str(),
        static_cast<unsigned long long>(s.skipped), static_cast<unsigned long long>(s.brokenGroups));
    return std::string(line) +
        " (every presented frame reduced to a 128x72 luma thumbnail on the GPU, 16 point taps per cell, read back asynchronously; per group "
        "A, generated, B with motion above motionFloor (max(0.01, 3 x the 10th percentile of recent real-pair distances): grain and capture "
        "noise are not motion; small motion < 0.03 <= large): nearDuplicate = a generated frame within 10 % of d(A,B) of a neighbour; evenness = smallest / largest "
        "step; stepShare = each step / all steps, ideal 1/multiplier; blendLike = distance to the cross-fade of A and B / d(A,B), near 0 "
        "= ghosting instead of motion; byPosition = per generated frame of the group, 1 = first; thumbnails, not a quality judgement)";
}

}  // namespace veyra::engine
