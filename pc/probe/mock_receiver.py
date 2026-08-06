#!/usr/bin/env python3
"""ローカル検証用の模擬 iPad 受信器（USB転送統合の受入検証・実機不要）。

iPad 側 USBTransportClient + LinkStats の受信・集計セマンティクスを最小限で再現する:
  * probe ワイヤ形式 ([u32 length][u8 type][payload], LE) の境界復元
  * DATA の seq 連番検査（欠落・順序逆転）と片道遅延サンプル
  * ackInterval ごとの ACK、PING→PONG、REPORT_REQUEST→LinkReport 互換 JSON
  * DATA 本体（16B計測ヘッダ除去後）を到着順に --out へ連結
    → PC 側の同時ダンプ .h264 とバイト一致すれば転送無欠損の直接証明

使い方:
    python mock_receiver.py --port 7789 --out reassembled.h264
接続が切れたら集計を表示して終了する（スクリプト検証向けのワンショット動作）。
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time

HEADER = struct.Struct("<IB")   # length(type+payload), type
MSG_PING, MSG_PONG, MSG_DATA, MSG_ACK, MSG_RESET, MSG_REPORT_REQUEST, MSG_REPORT = range(1, 8)


def now_ns() -> int:
    return time.perf_counter_ns()  # monotonic_ns は Windows Python 3.10 で 15.6ms 刻み


def frame(msg_type: int, payload: bytes = b"") -> bytes:
    return HEADER.pack(len(payload) + 1, msg_type) + payload


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, max(0, round(p * (len(ordered) - 1))))]


class Stats:
    def __init__(self) -> None:
        self.reset("", 16)

    def reset(self, label: str, ack_interval: int) -> None:
        self.label = label
        self.ack_interval = max(1, ack_interval)
        self.packets = 0
        self.bytes = 0
        self.first_ns = 0
        self.last_ns = 0
        self.next_seq = 0
        self.missing = 0
        self.out_of_order = 0
        self.delay_samples: list[int] = []   # recv_ns - send_ns（オフセット込み）
        self.max_gap_ns = 0
        self.prev_recv_ns = 0

    def ingest(self, seq: int, send_ns: int, wire_bytes: int, recv_ns: int) -> bool:
        self.packets += 1
        self.bytes += wire_bytes
        if self.first_ns == 0:
            self.first_ns = recv_ns
        if self.prev_recv_ns:
            self.max_gap_ns = max(self.max_gap_ns, recv_ns - self.prev_recv_ns)
        self.prev_recv_ns = recv_ns
        self.last_ns = recv_ns
        if seq > self.next_seq:
            self.missing += seq - self.next_seq
            self.next_seq = seq + 1
        elif seq < self.next_seq:
            self.out_of_order += 1
        else:
            self.next_seq = seq + 1
        self.delay_samples.append(recv_ns - send_ns)
        return self.packets % self.ack_interval == 0

    def report(self) -> dict:
        duration = (self.last_ns - self.first_ns) / 1e9 if self.packets > 1 else 0.0
        base = min(self.delay_samples) if self.delay_samples else 0
        excess_ms = [(d - base) / 1e6 for d in self.delay_samples]
        return {
            "label": self.label,
            "durationSec": round(duration, 3),
            "packets": self.packets,
            "bytes": self.bytes,
            "throughputMBps": round(self.bytes / duration / 1_048_576, 2) if duration else 0.0,
            "throughputMbps": round(self.bytes * 8 / duration / 1e6, 2) if duration else 0.0,
            "meanPacketBytes": round(self.bytes / self.packets, 1) if self.packets else 0.0,
            "owdExcessP50Ms": round(percentile(excess_ms, 0.50), 3),
            "owdExcessP95Ms": round(percentile(excess_ms, 0.95), 3),
            "owdExcessP99Ms": round(percentile(excess_ms, 0.99), 3),
            "owdExcessMaxMs": round(max(excess_ms), 3) if excess_ms else 0.0,
            "interarrivalJitterMs": 0.0,   # 模擬では省略（実機のLinkStatsが正）
            "maxArrivalGapMs": round(self.max_gap_ns / 1e6, 2),
            "outOfOrder": self.out_of_order,
            "missingSeq": self.missing,
            "cpuPercent": 0.0,
            "samples": len(self.delay_samples),
        }


def main() -> int:
    parser = argparse.ArgumentParser(description="模擬iPad受信器（ローカル検証用）")
    parser.add_argument("--port", type=int, default=7789)
    parser.add_argument("--out", default="", help="受信フレーム本体の連結先（.h264）")
    args = parser.parse_args()

    listener = socket.create_server(("127.0.0.1", args.port))
    print(f"listening on 127.0.0.1:{args.port}", flush=True)
    conn, addr = listener.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"connected: {addr}", flush=True)

    out = open(args.out, "wb") if args.out else None
    stats = Stats()
    buffer = bytearray()

    try:
        while True:
            chunk = conn.recv(262144)
            recv_ns = now_ns()
            if not chunk:
                break
            buffer.extend(chunk)
            # フレーム全長 = 4 + length（5+length は既知の破損バグ。probe実測 2026-08-06）
            while len(buffer) >= HEADER.size:
                length, msg_type = HEADER.unpack_from(buffer, 0)
                total = 4 + length
                if length < 1 or len(buffer) < total:
                    break
                payload = bytes(buffer[5:total])
                del buffer[:total]

                if msg_type == MSG_DATA and len(payload) >= 16:
                    seq, send_ns = struct.unpack_from("<QQ", payload)
                    if stats.ingest(seq, send_ns, len(payload) + 5, recv_ns):
                        conn.sendall(frame(MSG_ACK, struct.pack("<QQ", seq, recv_ns)))
                    if out:
                        out.write(payload[16:])
                elif msg_type == MSG_PING and len(payload) >= 16:
                    conn.sendall(frame(MSG_PONG, payload[:16] + struct.pack("<Q", now_ns())))
                elif msg_type == MSG_RESET:
                    interval = struct.unpack_from("<I", payload)[0] if len(payload) >= 4 else 16
                    label = payload[4:].decode("utf-8", "replace")
                    stats.reset(label, interval)
                    print(f"reset: label={label!r} ackInterval={interval}", flush=True)
                elif msg_type == MSG_REPORT_REQUEST:
                    conn.sendall(frame(MSG_REPORT, json.dumps(stats.report()).encode()))
    except (ConnectionError, OSError) as exc:
        print(f"connection error: {exc}", file=sys.stderr)
    finally:
        if out:
            out.close()
        conn.close()
        listener.close()

    r = stats.report()
    print("\n=== mock receiver summary ===")
    print(json.dumps(r, indent=2))
    return 0 if r["missingSeq"] == 0 and r["outOfOrder"] == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
