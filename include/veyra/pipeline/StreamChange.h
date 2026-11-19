// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// How much a decoded picture differs from the one before it: the measurement behind
// remoteplay/StreamCadence.h.
//
// Two views of the luma plane, both against the previous picture and both in 8-bit steps of the
// plane's code values (a 10-bit plane in a 16-bit word counts as its normalised value x 255):
//
//  blocks   the plane reduced to the mean of every 16x16 block (blocks cut off by the right and
//           bottom edges hold the samples they have), each mean compared with the same block before:
//             maxBlock      the largest difference of any block
//             meanBlock     the mean difference over all blocks
//             changedShare  the share of blocks that moved by more than one step
//  samples  every luma sample compared with the same sample before:
//             pixelMax      the largest difference of any sample
//             pixelsOver    how many samples differ by more than kStreamChangePixelStep steps
//
// The block view shows the rhythm of a doubled stream. It cannot see what moves inside a block: a
// 6x6 object moving one pixel, a drifting particle. The sample view can, and "nothing changed"
// has to mean that no sample changed (independent review, 18 November 2026).
//
// This header is the portable half: what the GPU pass (shaders/PushStreamChange.hlsl, run by
// pipeline/StreamChangePass.h) writes into its result words and how they are read, and the same
// measurement on CPU planes, which is the reference the GPU test compares with and what
// tools/remoteplay_latency_bench/cadence_streams.py computes with numpy. No platform types.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace veyra::pipeline {

inline constexpr unsigned kStreamChangeBlock = 16;
inline constexpr float kStreamChangeStep = 1.0f;          // a block "changed" when it moved by more than this
inline constexpr std::uint32_t kStreamChangeFixed = 256;  // differences in the result words are 8.8 fixed point
// A sample counts in pixelsOver when it differs by more than this many 8-bit steps. The comparison
// is made half a step higher (kStreamChangePixelLimit), where no 8-bit difference lies, so the
// float arithmetic of the shader and the integers of the reference agree.
inline constexpr unsigned kStreamChangePixelStep = 4;
inline constexpr float kStreamChangePixelLimit = float(kStreamChangePixelStep) + 0.5f;

// The result buffer of the GPU pass, in this order (cleared before every dispatch).
struct StreamChangeWords {
    std::uint32_t maxFixed = 0;       // largest block difference x 256
    std::uint32_t sumFixed = 0;       // sum of all block differences x 256
    std::uint32_t changed = 0;        // blocks that moved by more than one step
    std::uint32_t blocks = 0;         // blocks that reported in (must be all of them)
    std::uint32_t pixelMaxFixed = 0;  // largest sample difference x 256
    std::uint32_t pixelsOver = 0;     // samples that differ by more than kStreamChangePixelStep
    std::uint32_t pixels = 0;         // samples that were read (must be all of them)
    std::uint32_t reserved = 0;
};
inline constexpr unsigned kStreamChangeWordCount = 8;
static_assert(sizeof(StreamChangeWords) == kStreamChangeWordCount * sizeof(std::uint32_t), "StreamChangeWords is the readback buffer");

struct StreamChangeMeasurement {
    bool valid = false;        // the numbers are a measurement of this picture
    bool comparable = false;   // ... against the picture before it (false: the first after a reset)
    float maxBlock = 0.0f, meanBlock = 0.0f, changedShare = 0.0f;
    float pixelMax = 0.0f;     // largest difference of any luma sample, 8-bit steps
    float pixelsOver = 0.0f;   // samples that differ by more than kStreamChangePixelStep (a count)
};

// What one call of the GPU measurement gives the engine (pipeline/StreamChangePass.h).
struct StreamChangeOutcome {
    StreamChangeMeasurement now;    // this picture; not valid: no number, keep the frame
    StreamChangeMeasurement late;   // the picture before, when its answer missed the wait (hasLate)
    bool hasLate = false;
    bool timedOut = false;          // the GPU did not answer within the wait
    bool queued = false;            // ... and this picture's measurement is still running: it comes as `late` next time
    bool reading = false;           // a submitted list may still read this picture's texture: the caller keeps the
                                    // texture alive until StreamChangeRunner::reading() is false
    double waitMs = 0.0;            // how long the calling thread waited for this picture
};

// What a fence says about work that was signalled as `target`. A removed device answers
// UINT64_MAX for every fence (ID3D12Fence::GetCompletedValue): nothing finished, and what the
// readback buffer holds is not this picture's result. That is a failure, never a completion.
enum class StreamChangeFence : std::uint8_t { Running, Done, Lost };
inline StreamChangeFence streamChangeFence(std::uint64_t completed, std::uint64_t target) {
    if (completed == (std::numeric_limits<std::uint64_t>::max)()) return StreamChangeFence::Lost;
    return completed >= target ? StreamChangeFence::Done : StreamChangeFence::Running;
}

