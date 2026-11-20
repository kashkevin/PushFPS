#!/usr/bin/env bash
# Does include/veyra/remoteplay/ReorderQueueModel.h decide like the library?
#   tools/remoteplay_latency_bench/reorder_library_check.sh <chiaki-ng tree with patches 0001-0004 applied> [traces] [study seeds]
# Builds the library's own reorder queue code (lib/src/takion.c, reorderqueue.c) into a
# virtual-time harness, replays generated arrival traces through it and through the model,
# and compares every released packet, its time and patch 0004's counters, the per-frame
# late-packet evidence (LATE_FRAMES_FAILED against LateFrameEvidenceModel) among them.
# Then the same on the study's conditions (ReorderStudy.h; the Wi-Fi-like ones and the two
# with heavy reordering), [study seeds] seeds of 60 s each (default 2).
# Linux only; a check of the model against the C source, not of anything on Windows or a PS5.
set -euo pipefail
stage="${1:?path to the patched chiaki-ng tree}"; traces="${2:-40}"; study_seeds="${3:-2}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${TMPDIR:-/tmp}/reorder-library-check"; mkdir -p "$out"
grep -q CHIAKI_VEYRA_TRANSPORT_LIVE_REORDER_API "$stage/lib/include/chiaki/takion.h" || { echo "FAIL: $stage does not have patch 0004 with the live reorder wait"; exit 1; }
grep -q CHIAKI_VEYRA_TRANSPORT_FRAME_EVIDENCE_API "$stage/lib/include/chiaki/takion.h" || { echo "FAIL: $stage does not have patch 0004 with the per-frame evidence"; exit 1; }
gcc -std=c11 -O1 -w -I"$stage" -I"$stage/lib/include" "$root/tools/remoteplay_latency_bench/reorder_library_check.c" "$stage/lib/src/reorderqueue.c" \
  -Wl,--unresolved-symbols=ignore-all -o "$out/library"
g++ -std=c++20 -O2 -Wall -Wextra -Werror -I"$root/include" "$root/tools/remoteplay_latency_bench/reorder_replay.cpp" -o "$out/replay"
released=0; frames_failed=0; late_frames=0; early_cuts=0; compared=0
compare() { # $1: label; the trace is in $out/trace.txt
  "$out/library" < "$out/trace.txt" > "$out/library.txt"
  "$out/replay" --model < "$out/trace.txt" > "$out/model.txt"
  if ! cmp -s "$out/library.txt" "$out/model.txt"; then
    echo "FAIL: $1 differs (library | model):"; diff "$out/library.txt" "$out/model.txt" | head -10; exit 1
  fi
  released=$((released + $(grep -c '^R' "$out/library.txt"))); compared=$((compared + 1))
  local last; last="$(tail -1 "$out/library.txt")"; echo "$last" > "$out/last-counters.txt"
  frames_failed=$((frames_failed + $(sed -n 's/.* fecFailed=\([0-9]*\).*/\1/p' <<<"$last")))
  late_frames=$((late_frames + $(sed -n 's/.* lateFramesFailed=\([0-9]*\).*/\1/p' <<<"$last")))
  early_cuts=$((early_cuts + $(sed -n 's/.* earlyCuts=\([0-9]*\).*/\1/p' <<<"$last")))
}
for seed in $(seq 1 "$traces"); do
  "$out/replay" --generate "$seed" > "$out/trace.txt"; compare "trace $seed"
done
for condition in ${REORDER_CHECK_CONDITIONS:-wifi_stall_clumps wifi_retransmission_reorder wifi_rate_dips wifi_mixed heavy_reordering congestion_with_reordering}; do
  for seed in $(seq 1 "$study_seeds"); do
    "$out/replay" --study "$condition" "$seed" 60 > "$out/trace.txt"; compare "$condition seed $seed"
  done
done
# The reviewers' cases: compared like the others, and their outcome checked.
outcome() { sed -n "s/.* $1=\([0-9]*\).*/\1/p" "$out/last-counters.txt"; }
for name in late_duplicate late_duplicate_once wrap wrap_fixed; do
  "$out/replay" --case "$name" > "$out/trace.txt"; compare "case $name"
  case "$name" in
    late_duplicate*) [ "$(outcome lateFramesFailed)" = 0 ] || { echo "FAIL: $name counted a frame that could not be completed"; exit 1; };;
    wrap) [ "$(outcome dropped)" = 1 ] || { echo "FAIL: wrap: the library's drop of packet 0 was not reproduced"; exit 1; };;
    wrap_fixed) [ "$(outcome dropped)" = 0 ] || { echo "FAIL: wrap_fixed: packet 0 still dropped"; exit 1; };;
  esac
done
echo "reorder model = library on $compared traces ($traces generated, $((compared - traces - 4)) study, 4 cases), $released released packets compared; frames failed $frames_failed, of them lost to late packets (LATE_FRAMES_FAILED) $late_frames; early loss cuts $early_cuts; reviewer cases late_duplicate, wrap, wrap_fixed as expected; last: $(cut -c1-200 "$out/last-counters.txt")"
