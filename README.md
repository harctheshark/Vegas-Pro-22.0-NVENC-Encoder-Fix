# vegas-nvenc-fix

A compatibility shim that fixes **VEGAS Pro render error `0x80660008 (message missing)`**
on recent NVIDIA drivers, by restoring the NVENC preset API that NVIDIA removed.

No VEGAS file is patched. No NVIDIA file is patched or redistributed. The fix is a
single proxy DLL that sits next to `vegas220.exe`, forwards everything to the real
driver, and repairs the two calls that a decade-old NVENC client can no longer make.

---

## The bug

On NVIDIA driver branches from 591.x onward, rendering with NVENC hardware
acceleration in VEGAS Pro fails immediately:

> An error occurred while creating the media file Untitled.mp4.
> Error 0x80660008 (message missing)

### Decoding the error number

`0x80660008` is not an opaque VEGAS code. `mxavcaacplug.dll` builds it by ORing
the raw NVENC status into a fixed mask — the instruction appears at eleven sites
in that DLL, each immediately after a failed call:

```asm
000000018004AE95:  call r8                  ; nvEncOpenEncodeSessionEx
000000018004AE98:  test eax,eax
000000018004AE9A:  je   ...success
000000018004AE9C:  or   eax,80660000h       ; 0x80660000 | NVENCSTATUS
```

So **`0x8066NNNN` means NVENCSTATUS `NNNN`**, and the low half is readable
straight off the `NVENCSTATUS` enum:

| Error shown | NVENC status | Meaning |
|---|---|---|
| `0x80660001` | 1 | `NV_ENC_ERR_NO_ENCODE_DEVICE` |
| `0x80660002` | 2 | `NV_ENC_ERR_UNSUPPORTED_DEVICE` |
| `0x80660008` | **8** | **`NV_ENC_ERR_INVALID_PARAM`** |
| `0x8066000C` | 12 | `NV_ENC_ERR_UNSUPPORTED_PARAM` |
| `0x8066000F` | 15 | `NV_ENC_ERR_INVALID_VERSION` |

`0x80660008` therefore means *invalid parameter*, and it is raised by the
plugin's own pre-flight validation in `NvHWEncoder.cpp` — often **before** any
NVENC call is made, which is why an NVENC-level trace can come back clean while
the render still fails. That validation returns `NV_ENC_ERR_INVALID_PARAM` when
the frame is larger than the configured maximum, when width or height is zero,
or when a 10-bit pixel format is paired with H.264 (its only logged case:
`"10 bit is not supported with H264"`, line 755).

Those diagnostics go to **stderr**, which a GUI process discards. Capture them
with `scripts\run-vegas-traced.ps1`, which starts VEGAS with an inherited stderr
handle.

### Root cause

NVIDIA Video Codec SDK 13.x **removed the legacy encode presets** —
`NV_ENC_PRESET_DEFAULT/HP/HQ/BD/LOW_LATENCY_*/LOSSLESS_*`. They are gone from the
public header, and the driver now rejects them.

VEGAS Pro 17–22 ships MAGIX/MainConcept encoder plug-ins that reference *only*
those legacy GUIDs. Scanning the shipped binaries on VEGAS Pro 22.0 shows it
directly:

| Plug-in | Legacy preset GUIDs | Modern (P1–P7) GUIDs |
|---|---|---|
| `FileIO Plug-Ins/mxavcaacplug/mxavcaacplug.dll` (MAGIX AVC/AAC MP4) | 7 | **none** |
| `FileIO Plug-Ins/mxavcaacplug/mc_cpu/mc_enc_hevc.dll` | 5 | **none** |
| `FileIO Plug-Ins/mxcompoundplug/mvencnvidia.dll` | 1 | P5 |

`mxavcaacplug.dll` — the renderer that produces `Untitled.mp4` — contains the
literal error string `nvEncGetEncodePresetConfig returned failure` and aborts the
render when that call fails. VEGAS reports the abort as `0x80660008`.

