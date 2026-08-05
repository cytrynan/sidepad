#!/bin/bash
# WSL(usbip)経由でリンク実測を回す。usbmux のポートフォワードを張ってから probe を叩く。
#
# 前提: iPad が usbipd で WSL に attach 済み・iPad で Sidepad が前面で起動している。
# 使い方: bash run-probe-wsl.sh [probe への追加引数...]
#   例: bash run-probe-wsl.sh --sweep --out /root/probe-wsl.json
set -u

PMD3=/root/pmd3/bin/pymobiledevice3
LOCAL_PORT=${LOCAL_PORT:-5555}
DEVICE_PORT=${DEVICE_PORT:-7789}
PROBE=/root/sidepad/pc/probe/usb_link_probe.py

pkill -f "usbmux forward" 2>/dev/null
sleep 1

# WSL 検出で Windows 側 TCP に繋ぎに行く挙動を殺すため usbmux ソケットを明示する
# （pymobiledevice3 の地雷。~/.claude/dev/ios-dev-without-mac.md 追記4）
"$PMD3" usbmux forward --usbmux /var/run/usbmuxd -d "$LOCAL_PORT" "$DEVICE_PORT" \
  >/root/usbmux-forward.log 2>&1
sleep 2

if ! ss -ltn 2>/dev/null | grep -q ":$LOCAL_PORT"; then
  echo "ERROR: forward が listen していない。ログ:"
  tail -20 /root/usbmux-forward.log
  exit 1
fi
echo "forward ready: 127.0.0.1:$LOCAL_PORT -> device:$DEVICE_PORT"

python3 "$PROBE" --host 127.0.0.1 --port "$LOCAL_PORT" "$@"
status=$?
pkill -f "usbmux forward" 2>/dev/null
exit $status
