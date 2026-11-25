# GPU DIS subset used by Veyra

Source: https://github.com/gggz114514-oss/XeSS-GPU-Motion

Pinned Git commit: `cb7523b5104fc914dc501767c3139b43c2067af7`.
The upstream export labels itself R4.2 and records its pre-export snapshot separately as `ed27d62ad7ce17f67f4db936ed2fc5d48e6b0f87`.

Only the public D3D12 DIS provider, GPU frame contract and DIS shader closure
are included. No Intel SDK headers, runtime binaries, oneVPL/OpenVINO worker,
models, GPU Block provider or other application implementation is included.
The native provider and shaders retain upstream license headers. See LICENSE,
NOTICE, PROVENANCE.md, and licenses/ for Apache-2.0 and OpenCV BSD terms.
References in the original PROVENANCE.md to other components describe the full
upstream project, not the contents of this subset.

Veyra modifications, 2026-09-13:

- `native_strict_dis_provider.cpp`: stage SRV creation in a non-visible heap
  before copying into the visible heap, following Veyra's existing driver workaround.
- Removed an extra trailing blank line from the BSD license file; its text is unchanged.
- Algorithm and shader math are unchanged. `shader-recipes.json` contains the
  DIS entries selected from upstream source-manifest.json. The CMake recipe
  retains their entry points, macros and strict floating-point flags.
- Veyra selects FAST with two slots and both directions. This is not a claim
  of parity with the author's newer real-time toolbox or its default preset.
- The adapter outside this directory converts existing encoded flow RGB to
  full-range BT.709 R8 luma on GPU. RawLuma8 avoids a second limited-range
  expansion. Output is current-to-previous in flow-input pixel units, with
  photometric and roundtrip confidence; the shared FlowAdapt scales once for
  each consumer. Source pictures themselves are unchanged.
- Veyra supplies the queue and completion fence, retires slots before descriptor
  reuse, and drains before destruction. No new CPU pixel readback or per-pass wait.

Future binary packages must include this file, LICENSE, NOTICE, PROVENANCE.md,
licenses/* and all compiled `shaders/dis/*.dxil` alongside Veyra's adapter shaders.
The source subset and build recipes remain in the matching Veyra source revision.

PushFPS modifications, 2026-11-25 (Package 16, "Lower delay" item 4 d):

- `native_strict_dis_provider.{h,cpp}`: `make_float_dis_provider`, the same provider without the FP64 feature check,
  meant for the same shaders compiled with `-D double=float` (`cmake/VeyraGpuDis.cmake`, output `shaders/dis_float`).
  Shader sources are unchanged.
- `native_strict_dis_provider.{h,cpp}` (2026-11-25, crash triage of the predicted frame's flow self-check):
  `strict_dis_set_test` / `strict_dis_test_names`, tests only: record at most N dispatches of one `record()` (the
  skipped ones keep their barriers) and keep each dispatch's shader name and group count, so a separate process per
  limit can find the first dispatch whose execution fails (`tests/integration/dis_triage.py`). `Pipeline` keeps its file
  name for that list. Nothing changes when the limit is not set.