### Measured driver behaviour

From `nvenc_probe.exe` on driver 616.92 / RTX 4090 (`tools/nvenc_probe.c`):

```
driver max supported version      : 13.1
NvEncodeAPICreateInstance          -> SUCCESS for every SDK 7.0 .. 13.1
nvEncGetEncodePresetConfig         -> still a valid, non-NULL pointer
presets enumerated for H.264       -> 7   (P1..P7 only, no legacy presets)

nvEncGetEncodePresetConfig(<any legacy GUID>)   -> ERR_UNSUPPORTED_PARAM
nvEncGetEncodePresetConfigEx(P1..P7, tuning)    -> SUCCESS
```

So the entry point is fine and the function pointer is fine. Only the *preset
identity* is no longer recognised.

### Why translation alone is not enough

`mxavcaacplug.dll` drives NVENC as an **SDK 7.1 client** — it sends function-list
version `0x71020007` and stamps every API struct with 7.1-era versions. That
matters because the replacement API refuses those stamps
(`tools/nvenc_probe71.c`):

```
nvEncGetEncodePresetConfigEx, 7.1-stamped structs  -> ERR_INVALID_VERSION
nvEncGetEncodePresetConfigEx, current stamps       -> SUCCESS
```

So a shim that only swapped the preset GUID would still fail: the translated call
would be rejected for its version stamp instead. The shim therefore also uplifts
struct versions. Crossing the two version fields shows that only
`NV_ENC_*::version` is checked, not `apiVersion`:

| `op.version` | `op.apiVersion` | `nvEncOpenEncodeSessionEx` |
|---|---|---|
| 13.1 | 13.1 | SUCCESS |
| 13.1 | 7.1 | SUCCESS |
| 7.1 | 13.1 | `ERR_INVALID_VERSION` |

> **A measurement trap worth recording.** The driver latches a client API version
> per *process* at `NvEncodeAPICreateInstance`. A probe that creates a current
> -version instance first will then see 7.1 stamps rejected everywhere and
> conclude, wrongly, that old clients are locked out entirely. Run the old-client
> test before any newer instance exists — `nvenc_probe71.c` section `[1b]` does,
> and shows a 7.1 session opening fine. The only genuine break is the presets.

---

## What the fix does

`nvEncodeAPI64.dll` in the VEGAS program folder is found by the loader ahead of
`%SystemRoot%\System32` (the application directory precedes the system directory
in the standard search order). The shim forwards every call to the genuine driver
DLL, loaded by absolute System32 path, and repairs two things:

1. **Version uplift.** Every NVENC struct begins with a `uint32_t version`. On
   the way in the shim overwrites it with the stamp this build understands, and
   restores the caller's original on the way out, so the caller's memory is
   returned exactly as it was handed over. This is safe because NVENC structs are
   fixed size across SDK revisions — the function list is 317 pointer slots in
   SDK 8.0, 11.1 and 13.1 alike — so new fields are carved from trailing reserved
   space an older client has already zeroed, and zero is the documented default.
   `tuningInfo`, which a pre-SDK-10 client never sets and for which zero is
   invalid, is filled in explicitly.

2. **Preset translation.** A removed legacy preset GUID is mapped to the P1–P7
   preset and tuning info NVIDIA's migration guide calls equivalent, and fetched
   through `nvEncGetEncodePresetConfigEx`. The legacy GUIDs are also
   re-advertised by `nvEncGetEncodePresetCount` / `...GUIDs`, and rewritten in
   `nvEncInitializeEncoder`.

Everything else passes through untouched, including all eight undocumented
`NvTool*` exports, forwarded in assembly so that any signature survives intact.

**The real driver function is always tried first.** On a driver where the legacy
presets still work, the shim changes nothing, so it is safe to leave installed
across driver updates and rollbacks.

### Preset mapping

