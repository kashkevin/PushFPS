// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the engine's numbering of hardware-decoded pictures over the files written by
// cadence_streams.py: veyra::engine::PushStreamDedup (which asks veyra::remoteplay::StreamCadence)
// and then veyra::engine::ContentDedup, exactly as EngineController does, and counts against the
// known cadence:
//   dropped new: a new game picture that ContentDedup dropped                      -- the one that matters
//     of those, "identical": no luma sample of the decoded picture differs from the picture before
//     (the encoder sent nothing, or the generator moved nothing); nothing that could be seen was lost
//   false repeat: a new picture the classifier called a repeat (the same thing seen one step earlier;
//     it is dropped unless the frame before it had no number)
//   missed repeat: a repeat that was kept (costs nothing visible)
//
//   cadence_eval [options] <file.txt>...
//     --max-only        give the classifier the largest block difference only (no mean, share or pixels)
//     --no-pixels       leave out the per-sample evidence (what the first version of the pass measured)
//     --pixel-step N    which pixel count to use: samples that differ by more than N (0 1 2 4 8 16 32 64);
//                       default veyra::pipeline::kStreamChangePixelStep, what the GPU pass counts
//     --cuts            tell the classifier where the scene cuts are (column 6); default: it must find them
//     --set NAME=VALUE  override one StreamCadenceConfig value; for tuning only
//     --detail          one line per dropped new picture and per false repeat on stderr
// One JSON line per stream and a total on stdout. Exit status 1 when a new picture was dropped.
#include "veyra/engine/ContentDedup.h"
#include "veyra/engine/PushStreamDedup.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
bool setValue(veyra::remoteplay::StreamCadenceConfig& c, const std::string& name, double v) {
    const auto f = static_cast<float>(v); const auto u = static_cast<unsigned>(v);
    if (name == "ratio") c.ratio = f; else if (name == "identical") c.identical = f; else if (name == "ceiling") c.ceiling = f;
    else if (name == "meanVeto") c.meanVeto = f; else if (name == "meanFloor") c.meanFloor = f; else if (name == "meanRatio") c.meanRatio = f;
    else if (name == "shareVeto") c.shareVeto = f; else if (name == "cutFactor") c.cutFactor = f; else if (name == "marginGain") c.marginGain = f;
    else if (name == "confidenceFloor") c.confidenceFloor = f; else if (name == "lockSteps") c.lockSteps = u;
    else if (name == "fastLockSteps") c.fastLockSteps = u; else if (name == "unlockMisses") c.unlockMisses = u;
    else if (name == "rhythmFloor") c.rhythmFloor = f; else if (name == "pixelCeiling") c.pixelCeiling = f; else if (name == "pixelRatio") c.pixelRatio = f;
    else return false;
    return true;
}
constexpr int kSteps[] = {0, 1, 2, 4, 8, 16, 32, 64};
struct Row { long long bytes = 0; int isNew = 1; float maxBlock = 0, meanBlock = 0, share = 0; int cut = 0; float pixelMax = -1, madMax = -1, madMean = -1; double over[8] = {-1, -1, -1, -1, -1, -1, -1, -1}; };
}

