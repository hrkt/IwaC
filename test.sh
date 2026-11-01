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
#       文法エラー (65+ / 2048 など) では失敗し、出力が空になる
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
#   v7: 優先順位を C に合わせる (乗除が先、== が & より先)、
#       かっこ、両側が式になる計算 (スタック評価)
#   v8: if / else / else if (while の中や関数の中でも)
#   v9: 配列 (宣言・要素の読み書き・先頭アドレス・関数へ渡す)
#   v10: ファイル入力 (iwac hello.c の形、改行・// コメント)
#   v11: 関係演算子 < > <= >= (優先順位は C の段)
#   v12: main の形 (入口は main、混ぜ書きは禁止)
#   v13: 単純な #define (単語単位の置き換え、文字列・コメントは対象外)
#   v14: KosabaOS ターゲット (--kosaba: ELF/0x40000000/syscall)
#   v15: iwac 本体の KosabaOS 版 (kosaba/ の crt+shim でビルド)
#   v16: 字句の完成 (ブロックコメント/文字リテラル/型の語彙)
#   v17: 複数ファイル入力 (順につないで1つのソースとして読む)
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
# '65*2' は v7 で掛け算として意味を持つようになった (下の v7 で確認)
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
"$IWAC" 'void uart_putc(char c) { while ((*0x10000005 & 32) == 0) { } *0x10000000 = c; } void uart_puts(char *s) { while (*s != 0) { uart_putc(*s); s = s + 1; } } msg = "hello"; uart_puts(msg);' > "$TMP/v6a.bin"
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

# --- v7: 優先順位を C に合わせる (かっこ・乗除・スタック評価) ---
"$IWAC" '60+2*3' > "$TMP/v7a.bin"
check_tail "$TMP/v7a.bin" 'B' "v7: 掛け算が先 (60+2*3 = 66)"

"$IWAC" '2*(30+3)' > "$TMP/v7b.bin"
check_tail "$TMP/v7b.bin" 'B' "v7: かっこで順序を変える"

"$IWAC" '(50 + 10) * (1 + 1)' > "$TMP/v7c.bin"
check_tail "$TMP/v7c.bin" 'x' "v7: 両側が式の計算 (スタック評価)"

"$IWAC" '100/4+40' > "$TMP/v7d.bin"
check_tail "$TMP/v7d.bin" 'A' "v7: 割り算 (100/4+40 = 65)"

"$IWAC" 'x = 1 & 2 == 2; putc(64 + x);' > "$TMP/v7e.bin"
check_tail "$TMP/v7e.bin" 'A' "v7: C の順では == が & より先 (1 & (2==2) = 1)"

check_error '(65' "かっこが閉じていない"

# --- v8: if / else ---
"$IWAC" 'x = 5; if (x == 5) { putc(65); }' > "$TMP/v8a.bin"
check_tail "$TMP/v8a.bin" 'A' "v8: if が成り立つ"

"$IWAC" 'x = 4; if (x == 5) { putc(65); } else { putc(66); }' > "$TMP/v8b.bin"
check_tail "$TMP/v8b.bin" 'B' "v8: if/else の else 側"

"$IWAC" 'x = 2; if (x == 1) { putc(65); } else if (x == 2) { putc(66); } else { putc(67); }' > "$TMP/v8c.bin"
check_tail "$TMP/v8c.bin" 'B' "v8: else if の連なり"

"$IWAC" 'i = 3; while (i) { if (i == 2) { putc(66); } else { putc(65); } i = i - 1; }' > "$TMP/v8d.bin"
check_tail "$TMP/v8d.bin" 'ABA' "v8: while の中の if/else"

"$IWAC" 'void f(char c) { if (c == 65) { putc(97); } else { putc(98); } } f(65); f(66);' > "$TMP/v8e.bin"
check_tail "$TMP/v8e.bin" 'ab' "v8: 関数の中の if"

check_error 'if (1) putc(65);' "if の本体に { } が無い"
check_error 'if (1) { putc(65);' "if の本体が閉じていない"

# --- v9: 配列 ---
"$IWAC" 'char b[4]; b[0] = 65; b[1] = 66; putc(b[0]); putc(b[1]);' > "$TMP/v9a.bin"
check_tail "$TMP/v9a.bin" 'AB' "v9: 配列の要素の読み書き"

"$IWAC" 'char b[4]; i = 1; b[i] = 67; b[i + 1] = 68; putc(b[1]); putc(b[2]);' > "$TMP/v9b.bin"
check_tail "$TMP/v9b.bin" 'CD' "v9: 計算した添字"

"$IWAC" 'char b[4]; b[0] = 40 + 25; putc(b[0]);' > "$TMP/v9c.bin"
check_tail "$TMP/v9c.bin" 'A' "v9: 書く値が式の書き込み"