// The longest the engine thread waits for one picture's measurement: 4 ms, in 100 ns units.
inline constexpr std::int64_t kStreamChangeWait100ns = 40000;
inline constexpr std::int64_t streamChangeWaitClamp(std::int64_t wait100ns) {
    return wait100ns <= 0 ? 0 : wait100ns < kStreamChangeWait100ns ? wait100ns : kStreamChangeWait100ns;
}
// How long it may wait now. The same thread presents frames at exact times, so the wait never
// passes the moment the live scheduler has to wake for the next presentation (wake100ns, on the
// clock of now100ns; 0 or negative: nothing is scheduled). Due now or overdue: no wait at all;
// the measurement then arrives with the next picture and this frame is kept without a number.
inline constexpr std::int64_t streamChangeWaitBudget(std::int64_t now100ns, std::int64_t wake100ns) {
    if (wake100ns <= 0) return kStreamChangeWait100ns;
    if (wake100ns <= now100ns) return 0;
    if (now100ns < 0) return kStreamChangeWait100ns;              // wake - now could overflow, and is more than 4 ms anyway
    return streamChangeWaitClamp(wake100ns - now100ns);
}

struct StreamChangeGrid {
    unsigned x = 0, y = 0;
    constexpr unsigned count() const { return x * y; }
};

// Every sample belongs to a block: the blocks at the right and bottom edges are smaller.
inline constexpr StreamChangeGrid streamChangeBlocks(unsigned width, unsigned height) {
    return {(width + kStreamChangeBlock - 1) / kStreamChangeBlock, (height + kStreamChangeBlock - 1) / kStreamChangeBlock};
}

// The largest picture the pass measures, and the smallest. Its sum of block differences is one
// 32-bit word of 8.8 fixed point; with every block moving by the full 255 steps that holds up to
// this size (checked below), and no further. A larger picture is not measured: its frames are
// kept unnumbered. The sample count of such a picture fits 32 bits as well.
inline constexpr unsigned kStreamChangeMaxWidth = 4096, kStreamChangeMaxHeight = 2304;
static_assert(std::uint64_t(streamChangeBlocks(kStreamChangeMaxWidth, kStreamChangeMaxHeight).count()) * 255u * kStreamChangeFixed <=
              (std::numeric_limits<std::uint32_t>::max)(), "the sum of block differences must fit its 32-bit word");
inline constexpr bool streamChangeMeasurable(unsigned width, unsigned height) {
    return width >= kStreamChangeBlock && height >= kStreamChangeBlock && width <= kStreamChangeMaxWidth && height <= kStreamChangeMaxHeight;
}

// Reads the GPU result of a width x height picture. Anything that cannot be a complete
// measurement is rejected: the caller then has no number for this picture and keeps the frame.
inline StreamChangeMeasurement decodeStreamChange(const StreamChangeWords& words, unsigned width, unsigned height, bool comparable) {
    StreamChangeMeasurement m;
    if (!streamChangeMeasurable(width, height)) return m;
    const unsigned expectedBlocks = streamChangeBlocks(width, height).count();
    const std::uint32_t expectedPixels = std::uint32_t(width) * std::uint32_t(height);
    if (words.blocks != expectedBlocks || words.pixels != expectedPixels || words.changed > words.blocks || words.pixelsOver > words.pixels ||
        words.maxFixed > 255u * kStreamChangeFixed || words.pixelMaxFixed > 255u * kStreamChangeFixed) return m;
    m.valid = true; m.comparable = comparable;
    if (!comparable) return m;
    m.maxBlock = static_cast<float>(words.maxFixed) / static_cast<float>(kStreamChangeFixed);
    m.meanBlock = static_cast<float>(static_cast<double>(words.sumFixed) / (static_cast<double>(kStreamChangeFixed) * expectedBlocks));
    m.changedShare = static_cast<float>(words.changed) / static_cast<float>(words.blocks);
    m.pixelMax = static_cast<float>(words.pixelMaxFixed) / static_cast<float>(kStreamChangeFixed);
    m.pixelsOver = static_cast<float>(words.pixelsOver);
    return m;
}

// One luma sample in 8-bit steps. bytesPerSample 1 (8-bit) or 2 (little-endian 16-bit words, as
// P010 stores them).
inline float streamChangeSample(const std::uint8_t* at, unsigned bytesPerSample) {
    return bytesPerSample == 2 ? static_cast<float>(static_cast<double>(unsigned(at[0]) | (unsigned(at[1]) << 8)) * (255.0 / 65535.0)) : static_cast<float>(at[0]);
}