int main(int argc, char** argv) {
    veyra::remoteplay::StreamCadenceConfig config;
    bool maxOnly = false, noPixels = false, cuts = false, detail = false;
    int pixelStep = int(veyra::pipeline::kStreamChangePixelStep);
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--max-only") maxOnly = true;
        else if (a == "--no-pixels") noPixels = true;
        else if (a == "--cuts") cuts = true;
        else if (a == "--detail") detail = true;
        else if (a == "--pixel-step" && i + 1 < argc) pixelStep = std::atoi(argv[++i]);
        else if (a == "--set" && i + 1 < argc) {
            const std::string kv = argv[++i]; const auto eq = kv.find('=');
            if (eq == std::string::npos || !setValue(config, kv.substr(0, eq), std::atof(kv.c_str() + eq + 1))) { std::fprintf(stderr, "bad --set %s\n", kv.c_str()); return 2; }
        } else files.push_back(a);
    }
    int stepColumn = -1;
    for (int k = 0; k < 8; ++k) if (kSteps[k] == pixelStep) stepColumn = k;
    if (stepColumn < 0) { std::fprintf(stderr, "bad --pixel-step %d\n", pixelStep); return 2; }
    unsigned long long falseRepeat = 0, droppedNew = 0, droppedIdentical = 0, missedRepeat = 0, news = 0, repeats = 0, newsLocked = 0, fallbacks = 0, pixelVetoes = 0;
    double worstBlock = 0, worstPixel = 0, closest = 1e9;
    for (const auto& path : files) {
        std::ifstream in(path);
        if (!in) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); return 2; }
        std::vector<Row> rows; std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line); Row r;
            if (!(fields >> r.bytes >> r.isNew >> r.maxBlock >> r.meanBlock >> r.share)) continue;
            // The cut flag is absent in files of the first study, the per-sample columns in files made before the review.
            if (fields >> r.cut) { if (fields >> r.pixelMax >> r.madMax >> r.madMean) for (double& v : r.over) fields >> v; }
            rows.push_back(r);
        }
        veyra::engine::PushStreamDedup dedup(config);
        veyra::engine::ContentDedup content;
        unsigned long long fileFalse = 0, fileDropped = 0, fileIdentical = 0, fileMissed = 0, fileNew = 0, fileRepeat = 0, fileNewLocked = 0, lockedAt = 0, fileFallback = 0;
        double fileWorstBlock = 0, fileWorstPixel = 0, fileClosest = 1e9, rateSum = 0; unsigned long long rateCount = 0;
        float previous = 0;
        std::string name = path;
        const auto slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const Row& row = rows[index];
            veyra::pipeline::StreamChangeOutcome outcome;
            outcome.now.valid = true; outcome.now.comparable = index != 0;
            outcome.now.maxBlock = row.maxBlock;
            outcome.now.meanBlock = maxOnly ? -1.0f : row.meanBlock; outcome.now.changedShare = maxOnly ? -1.0f : row.share;
            const bool pixels = !maxOnly && !noPixels && row.pixelMax >= 0 && row.over[stepColumn] >= 0;
            outcome.now.pixelMax = pixels ? row.pixelMax : -1.0f; outcome.now.pixelsOver = pixels ? static_cast<float>(row.over[stepColumn]) : -1.0f;
            const std::int64_t pts = 1000000 + static_cast<std::int64_t>(index) * 166833;
            const auto d = dedup.observe(outcome, index + 1, pts, index == 0, cuts && row.cut != 0);
            const auto& r = d.cadence;
            const auto kept = content.observe(d.numbered ? &d.picture : nullptr, pts, 166833, index == 0, 0, 0);
            if (r.locked && !lockedAt) lockedAt = index;
            if (r.fallback) ++fileFallback;
            if (row.isNew) {
                ++fileNew;
                if (r.locked) {
                    ++fileNewLocked;
                    // How close a new picture came to the fall a repeat shows (1 = at the limit).
                    if (index && previous > 0) fileClosest = std::min(fileClosest, double(row.maxBlock) / (double(config.ratio) * double(previous)));
                }
                if (r.repeat) ++fileFalse;
                if (!kept.keep) {
                    ++fileDropped; fileWorstBlock = std::max(fileWorstBlock, double(row.maxBlock)); fileWorstPixel = std::max(fileWorstPixel, double(row.pixelMax));
                    if (row.pixelMax == 0.0f) ++fileIdentical;
                }
                if (detail && (r.repeat || !kept.keep)) std::fprintf(stderr, "%s %s picture %zu change %.3f mean %.4f share %.4f pixelMax %.0f pixelsOver %.0f before %.3f confidence %.2f level %.3f\n",
                    kept.keep ? "FALSE REPEAT (kept)" : "DROPPED NEW", name.c_str(), index, row.maxBlock, row.meanBlock, row.share, row.pixelMax, row.over[stepColumn], previous, r.confidence, dedup.cadence().repeatLevel());
            } else { ++fileRepeat; if (kept.keep) ++fileMissed; }
            if (index >= rows.size() / 2 && r.sourceFps > 0) { rateSum += r.sourceFps; ++rateCount; }
            previous = row.maxBlock;
        }
        const auto& stats = dedup.cadence().stats();
        std::printf("{\"stream\":\"%s\",\"pictures\":%zu,\"new\":%llu,\"repeat\":%llu,\"droppedNew\":%llu,\"droppedNewIdentical\":%llu,\"falseRepeat\":%llu,\"missedRepeat\":%llu,\"newWhileLocked\":%llu,"
                    "\"worstDroppedChange\":%.3f,\"worstDroppedPixel\":%.0f,\"closestNewToLimit\":%.3f,\"fallbacks\":%llu,\"pixelVetoes\":%llu,\"firstLockedAt\":%llu,\"locks\":%llu,\"fastLocks\":%llu,\"unlocks\":%llu,\"cutsFound\":%llu,\"sourceFpsSecondHalf\":%.2f}\n",
            name.c_str(), rows.size(), fileNew, fileRepeat, fileDropped, fileIdentical, fileFalse, fileMissed, fileNewLocked, fileWorstBlock, fileWorstPixel, fileClosest > 1e8 ? 0.0 : fileClosest, fileFallback,
            static_cast<unsigned long long>(stats.pixelVetoes), lockedAt, static_cast<unsigned long long>(stats.locks), static_cast<unsigned long long>(stats.fastLocks),
            static_cast<unsigned long long>(stats.unlocks), static_cast<unsigned long long>(stats.cuts), rateCount ? rateSum / double(rateCount) : 0.0);
        falseRepeat += fileFalse; droppedNew += fileDropped; droppedIdentical += fileIdentical; missedRepeat += fileMissed; news += fileNew; repeats += fileRepeat; newsLocked += fileNewLocked;
        fallbacks += fileFallback; pixelVetoes += stats.pixelVetoes;
        worstBlock = std::max(worstBlock, fileWorstBlock); worstPixel = std::max(worstPixel, fileWorstPixel); closest = std::min(closest, fileClosest);
    }
    std::printf("{\"total\":true,\"streams\":%zu,\"new\":%llu,\"repeat\":%llu,\"droppedNew\":%llu,\"droppedNewIdentical\":%llu,\"falseRepeat\":%llu,\"missedRepeat\":%llu,\"newWhileLocked\":%llu,\"worstDroppedChange\":%.3f,\"worstDroppedPixel\":%.0f,\"closestNewToLimit\":%.3f,\"fallbacks\":%llu,\"pixelVetoes\":%llu}\n",
        files.size(), news, repeats, droppedNew, droppedIdentical, falseRepeat, missedRepeat, newsLocked, worstBlock, worstPixel, closest > 1e8 ? 0.0 : closest, fallbacks, pixelVetoes);
    return droppedNew ? 1 : 0;
}