NVENC separates two axes, and the mapping follows that split:

* **preset P1…P7** — speed vs quality, P1 fastest, P7 best
* **tuning info** — what the encode is *for*

Each legacy family keeps its tuning, and the words the host shows in its dropdown
select the point on the P scale they claim to select. Both codecs use the same
numbers, so a given name means one thing:

| VEGAS dropdown | Legacy GUID | Preset | Tuning |
|---|---|---|---|
| High performance | `HP` | **P1** | High Quality |
| Default | `DEFAULT` | **P4** | High Quality |
| **High quality** | `HQ` | **P6** | High Quality |
| *(Blu-ray)* | `BD` | **P5** | High Quality |
| Low latency · high performance | `LOW_LATENCY_HP` | **P1** | Low Latency |
| Low latency · default | `LOW_LATENCY_DEFAULT` | **P4** | Low Latency |
| Low latency · high quality | `LOW_LATENCY_HQ` | **P6** | Low Latency |
| Lossless · high performance | `LOSSLESS_HP` | **P1** | Lossless |
| Lossless · default | `LOSSLESS_DEFAULT` | **P4** | Lossless |

**For file rendering, use "High quality" (P6).** The Low Latency tunings trade
compression efficiency for encode latency, which only pays off when streaming;
Lossless ignores your bitrate and produces enormous files.

This deliberately departs from NVIDIA's *NVENC Preset Migration Guide*. Those
tables map a legacy preset to whatever reproduces its **old** output — for H.264,
`HQ` → P4 — and they differ per codec, so the same name would mean different
things in AVC and HEVC. That is the right target for bit-compatibility and the
wrong one here: the host is working again, so the names should describe what you
actually get. P6 is better than anything VEGAS 22 could originally reach. Edit
`src/nvenc_preset_map.h` to choose differently; P7 is the obvious alternative.

The shim translates **preset identity only**. NVIDIA's tables also list multipass,
GOP length and slice-mode columns; those are deliberately not applied, because
VEGAS sets its own rate control, bitrate and GOP on the config it gets back.
Forcing them would override your render settings.

---

## Install

Requires Visual Studio 2022 (or Build Tools) with the **Desktop development with
C++** workload.

```bat
scripts\build.cmd
```

