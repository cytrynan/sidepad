# pc/ — PC-side capture + encode spikes

Technical spikes for the PC side of sidepad (iPad as a low-latency extended
display for Windows):

- `pc_capture_spike` — DXGI Desktop Duplication capture, measured 2026-08-03
  (flat 60 fps, non-bottleneck; details below).
- `pc_encode_spike` — capture + **AMF H.264 hardware encode** configured from
  the 2026-08-06 USB datalink measurements, verified same day (see
  "Encode spike" section).
- `pc_test_source` — vsync-paced screen-change generator, the control both
  measurements run against.

**Not yet implemented**: network transport (the encoded bitstream currently
goes to stats + an optional `.h264` dump) and the IddCx virtual display.

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
Ninja to produce `pc\build\pc_capture_spike.exe` and
`pc\build\pc_test_source.exe`. It does not touch anything outside
`pc\build\` and needs no elevation.

## Measuring it

Both binaries take `[run_seconds] [output_index]`. Because capture is event
driven, a capture-only run on an idle desktop measures nothing — start the
source first, then the spike, and compare the two rates:

```powershell
Start-Process .\build\pc_test_source.exe -ArgumentList 40,0
.\build\pc_capture_spike.exe 30 0
```

`pc_test_source` paces itself off the vblank via `Present(1, 0)`, so its
rate is trustworthy in a way a timer-driven window is not — see the header
comment in `src/testsource.cpp`, and the Result section for what happens
when you skip this.

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

## Result (measured on this machine, 2026-08-03)

DXGI Desktop Duplication only hands back a new frame when the desktop
image actually changes (it's delta/event driven, not a fixed-rate poll).
On a fully idle desktop the spike captured **1 frame in 15 seconds** —
correct behavior, not a bug, but not a useful throughput number. Which
also means **the number this spike prints is bounded by the source's
repaint rate, not by how fast capture can go** — a fact that turned out to
matter more here than anything else.

Measured against `pc_test_source` (vsync-paced `Present(1, 0)`), 30 s on
`\\.\DISPLAY2`, 3440x1440 @ 60 Hz:

```
[frame    120] fps= 60.08  acquire_latency avg=16.448ms max=18.444ms (window=60 frames / 0.999s)
               wall: acquire=987ms idle_timeout=0ms(0x) cursor_only=0ms(0x) other=12ms
               source: presents=60 (60.08/s) coalesced=0 max_accum=1 | src_interval avg=16.64ms max=18.54ms
               vsync gaps (16.67ms period): 1x:60
...
Done. Total frames captured: 1799
```

Every one of the 30 windows looked like that. Reading it:

- **Flat 60.00 fps for the whole run** — exactly the display refresh rate,
  which is the ceiling here. No decay, no stalls.
- **`coalesced=0`, `max_accum=1` in every window**: the OS never had to
  merge two presents into one frame, i.e. the capture loop did not fall
  behind a single time.
- **Every inter-frame gap lands in the `1x` bucket** (one refresh period).
  No dropped vsyncs.
- `acquire_latency avg=16.4 ms` is *not* a cost — it is the loop blocking
  inside `AcquireNextFrame` waiting for a frame that doesn't exist yet.
  Sitting at exactly one refresh period is the signature of keeping up.
- Real work outside `AcquireNextFrame` (`other`) is **12-19 ms per
  1000 ms, ~1.5%**.

### The earlier "fps degradation" was the measuring stick, not the capture

The 2026-08-01 run reported `59.93 → 6.80 → 53.02 → 41.40 → 37.10` and was
recorded as a monotonic decline needing investigation. It was neither
monotonic nor a capture problem. That run's source was a WinForms window
repainting on a 16 ms timer, and re-running the spike against the same kind
of source reproduces the old numbers on demand:

```
[frame    180] fps= 36.74  acquire_latency avg=26.943ms max=47.918ms (window=60 frames / 1.633s)
               source: presents=60 (36.74/s) coalesced=0 max_accum=1 | src_interval avg=27.22ms max=48.41ms
               vsync gaps (16.67ms period): 1x:23 2x:36 3x:1
