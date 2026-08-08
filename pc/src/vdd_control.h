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

    // devcon enable/disable "Root\MttVDD". Returns true on exit code 0 or 1
    // (1 = done with reboot flag; the display still toggles). *detail gets a
    // one-line description for the log either way.
    bool Enable(std::string* detail) { return RunDevcon("enable", detail); }
    bool Disable(std::string* detail) { return RunDevcon("disable", detail); }

private:
    bool RunDevcon(const char* verb, std::string* detail);

    std::string devcon_path_;
};
