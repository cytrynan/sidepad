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

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int kWaitDisplayTimeoutMs = 5'000;
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
// Matches by current desktop mode 2048x1536 + 120Hz (the sidepad settings in
// C:\VirtualDisplayDriver\vdd_settings.xml), same criterion --list-displays
// prints as TARGET MODE. Index-based on purpose: Pipeline::InitCapture
// enumerates the same default adapter, so indices agree.
int FindVirtualDisplayOutput() {
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
        if (rc.right - rc.left == 2048 && rc.bottom - rc.top == 1536 &&
            DisplayRefreshHz(od.DeviceName) == 120) {
            return static_cast<int>(o);
        }
    }
}

// --- Shared device state (listener thread -> main) ---------------------------

struct DeviceState {
    std::mutex mutex;
    std::condition_variable cv;
    bool attached = false;
    long long device_id = -1;
    std::atomic<bool> quit{false};

    void OnEvent(const UsbmuxDeviceEvent& ev) {
        std::lock_guard<std::mutex> lock(mutex);
        if (ev.attached && ev.usb) {
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

BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        if (g_state) {
            g_state->quit.store(true);
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

// One attach->stream->teardown cycle. Returns true when the session reached
// STREAMING and ended normally (detach / --session-seconds), false on any
// failure before or during streaming (caller backs off and retries).
bool RunSession(const Options& opt, DeviceState& state, VddControl& vdd,
                Logger& log, FILE* csv, Clock::time_point attach_time) {
    std::string detail;
    if (!vdd.Enable(&detail)) {
        log.Line("WARN vdd enable failed (%s) -- continuing, display may "
                 "already be up", detail.c_str());
    } else {
        log.Line("STATE vdd_on (%s)", detail.c_str());
    }

    int display = -1;
    for (int waited = 0; waited <= kWaitDisplayTimeoutMs;
         waited += kWaitDisplayPollMs) {
        display = FindVirtualDisplayOutput();
        if (display >= 0 || state.quit.load()) break;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kWaitDisplayPollMs));
    }
    if (display < 0) {
        log.Line("FAIL wait_display: no 2048x1536@120 output within %dms",
                 kWaitDisplayTimeoutMs);
        return false;
    }
    log.Line("STATE wait_display ok: output %d", display);

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
    while (true) {
        if (pipeline.stopping()) break;  // send failure or capture loss
        if (state.quit.load()) {
            ended_normally = true;
            break;
        }
        if (!state.IsAttached()) {
            log.Line("EVENT detached (usbmux)");
            ended_normally = true;
            break;
        }
        const auto now = Clock::now();
        if (opt.session_seconds > 0 &&
            now - stream_start >= std::chrono::seconds(opt.session_seconds)) {
            ended_normally = true;
            break;
        }
        if (now >= next_stats && csv) {
            const long long frames = pipeline.encoded_frames();
            const double dt =
                std::chrono::duration<double>(now - last_stats_time).count();
            const double fps = (frames - last_frames) / (dt > 0 ? dt : 1);
            const double p99 =
                PercentileOf(pipeline.TakeLatencyWindow(), 0.99);
            SYSTEMTIME st;
            GetLocalTime(&st);
            std::fprintf(csv,
                         "%04u-%02u-%02uT%02u:%02u:%02u,%.2f,%lld,%.2f,%.1f,"
                         "%lld,%lld\n",
                         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                         st.wSecond, fps, frames, p99, WorkingSetMb(),
                         pipeline.encoder().input_full_drops(),
                         pipeline.encoder().pool_starved_drops());
            std::fflush(csv);
            last_frames = frames;
            last_stats_time = now;
            next_stats = now + stats_period;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

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

    if (!vdd.Disable(&detail)) {
        log.Line("WARN vdd disable failed (%s)", detail.c_str());
    } else {
        log.Line("STATE vdd_off (%s)", detail.c_str());
    }

    // A session that died instantly (e.g. encoder loss loop) must not count
    // as healthy even if it ended via detach bookkeeping.
    return ended_normally ||
           streamed_s * 1000.0 >= static_cast<double>(kHealthySessionMs);
}

}  // namespace

int main(int argc, char** argv) {
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

    const std::string dir = LocalAppDataDir();
    CreateDirectoryA(dir.c_str(), nullptr);
    Logger log;
    if (!log.Open(dir + "\\sidepadd.log")) {
        std::fprintf(stderr, "[fatal] cannot open log in %s\n", dir.c_str());
        return 1;
    }
    const std::string csv_path = dir + "\\stats.csv";
    const bool csv_new =
        GetFileAttributesA(csv_path.c_str()) == INVALID_FILE_ATTRIBUTES;
    FILE* csv = std::fopen(csv_path.c_str(), "ab");
    if (csv && csv_new) {
        std::fprintf(csv,
                     "time,fps,frames_total,lat_p99_ms,working_set_mb,"
                     "drops_input,drops_pool\n");
        std::fflush(csv);
    }

    DeviceState state;
    g_state = &state;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    VddControl vdd(VddControl::FindDevcon());
    log.Line("sidepadd start (devcon: %s)",
             vdd.available() ? "found" : "NOT FOUND -- vdd toggle disabled");

    UsbmuxListener listener;
    std::string err;
    if (!listener.Start([&](const UsbmuxDeviceEvent& ev) { state.OnEvent(ev); },
                        &err)) {
        log.Line("FATAL usbmux listener: %s", err.c_str());
        return 1;
    }
    log.Line("STATE idle: waiting for iPad over USB (usbmux Listen)");

    int backoff_ms = 1'000;
    while (!state.quit.load()) {
        // IDLE: wait for an attach (or quit).
        Clock::time_point attach_time;
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.cv.wait(lock,
                          [&] { return state.attached || state.quit.load(); });
            if (state.quit.load()) break;
            attach_time = Clock::now();
            log.Line("EVENT attached: DeviceID=%lld (USB)", state.device_id);
        }

        const bool healthy = RunSession(opt, state, vdd, log, csv, attach_time);
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

    listener.Stop();
    if (csv) std::fclose(csv);
    log.Line("sidepadd exit");
    return 0;
}
