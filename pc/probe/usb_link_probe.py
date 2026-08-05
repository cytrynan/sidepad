#!/usr/bin/env python3
"""PC→iPad の USB データリンクを実測する送信側 probe。

iPad 側 (Sidepad アプリ / USBTransportClient) が tcp/7789 で listen しているので、
usbmux のポートフォワード越しにそこへ接続して負荷をかけ、以下を測る。

  * 実効スループット (MB/s, Mbps)
  * 負荷下の RTT 分布 (p50/p95/p99) ← 絶対遅延の当たりを付ける
  * 片道遅延の「最小値からの上振れ」p99 ← iPad 側で算出。ジッタバッファ深さの根拠
  * 到着ジッタ・最大到着ギャップ・取りこぼし・iPad 側 CPU%

使い方（先に usbmux forward でローカルポートを iPad の 7789 に繋いでおくこと）:
    python3 usb_link_probe.py --port 5555 --sweep
    python3 usb_link_probe.py --port 5555 --chunk 65536 --duration 8 --bitrate 40

出力は human-readable なサマリ＋ --out で JSON。
"""

from __future__ import annotations

import argparse
import json
import os
import random
import socket
import struct
import sys
import threading
import time

# ---- ワイヤフォーマット（Sources/Sidepad/ProbeProtocol.swift と一致させること） ----
MSG_PING = 1
MSG_PONG = 2
MSG_DATA = 3
MSG_ACK = 4
MSG_RESET = 5
MSG_REPORT_REQUEST = 6
MSG_REPORT = 7

HEADER = struct.Struct("<IB")   # length(type+payload), type
U64X2 = struct.Struct("<QQ")    # seq, timestamp
U64X3 = struct.Struct("<QQQ")   # seq, t_pc, t_dev


def now_ns() -> int:
    """単調クロック。iPad 側の CLOCK_MONOTONIC_RAW と単位(ns)だけ揃っていればよい。

    Windows の Python 3.10 では time.monotonic_ns が 15.6ms 刻み（RTTが16の倍数に量子化される。
    2026-08-06 実測）。perf_counter_ns は QPC ベースでサブマイクロ秒精度。
    """
    return time.perf_counter_ns()


def frame(msg_type: int, payload: bytes = b"") -> bytes:
    return HEADER.pack(len(payload) + 1, msg_type) + payload


