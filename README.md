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

From NVIDIA's *NVENC Preset Migration Guide* (Video Codec SDK 11.1, Tables 1 and
2), using the Ampere/Turing column at 1080p. H.264 and HEVC genuinely differ:

| Legacy preset | H.264 | HEVC | Tuning |
|---|---|---|---|
| `HP` | P2 | P1 | High Quality |
| `DEFAULT` | P3 | P5 | High Quality |
| `HQ` | P4 | P6 | High Quality |
| `BD` | P4 | P5 | High Quality |
| `LOW_LATENCY_HP` | P2 | P2 | Low Latency |
| `LOW_LATENCY_DEFAULT` | P3 | P3 | Low Latency |
| `LOW_LATENCY_HQ` | P4 | P4 | Low Latency |
| `LOSSLESS_HP` | P2 | P3 | Lossless |
| `LOSSLESS_DEFAULT` | P3 | P5 | Lossless |

The shim translates **preset identity only**. NVIDIA's tables also list multipass,
GOP length and slice-mode columns for reproducing a legacy preset exactly; those
are deliberately not applied, because VEGAS sets its own rate control, bitrate and
GOP on the config it gets back. Forcing them would override your render settings.

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
