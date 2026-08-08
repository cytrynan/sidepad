// pc/src/usbmux_events.h
//
// Device attach/detach events from usbmuxd (AMDS on 127.0.0.1:27015) via the
// usbmux Listen command. usbmuxd announces every already-connected device as
// Attached right after Listen succeeds, so a daemon started with the iPad
// plugged in still receives its event -- no separate ListDevices poll needed.
//
// The listener owns one background thread. If the connection to AMDS drops
// (service restart), it keeps retrying every 2s until Stop(); each successful
// re-Listen re-announces attached devices, which the daemon treats as a
// fresh attach.
//
// The callback runs on the listener thread; do only cheap work there (post
// to a queue / flip a flag).

#pragma once

#include <winsock2.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

struct UsbmuxDeviceEvent {
    bool attached = false;    // false = Detached
    long long device_id = -1;
    bool usb = false;         // ConnectionType == USB (Attached only; a Wi-Fi
                              // sync entry for the same iPad must be ignored)
};

class UsbmuxListener {
public:
    using Callback = std::function<void(const UsbmuxDeviceEvent&)>;

    UsbmuxListener() = default;
    ~UsbmuxListener() { Stop(); }
    UsbmuxListener(const UsbmuxListener&) = delete;
    UsbmuxListener& operator=(const UsbmuxListener&) = delete;

    // Spawns the listener thread. Fails only on WSAStartup; an unreachable
    // AMDS is handled by the retry loop, not reported here.
    bool Start(Callback callback, std::string* error);
    void Stop();

private:
    void Run();
    // One Listen session: connect, send Listen, dispatch events until the
    // socket dies or stop_. Returns when the session ends.
    void ListenSession();

    Callback callback_;
    std::thread thread_;
    // The session socket lives entirely on the listener thread; Stop() only
    // sets stop_. A 2s recv timeout on the socket bounds how long the thread
    // takes to notice it (no cross-thread socket close, no race).
    std::atomic<bool> stop_{false};
    bool winsock_up_ = false;
};
