#!/usr/bin/env bash
# AnnexB.swift の単体検証を swift インタプリタで実行する。
# WSL(Ubuntu-24.04 + /opt/swift) と Git Bash(Windows Swift toolchain) の両方で動く。
# 合格 = "PASS" 出力 + exit 0
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v swift >/dev/null 2>&1; then
    # WSL の非対話 bash は profile を読まず swift が PATH に無い（既知の罠）
    export PATH="/opt/swift/usr/bin:$PATH"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cat Sources/Sidepad/AnnexB.swift xtool/tests/annexb_main.swift > "$tmp/annexb_combined.swift"
# swift 直接実行(JIT)は Windows で FORCE_LOAD シンボル欠落で落ちる（2026-08-06 実測）
# → コンパイルして実行する（WSL/Windows 共通で安定）
swiftc -o "$tmp/annexb_test" "$tmp/annexb_combined.swift"
"$tmp/annexb_test"
