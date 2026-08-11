// pc/src/daemon_main.cpp
//
// sidepadd: resident daemon that turns the iPad into a plug-and-play
// extended display (.steering/20260808-v1-resident-ops/design.md).
//
//   IDLE --(usbmux Attached, USB)--> VDD_ON --> WAIT_DISPLAY --> STREAMING
//   STREAMING --(Detached / send failure / capture loss)--> TEARDOWN --> IDLE
//
// Every failure path backs off (1s -> 2s -> ... capped at 10s) and retries
// while the device stays attached -- e.g. the Sidepad app not yet open only
// delays streaming, it doesn't kill the daemon. All transitions go to the
// log file; while STREAMING a stats line is appended to the CSV every 60s
// (the 4-hour soak evidence, .steering acceptance).
//
// Usage:
//   sidepadd [--once] [--session-seconds N] [--bitrate-mbps N]
//
//   --once             exit after the first session ends (testing/acceptance)
//   --session-seconds  end the session after N seconds (0 = until detach)
//   --bitrate-mbps     default 40 (the measured-safe CBR)
//
// Logs:  %LOCALAPPDATA%\sidepad\sidepadd.log
// Stats: %LOCALAPPDATA%\sidepad\stats.csv
//
// Runs elevated via the "sidepadd" scheduled task (pc/daemon/install-task.ps1);
// unelevated the VDD toggle fails and the daemon still streams if the virtual
// display is already enabled (see vdd_control.h).

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <powrprof.h>  // PowerRegisterSuspendResumeNotification
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "pipeline.h"
#include "usbmux_events.h"
#include "vdd_control.h"
#include "vdd_mode.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "powrprof.lib")

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

namespace {

// 15s, not 5: a disable->enable cycle takes a measured 10-13s before the
// 2048x1536@120 mode is back (PnP re-enumeration; review finding 10).
constexpr int kWaitDisplayTimeoutMs = 15'000;
constexpr int kWaitDisplayPollMs = 100;
constexpr int kBackoffMaxMs = 10'000;
// A session that streamed at least this long counts as healthy and resets
// the retry backoff.
constexpr int kHealthySessionMs = 30'000;

// --- Logging -----------------------------------------------------------------

class Logger {
public:
    bool Open(const std::string& path) {
        file_ = std::fopen(path.c_str(), "ab");
        return file_ != nullptr;
    }
    // printf-style; adds a wall-clock timestamp, mirrors to stdout, flushes
    // (the log must survive a crash/kill -- it is the acceptance evidence).
    void Line(const char* fmt, ...) {
        char msg[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        SYSTEMTIME st;
        GetLocalTime(&st);
        char stamp[40];
        std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                      st.wSecond, st.wMilliseconds);
        std::printf("[%s] %s\n", stamp, msg);
        if (file_) {
            std::fprintf(file_, "[%s] %s\n", stamp, msg);
            std::fflush(file_);
        }
    }
    ~Logger() {
        if (file_) std::fclose(file_);
    }

private:
    FILE* file_ = nullptr;
};

std::string LocalAppDataDir() {
    char buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    return std::string(buf) + "\\sidepad";
}

// --- Virtual display lookup --------------------------------------------------

// Output index of the VDD virtual display on the default adapter, or -1.
// Matches by current desktop mode (native 2732x2048 or fallback 2048x1536,
// both at 120Hz -- vdd_mode.h), same criterion --list-displays prints as
// NATIVE/FALLBACK MODE. Index-based on purpose: Pipeline::InitCapture
// enumerates the same default adapter, so indices agree.
bool IsVddMode(int w, int h, int hz) {
    if (hz != kVddRefreshHz) return false;
    return (w == kVddNativeWidth && h == kVddNativeHeight) ||
           (w == kVddFallbackWidth && h == kVddFallbackHeight);
}

// Switch the virtual display to its native mode after every enable.
// Windows remembers the last mode per monitor, so the VDD comes back at
// whatever it used previously (2048x1536 during the spike era); mode
// changes need no elevation but must run in the interactive session, which
// the daemon does. Identification: the only non-primary desktop-attached
// device whose mode list contains the native mode (the internal panel and
// common externals don't list 2732x2048).
//
// MUST be called after the display is already enumerable (wait_display):
// a cold devcon enable takes a measured 10-13s of PnP re-enumeration, so
// any polling budget here that is shorter than kWaitDisplayTimeoutMs would
// structurally time out on the plug-and-play path and silently leave the
// fallback mode in place (review 2026-08-11 A-1). The short poll below
// only bridges GDI/DXGI enumeration skew, not device bring-up.
// Returns true when it issued a successful mode change.
bool EnsureVddNativeMode(Logger& log) {
    for (int waited = 0; waited <= 3'000; waited += 200) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
            if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
            if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) continue;
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            bool supports_native = false;
            for (DWORD m = 0; EnumDisplaySettingsW(dd.DeviceName, m, &dm); ++m) {
                if (dm.dmPelsWidth == kVddNativeWidth &&
                    dm.dmPelsHeight == kVddNativeHeight &&
                    dm.dmDisplayFrequency == kVddRefreshHz) {
                    supports_native = true;
                    break;
                }
            }
            if (!supports_native) continue;

            DEVMODEW cur{};
            cur.dmSize = sizeof(cur);
            if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS,
                                     &cur) &&
                cur.dmPelsWidth == kVddNativeWidth &&
                cur.dmPelsHeight == kVddNativeHeight &&
                cur.dmDisplayFrequency == kVddRefreshHz) {
                return false;  // already native
            }
            DEVMODEW want{};
            want.dmSize = sizeof(want);
            want.dmPelsWidth = kVddNativeWidth;
            want.dmPelsHeight = kVddNativeHeight;
            want.dmDisplayFrequency = kVddRefreshHz;
            want.dmFields =
                DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
            const LONG r = ChangeDisplaySettingsExW(
                dd.DeviceName, &want, nullptr, CDS_UPDATEREGISTRY, nullptr);
            if (r == DISP_CHANGE_SUCCESSFUL) {
                log.Line("MODE %dx%d@%d set on %ls", kVddNativeWidth,
                         kVddNativeHeight, kVddRefreshHz, dd.DeviceName);
                return true;
            }
            log.Line("WARN native mode change failed (%ld); staying at "
                     "%ux%u@%u", static_cast<long>(r), cur.dmPelsWidth,
                     cur.dmPelsHeight, cur.dmDisplayFrequency);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    log.Line("WARN no display advertising %dx%d@%d; keeping fallback mode",
             kVddNativeWidth, kVddNativeHeight, kVddRefreshHz);
    return false;
}

