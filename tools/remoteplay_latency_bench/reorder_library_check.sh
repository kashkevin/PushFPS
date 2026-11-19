#!/usr/bin/env bash
# Does include/veyra/remoteplay/ReorderQueueModel.h decide like the library?
#   tools/remoteplay_latency_bench/reorder_library_check.sh <chiaki-ng tree with patches 0001-0004 applied> [traces]
# Builds the library's own reorder queue code (lib/src/takion.c, reorderqueue.c) into a
# virtual-time harness, replays generated arrival traces through it and through the model,
# and compares every released packet, its time and patch 0004's counters.
# Linux only; a check of the model against the C source, not of anything on Windows or a PS5.
set -euo pipefail
stage="${1:?path to the patched chiaki-ng tree}"; traces="${2:-40}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${TMPDIR:-/tmp}/reorder-library-check"; mkdir -p "$out"
grep -q CHIAKI_VEYRA_TRANSPORT_LIVE_REORDER_API "$stage/lib/include/chiaki/takion.h" || { echo "FAIL: $stage does not have patch 0004 with the live reorder wait"; exit 1; }
gcc -std=c11 -O1 -w -I"$stage" -I"$stage/lib/include" "$root/tools/remoteplay_latency_bench/reorder_library_check.c" "$stage/lib/src/reorderqueue.c" \
  -Wl,--unresolved-symbols=ignore-all -o "$out/library"
g++ -std=c++20 -O2 -Wall -Wextra -Werror -I"$root/include" "$root/tools/remoteplay_latency_bench/reorder_replay.cpp" -o "$out/replay"
released=0
for seed in $(seq 1 "$traces"); do
  "$out/replay" --generate "$seed" > "$out/trace.txt"
  "$out/library" < "$out/trace.txt" > "$out/library.txt"
  "$out/replay" --model < "$out/trace.txt" > "$out/model.txt"
  if ! cmp -s "$out/library.txt" "$out/model.txt"; then
    echo "FAIL: trace $seed differs (library | model):"; diff "$out/library.txt" "$out/model.txt" | head -10; exit 1
  fi
  released=$((released + $(grep -c '^R' "$out/library.txt")))
  tail -1 "$out/library.txt" > "$out/last-counters.txt"
done
echo "reorder model = library on $traces traces, $released released packets compared; last: $(cut -c1-160 "$out/last-counters.txt")"
