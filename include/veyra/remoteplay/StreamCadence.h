// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Picture numbers for a hardware-decoded stream that shows every game picture twice.
//
// A console game running at 30 fps inside a 60 Hz output sends A A B B C C. The second copy is a
// new encoded frame, and a constant-bitrate encoder spends its bits on it too: it refines the first
// copy. So the second copy is neither small nor identical after decoding. Measured on libx264 and
// libx265 low-delay streams (tools/remoteplay_latency_bench/cadence_streams.py, 241 streams): the
// median repeat was 0.02 to 3.5 times the size of the median new frame, and a block of the decoded
// repeat differed from the first copy by up to 111 of 255 steps, more than a new picture differs
// in a slow scene. Neither size nor a fixed difference threshold separates them.
//
// What does separate them is the rhythm. `change` is how much the decoded picture differs from the
// previous decoded picture (largest difference of the mean luma of any 16x16 block, in 8-bit
// steps). In a doubled stream it goes high, low, high, low; ups and downs under `rhythmFloor` are
// encoder noise and do not count. Once the rhythm has held for `lockSteps` steps it is trusted, and
// from then on a picture is a repeat when ALL of this holds:
//   - it changed much less than the picture before it (`ratio`), and that picture had risen as
//     much above the one before it (so two falls in a row are never two repeats);
//   - it changed no more than `ceiling` times what recent repeats changed;
//   - the picture as a whole did not move (`meanVeto`, `shareVeto`), and its mean block change
//     fell as well (`meanRatio`): a repeat touches a few blocks, a new picture under a moving
//     camera touches all of them, on every picture;
//   - sample by sample it looks like the same picture coded again: no luma sample moved by more
//     than `pixelCeiling` steps, and far fewer samples moved than in the picture before it
//     (`pixelRatio`). The mean of a block cannot see what moves inside the block: a small object,
//     a particle, a cursor drawn at 60 fps over a world drawn at 30. One sample far off is such a
//     thing, and the picture is kept. This is the bound on the damage of a wrong answer: a picture
//     that is dropped does not differ from the picture kept before it by more than `pixelCeiling`
//     steps in any luma sample;
//   - signed evidence (second review): it changed one way in far fewer blocks than the picture
//     before it (`coherentRatio`), it did not touch or move the picture as a whole, and its
//     samples did not move one way as a whole (`touchedCeiling`, `absCeiling`, `signCeiling`);
//   - it stays near what this stream's confirmed repeats changed (the learned level, `floor*`);
//   - the picture before it was not a scene cut (after a cut nothing is comparable);
//   - the confidence of all that is at least `confidenceFloor`.
// A picture in the repeat's place that a sample or signed rule finds new counts against the
// rhythm: `mixedVetoes` of them within `mixedWindow` pictures give the rhythm up (something changes
// at the stream rate under a world drawn at half of it: such a stream is not halved), and it may
// not lock again for `mixedHold` pictures, doubling up to 15 s. While the rhythm is being learned,
// a fall that such a rule refuses breaks the run.
// Encoder refinement is not the zero-mean, incoherent noise one might expect: x264 repeats of real
// footage move 16 to 53 % of their absolute change one way, and up to a third of their blocks
// change one way. So the signed rules are relative (to the picture before, to the stream's own
// repeats) or sit above what real repeats reached (tools/remoteplay_latency_bench, tune set).
// Outside a trusted rhythm no picture is a repeat, however little its blocks moved: small block
// differences do not mean that nothing changed. (The first version called every picture under
// `identical` a repeat, and judged repeats by block means alone; an independent review found
// real 60 fps pictures dropped: a 6x6 object, a particle, a slow fade over a still picture.)
//
// Errors are one-sided on purpose: when unsure the picture counts as new, and the frame is kept
// (`fallback`). A kept repeat costs nothing visible; a dropped new picture is a lost game frame.
// The price of the sample rule is that a repeat is only found where the encoder's own refinement
// stays under `pixelCeiling`: on camera footage and photographs most repeats, on a synthetic
// scene of random blocks and on a fractal zoom almost none (their repeats move samples by 30 to
// 190 steps; they are kept).
//
// What is not protected (second review, measured): a small faint thing (a few samples within
// `pixelCeiling` steps of what is under them, a soft blob, film grain of sigma 1 to 2 or a camera
// creeping by a tenth of a sample once it has been through an encoder) drawn at 60 fps over a world
// drawn at 30, from the first picture on: its repeat-slot picture changes less than real refinement
// in every number the pass has, and there is no clean repeat to learn the level from; every second
// picture is dropped. The same things appearing after clean repeats were seen are caught by the
// learned level where the stream's refinement is small (lossless, or the high bitrates of the
// model). Chroma is not measured at all.
//
// Evidence of the first round, before the signed evidence (the second round's numbers are in
// docs/PS5_REMOTE_PLAY_EXPERIMENTS.md, "Second review"; Linux, software encoders, not a
// PS5; every stream through PushStreamDedup and the engine's ContentDedup): thresholds chosen on
// 90 tuning streams and 79 adversarial streams (the reviewer's scenes through real encoders, 720p
// and 1080p, in real 60 fps, inside a locked 30-in-60 rhythm, across a 30 -> 60 switch) and
// frozen: no new picture dropped in either (0 of 31,251 and 0 of 37,404; without the samples the
// adversarial set loses 4,143), with 6,334 of 11,949 and 6,145 of 9,996 repeats kept. Then, scored
// once, 111 hold-out streams of other content and encoder settings: 0 of 37,764 new pictures
// dropped, 7,440 of 15,516 repeats kept (film and camera clips: 2,542 of 7,461). Given the largest
// block alone (the two-argument observe) the hold-out loses 998 pictures: the engine always gives
// every number. More in docs/PS5_REMOTE_PLAY_EXPERIMENTS.md.
//
// Third review (19 November 2026), the owner's ruling: a kept repeat costs nothing visible, a dropped
// new picture is a lost game frame, so preserving genuine pictures outranks removing repeats. The
// numbers above cannot tell a faint thing drawn at 60 fps from an encoder's refinement of a repeat
// (third-round investigation in docs/PS5_REMOTE_PLAY_EXPERIMENTS.md: not by size, not by sign, not by
// the place of the largest change). So the engine runs with `exactOnly` (engine::streamDedupConfig):
// everything above still has to agree, and in addition the picture must be the previous decoded
// luma plane exactly, and the stream must have shown `resumeAfter` exact repeats in a row in the
// repeat's place since the last boundary and since anything last moved there twice in a row. A
// stream whose repeats are refined (x264 at any bitrate in the model) gets no drop at all: it
// behaves as with the switch off. Fourth review: "exact" includes chroma (Input::chromaChanged must
// be measured and 0). A colour-only change at 60 fps over exact luma repeats was dropped on almost
// every repeat slot before. The GPU pass does not read chroma yet, so in the engine nothing is ever
// exact: the experiment drops nothing at all until a chroma readback exists.
//
// Portable and free of platform types, so the unit tests run anywhere
// (tests/remoteplay/StreamCadenceTests.inc). It only numbers pictures;
// include/veyra/engine/ContentDedup.h decides what to do with repeats.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace veyra::remoteplay {

