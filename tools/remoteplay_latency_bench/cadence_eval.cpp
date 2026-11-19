// SPDX-License-Identifier: GPL-3.0-or-later
// Runs veyra::remoteplay::StreamCadence over the files written by cadence_streams.py and counts
// its two kinds of error against the known cadence:
//   false repeat: a new picture called a repeat (a game frame would be dropped)  -- must be 0
//   missed repeat: a repeat called new (the frame is kept; costs nothing visible)
//   cadence_eval <file.txt>...
#include "veyra/remoteplay/StreamCadence.h"
#include <cstdio>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    unsigned long long falseRepeat = 0, missedRepeat = 0, news = 0, repeats = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i]);
        if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[i]); return 2; }
        veyra::remoteplay::StreamCadence cadence;
        unsigned long long fileFalse = 0, fileMissed = 0, fileNew = 0, fileRepeat = 0, index = 0, lockedAt = 0;
        long long bytes; int isNew; float maxBlock, meanBlock, share;
        while (in >> bytes >> isNew >> maxBlock >> meanBlock >> share) {
            const auto r = cadence.observe(maxBlock, index == 0);
            if (r.locked && !lockedAt) lockedAt = index;
            if (isNew) { ++fileNew; if (r.repeat) ++fileFalse; }
            else { ++fileRepeat; if (!r.repeat) ++fileMissed; }
            ++index;
        }
        std::string name = argv[i];
        const auto slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        std::printf("{\"stream\":\"%s\",\"pictures\":%llu,\"new\":%llu,\"repeat\":%llu,\"falseRepeat\":%llu,\"missedRepeat\":%llu,\"firstLockedAt\":%llu,\"locks\":%llu,\"unlocks\":%llu}\n",
            name.c_str(), index, fileNew, fileRepeat, fileFalse, fileMissed, lockedAt,
            static_cast<unsigned long long>(cadence.stats().locks), static_cast<unsigned long long>(cadence.stats().unlocks));
        falseRepeat += fileFalse; missedRepeat += fileMissed; news += fileNew; repeats += fileRepeat;
    }
    std::printf("{\"total\":true,\"new\":%llu,\"repeat\":%llu,\"falseRepeat\":%llu,\"missedRepeat\":%llu}\n", news, repeats, falseRepeat, missedRepeat);
    return falseRepeat ? 1 : 0;
}