```

37 fps at a 26.7 ms average interval — the old window-5 line almost to the
decimal. The histogram gives the mechanism away: gaps split between `1x`
and `2x`, because a **16 ms timer against a 16.667 ms refresh period beats**
— each repaint either makes the next vblank or misses it and rides the one
after. The average of that mix is ~26 ms ≈ 38 fps. Arithmetic, not
thermals, not GPU power state, not a leak. And `max_accum=1` here too:
capture kept up even while the source stuttered.

The old `6.80 fps / 8.8 s` window was a different thing again — plain idle.
`60 × avg_latency = 1.93 s` against an `8.818 s` window leaves 6.9 s in
which no frame arrived at all. The wall-clock breakdown the spike now
prints separates those two cases directly, which is why it was added.

**Takeaway for the next step (encode)**: the capture stage is not a
bottleneck and has no degradation mechanism to fix. ~98% of the frame
budget is unspent, so there is real room for AMF encode + transport. Note
also that low fps on an idle desktop is the *desired* behaviour, not a
defect — nothing changed means nothing to encode or send.

**Still unmeasured**: whether capture can exceed 60 fps. The primary output
here is 60 Hz, so this run cannot distinguish "60 is the display's ceiling"
from "60 is capture's ceiling". That matters if the iPad's 120 Hz ProMotion
is to be driven, and needs a 120 Hz output (or a 120 Hz virtual display) to
answer.

### Original 2026-08-01 log, kept for reference

The source below was a WinForms window with a 16 ms timer — see above for
why that made the numbers meaningless:

```
[frame     60] fps= 59.93  acquire_latency avg=16.520ms max=29.667ms (window=60 frames / 1.001s)
[frame    120] fps=  6.80  acquire_latency avg=32.100ms max=250.793ms (window=60 frames / 8.818s)
[frame    180] fps= 53.02  acquire_latency avg=18.450ms max=37.200ms (window=60 frames / 1.132s)
[frame    240] fps= 41.40  acquire_latency avg=12.533ms max=35.623ms (window=60 frames / 1.449s)
[frame    300] fps= 37.10  acquire_latency avg=26.671ms max=50.858ms (window=60 frames / 1.617s)

Done. Total frames captured: 340
```

The guess written down at the time — DWM/driver throttling or a Radeon 780M
power-state transition — was wrong on both counts. The `37.10 / 26.671 ms`
line is the timer/refresh beat, and the `6.80 / 8.8 s` line is 6.9 s of
idle desktop. Two unrelated causes, read as one trend.

Also captured on `\\.\DISPLAY2` (3440x1440): output index 0 on this machine
is an ultrawide external monitor, not the laptop panel. See Known gaps.

## Encode spike (`pc_encode_spike`, 2026-08-06)

Capture -> AMF H.264 encode on the same D3D11 device (zero-copy: the desktop
texture is GPU-copied into a pool texture that AMF wraps directly). Every
numeric default is a measured design input from
`.steering/20260803-usb-datalink-probe/result.md`:

| Measured (USB link, 2026-08-06) | Encoder setting |
|---|---|
| 16 KiB writes are the latency sweet spot (p99 2.5 ms @40 Mbps) | `SLICES_PER_FRAME = ceil(frame_budget / 16Ki)` so one slice NAL ~= one transport write |
| 209-249 Mbps effective ceiling | 40 Mbps CBR default (`--bitrate-mbps` up to 80 measured-safe) |
| rare single 15-30 ms hiccups; deep buffering unwanted | VBV = 1 frame of bits; busy encoder => drop frame (never queue); no periodic IDR — intra refresh sweeps the frame over ~1 s |

```powershell
Start-Process .\build\pc_test_source.exe -ArgumentList 35,0
.\build\pc_encode_spike.exe --seconds 25 --output out.h264   # optional: --bitrate-mbps N --display N
```

The run ends with PASS/FAIL lines against the acceptance criteria in
`.steering/20260806-amf-encoder/requirements.md`.

Accepted run on this machine (3440x1440@60, 40 Mbps CBR, test source active):
1501/1501 frames encoded, 0 drops, latency avg 7.6 ms / p99 9.8 ms
(criterion: <=12 ms — the mandatory `SetMultithreadProtected` serialization
costs ~3 ms of p99), bitrate 36.3 Mbps, frame bytes p99 = 1.07x budget,
slice median 13-14 KB, exactly 1 IDR, and `ffprobe -count_frames` decodes
all 1501 frames. Direct BGRA submit works on this runtime (no converter
fallback needed); latency floor is ~5 ms even for near-empty frames.

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

- `src/main.cpp` — the capture spike, heavily commented.
- `src/testsource.cpp` — vsync-paced screen-change generator, the control
  for any capture measurement.
- `src/amf_encoder.h/.cpp` — AMF H.264 encoder wrapper (measured design
  inputs, surface pool, drop-when-busy policy).
- `src/encode_main.cpp` — `pc_encode_spike`: capture loop + drain thread +
  stats + acceptance PASS/FAIL.
- `third_party/AMF/` — vendored AMF v1.5.2 headers (MIT); runtime
  `amfrt64.dll` is loaded dynamically from the AMD driver.
- `CMakeLists.txt` — minimal CMake setup, C++17, links `d3d11`/`dxgi`.
- `build.ps1` — locates VS via `vswhere`, sources `vcvars64.bat`, drives
  VS's bundled CMake+Ninja. `-Run` flag builds and runs in one step.
