// pc/src/stream_sender.cpp -- see stream_sender.h for the contract.

#include "stream_sender.h"

#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace {

// Wire message types (ProbeProtocol.swift / usb_link_probe.py).
constexpr uint8_t kMsgPing = 1;
constexpr uint8_t kMsgPong = 2;
constexpr uint8_t kMsgData = 3;
constexpr uint8_t kMsgAck = 4;
constexpr uint8_t kMsgReset = 5;
constexpr uint8_t kMsgReportRequest = 6;
constexpr uint8_t kMsgReport = 7;

constexpr size_t kSendChunkBytes = 16 * 1024;  // probe's best write size
constexpr uint16_t kUsbmuxdPort = 27015;       // AMDS listener on Windows

void PutLE32(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v);
    dst[1] = static_cast<uint8_t>(v >> 8);
    dst[2] = static_cast<uint8_t>(v >> 16);
    dst[3] = static_cast<uint8_t>(v >> 24);
}

void PutLE64(uint8_t* dst, uint64_t v) {
    for (int i = 0; i < 8; ++i) dst[i] = static_cast<uint8_t>(v >> (8 * i));
}

bool RecvExact(SOCKET s, uint8_t* dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = recv(s, reinterpret_cast<char*>(dst) + got,
                     static_cast<int>(n - got), 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

// --- usbmux plist session helpers -------------------------------------------
// The plist bodies are tiny and fully under our control, so responses are
// scanned with plain string search instead of a real XML parser (same
// semantics as the probe's plistlib usage; single-device assumption).

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

bool RecvPlist(SOCKET s, std::string* body) {
    uint8_t header[16];
    if (!RecvExact(s, header, 16)) return false;
    uint32_t length = header[0] | (header[1] << 8) | (header[2] << 16) |
                      (static_cast<uint32_t>(header[3]) << 24);
    if (length < 16 || length > 4 * 1024 * 1024) return false;
    body->resize(length - 16);
    return RecvExact(s, reinterpret_cast<uint8_t*>(body->data()), body->size());
}

std::string PlistDocument(const std::string& dict_entries) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\"><dict>"
           "<key>ClientVersionString</key><string>sidepad-stream</string>"
           "<key>ProgName</key><string>sidepad-stream</string>"
           "<key>kLibUSBMuxVersion</key><integer>3</integer>" +
           dict_entries + "</dict></plist>";
}

// First <integer> value following `key` in a scanned plist body; -1 if absent.
long long IntegerAfterKey(const std::string& body, const std::string& key) {
    size_t pos = body.find("<key>" + key + "</key>");
    if (pos == std::string::npos) return -1;
    pos = body.find("<integer>", pos);
    if (pos == std::string::npos) return -1;
    return _atoi64(body.c_str() + pos + 9);
}

// DeviceID of the first ListDevices entry whose ConnectionType is USB; -1 if
// none. With Wi-Fi sync enabled AMDS lists the same iPad twice (USB and
// Network, different DeviceIDs), so taking the document's first DeviceID
// would silently connect -- and measure -- over Wi-Fi (review 2026-08-06
// finding 1). The device's own IDs surround its ConnectionType (top-level
// DeviceID before Properties, and Properties usually carries a copy), so the
// nearest <key>DeviceID</key> BEFORE the USB marker belongs to that entry.
long long UsbDeviceIdFromListing(const std::string& body) {
    const std::string conn_key = "<key>ConnectionType</key>";
    const std::string id_key = "<key>DeviceID</key>";
    size_t search = 0;
    while (true) {
        const size_t conn = body.find(conn_key, search);
        if (conn == std::string::npos) return -1;
        search = conn + conn_key.size();
        const size_t value = body.find("<string>", conn);
        if (value == std::string::npos) return -1;
        if (body.compare(value, 15, "<string>USB</st") != 0) continue;

        const size_t before = body.rfind(id_key, conn);
        const size_t at = (before != std::string::npos)
                              ? before
                              : body.find(id_key, conn);  // defensive fallback
        if (at == std::string::npos) return -1;
        const size_t integer = body.find("<integer>", at);
        if (integer == std::string::npos) return -1;
        return _atoi64(body.c_str() + integer + 9);
    }
}

SOCKET DialUsbmuxd(std::string* error) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        *error = "socket() failed: " + std::to_string(WSAGetLastError());
        return INVALID_SOCKET;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kUsbmuxdPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        *error = "cannot reach usbmuxd (AMDS) on 127.0.0.1:27015 -- is the "
                 "Apple Mobile Device Service running? err=" +
                 std::to_string(WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    // The probe uses a 10s timeout on the plist session; without one, an
    // unresponsive AMDS/device hangs RecvExact forever with no diagnostic.
    // FinishConnect clears the recv timeout again for the data socket.
    DWORD timeout_ms = 10'000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    return s;
}

}  // namespace

