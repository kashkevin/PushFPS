# The neural layer's runtime (optional, experimental)

Status, 20 November 2026: written and unit-tested on Linux; the Windows code is **unverified on Windows** and
nothing here has run on a graphics card. No performance or picture-quality claim is made in this file.

## What it is

"AI picture enhancement" (the neural layer) runs NVIDIA's `nvngx_dlssnr.dll`, the runtime of NVIDIA's DLSS 5
"3D-Guided Neural Rendering", on a **decoded video stream**. The layer applies NVIDIA's model to a decoded video
stream without the game's motion vectors, depth or masks; it is not NVIDIA's game-integrated DLSS 5 and not
endorsed by NVIDIA. A stream gives the model compressed colour with the HUD already drawn in, so its result on
video is its own thing and has to be judged on the screen.

**HDR accuracy with the neural layer is unverified.** NVIDIA's open RTX Remix code feeds the model an LDR-domain
picture and recovers highlights afterwards; nobody documents the model on an HDR video stream. The Settings row
says so, and a test run can be told apart by `hdr=` in `[slipstream-config]` and `streamHdr=` / `videoHdr=` in
`[slipstream-pipeline]`.

The layer is off unless the user turns it on, and it cannot run without the runtime file described here.

## What the project does and does not do with the file

- **Never modified, never committed, never downloaded by the app.** The repository and the app contain no NVIDIA
  binary. There is no download code for this file.
- **Only an NVIDIA-signed original is used.** Before the file can be selected, the app checks the copy in
  `runtime\experimental\nr-original` (the file that will be loaded, not a file somewhere else):
  1. Windows' Authenticode verdict (`WinVerifyTrust`, whole chain, revocation as Windows checks it; from the
     cache at start so that starting never waits for the network, over the network when a file is installed);
  2. the signer: the verified signature's own certificate, common name and organisation both exactly
     `NVIDIA Corporation` (compared whole, not searched for in a longer text);
  3. the version resource names `nvngx_dlssnr.dll` or NVIDIA's DLSS product, company NVIDIA;
  4. a size sanity range;
  5. the SHA-256 against the versions the project has a record of. Recorded today: **310.8.0.0**, SHA-256
     `E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E`, 165,840,496 bytes. A file that claims a
     recorded version with other bytes is refused. Any other NVIDIA-signed version is accepted only as
     "NVIDIA-signed, version not tested with this app" and is marked so in Settings, in the layer's choice and in
     the log.
  A file with NVIDIA's certificate over changed content (Windows: hash mismatch), an unsigned file, another
  signer, a revoked or untrusted certificate: refused, with the reason shown. A refused file is never selected.
- **The checked file is the loaded file.** An accepted file is checked through a handle that stays open without
  write or delete sharing until the app closes (or the user removes or replaces the runtime in Settings), so it
  cannot be swapped between the check and NVIDIA's loader.
- **Community-modified builds** (for example the ones for older cards) are not accepted, not installed by
  default and not recommended. The older opt-in `-ExperimentalExtras Yes` is unchanged and separate.
- **Other graphics cards.** Whether a card can run the runtime is asked of the app's existing hardware gate, not
  of a model list in new code. On AMD and Intel cards, and NVIDIA cards the runtime does not support, the layer
  reports "not supported on this graphics card" and nothing else happens. NVIDIA states the feature runs on
  RTX 50 cards only today and needs driver 616.64 or newer.
- **Follow-up, not done (nothing was guessed):** NVIDIA's open RTX Remix source asks NGX itself whether the
  feature is available, whether it needs an updated driver and which driver version is the minimum. This tree
  holds no verified parameter name for those three answers for the neural-rendering feature
  (`include/veyra/ngx/DlssNrParameters.h` lists the create and evaluate parameters only; the app loads the
  runtime through `src/ngx/DlssNrRuntimeAdapter.cpp`, which asks none of them; the availability and driver
  parameters in `src/ngx` belong to frame generation and RTX Video HDR). So today's gate stays, `driver-too-old`
  is never reported by the bridge (an old driver shows as the layer failing to start, in the log), and the
  follow-up is: read the three names in Remix's `rtx_ngx_wrapper.cpp`, confirm them against the runtime on the
  RTX 50 PC, then feed `PushNrGpu::supportsOriginal` and `driverOk` (engine/PushNrRuntime.h) from them.

## Where the file legitimately comes from (web research of 20 November 2026)

Facts, with their limits:

- NVIDIA has published **no DLSS 5 SDK, header, guide or licence**. Its developer-forum replies of 9 and
  20 November 2026 say partner developers only. The public DLSS SDK (release notes up to 310.9.16) and the
  Streamline SDK do not contain the runtime.
- The channels the research confirms are **products**: partner games (NBA 2K27 since 4 September 2026; NVIDIA
  also names Hogwarts Legacy, War Thunder and Where Winds Meet) and NVIDIA's **RTX Remix 1.6.1** (23 October
  2026), whose public build recipe installs `nvngx_dlssnr.dll`. **Not verified:** the contents of Remix's
  released package, and the version, signature and terms of its copy. Press analysis of NBA 2K27's early file
  reports version 310.8.0, NVIDIA-signed, 158.2 MB, which is the recorded file's size.
