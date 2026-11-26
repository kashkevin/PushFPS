#!/usr/bin/env bash
# Re-copies the vendored references from the source branches (read-only) and rewrites SOURCES.txt's commit ids.
set -euo pipefail
S=${PUSHFPS_SRC:-/home/claude/src/PushFPS}; V=$(cd "$(dirname "$0")" && pwd)
P=$(git -C $S rev-parse exp/s12-pace); H=$(git -C $S rev-parse exp/s12-hud); O=$(git -C $S rev-parse exp/s11-ownfg)
for f in PushDownscale PushFrameProbe PushOnScreenCadence; do git -C $S show $P:include/veyra/engine/$f.h > $V/$f.h; done
git -C $S show $H:include/veyra/pipeline/PushHudGuard.h > $V/PushHudGuard.h
git -C $S show $O:include/veyra/pipeline/PushWarp.h > $V/PushWarp.h
git -C $S show $O:tools/pushwarp_eval/pushwarp_cli.cpp > $V/pushwarp_cli.cpp
git -C $S show $O:tools/pushwarp_eval/ffx_check_sim.py > $V/ffx_check_sim.py
git -C $S show $O:tools/pushwarp_eval/FlowModel.h > $V/FlowModel.h
git -C $S show $O:tools/pushwarp_eval/README.md > $V/pushwarp_eval_README.md
sed -i -E "s/^exp\/s12-pace .*/exp\/s12-pace $P/; s/^exp\/s12-hud .*/exp\/s12-hud $H/; s/^exp\/s11-ownfg .*/exp\/s11-ownfg $O/; s/^Copied .*/Copied $(date -u +%F)./" $V/SOURCES.txt
echo "re-taken: s12-pace ${P:0:8} s12-hud ${H:0:8} s11-ownfg ${O:0:8}"