struct StreamCadenceConfig {
    float ratio = 0.4f;            // a fall or rise: one change is less than this share of the other
    float identical = 0.6f;        // the floor of the repeat level and of a scene cut; never by itself a repeat
    float ceiling = 2.5f;          // a repeat changes at most this many times the recent repeat level
    float meanVeto = 4.0f;         // mean block change at or above this: the whole picture moved, never a repeat
    float meanRatio = 0.25f;       // where the mean is known it must fall below this share of the mean before, too
    float meanFloor = 1.0f;        // a mean this large can keep a trusted rhythm through a rewritten block
    float shareVeto = 0.9f;        // share of blocks changed by more than one step at or above this: never a repeat
    float cutFactor = 3.0f;        // a change this many times the recent level of new pictures is a scene cut
    float marginGain = 2.0f;       // confidence falls to 0 as the fall approaches `ratio`
    float confidenceFloor = 0.5f;  // below this a picture is always new
    float rhythmFloor = 1.2f;      // a rise or fall counts only when the larger change is at least this: noise is not rhythm
    float pixelCeiling = 16.0f;    // a repeat changes no luma sample by more than this many 8-bit steps
    float pixelRatio = 0.4f;       // ... and changes at most this share of the samples the picture before it changed
    // Signed evidence (second review): what refinement of an identical picture cannot look like.
    float coherentCeiling = 0.35f; // a repeat changes at most this share of its blocks one way (streamChangeCoherent)
    float touchedCeiling = 0.98f;  // ... touches at most this share of its blocks at all
    float absCeiling = 0.8f;       // ... moves its samples by at most this much on average, 8-bit steps
    float coherentRatio = 0.5f;    // ... and changes one way at most this share of the blocks the picture before it changed
    float coherentSlack = 0.01f;   //     one way (plus this share of all blocks): a repeat is far quieter than a new picture;
    float coherentFrom = 0.05f;    //     judged only where the picture before it changed at least this share one way
    float signCeiling = 0.6f;      // ... and its samples do not move one way as a whole: |signed mean| at most this share of the absolute mean
    float signFloor = 0.02f;       // (the sign is judged only above this absolute mean, 8-bit steps)
    float floorMargin = 3.0f;      // a repeat's touched share, coherent share and absolute mean stay within this many times
                                   // the level of this stream's confirmed repeats ...
    float floorTouched = 0.1f;     // ... plus these slacks
    float floorCoherent = 0.05f;
    float floorAbs = 0.1f; 
    float floorPixelMargin = 3.0f; // and its largest sample change within this many times that level's largest sample change
    float floorPixel = 3.0f;       // ... plus this many steps
    unsigned floorSamples = 8;     // confirmed repeats before the learned level is used
    unsigned mixedVetoes = 4;      // repeat-slot pictures found new within `mixedWindow` pictures that unlock the rhythm:
    unsigned mixedWindow = 16;     // something keeps changing at the stream rate under a world drawn at half of it
    unsigned mixedHold = 120;      // pictures the rhythm may not lock again after that (doubles each time, up to 15 s)
    // Third review: exact repeats only (the engine's default, engine::streamDedupConfig()). A picture
    // the rules above call a repeat is dropped only when its decoded luma is the very picture before
    // it (no sample, block or word moved by `exactPixel`), and only while the stream has shown
    // `resumeAfter` such exact repeats in a row in the repeat's place, with no `suspendAfter`
    // repeat-slot pictures in a row since that changed anything (then it is suspended again).
    // False: the second round's verdict (refined repeats too), kept for the evaluation tool.
    bool exactOnly = false;
    float exactPixel = 0.5f;       // a sample, block or picture mean "did not move": below this, 8-bit steps
    unsigned suspendAfter = 2;     // repeat-slot pictures in a row that changed anything: nothing is dropped any more ...
    unsigned resumeAfter = 4;      // ... until this many exact ones in a row were seen (also at the start and after a boundary)
    unsigned lockSteps = 6;        // alternating steps before the rhythm is trusted
    unsigned fastLockSteps = 3;    // ... when it resumes on the phase it had before a still scene
    unsigned unlockMisses = 2;     // steps without a clear rise or fall the rhythm survives
    unsigned phaseMemory = 900;    // pictures a still scene keeps the phase for (15 s)
    std::int64_t nominalPeriod100ns = 166833;  // stream period when timestamps are missing (59.94 Hz)
};