"$IWAC" 'char b[4]; p = b; *p = 69; putc(b[0]);' > "$TMP/v9d.bin"
check_tail "$TMP/v9d.bin" 'E' "v9: 配列の先頭アドレスをポインタへ"

"$IWAC" 'void uart_puts(char *s) { while (*s != 0) { putc(*s); s = s + 1; } } char buf[8]; buf[0] = 72; buf[1] = 105; buf[2] = 0; uart_puts(buf);' > "$TMP/v9e.bin"
check_tail "$TMP/v9e.bin" 'Hi' "v9: 配列を関数へ渡す"

check_error 'char b[0];' "配列の大きさ 0"
check_error 'char b[2]; char b[3];' "配列の二重宣言"
check_error 'c[0] = 65;' "宣言していない配列への書き込み"

# --- v10: ファイル入力 (改行・コメント入りのソース) ---
cat > "$TMP/prog.c" <<'EOF'
// コメントと改行のあるプログラム
void uart_puts(char *s) {
    while (*s != 0) {
        putc(*s);
        s = s + 1;  // 次の文字へ
    }
}
uart_puts("file");
EOF
"$IWAC" "$TMP/prog.c" > "$TMP/v10.bin"
check_tail "$TMP/v10.bin" 'file' "v10: ファイル入力 (改行・コメント)"

# リポジトリ同梱の examples/hello.c が Hello, World! を出す
# (末尾の改行はシェルの変数代入で消えてしまうので、
#  check_tail ではなくログ全体から探して確かめる)
"$IWAC" examples/hello.c > "$TMP/hello.bin"
timeout 3 "$QEMU" -machine virt -bios default -kernel "$TMP/hello.bin" \
    -m 128M -display none -monitor none -serial "file:$TMP/serial.log" \
    >/dev/null 2>&1 || true
if grep -q 'Hello, World!' "$TMP/serial.log"; then
    echo "ok: v10: examples/hello.c → 'Hello, World!' が出た"
else
    fail "v10: examples/hello.c: ログに Hello, World! が無い"
fi

check_error_file() {
    if "$IWAC" "$1" > "$TMP/err.bin" 2> "$TMP/err.txt"; then
        echo "FAIL: $2 (成功してしまった)"; exit 1
    fi
    if [ -s "$TMP/err.bin" ]; then
        echo "FAIL: $2 (エラーなのに出力がある)"; exit 1
    fi
    echo "ok: $2 はエラーになり、出力は空だった"
}
check_error_file "$TMP/no-such-file.c" "存在しない .c ファイル"

# --- v12: main の形 ---
"$IWAC" 'int main() { putc(65); putc(66); }' > "$TMP/v12a.bin"
check_tail "$TMP/v12a.bin" 'AB' "v12: main のみ"

"$IWAC" 'void uart_puts(char *s) { while (*s != 0) { putc(*s); s = s + 1; } } int main() { uart_puts("main!"); }' > "$TMP/v12b.bin"
check_tail "$TMP/v12b.bin" 'main!' "v12: main から関数を呼ぶ"

check_error 'int main() { putc(65); } putc(66);' "main と外の文の混ぜ書き"
check_error 'int main(int x) { putc(65); }' "main の引数"

# --- v11: 関係演算子 ---
"$IWAC" 'i = 0; while (i < 3) { putc(65); i = i + 1; }' > "$TMP/v11a.bin"
check_tail "$TMP/v11a.bin" 'AAA' "v11: while (i < 3) のループ"

"$IWAC" 'if (5 > 3) { putc(65); } if (5 <= 5) { putc(66); } if (4 >= 5) { putc(88); } else { putc(67); }' > "$TMP/v11b.bin"
check_tail "$TMP/v11b.bin" 'ABC' "v11: > <= >="

"$IWAC" 'putc(48 + (1 < 2 == 1));' > "$TMP/v11c.bin"
check_run "$TMP/v11c.bin" 1 "v11: 優先順位 (1 < 2 == 1 は (1<2)==1)"

# --- v13: 単純な #define ---
cat > "$TMP/def.c" <<'EOF'
#define UART_LSR 0x10000005
#define THRE 32
void uart_putc(char c) { while ((*UART_LSR & THRE) == 0) { } *0x10000000 = c; }
uart_putc(66);
EOF
"$IWAC" "$TMP/def.c" > "$TMP/v13a.bin"
check_run "$TMP/v13a.bin" B "v13: #define で定数に名前"

"$IWAC" '#define ABC 90
putc(ABC + 1);' > "$TMP/v13b.bin"
check_run "$TMP/v13b.bin" '[' "v13: 式の中での展開"

