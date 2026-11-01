#!/bin/sh
# test.sh -- iwac の検証をまとめて行う。
# (ツールのインストールはしない。それは doc/environment.md の範囲)
#
# 検証の考え方 (設計書 doc/design.md の「検証の方針」):
#   ベアメタル実行の確認は QEMU のシリアル出力をファイルに落とし、
#   「プログラムの結果の1バイトがログの最後に出るか」で見る。
#   OpenSBI の起動表示が前に混ざるが、結果のバイトは最後に
#   追加されるので、最後の1バイトを見れば判定できる。
#
# 検証内容:
#   v0: iwac A と iwac 65 が同じ16バイトを出し、実行すると A が出る
#   v1: 式 65+1 → B、66-1 → A、50+10+5 → A が実行時に計算されて出る
#       空白ありの式が空白なしと同じバイナリになる
#       文法エラー (65+ / 65*2 / 2048) では失敗し、出力が空になる
set -e
cd "$(dirname "$0")"

CC=${CC:-gcc}
QEMU=${QEMU:-qemu-system-riscv64}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "build: CC=$CC"
"$CC" -std=c11 -Wall -Wextra -O2 -o "$TMP/iwac" iwac.c
IWAC="$TMP/iwac"

# 約束の確認: iwac.c にプリプロセッサ行 (#include や #define
# など、行頭の #) が無いこと。「まだ作っていないものを
# 使って作らない」という決まり (doc/design.md 6章)
if grep -q '^[[:space:]]*#' iwac.c; then
    echo "FAIL: iwac.c にプリプロセッサ行がある" >&2
    exit 1
fi
echo "ok: iwac.c にプリプロセッサ行が無い"

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

# --- バイナリを指定して QEMU で実行し、シリアルの最後の1バイトを
#     期待値と比べる ---
check_run() {
    bin=$1; expect=$2; label=$3
    timeout 3 "$QEMU" -machine virt -bios default -kernel "$bin" \
        -m 128M -display none -monitor none -serial "file:$TMP/serial.log" \
        >/dev/null 2>&1 || true
    last=$(tail -c 1 "$TMP/serial.log")
    if [ "$last" != "$expect" ]; then
        fail "$label: 最後の1バイトが '$last' (期待は '$expect')"
    fi
    echo "ok: $label → '$expect' が出た"
}

# --- v0 の確認 ---
"$IWAC" A > "$TMP/a.bin"
"$IWAC" 65 > "$TMP/a65.bin"
size=$(wc -c < "$TMP/a.bin")
[ "$size" -eq 16 ] || fail "a.bin は ${size} バイト (16のはず)"
cmp "$TMP/a.bin" "$TMP/a65.bin"
echo "ok: 'A' と '65' が同じ16バイトを出した"
check_run "$TMP/a.bin" A "v0: 'A'"

# --- v1: 式の確認 ---
"$IWAC" '65+1' > "$TMP/expr.bin"
size=$(wc -c < "$TMP/expr.bin")
[ "$size" -eq 24 ] || fail "65+1 のバイナリは ${size} バイト (24のはず)"
check_run "$TMP/expr.bin" B "v1: '65+1'"

"$IWAC" '66-1' > "$TMP/expr2.bin"
check_run "$TMP/expr2.bin" A "v1: '66-1'"

"$IWAC" '50+10+5' > "$TMP/expr3.bin"
check_run "$TMP/expr3.bin" A "v1: '50+10+5'"

# 空白を挟んでも同じバイナリになること
"$IWAC" '65 + 1' > "$TMP/expr-sp.bin"
cmp "$TMP/expr.bin" "$TMP/expr-sp.bin"
echo "ok: 空白あり/なしで同じバイナリになった"

# --- v1: エラーの確認 ---
# 文法エラーのときは iwac が失敗し、標準出力 (バイナリ) が
# 空のままになること。「解析が全部済んでから生成する」設計の確認。
check_error() {
    prog=$1; label=$2
    if "$IWAC" "$prog" > "$TMP/err.bin" 2>/dev/null; then
        fail "$label: エラーになるはずが成功してしまった"
    fi
    [ ! -s "$TMP/err.bin" ] || fail "$label: エラーなのにバイナリが出力された"
    echo "ok: $label はエラーになり、出力は空だった"
}
check_error '65+' "'65+'"
check_error '65*2' "'65*2' (v1 では * は未対応)"
check_error '2048' "'2048' (addi 即値の上限超え)"
check_error '' "空文字列"

echo "PASS"
