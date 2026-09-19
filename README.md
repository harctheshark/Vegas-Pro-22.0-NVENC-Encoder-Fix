# vegas-nvenc-fix

A compatibility shim that fixes **VEGAS Pro render error `0x80660008 (message missing)`**
on recent NVIDIA drivers, by restoring the NVENC preset API that NVIDIA removed.

No VEGAS file is patched. No NVIDIA file is patched or redistributed. The fix is a
single proxy DLL that sits next to `vegas220.exe` and translates one API call.

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
NvEncodeAPICreateInstance          -> SUCCESS for every SDK 9.0 .. 13.1
nvEncGetEncodePresetConfig         -> still a valid, non-NULL pointer
presets enumerated for H.264       -> 7   (P1..P7 only, no legacy presets)

nvEncGetEncodePresetConfig(<any legacy GUID>)   -> ERR_UNSUPPORTED_PARAM
nvEncGetEncodePresetConfigEx(P1..P7, tuning)    -> SUCCESS
```

So the entry point is fine and the function pointer is fine. Only the *preset
identity* is no longer recognised. That is a translation problem, and translation
is all this shim does.

---

## What the fix does

`nvEncodeAPI64.dll` in the VEGAS program folder is found by the loader ahead of
`%SystemRoot%\System32` (the application directory precedes the system directory
in the standard search order). The shim forwards every call to the genuine driver
DLL, loaded by absolute System32 path, and repairs three things:

1. **`nvEncGetEncodePresetConfig`** — if the driver rejects a legacy preset GUID,
   retries through `nvEncGetEncodePresetConfigEx` using the P1–P7 preset and
   tuning info that NVIDIA's own migration guide designates as equivalent.
2. **`nvEncGetEncodePresetCount` / `nvEncGetEncodePresetGUIDs`** — re-advertises
   the legacy presets, so callers that enumerate before choosing still see them.
3. **`nvEncInitializeEncoder`** — rewrites a legacy `presetGUID` in the init
   params, since the driver rejects it there too.

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
.\scripts\install.ps1
```

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

`nvenc_verify.exe` runs the exact sequence VEGAS uses — legacy preset config,
encoder init with a legacy preset, then ten real encoded frames.

```bat
build\nvenc_verify.exe
build\nvenc_verify.exe build\nvEncodeAPI64.dll
```

On an affected driver the first is expected to fail and the second to pass:

```
A) system DLL                          B) the shim
[1] legacy preset config               [1] legacy preset config
  [FAIL] ... (status 12)                 [ ok ] ...
                                       [2] preset enumeration
  RESULT: BROKEN                         [ ok ] 16 presets, legacy HQ present
                                       [3] encoder initialisation
                                         [ ok ] ...
                                       [4] encoding 10 frames
                                         [ ok ] 10 frames, 1684 bytes
                                       RESULT: WORKING
```

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