- A version "310.9.16.0" of this file is confirmed by no NVIDIA page. The app therefore treats every version
  other than the recorded 310.8.0.0 as "NVIDIA-signed, version not tested with this app".
- Upstream Veyra's NVIDIA runtime package (2.1.9) carries the signed 310.8.0.0 file. Nothing that was read
  grants that, so this project's installer never keeps it by default (see below).

Open questions (stated as they stand; no verdict is given here and this is not legal advice):

- No NVIDIA text was found that lets a third-party program load, bundle or redistribute `nvngx_dlssnr.dll`,
  and that includes the signed 310.8.0.0 copy in upstream's package.
- The NVIDIA RTX SDKs License covers SDKs NVIDIA made available under it (distribution only as incorporated
  into an application, never stand-alone, only for systems with NVIDIA graphics); DLSS 5 has not been made
  available under it. The GeForce driver licence treats the driver as a single product whose parts may not be
  separated or distributed.
- Whether any terms cover a copy from a game, from RTX Remix or from upstream's package once another program
  loads it is unknown. That is why the file is optional, asked for in plain words, and the user's decision.
- NVIDIA's brand rules require approval for its marks and forbid implying endorsement; the interface says what
  the layer is and is not.

## Installing, removing

- **Installer** (`Install-PushFPS.ps1 -NeuralRuntime Ask|Yes|No`, default `Ask`): the packaged copy is never
  kept without an explicit yes. In a console window `Ask` puts one question, in plain words (an NVIDIA-signed
  original; NVIDIA has published no terms covering its use by other programs; your decision). `Yes` keeps
  upstream's packaged `nr-original\nvngx_dlssnr.dll` only if its signature is valid, the signer is NVIDIA
  Corporation and its SHA-256 is the recorded one; otherwise it is refused with the reason. `No`, and `Ask` with
  no one to ask (for example an update started by the app), remove it, including a copy an earlier install
  placed, and say so; what was left out is listed in `PushFPS-VERSION.txt`. After such an update the runtime is
  installed again from Settings if wanted. The test kit's `install-slipstream.ps1 -NeuralRuntime Ask|Yes|No`
  asks once and passes the answer to every build whose installer knows the choice (`No` without a window).
- **In the app**: *Settings → Components and licenses → Neural rendering runtime (experimental)* shows the state
  (not installed / installed with version, "NVIDIA-signed original", tested or untested / refused with the
  reason) and offers **Install from a file you have…** (the file is checked, copied into
  `runtime\experimental\nr-original` through a `.new` / `.old` swap, and the installed copy is checked again;
  the place the file came from is neither stored nor logged) and **Remove**.

## Why the layer could not be turned on before, and what changed

The layer's default runtime choice was the community-modified slot (`nr-ampere`, id 2), whose file this
project's installer leaves out because it is not an NVIDIA original; the availability check then answered "this
package lacks the runtime components for this feature", while the NVIDIA-signed original (`nr-original`, id 3)
sat installed and unused. Now an untouched default resolves to the verified original when the default slot's
file is not installed; a runtime the user picked is never changed (preference `pushNrRuntimeChosen`), and the
reason shown when the layer cannot run names the blocker: `runtime-missing`, `not-signed-original`,
`signature-unchecked`, `gpu-not-supported`, `driver-too-old`, `runtime-version-untested` (usable, marked).

Once per start the log has one line, without paths or names:

```
[nr-runtime-status] original=present|absent originalVersion=<a.b.c.d|none> originalSigned=1|0 sf=present|absent
  selected=sf|original|amd selectedAvailable=1|0 gpuSupportsOriginal=1|0 reason=<blocker|none> originalTested=1|0 resolvedDefault=1|0
```

## Strength

Four named looks set every layer's Local tone / Local structure. The app passes the two values unchanged to the
runtime's own tone-strength and structure-strength parameters (`src/pipeline/EnhanceGraph.cpp`, the evaluate
block), so they are on the runtime's own 0..1 scale:

| Look | tone | structure | Where the numbers come from |
|---|---|---|---|
| `conservative` | 0.15 | 0.35 | half of NVIDIA's published RTX Remix defaults (tone 0.3, structure 0.7), so clearly below both |
| `subtle` | 0.40 | 0.50 | the quick setup's existing look (PushFPS 3.2.1), unchanged |
| `balanced` | 0.70 | 0.80 | the quick setup's existing look, unchanged |
| `strong` | 1 | 1 | what every layer starts with, unchanged |

Remix's defaults are from NVIDIA's open `RtxOptions.md` (`rtx.dlssNeuralRendering.*`: structure 0.7, tone 0.3,
skin structure 0.5); the app's skin value is left at the runtime's own default. Note that `subtle`'s tone (0.40)
is above Remix's default tone: it is "subtle" only next to the app's starting value. Whether any look suits a
given game on video is for the eye on the target screen; none has been looked at on hardware.

## What the next hardware run must show

`[nr-runtime-status]` with `originalSigned=1 selected=original selectedAvailable=1` on the RTX 50 PC after the
runtime was installed by choice; the Settings row "Installed: 310.8.0.0 · NVIDIA-signed original · tested with
this app"; the layer turning on from the quick setup; a deliberately altered copy being refused; and Remove
working with no picture open. None of this has been run yet.
