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
#   v2: 変数と代入 (x=65; x+1 → B、2変数の加算、再代入、長い変数名)、
#       未代入の変数や変数5個では失敗し、出力が空になる
#   v3: 文字列とポインタ (s="Hi"; *s → H、s=s+1 で次を読む、
#       *s+1 の計算、文字列2つ)、閉じていない文字列などは失敗
#   v4: while と putc で print loop 本体 (文字列を丸ごと出す)、
#       カウントするループ、条件が偽なら素通り、入れ子のループ
#   v5: 関数定義と呼び出し (引数1つ、入れ子の呼び出し)、
#       本物の uart_puts の形、比較 == / != のループ
#   v6: 本物の uart_putc 本体 (LSR のポーリング)、絶対アドレス
#       への書き込み、ビット演算 &、16進数、ポインタ経由の書き込み
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

# --- v2: 変数と代入、文の並び ---
"$IWAC" 'x=65; x+1' > "$TMP/v2a.bin"
check_run "$TMP/v2a.bin" B "v2: 'x=65; x+1'"

"$IWAC" 'x=60; y=5; x+y' > "$TMP/v2b.bin"
check_run "$TMP/v2b.bin" A "v2: 'x=60; y=5; x+y'"

"$IWAC" 'x=65; x=x+1; x' > "$TMP/v2c.bin"
check_run "$TMP/v2c.bin" B "v2: 'x=65; x=x+1; x' (再代入)"

"$IWAC" 'abc=66; abc' > "$TMP/v2d.bin"
check_run "$TMP/v2d.bin" B "v2: 'abc=66; abc' (長い変数名)"

check_error 'y+1' "'y+1' (代入されていない変数)"
check_error 'x=x+1; x' "'x=x+1; x' (初回代入での自己参照)"
check_error 'a=1; b=2; c=3; d=4; e=5; a' "変数5個 (上限は4個)"

# --- v3: ポインタと文字列 ---
"$IWAC" 's="Hi"; *s' > "$TMP/v3a.bin"
check_run "$TMP/v3a.bin" H "v3: 's=\"Hi\"; *s'"

"$IWAC" 's="Hi"; s=s+1; *s' > "$TMP/v3b.bin"
check_run "$TMP/v3b.bin" i "v3: 's=\"Hi\"; s=s+1; *s' (ポインタを進める)"

"$IWAC" 's="AB"; *s+1' > "$TMP/v3c.bin"
check_run "$TMP/v3c.bin" B "v3: 's=\"AB\"; *s+1' (読んだ文字で計算)"

"$IWAC" 's="A"; t="B"; *t' > "$TMP/v3d.bin"
check_run "$TMP/v3d.bin" B "v3: 's=\"A\"; t=\"B\"; *t' (文字列2つ)"

check_error 's="Hi; *s' "文字列が閉じていない"
# v6 で '*5' は「アドレス5を読む」として意味を持つようになった
# (絶対アドレス読み)。エラーではなくなったのでここには無い