enum class StreamCadenceState : std::uint8_t {
    Unknown,  // not enough pictures yet
    Unique,   // every picture is new (the game runs at the stream rate)
    Doubled,  // every picture comes twice and the rhythm is trusted
    Still     // the picture stands still (pause, menu): only encoder refinement is left
};

class StreamCadence {
public:
    struct Input {
        float change = -1.0f;        // largest block difference to the previous decoded picture, 8-bit steps
        float meanChange = -1.0f;    // mean block difference, 8-bit steps; negative = not measured
        float changedShare = -1.0f;  // share of blocks changed by more than one step; negative = not measured
        float pixelMax = -1.0f;      // largest difference of any luma sample, 8-bit steps; negative = not measured
        float pixelsOver = -1.0f;    // luma samples that differ by more than a small step (a count); negative = not measured
        // The signed evidence (pipeline::StreamChangeMeasurement); absMean negative = not measured.
        float signedMean = 0.0f;     // mean signed sample difference, 8-bit steps
        float absMean = -1.0f;       // mean absolute sample difference, 8-bit steps
        float touchedShare = -1.0f;  // share of blocks in which any sample changed
        float coherentShare = -1.0f; // share of blocks that changed one way
        float chromaChanged = -1.0f; // chroma samples that differ at all (a count); negative = not measured (fourth review)
        std::int64_t pts100ns = 0;   // stream time of the picture; 0 or not increasing = unknown
        bool boundary = false;       // first picture, decoder reset, lost frames, seek, or no comparison was
                                     // possible: the picture is new and the rhythm starts again
        bool sceneChange = false;    // the caller knows this picture starts another scene
    };
    struct Result {
        std::uint64_t picture = 0;   // equal numbers show the same picture; never 0
        bool repeat = false;
        bool locked = false;         // the doubled rhythm is currently trusted
        bool fallback = false;       // looked like a repeat, kept as new because the evidence was not enough
        bool sceneChange = false;    // told or found: this picture starts another scene
        float confidence = 0.0f;     // 0..1 of the repeat verdict, or of the rhythm for a new picture
        float sourceFps = 0.0f;      // unique pictures per second over the last second; 0 = not known yet
    };
    struct Stats {
        std::uint64_t pictures = 0, repeats = 0, locks = 0, fastLocks = 0, unlocks = 0;
        std::uint64_t boundaries = 0, fallbacks = 0, cuts = 0, pixelVetoes = 0;
        std::uint64_t signedVetoes = 0, floorVetoes = 0, mixedUnlocks = 0;
        std::uint64_t exactVetoes = 0, suspendedKeeps = 0, suspensions = 0, resumes = 0;   // exactOnly
    };

