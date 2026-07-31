#!/bin/bash
# xtool-doctor: check how far the xtool pipeline is set up.
# Run inside WSL:  wsl -d Ubuntu-24.04 bash /root/xtool-doctor.sh
export PATH=/opt/swift/usr/bin:$PATH
ok(){ echo "  [OK]   $*"; }
ng(){ echo "  [TODO] $*"; }

echo "=============================================="
echo " xtool doctor"
echo "=============================================="

echo ""
echo "-- 1. toolchain --"
command -v swift >/dev/null && ok "swift: $(swift --version 2>&1|head -1)" || ng "swift missing"
command -v xtool >/dev/null && ok "xtool: $(xtool --version 2>&1)"        || ng "xtool missing"

echo ""
echo "-- 2. Apple auth (K action: xtool auth login) --"
A=$(xtool auth status 2>&1)
[ "$A" = "Logged out" ] && ng "auth: $A  -> run: xtool auth login" || ok "auth: $A"

echo ""
echo "-- 3. Darwin SDK (K action: needs Xcode .xip) --"
S=$(xtool sdk status 2>&1)
[ "$S" = "Not installed" ] && ng "sdk: $S  -> run: xtool sdk install ~/Xcode.xip" || ok "sdk: $S"
echo "     swift sdk list: $(swift sdk list 2>&1 | tr '\n' ' ')"

echo ""
echo "-- 4. USB / device stack --"
modprobe vhci-hcd 2>/dev/null
[ -d /sys/devices/platform/vhci_hcd.0 ] && ok "vhci_hcd loaded (USB/IP client ready)" || ng "vhci_hcd NOT available"
pgrep -x usbmuxd >/dev/null && ok "usbmuxd running (pid $(pgrep -x usbmuxd|head -1))" || { ng "usbmuxd not running -> starting"; (usbmuxd 2>/dev/null &); sleep 2; pgrep -x usbmuxd >/dev/null && ok "usbmuxd started"; }
echo "     lsusb:"
lsusb 2>&1 | sed 's/^/       /'
D=$(timeout 10 idevice_id -l 2>&1)
[ -n "$D" ] && ok "iOS device seen: $D" || ng "no iOS device (needs usbipd attach from Windows)"

echo ""
echo "-- 5. xtool devices --"
timeout 12 xtool devices --no-wait 2>&1 | sed 's/^/     /'

echo ""
echo "-- disk --"
df -h / | tail -1 | sed 's/^/     /'
echo ""
echo "Next unfinished step is the first [TODO] above."