def usbmux_connect(device_port: int, usbmuxd_addr: tuple[str, int] = ("127.0.0.1", 27015)) -> socket.socket:
    """AMDS(Windows版usbmuxd) に直接 Connect を張り、デバイスポートへの生パイプを返す。

    pymobiledevice3 の forward 中継（Python の select ループ）を経由しないので、
    本実装（ネイティブアプリが 27015 に直接繋ぐ形）と同じ経路の実力が測れる。
    プロトコル: [16Bヘッダ(length,version=1,type=8=plist,tag)] + XML plist。
    Connect の PortNumber はネットワークバイトオーダーで渡す点に注意。
    """
    import plistlib

    def send_plist(sock: socket.socket, tag: int, payload: dict) -> dict:
        body = plistlib.dumps(payload, fmt=plistlib.FMT_XML)
        sock.sendall(struct.pack("<IIII", 16 + len(body), 1, 8, tag) + body)
        header = _recv_exact(sock, 16)
        length, _ver, _typ, _tag = struct.unpack("<IIII", header)
        return plistlib.loads(_recv_exact(sock, length - 16))

    def _recv_exact(sock: socket.socket, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("usbmuxd closed connection")
            buf += chunk
        return buf

    base = {"ClientVersionString": "sidepad-probe", "ProgName": "sidepad-probe", "kLibUSBMuxVersion": 3}

    # 1本目の接続でデバイス一覧を取り、2本目で Connect する（Connect 後は plist を話せなくなるため）
    s = socket.create_connection(usbmuxd_addr, timeout=10)
    devices = send_plist(s, 1, {**base, "MessageType": "ListDevices"}).get("DeviceList", [])
    s.close()
    usb = [d for d in devices if d.get("Properties", {}).get("ConnectionType") == "USB"]
    if not usb:
        raise ConnectionError("usbmuxd にUSBデバイスが居ない (信頼/接続を確認)")
    device_id = usb[0]["DeviceID"]

    s = socket.create_connection(usbmuxd_addr, timeout=10)
    result = send_plist(s, 2, {**base, "MessageType": "Connect",
                               "DeviceID": device_id,
                               "PortNumber": socket.htons(device_port)})
    if result.get("Number") != 0:
        s.close()
        raise ConnectionError(f"usbmux Connect 失敗: {result}")
    return s


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = min(len(ordered) - 1, max(0, round(p * (len(ordered) - 1))))
    return ordered[idx]


class Receiver(threading.Thread):
    """iPad からの ACK / PONG / REPORT を読むスレッド。"""

    daemon = True

    def __init__(self, sock: socket.socket):
        super().__init__(daemon=True)
        self.sock = sock
        self.buffer = bytearray()
        self.rtt_ns: list[int] = []
        self.acks = 0
        self.report: dict | None = None
        self.report_event = threading.Event()
        self.stop_flag = threading.Event()

    def run(self) -> None:
        while not self.stop_flag.is_set():
            try:
                chunk = self.sock.recv(65536)
            except (OSError, socket.timeout):
                break
            if not chunk:
                break
            self.buffer.extend(chunk)
            self._drain()

    def _drain(self) -> None:
        # ワイヤ形式: [4B length][1B type][payload(length-1)B]。フレーム全長 = 4 + length。
        # HEADER.size(=5) を全長計算に使うと1バイト余計に食って以降のフレームが全部壊れる
        # （1発目だけ正常・2発目以降サイレント破損。2026-08-06 実測）
        while len(self.buffer) >= HEADER.size:
            length, msg_type = HEADER.unpack_from(self.buffer, 0)
            total = 4 + length
            if len(self.buffer) < total:
                break
            payload = bytes(self.buffer[5:total])
            del self.buffer[:total]

            if msg_type == MSG_PONG and len(payload) >= 24:
                _seq, t_pc, _t_dev = U64X3.unpack(payload[:24])
                self.rtt_ns.append(now_ns() - t_pc)
            elif msg_type == MSG_ACK:
                self.acks += 1
            elif msg_type == MSG_REPORT:
                try:
                    self.report = json.loads(payload.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    self.report = {"error": f"report parse failed: {exc}"}
                self.report_event.set()


class Pinger(threading.Thread):
    """負荷をかけながら定期的に PING を打つ。RTT が遅延の当たりになる。"""

    def __init__(self, sock: socket.socket, lock: threading.Lock, interval: float = 0.05):
        super().__init__(daemon=True)
        self.sock = sock
        self.lock = lock
        self.interval = interval
        self.stop_flag = threading.Event()
        self.seq = 0

    def run(self) -> None:
        while not self.stop_flag.wait(self.interval):
            payload = U64X2.pack(self.seq, now_ns())
            self.seq += 1
            try:
                with self.lock:
                    self.sock.sendall(frame(MSG_PING, payload))
            except OSError:
                return


def run_one(connect, chunk: int, duration: float, bitrate_mbps: float,
            ack_interval: int, label: str, verbose: bool = True) -> dict:
    """1条件を計測する。chunk はアプリ層の1パケット長（計測ヘッダ16B込み）。

    connect: 接続済みソケットを返す callable（TCP forward 経由 or usbmux 直結）。
    """
    sock = connect()
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(15)
    send_lock = threading.Lock()

    receiver = Receiver(sock)
    receiver.start()

    # 計測リセット（ラベルと ACK 間引き間隔を渡す）
    with send_lock:
        sock.sendall(frame(MSG_RESET, struct.pack("<I", ack_interval) + label.encode("utf-8")))

    # 無負荷 RTT を先に取る（負荷下 RTT との差が「詰まり」の量になる）
    idle_rtt = measure_rtt(sock, send_lock, receiver, count=30)

    pinger = Pinger(sock, send_lock)
    baseline_pongs = len(receiver.rtt_ns)
    pinger.start()

    body_len = max(0, chunk - 16)
    body = bytes(random.getrandbits(8) for _ in range(min(body_len, 4096)))
    body = (body * (body_len // len(body) + 1))[:body_len] if body_len else b""

    interval = (chunk * 8) / (bitrate_mbps * 1_000_000) if bitrate_mbps > 0 else 0.0

    seq = 0
    sent_bytes = 0
    start = time.perf_counter()
    next_send = start
    deadline = start + duration
    try:
        while True:
            now = time.perf_counter()
            if now >= deadline:
                break
            if interval > 0:
                if now < next_send:
                    time.sleep(min(next_send - now, 0.002))
                    continue
                next_send += interval
            payload = U64X2.pack(seq, now_ns()) + body
            with send_lock:
                sock.sendall(frame(MSG_DATA, payload))
            seq += 1
            sent_bytes += len(payload) + HEADER.size  # ワイヤ全長 = 4B長 + 1B型 + payload
    except OSError as exc:
        elapsed = time.perf_counter() - start
        pinger.stop_flag.set()
        receiver.stop_flag.set()
        sock.close()
        return {
            "label": label, "chunk": chunk, "error": f"{type(exc).__name__}: {exc}",
            "sent_bytes": sent_bytes, "sent_packets": seq, "elapsed_sec": elapsed,
        }

    elapsed = time.perf_counter() - start
    pinger.stop_flag.set()

    time.sleep(0.3)  # 残りの ACK / 最終パケットの到着待ち
    loaded_rtt = [ns / 1e6 for ns in receiver.rtt_ns[baseline_pongs:]]

    receiver.report_event.clear()
    with send_lock:
        sock.sendall(frame(MSG_REPORT_REQUEST))
    got_report = receiver.report_event.wait(timeout=10)
    device = receiver.report if got_report else {"error": "report timeout"}

    receiver.stop_flag.set()
    sock.close()

    result = {
        "label": label,
        "chunk": chunk,
        "bitrate_target_mbps": bitrate_mbps,
        "elapsed_sec": round(elapsed, 3),
        "pc_sent_packets": seq,
        "pc_sent_bytes": sent_bytes,
        "pc_throughput_MBps": round(sent_bytes / elapsed / 1_048_576, 2) if elapsed else 0,
        "pc_throughput_Mbps": round(sent_bytes * 8 / elapsed / 1_000_000, 2) if elapsed else 0,
        "rtt_idle_ms": {k: round(v, 3) for k, v in idle_rtt.items()},
        "rtt_loaded_ms": {
            "n": len(loaded_rtt),
            "p50": round(percentile(loaded_rtt, 0.50), 3),
            "p95": round(percentile(loaded_rtt, 0.95), 3),
            "p99": round(percentile(loaded_rtt, 0.99), 3),
            "max": round(max(loaded_rtt), 3) if loaded_rtt else 0,
        },
        "acks_received": receiver.acks,
        "device": device,
    }
    if verbose:
        print_result(result)
    return result


def measure_rtt(sock: socket.socket, lock: threading.Lock, receiver: Receiver, count: int) -> dict:
    before = len(receiver.rtt_ns)
    for i in range(count):
        with lock:
            sock.sendall(frame(MSG_PING, U64X2.pack(i, now_ns())))
        time.sleep(0.01)
    time.sleep(0.2)
    samples = [ns / 1e6 for ns in receiver.rtt_ns[before:]]
    return {
        "n": len(samples),
        "min": min(samples) if samples else 0,
        "p50": percentile(samples, 0.50),
        "p99": percentile(samples, 0.99),
        "max": max(samples) if samples else 0,
    }


def print_result(r: dict) -> None:
    dev = r.get("device") or {}
    if "error" in r:
        print(f"  [{r['label']}] chunk={r['chunk']}  ERROR: {r['error']}  "
              f"(送信済 {r['sent_bytes']}B / {r['sent_packets']}パケット)")
        return
    print(f"  [{r['label']}] chunk={r['chunk']:>7}B  "
          f"PC送信 {r['pc_throughput_MBps']:>6.1f} MB/s ({r['pc_throughput_Mbps']:.0f} Mbps)  "
          f"iPad受信 {dev.get('throughputMBps', 0):>6.1f} MB/s")
    print(f"        RTT無負荷 p50 {r['rtt_idle_ms']['p50']:.2f}ms / 負荷下 p50 "
          f"{r['rtt_loaded_ms']['p50']:.2f}ms p99 {r['rtt_loaded_ms']['p99']:.2f}ms  "
          f"片道上振れ p99 {dev.get('owdExcessP99Ms', 0):.2f}ms  "
          f"最大ギャップ {dev.get('maxArrivalGapMs', 0):.1f}ms  "
          f"CPU {dev.get('cpuPercent', 0):.0f}%  欠落 {dev.get('missingSeq', 0)}")


def wait_for_device(connect, timeout: float) -> bool:
    """iPad 側アプリが listen して PING に応えるまで待つ。

    usbmux forward はアプリが起きていなくてもローカル側は accept してしまい、
    デバイス側で拒否されて初めて Broken pipe になる。つまり「TCP が繋がったか」
    では判定できないので、PING/PONG が往復するかで生死を見る。
    """
    deadline = time.time() + timeout
    announced = False
    while time.time() < deadline:
        try:
            sock = connect()
            sock.settimeout(3)
            sock.sendall(frame(MSG_PING, U64X2.pack(0, now_ns())))
            data = sock.recv(64)
            sock.close()
            if data:
                return True
        except OSError:
            pass
        if not announced:
            print(f"iPad の Sidepad が listen するのを待機中… (最大 {timeout:.0f} 秒)", flush=True)
            announced = True
        time.sleep(2)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description="PC→iPad USBデータリンク実測")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5555, help="usbmux forward のローカル側ポート")
    parser.add_argument("--chunk", type=int, default=65536, help="1パケットのバイト数")
    parser.add_argument("--duration", type=float, default=6.0)
    parser.add_argument("--bitrate", type=float, default=0.0, help="Mbps。0なら全力送信")
    parser.add_argument("--ack-interval", type=int, default=16)
    parser.add_argument("--label", default="run")
    parser.add_argument("--sweep", action="store_true", help="チャンクサイズ×負荷条件を一括計測")
    parser.add_argument("--direct", action="store_true",
                        help="usbmuxd(27015)に直接Connectする（forward中継を経由しない＝本実装と同じ経路）")
    parser.add_argument("--device-port", type=int, default=7789)
    parser.add_argument("--wait", type=float, default=0.0,
                        help="アプリが listen するまで待つ秒数（0なら待たない）")
    parser.add_argument("--out", default="")
    args = parser.parse_args()

    if args.direct:
        def connect():
            return usbmux_connect(args.device_port)
    else:
        def connect():
            return socket.create_connection((args.host, args.port), timeout=15)

    if args.wait > 0 and not wait_for_device(connect, args.wait):
        print(f"タイムアウト: {args.wait:.0f} 秒待っても iPad 側が応答しない", file=sys.stderr)
        return 2

    results = []
    try:
        if args.sweep:
            print("== スイープ1: 全力送信（実効上限とチャンクサイズ耐性）==")
            for chunk in (4096, 16384, 65536, 262144):
                results.append(run_one(connect, chunk, args.duration, 0.0,
                                       args.ack_interval, f"max-{chunk // 1024}K"))
                time.sleep(0.5)
            print("== スイープ2: 40Mbps ペーシング（実運用の映像ビットレート相当）==")
            for chunk in (16384, 65536):
                results.append(run_one(connect, chunk, args.duration, 40.0,
                                       args.ack_interval, f"paced40-{chunk // 1024}K"))
                time.sleep(0.5)
        else:
            results.append(run_one(connect, args.chunk, args.duration,
                                   args.bitrate, args.ack_interval, args.label))
    except (ConnectionRefusedError, socket.timeout, OSError) as exc:
        print(f"接続失敗: {exc}", file=sys.stderr)
        print("  → iPad で Sidepad が前面で動いているか、usbmux forward が生きているか確認",
              file=sys.stderr)
        return 1

    if args.out:
        payload = {
            "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "host": f"{args.host}:{args.port}",
            "platform": sys.platform,
            "pid": os.getpid(),
            "results": results,
        }
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=2)
        print(f"JSON 出力: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