    explicit StreamCadence(StreamCadenceConfig config = {}) : config_(config) { forgetStream(); }

    Result observe(float change, bool boundary) {
        Input input; input.change = change; input.boundary = boundary;
        return observe(input);
    }

    Result observe(const Input& input) {
        ++stats_.pictures; ++index_;
        Result result;
        const float change = input.change;
        const bool valid = change >= 0.0f && change <= 1.0e6f;   // rejects NaN, negative and infinite
        if (input.boundary || !valid) {
            ++stats_.boundaries;
            restart();
            result.picture = ++picture_;
            result.sourceFps = rate(input.pts100ns, true);
            return result;
        }
        const float mean = input.meanChange >= 0.0f && input.meanChange <= 1.0e6f ? input.meanChange : -1.0f;
        const float share = input.changedShare >= 0.0f && input.changedShare <= 1.0f ? input.changedShare : -1.0f;
        // The per-sample evidence, where the caller has it (the engine always has). Left out means
        // both numbers negative; anything else that is not two sane numbers (NaN, one missing) is
        // given but unusable, and never reads as "nothing moved".
        const bool pixelsGiven = !(input.pixelMax < 0.0f && input.pixelsOver < 0.0f);
        const bool pixels = input.pixelMax >= 0.0f && input.pixelMax <= 1.0e6f && input.pixelsOver >= 0.0f && input.pixelsOver <= 1.0e12f;
        const float pixelsOver = pixels ? input.pixelsOver : -1.0f;
        // The signed evidence, the same way: left out (absMean, touchedShare and coherentShare all
        // negative) or given; given but not sane counts as "something changed".
        const bool signedGiven = !(input.absMean < 0.0f && input.touchedShare < 0.0f && input.coherentShare < 0.0f);
        const bool signedSane = input.absMean >= 0.0f && input.absMean <= 255.0f && input.touchedShare >= 0.0f && input.touchedShare <= 1.0f &&
                                input.coherentShare >= 0.0f && input.coherentShare <= input.touchedShare &&
                                std::fabs(input.signedMean) <= input.absMean + 1.0e-3f;   // also rejects NaN
        // A picture that moved as a whole is never a repeat, and never the low half of the rhythm.
        const bool whole = (mean >= 0.0f && mean >= config_.meanVeto) || (share >= 0.0f && share >= config_.shareVeto);
        // A scene cut: told, or a jump far above what new pictures changed a moment ago, in the
        // largest block or in the picture as a whole.
        const bool jump = newSamples_ >= 4 && change >= config_.cutFactor * newLevel_ && change >= 8.0f * config_.identical;
        const bool meanJump = newSamples_ >= 4 && mean >= config_.meanVeto && meanLevel_ >= 0.0f && mean >= config_.cutFactor * meanLevel_;
        const bool cut = input.sceneChange || jump || meanJump;
        if (cut) { ++stats_.cuts; result.sceneChange = true; }

        // Falls and rises of the largest block: the rhythm is learned from these alone. Under a still
        // camera, where only an object moves, the largest block is the only sign there is.
        // Once the rhythm is trusted the mean block change keeps it through one kind of repeat the
        // largest block misses: an encoder now and then rewrites single blocks of a repeat completely
        // (libx265: 25 to 80 steps in one block, while the mean stays a hundredth of a new picture's).
        // Such a repeat is kept, but it does not count against the rhythm. The mean never starts the
        // rhythm: a 60 fps game whose camera moves on every second picture only shows it too.
        bool down = false, up = false, meanDown = false, meanUp = false;
        if (havePrevious_) {
            const bool means = mean >= 0.0f && previousMean_ >= 0.0f;
            const bool meanFall = means && previousMean_ >= config_.meanFloor && mean < config_.ratio * previousMean_;
            const bool meanRise = means && mean >= config_.meanFloor && previousMean_ < config_.ratio * mean;
            // A camera that moves on every picture while something else moves on every second one
            // changes the largest block in this rhythm but not the picture as a whole: no fall. Where
            // the mean is known it has to fall too, however small it is: a slow pan moves the mean by
            // less than a step, and still every picture of it is new.
            const bool meanStays = means && mean >= config_.meanRatio * previousMean_;
            // Below the floor the largest block only shows encoder noise; its ups and downs are not a rhythm.
            const bool loud = std::max(change, previous_) >= config_.rhythmFloor;
            const bool fall = change < config_.ratio * previous_;
            down = loud && fall && !whole && !cut && !meanRise && !meanStays;
            up = loud && !down && previous_ < config_.ratio * change;
            meanDown = locked_ && !down && !up && meanFall && !whole && !cut;
            meanUp = locked_ && !down && !up && meanRise;
        }

        // Whether a picture in the repeat's place looks like the same picture coded again.
        // What a block mean cannot see: something small that moved inside a block. A repeat is
        // the same picture coded again: the encoder only refines it, so no sample moves far, and
        // the picture touches far fewer samples than the new picture before it did. One sample
        // far off, or as many samples touched as before, is something happening in the picture.
        // Evidence of a change counts against the rhythm (below); a number that is missing or not
        // usable only keeps the picture.
        const bool pixelChange = pixels && (input.pixelMax > config_.pixelCeiling || (previousPixels_ >= 0.0f && pixelsOver > config_.pixelRatio * previousPixels_));
        const bool pixelVeto = pixelsGiven && (!pixels || previousPixels_ < 0.0f || pixelChange);
        // What refinement of a picture already sent does not look like: as many blocks changed one
        // way as in the new picture before it (a camera that keeps moving), most blocks changed one
        // way, the whole picture touched or moved far on average (grain, clouds), every sample
        // moved the same way (a fade, a pulse). And it stays near what this stream's confirmed
        // repeats changed (the learned level).
        bool signedVeto = false, signedChange = false, floorVeto = false;
        if (signedGiven) {
            if (!signedSane) signedVeto = true;
            else {
                signedChange = input.coherentShare > config_.coherentCeiling || input.touchedShare > config_.touchedCeiling || input.absMean > config_.absCeiling ||
                               (previousCoherent_ >= config_.coherentFrom && input.coherentShare > config_.coherentRatio * previousCoherent_ + config_.coherentSlack) ||
                               (input.absMean >= config_.signFloor && std::fabs(input.signedMean) > config_.signCeiling * input.absMean);
                signedVeto = signedChange || previousCoherent_ < 0.0f;
                floorVeto = !signedVeto && floorCount_ >= config_.floorSamples &&
                            (input.touchedShare > config_.floorMargin * levelTouched_ + config_.floorTouched ||
                             input.coherentShare > config_.floorMargin * levelCoherent_ + config_.floorCoherent ||
                             input.absMean > config_.floorMargin * levelAbs_ + config_.floorAbs ||
                             (pixels && input.pixelMax > config_.floorPixelMargin * levelPixel_ + config_.floorPixel));
            }
        }

        // The rhythm: rises and falls in turn. While it is being learned, a fall whose picture does
        // not look like a repeat breaks it: a stream whose low half keeps changing does not lock.
        if (!locked_ && down && (pixelVeto || signedVeto)) { down = false; up = false; }
        if (down || up) {
            if (haveStep_ && down != lastDown_) ++run_;
            else { run_ = 1; runLow_ = 0.0f; fastRun_ = phaseKnown_; }
            haveStep_ = true; lastDown_ = down;
            if (down) {
                runLow_ = std::max(runLow_, change);
                if (fastRun_ && ((index_ & 1u) != phase_ || index_ > phaseUntil_)) fastRun_ = false;
            }
        } else {
            run_ = 0; haveStep_ = false; fastRun_ = false;
        }
        bool lockedNow = false;
        if (!locked_ && havePrevious_ && index_ >= holdUntil_) {
            const bool fast = fastRun_ && run_ >= config_.fastLockSteps;
            if (fast || run_ >= config_.lockSteps) {
                locked_ = true; lockedNow = true; misses_ = 0; trust_ = 0.5f; ceilingRun_ = 0; meanRun_ = 0; sinceRepeat_ = 0;
                ++stats_.locks;
                if (fast) ++stats_.fastLocks;
                else { repeatLevel_ = std::max(runLow_, config_.identical); levelKnown_ = true; }
                state_ = StreamCadenceState::Doubled;
            }
        }

        // Within what repeats themselves changed a moment ago (times `ceiling`).
        vetoes_ = (vetoes_ << 1) & windowMask();
        const bool within = levelKnown_ && change <= config_.ceiling * repeatLevel_;

        bool repeat = false, rewritten = false;
        float confidence = locked_ ? trust_ : 0.0f;
        // A repeat only in its place in a trusted rhythm: there is no "nothing changed" shortcut.
        const bool slot = locked_ && down && previousUp_ && !afterCut_;   // a picture in the repeat's place of a trusted rhythm
        if (locked_ && down) {
            const float margin = std::clamp(config_.marginGain * (1.0f - change / (config_.ratio * previous_)), 0.0f, 1.0f);
            confidence = std::min(trust_, margin);
            if (!previousUp_ || afterCut_) result.fallback = true;
            else if (pixelVeto) { result.fallback = true; ++stats_.pixelVetoes; if (pixelChange) vetoes_ |= 1u; }
            else if (signedVeto) { result.fallback = true; ++stats_.signedVetoes; if (signedChange) vetoes_ |= 1u; }
            else if (floorVeto) { result.fallback = true; ++stats_.floorVetoes; vetoes_ |= 1u; }
            else if (!within) {
                result.fallback = true;
                // Three in a row above the ceiling, each in its place in the rhythm: the encoder's
                // repeats have grown (less bitrate); learn their level again.
                ceilingMax_ = ceilingRun_ ? std::max(ceilingMax_, change) : change;
                if (++ceilingRun_ >= 3) { repeatLevel_ = ceilingMax_; ceilingRun_ = 0; }
            } else if (confidence < config_.confidenceFloor) result.fallback = true;
            else repeat = true;
        } else if (meanDown && previousUp_ && !afterCut_) {
            // A repeat by the mean only (a rewritten block): kept. The rhythm goes on through a few of
            // these in a row, not for ever: the largest block has to confirm it again.
            result.fallback = true; rewritten = ++meanRun_ <= kMeanRun;
        }

        if (repeat && signedGiven && signedSane) {
            // The learned level: what this stream's confirmed repeats change (a running mean).
            if (floorCount_ == 0) { levelTouched_ = input.touchedShare; levelCoherent_ = input.coherentShare; levelAbs_ = input.absMean; levelPixel_ = pixels ? input.pixelMax : 0.0f; }
            else {
                if (pixels) levelPixel_ += (input.pixelMax - levelPixel_) * kLevelGain;
                levelTouched_ += (input.touchedShare - levelTouched_) * kLevelGain;
                levelCoherent_ += (input.coherentShare - levelCoherent_) * kLevelGain;
                levelAbs_ += (input.absMean - levelAbs_) * kLevelGain;
            }
            if (floorCount_ < 1000u) ++floorCount_;
        }
        if (locked_ && !lockedNow && std::popcount(vetoes_) >= int(config_.mixedVetoes)) {
            // Pictures in the repeat's place keep turning out new: something changes at the stream
            // rate while the rest of the picture keeps the doubled rhythm. Such a stream is not
            // halved: the rhythm is given up and may not lock again for a while.
            unlock(false);
            ++stats_.mixedUnlocks;
            holdUntil_ = index_ + hold_;
            hold_ = std::min(hold_ * 2u, kMaxHold);
            vetoes_ = 0;
        }
        if (locked_ && !lockedNow) {
            // A rhythm that finds no repeat for a dozen pictures is not a doubled stream any more,
            // whatever the largest block does (rises alone keep the count of misses at zero).
            if (repeat || rewritten) sinceRepeat_ = 0;
            else if (++sinceRepeat_ > kBarren) unlock(false);
        }
        if (locked_ && !lockedNow) {
            if (repeat) {
                misses_ = 0; ceilingRun_ = 0; meanRun_ = 0; trust_ += (1.0f - trust_) * 0.5f;
                repeatLevel_ = std::max({change, repeatLevel_ * 0.98f, config_.identical});
                phase_ = index_ & 1u; phaseKnown_ = true;
            } else if (up || rewritten || (meanUp && meanRun_ <= kMeanRun)) {
                misses_ = 0; trust_ += (1.0f - trust_) * 0.5f;
            } else if (!repeat) {
                if (!(down && !within && previousUp_ && !afterCut_)) ceilingRun_ = 0;
                trust_ *= 0.7f;
                missWithin_ = misses_ ? (missWithin_ && within) : within;
                if (++misses_ > config_.unlockMisses) unlock(missWithin_);
            }
        } else if (!locked_) {
            // Outside the rhythm: is the game still, or does it show a new picture every time?
            if (state_ == StreamCadenceState::Still) {
                if (index_ > phaseUntil_) phaseKnown_ = false;
                if (!within && !down && !up) { if (++bigFlat_ >= 3) { state_ = StreamCadenceState::Unique; phaseKnown_ = false; } }
                else if (within) bigFlat_ = 0;
            } else if (state_ == StreamCadenceState::Unknown && ++unknown_ >= 2 * config_.lockSteps) {
                state_ = StreamCadenceState::Unique;
            }
        }

        if (cut) {
            // The scene after a cut may move at any speed: start its level so that the same jump
            // again is not a second cut.
            newLevel_ = std::max(newLevel_, change * 0.5f);
            if (mean >= 0.0f) meanLevel_ = std::max(meanLevel_, mean * 0.5f);
        } else if (!repeat) {
            if (mean >= 0.0f) meanLevel_ = newSamples_ && meanLevel_ >= 0.0f ? std::max(mean, meanLevel_ * 0.95f) : mean;
            // The level of new pictures, for finding cuts: the largest recent change, fading 5 % per
            // picture, so the repeats that are kept while the rhythm is being learned do not lower it.
            newLevel_ = newSamples_ ? std::max(change, newLevel_ * 0.95f) : change;
            if (newSamples_ < 1000u) ++newSamples_;
        }
        previousUp_ = up || meanUp;
        afterCut_ = cut;
        previous_ = change; previousMean_ = mean; previousPixels_ = pixelsOver; havePrevious_ = true;
        previousCoherent_ = signedGiven && signedSane ? input.coherentShare : -1.0f;

        if (config_.exactOnly) {
            // The rhythm above was kept with the second round's verdict; what is dropped is decided
            // here, and only for a picture in the repeat's place (slot). Anything that moved keeps the
            // picture and, twice in a row, suspends dropping until exact repeats are seen again.
            if (slot) {
                const bool exact = pixels && signedGiven && signedSane && change <= config_.exactPixel && input.pixelMax <= config_.exactPixel &&
                                   pixelsOver <= 0.0f && input.absMean <= 0.0f && input.touchedShare <= 0.0f &&
                                   input.coherentShare <= 0.0f && (mean < 0.0f || mean <= config_.exactPixel) &&
                                   input.chromaChanged == 0.0f;   // chroma measured and identical: a colour-only change is not a repeat
                const bool wasSuspended = suspended_;
                if (repeat && !exact) ++stats_.exactVetoes;
                else if (repeat && wasSuspended) ++stats_.suspendedKeeps;
                if (repeat && (!exact || wasSuspended)) { repeat = false; result.fallback = true; }
                if (exact) {
                    roughRun_ = 0;
                    if (exactRun_ < 1000u) ++exactRun_;
                    if (suspended_ && exactRun_ >= config_.resumeAfter) { suspended_ = false; ++stats_.resumes; }
                } else {
                    exactRun_ = 0;
                    if (roughRun_ < 1000u) ++roughRun_;
                    if (!suspended_ && roughRun_ >= config_.suspendAfter) { suspended_ = true; ++stats_.suspensions; }
                }
            } else if (repeat) { repeat = false; result.fallback = true; }
        }
        if (result.fallback) ++stats_.fallbacks;
        result.repeat = repeat;
        result.locked = locked_;
        result.confidence = std::clamp(confidence, 0.0f, 1.0f);
        if (repeat) { ++stats_.repeats; result.picture = picture_ ? picture_ : ++picture_; }
        else result.picture = ++picture_;
        result.sourceFps = rate(input.pts100ns, !repeat);
        return result;
    }