// CPU reference: block means of a luma plane, in 8-bit steps.
inline bool streamChangeBlockMeans(const std::uint8_t* luma, std::size_t pitch, unsigned width, unsigned height,
                                   unsigned bytesPerSample, std::vector<float>& means) {
    if (!luma || (bytesPerSample != 1 && bytesPerSample != 2) || !streamChangeMeasurable(width, height) || pitch < std::size_t(width) * bytesPerSample) return false;
    const auto grid = streamChangeBlocks(width, height);
    means.assign(grid.count(), 0.0f);
    const double scale = bytesPerSample == 2 ? 255.0 / 65535.0 : 1.0;
    for (unsigned by = 0; by < grid.y; ++by)
        for (unsigned bx = 0; bx < grid.x; ++bx) {
            const unsigned x0 = bx * kStreamChangeBlock, y0 = by * kStreamChangeBlock;
            const unsigned columns = width - x0 < kStreamChangeBlock ? width - x0 : kStreamChangeBlock, rows = height - y0 < kStreamChangeBlock ? height - y0 : kStreamChangeBlock;
            std::uint64_t sum = 0;
            for (unsigned y = 0; y < rows; ++y) {
                const std::uint8_t* row = luma + std::size_t(y0 + y) * pitch + std::size_t(x0) * bytesPerSample;
                for (unsigned x = 0; x < columns; ++x)
                    sum += bytesPerSample == 2 ? std::uint64_t(row[2 * x]) | (std::uint64_t(row[2 * x + 1]) << 8) : std::uint64_t(row[x]);
            }
            means[std::size_t(by) * grid.x + bx] = static_cast<float>(static_cast<double>(sum) * scale / double(columns * rows));
        }
    return true;
}

// The block words the GPU pass writes for these two pictures (same rounding as the shader).
inline StreamChangeWords streamChangeWords(const std::vector<float>& previous, const std::vector<float>& current) {
    StreamChangeWords words;
    if (previous.size() != current.size()) return words;
    for (std::size_t i = 0; i < current.size(); ++i) {
        const float difference = std::fabs(current[i] - previous[i]);
        const auto fixed = static_cast<std::uint32_t>(std::fmin(difference, 255.0f) * static_cast<float>(kStreamChangeFixed) + 0.5f);
        if (fixed > words.maxFixed) words.maxFixed = fixed;
        words.sumFixed += fixed;
        if (difference > kStreamChangeStep) ++words.changed;
        ++words.blocks;
    }
    return words;
}

// CPU reference of the whole pass: all the words it writes for the picture `after` when the
// picture before it was `before` (same size, pitch and sample width; same rounding as the shader).
inline StreamChangeWords streamChangeReference(const std::uint8_t* before, const std::uint8_t* after, std::size_t pitch,
                                               unsigned width, unsigned height, unsigned bytesPerSample) {
    std::vector<float> a, b;
    if (!streamChangeBlockMeans(before, pitch, width, height, bytesPerSample, a) || !streamChangeBlockMeans(after, pitch, width, height, bytesPerSample, b)) return {};
    StreamChangeWords words = streamChangeWords(a, b);
    float largest = 0.0f;
    for (unsigned y = 0; y < height; ++y) {
        const std::uint8_t* rowBefore = before + std::size_t(y) * pitch;
        const std::uint8_t* rowAfter = after + std::size_t(y) * pitch;
        if (bytesPerSample == 1) {   // the common case, kept free of floats
            unsigned rowMax = 0;
            for (unsigned x = 0; x < width; ++x) {
                const unsigned difference = rowAfter[x] > rowBefore[x] ? unsigned(rowAfter[x] - rowBefore[x]) : unsigned(rowBefore[x] - rowAfter[x]);
                if (difference > rowMax) rowMax = difference;
                if (difference > kStreamChangePixelStep) ++words.pixelsOver;
            }
            if (static_cast<float>(rowMax) > largest) largest = static_cast<float>(rowMax);
        } else {
            for (unsigned x = 0; x < width; ++x) {
                const float difference = std::fabs(streamChangeSample(rowAfter + std::size_t(x) * 2, 2) - streamChangeSample(rowBefore + std::size_t(x) * 2, 2));
                if (difference > largest) largest = difference;
                if (difference > kStreamChangePixelLimit) ++words.pixelsOver;
            }
        }
    }
    words.pixelMaxFixed = static_cast<std::uint32_t>(std::fmin(largest, 255.0f) * static_cast<float>(kStreamChangeFixed) + 0.5f);
    words.pixels = std::uint32_t(width) * std::uint32_t(height);
    return words;
}

// CPU reference of the block view, without the fixed-point rounding.
inline StreamChangeMeasurement streamChangeCompare(const std::vector<float>& previous, const std::vector<float>& current) {
    StreamChangeMeasurement m;
    if (current.empty() || previous.size() != current.size()) return m;
    double sum = 0.0; float largest = 0.0f; std::size_t changed = 0;
    for (std::size_t i = 0; i < current.size(); ++i) {
        const float difference = std::fabs(current[i] - previous[i]);
        if (difference > largest) largest = difference;
        sum += double(difference);
        if (difference > kStreamChangeStep) ++changed;
    }
    m.valid = true; m.comparable = true; m.maxBlock = largest;
    m.meanBlock = static_cast<float>(sum / double(current.size()));
    m.changedShare = static_cast<float>(changed) / static_cast<float>(current.size());
    return m;
}

} // namespace veyra::pipeline