// `out_width`, when non-null, receives the matched output's current desktop
// width so callers can tell native (2732) from fallback (2048) apart.
int FindVirtualDisplayOutput(int* out_width = nullptr) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
        return -1;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(0, adapter.GetAddressOf()) ==
        DXGI_ERROR_NOT_FOUND) {
        return -1;
    }
    for (UINT o = 0;; ++o) {
        ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(o, output.GetAddressOf()) ==
            DXGI_ERROR_NOT_FOUND) {
            return -1;
        }
        DXGI_OUTPUT_DESC od{};
        if (FAILED(output->GetDesc(&od)) || !od.AttachedToDesktop) continue;
        const auto& rc = od.DesktopCoordinates;
        const int w = static_cast<int>(rc.right - rc.left);
        if (IsVddMode(w, static_cast<int>(rc.bottom - rc.top),
                      DisplayRefreshHz(od.DeviceName))) {
            if (out_width) *out_width = w;
            return static_cast<int>(o);
        }
    }
}

// --- Shared device state (listener thread -> main) ---------------------------

struct DeviceState {
    std::mutex mutex;
    std::condition_variable cv;
    // Set once at startup, before the listener thread exists. The listener
    // thread logs event receipt itself so a wedged main loop can no longer
    // hide whether usbmux events were delivered (2026-08-09 wedge lesson).
    Logger* logger = nullptr;
    bool attached = false;
    // Set only on the false->true transition, consumed by the main loop:
    // session retries with the cable left in must neither re-log
    // "EVENT attached" nor reset attach_time, or the attach->stream KPI
    // reads a fake 0.2s instead of the real recovery time (review finding 2).
    bool pending_attach_log = false;
    Clock::time_point attach_time{};
    long long device_id = -1;
    std::atomic<bool> quit{false};
    // Set by the power-notification callback (an OS thread). While true, a
    // running session tears down and no new session starts; the resume
    // notification (or the USB re-attach after resume) wakes the main loop.
    std::atomic<bool> suspended{false};