    // Forget everything but the picture numbers and the counters. A boundary (observe) forgets
    // less: what this stream's repeats look like (the learned level) and a hold after a
    // mixed-cadence unlock outlive a lost picture or a decoder reset.
    void reset() { restart(); forgetStream(); }
    bool locked() const { return locked_; }
    // exactOnly: dropping is suspended (not proven yet since the last boundary, or something moved in
    // the repeat's place lately). Always false without exactOnly.
    bool suspended() const { return config_.exactOnly && suspended_; }
    StreamCadenceState state() const { return state_; }
    float sourceFps() const { return sourceFps_; }
    float repeatLevel() const { return levelKnown_ ? repeatLevel_ : 0.0f; }
    const StreamCadenceConfig& config() const { return config_; }
    const Stats& stats() const { return stats_; }

private:
    static constexpr unsigned kWindow = 64;   // pictures the source rate is measured over
    static constexpr unsigned kMeanRun = 3;   // repeats in a row the mean alone may vouch for
    static constexpr unsigned kBarren = 12;   // pictures a trusted rhythm may go without finding a repeat
    static constexpr float kLevelGain = 0.125f; // weight of one confirmed repeat in the learned level
    static constexpr unsigned kMaxHold = 900; // the longest hold after a mixed-cadence unlock (15 s)
    std::uint32_t windowMask() const {
        const unsigned w = std::clamp(config_.mixedWindow, 1u, 32u);
        return w >= 32u ? 0xFFFFFFFFu : ((1u << w) - 1u);
    }

