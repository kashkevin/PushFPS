#!/usr/bin/env bash
# Long-run soak of the portable PS5 Remote Play pieces (soak_pipeline.cpp) in three builds.
#
#   tools/remoteplay_latency_bench/run_soak.sh [--minutes M] [--seeds "1 2"] [--configs "plain tsan asan"] [--speed K] [--parallel N]
#
# Each configuration and seed runs for M minutes of wall time (default 5) at K times real time
# (default 20 plain, 4 under ThreadSanitizer, 8 under AddressSanitizer). At most --parallel runs
# at once (default 1; the build machines are shared). Logs and one JSON line per run go to
# $SOAK_DIR (default /tmp/pushfps-soak). Exit code 1 when any run failed an invariant or a
# sanitizer reported something. A Linux model: it says nothing about Windows, the GPU or a PS5.
set -euo pipefail
root="$(git rev-parse --show-toplevel)"; cd "$root"
minutes=5; seeds="1"; configs="plain tsan asan"; speed=""; parallel=1
while [ $# -gt 0 ]; do case "$1" in
  --minutes) minutes="$2"; shift 2;;
  --seeds) seeds="$2"; shift 2;;
  --configs) configs="$2"; shift 2;;
  --speed) speed="$2"; shift 2;;
  --parallel) parallel="$2"; shift 2;;
  *) echo "unknown argument $1" >&2; exit 2;; esac; done
out="${SOAK_DIR:-${TMPDIR:-/tmp}/pushfps-soak}"; mkdir -p "$out"
core=(src/remoteplay/Validation.cpp src/remoteplay/AnnexB.cpp src/remoteplay/Timeline.cpp src/remoteplay/VideoIngress.cpp src/remoteplay/SessionInbox.cpp)
build() { # config
  case "$1" in
    plain) flags=(-O2 -g);;
    tsan) flags=(-O1 -g -fsanitize=thread);;
    asan) flags=(-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer);;
    *) echo "unknown configuration $1" >&2; exit 2;;
  esac
  nice -n 10 g++ -std=c++20 -pthread -Iinclude "${flags[@]}" "${core[@]}" tools/remoteplay_latency_bench/soak_pipeline.cpp -o "$out/soak_$1"
}
for c in $configs; do echo "build $c"; build "$c"; done
status=0
run() { # config seed
  local c="$1" s="$2" k="$speed"
  [ -z "$k" ] && case "$c" in plain) k=20;; tsan) k=4;; asan) k=8;; esac
  local log="$out/soak_${c}_seed${s}.log"
  TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1" ASAN_OPTIONS="detect_leaks=1" \
    nice -n 10 "$out/soak_$c" --seed "$s" --speed "$k" --sim-hours 1000 --wall-minutes "$minutes" >"$log" 2>&1 || true
  if grep -qE "^FAIL|WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:" "$log" || ! grep -q '"violations":0' "$log"; then
    echo "FAIL $c seed $s (see $log)"; grep -E "^FAIL|WARNING: ThreadSanitizer|ERROR:|runtime error:|SUMMARY" "$log" | head -10
    return 1
  fi
  echo "ok   $c seed $s: $(grep -o '"simHours":[0-9.]*,"wallSeconds":[0-9.]*' "$log")"
}
jobs=()
for c in $configs; do for s in $seeds; do
  run "$c" "$s" & jobs+=($!)
  if [ "${#jobs[@]}" -ge "$parallel" ]; then wait "${jobs[0]}" || status=1; jobs=("${jobs[@]:1}"); fi
done; done
for j in "${jobs[@]}"; do wait "$j" || status=1; done
cat "$out"/soak_*_seed*.log | grep '^{"bench":"soak_pipeline"' > "$out/results.jsonl" || true
echo "results: $out/results.jsonl"
exit $status