Then, from an **elevated** PowerShell (writing to `C:\Program Files` needs admin),
with VEGAS closed:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\install.ps1
```

The `-ExecutionPolicy Bypass` applies to that one invocation only and changes no
machine setting; Windows blocks unsigned `.ps1` files by default. If your policy
already allows local scripts, `.\scripts\install.ps1` works directly.

The installer auto-detects the VEGAS folder, refuses to run while VEGAS is open,
backs up anything already at the destination, verifies the copy by SHA-256, and
writes a manifest so the uninstaller can put things back exactly.

To target a specific install:

```powershell
.\scripts\install.ps1 -VegasPath "D:\Apps\VEGAS Pro 21.0"
```

### Uninstall

```powershell
.\scripts\uninstall.ps1
```

Removes the shim and restores any displaced file. It refuses to delete a DLL whose
hash does not match the manifest unless you pass `-Force`.

### Disable without uninstalling

Set `VEGAS_NVENC_FIX_DISABLE=1`. The shim becomes a pure pass-through.

---

## Verify

`nvenc_verify.exe` runs the exact sequence VEGAS uses — open session, legacy
preset config, encoder init with a legacy preset, then ten real encoded frames.
`--as71` impersonates VEGAS precisely, declaring an SDK 7.1 client and stamping
every struct the way `mxavcaacplug.dll` does.

```bat
build\nvenc_verify.exe                              :: raw driver, modern client
build\nvenc_verify.exe --as71                       :: raw driver, as VEGAS
build\nvenc_verify.exe build\nvEncodeAPI64.dll      :: shim, modern client
build\nvenc_verify.exe build\nvEncodeAPI64.dll --as71  :: shim, as VEGAS
```

On an affected driver:

| Case | Result | First failure |
|---|---|---|
| raw driver, modern client | BROKEN | `nvEncGetEncodePresetConfig` status 12 |
| raw driver, VEGAS 7.1 | BROKEN | `nvEncGetEncodePresetConfig` status 12 |
| **shim, modern client** | **WORKING** | — |
| **shim, VEGAS 7.1** | **WORKING** | — |

A passing run ends with 10 frames and a non-empty H.264 bitstream.

`nvenc_probe.exe` dumps the full driver capability picture and is the right thing
to attach to a bug report.

---

## Logging

The shim writes to `%LOCALAPPDATA%\vegas-nvenc-fix\nvenc-shim.log`:

```
--- vegas-nvenc-fix 1.0.0 loaded ---
host process: C:\Program Files\VEGAS\VEGAS Pro 22.0\vegas220.exe
real driver DLL: C:\WINDOWS\system32\nvEncodeAPI64.dll
CreateInstance(ver 0x7102000D = SDK 13.1) -> SUCCESS
translated preset HQ (H.264) -> P4 tuning=1 : SUCCESS
InitializeEncoder: preset HQ -> P4 tuning=1 (1920x1080, params ver 0xF107000D)
```

| Variable | Effect |
|---|---|
| `VEGAS_NVENC_FIX_LOG` | `0` off, `1` normal (default), `2` verbose |
| `VEGAS_NVENC_FIX_DISABLE` | `1` = pure pass-through |
| `VEGAS_NVENC_FIX_ENUM` | `0` = do not re-advertise legacy presets |
| `VEGAS_NVENC_FIX_UPLIFT` | `0` = do not rewrite struct version stamps |

---

## Troubleshooting

**`0x80660008` specifically — invalid parameter, raised before NVENC is called.**
The shim log shows `OpenEncodeSessionEx` succeeding, `DestroyEncoder` right
after, and no preset or init call in between. The encoder opened fine and the
plugin then rejected the configuration itself. Two settings cause this:

1. **A 10-bit pixel format with H.264.** The plugin refuses that combination
   outright (`NvHWEncoder.cpp` line 755). Check **File → Properties → Video →
   Pixel format** and set it to **8-bit**; "32-bit floating point" makes VEGAS
   work in high bit depth and can push a 10-bit format at the encoder. Switching
   the render to HEVC also sidesteps this check, since HEVC does support 10-bit.
2. **A frame larger than the encoder's configured maximum.** In the render
   template's Custom Settings, make the frame size match the project exactly and
   untick *Allow source to adjust frame size*.

To find out which, capture the plugin's own message with
`scripts\run-vegas-traced.ps1` and look for a `NvHWEncoder.cpp` line.

**The Preset list is empty and NVENC renders fail, but the shim log shows a
successful `OpenEncodeSessionEx` followed by `DestroyEncoder` and nothing else.**
That pattern means NVENC is healthy and VEGAS is choosing not to use it. The
usual cause is a second GPU: on a machine with an integrated Radeon or Intel
adapter driving the primary display, Windows hands VEGAS the iGPU, and VEGAS
will not offer the NVIDIA encoder for a non-NVIDIA render device. Check what
VEGAS detected — its own telemetry reports the adapter, and so does:

```powershell
Get-CimInstance Win32_VideoController |
  Select-Object Name, CurrentHorizontalResolution
