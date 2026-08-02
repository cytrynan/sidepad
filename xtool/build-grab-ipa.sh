#!/bin/bash
# sidepad: xtool dev にビルド→署名→パッケージまでやらせ、
# Connecting ハング中に署名済み ipa を横取りして kill する。
# 出力: /root/rescue/Sidepad-signed.ipa（成功時 GRABBED を表示して exit 0）
set -u
export PATH="/opt/swift/usr/bin:$PATH"
cd /root/sidepad || exit 1

rm -rf /root/.cache/xtool/tmp-staging-*
mkdir -p /root/rescue
pkill -f "xtool dev" 2>/dev/null
sleep 1

(printf "y\n" | script -qec "xtool dev" /dev/null >/root/xtool-auto.log 2>&1 &)

# 署名済み ipa の出現を待つ（ビルド+署名+パッケージで通常 30 秒前後）
ipa=""
for i in $(seq 1 180); do
  ipa=$(ls /root/.cache/xtool/tmp-staging-*/staging/app.ipa 2>/dev/null | head -1)
  [ -n "$ipa" ] && break
  sleep 1
done
if [ -z "$ipa" ]; then
  echo "ERROR: staged ipa not found (xtool log tail:)"
  tail -5 /root/xtool-auto.log
  pkill -f "xtool dev" 2>/dev/null
  exit 1
fi

# 書き込み完了待ち（サイズが 2 秒間安定するまで）
prev=-1
for i in $(seq 1 30); do
  cur=$(stat -c %s "$ipa" 2>/dev/null || echo -2)
  [ "$cur" = "$prev" ] && [ "$cur" -gt 0 ] && break
  prev=$cur
  sleep 2
done

cp "$ipa" /root/rescue/Sidepad-signed.ipa
pkill -f "xtool dev" 2>/dev/null
echo "GRABBED size=$(stat -c %s /root/rescue/Sidepad-signed.ipa)"
