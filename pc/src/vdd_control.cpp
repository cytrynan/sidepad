// pc/src/vdd_control.cpp -- see vdd_control.h for the contract.

#include "vdd_control.h"

#include <windows.h>

#include <cstdio>

namespace {

// First match for %LOCALAPPDATA%\Microsoft\WinGet\Packages\<prefix>*.
std::string FindPackageDir() {
    char local[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    const std::string base =
        std::string(local) + "\\Microsoft\\WinGet\\Packages\\";
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(
        (base + "VirtualDrivers.Virtual-Display-Driver_*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string dir;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            dir = base + fd.cFileName;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return dir;
}

}  // namespace

std::string VddControl::FindDevcon() {
    const std::string pkg = FindPackageDir();
    if (pkg.empty()) return {};
    const std::string devcon = pkg + "\\Dependencies\\devcon.exe";
    const DWORD attr = GetFileAttributesA(devcon.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES ||
        (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return {};
    }
    return devcon;
}

bool VddControl::RunDevcon(const char* verb, std::string* detail) {
    if (devcon_path_.empty()) {
        *detail = "devcon.exe not found (VDD winget package missing?)";
        return false;
    }
    // devcon matches "Root\MttVDD" as a hardware id -- the same id the
    // install script created the device with.
    std::string cmdline =
        "\"" + devcon_path_ + "\" " + verb + " \"Root\\MttVDD\"";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    // CreateProcess mutates the command line buffer; cmdline is ours.
    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        *detail = std::string("CreateProcess(devcon ") + verb +
                  ") failed: " + std::to_string(GetLastError());
        return false;
    }
    // devcon enable/disable finishes in well under a second; 15s covers a
    // PnP re-enumeration stall without hanging the daemon forever.
    const DWORD wait = WaitForSingleObject(pi.hProcess, 15'000);
    DWORD exit_code = 1'000;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exit_code);
    } else {
        TerminateProcess(pi.hProcess, 1'001);
        exit_code = 1'001;  // timed out
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "devcon %s Root\\MttVDD -> exit %lu%s",
                  verb, static_cast<unsigned long>(exit_code),
                  exit_code == 1'001 ? " (timeout)" : "");
    *detail = buf;
    // 0 = ok, 1 = ok-with-reboot-flag (devcon's "must restart" answer; the
    // IddCx display still appears/disappears without one in practice).
    return exit_code == 0 || exit_code == 1;
}