```

An adapter with a non-null resolution is driving a display. If the integrated
one is on your primary monitor, fix it in this order:

1. Settings → System → Display → **Graphics** → Add desktop app →
   `vegas220.exe` → Options → **High performance** (the NVIDIA card).
2. VEGAS → Options → Preferences → **Video** → *GPU acceleration of video
   processing* → select the NVIDIA card. Restart VEGAS.
3. Most robust: move the primary monitor's cable to the NVIDIA card.

This is a separate problem from the one this shim fixes, and both have to be
resolved: the GPU selection is what lets VEGAS attempt NVENC at all, and the
shim is what makes the attempt succeed once it does.

**Still failing, and the log file does not exist.** The shim was never loaded.
Confirm `nvEncodeAPI64.dll` sits in the same folder as `vegas220.exe`, not in a
plug-in subfolder — the loader searches the *executable's* directory. Copy
`nvenc_whichdll.exe` into the VEGAS folder and run it to see which file the
loader actually picks:

```
this executable : C:\Program Files\VEGAS\VEGAS Pro 22.0\nvenc_whichdll.exe
resolved to     : C:\Program Files\VEGAS\VEGAS Pro 22.0\nvEncodeAPI64.dll
=> local copy wins. A shim placed here WILL be used.
```

If it reports the System32 copy instead, the shim is in the wrong folder.

**Log exists but shows no `translated preset` line.** The render never reached
NVENC. Check that the render template actually has hardware encoding enabled.

**A different error number now.** The preset breakage is fixed and something else
is failing; the log's `InitializeEncoder -> ...` line names the real status.

**Not all render templates use NVENC.** "MAGIX AVC/AAC MP4" with hardware
acceleration set to NVIDIA NVENC is the affected path. CPU-only templates were
never broken.

### Alternatives if you would rather not install anything

- Set the render template's hardware acceleration to **Off / CPU** — slower, works.
- Roll the NVIDIA driver back to 580.x or earlier — keeps the legacy presets, but
  gives up later driver fixes, and 590+ dropped Maxwell/Pascal support anyway.

---

## Scope and limitations

- **x64 only.** VEGAS Pro 17+ is 64-bit; there is no `nvEncodeAPI.dll` (32-bit) shim.
- **Turing or newer.** The mapping uses NVIDIA's Ampere/Turing column. Driver
  branches that removed the legacy presets also dropped Maxwell and Pascal support,
  so older cards cannot reach this state.
- **Quality is equivalent, not bit-identical.** P-presets are a different control
  surface; NVIDIA describes the mapping as "closest equivalent".
- Tested on VEGAS Pro 22.0 build 248, driver 616.92, RTX 4090. The same plug-ins
  ship in VEGAS 17–21, so it should apply there; `-VegasPath` targets them.

---

## Repository layout

```
src/nvenc_shim.c                 the proxy and the three repaired calls
src/nvenc_preset_map.h           NVIDIA's migration table as data
src/nvenc_deprecated_presets.h   GENERATED - legacy GUIDs
src/nvenc_thunks.asm             register-exact NvTool* forwarders
src/nvenc_shim.def               export surface, ordinals pinned to the driver's
tools/nvenc_probe.c              driver capability probe
tools/nvenc_verify.c             end-to-end pass/fail test
tools/nvenc_probe71.c            what the driver does for an SDK 7.1 client
tools/nvenc_whichdll.c           shows which nvEncodeAPI64.dll the loader picks
scripts/build.cmd                MSVC build
scripts/install.ps1              install, with backup + manifest
scripts/uninstall.ps1            uninstall, with restore
scripts/gen_deprecated_presets.sh  regenerates the GUID header from upstream
third_party/nvEncodeAPI.h        NVIDIA NVENC header (MIT, via nv-codec-headers)
```

The legacy GUIDs are *extracted mechanically* from nv-codec-headers `n11.1.5.3`
rather than transcribed, so they can be re-verified against upstream at any time:

```bash
bash scripts/gen_deprecated_presets.sh
```

---

## Licence

MIT — see [LICENSE](LICENSE).

`third_party/nvEncodeAPI.h` is NVIDIA's header, redistributed under the MIT
permission notice it carries, obtained from
[FFmpeg/nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers).

Not affiliated with or endorsed by MAGIX or NVIDIA.

Built with [Claude Code](https://claude.com/claude-code) (Claude Opus 5).