    void restart() {
        suspended_ = true; exactRun_ = 0; roughRun_ = 0;   // exactOnly: proven again after every boundary
        havePrevious_ = false; previous_ = 0.0f; previousMean_ = -1.0f; previousPixels_ = -1.0f; previousCoherent_ = -1.0f; previousUp_ = false; afterCut_ = false;
        run_ = 0; haveStep_ = false; lastDown_ = false; runLow_ = 0.0f; fastRun_ = false;
        locked_ = false; misses_ = 0; missWithin_ = false; trust_ = 0.0f; ceilingRun_ = 0; ceilingMax_ = 0.0f; meanRun_ = 0; sinceRepeat_ = 0;
        levelKnown_ = false; repeatLevel_ = 0.0f; phaseKnown_ = false; phase_ = 0; phaseUntil_ = 0;
        newLevel_ = 0.0f; meanLevel_ = -1.0f; newSamples_ = 0; unknown_ = 0; bigFlat_ = 0; vetoes_ = 0;
        state_ = StreamCadenceState::Unknown;
        windowCount_ = 0; windowHead_ = 0; lastPts_ = 0; sourceFps_ = 0.0f;
    }

    void forgetStream() {
        holdUntil_ = 0; hold_ = std::max(config_.mixedHold, 1u);
        floorCount_ = 0; levelTouched_ = levelCoherent_ = levelAbs_ = levelPixel_ = 0.0f;
    }