    void OnEvent(const UsbmuxDeviceEvent& ev) {
        std::lock_guard<std::mutex> lock(mutex);
        if (logger) {
            logger->Line("EVENT usbmux(listener): %s DeviceID=%lld%s",
                         ev.attached ? "attached" : "detached", ev.device_id,
                         ev.usb ? " (USB)" : "");
        }
        if (ev.attached && ev.usb) {
            if (!attached) {
                attach_time = Clock::now();
                pending_attach_log = true;
            }
            attached = true;
            device_id = ev.device_id;
        } else if (!ev.attached && ev.device_id == device_id) {
            attached = false;
        } else {
            return;  // Wi-Fi entry of the same iPad, or unrelated device
        }
        cv.notify_all();
    }
    bool IsAttached() {
        std::lock_guard<std::mutex> lock(mutex);
        return attached;
    }
};

DeviceState* g_state = nullptr;  // for the console ctrl handler only

// DEVICE_NOTIFY_CALLBACK: no window needed (sidepadd has no message loop).
// Runs on a system thread; only flips flags. The flag stores happen under
// state->mutex even though they are atomics: the main loop's cv.wait
// predicate reads them, and an unlocked store+notify can slip into the
// window between predicate evaluation and wait registration -- a lost
// wakeup that leaves the daemon hung in IDLE (review finding 3).
ULONG CALLBACK SuspendResumeCallback(PVOID context, ULONG type, PVOID) {
    auto* state = static_cast<DeviceState*>(context);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (type == PBT_APMSUSPEND) {
            state->suspended.store(true);
        } else if (type == PBT_APMRESUMEAUTOMATIC ||
                   type == PBT_APMRESUMESUSPEND) {
            state->suspended.store(false);
        } else {
            return 0;
        }
    }
    state->cv.notify_all();
    return 0;
}

BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        if (g_state) {
            {
                // Same lost-wakeup guard as SuspendResumeCallback.
                std::lock_guard<std::mutex> lock(g_state->mutex);
                g_state->quit.store(true);
            }
            g_state->cv.notify_all();
        }
        return TRUE;
    }
    return FALSE;
}

double PercentileOf(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(p * (v.size() - 1))];
}

double WorkingSetMb() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return pmc.WorkingSetSize / (1024.0 * 1024.0);
}

struct Options {
    bool once = false;
    int session_seconds = 0;
    int stats_seconds = 60;  // CSV cadence; short values are for testing
    long long bitrate_bps = 40'000'000;
};

// Owns the virtual display ACROSS sessions (formerly a per-session RAII).
// Rapid replug cycles used to devcon-toggle the VDD once per cycle; under
// that churn PnP occasionally defers the transition ("Disabled on reboot")
// and completes it asynchronously mid-capture -- observed 2026-08-10 in the
// replug acceptance as ACCESS_LOST plus 21-23s recoveries. The keeper
// (a) reuses an already-lit display for in-session retries and quick
// replugs, and (b) lingers kVddLingerMs after detach before turning the
// display off, so a replug inside the window never touches devcon.
// The ghost-monitor guarantee (review finding 1) moves to the main loop:
// every path into IDLE schedules the linger-off, and quit turns it off
// immediately. The functional display check stays the primary verdict
// (devcon's exit code lies).
constexpr int kVddLingerMs = 10'000;

class VddKeeper {
public:
    VddKeeper(VddControl& vdd, Logger& log) : vdd_(vdd), log_(log) {}

    // Session start. Idempotent: skips devcon when the display is already
    // up (in-session retry, or a re-attach inside the linger window).
    void EnsureOn() {
        linger_armed_ = false;
        if (FindVirtualDisplayOutput() >= 0) {
            log_.Line("STATE vdd_on (reused: display already up)");
            on_ = true;
            return;
        }
        std::string detail;
        if (!vdd_.Enable(&detail)) {
            log_.Line("WARN vdd enable failed (%s) -- continuing, display "
                      "may already be up", detail.c_str());
        } else {
            log_.Line("STATE vdd_on (%s)", detail.c_str());
        }
        on_ = true;
    }

