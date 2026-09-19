# Installation

Fixes `Error 0x80660008 (message missing)` when rendering with **NV Encoder** in
VEGAS Pro, and the empty **Preset** dropdown that comes with it.

---

## Before you start

| | |
|---|---|
| **VEGAS** | Pro 17 – 22 (built and tested against 22.0, build 250) |
| **GPU** | NVIDIA, Turing (RTX 20-series) or newer |
| **Driver** | 591.x or newer — the versions that broke it. Older drivers don't need this |
| **Windows** | 10 or 11, 64-bit |
| **Admin rights** | Yes — the file goes into your VEGAS folder |

You do **not** need Visual Studio, and you do **not** need to roll back your driver.

---

## Install

**1. Download** the latest release `.zip` from the
[Releases page](../../releases) and unzip it anywhere — Desktop or Downloads is
fine. Keep the files together; `install.cmd` needs the `scripts` and `build`
folders next to it.

**2. Close VEGAS Pro completely.** The installer refuses to run while it's open,
because the file it replaces is locked.

**3. Double-click `install.cmd`.**

Windows will ask for administrator rights — that's the installer requesting them
so it can write into your VEGAS folder. Accept.

You should see:

```
==> Locating VEGAS Pro
    Found VEGAS: C:\Program Files\VEGAS\VEGAS Pro 22.0  (via registry)
    Host executable: vegas220.exe
==> Installing nvEncodeAPI64.dll
    Installed: C:\Program Files\VEGAS\VEGAS Pro 22.0\nvEncodeAPI64.dll

  Installed successfully.
```

> **SmartScreen or antivirus warning?** The DLL is unsigned, and it works by
> loading into VEGAS and correcting the calls it makes to your GPU driver —
> behaviour that resembles what a code-signing check is designed to flag. If you
> would rather not run a binary you didn't compile, build it yourself: install
> Visual Studio 2022 with *Desktop development with C++*, then run `install.cmd`
> from a clone of the source. It compiles the DLL and installs it in one step.

---

## Use it

Start VEGAS, then:

**Render As → MAGIX AVC/AAC MP4 → Customize Template**

| Setting | Value |
|---|---|
| **Encode mode** | `NV Encoder` |
| **Preset** | `High quality` |

The **Preset** dropdown should now be populated. If it still shows an empty box,
see Troubleshooting below.

### Which preset?

| Dropdown entry | Maps to | Good for |
|---|---|---|
| High performance | P1 | Fastest, lowest quality |
| Default | P4 | Balanced |
| **High quality** | **P6** | **Rendering a file — use this** |
| Low latency · anything | P1 / P4 / P6, low-latency tuned | Live streaming only |
| Lossless · anything | P1 / P4, lossless tuned | Archival; ignores your bitrate, huge files |

The **Low latency** entries trade picture quality for encode latency, which only
helps when streaming — for a file render they're strictly worse at the same
bitrate. **Lossless** ignores your bitrate setting entirely.

`High quality` maps to **P6**, which is better than the preset VEGAS originally
used. Expect it to render somewhat slower than it used to; if that bothers you,
`Default` (P4) is close to the old behaviour.

---

## Uninstall

**Close VEGAS**, then double-click **`uninstall.cmd`**.

It removes the DLL and restores anything it displaced. Your original render error
will come back, because nothing else about VEGAS was changed.

---

## Troubleshooting

**"No VEGAS Pro installation found"**
Detection checks the uninstall registry, a running VEGAS, Program Files, and
common folders on every fixed drive. If yours is somewhere unusual, tell it
directly — open a Command Prompt in the unzipped folder and run:

```bat
install.cmd "D:\Wherever\VEGAS Pro 22.0"
```

**"vegas220.exe is running"**
Close VEGAS and try again. Check Task Manager if you're not sure — it sometimes
lingers after the window closes.

**The render still fails**
Check the log at:

```
%LOCALAPPDATA%\vegas-nvenc-fix\nvenc-shim.log
```

A working install looks like this:

```
--- vegas-nvenc-fix 2.0.0 loaded ---
pinned: this module will stay mapped for the process lifetime
CreateInstance(0x71020007 = SDK 7.1) -> table uplifted to 13.1
GetEncodePresetConfig(H.264, preset=HQ) via HQ -> SUCCESS
InitializeEncoder: 3440x1440 preset HQ -> P6 tuning=1 : SUCCESS
```

* **No log file at all** — the DLL isn't being loaded. Confirm
  `nvEncodeAPI64.dll` is in the same folder as `vegas220.exe`, not in a
  subfolder.
* **Log stops after `DestroyEncoder`** — the shim was unloaded before the render.
  It pins itself to prevent exactly this, so if you see it, please open an issue
  and attach the log.
* **A different error number** — note it exactly and open an issue. The number is
  meaningful: `0x8066NNNN` is an NVENC status code, so `0x8066000C` means status
  12, `NV_ENC_ERR_UNSUPPORTED_PARAM`.

**It works, but is it actually using the GPU?**
Open Task Manager → Performance → GPU and watch the **NVENC** graph while
rendering. It should be busy. A CPU render leaves it flat.

---

## What this actually changes

* Adds one file: `nvEncodeAPI64.dll`, in the VEGAS program folder.
* Adds one folder: `vegas-nvenc-fix-backup`, holding backups and a manifest.
* Writes a log to `%LOCALAPPDATA%\vegas-nvenc-fix`.

It does **not** modify any VEGAS file, does not touch anything in `System32`,
does not install a service or a driver, and does not phone home. The one runtime
patch it applies lives in memory only and disappears when VEGAS closes.

---

## Switches

For unusual setups, set these as environment variables before starting VEGAS:

| Variable | Effect |
|---|---|
| `VEGAS_NVENC_FIX_LOG=0` | Turn the log off (`2` for verbose) |
| `VEGAS_NVENC_FIX_DISABLE=1` | Pass everything straight through — disables the fix without uninstalling |
| `VEGAS_NVENC_FIX_TRAP=1` | Diagnostic: tag error codes with the failing site |
| `VEGAS_NVENC_FIX_PATCH=0` | Skip the in-memory patch to the encoder plug-in |
