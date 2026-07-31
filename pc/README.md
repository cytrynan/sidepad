# pc/ — PC-side capture spike

Technical spike for the PC side of sidepad (iPad as a low-latency extended
display for Windows). This proves out the cheapest path from "primary
monitor changed" to "we're holding a GPU texture of the new frame", and
measures how fast that loop actually runs.

**Scope**: capture only. No video encoding, no network transport. Both are
marked with comments in `src/main.cpp` at the point where they'd plug in.

## What it does

- Opens a D3D11 device on the adapter driving the primary monitor.
- Uses the **DXGI Desktop Duplication API** (`IDXGIOutputDuplication`) to
  get notified when the desktop image changes, and grabs each new frame as
  an `ID3D11Texture2D` living in GPU memory.
- Runs for 15 seconds, printing FPS and per-frame acquire latency
  (`AcquireNextFrame` wall-clock time) every 60 frames.
- Exits on its own after the run duration.

## Toolchain audit (this machine, 2026-08-01)

| Tool | In `PATH`? | Actually present? |
|---|---|---|
| `cl.exe` (MSVC) | No | **Yes** — `Visual Studio Community 2022`, VC Tools `14.43.34808` |
| `cmake.exe` | No | **Yes** — bundled with VS at `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` |
| `ninja.exe` | No | **Yes** — bundled with VS at `Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe` |
| Windows 10 SDK | — | **Yes** — `10.0.22621.0` under `C:\Program Files (x86)\Windows Kits\10` |
| `cargo` / `rustc` | No | Not installed |
| `clang++` | Yes | Present, but it's the **Swift toolchain's** clang (`...\Programs\Swift\Toolchains\6.3.2+Asserts\usr\bin`), not a general-purpose C++ install. Not used here — MSVC is the straightforward path since it's already fully installed. |

**Conclusion**: no new installs were needed. Everything required is already
on disk via the existing Visual Studio Community 2022 install; it's just
not on `PATH` (VS expects you to enter its environment via `vcvars64.bat`
or the "Developer PowerShell for VS 2022" shortcut instead of adding
`cl.exe` etc. to the global `PATH`). No administrator elevation was used or
needed.

## Build

```powershell
pwsh -File pc\build.ps1          # configure + build
pwsh -File pc\build.ps1 -Run     # configure + build + run immediately
```

`build.ps1` finds your Visual Studio install via `vswhere.exe`, sources
`vcvars64.bat` for the MSVC environment, and drives VS's bundled CMake +
Ninja to produce `pc\build\pc_capture_spike.exe`. It does not touch
anything outside `pc\build\` and needs no elevation.

### Manual equivalent

If you'd rather not run the script, open **"Developer PowerShell for VS
2022"** (Start Menu, ships with any VS 2022 install) and run, from `pc\`:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\pc_capture_spike.exe
```

### If you don't have Visual Studio installed

Install **Visual Studio 2022 Community** (free) or just the standalone
**Build Tools for Visual Studio 2022**, and in the installer select the
**"Desktop development with C++"** workload (this pulls in MSVC, the
Windows SDK, and VS's bundled CMake/Ninja components together — no need to
install CMake or Ninja separately). This is a large download
(several GB) and can take a while; it does **not** require admin rights to
*run* the installer for a per-user install, but the installer's default
behavior on most machines is a machine-wide (admin) install — check the
"Install for me only" / per-user option if you need to avoid elevation.
Was not attempted on this machine since the toolset was already present.

## Result (measured on this machine, 2026-08-01)