    // Detach path: keep the display lit for the grace period.
    void ArmLingerOff() {
        if (!on_ || linger_armed_) return;
        linger_armed_ = true;
        linger_deadline_ = Clock::now() + std::chrono::milliseconds(kVddLingerMs);
        log_.Line("STATE vdd linger %dms (reused if re-attached)", kVddLingerMs);
    }

    bool lingering() const { return on_ && linger_armed_; }
    bool linger_expired() const {
        return lingering() && Clock::now() >= linger_deadline_;
    }

    // Immediate off (linger expiry, quit). Never call with state.mutex held:
    // devcon takes seconds and would stall the usbmux listener callback.
    void Off() {
        linger_armed_ = false;
        if (!on_) return;
        on_ = false;
        std::string detail;
        vdd_.Disable(&detail);
        bool display_gone = false;
        for (int waited = 0; waited <= 2'000; waited += 200) {
            if (FindVirtualDisplayOutput() < 0) {
                display_gone = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (display_gone) {
            log_.Line("STATE vdd_off (display detached; %s)", detail.c_str());
        } else {
            log_.Line("WARN vdd_off incomplete: display STILL ATTACHED -- "
                      "needs elevation? (%s)", detail.c_str());
        }
    }

    VddKeeper(const VddKeeper&) = delete;
    VddKeeper& operator=(const VddKeeper&) = delete;

private:
    VddControl& vdd_;
    Logger& log_;
    bool on_ = false;
    bool linger_armed_ = false;
    Clock::time_point linger_deadline_{};
};

// One attach->stream->teardown cycle. Returns true when the session reached
// STREAMING and ended normally (detach / --session-seconds), false on any
// failure before or during streaming (caller backs off and retries).
bool RunSession(const Options& opt, DeviceState& state, VddKeeper& vdd_keeper,
                Logger& log, FILE* csv, Clock::time_point attach_time) {
    vdd_keeper.EnsureOn();

    int display = -1;
    for (int waited = 0; waited <= kWaitDisplayTimeoutMs;
         waited += kWaitDisplayPollMs) {
        display = FindVirtualDisplayOutput();
        if (display >= 0 || state.quit.load()) break;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kWaitDisplayPollMs));
    }
    if (display < 0) {
        log.Line("FAIL wait_display: no %dx%d/%dx%d@%d output within %dms",
                 kVddNativeWidth, kVddNativeHeight, kVddFallbackWidth,
                 kVddFallbackHeight, kVddRefreshHz, kWaitDisplayTimeoutMs);
        // Dump what DXGI actually sees so a GDI/DXGI disagreement (e.g. the
        // 2026-08-10 native-mode switch) is diagnosable from the log alone.
        {
            ComPtr<IDXGIFactory1> factory;
            ComPtr<IDXGIAdapter1> adapter;
            if (SUCCEEDED(CreateDXGIFactory1(
                    IID_PPV_ARGS(factory.GetAddressOf()))) &&
                factory->EnumAdapters1(0, adapter.GetAddressOf()) !=
                    DXGI_ERROR_NOT_FOUND) {
                for (UINT o = 0;; ++o) {
                    ComPtr<IDXGIOutput> output;
                    if (adapter->EnumOutputs(o, output.GetAddressOf()) ==
                        DXGI_ERROR_NOT_FOUND) {
                        break;
                    }
                    DXGI_OUTPUT_DESC od{};
                    if (FAILED(output->GetDesc(&od))) continue;
                    const auto& rc = od.DesktopCoordinates;
                    log.Line("  dxgi output %u: %ls %ldx%ld@%d attached=%d", o,
                             od.DeviceName, rc.right - rc.left,
                             rc.bottom - rc.top,
                             DisplayRefreshHz(od.DeviceName),
                             od.AttachedToDesktop ? 1 : 0);
                }
            }
        }
        return false;
    }
    log.Line("STATE wait_display ok: output %d", display);