check_error '#define A 65
#define A 66
putc(A);' "#define の再定義"
check_error '#include "x.h"
putc(65);' "#include は対象外"

# --- v14: KosabaOS ターゲット (--kosaba) ---
# ELF の構造を readelf で確かめる (OS 上での実行確認は
# KosabaOS 側との統合検証として別途実施。doc/progress.md)
"$IWAC" --kosaba examples/hello-kosaba.c > "$TMP/k.elf"
readelf -h "$TMP/k.elf" > "$TMP/k.hdr"
grep -q 'Class:.*ELF64' "$TMP/k.hdr" || fail "v14: ELF64 でない"
grep -q 'Type:.*EXEC' "$TMP/k.hdr" || fail "v14: ET_EXEC でない"
grep -q 'Machine:.*RISC-V' "$TMP/k.hdr" || fail "v14: RISC-V でない"
grep -q 'Entry point address:.*0x40000000' "$TMP/k.hdr" || fail "v14: 入口が 0x40000000 でない"
readelf -l "$TMP/k.elf" | grep 'LOAD' | grep -q '0x0000000040000000' || fail "v14: PT_LOAD の vaddr が違う"
echo "ok: v14: --kosaba の ELF (ELF64/EXEC/RISC-V/入口 0x40000000)"

# --- v15: iwac 本体の KosabaOS 版ビルド ---
# RISC-V 用 gcc が見つかるときだけ、kosaba 版 iwac.elf が
# 作れることを確かめる (OS 上での実行確認は統合検証として
# 別途実施。doc/progress.md)。見つからなければ飛ばす
RISCV_CC=""
if [ -n "${RISCV_CC_ENV:-}" ]; then
    RISCV_CC="$RISCV_CC_ENV"
elif [ -x ../../kosaba-os/tools/riscv-gcc/bin/riscv-none-elf-gcc ]; then
    RISCV_CC=$(cd ../../kosaba-os/tools/riscv-gcc/bin && pwd)/riscv-none-elf-gcc
elif command -v riscv64-linux-gnu-gcc >/dev/null 2>&1; then
    RISCV_CC=riscv64-linux-gnu-gcc
fi
if [ -n "$RISCV_CC" ]; then
    (cd kosaba && CC="$RISCV_CC" ./build-kosaba.sh >/dev/null 2>&1) || fail "v15: kosaba 版のビルドに失敗"
    readelf -h kosaba/iwac.elf | grep -q 'Entry point address:.*0x40000000' || fail "v15: iwac.elf の入口が違う"
    echo "ok: v15: kosaba 版 iwac.elf がビルドできた (入口 0x40000000)"
else
    echo "skip: v15 (RISC-V 用 gcc が無いため)"
fi

# --- v16: 字句の完成 ---
"$IWAC" '/* 注 */ putc(65); /* 複数
行 */' > "$TMP/v16a.bin"
check_run "$TMP/v16a.bin" A "v16: ブロックコメント"
"$IWAC" "putc('B');" > "$TMP/v16b.bin"
check_run "$TMP/v16b.bin" B "v16: 文字リテラル"
"$IWAC" "putc('\n' - 10 + 67);" > "$TMP/v16c.bin"
check_run "$TMP/v16c.bin" C "v16: エスケープ文字リテラル"
"$IWAC" 'long f(int a) { putc(68); } f(1);' > "$TMP/v16d.bin"
check_run "$TMP/v16d.bin" D "v16: long の型"
"$IWAC" 'unsigned char buf[2]; buf[0] = 69; putc(buf[0]);' > "$TMP/v16e.bin"
check_run "$TMP/v16e.bin" E "v16: unsigned char の配列"
# --- v17: 複数ファイル入力 ---
printf 'void kputs(char *s) { while (*s != 0) { putc(*s); s = s + 1; } }\n' > "$TMP/lib.c"
printf 'int main() { kputs("Hi"); }' > "$TMP/main.c"
"$IWAC" "$TMP/lib.c" "$TMP/main.c" > "$TMP/v17.bin"
check_run "$TMP/v17.bin" i "v17: 複数ファイル (lib + main)"
if "$IWAC" "$TMP/lib.c" "$TMP/nosuch.c" > "$TMP/err.bin" 2>/dev/null; then
    fail "v17: 開けないファイルでエラーになるはずが成功した"
fi
[ ! -s "$TMP/err.bin" ] || fail "v17: エラーなのにバイナリが出た"
echo "ok: v17: 開けないファイルはエラーになり、出力は空だった"

check_error "putc('');" "空の文字リテラル"
check_error '/* 終わらない' "閉じないブロックコメント"
check_error "putc('AB');" "2文字の文字リテラル"

echo "PASS"