uint64_t MonotonicNs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const long long whole = now.QuadPart / freq.QuadPart;
    const long long rem = now.QuadPart % freq.QuadPart;
    return static_cast<uint64_t>(whole) * 1'000'000'000ULL +
           static_cast<uint64_t>(rem * 1'000'000'000LL / freq.QuadPart);
}

bool StreamSender::EnsureWinsock(std::string* error) {
    if (winsock_up_) return true;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        *error = "WSAStartup failed";
        return false;
    }
    winsock_up_ = true;
    return true;
}

bool StreamSender::ConnectTcp(const std::string& host, uint16_t port,
                              std::string* error) {
    if (!EnsureWinsock(error)) return false;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
        *error = "getaddrinfo failed for " + host;
        return false;
    }
    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            sock_ = s;
            break;
        }
        closesocket(s);
    }
    freeaddrinfo(result);
    if (sock_ == INVALID_SOCKET) {
        *error = "connect to " + host + ":" + port_str + " failed (err=" +
                 std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    return FinishConnect(error);
}

bool StreamSender::ConnectUsbmux(uint16_t device_port, std::string* error) {
    if (!EnsureWinsock(error)) return false;

    // Session 1: ListDevices. A Connect-ed socket can no longer speak plist,
    // so the listing needs its own connection (probe does the same).
    SOCKET s = DialUsbmuxd(error);
    if (s == INVALID_SOCKET) return false;
    std::string body;
    bool ok = SendPlist(s, 1, PlistDocument(
                  "<key>MessageType</key><string>ListDevices</string>")) &&
              RecvPlist(s, &body);
    closesocket(s);
    if (!ok) {
        *error = "usbmux ListDevices failed";
        return false;
    }
    // The reply format (XML vs binary plist) is not pinned down by primary
    // sources; if AMDS answers with a binary plist, say so explicitly instead
    // of a misleading "no device" (review 2026-08-06 finding 6).
    if (body.rfind("bplist", 0) == 0) {
        *error = "AMDS answered with a binary plist; the string-scan parser "
                 "only handles XML (needs a real plist parser)";
        return false;
    }
    const long long device_id = UsbDeviceIdFromListing(body);
    if (device_id < 0) {
        *error = "usbmuxd lists no USB-connected device (check cable / trust "
                 "dialog; Wi-Fi-only entries are ignored on purpose)";
        return false;
    }
    std::printf("usbmux: using DeviceID %lld (ConnectionType=USB)\n", device_id);

    // Session 2: Connect. PortNumber goes over the wire in network byte
    // order (canon: ios-dev-without-mac.md addendum 7). On success the same
    // socket becomes a raw pipe to the device port.
    s = DialUsbmuxd(error);
    if (s == INVALID_SOCKET) return false;
    ok = SendPlist(s, 2, PlistDocument(
             "<key>MessageType</key><string>Connect</string>"
             "<key>DeviceID</key><integer>" + std::to_string(device_id) +
             "</integer>"
             "<key>PortNumber</key><integer>" +
             std::to_string(htons(device_port)) + "</integer>")) &&
         RecvPlist(s, &body);
    if (!ok || IntegerAfterKey(body, "Number") != 0) {
        *error = "usbmux Connect to device port " + std::to_string(device_port) +
                 " refused (is the Sidepad app foregrounded?)";
        closesocket(s);
        return false;
    }
    sock_ = s;
    return FinishConnect(error);
}

bool StreamSender::FinishConnect(std::string* error) {
    // Nagle would batch small control messages behind bulk frame data.
    BOOL yes = TRUE;
    if (setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0) {
        *error = "setsockopt(TCP_NODELAY) failed";
        Close();
        return false;
    }
    // Send timeout so a suspended iPad app (window closed, no FIN over
    // usbmux) surfaces as a SendFrame failure -> the caller's fatal path,
    // instead of a silent forever-hang in drain.join() (review finding 2).
    DWORD send_timeout_ms = 2'000;
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&send_timeout_ms),
               sizeof(send_timeout_ms));
    // Clear any recv timeout inherited from the usbmux plist session: the
    // receiver loop treats recv<=0 as "link closed", so a periodic
    // WSAETIMEDOUT would falsely kill it.
    DWORD no_timeout = 0;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&no_timeout), sizeof(no_timeout));
    stop_ = false;
    receiver_ = std::thread([this] { ReceiverLoop(); });
    return true;
}