    // Self-heal to native res now that the display is enumerable (calling
    // this before wait_display would always time out on a cold devcon
    // enable -- 10-13s PnP bring-up vs its 3s poll; review 2026-08-11 A-1).
    // A successful mode change alters the output's desktop mode, so re-find
    // the output and wait until DXGI reports the native width before
    // capturing (a capture opened mid-switch would see the stale mode).
    if (EnsureVddNativeMode(log)) {
        int w = 0;
        for (int waited = 0; waited <= 3'000; waited += kWaitDisplayPollMs) {
            const int d = FindVirtualDisplayOutput(&w);
            if (d >= 0 && w == kVddNativeWidth) {
                display = d;
                break;
            }
            if (state.quit.load()) break;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kWaitDisplayPollMs));
        }
        if (w != kVddNativeWidth) {
            log.Line("WARN native mode set but DXGI still reports %d wide; "
                     "capturing as-is", w);
        }
    }

    Pipeline pipeline;
    std::string err;
    if (!pipeline.InitCapture(static_cast<UINT>(display), &err)) {
        log.Line("FAIL init_capture: %s", err.c_str());
        return false;
    }
    if (!pipeline.InitEncoder(opt.bitrate_bps, &err)) {
        log.Line("FAIL init_encoder: %s", err.c_str());
        return false;
    }
    if (!pipeline.InitOutputs("", "usb", &err)) {
        log.Line("FAIL init_outputs: %s", err.c_str());  // app not open yet etc.
        return false;
    }
    // The CLI-only full series (latency/frame/slice vectors) would grow
    // ~25MB/hour at 120fps in a daemon that never reads them (finding 4).
    pipeline.DisableFullSeries();

    pipeline.Start();
    const auto stream_start = Clock::now();
    const double attach_to_stream_s =
        std::chrono::duration<double>(stream_start - attach_time).count();
    log.Line("STATE streaming: display=%d %dx%d@%d %lldbps "
             "(%.1fs after attach)",
             display, pipeline.width(), pipeline.height(), pipeline.fps(),
             pipeline.bitrate_bps(), attach_to_stream_s);

    // Stream until detach, quit, session limit, or pipeline death. The 60s
    // CSV cadence doubles as the soak evidence.
    const auto stats_period = std::chrono::seconds(opt.stats_seconds);
    auto next_stats = stream_start + stats_period;
    long long last_frames = 0;
    auto last_stats_time = stream_start;
    bool ended_normally = false;
    const char* stop_reason = "unknown";
    while (true) {
        if (pipeline.stopping()) {  // send failure or capture loss
            stop_reason = "pipeline";
            break;
        }
        if (state.quit.load()) {
            stop_reason = "quit";
            ended_normally = true;
            break;
        }
        if (state.suspended.load()) {
            log.Line("EVENT suspend: tearing down before sleep");
            stop_reason = "suspend";
            ended_normally = true;
            break;
        }
        if (!state.IsAttached()) {
            log.Line("EVENT detached (usbmux)");
            stop_reason = "detach";
            ended_normally = true;
            break;
        }
        const auto now = Clock::now();
        if (opt.session_seconds > 0 &&
            now - stream_start >= std::chrono::seconds(opt.session_seconds)) {
            stop_reason = "session-limit";
            ended_normally = true;
            break;
        }
        if (now >= next_stats) {
            // Drain the latency window even when the CSV is unavailable, or
            // it grows ~7MB/hour unread (review finding 5).
            const double p99 =
                PercentileOf(pipeline.TakeLatencyWindow(), 0.99);
            const long long frames = pipeline.encoded_frames();
            const double dt =
                std::chrono::duration<double>(now - last_stats_time).count();
            const double fps = (frames - last_frames) / (dt > 0 ? dt : 1);
            if (csv) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                std::fprintf(csv,
                             "%04u-%02u-%02uT%02u:%02u:%02u,%.2f,%lld,%.2f,"
                             "%.1f,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
                             st.wYear, st.wMonth, st.wDay, st.wHour,
                             st.wMinute, st.wSecond, fps, frames, p99,
                             WorkingSetMb(),
                             pipeline.encoder().input_full_drops(),
                             pipeline.encoder().pool_starved_drops(),
                             pipeline.cursor_only_frames(),
                             pipeline.cursor_drawn_frames(),
                             pipeline.touch_moves(), pipeline.touch_taps(),
                             pipeline.touch_scrolls(),
                             pipeline.touch_inject_errors());
                std::fflush(csv);
            }
            last_frames = frames;
            last_stats_time = now;
            next_stats = now + stats_period;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Logged before Stop() on purpose: if a join inside Stop() ever wedges,
    // this line localizes the hang to Stop() instead of leaving the log
    // ending at "STATE streaming" with no clue (2026-08-09 wedge lesson).
    log.Line("STATE stopping (%s); joining pipeline", stop_reason);
    pipeline.Stop();
    const double streamed_s =
        std::chrono::duration<double>(Clock::now() - stream_start).count();
    if (!pipeline.fatal_error().empty()) {
        log.Line("STATE teardown after %.1fs: %s", streamed_s,
                 pipeline.fatal_error().c_str());
    } else if (pipeline.send_failed()) {
        log.Line("STATE teardown after %.1fs: transport send failed "
                 "(cable pulled / app closed)", streamed_s);
    } else {
        log.Line("STATE teardown after %.1fs: %lld frames sent", streamed_s,
                 pipeline.sent_frames());
    }

    // The VDD turn-off now happens in the main loop (linger on IDLE entry,
    // immediate on quit) -- see VddKeeper.

    // A session that died instantly (e.g. encoder loss loop) must not count
    // as healthy even if it ended via detach bookkeeping.
    return ended_normally ||
           streamed_s * 1000.0 >= static_cast<double>(kHealthySessionMs);
}

}  // namespace