    void unlock(bool still) {
        locked_ = false; run_ = 0; haveStep_ = false; fastRun_ = false; misses_ = 0; trust_ = 0.0f; ceilingRun_ = 0; bigFlat_ = 0;
        ++stats_.unlocks;
        if (still) {
            // A paused game keeps its place in the output: remember which pictures were the repeats.
            state_ = StreamCadenceState::Still;
            phaseUntil_ = index_ + config_.phaseMemory;
        } else {
            state_ = StreamCadenceState::Unique; phaseKnown_ = false;
        }
    }

    // Unique pictures per second over the last kWindow pictures.
    float rate(std::int64_t pts, bool isNew) {
        std::int64_t step = config_.nominalPeriod100ns > 0 ? config_.nominalPeriod100ns : 166833;
        if (pts > 0 && lastPts_ > 0 && pts > lastPts_ && pts - lastPts_ < 10000000) step = pts - lastPts_;
        lastPts_ = pts;
        windowStep_[windowHead_] = step; windowNew_[windowHead_] = isNew;
        windowHead_ = (windowHead_ + 1) % kWindow;
        windowCount_ = std::min(windowCount_ + 1, kWindow);
        if (windowCount_ < kWindow / 4) { sourceFps_ = 0.0f; return sourceFps_; }
        // The oldest picture only marks where the span starts.
        std::int64_t span = 0; unsigned unique = 0;
        for (unsigned i = 1; i < windowCount_; ++i) {
            const unsigned at = (windowHead_ + kWindow - windowCount_ + i) % kWindow;
            span += windowStep_[at];
            if (windowNew_[at]) ++unique;
        }
        sourceFps_ = span > 0 ? static_cast<float>(static_cast<double>(unique) * 1.0e7 / static_cast<double>(span)) : 0.0f;
        return sourceFps_;
    }

