#!/usr/bin/env bash
# Re-copies the vendored references (read-only) from the streams' PUSHED branches and rewrites SOURCES.txt's commit ids.
# Run from any checkout that has fetched them: PUSHFPS_SRC=<repo> [PACE_REF=..] [HUD_REF=..] [OWNFG_REF=..] retake.sh
set -euo pipefail
S=${PUSHFPS_SRC:-$(git rev-parse --show-toplevel)}; V=$(cd "$(dirname "$0")" && pwd)
PACE_REF=${PACE_REF:-origin/ck/s12-pace}; HUD_REF=${HUD_REF:-origin/ck/s12-hud}; OWNFG_REF=${OWNFG_REF:-origin/ck/ownfg}
P=$(git -C $S rev-parse $PACE_REF); H=$(git -C $S rev-parse $HUD_REF); O=$(git -C $S rev-parse $OWNFG_REF)
get() { git -C $S show $1:$2 | tr -d '\r' > $V/$3; }
for f in PushDownscale PushFrameProbe PushOnScreenCadence; do get $P include/veyra/engine/$f.h $f.h; done
get $H include/veyra/pipeline/PushHudGuard.h PushHudGuard.h
get $O include/veyra/pipeline/PushWarp.h PushWarp.h
get $O tools/pushwarp_eval/pushwarp_cli.cpp pushwarp_cli.cpp
get $O tools/pushwarp_eval/ffx_check_sim.py ffx_check_sim.py
get $O tools/pushwarp_eval/FlowModel.h FlowModel.h
get $O tools/pushwarp_eval/README.md pushwarp_eval_README.md
sed -i -E "s#^pace .*#pace $PACE_REF $P#; s#^hud .*#hud $HUD_REF $H#; s#^ownfg .*#ownfg $OWNFG_REF $O#; s#^Copied .*#Copied $(date -u +%F).#" $V/SOURCES.txt
echo "re-taken: pace ${P:0:8} hud ${H:0:8} ownfg ${O:0:8}"
