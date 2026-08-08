// pc/src/vdd_control.h
//
// Enable/disable the VDD virtual display device (hwid Root\MttVDD) via the
// devcon.exe that ships inside the winget package -- the same tool
// pc/vdd/install-vdd-driver.ps1 used to create the device. Both operations
// need administrator rights (the sidepadd scheduled task runs elevated;
// see pc/daemon/install-task.ps1).
//
// Callers treat Enable/Disable as best-effort: a failed Enable is followed
// by the display-wait step anyway (the device may already be enabled, e.g.
// when testing unelevated), and a failed Disable only leaves the virtual
// display on -- logged, not fatal.

#pragma once

#include <string>

class VddControl {
public:
    // Locates devcon.exe under
    //   %LOCALAPPDATA%\Microsoft\WinGet\Packages\
    //     VirtualDrivers.Virtual-Display-Driver_*\Dependencies\devcon.exe
    // Empty string when not found (VDD package missing).
    static std::string FindDevcon();

    explicit VddControl(std::string devcon_path)
        : devcon_path_(std::move(devcon_path)) {}

    bool available() const { return !devcon_path_.empty(); }

    // devcon enable/disable "Root\MttVDD". Success is judged from devcon's
    // OUTPUT, not its exit code: devcon exits 0 even when it prints
    // "Disable failed" (measured 2026-08-08, unelevated). The per-device
    // result line (": Enabled" / ": Disabled") is the only trustworthy
    // signal. *detail gets the relevant output line for the log.
    bool Enable(std::string* detail) {
        return RunDevcon("enable", ": Enabled", detail);
    }
    bool Disable(std::string* detail) {
        return RunDevcon("disable", ": Disabled", detail);
    }

private:
    bool RunDevcon(const char* verb, const char* success_marker,
                   std::string* detail);

    std::string devcon_path_;
};