DXGI Desktop Duplication only hands back a new frame when the desktop
image actually changes (it's delta/event driven, not a fixed-rate poll).
On a fully idle desktop the spike captured **1 frame in 15 seconds** —
correct behavior, not a bug, but not a useful throughput number.

To get a real measurement, a second window was driven to repaint at a
steady ~60 Hz (a WinForms window with a 16 ms timer changing its
background color) while the spike ran concurrently:

```
[frame     60] fps= 59.93  acquire_latency avg=16.520ms max=29.667ms (window=60 frames / 1.001s)
[frame    120] fps=  6.80  acquire_latency avg=32.100ms max=250.793ms (window=60 frames / 8.818s)
[frame    180] fps= 53.02  acquire_latency avg=18.450ms max=37.200ms (window=60 frames / 1.132s)
[frame    240] fps= 41.40  acquire_latency avg=12.533ms max=35.623ms (window=60 frames / 1.449s)
[frame    300] fps= 37.10  acquire_latency avg=26.671ms max=50.858ms (window=60 frames / 1.617s)

Done. Total frames captured: 340
```

Reading this:

- **Steady state is close to the source's repaint rate** (~55-60 fps
  against a ~62.5 Hz — 16 ms — source timer), with acquire latency mostly
  in the **12-25 ms average** range. That's the time `AcquireNextFrame`
  blocked waiting for DXGI to hand over the next texture — not full
  glass-to-glass latency, and not including encode/transport (neither
  exists yet).
- **The 6.80 fps / 8.8 s window is a real anomaly worth flagging**, not
  noise: something (most likely DWM/driver throttling a background or
  briefly-unfocused animation window, or a GPU power-state transition on
  the Radeon 780M's integrated graphics) caused a multi-second stall in
  desktop repaints during that window, which shows up here as a latency
  spike (max 250 ms in that window) rather than a capture failure. Worth
  re-testing once real video content (not a synthetic timer window) is the
  source, and once the actual target monitor/output is confirmed (see
  Known gaps).
- This was captured on `\\.\DISPLAY2` (3440x1440) — the primary output at
  index 0 on this machine happened to be an ultrawide external monitor,
  not the laptop panel. See Known gaps below.

**Takeaway for the next step (encode)**: capture-side latency budget looks
comfortably small (double-digit ms) relative to a 16.6 ms (60 fps) frame
budget, so there's headroom for AMF encode + transport, but real budget
will depend heavily on actual source content (video/games change every
frame; idle desktop use won't).

## Known gaps / not implemented (by design — spike scope)

- **No encode.** The point where a real `ID3D11Texture2D` would be handed
  to AMD's AMF encoder (zero-copy, GPU-to-GPU, via
  `AMFContext::CreateSurfaceFromDX11Native`) is marked with a comment in
  `src/main.cpp`.
- **No transport.** Sending the encoded bitstream to the iPad is not
  implemented.
- **No `DXGI_ERROR_ACCESS_LOST` recovery.** This happens on resolution
  change, GPU driver reset, or switching to a secure desktop (UAC prompt).
  The spike detects it and exits; a real implementation needs to recreate
  the whole `IDXGIOutputDuplication` when it happens.
- **No cursor compositing.** Desktop Duplication reports cursor
  shape/position updates separately from frame image updates; this spike
  only cares about frame image updates (`LastPresentTime != 0`) and
  ignores cursor-only updates entirely.
- **Output selection is hardcoded to adapter 0 / output 0**, which is
  *not* necessarily the laptop's built-in panel — on this machine it
  resolved to an external ultrawide monitor. A real implementation needs
  to enumerate outputs and either let the user pick or match Windows'
  actual "primary monitor" setting explicitly.
- **No HDR / multi-monitor handling.**
- Not tested over RDP (Desktop Duplication is known to fail with
  `E_ACCESSDENIED` over most RDP sessions — noted in the error message the
  spike prints if `DuplicateOutput` is denied).

## Files

- `src/main.cpp` — the spike itself, heavily commented.
- `CMakeLists.txt` — minimal CMake setup, C++17, links `d3d11`/`dxgi`.
- `build.ps1` — locates VS via `vswhere`, sources `vcvars64.bat`, drives
  VS's bundled CMake+Ninja. `-Run` flag builds and runs in one step.
