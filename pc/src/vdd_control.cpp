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

bool VddControl::RunDevcon(const char* verb, const char* success_marker,
                           std::string* detail) {
    if (devcon_path_.empty()) {
        *detail = "devcon.exe not found (VDD winget package missing?)";
        return false;
    }
    // devcon matches "Root\MttVDD" as a hardware id -- the same id the
    // install script created the device with.
    std::string cmdline =
        "\"" + devcon_path_ + "\" " + verb + " \"Root\\MttVDD\"";

    // Capture stdout: the per-device result line is the only reliable
    // success signal (see header). The pipe's write end must be inheritable
    // and closed on our side before draining, or ReadFile never sees EOF.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        *detail = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    PROCESS_INFORMATION pi{};
    // CreateProcess mutates the command line buffer; cmdline is ours.
    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        *detail = std::string("CreateProcess(devcon ") + verb +
                  ") failed: " + std::to_string(GetLastError());
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return false;
    }
    CloseHandle(write_pipe);

    std::string output;
    char buf[512];
    DWORD got = 0;
    while (ReadFile(read_pipe, buf, sizeof(buf), &got, nullptr) && got > 0) {
        output.append(buf, got);
    }
    CloseHandle(read_pipe);

    // Output drained to EOF means devcon already exited (or closed stdout);
    // the wait is just cleanup. 15s covers a PnP re-enumeration stall.
    WaitForSingleObject(pi.hProcess, 15'000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Condense the output to its meaningful line for the log.
    for (char& c : output) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    while (!output.empty() && output.back() == ' ') output.pop_back();
    *detail = "devcon " + std::string(verb) + ": " +
              (output.empty() ? "(no output)" : output);

    return output.find(success_marker) != std::string::npos;
}
