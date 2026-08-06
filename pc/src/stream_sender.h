// pc/src/stream_sender.h
//
// Sends encoded frames to the iPad over the probe wire format
// (Sources/Sidepad/ProbeProtocol.swift): [u32 length][u8 type][payload],
// little-endian, length = type + payload. One encoded frame = one DATA
// message whose payload is [u64 seq][u64 t_pc_ns][Annex-B access unit], so
// the iPad's Framing.drain hands complete access units to onFrameData.
//
// Socket writes are issued in <=16 KiB chunks -- the write size the
// 2026-08-06 usbmux link probe measured as best for one-way latency
// (.steering/20260803-usb-datalink-probe/result.md). TCP_NODELAY is the only
// socket option touched, matching the probe conditions so its measurements
// stay predictive.
//
// Two connect paths:
//   ConnectTcp    -- plain TCP (local mock receiver, future wired-LAN)
//   ConnectUsbmux -- speaks the usbmux plist protocol to AMDS on
//                    127.0.0.1:27015 and turns the socket into a raw pipe to
//                    the device port (canon: ~/.claude/dev/ios-dev-without-mac.md
//                    addendum 7: 16B header <IIII>, PortNumber in htons).
//
// Threading contract: exactly one thread calls SendReset/SendFrame/
// RequestReport at a time (the encode drain thread during the run, the main
// thread after the join). A background thread drains ACK/PONG/REPORT.

#pragma once

#include <winsock2.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class StreamSender {
public:
    StreamSender() = default;
    ~StreamSender() { Close(); }
    StreamSender(const StreamSender&) = delete;
    StreamSender& operator=(const StreamSender&) = delete;

    bool ConnectTcp(const std::string& host, uint16_t port, std::string* error);
    // device_port is the iPad app's listen port (host byte order; 7789).
    bool ConnectUsbmux(uint16_t device_port, std::string* error);

    // MSG_RESET: clears the device-side LinkStats and sets the ACK cadence.
    bool SendReset(const std::string& label, uint32_t ack_interval);

    // Blocking send of one frame; false means the link is broken (callers
    // must treat that as fatal, not skip silently). Wall time is recorded
    // into send_elapsed_ns() for the run statistics.
    bool SendFrame(uint64_t seq, uint64_t t_pc_ns, const uint8_t* data,
                   size_t size);

    // MSG_REPORT_REQUEST -> waits for the device's MSG_REPORT JSON.
    // Empty string on timeout/error.
    std::string RequestReport(int timeout_ms);

    void Close();

    bool connected() const { return sock_ != INVALID_SOCKET; }
    long long acks_received() const { return acks_.load(); }
    // Per-frame SendFrame wall time; owned by the sending thread, read after
    // the run (same single-writer contract as the call sites).
    const std::vector<long long>& send_elapsed_ns() const { return send_ns_; }

private:
    bool EnsureWinsock(std::string* error);
    bool FinishConnect(std::string* error);  // TCP_NODELAY + receiver thread
    bool SendAllChunked(const uint8_t* data, size_t size);
    bool SendMessage_(uint8_t type, const uint8_t* payload, size_t size);
    void ReceiverLoop();

    SOCKET sock_ = INVALID_SOCKET;
    bool winsock_up_ = false;
    std::thread receiver_;
    std::atomic<bool> stop_{false};

    std::atomic<long long> acks_{0};
    std::vector<long long> send_ns_;

    std::mutex report_mutex_;
    std::condition_variable report_cv_;
    std::string report_json_;
    bool report_ready_ = false;
};

// Monotonic nanoseconds from QPC. Only differences ever cross the wire
// (the iPad cancels the unknown clock offset), so the epoch is irrelevant;
// what matters is sub-ms granularity, which QPC provides.
uint64_t MonotonicNs();
