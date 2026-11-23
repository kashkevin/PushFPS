#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#
# The restoration bench (docs/RESTORATION.md), repeatable on Linux:
#   1. gen_content.py draws pristine 1920x1080 frames (MODEL content, not game footage);
#   2. ffmpeg encodes them like a low-latency stream (no B-frames, zerolatency, a tight buffer, 60 fps,
#      8-bit 4:2:0 limited-range BT.709) with x264 and, when this ffmpeg has it, x265, at a ladder of
#      bitrates, and decodes them again (SOFTWARE encoders: not the PS5's hardware encoder);
#   3. restore_bench runs the CPU REFERENCE of the stage (include/veyra/engine/PushClean.h: not a GPU,
#      not the shader) on the LAST frames of each stream (the first ones are spent on the key frame) and
#      measures every setting against the pristine picture; "off" is the picture as decoded;
#   4. summarize.py turns results.csv into results.md.
# usage: tools/restore_bench/run_bench.sh [--out DIR] [--frames N] [--measure M] [--bitrates "4 8 15"]
#                                         [--phase all|prepare|measure]
#        N frames encoded (default 30), the last M measured (default 4). --phase prepare stops after the
#        decoded frames are on disk, --phase measure starts from them. Needs g++, python3 with numpy and
#        PIL, ffmpeg with libx264. One thread; about 10 minutes of CPU for the defaults.
#        RESTORE_BENCH_WRAPPER (optional): a command put in front of the g++ call (a shared build slot).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; root="$(cd "$here/../.." && pwd)"
out="${TMPDIR:-/tmp}/restore-bench"; frames=30; measure=4; bitrates="4 8 15"; phase=all
ffmpeg="${FFMPEG:-$(command -v ffmpeg || echo /usr/local/bin/ffmpeg)}"
while [ $# -gt 0 ]; do case "$1" in
  --out) out="$2"; shift 2;;
  --frames) frames="$2"; shift 2;;
  --measure) measure="$2"; shift 2;;
  --bitrates) bitrates="$2"; shift 2;;
  --phase) phase="$2"; shift 2;;
  *) echo "unknown argument $1" >&2; exit 2;; esac; done
[ "$measure" -le "$frames" ] || { echo "--measure cannot exceed --frames" >&2; exit 2; }
case "$phase" in all|prepare|measure) ;; *) echo "--phase is all, prepare or measure" >&2; exit 2;; esac
mkdir -p "$out"
encoders="libx264"
list="$("$ffmpeg" -hide_banner -encoders 2>/dev/null || true)"
case "$list" in *libx265*) encoders="$encoders libx265";; *) echo "note: this ffmpeg has no libx265; x264 only";; esac
first=$((frames - measure))
if [ "$phase" != measure ]; then
  echo "== content ($frames frames)"
  python3 -B "$here/gen_content.py" --out "$out/content" --frames "$frames"
  cat "$out"/content/src_*.rgb24 > "$out/source.rgb"
  rm -f "$out"/content/src_*.rgb24
  for n in $(seq 0 $((first - 1))); do rm -f "$out/content/ref_$(printf %03d "$n").rgb48"; done   # only the measured frames are compared
  common=(-hide_banner -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 1920x1080 -r 60 -i "$out/source.rgb"
          -vf "scale=out_color_matrix=bt709:out_range=tv:flags=accurate_rnd+full_chroma_int" -pix_fmt yuv420p
          -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -threads 1)
  : > "$out/streams.txt"
  for encoder in $encoders; do
    for mbps in $bitrates; do
      name="${encoder#lib}-${mbps}M"
      echo "== $name: encode and decode"
      kbps=$((mbps * 1000)); buffer=$((kbps / 15))   # about four frames of buffer at 60 fps
      if [ "$encoder" = libx264 ]; then
        "$ffmpeg" "${common[@]}" -c:v libx264 -preset veryfast -tune zerolatency -bf 0 -g 600 \
          -b:v "${kbps}k" -maxrate "${kbps}k" -bufsize "${buffer}k" "$out/$name.mkv"
      else
        "$ffmpeg" "${common[@]}" -c:v libx265 -preset veryfast -tune zerolatency \
          -x265-params "bframes=0:keyint=600:log-level=error:vbv-maxrate=${kbps}:vbv-bufsize=${buffer}" -b:v "${kbps}k" "$out/$name.mkv"
      fi
      rm -rf "$out/$name"; mkdir -p "$out/$name"
      "$ffmpeg" -hide_banner -loglevel error -y -i "$out/$name.mkv" \
        -vf "scale=in_color_matrix=bt709:in_range=tv:flags=accurate_rnd+full_chroma_int" -pix_fmt rgb24 \
        -start_number 0 -f image2 -c:v rawvideo "$out/$name/dec_%03d.rgb24"
      for n in $(seq 0 $((first - 1))); do rm -f "$out/$name/dec_$(printf %03d "$n").rgb24"; done
      bytes=$(stat -c %s "$out/$name.mkv")
      echo "$name $bytes $frames $((bytes * 8 * 60 / frames / 1000))" >> "$out/streams.txt"
      echo "   file $bytes bytes for $frames frames ($((bytes * 8 * 60 / frames / 1000)) kbit/s average)"
    done
  done
  rm -f "$out/source.rgb"
fi
[ "$phase" = prepare ] && { echo "== prepared in $out"; exit 0; }
echo "== build restore_bench (g++ -O2)"
${RESTORE_BENCH_WRAPPER:-} g++ -std=c++20 -O2 -Wall -Wextra -Wshadow -I"$root/include" "$here/restore_bench.cpp" -o "$out/restore_bench"
: > "$out/results.csv"
while read -r name bytes total kbit; do
  echo "== $name: measure ($kbit kbit/s average over $total frames)"
  "$out/restore_bench" --content "$out/content" --decoded "$out/$name/dec_" --first "$first" --frames "$measure" \
    --label "$name" --csv "$out/results.csv"
done < "$out/streams.txt"
python3 -B "$here/summarize.py" "$out/results.csv" --full > "$out/results.md"
echo "== results: $out/results.csv and $out/results.md"
