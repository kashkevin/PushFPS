#!/usr/bin/env bash
# Builds and runs the portable PS5 Remote Play latency benchmarks (Linux or any
# C++20 toolchain; no Windows, GPU, FFmpeg or PS5 needed).
#   tools/remoteplay_latency_bench/run.sh [original-source-tree]
# With a second checkout of the commit before this work as the argument, the
# "poll" baseline of the hand-off benchmark is built from those sources.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${TMPDIR:-/tmp}/remoteplay-latency-bench"; mkdir -p "$out"
core="src/remoteplay/Validation.cpp src/remoteplay/AnnexB.cpp src/remoteplay/Timeline.cpp src/remoteplay/VideoIngress.cpp src/remoteplay/SessionInbox.cpp"
cxx="${CXX:-g++} -std=c++20 -O2 -pthread"
cd "$root"
$cxx -Iinclude $core tests/remoteplay/CoreTests.cpp -o "$out/core_tests"
"$out/core_tests" | tail -1
$cxx -DREMOTEPLAY_BENCH_HAVE_WAKE -Iinclude $core tools/remoteplay_latency_bench/handoff_bench.cpp -o "$out/handoff"
$cxx -Iinclude tools/remoteplay_latency_bench/input_sim.cpp -o "$out/input_sim"
baseline="$out/handoff"
if [ "${1:-}" != "" ]; then
  (cd "$1" && $cxx -Iinclude $core "$root/tools/remoteplay_latency_bench/handoff_bench.cpp" -o "$out/handoff_original")
  baseline="$out/handoff_original"
fi
echo "# frame hand-off: inbox -> decoder owner (measured on this machine)"
"$baseline" --mode poll --frames 1500
"$out/handoff" --mode wake --frames 1500
echo "# input dispatch: pad state readable -> sent to Chiaki (model, virtual time)"
"$out/input_sim" --mode original --owner-sleep-ms 1.5
"$out/input_sim" --mode new
if python3 -c "import av, numpy" 2>/dev/null; then
  echo "# decoder output delay with console-like headers (libavcodec via PyAV)"
  python3 -I tools/remoteplay_latency_bench/decoder_delay.py 2>/dev/null | grep -E "^(h264|hevc) "
fi