int main(int argc, char** argv) {
    // Physical pixels everywhere. Without this the process is DPI-virtualized
    // and DXGI DesktopCoordinates come back scaled (2732x2048 with the auto-
    // assigned 150% scaling showed as 1821x1365, so wait_display could never
    // match the native mode -- 2026-08-10). GDI mode queries were unaffected,
    // which is why the mismatch only appeared on the DXGI side.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    setvbuf(stdout, nullptr, _IONBF, 0);
    Options opt;
    for (int i = 1; i < argc; ++i) {
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[fatal] %s needs a value\n", flag);
                std::exit(1);
            }
            return argv[++i];
        };
        if (std::strcmp(argv[i], "--once") == 0) {
            opt.once = true;
        } else if (std::strcmp(argv[i], "--session-seconds") == 0) {
            opt.session_seconds = std::atoi(need_value("--session-seconds"));
        } else if (std::strcmp(argv[i], "--stats-seconds") == 0) {
            opt.stats_seconds = std::atoi(need_value("--stats-seconds"));
            if (opt.stats_seconds < 1) opt.stats_seconds = 60;
        } else if (std::strcmp(argv[i], "--bitrate-mbps") == 0) {
            opt.bitrate_bps =
                1'000'000LL * std::atoll(need_value("--bitrate-mbps"));
        } else {
            std::fprintf(stderr, "[fatal] unknown arg: %s\n", argv[i]);
            return 1;
        }
    }

    // Single-instance guard: two sidepadds fight over the VDD toggle and
    // interleave the same log/CSV (review finding 9). Handle leaks until
    // process exit by design.
    CreateMutexA(nullptr, TRUE, "Local\\sidepadd-singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::fprintf(stderr, "[fatal] sidepadd is already running\n");
        return 1;
    }

    const std::string dir = LocalAppDataDir();
    CreateDirectoryA(dir.c_str(), nullptr);
    Logger log;
    if (!log.Open(dir + "\\sidepadd.log")) {
        std::fprintf(stderr, "[fatal] cannot open log in %s\n", dir.c_str());
        return 1;
    }
    const std::string csv_path = dir + "\\stats.csv";
    static const char kCsvHeader[] =
        "time,fps,frames_total,lat_p99_ms,working_set_mb,"
        "drops_input,drops_pool,cursor_only_total,cursor_drawn_total,"
        "touch_moves,touch_taps,touch_scrolls,inject_errors\n";
    // Import-Csv maps values to the FILE's first line, so appending wider
    // rows to a file with an older header would silently drop the new
    // columns. Rotate instead of mixing schemas.
    bool csv_new =
        GetFileAttributesA(csv_path.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (!csv_new) {
        std::string first_line;
        if (FILE* f = std::fopen(csv_path.c_str(), "rb")) {
            char buf[256] = {};
            if (std::fgets(buf, sizeof(buf), f)) first_line = buf;
            std::fclose(f);
        }
        if (first_line != kCsvHeader) {
            const std::string rotated = dir + "\\stats-oldheader.csv";
            std::remove(rotated.c_str());
            if (std::rename(csv_path.c_str(), rotated.c_str()) == 0) {
                log.Line("EVENT stats.csv rotated to stats-oldheader.csv "
                         "(header changed)");
                csv_new = true;
            } else {
                log.Line("WARN stats.csv header is stale and rotation "
                         "failed; new columns will be dropped by Import-Csv");
            }
        }
    }
    FILE* csv = std::fopen(csv_path.c_str(), "ab");
    if (!csv) {
        // Not fatal, but the soak evidence silently disappearing is
        // (review finding 5): make it loud.
        log.Line("WARN stats.csv open failed (%s) -- no soak evidence",
                 csv_path.c_str());
    }
    if (csv && csv_new) {
        std::fprintf(csv, "%s", kCsvHeader);
        std::fflush(csv);
    }

    DeviceState state;
    state.logger = &log;
    g_state = &state;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS power_params{};
    power_params.Callback = SuspendResumeCallback;
    power_params.Context = &state;
    HPOWERNOTIFY power_notify = nullptr;
    if (PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK,
                                               &power_params,
                                               &power_notify) != ERROR_SUCCESS) {
        power_notify = nullptr;  // degraded: sleep looks like a USB detach
    }

    VddControl vdd(VddControl::FindDevcon());
    log.Line("sidepadd start (devcon: %s)",
             vdd.available() ? "found" : "NOT FOUND -- vdd toggle disabled");

    UsbmuxListener listener;
    std::string err;
    if (!listener.Start([&](const UsbmuxDeviceEvent& ev) { state.OnEvent(ev); },
                        &err)) {
        log.Line("FATAL usbmux listener: %s", err.c_str());
        // Unregister before `state` goes out of scope, or the power
        // callback can fire into a destroyed object (review, optional 2).
        if (power_notify) PowerUnregisterSuspendResumeNotification(power_notify);
        return 1;
    }
    log.Line("STATE idle: waiting for iPad over USB (usbmux Listen)");

    VddKeeper vdd_keeper(vdd, log);
    int backoff_ms = 1'000;
    while (!state.quit.load()) {
        // Entering IDLE with the display still lit: schedule the linger-off.
        // (A re-attach before expiry reuses the display; EnsureOn disarms.)
        vdd_keeper.ArmLingerOff();

        // IDLE: wait for an attach (or quit). While a linger is armed the
        // wait polls so the off can fire without an event; afterwards it
        // falls back to the predicate wait (lost-wakeup-safe, finding 3).
        Clock::time_point attach_time;
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            for (;;) {
                if ((state.attached && !state.suspended.load()) ||
                    state.quit.load()) {
                    break;
                }
                if (vdd_keeper.lingering()) {
                    state.cv.wait_for(lock, std::chrono::milliseconds(250));
                    if (vdd_keeper.linger_expired()) {
                        lock.unlock();  // devcon takes seconds; don't stall OnEvent
                        vdd_keeper.Off();
                        lock.lock();
                    }
                } else {
                    state.cv.wait(lock, [&] {
                        return (state.attached && !state.suspended.load()) ||
                               state.quit.load();
                    });
                }
            }
            if (state.quit.load()) break;
            // Physical attach moment, not "this retry's start" -- the
            // attach->stream KPI must include failed attempts (finding 2).
            attach_time = state.attach_time;
            if (state.pending_attach_log) {
                state.pending_attach_log = false;
                log.Line("EVENT attached: DeviceID=%lld (USB)",
                         state.device_id);
            }
        }

        const bool healthy =
            RunSession(opt, state, vdd_keeper, log, csv, attach_time);
        if (opt.once) break;
        if (healthy) {
            backoff_ms = 1'000;
            continue;  // straight back to IDLE; re-attach is a fresh cycle
        }
        // Failure: retry while the device is still attached, else IDLE waits.
        if (state.IsAttached() && !state.quit.load()) {
            log.Line("STATE backoff %dms then retry (device still attached)",
                     backoff_ms);
            for (int waited = 0; waited < backoff_ms && !state.quit.load();
                 waited += 100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            backoff_ms = std::min(backoff_ms * 2, kBackoffMaxMs);
        }
    }

    vdd_keeper.Off();  // quit: no linger, restore the monitor set now
    if (power_notify) PowerUnregisterSuspendResumeNotification(power_notify);
    listener.Stop();
    if (csv) std::fclose(csv);
    log.Line("sidepadd exit");
    return 0;
}
