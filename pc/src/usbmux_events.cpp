// pc/src/usbmux_events.cpp -- see usbmux_events.h for the contract.
//
// The plist session helpers mirror stream_sender.cpp's (16-byte header,
// type 8 = XML plist, string-scan parsing). They are duplicated on purpose:
// stream_sender.cpp is acceptance-verified code and its helpers live in an
// anonymous namespace; sharing would mean touching it for no behavioral
// gain. Canon for the wire format: ~/.claude/dev/ios-dev-without-mac.md
// addendum 7.

#include "usbmux_events.h"

#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr uint16_t kUsbmuxdPort = 27015;
constexpr DWORD kRecvTimeoutMs = 2'000;   // bounds Stop() latency
constexpr int kRetryDelayMs = 2'000;      // AMDS unreachable / session died

void PutLE32(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v);
    dst[1] = static_cast<uint8_t>(v >> 8);
    dst[2] = static_cast<uint8_t>(v >> 16);
    dst[3] = static_cast<uint8_t>(v >> 24);
}

bool SendAll(SOCKET s, const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    size_t off = 0;
    while (off < n) {
        const int sent = send(s, p + off, static_cast<int>(n - off), 0);
        if (sent <= 0) return false;
        off += static_cast<size_t>(sent);
    }
    return true;
}

bool SendPlist(SOCKET s, uint32_t tag, const std::string& xml) {
    uint8_t header[16];
    PutLE32(header + 0, static_cast<uint32_t>(16 + xml.size()));
    PutLE32(header + 4, 1);   // version
    PutLE32(header + 8, 8);   // type 8 = plist
    PutLE32(header + 12, tag);
    return SendAll(s, header, 16) && SendAll(s, xml.data(), xml.size());
}

std::string PlistDocument(const std::string& dict_entries) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\"><dict>"
           "<key>ClientVersionString</key><string>sidepadd</string>"
           "<key>ProgName</key><string>sidepadd</string>"
           "<key>kLibUSBMuxVersion</key><integer>3</integer>" +
           dict_entries + "</dict></plist>";
}

long long IntegerAfterKey(const std::string& body, const std::string& key) {
    size_t pos = body.find("<key>" + key + "</key>");
    if (pos == std::string::npos) return -1;
    pos = body.find("<integer>", pos);
    if (pos == std::string::npos) return -1;
    return _atoi64(body.c_str() + pos + 9);
}

bool StringAfterKeyIs(const std::string& body, const std::string& key,
                      const std::string& value) {
    size_t pos = body.find("<key>" + key + "</key>");
    if (pos == std::string::npos) return false;
    pos = body.find("<string>", pos);
    if (pos == std::string::npos) return false;
    return body.compare(pos + 8, value.size(), value) == 0 &&
           body.compare(pos + 8 + value.size(), 2, "</") == 0;
}

SOCKET DialUsbmuxd() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kUsbmuxdPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    DWORD timeout_ms = kRecvTimeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    return s;
}

}  // namespace

bool UsbmuxListener::Start(Callback callback, std::string* error) {
    if (thread_.joinable()) return true;
    if (!winsock_up_) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            *error = "WSAStartup failed";
            return false;
        }
        winsock_up_ = true;
    }
    callback_ = std::move(callback);
    stop_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { Run(); });
    return true;
}

void UsbmuxListener::Stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
    if (winsock_up_) {
        WSACleanup();
        winsock_up_ = false;
    }
}

void UsbmuxListener::Run() {
    while (!stop_.load(std::memory_order_relaxed)) {
        ListenSession();
        // Session ended (AMDS down/restarted, or Listen refused): pause,
        // then retry. A successful re-Listen re-announces attached devices.
        for (int waited = 0;
             waited < kRetryDelayMs && !stop_.load(std::memory_order_relaxed);
             waited += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void UsbmuxListener::ListenSession() {
    SOCKET s = DialUsbmuxd();
    if (s == INVALID_SOCKET) return;

    if (!SendPlist(s, 1, PlistDocument(
                       "<key>MessageType</key><string>Listen</string>"))) {
        closesocket(s);
        return;
    }

    // Buffered parse of [16B header][plist body] messages. recv times out
    // every kRecvTimeoutMs so stop_ is honored even on a silent socket.
    std::string buffer;
    char chunk[16384];
    bool result_seen = false;
    while (!stop_.load(std::memory_order_relaxed)) {
        const int r = recv(s, chunk, sizeof(chunk), 0);
        if (r == 0) break;  // AMDS closed
        if (r < 0) {
            if (WSAGetLastError() == WSAETIMEDOUT) continue;
            break;
        }
        buffer.append(chunk, static_cast<size_t>(r));

        size_t off = 0;
        while (buffer.size() - off >= 16) {
            const uint8_t* h =
                reinterpret_cast<const uint8_t*>(buffer.data()) + off;
            const uint32_t length = h[0] | (h[1] << 8) | (h[2] << 16) |
                                    (static_cast<uint32_t>(h[3]) << 24);
            if (length < 16 || length > 4 * 1024 * 1024) {
                closesocket(s);
                return;  // corrupt stream; retry with a fresh session
            }
            if (buffer.size() - off < length) break;
            const std::string body = buffer.substr(off + 16, length - 16);
            off += length;

            if (!result_seen &&
                StringAfterKeyIs(body, "MessageType", "Result")) {
                result_seen = true;
                if (IntegerAfterKey(body, "Number") != 0) {
                    closesocket(s);
                    return;  // Listen refused
                }
                continue;
            }
            UsbmuxDeviceEvent ev;
            if (StringAfterKeyIs(body, "MessageType", "Attached")) {
                ev.attached = true;
                ev.device_id = IntegerAfterKey(body, "DeviceID");
                ev.usb = StringAfterKeyIs(body, "ConnectionType", "USB");
            } else if (StringAfterKeyIs(body, "MessageType", "Detached")) {
                ev.attached = false;
                ev.device_id = IntegerAfterKey(body, "DeviceID");
            } else {
                continue;  // Paired etc.: ignored
            }
            if (ev.device_id >= 0 && callback_) callback_(ev);
        }
        if (off) buffer.erase(0, off);
    }
    closesocket(s);
}