# --- v4: while と putc (print loop の本体) ---
# 複数バイトの出力は「シリアルの末尾が期待の文字列と一致するか」で見る
check_tail() {
    bin=$1; expect=$2; label=$3
    timeout 3 "$QEMU" -machine virt -bios default -kernel "$bin" \
        -m 128M -display none -monitor none -serial "file:$TMP/serial.log" \
        >/dev/null 2>&1 || true
    n=${#expect}
    last=$(tail -c "$n" "$TMP/serial.log")
    if [ "$last" != "$expect" ]; then
        fail "$label: 末尾が '$last' (期待は '$expect')"
    fi
    echo "ok: $label → '$expect' が出た"
}

"$IWAC" 'putc(72); putc(105);' > "$TMP/v4a.bin"
check_tail "$TMP/v4a.bin" 'Hi' "v4: 'putc(72); putc(105);'"

"$IWAC" 'i=3; while (i) { putc(65); i=i-1; }' > "$TMP/v4b.bin"
check_tail "$TMP/v4b.bin" 'AAA' "v4: カウントする while"

"$IWAC" 'x=0; while (x) { putc(88); } 65' > "$TMP/v4c.bin"
check_run "$TMP/v4c.bin" A "v4: 条件が最初から偽なら本体は動かない"

# 目標の print loop 本体: 文字列を終わり (0) まで1文字ずつ出す
"$IWAC" 's="hello"; while (*s) { putc(*s); s = s + 1; }' > "$TMP/v4d.bin"
check_tail "$TMP/v4d.bin" 'hello' "v4: print loop 'while (*s) { putc(*s); s = s + 1; }'"

"$IWAC" 'i=2; while (i) { j=2; while (j) { putc(66); j=j-1; } i=i-1; }' > "$TMP/v4e.bin"
check_tail "$TMP/v4e.bin" 'BBBB' "v4: while の入れ子"

check_error 'putc(65)' "'putc(65)' (; が無い)"
check_error 'while (1) { putc(65);' "閉じていない while"

# --- v5: 関数と比較 ---
"$IWAC" 'void out(char c) { putc(c); } out(65); out(66);' > "$TMP/v5a.bin"
check_tail "$TMP/v5a.bin" 'AB' "v5: 関数の定義と呼び出し"

"$IWAC" 'void out2(char c) { putc(c); putc(c); } void wrap(char c) { out2(c); } wrap(88);' > "$TMP/v5b.bin"
check_tail "$TMP/v5b.bin" 'XX' "v5: 関数が関数を呼ぶ (ra の退避)"

# 目標の形そのもの: 本物の uart_puts (型・引数・!= 0 付き)
"$IWAC" 'void uart_puts(char *s) { while (*s != 0) { putc(*s); s = s + 1; } } msg = "hello"; uart_puts(msg);' > "$TMP/v5c.bin"
check_tail "$TMP/v5c.bin" 'hello' "v5: uart_puts の形で hello"

"$IWAC" 'x=3; while (x != 0) { putc(65); x = x - 1; }' > "$TMP/v5d.bin"
check_tail "$TMP/v5d.bin" 'AAA' "v5: 比較 != のループ"

"$IWAC" 'y=0; while (y == 0) { putc(66); y = 1; }' > "$TMP/v5e.bin"
check_tail "$TMP/v5e.bin" 'B' "v5: 比較 == のループ"

check_error 'foo(65);' "定義されていない関数の呼び出し"
check_error 'void f() { } f(65);' "引数の数が合わない呼び出し"

# --- v6: デバイスに触る部品 (絶対アドレス・& ・メモリ書き込み) ---
"$IWAC" 'void uart_putc(char c) { while (*0x10000005 & 32 == 0) { } *0x10000000 = c; } void uart_puts(char *s) { while (*s != 0) { uart_putc(*s); s = s + 1; } } msg = "hello"; uart_puts(msg);' > "$TMP/v6a.bin"
check_tail "$TMP/v6a.bin" 'hello' "v6: 本物の uart_putc 本体 (ポーリング付き)"

"$IWAC" '*0x10000000 = 65; *0x10000000 = 66;' > "$TMP/v6b.bin"
check_tail "$TMP/v6b.bin" 'AB' "v6: 絶対アドレスへの書き込み文"

"$IWAC" 'x = 6 & 3; putc(64 + x);' > "$TMP/v6c.bin"
check_tail "$TMP/v6c.bin" 'B' "v6: ビット演算 & (6 & 3 = 2)"

"$IWAC" 'putc(0x41);' > "$TMP/v6d.bin"
check_tail "$TMP/v6d.bin" 'A' "v6: 16進数リテラル"

"$IWAC" 'p = 0x10000000; *p = 88;' > "$TMP/v6e.bin"
check_tail "$TMP/v6e.bin" 'X' "v6: ポインタ変数経由の書き込み"

check_error 'x = 5 && 3;' "&& は未対応"
check_error '*0x = 65;' "0x の後ろに数字が無い"

echo "PASS"