    StreamCadenceConfig config_;
    Stats stats_;
    std::uint64_t picture_ = 0, index_ = 0, phaseUntil_ = 0, phase_ = 0, holdUntil_ = 0;
    std::uint32_t vetoes_ = 0;                       // one bit per recent picture: a repeat's place that turned out new
    unsigned hold_ = 120, floorCount_ = 0;
    float previousCoherent_ = -1.0f;
    float levelTouched_ = 0.0f, levelCoherent_ = 0.0f, levelAbs_ = 0.0f, levelPixel_ = 0.0f;
    std::int64_t lastPts_ = 0;
    std::array<std::int64_t, kWindow> windowStep_{};
    std::array<bool, kWindow> windowNew_{};
    float previous_ = 0.0f, previousMean_ = -1.0f, runLow_ = 0.0f, repeatLevel_ = 0.0f, newLevel_ = 0.0f;
    float trust_ = 0.0f, ceilingMax_ = 0.0f, sourceFps_ = 0.0f, meanLevel_ = -1.0f, previousPixels_ = -1.0f;
    unsigned run_ = 0, misses_ = 0, ceilingRun_ = 0, meanRun_ = 0, sinceRepeat_ = 0, newSamples_ = 0, unknown_ = 0, bigFlat_ = 0;
    unsigned windowCount_ = 0, windowHead_ = 0;
    bool havePrevious_ = false, haveStep_ = false, lastDown_ = false, locked_ = false, previousUp_ = false, afterCut_ = false;
    bool fastRun_ = false, phaseKnown_ = false, levelKnown_ = false, missWithin_ = false;
    bool suspended_ = true;                          // exactOnly: no drop until exact repeats were seen
    unsigned exactRun_ = 0, roughRun_ = 0;
    StreamCadenceState state_ = StreamCadenceState::Unknown;
};

} // namespace veyra::remoteplay
