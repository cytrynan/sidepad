#!/usr/bin/env python3
"""ローカル検証用の模擬 iPad 受信器（USB転送統合の受入検証・実機不要）。

iPad 側 USBTransportClient + LinkStats の受信・集計セマンティクスを最小限で再現する:
  * probe ワイヤ形式 ([u32 length][u8 type][payload], LE) の境界復元
  * DATA の seq 連番検査（欠落・順序逆転）と片道遅延サンプル
  * ackInterval ごとの ACK、PING→PONG、REPORT_REQUEST→LinkReport 互換 JSON
  * DATA 本体（16B計測ヘッダ除去後）を到着順に --out へ連結
    → PC 側の同時ダンプ .h264 とバイト一致すれば転送無欠損の直接証明
  * --send-touch-script: type=8 (touch, iPad→PC) を四隅move→中央tap→scroll の
    順で1秒間隔送信し、PC側 input_injector.cpp をiPad実機なしで駆動する
    (.steering/20260811-touch-input/design.md §3-4)

使い方:
    python mock_receiver.py --port 7789 --out reassembled.h264
    python mock_receiver.py --port 7789 --send-touch-script
接続が切れたら集計を表示して終了する（スクリプト検証向けのワンショット動作）。
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import threading
import time

HEADER = struct.Struct("<IB")   # length(type+payload), type
TOUCH = struct.Struct("<BBHHhh")  # kind, source, x, y, dx, dy (design.md §1)
MSG_PING, MSG_PONG, MSG_DATA, MSG_ACK, MSG_RESET, MSG_REPORT_REQUEST, MSG_REPORT, MSG_TOUCH = range(1, 9)


def now_ns() -> int:
    return time.perf_counter_ns()  # monotonic_ns は Windows Python 3.10 で 15.6ms 刻み


def frame(msg_type: int, payload: bytes = b"") -> bytes:
    return HEADER.pack(len(payload) + 1, msg_type) + payload


def touch_frame(kind: int, source: int, x: int, y: int, dx: int, dy: int) -> bytes:
    """type=8 touch メッセージ1件を組み立てる（design.md §1のワイヤ層と同一）。"""
    return frame(MSG_TOUCH, TOUCH.pack(kind, source, x, y, dx, dy))


# --send-touch-script のスクリプト本体: 四隅move→中央tap→scroll→長押しドラッグ
# (dragBegin→move×2→dragEnd)。
# 期待座標は pc/probe/verify-touch-inject.ps1 がこの座標から独立に再計算して
# GetCursorPos と突き合わせる（実装の式を写さない = judge-design.mdの教訓）。
# dragBegin/dragEndの座標はG3(0x0001,0xFFFE)とは別に、四隅・中央と重ならない
# 内部の点を選んで軌跡判定を独立させる(2026-08-11 K承認の長押しドラッグ追加)。
# dragBegin〜dragEnd間の2発のkind=1 moveは「掴んだまま動く」の実測用
# (レビュー2026-08-11 判定器補強2: 左ボタンDOWNのまま移動できることを
# GetAsyncKeyStateで直接確認するverify-touch-inject.ps1の材料になる)。
TOUCH_SCRIPT = [
    # kind, source, x,     y,     dx,  dy
    (1,     0,      0,     0,     0,   0),      # move: top-left corner
    (1,     0,      65535, 0,     0,   0),      # move: top-right corner
    (1,     0,      0,     65535, 0,   0),      # move: bottom-left corner
    (1,     0,      65535, 65535, 0,   0),      # move: bottom-right corner
    (2,     0,      32767, 32767, 0,   0),      # tap: center
    (3,     1,      32767, 32767, -10, 300),    # scroll: same magnitude as G2
    (4,     0,      16384, 16384, 0,   0),      # dragBegin: upper-left-ish
    (1,     0,      27307, 16384, 0,   0),      # move while dragging (button held)
    (1,     0,      38229, 27307, 0,   0),      # move while dragging (button held)
    (5,     0,      49151, 49151, 0,   0),      # dragEnd: lower-right-ish
]

# メッセージ間隔（秒）。判定側のサンプリング周期(約20Hz)とのタイミング結合を
# 避けるため軌跡ベース判定に変更した(judge-design.md J2)。それでも各メッセージ
# には十分な反応時間を与える(1.0s→1.5s、レビュー2026-08-11 J2)。
TOUCH_SCRIPT_INTERVAL_SEC = 1.5


def send_touch_script(conn: socket.socket, send_lock: threading.Lock,
                       stop_event: threading.Event) -> None:
    """TOUCH_SCRIPT_INTERVAL_SEC間隔でTOUCH_SCRIPTを送信するスレッド本体。
    他のsendall呼び出し(ACK/PONG/REPORT)と同じソケットを使うため
    send_lock で直列化する。"""
    for kind, source, x, y, dx, dy in TOUCH_SCRIPT:
        if stop_event.is_set():
            return
        msg = touch_frame(kind, source, x, y, dx, dy)
        try:
            with send_lock:
                conn.sendall(msg)
        except OSError:
            return  # link already closed; nothing more to do
        print(f"touch-script: sent kind={kind} source={source} x={x} y={y} "
              f"dx={dx} dy={dy}", flush=True)
        time.sleep(TOUCH_SCRIPT_INTERVAL_SEC)
    print("touch-script: done (all messages sent)", flush=True)


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
    parser.add_argument("--send-touch-script", action="store_true",
                         help="接続受理後、TOUCH_SCRIPT(四隅move→中央tap→scroll)を"
                              "1秒間隔で送信する(PC側 input_injector.cpp を"
                              "iPad実機なしで駆動)")
    args = parser.parse_args()

    listener = socket.create_server(("127.0.0.1", args.port))
    print(f"listening on 127.0.0.1:{args.port}", flush=True)
    conn, addr = listener.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"connected: {addr}", flush=True)

    out = open(args.out, "wb") if args.out else None
    stats = Stats()
    buffer = bytearray()
    # ACK/PONG/REPORT の通常応答とtouch-scriptの送信は別スレッドから同じ
    # ソケットに書くため、send_lock で直列化する(TCPは書き込み単位の
    # 排他を保証しないので、複数スレッドの sendall が交互に割り込むと
    # ワイヤが破損しうる)。
    send_lock = threading.Lock()
    stop_event = threading.Event()
    touch_thread: threading.Thread | None = None
    if args.send_touch_script:
        touch_thread = threading.Thread(
            target=send_touch_script, args=(conn, send_lock, stop_event),
            daemon=True)
        touch_thread.start()

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
                        with send_lock:
                            conn.sendall(frame(MSG_ACK, struct.pack("<QQ", seq, recv_ns)))
                    if out:
                        out.write(payload[16:])
                elif msg_type == MSG_PING and len(payload) >= 16:
                    with send_lock:
                        conn.sendall(frame(MSG_PONG, payload[:16] + struct.pack("<Q", now_ns())))
                elif msg_type == MSG_RESET:
                    interval = struct.unpack_from("<I", payload)[0] if len(payload) >= 4 else 16
                    label = payload[4:].decode("utf-8", "replace")
                    stats.reset(label, interval)
                    print(f"reset: label={label!r} ackInterval={interval}", flush=True)
                elif msg_type == MSG_REPORT_REQUEST:
                    with send_lock:
                        conn.sendall(frame(MSG_REPORT, json.dumps(stats.report()).encode()))
                # MSG_TOUCH: iPad -> PC専用。このスクリプトはPC役ではないので
                # 受信しても捨てる(送るだけ)。
    except (ConnectionError, OSError) as exc:
        print(f"connection error: {exc}", file=sys.stderr)
    finally:
        stop_event.set()
        if touch_thread:
            touch_thread.join(timeout=2.0)
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
