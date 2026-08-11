#!/usr/bin/env bash
# TouchWire（TouchInput.swift）の単体検証を swiftc でコンパイル・実行する。
# run-annexb-tests.sh と同じ形。WSL(Ubuntu-24.04 + /opt/swift) と
# Git Bash(Windows Swift toolchain) の両方で動く。
# 合格 = "PASS" 出力 + exit 0
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v swiftc >/dev/null 2>&1; then
    # WSL の非対話 bash は profile を読まず swift が PATH に無い（既知の罠）
    export PATH="/opt/swift/usr/bin:$PATH"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cat Sources/Sidepad/ProbeProtocol.swift Sources/Sidepad/TouchInput.swift xtool/tests/touchwire_main.swift \
    > "$tmp/touchwire_combined.swift"
# swift 直接実行(JIT)は Windows で FORCE_LOAD シンボル欠落で落ちる（2026-08-06 annexb実測と同じ回避）
swiftc -o "$tmp/touchwire_test" "$tmp/touchwire_combined.swift"
"$tmp/touchwire_test"