bool StreamSender::SendAllChunked(const uint8_t* data, size_t size) {
    size_t off = 0;
    while (off < size) {
        const int want =
            static_cast<int>(size - off < kSendChunkBytes ? size - off
                                                          : kSendChunkBytes);
        const int sent =
            send(sock_, reinterpret_cast<const char*>(data) + off, want, 0);
        if (sent <= 0) return false;
        off += static_cast<size_t>(sent);
    }
    return true;
}

bool StreamSender::SendMessage_(uint8_t type, const uint8_t* payload,
                                size_t size) {
    // One contiguous wire buffer so the 16 KiB chunking spans header+payload
    // uniformly (a lone 5-byte write would be its own tiny TCP segment under
    // TCP_NODELAY). The memcpy is ~83 KB per frame -- noise next to encode.
    std::vector<uint8_t> wire(5 + size);
    PutLE32(wire.data(), static_cast<uint32_t>(size + 1));
    wire[4] = type;
    if (size) std::memcpy(wire.data() + 5, payload, size);
    return SendAllChunked(wire.data(), wire.size());
}

bool StreamSender::SendReset(const std::string& label, uint32_t ack_interval) {
    std::vector<uint8_t> payload(4 + label.size());
    PutLE32(payload.data(), ack_interval);
    std::memcpy(payload.data() + 4, label.data(), label.size());
    return SendMessage_(kMsgReset, payload.data(), payload.size());
}

bool StreamSender::SendFrame(uint64_t seq, uint64_t t_pc_ns,
                             const uint8_t* data, size_t size) {
    const uint64_t t0 = MonotonicNs();
    std::vector<uint8_t> wire(5 + 16 + size);
    PutLE32(wire.data(), static_cast<uint32_t>(16 + size + 1));
    wire[4] = kMsgData;
    PutLE64(wire.data() + 5, seq);
    PutLE64(wire.data() + 13, t_pc_ns);
    std::memcpy(wire.data() + 21, data, size);
    const bool ok = SendAllChunked(wire.data(), wire.size());
    send_ns_.push_back(static_cast<long long>(MonotonicNs() - t0));
    return ok;
}

std::string StreamSender::RequestReport(int timeout_ms) {
    {
        std::lock_guard<std::mutex> lock(report_mutex_);
        report_ready_ = false;
        report_json_.clear();
    }
    if (!SendMessage_(kMsgReportRequest, nullptr, 0)) return {};
    std::unique_lock<std::mutex> lock(report_mutex_);
    report_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this] { return report_ready_; });
    return report_json_;
}

void StreamSender::ReceiverLoop() {
    std::vector<uint8_t> buffer;
    uint8_t chunk[65536];
    while (!stop_.load(std::memory_order_relaxed)) {
        const int r = recv(sock_, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
        if (r <= 0) break;
        buffer.insert(buffer.end(), chunk, chunk + r);

        // Frame total = 4 (length prefix) + length. Using 5 here re-creates
        // the probe's "first frame fine, rest silently corrupt" bug
        // (2026-08-06 measured) -- keep it 4.
        size_t off = 0;
        while (buffer.size() - off >= 4) {
            const uint32_t length = buffer[off] | (buffer[off + 1] << 8) |
                                    (buffer[off + 2] << 16) |
                                    (static_cast<uint32_t>(buffer[off + 3]) << 24);
            if (length < 1) {
                // Corrupt stream: drop the buffer and give up on resync,
                // matching Framing.drain on the iPad. Leaving it in place
                // would loop on the same bytes forever (review finding 4).
                buffer.clear();
                off = 0;
                break;
            }
            const size_t total = 4 + static_cast<size_t>(length);
            if (buffer.size() - off < total) break;
            const uint8_t type = buffer[off + 4];
            if (type == kMsgAck) {
                acks_.fetch_add(1, std::memory_order_relaxed);
            } else if (type == kMsgReport) {
                std::lock_guard<std::mutex> lock(report_mutex_);
                report_json_.assign(
                    reinterpret_cast<const char*>(buffer.data() + off + 5),
                    length - 1);
                report_ready_ = true;
                report_cv_.notify_all();
            }
            // kMsgPong and anything else: drained and ignored (we send no pings).
            off += total;
        }
        if (off) buffer.erase(buffer.begin(), buffer.begin() + off);
    }
}

void StreamSender::Close() {
    stop_ = true;
    // Order matters: shutdown unblocks the receiver's recv, the join makes
    // sure no thread still reads sock_, and only then is the handle closed
    // and overwritten (closing first would race the receiver on a non-atomic
    // SOCKET and could hand a recycled handle to recv -- review finding 5).
    if (sock_ != INVALID_SOCKET) shutdown(sock_, SD_BOTH);
    if (receiver_.joinable()) receiver_.join();
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    if (winsock_up_) {
        WSACleanup();
        winsock_up_ = false;
    }
}
