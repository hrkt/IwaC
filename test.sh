#!/bin/sh
# test.sh -- iwac v0 の検証をまとめて行う。
# (ツールのインストールはしない。それは doc/environment.md の範囲)
#
# 検証内容:
#   1. gcc で iwac がビルドできる
#   2. iwac A と iwac 65 が同じ16バイトのバイナリを出す
#   3. QEMU virt (ベアメタル) で実行すると、UART (シリアル) の
#      最後の1バイトが A / B になる (A用とB用でログがその1バイト
#      だけ違うことも確認する)
set -e
cd "$(dirname "$0")"

CC=${CC:-gcc}
QEMU=${QEMU:-qemu-system-riscv64}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "build: CC=$CC"
"$CC" -std=c11 -Wall -Wextra -O2 -o "$TMP/iwac" iwac.c

"$TMP/iwac" A > "$TMP/a.bin"
"$TMP/iwac" 65 > "$TMP/a65.bin"
"$TMP/iwac" B > "$TMP/b.bin"

size=$(wc -c < "$TMP/a.bin")
if [ "$size" -ne 16 ]; then
    echo "FAIL: a.bin は ${size} バイト (16のはず)" >&2
    exit 1
fi
cmp "$TMP/a.bin" "$TMP/a65.bin"
echo "ok: 'A' と '65' が同じ16バイトを出した"

run_one() {
    timeout 3 "$QEMU" -machine virt -bios default -kernel "$1" \
        -m 128M -display none -monitor none -serial "file:$2" \
        >/dev/null 2>&1 || true
}
run_one "$TMP/a.bin" "$TMP/serial-a.log"
run_one "$TMP/b.bin" "$TMP/serial-b.log"

if [ "$(tail -c 1 "$TMP/serial-a.log")" != "A" ]; then
    echo "FAIL: シリアルの最後の1バイトが A でない" >&2
    exit 1
fi
if [ "$(tail -c 1 "$TMP/serial-b.log")" != "B" ]; then
    echo "FAIL: シリアルの最後の1バイトが B でない" >&2
    exit 1
fi
# ログ同士は最後の1バイトだけが違うはず (OpenSBIの表示は同一)
if cmp -s "$TMP/serial-a.log" "$TMP/serial-b.log"; then
    echo "FAIL: A用とB用のシリアルログが同一" >&2
    exit 1
fi
echo "ok: QEMUで実行して UART に A / B が出た"
echo "PASS"
