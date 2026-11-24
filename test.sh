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
#       文法エラー (65+ など) では失敗し、出力が空になる
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
#   v18: 条件付きコンパイル (#ifdef/#ifndef/#else/#endif、定義済みマクロ)
#   v19: 演算子の完成 (<< >> | ^ %、単項 - !、&& || 短絡、前置/後置 ++ --)
#   v20: 制御構文の完成 (for、break、continue、値なし早期 return)
#   v21: 変数のメモリ化 (グローバル=静的領域、ローカル・引数=フレーム、宣言の文、単項 &)
#   v22: 関数の完成 (引数4つ、戻り値、再帰、main の引数と決め打ち argv)
#   v23: 配列の完成 (8バイト要素、2次元配列、ポインタの添字)
#   v24: グローバル宣言とプロトタイプ (初期化子は初期イメージ、宣言だけの関数、ELF のデータセグメント分離)
#   v25: 容量の調整と小さい構文の追随 (実測で決めた表の定数、static、引数8つ、三項、キャスト、複合代入、要素の増減、単一文本体)
#   v26: 配列のスコープとローカル配列 (同名を別関数で宣言、隠蔽、2次元、再帰、フレーム配置)
#   v27: セルフホスト (syscall3、kosaba_rt.c、分岐の緩和、大きな定数と *p の幅、前処理の文字リテラル、セルフコンパイル)
set -e
cd "$(dirname "$0")"

CC=${CC:-gcc}
QEMU=${QEMU:-qemu-system-riscv64}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "build: CC=$CC"
cat src/0*.c > "$TMP/iwac_all.c"
"$CC" -std=c11 -Wall -Wextra -O2 -o "$TMP/iwac" "$TMP/iwac_all.c"
IWAC="$TMP/iwac"

# 約束の確認: iwac 本体 (src/ の断片) にプリプロセッサ行
# (#include や #define など、行頭の #) が無いこと。
# 「まだ作っていないものを使って作らない」という決まり
# (doc/design.md 6章)
if grep -q '^[[:space:]]*#' src/0*.c; then
    echo "FAIL: iwac 本体 (src/) にプリプロセッサ行がある" >&2
    exit 1
fi
echo "ok: iwac 本体 (src/) にプリプロセッサ行が無い"

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
# v55 で即値形 (addi) になり1命令減って 20 バイト
[ "$size" -eq 20 ] || fail "65+1 のバイナリは ${size} バイト (20のはず)"
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
# '2048' は v25 で大きな数として意味を持つようになった (下の v25 で確認)
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
"$IWAC" 'a=1; b=2; c=3; d=4; e=5; putc(64 + a + b + c + d + e);' > "$TMP/v21vars.bin"
check_run "$TMP/v21vars.bin" O "v21: 変数5個が使える (レジスタ時代の上限4は廃止)"

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

"$IWAC" 'x = 5 && 3; putc(64 + x);' > "$TMP/v19and.bin"
check_run "$TMP/v19and.bin" A "v19: && が使える (v6 当時は未対応だった)"
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

# 'if (1) putc(65);' は v25 で単一文の本体として意味を持つようになった (下の v25 で確認)
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
# main の引数は v22 から許可 (v12 時点ではエラーだった)

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
check_error '#pragma once
putc(65);' "知らない指令"

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

# --- v18: 条件付きコンパイル ---
"$IWAC" '#define FOO
#ifdef FOO
putc(72);
#else
putc(66);
#endif' > "$TMP/v18a.bin"
check_run "$TMP/v18a.bin" H "v18: #ifdef 定義あり"
"$IWAC" '#ifdef FOO
putc(72);
#else
putc(66);
#endif' > "$TMP/v18b.bin"
check_run "$TMP/v18b.bin" B "v18: #ifdef 未定義は else"
"$IWAC" '#ifndef FOO
putc(72);
#endif' > "$TMP/v18c.bin"
check_run "$TMP/v18c.bin" H "v18: #ifndef"
"$IWAC" '#ifdef NOPE
#define X 66
#else
#define X 65
#endif
putc(X);' > "$TMP/v18d.bin"
check_run "$TMP/v18d.bin" A "v18: 無効範囲の #define は効かない"
"$IWAC" '#ifdef __KOSABA__
putc(75);
#else
putc(66);
#endif' > "$TMP/v18e.bin"
check_run "$TMP/v18e.bin" B "v18: ベアでは __KOSABA__ 未定義"
printf '#ifdef __KOSABA__\nputc(75);\n#else\nputc(66);\n#endif\n' > "$TMP/v18.c"
"$IWAC" --kosaba "$TMP/v18.c" > "$TMP/v18k.elf"
"$IWAC" --kosaba 'putc(75);' > "$TMP/v18k2.elf"
cmp -s "$TMP/v18k.elf" "$TMP/v18k2.elf" || fail "v18: --kosaba では __KOSABA__ の枝になるはず"
echo "ok: v18: --kosaba では __KOSABA__ の枝になった"
check_error '#else
putc(65);' "#else だけ"
check_error '#ifdef FOO
putc(65);' "#endif が無い"

check_error "putc('');" "空の文字リテラル"
check_error '/* 終わらない' "閉じないブロックコメント"
check_error "putc('AB');" "2文字の文字リテラル"

# --- v19: 演算子の完成 ---
"$IWAC" 'putc(64 + (1 << 2));' > "$TMP/v19a.bin"
check_run "$TMP/v19a.bin" D "v19: 左シフト << (64 + (1<<2) = 68)"
"$IWAC" 'putc((256 >> 2) + 1);' > "$TMP/v19b.bin"
check_run "$TMP/v19b.bin" A "v19: 右シフト >>"
"$IWAC" 'x = 0 - 8; putc(72 + (x >> 2));' > "$TMP/v19c.bin"
check_run "$TMP/v19c.bin" F "v19: >> は算術シフト (負でも符号が保たれる)"
"$IWAC" 'putc(64 | 1);' > "$TMP/v19d.bin"
check_run "$TMP/v19d.bin" A "v19: ビット OR |"
"$IWAC" 'putc(127 ^ 62);' > "$TMP/v19e.bin"
check_run "$TMP/v19e.bin" A "v19: ビット XOR ^"
"$IWAC" 'putc(48 + (100 % 7) + 15);' > "$TMP/v19f.bin"
check_run "$TMP/v19f.bin" A "v19: 剰余 %"
"$IWAC" 'putc(65 + !0);' > "$TMP/v19g.bin"
check_run "$TMP/v19g.bin" B "v19: 論理 NOT (!0 = 1)"
"$IWAC" 'putc(65 + !5);' > "$TMP/v19h.bin"
check_run "$TMP/v19h.bin" A "v19: 論理 NOT (!5 = 0)"
"$IWAC" 'x = 5; putc(70 + -x);' > "$TMP/v19i.bin"
check_run "$TMP/v19i.bin" A "v19: 単項マイナス"
"$IWAC" 'x = 0 - 1; putc(66 + x);' > "$TMP/v19j.bin"
check_run "$TMP/v19j.bin" A "v19: 負の数の代入"
"$IWAC" 'i = 0; while (i < 3) { putc(65 + i); i++; }' > "$TMP/v19k.bin"
check_tail "$TMP/v19k.bin" ABC "v19: 後置 ++ 文"
"$IWAC" 'i = 0; while (i < 3) { putc(65 + i); ++i; }' > "$TMP/v19l.bin"
check_tail "$TMP/v19l.bin" ABC "v19: 前置 ++ 文"
"$IWAC" 'i = 5; putc(60 + i++); putc(60 + i);' > "$TMP/v19m.bin"
check_tail "$TMP/v19m.bin" AB "v19: 式の中の後置 ++ (値は増える前)"
"$IWAC" 'i = 5; putc(60 + ++i);' > "$TMP/v19n.bin"
check_run "$TMP/v19n.bin" B "v19: 式の中の前置 ++ (値は増えた後)"
"$IWAC" 's = "AB"; putc(*s++); putc(*s);' > "$TMP/v19o.bin"
check_tail "$TMP/v19o.bin" AB "v19: *s++ は *(s++)"
"$IWAC" 'i = 0; j = 0; x = (i == 0) || (j++ == 0); putc(64 + j); putc(64 + x);' > "$TMP/v19p.bin"
check_tail "$TMP/v19p.bin" '@A' "v19: || の短絡 (右を評価しない)"
"$IWAC" 'i = 0; j = 0; x = (i != 0) && (j++ == 0); putc(64 + j); putc(64 + x);' > "$TMP/v19q.bin"
check_tail "$TMP/v19q.bin" '@@' "v19: && の短絡 (右を評価しない)"
"$IWAC" 'putc(48 + (1 << 2 + 1));' > "$TMP/v19r.bin"
check_run "$TMP/v19r.bin" 8 "v19: 優先順位 (加減がシフトより固い)"
"$IWAC" 'x = 6; putc(64 + (x > 3 && x < 10));' > "$TMP/v19s.bin"
check_run "$TMP/v19s.bin" A "v19: 関係演算子と &&"
check_error 'x = 1; putc(64 + (5++));' "数への後置 ++"
check_error '++5;' "数への前置 ++"

# --- v20: 制御構文の完成 ---
"$IWAC" 'for (i = 0; i < 3; i++) { putc(65 + i); }' > "$TMP/v20a.bin"
check_tail "$TMP/v20a.bin" ABC "v20: for の基本"
"$IWAC" 'for (i = 0; i < 5; i++) { if (i == 2) { continue; } putc(65 + i); }' > "$TMP/v20b.bin"
check_tail "$TMP/v20b.bin" ABDE "v20: for の continue (更新は飛ばさない)"
"$IWAC" 'for (i = 0; i < 9; i++) { if (i == 2) { break; } putc(65 + i); }' > "$TMP/v20c.bin"
check_tail "$TMP/v20c.bin" AB "v20: for の break"
"$IWAC" 'i = 0; while (1) { if (i == 3) { break; } putc(65 + i); i++; }' > "$TMP/v20d.bin"
check_tail "$TMP/v20d.bin" ABC "v20: while の break"
"$IWAC" 'i = 0; while (i < 5) { i++; if (i == 2) { continue; } putc(64 + i); }' > "$TMP/v20e.bin"
check_tail "$TMP/v20e.bin" ACDE "v20: while の continue"
"$IWAC" 'for (i = 0; i < 3; i++) { for (j = 0; j < 3; j++) { if (j == 1) { break; } putc(65 + i); } }' > "$TMP/v20f.bin"
check_tail "$TMP/v20f.bin" ABC "v20: 入れ子の break は内側だけ"
"$IWAC" 'void f(int n) { while (n > 0) { if (n == 2) { return; } putc(64 + n); n = n - 1; } putc(88); } int main() { f(4); putc(46); }' > "$TMP/v20g.bin"
check_tail "$TMP/v20g.bin" 'DC.' "v20: 早期 return (X は出ない)"
"$IWAC" 'int main() { putc(65); return; putc(66); }' > "$TMP/v20h.bin"
check_run "$TMP/v20h.bin" A "v20: main の中の return"
"$IWAC" 'i = 10; for (;;) { putc(64 + i); break; }' > "$TMP/v20i.bin"
check_run "$TMP/v20i.bin" J "v20: for (;;) と break"
"$IWAC" 'for (i = 0; i < 3; i = i + 2) { putc(65 + i); }' > "$TMP/v20j.bin"
check_tail "$TMP/v20j.bin" AC "v20: 更新が代入の for"
check_error 'break;' "ループの外の break"
check_error 'return;' "関数の外の return"

# --- v21: 変数のメモリ化 ---
"$IWAC" 'for (int i = 0; i < 3; i++) { putc(65 + i); }' > "$TMP/v21a.bin"
check_tail "$TMP/v21a.bin" ABC "v21: for の宣言つき初期化 (v20 ではエラーだった)"
"$IWAC" 'a = 1; b = 2; c = 3; d = 4; e = 5; f = 6; putc(64 + a + b + c + d + e + f);' > "$TMP/v21b.bin"
check_run "$TMP/v21b.bin" U "v21: 変数6個 (レジスタ時代の上限4を超える)"
"$IWAC" 'void f(int n) { x = n + 1; putc(64 + x); } void g(int n) { x = n + 2; putc(64 + x); } int main() { f(1); g(1); }' > "$TMP/v21c.bin"
check_tail "$TMP/v21c.bin" BC "v21: 別の関数の同名ローカルは別物"
"$IWAC" 'g = 5; void f(int n) { g = n; } f(9); putc(64 + g);' > "$TMP/v21d.bin"
check_run "$TMP/v21d.bin" I "v21: 関数からグローバルへ代入"
"$IWAC" 'g = 5; void f(int n) { int g; g = 1; putc(64 + g); } f(0); putc(64 + g);' > "$TMP/v21e.bin"
check_tail "$TMP/v21e.bin" AE "v21: ローカルが同名グローバルを隠す"
"$IWAC" 'g = 5; void f(int g) { putc(64 + g); } f(2); putc(64 + g);' > "$TMP/v21f.bin"
check_tail "$TMP/v21f.bin" BE "v21: 引数が同名グローバルを隠す"
"$IWAC" 'int counter; void bump(int n) { counter = counter + n; } bump(2); bump(3); putc(64 + counter);' > "$TMP/v21g.bin"
check_run "$TMP/v21g.bin" E "v21: 宣言だけのグローバル (0 から始まる)"
"$IWAC" 'void f(int n) { int i; int s; s = 0; for (i = 0; i < n; i++) { s = s + i; } putc(64 + s); } int main() { f(4); }' > "$TMP/v21h.bin"
check_run "$TMP/v21h.bin" F "v21: ローカル宣言とフレーム"
"$IWAC" 'void out(int c) { int b; b = c; putc(*&b); } int main() { out(65); }' > "$TMP/v21i.bin"
check_run "$TMP/v21i.bin" A "v21: 単項 & (ローカルの住所)"
check_error 'putc(&5);' "数への &"

# --- v22: 関数の完成 ---
"$IWAC" 'int fib(int n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } putc(48 + fib(10) / 10); putc(48 + fib(10) % 10);' > "$TMP/v22a.bin"
check_tail "$TMP/v22a.bin" 55 "v22: 再帰 (fib 10、両辺が呼び出し)"
"$IWAC" 'int add4(int a, int b, int c, int d) { return a + b + c + d; } putc(64 + add4(1, 2, 3, 4));' > "$TMP/v22b.bin"
check_run "$TMP/v22b.bin" J "v22: 引数4つ (a0〜a3)"
"$IWAC" 'int add4(int a, int b, int c, int d) { return a + b + c + d; } putc(64 + add4(1 + 1, 2 * 2, 9 - 3, 8 / 4));' > "$TMP/v22c.bin"
check_run "$TMP/v22c.bin" N "v22: 引数が式"
"$IWAC" 'int inc(int x) { return x + 1; } putc(64 + inc(inc(inc(0))));' > "$TMP/v22d.bin"
check_run "$TMP/v22d.bin" C "v22: 入れ子の呼び出し (引数の中の呼び出し)"
"$IWAC" 'int f(int x) { return x - 3; } i = 0; while (f(i) < 0) { i = i + 1; } putc(48 + i);' > "$TMP/v22e.bin"
check_run "$TMP/v22e.bin" 3 "v22: 条件の中の呼び出し"
"$IWAC" 'int g; int bump(int d) { g = g + d; return g; } int add2(int a, int b) { return a * 10 + b; } g = 0; putc(48 + add2(bump(1), bump(2)));' > "$TMP/v22f.bin"
check_run "$TMP/v22f.bin" P "v22: 引数は右から評価 (副作用の順序で確認)"
"$IWAC" 'int sq(int x) { return x * x; } y = sq(9); putc(y);' > "$TMP/v22g.bin"
check_run "$TMP/v22g.bin" Q "v22: 戻り値を代入に使う"
"$IWAC" 'int f() { return 66; } int main() { putc(f()); return 0; }' > "$TMP/v22h.bin"
check_run "$TMP/v22h.bin" B "v22: 戻り値つき return (v20 ではエラーだった)"
# 引数5つの定義は v25 で上限が8つになり意味を持つようになった (上の v25 で8つを確認)
check_error 'int f(int a) { return a; } putc(f(1, 2));' "引数の数が合わない呼び出し"

# --- v22/v29: KosabaOS 向けの main 引数 (argv) ---
# v22 では決め打ち argv の置き場所 (48 バイト) が静的
# 領域に確保されることを確かめていた。v29 から argv は
# OS が入口レジスタ (a0 = argc、a1 = argv) で渡す約束
# になり、静的領域は廃止された。ここでは3本目の LOAD
# の memsz が filesz と一致する (= argv 領域が無い)
# ことを確かめる。OS 上での実行確認は v29 で済んだ
# (run t.elf hello で argc と argv[1] が届くこと、
# main の戻り値 7 が exit status として Nish に
# 表示されること、iwac の固定点が新方式でも一致)
"$IWAC" --kosaba 'int main(int argc, char **argv) { putc(64 + argc); }' > "$TMP/v22k.elf"
set -- $(readelf -l "$TMP/v22k.elf" | awk '/LOAD/{n++; if (n == 3) {getline; print $1, $2; exit}}')
[ $(( $2 - $1 )) -eq 0 ] || fail "v29: kosaba の静的 argv 領域が残っている (filesz=$1 memsz=$2)"
echo "ok: v29: kosaba の argv はレジスタ渡し (静的領域なし)"

# --- v23: 配列の完成 ---
"$IWAC" 'int a[4]; a[0] = 65; a[1] = 66; a[2] = 67; putc(a[0]); putc(a[1]); putc(a[2]);' > "$TMP/v23a.bin"
check_tail "$TMP/v23a.bin" ABC "v23: int 配列 (要素8バイト、隣と重ならない)"
"$IWAC" 'int a[4]; a[3] = 1000; putc(64 + (a[3] == 1000));' > "$TMP/v23b.bin"
check_run "$TMP/v23b.bin" A "v23: int 配列に大きい値 (ld で読む)"
"$IWAC" 'char t[2][3]; t[0][0] = 65; t[1][2] = 67; t[0][2] = 66; putc(t[0][0]); putc(t[0][2]); putc(t[1][2]);' > "$TMP/v23c.bin"
check_tail "$TMP/v23c.bin" ABC "v23: 2次元 char 配列"
"$IWAC" 'int m[3][4]; m[2][3] = 68; m[0][1] = 65; m[1][0] = 66; putc(m[0][1]); putc(m[1][0]); putc(m[2][3]);' > "$TMP/v23d.bin"
check_tail "$TMP/v23d.bin" ABD "v23: 2次元 int 配列"
"$IWAC" 'char t[2][4]; char *r; r = t[1]; r[0] = 69; r[1] = 70; putc(t[1][0]); putc(t[1][1]);' > "$TMP/v23e.bin"
check_tail "$TMP/v23e.bin" EF "v23: 名前[行] は行の先頭アドレス"
"$IWAC" 'char *s; s = "Hi"; putc(s[0]); putc(s[1]);' > "$TMP/v23f.bin"
check_tail "$TMP/v23f.bin" Hi "v23: ポインタの添字 (char* は歩幅1)"
"$IWAC" 'char buf[8]; char *p; p = buf; p[0] = 72; p[1] = 105; putc(buf[0]); putc(buf[1]);' > "$TMP/v23g.bin"
check_tail "$TMP/v23g.bin" Hi "v23: ポインタの添字書き込み"
"$IWAC" 'void show(char **pp) { putc(*pp[0]); putc(*pp[1]); } int main() { char *names[2]; names[0] = "A"; names[1] = "B"; show(names); }' > "$TMP/v23h.bin"
check_tail "$TMP/v23h.bin" AB "v23: char** の添字は歩幅8 (argv の形)"
"$IWAC" 'int a[4]; i = 2; a[i + 1] = 68; putc(a[3]);' > "$TMP/v23i.bin"
check_run "$TMP/v23i.bin" D "v23: 添字が式"
check_error 'char a[4]; putc(a[0][0]);' "1次元への二重添字"
check_error 'x = 1; putc(x[0]);' "ポインタでない変数への添字"

# --- v24: グローバル宣言とプロトタイプ ---
"$IWAC" 'int g = 65; int main() { putc(g); }' > "$TMP/v24a.bin"
check_run "$TMP/v24a.bin" A "v24: ファイル範囲の初期化つき宣言 (main と混在)"
"$IWAC" 'int g = 60 + 5; int main() { putc(g); }' > "$TMP/v24b.bin"
check_run "$TMP/v24b.bin" A "v24: 初期化子は定数式 (60 + 5)"
"$IWAC" 'char *s = "Hi"; int main() { putc(*s); putc(*(s + 1)); }' > "$TMP/v24c.bin"
check_tail "$TMP/v24c.bin" Hi "v24: 文字列の初期化 (値はその住所)"
"$IWAC" 'int g = 65; int h = 66; int main() { putc(g); putc(h); g = 67; putc(g); }' > "$TMP/v24d.bin"
check_tail "$TMP/v24d.bin" ABC "v24: 初期値つきが並び、後から書き換えもできる"
"$IWAC" 'int is_odd(int n); int is_even(int n) { if (n == 0) { return 1; } return is_odd(n - 1); } int is_odd(int n) { if (n == 0) { return 0; } return is_even(n - 1); } int main() { putc(64 + is_even(10)); putc(64 + is_odd(7)); }' > "$TMP/v24e.bin"
check_tail "$TMP/v24e.bin" AA "v24: プロトタイプで相互再帰"
"$IWAC" 'int f(int); int f(int x) { return x + 1; } int main() { putc(f(64)); }' > "$TMP/v24f.bin"
check_run "$TMP/v24f.bin" A "v24: 宣言の引数は名前なしでもよい"
check_error 'int f(int x); int main() { putc(f(1)); }' "定義のない関数の呼び出し"
check_error 'int g = 1; int h = g + 1; int main() { putc(65); }' "非定数のファイル範囲初期化子"
check_error 'int f(int a, int b); int f(int a) { return a; }' "宣言と定義で引数の数が違う"

# --- v24: KosabaOS の ELF は PT_LOAD 分割 (v28 で3本) ---
"$IWAC" --kosaba examples/hello-kosaba.c > "$TMP/v24k.elf"
[ "$(readelf -l "$TMP/v24k.elf" | grep -c 'LOAD')" -eq 3 ] || fail "v24: PT_LOAD が3本でない"
readelf -l "$TMP/v24k.elf" | awk '/LOAD/{n++; getline; if (n == 1) code_ok = ($3 == "R" && $4 == "E"); if (n == 2) data_ok = ($3 == "RW"); if (n == 3) gbl_ok = ($3 == "RW")} END{exit !(code_ok && data_ok && gbl_ok)}' || fail "v24: 1本目は R E、2本目と3本目は RW のはず"
set -- $(readelf -l "$TMP/v24k.elf" | awk '/LOAD/{n++; if (n == 2) print $3; }')
[ "$1" = "0x0000000040200000" ] || fail "v24: データセグメントの vaddr が 0x40200000 でない ($1)"
echo "ok: v24: kosaba ELF は PT_LOAD 3本 (コード R+X / データ 0x40200000 R+W、v28 で分割)"
# 初期値つきグローバルの初期イメージがデータ側の実体に入る
"$IWAC" --kosaba 'int g = 72; int main() { putc(g); }' > "$TMP/v24k2.elf"
set -- $(readelf -l "$TMP/v24k2.elf" | awk '/LOAD/{n++; if (n == 3) {getline; print $1, $2; exit}}')
[ $(( $1 )) -eq 8 ] || fail "v24: 初期イメージ (グローバル1個=8バイト) が filesz に無い (filesz=$1)"
echo "ok: v24: 初期イメージが3本目のセグメントの実体に入った"

# --- v25: 容量の調整と小さい構文の追随 ---
"$IWAC" 'int calls; int side() { calls = calls + 1; return 66; } int main() { int x = (3 > 2) ? 65 : 66; putc(x); int z = 1 ? 67 : side(); putc(z); if (calls == 0) { putc(48); } int w = 0 ? side() : 68; putc(w); if (calls == 0) { putc(48); } return 0; }' > "$TMP/v25a.bin"
check_tail "$TMP/v25a.bin" AC0D0 "v25: 三項演算子 (短絡で呼ばれない側は評価しない)"
"$IWAC" 'putc(~0 + 66);' > "$TMP/v25b.bin"
check_run "$TMP/v25b.bin" A "v25: ビット反転 ~"
"$IWAC" 'putc((char)65); putc((int)(unsigned char)66 - 1);' > "$TMP/v25c.bin"
check_tail "$TMP/v25c.bin" AA "v25: キャスト (読み捨て)"
"$IWAC" 'x = 60; x += 5; putc(x); x = 130; x >>= 1; putc(x); x = 32; x <<= 1; x += 1; putc(x);' > "$TMP/v25d.bin"
check_tail "$TMP/v25d.bin" AAA "v25: 複合代入 (+= >>= <<=)"
"$IWAC" 'int a[4]; int main() { a[0] = 64; a[0]++; putc(a[0]); int old = a[0]++; putc(old); putc(a[0]); ++a[0]; putc(a[0]); a[0]--; putc(a[0]); return 0; }' > "$TMP/v25e.bin"
check_tail "$TMP/v25e.bin" AABCB "v25: 要素への ++ / -- (前置と後置、文と式)"
"$IWAC" 'char s[8]; int main() { s[0] = 64; s[0]++; putc(s[0]); return 0; }' > "$TMP/v25f.bin"
check_run "$TMP/v25f.bin" A "v25: char 要素への ++ (1バイト幅)"
"$IWAC" 'int a[4]; int main() { int *p = &a[1]; *p = 72; putc(a[1]); return 0; }' > "$TMP/v25g.bin"
check_run "$TMP/v25g.bin" H "v25: 要素への & (&a[1])"
"$IWAC" 'int main() { int i = 0; while (i < 3) i++; if (i == 3) putc(65); else putc(66); for (int k = 0; k < 2; k++) putc(68); { putc(69); } return 0; }' > "$TMP/v25h.bin"
check_tail "$TMP/v25h.bin" ADDE "v25: 単一文の本体と裸のブロック"
"$IWAC" 'int sum8(int a, int b, int c, int d, int e, int f, int g, int h) { return a + b + c + d + e + f + g + h; } putc(sum8(1, 2, 3, 4, 5, 6, 7, 37));' > "$TMP/v25i.bin"
check_run "$TMP/v25i.bin" A "v25: 引数8つ (a0〜a7)"
"$IWAC" 'static int helper(void) { return 65; } static int counter; int main(void) { counter = helper(); putc(counter); return 0; }' > "$TMP/v25j.bin"
check_run "$TMP/v25j.bin" A "v25: static と (void) 引数"
"$IWAC" 'long x = 65536; putc((x >> 10) + 1); putc(65UL); putc(0x42u);' > "$TMP/v25k.bin"
check_tail "$TMP/v25k.bin" AAB "v25: 大きな数と接尾辞 (2047 の壁を撤廃)"
"$IWAC" 'char big[100000]; big[99999] = 65; putc(big[99999]);' > "$TMP/v25l.bin"
check_run "$TMP/v25l.bin" A "v25: 大きな配列 (合計 4MB まで)"
check_error 'int f(int a, int b, int c, int d, int e, int g, int h, int i, int j) { return a; }' "引数9つの定義"

# --- v26: 配列のスコープとローカル配列 ---
"$IWAC" 'int f(void) { char name_buf[32]; name_buf[0] = 65; return name_buf[0]; } int g(void) { char name_buf[32]; name_buf[0] = 66; return name_buf[0]; } int main(void) { putc(f()); putc(g()); return 0; }' > "$TMP/v26a.bin"
check_tail "$TMP/v26a.bin" AB "v26: 別々の関数で同名のローカル配列"
"$IWAC" 'char buf[4]; int main(void) { char buf[4]; buf[0] = 65; putc(buf[0]); return 0; }' > "$TMP/v26b.bin"
check_run "$TMP/v26b.bin" A "v26: ローカル配列が同名グローバルを隠す"
"$IWAC" 'void put_first(char *p) { putc(p[0]); } int main(void) { char m[2][3]; m[0][0] = 65; m[1][0] = 66; m[0][1] = m[1][0] + 1; putc(m[0][1]); put_first(m[1]); return 0; }' > "$TMP/v26c.bin"
check_tail "$TMP/v26c.bin" CB "v26: 2次元のローカル配列と行アドレス"
"$IWAC" 'int rec(int n) { char b[8]; b[0] = 65 + n; if (n == 0) { putc(b[0]); return 0; } rec(n - 1); putc(b[0]); return 0; } int main(void) { rec(2); return 0; }' > "$TMP/v26d.bin"
check_tail "$TMP/v26d.bin" ABC "v26: 再帰でもフレームごとに別の配列"
"$IWAC" 'int main(void) { int a[4]; int x; x = 90; a[0] = 65; a[1] = 66; putc(a[0]); putc(a[1]); putc(x - 25); return 0; }' > "$TMP/v26e.bin"
check_tail "$TMP/v26e.bin" ABA "v26: ローカル配列とローカル変数が重ならない"
"$IWAC" 'int main(void) { char big[5000]; big[4999] = 65; big[0] = 66; putc(big[0]); putc(big[4999]); return 0; }' > "$TMP/v26f.bin"
check_tail "$TMP/v26f.bin" BA "v26: 大きなローカル配列 (フレームのオフセットが addi 範囲外)"
check_error 'int main(void) { char b[4]; char b[8]; return 0; }' "同じ関数で同じ名前の配列を2回宣言"
check_error 'int main(void) { char b[4]; int b; return 0; }' "配列と同じ名前の変数"

# --- v27: セルフホスト ---
check_error 'int main(void) { syscall3(1, 65, 0, 0); return 0; }' "syscall3 (ベアメタルではエラー)"
"$IWAC" --kosaba 'int main(void) { syscall3(1, 65, 0, 0); syscall3(1, 66, 0, 0); return 0; }' > "$TMP/v27sc.elf"
sz=$(od -A n -t u8 -j 96 -N 8 "$TMP/v27sc.elf" | tr -d ' ')
n=$(od -v -t x4 -j 4096 -N "$sz" "$TMP/v27sc.elf" | grep -o '00000073' | wc -l)
[ "$n" -eq 3 ] || fail "v27: syscall3 の ecall が $n 個 (3のはず: 2回分 + 終了)"
echo "ok: v27: syscall3 が ecall になった (2回分 + 終了 = 3)"
# 前処理が文字リテラルを守ること ('"' で文字列と誤認すると
# 後ろの #define が行頭と認識されず効かなくなる)
cat > "$TMP/v27pp.c" <<'EOF'
#ifdef NOPE
int bad(void) { return '"'; }
#endif
int dq(void) { return '"'; }
#define MARK 66
int main(void) { putc(MARK); putc(dq() == 34 ? 65 : 88); return 0; }
EOF
"$IWAC" "$TMP/v27pp.c" > "$TMP/v27pp.bin"
check_tail "$TMP/v27pp.bin" BA "v27: 文字リテラルの後ろでも #define が効く (前処理)"
# 分岐の距離: 本体が beq の届く範囲 (±4KB) を大きく超える
# while / if でも正しく飛べること (分岐の緩和)
{
    echo 'int main(void) { int i = 0; int x = 0; int y = 0;'
    echo 'while (i < 3) {'
    n=0; while [ $n -lt 700 ]; do echo 'x = x + 1;'; n=$((n+1)); done
    echo 'i = i + 1; }'
    echo 'if (x == 2100) putc(65); else putc(88);'
    echo 'if (x == 9999) {'
    n=0; while [ $n -lt 700 ]; do echo 'y = y + 1;'; n=$((n+1)); done
    echo '}'
    echo 'if (x == 2100) putc(66); else putc(88);'
    echo 'return 0; }'
} > "$TMP/v27branch.c"
"$IWAC" "$TMP/v27branch.c" > "$TMP/v27branch.bin"
check_tail "$TMP/v27branch.bin" AB "v27: 4KB を超える本体を持つ while / if (分岐の緩和)"
"$IWAC" 'long a = 0xFFFFFFFF; long b = 0x80200000; putc(a == 0xFFFFFFFF ? 65 : 88); putc(b == 0x80200000 ? 66 : 88); putc(2147483647 > 2147483646 ? 67 : 88); putc(0x80000000 > 2000000000 ? 68 : 88);' > "$TMP/v27const.bin"
check_tail "$TMP/v27const.bin" ABCD "v27: 大きな定数 (0xFFFFFFFF / 0x80200000 / INT_MAX 境界)"
"$IWAC" 'long id(long v) { return v; } int main(void) { putc(id(2066) == 2066 ? 65 : 88); int x; int *p = &x; *p = 2066; putc(x == 2066 ? 66 : 88); putc(*p == 2066 ? 67 : 88); long y; long *q = &y; *q = 70000; putc(y == 70000 ? 68 : 88); char c; char *r = &c; *r = 69; putc(c); return 0; }' > "$TMP/v27deref.bin"
check_tail "$TMP/v27deref.bin" ABCDE "v27: 関数をまたぐ大きな値と *p の幅 (int* / long* は8バイト、char* は1バイト)"
# 一括コンパイルの代表: nori 本体 (+ libdashi +
# ランタイム) を一括で訳せること。セルフホストの
# 固定点 (iwac2 == iwac3) の確認は分割ビルドで、
# 統合検証として別途実施 (doc/selfhost.md)
# (2026-11-18、doc/split-selfhost.md 段2・段3) 固定点の
# 定義は分割ビルド (v33 の節) へ移り、上限は TU 基準に
# なった。iwac 自身の一括コンパイルは上限を超えるため、
# ここでは一括の代表として nori 本体を訳す
"$IWAC" --kosaba nori/nori.c libdashi/libdashi.c kosaba/kosaba_rt.c > "$TMP/v27one.elf" || fail "v27: 一括コンパイル (nori) に失敗"
size=$(wc -c < "$TMP/v27one.elf")
[ "$size" -gt 10000 ] || fail "v27: 一括コンパイルの出力が小さすぎる ($size バイト)"
echo "ok: v27: 一括コンパイル (nori 本体) で $size バイトの ELF ができた"

# --- v28: ELF の BSS 化 (配列のゼロをファイルに含めない) ---
# 大きなゼロ配列 + 初期値つきグローバル: ファイルは小さく、
# 配列の確保分は2本目の p_memsz に、グローバルの実体
# (8バイト) は3本目の p_filesz に出ること
printf 'char pad[200000]; int g = 66; int main() { putc(g); return 0; }' > "$TMP/v28bss.c"
"$IWAC" --kosaba "$TMP/v28bss.c" > "$TMP/v28bss.elf" || fail "v28: BSS テストのコンパイルに失敗"
size=$(wc -c < "$TMP/v28bss.elf")
[ "$size" -lt 16384 ] || fail "v28: 大きなゼロ配列がファイルに含まれている ($size バイト)"
m2=$(od -An -tu8 -j 160 -N 8 "$TMP/v28bss.elf" | tr -d ' ')
[ "$m2" = "200000" ] || fail "v28: 2本目の p_memsz が配列の確保分でない ($m2)"
f3=$(od -An -tu8 -j 208 -N 8 "$TMP/v28bss.elf" | tr -d ' ')
[ "$f3" = "8" ] || fail "v28: 3本目の p_filesz がグローバルの実体でない ($f3)"
echo "ok: v28: ELF の BSS 化 (ファイル $size バイト、配列 200000 バイトはゼロ埋め分)"

# --- v31: オブジェクト出力 (分割コンパイルの段2) ---
# --elf の形と決まりを readelf で確かめる (設計は
# doc/elf-object.md)。リンクの確認は v32 (nori 本体)
# が担う。旧 .iwo の形の確認と参照リンカ mini_nori
# は v42 で引退した (doc/iwo-retirement.md)
printf 'int getg();\nint main() { return getg(); }\n' > "$TMP/v31b.c"
printf 'int g = 65;\nchar *msg = "Hi";\nint getg() { return g; }\n' > "$TMP/v31a.c"
"$IWAC" --kosaba --elf "$TMP/v31b.c" > "$TMP/v31b.eo" || fail "v31: オブジェクト出力 (b) に失敗"
"$IWAC" --kosaba --elf "$TMP/v31a.c" > "$TMP/v31a.eo" || fail "v31: オブジェクト出力 (a) に失敗"
readelf -h "$TMP/v31b.eo" | grep -q 'REL (Relocatable file)' || fail "v31: 再配置可能オブジェクトでない"
readelf -sW "$TMP/v31b.eo" | grep -q 'iwac_entry' || fail "v31: main のあるファイルに iwac_entry が無い"
readelf -sW "$TMP/v31b.eo" | grep -q 'UND getg' || fail "v31: 宣言だけの関数が未定義シンボルにならない"
readelf -sW "$TMP/v31a.eo" | grep -Eq 'OBJECT +GLOBAL +DEFAULT +[0-9]+ +g$' || fail "v31: 初期値つき変数が実体のあるシンボルにならない"
readelf -rW "$TMP/v31a.eo" | grep -q 'R_RISCV_64' || fail "v31: 文字列へのポインタ初期値の再配置が無い"
readelf -rW "$TMP/v31a.eo" | grep -q 'R_RISCV_PCREL_HI20' || fail "v31: 住所作りの再配置が無い"
"$IWAC" --elf "$TMP/v31a.c" >/dev/null 2>&1 && fail "v31: --kosaba 無しの --elf が通ってしまった"
"$IWAC" --kosaba --elf "$TMP/v31a.c" "$TMP/v31b.c" > "$TMP/v31ab.eo" || fail "v33: 複数ファイルの --elf に失敗"
readelf -h "$TMP/v31ab.eo" | grep -q 'REL (Relocatable file)' || fail "v33: 複数ファイルでもオブジェクトは1つのはず"
echo "ok: v31: オブジェクトの形と決まり"

# --- v32: リンカ nori 本体 (iwac の言語で書いたもの) ---
# gcc で nori を組み立て、オブジェクトをリンクして一括
# コンパイルとバイト一致することと、未定義で止まることを確かめる
if cc -std=c11 -O2 -o "$TMP/nori" nori/nori.c libdashi/libdashi.c 2>/dev/null; then
    "$IWAC" --kosaba --elf "$TMP/v31b.c" > "$TMP/v32b.eo" || fail "v32: オブジェクト出力 (b) に失敗"
    "$IWAC" --kosaba --elf "$TMP/v31a.c" > "$TMP/v32a.eo" || fail "v32: オブジェクト出力 (a) に失敗"
    "$IWAC" --kosaba "$TMP/v31b.c" "$TMP/v31a.c" > "$TMP/v32one.elf" || fail "v32: 一括コンパイルに失敗"
    "$TMP/nori" "$TMP/v32b.eo" "$TMP/v32a.eo" > "$TMP/v32split.elf" || fail "v32: nori でのリンクに失敗"
    cmp -s "$TMP/v32one.elf" "$TMP/v32split.elf" || fail "v32: nori と一括で出力が違う"
    "$IWAC" --kosaba --elf examples/hello-kosaba.c > "$TMP/v32h.eo" || fail "v32: hello のオブジェクト化に失敗"
    "$IWAC" --kosaba examples/hello-kosaba.c > "$TMP/v32h.elf" || fail "v32: hello の一括に失敗"
    "$TMP/nori" "$TMP/v32h.eo" > "$TMP/v32h2.elf" || fail "v32: hello のリンクに失敗"
    cmp -s "$TMP/v32h.elf" "$TMP/v32h2.elf" || fail "v32: hello で nori と一括が違う"
    printf 'int nosuch();\nint main() { return nosuch(); }\n' > "$TMP/v32u.c"
    "$IWAC" --kosaba --elf "$TMP/v32u.c" > "$TMP/v32u.eo" || fail "v32: 未定義テスト用オブジェクトに失敗"
    "$TMP/nori" "$TMP/v32u.eo" >/dev/null 2>&1 && fail "v32: 未定義の関数でリンクが通ってしまった"
    echo "ok: v32: nori でリンクして一括と同じ出力、未定義はエラー"
else
    echo "skip: v32: nori の確認 (cc が無いため)"
fi

# --- 分割セルフホスト計画 段1: nori の大きさ整合チェック ---
# 同じ名前の変数がオブジェクト間で違う大きさを持っていたら
# リンクで止まること (配列には既にある検査の変数版。
# doc/split-selfhost.md 段1)
if [ -x "$TMP/nori" ]; then
    printf 'struct S { int a; int b; }; struct S g; int main() { g.a = 3; return g.a; }\n' > "$TMP/st1a.c"
    printf 'struct S { int a; }; struct S g; int geta() { return g.a; }\n' > "$TMP/st1b.c"
    printf 'struct S { int a; int b; }; struct S g; int geta() { return g.a; }\n' > "$TMP/st1c.c"
    "$IWAC" --kosaba --elf "$TMP/st1a.c" > "$TMP/st1a.eo" || fail "段1: オブジェクト化 (a) に失敗"
    "$IWAC" --kosaba --elf "$TMP/st1b.c" > "$TMP/st1b.eo" || fail "段1: オブジェクト化 (b) に失敗"
    "$IWAC" --kosaba --elf "$TMP/st1c.c" > "$TMP/st1c.eo" || fail "段1: オブジェクト化 (c) に失敗"
    "$TMP/nori" "$TMP/st1a.eo" "$TMP/st1b.eo" >/dev/null 2>&1 && fail "段1: 変数の大きさの食い違いが通ってしまった"
    "$TMP/nori" "$TMP/st1a.eo" "$TMP/st1c.eo" > "$TMP/st1.elf" || fail "段1: 大きさが合うのにリンクできない"
    echo "ok: 段1: nori が変数の大きさの食い違いで止まる"
fi

# --- v33: 再宣言の合併と、分割セルフホスト ---
# ファイル範囲の再宣言 (変数・配列・関数のプロトタイプ) が
# C の仮定義と同じく合併すること、初期値の二重は止まること
"$IWAC" --kosaba 'int x; int x; int a[3]; int a[3]; int f(); int f() { return 41; } int f(); int main() { x = 1; a[0] = f(); return a[0] + x; }' > "$TMP/v33r.elf" || fail "v33: 再宣言の合併ができない"
"$IWAC" --kosaba 'int x = 1; int x = 2; int main() { return x; }' >/dev/null 2>&1 && fail "v33: 初期値の二重が通ってしまった"
echo "ok: v33: 再宣言は合併し、初期値の二重は止まる"
# 分割セルフホスト: 断片ごとにオブジェクト化し、nori で
# つないだものが正の iwac2 (2026-11-18 に定義を移行、
# doc/split-selfhost.md 段2)。一括とのバイト一致は
# 移行期の物差しとして段2まで残したが、段3の上限の
# TU 基準化で一括セルフコンパイルが上限超えになった
# ため引退した
if [ -x "$TMP/nori" ]; then
    for f in 01-head 02-parse 02b-decl 02c-fpconv 03-gen 03-geninl 03-genstmt 03b-genfp 04-encode 05-input 06-preproc 07-main; do
        "$IWAC" --kosaba --elf "src/decls.c" "src/$f.c" > "$TMP/v33-$f.eo" || fail "v33: 断片 $f のオブジェクト化に失敗"
    done
    "$IWAC" --kosaba --elf kosaba/kosaba_rt.c > "$TMP/v33-rt.eo" || fail "v33: rt のオブジェクト化に失敗"
    "$TMP/nori" "$TMP/v33-01-head.eo" "$TMP/v33-02-parse.eo" "$TMP/v33-02b-decl.eo" "$TMP/v33-02c-fpconv.eo" "$TMP/v33-03-gen.eo" "$TMP/v33-03-geninl.eo" "$TMP/v33-03-genstmt.eo" "$TMP/v33-03b-genfp.eo" "$TMP/v33-04-encode.eo" "$TMP/v33-05-input.eo" "$TMP/v33-06-preproc.eo" "$TMP/v33-07-main.eo" "$TMP/v33-rt.eo" > "$TMP/v33split.elf" || fail "v33: 分割リンクに失敗"
    size=$(wc -c < "$TMP/v33split.elf")
    [ "$size" -gt 100000 ] || fail "v33: 分割セルフホストの出力が小さすぎる ($size バイト)"
    [ "$size" -lt 1000000 ] || fail "v33: 分割セルフホストの出力が大きすぎる ($size バイト)"
    echo "ok: v33: 分割セルフホスト (断片→nori) で $size バイトの iwac2 ができた"
    cp "$TMP/v33split.elf" "$TMP/iwac2.elf"
else
    echo "skip: v33: 分割セルフホスト (nori が無いため)"
fi

# --- 段5: ドライバ Misoni ---
# マニフェスト (misoni.txt) を misoni.sh に回させ、
# 出来た iwac2 が一括 (v27 の節) とバイト一致すること。
# リポジトリを $TMP に写して、その中で回す
if [ -x "$TMP/nori" ]; then
    mkdir -p "$TMP/v34/nori"
    cp -r src kosaba misoni misoni.txt "$TMP/v34/"
    cp "$IWAC" "$TMP/v34/iwac"
    cp "$TMP/nori" "$TMP/v34/nori/nori"
    (cd "$TMP/v34" && ./misoni/misoni.sh > misoni.log 2>&1) || fail "段5: Misoni での分割ビルドに失敗"
    cmp -s "$TMP/iwac2.elf" "$TMP/v34/iwac2.elf" || fail "段5: Misoni の iwac2 が分割ビルドの iwac2 と違う"
    echo "ok: 段5: Misoni (マニフェスト駆動) でも iwac2 がバイト一致"
    # v34: --out は出力そのものを変えない (名前は OS 上でのみ効く)
    "$IWAC" --kosaba --elf examples/hello-kosaba.c > "$TMP/v34/plain.eo" || fail "v34: オブジェクト化に失敗"
    "$IWAC" --kosaba --elf --out zzz.eo examples/hello-kosaba.c > "$TMP/v34/named.eo" || fail "v34: --out つきオブジェクト化に失敗"
    cmp -s "$TMP/v34/plain.eo" "$TMP/v34/named.eo" || fail "v34: --out で出力が変わった"
    echo "ok: v34: --out は出力内容を変えない"
    # v34: Misoni 本体 (OS 上のプログラム) が iwac で訳せる
    "$IWAC" --kosaba misoni/misoni.c libdashi/libdashi.c kosaba/kosaba_rt.c > "$TMP/v34/misoni.elf" || fail "v34: misoni.c が訳せない"
    echo "ok: v34: Misoni 本体 (misoni.c) が iwac で訳せる"
else
    echo "skip: 段5: Misoni の確認 (nori が無いため)"
fi

# --- v35: 幅のある整数型 ---
# 折り返し・符号拡張・符号なし演算・要素の歩幅・変換を
# 実行で確かめる (値は putc の並びで比べる)
"$IWAC" 'int main() { uint32 x = 0xFFFFFFFF; x = x + 1; putc(48 + (x == 0)); uint32 z = 0; z = z - 1; putc(48 + (z == 0xFFFFFFFF)); return 0; }' > "$TMP/v35a.bin"
check_tail "$TMP/v35a.bin" 11 "v35: uint32 の折り返し (+1 で 0、-1 で最大)"
"$IWAC" 'int main() { int32 y = -2147483647 - 1; putc(48 + (y == -2147483647 - 1)); y = y >> 31; putc(48 + (y == -1)); int16 s = 32767; s = s + 1; putc(48 + (s == -32767 - 1)); return 0; }' > "$TMP/v35b.bin"
check_tail "$TMP/v35b.bin" 111 "v35: int32 の最小値と算術シフト、int16 の折り返し"
"$IWAC" 'int main() { uint32 d = 0x80000000; d = d / 2; putc(48 + (d == 0x40000000)); uint32 m = 0xFFFFFFFF; putc(48 + (m % 7 == 3)); uint32 r = 0x80000000; r = r >> 4; putc(48 + (r == 0x08000000)); return 0; }' > "$TMP/v35c.bin"
check_tail "$TMP/v35c.bin" 111 "v35: uint32 の除算 (divu)・剰余 (remu)・論理シフト"
"$IWAC" 'int main() { uint16 b[4]; b[0] = 65535; b[1] = 4660; putc(48 + (b[0] == 65535)); putc(48 + (b[1] == 4660)); uint32 c[2]; uint32 *p = c; *p = 0xDEADBEEF; p[1] = 7; putc(48 + (c[0] % 256 == 239)); putc(48 + (c[1] == 7)); return 0; }' > "$TMP/v35d.bin"
check_tail "$TMP/v35d.bin" 1111 "v35: uint16 配列の歩幅、uint32 ポインタの読み書き"
"$IWAC" 'int main() { long v = -1; uint32 u = (uint32)v; putc(48 + (u == 0xFFFFFFFF)); int32 s = (int32)0x80000000; putc(48 + (s < 0)); return 0; }' > "$TMP/v35e.bin"
check_tail "$TMP/v35e.bin" 11 "v35: 幅つきへのキャスト (切り詰めと符号拡張)"
"$IWAC" 'int main() { uint64 w = (uint64)-1; w = w / 16; putc(48 + ((w >> 32) == 0x0FFFFFFF)); putc(48 + ((w & 0xFFFFFFFF) == 0xFFFFFFFF)); uint64 b = (uint64)-1; putc(48 + (b > 1)); putc(48 + ((b >> 60) == 0xF)); return 0; }' > "$TMP/v35f.bin"
check_tail "$TMP/v35f.bin" 1111 "v35: uint64 の除算・符号なし比較・論理シフト"
"$IWAC" 'uint32 inc32(uint32 a) { return a + 1; } int main() { putc(48 + (inc32(0xFFFFFFFF) == 0)); uint32 g = 0xFFFFFFFF; g++; putc(48 + (g == 0)); return 0; }' > "$TMP/v35g.bin"
check_tail "$TMP/v35g.bin" 11 "v35: 関数の戻り値と引数の型、++ の折り返し"
# 幅つきの型でも分割コンパイルが一括と一致する (nori が
# ある環境では実リンクでも確かめる)
printf 'uint32 g;\nuint32 getg() { return g; }\nvoid setg(uint32 v) { g = v; }\n' > "$TMP/v35sa.c"
printf 'uint32 g;\nuint32 getg();\nvoid setg(uint32 v);\nint main() { setg(0xFFFFFFFF); return getg() == 0xFFFFFFFF ? 65 : 66; }\n' > "$TMP/v35sb.c"
"$IWAC" --kosaba --elf "$TMP/v35sa.c" > "$TMP/v35sa.eo" || fail "v35: オブジェクト出力 (a) に失敗"
"$IWAC" --kosaba --elf "$TMP/v35sb.c" > "$TMP/v35sb.eo" || fail "v35: オブジェクト出力 (b) に失敗"
"$IWAC" --kosaba "$TMP/v35sa.c" "$TMP/v35sb.c" > "$TMP/v35one.elf" || fail "v35: 一括コンパイルに失敗"
"$TMP/nori" "$TMP/v35sa.eo" "$TMP/v35sb.eo" > "$TMP/v35mini.elf" || fail "v35: nori でのリンクに失敗"
cmp -s "$TMP/v35one.elf" "$TMP/v35mini.elf" || fail "v35: 分割 (nori) と一括で出力が違う"
echo "ok: v35: 幅つきの型の分割コンパイルが一括と一致"

# --- v36: typedef ---
# 型への別名。名簿を引いて展開するので、幅つきの型の
# 折り返しやポインタの歩幅がそのまま効くこと
"$IWAC" 'typedef uint32 u32; u32 addwrap(u32 a, u32 b) { return a + b; } int main() { u32 x = 0xFFFFFFFF; putc(48 + (addwrap(x, 1) == 0)); u32 g = 0xFFFFFFFF; g++; putc(48 + (g == 0)); return 0; }' > "$TMP/v36a.bin"
check_tail "$TMP/v36a.bin" 11 "v36: typedef 名での宣言・引数・戻り値 (折り返し)"
"$IWAC" 'typedef uint32 u32; typedef u32 word; typedef uint32 *u32ptr; typedef char *str; int main() { word w = 0xFFFFFFFF; w = w + 1; putc(48 + (w == 0)); u32 v = 5; u32ptr p = &v; *p = 0xFFFFFFFF; putc(48 + (v == 0xFFFFFFFF)); str s = "A"; putc(s[0]); return 0; }' > "$TMP/v36b.bin"
check_tail "$TMP/v36b.bin" 11A "v36: 連鎖 typedef・ポインタの typedef・char * の typedef"
"$IWAC" 'typedef uint32 u32; typedef int16 i16; int main() { u32 a[2]; a[0] = 0x12345678; putc(48 + (a[0] == 0x12345678)); u32 z = (u32)-1; putc(48 + (z == 0xFFFFFFFF)); i16 h = 32767; h = h + 1; putc(48 + (h == -32767 - 1)); return 0; }' > "$TMP/v36c.bin"
check_tail "$TMP/v36c.bin" 111 "v36: typedef 名の配列・キャスト・int16 の折り返し"
"$IWAC" 'typedef long i64; typedef unsigned long usz; int main() { i64 v = -1; putc(48 + (v < 0)); usz u = 0xFFFFFFFF; u = u + 1; putc(48 + (u > 0xFFFFFFFF)); static i64 c; c = c + 5; putc(48 + (c == 5)); return 0; }' > "$TMP/v36d.bin"
check_tail "$TMP/v36d.bin" 111 "v36: 従来の型への typedef (64ビットのまま)"
check_error 'int main() { typedef int x; return 0; }' "v36: 関数の中の typedef"
check_error 'typedef int x; typedef uint32 x; int main() { return 0; }' "v36: typedef 名の違う型への付け直し"
check_error 'int x; typedef int x; int main() { return 0; }' "v36: 変数と同名の typedef"
# typedef を使っても分割コンパイルが一括と一致する (各 TU が
# 自分の typedef を持つ。宣言の形は v35 の分割チェックと同じ)
printf 'typedef uint32 u32;\nu32 g;\nu32 getg() { return g; }\nvoid setg(u32 v) { g = v; }\n' > "$TMP/v36sa.c"
printf 'typedef uint32 u32;\nu32 g;\nu32 getg();\nvoid setg(u32 v);\nint main() { setg(0xFFFFFFFF); return getg() == 0xFFFFFFFF ? 65 : 66; }\n' > "$TMP/v36sb.c"
"$IWAC" --kosaba --elf "$TMP/v36sa.c" > "$TMP/v36sa.eo" || fail "v36: オブジェクト出力 (a) に失敗"
"$IWAC" --kosaba --elf "$TMP/v36sb.c" > "$TMP/v36sb.eo" || fail "v36: オブジェクト出力 (b) に失敗"
"$IWAC" --kosaba "$TMP/v36sa.c" "$TMP/v36sb.c" > "$TMP/v36one.elf" || fail "v36: 一括コンパイルに失敗"
"$TMP/nori" "$TMP/v36sa.eo" "$TMP/v36sb.eo" > "$TMP/v36mini.elf" || fail "v36: nori でのリンクに失敗"
cmp -s "$TMP/v36one.elf" "$TMP/v36mini.elf" || fail "v36: 分割 (nori) と一括で出力が違う"
echo "ok: v36: typedef の分割コンパイルが一括と一致"

# --- v37: 構造体 ---
# 定義とレイアウトは C 互換 (doc/struct.md)。sizeof は
# gcc の値と一致するはず (Point 8、Flags 16、Rect 16)
"$IWAC" 'struct Point { int32 x; int32 y; }; struct Flags { char kind; uint32 mask; char name[8]; }; struct Rect { struct Point tl; struct Point br; }; int main() { putc(48 + sizeof(struct Point) - 8 + 1); putc(48 + (sizeof(struct Flags) == 16)); putc(48 + (sizeof(struct Rect) == 16)); putc(48 + (sizeof(char) == 1)); putc(48 + (sizeof(int) == 8)); return 0; }' > "$TMP/v37a.bin"
check_tail "$TMP/v37a.bin" 11111 "v37: sizeof が C と同じレイアウト"
"$IWAC" 'struct Point { int32 x; int32 y; }; struct Flags { char kind; uint32 mask; char name[8]; }; struct Point p; struct Flags f; int main() { p.x = 3; p.y = 4; putc(48 + p.x + p.y); f.kind = 65; f.mask = 0x12345678; f.name[2] = 122; putc(f.kind); putc(48 + (f.mask == 0x12345678)); putc(f.name[2]); return 0; }' > "$TMP/v37b.bin"
check_tail "$TMP/v37b.bin" "7A1z" "v37: メンバの読み書き・配列メンバ・パディング"
"$IWAC" 'struct Point { int32 x; int32 y; }; struct Node { int32 value; struct Node *next; }; struct Point pts[4]; struct Node n1; struct Node n2; int main() { struct Point *q = &pts[0]; q->x = 9; pts[3].y = 2; putc(48 + pts[0].x); putc(48 + pts[3].y); n1.value = 7; n1.next = &n2; n2.value = 8; putc(48 + n1.next->value - 1); return 0; }' > "$TMP/v37c.bin"
check_tail "$TMP/v37c.bin" "927" "v37: -> ・構造体の配列・リンクリスト"
"$IWAC" 'struct Mix { char a; int32 b; char c; int64 d; int16 e; char f; }; struct Mix m; typedef struct Mix Mix; int main() { m.a = 97; m.b = 0x11223344; m.c = 99; m.d = 0x01020304; m.e = 0x5566; m.f = 102; putc(m.a); putc(48 + (m.b == 0x11223344)); putc(m.c); putc(48 + (m.d == 0x01020304)); putc(48 + (m.e == 0x5566)); putc(m.f); m.c = 88; m.f = 90; putc(48 + (m.b == 0x11223344)); Mix loc; loc.b = 5; putc(48 + loc.b); return 0; }' > "$TMP/v37d.bin"
check_tail "$TMP/v37d.bin" "a1c11f15" "v37: パディングで隣が壊れない・typedef struct・ローカル"
"$IWAC" 'struct Point { int32 x; int32 y; }; struct Point p; int main() { p.x = 1; p.x++; ++p.y; putc(48 + p.x + p.y); struct Point *q = &p; putc(48 + (q->x == 2)); return 0; }' > "$TMP/v37e.bin"
check_tail "$TMP/v37e.bin" "31" "v37: メンバの ++ と & ・->"
# (v37 の値渡し・値返し・丸ごと代入のエラー確認は、v45 で解禁したため v45 の節に移した)
check_error 'struct P { int32 x; }; struct P a; int main() { a.zzz = 1; return 0; }' "v37: 無いメンバ"
check_error 'struct P { int32 x; }; struct P a; int main() { struct P *q = &a; q.x = 1; return 0; }' "v37: ポインタへの ."
check_error 'struct P { int32 x; }; int main() { struct P a = 1; return 0; }' "v37: 構造体の初期化"
check_error 'int main() { struct P { int32 x; }; return 0; }' "v37: 関数の中の定義"
# 構造体を使っても分割コンパイルが一括と一致する (各 TU が
# 同じ定義を持ち、複数枠のグローバルが合併される)
printf 'struct Point { int32 x; int32 y; };\nstruct Flags { char kind; uint32 mask; char name[8]; };\nstruct Point gp;\nstruct Flags gf;\nint after;\nint fill() { gp.x = 3; gp.y = 4; gf.kind = 75; gf.mask = 0x99; gf.name[1] = 110; after = 5; return gp.x + gp.y; }\n' > "$TMP/v37sa.c"
printf 'struct Point { int32 x; int32 y; };\nstruct Flags { char kind; uint32 mask; char name[8]; };\nstruct Point gp;\nstruct Flags gf;\nint after;\nint fill();\nint main() { int r = fill(); putc(48 + r); putc(gf.kind); putc(48 + (gf.mask == 0x99)); putc(gf.name[1]); putc(48 + after); putc(10); return 0; }\n' > "$TMP/v37sb.c"
"$IWAC" --kosaba --elf "$TMP/v37sa.c" > "$TMP/v37sa.eo" || fail "v37: オブジェクト出力 (a) に失敗"
"$IWAC" --kosaba --elf "$TMP/v37sb.c" > "$TMP/v37sb.eo" || fail "v37: オブジェクト出力 (b) に失敗"
"$IWAC" --kosaba "$TMP/v37sa.c" "$TMP/v37sb.c" > "$TMP/v37one.elf" || fail "v37: 一括コンパイルに失敗"
"$TMP/nori" "$TMP/v37sa.eo" "$TMP/v37sb.eo" > "$TMP/v37mini.elf" || fail "v37: nori でのリンクに失敗"
cmp -s "$TMP/v37one.elf" "$TMP/v37mini.elf" || fail "v37: 分割 (nori) と一括で出力が違う"
echo "ok: v37: 構造体の分割コンパイルが一括と一致"

# --- v38: Asari (encode の別出し) ---
# 物差しは ELF オブジェクトのバイト一致: iwac --elf の
# 出力と、iwac --asari → Asari の出力が全く同じこと
# (doc/asari.md。v42 で Asari の出力も ELF になった)
gcc -std=c11 -O2 -o "$TMP/asari" asari/asari.c libdashi/libdashi.c || fail "v38: Asari のホストビルドに失敗"
asari_check() {
    # $1 = ラベル、残り = ソースファイル
    label=$1; shift
    "$IWAC" --kosaba --elf "$@" > "$TMP/v38.eo" || fail "v38: ELF オブジェクト出力に失敗 ($label)"
    "$IWAC" --kosaba --asari "$@" > "$TMP/v38.ias" || fail "v38: Asari 形式の出力に失敗 ($label)"
    "$TMP/asari" "$TMP/v38.ias" > "$TMP/v38a.eo" || fail "v38: Asari が失敗 ($label)"
    cmp -s "$TMP/v38.eo" "$TMP/v38a.eo" || fail "v38: Asari の出力が内部 encode と違う ($label)"
}
asari_check hello examples/hello-kosaba.c
for f in 01-head 02-parse 02b-decl 02c-fpconv 03-gen 03-geninl 03-genstmt 03b-genfp 04-encode 05-input 06-preproc 07-main; do
    asari_check "断片 $f" src/decls.c "src/$f.c"
done
echo "ok: v38: Asari が全断片で内部 encode とバイト一致"
asari_check nori nori/nori.c libdashi/libdashi.c
asari_check asari自身 asari/asari.c libdashi/libdashi.c
asari_check rt kosaba/kosaba_rt.c
asari_check 構造体 "$TMP/v37sa.c"
echo "ok: v38: Asari が nori・Asari 自身・rt・構造体でも一致"
# チェーン: iwac → Asari → nori が直接の ELF と一致
"$IWAC" --kosaba --asari examples/hello-kosaba.c > "$TMP/v38h.ias" || fail "v38: チェーン用の出力に失敗"
"$TMP/asari" "$TMP/v38h.ias" > "$TMP/v38h.eo" || fail "v38: チェーンで Asari が失敗"
"$TMP/nori" "$TMP/v38h.eo" > "$TMP/v38h.elf" || fail "v38: チェーンで nori が失敗"
"$IWAC" --kosaba examples/hello-kosaba.c > "$TMP/v38hd.elf" || fail "v38: 直接の ELF 出力に失敗"
cmp -s "$TMP/v38h.elf" "$TMP/v38hd.elf" || fail "v38: チェーンの ELF が直接と違う"
echo "ok: v38: iwac → Asari → nori のチェーンが直接出力と一致"

# --- v39: オブジェクトの ELF 化 ---
# 物差しは「ELF 経由のリンク結果が一括とバイト一致」
# すること (doc/elf-object.md)。v39 当時は .iwo 経由
# との比較だったが、v42 の .iwo 引退で一括との比較に
# 替えた (rt と組のリンクは v33 と同じ形)。全断片の
# 組だけは、段3で一括が上限超えになったため、v33 の
# 分割ビルド (iwac2) と比べる
elf_check() {
    # $1 = ラベル、残り = ソース。rt と組でリンクして
    # 一括と比べる。先頭に kosaba_flush の宣言だけの
    # ファイルを足す: 入口の接頭辞の形 (flush を呼ぶか)
    # を宣言の有無で揃え、一括とリンクで同じ形にする
    # (宣言の無い TU は接頭辞が短く、rt と組にすると
    # 一括と形が違う。decls.c の約束と同じ)
    label=$1; shift
    printf 'void kosaba_flush();\n' > "$TMP/v39pre.c"
    "$IWAC" --kosaba --elf "$TMP/v39pre.c" "$@" > "$TMP/v39.eo" || fail "v39: ELF オブジェクト出力に失敗 ($label)"
    "$IWAC" --kosaba --elf kosaba/kosaba_rt.c > "$TMP/v39rt.eo" || fail "v39: rt の ELF 出力に失敗 ($label)"
    "$IWAC" --kosaba "$TMP/v39pre.c" "$@" kosaba/kosaba_rt.c > "$TMP/v39d.elf" || fail "v39: 一括出力に失敗 ($label)"
    "$TMP/nori" "$TMP/v39.eo" "$TMP/v39rt.eo" > "$TMP/v39b.elf" || fail "v39: ELF オブジェクトのリンクに失敗 ($label)"
    cmp -s "$TMP/v39d.elf" "$TMP/v39b.elf" || fail "v39: ELF 経由のリンク結果が一括と違う ($label)"
}
elf_check hello examples/hello-kosaba.c
# 全断片 (rt と組でリンクし、一括版と比べる)
V39_OBJS_EO=""
for f in 01-head 02-parse 02b-decl 02c-fpconv 03-gen 03-geninl 03-genstmt 03b-genfp 04-encode 05-input 06-preproc 07-main; do
    "$IWAC" --kosaba --elf src/decls.c "src/$f.c" > "$TMP/v39-$f.eo" || fail "v39: 断片 $f の ELF 出力に失敗"
    V39_OBJS_EO="$V39_OBJS_EO $TMP/v39-$f.eo"
done
"$IWAC" --kosaba --elf kosaba/kosaba_rt.c > "$TMP/v39-rt.eo" || fail "v39: rt の ELF 出力に失敗"
"$TMP/nori" $V39_OBJS_EO "$TMP/v39-rt.eo" > "$TMP/v39full-b.elf" || fail "v39: 全断片の ELF リンクに失敗"
cmp -s "$TMP/iwac2.elf" "$TMP/v39full-b.elf" || fail "v39: 全断片のリンク結果が v33 の iwac2 と違う"
echo "ok: v39: ELF オブジェクト (hello・全断片) のリンクが一致"
elf_check nori nori/nori.c libdashi/libdashi.c
elf_check asari自身 asari/asari.c libdashi/libdashi.c
elf_check 構造体 "$TMP/v37sa.c" "$TMP/v37sb.c"
echo "ok: v39: ELF オブジェクトが nori・Asari 自身・構造体でも一致"

# --- v40: 配列の初期化子 (doc/array-init.md) ---
# 値は実行コードではなくデータとして運ばれる。
# まずベアメタルで「読める値・0 埋め・書き戻し」を確かめる
"$IWAC" 'int32 t[5] = { 10, -20, 300, 0, 0 };
if (t[0] == 10) putc(65);
if (t[1] == -20) putc(66);
if (t[2] == 300) putc(67);
if (t[3] == 0) putc(68);
t[3] = 5; if (t[3] == 5) putc(69);
char m[] = "hi";
if (m[0] == 104) putc(70);
if (m[2] == 0) putc(71);
int a[2][3] = { {1, 2, 3}, {4, 5, 6} };
if (a[1][2] == 6) putc(72);
long b[] = { 1000000, -2 };
if (b[0] + b[1] == 999998) putc(73);' > "$TMP/v40a.bin"
check_tail "$TMP/v40a.bin" 'ABCDEFGHI' "v40: 配列初期化子の値が読める"
# KosabaOS 向け: 一括とオブジェクト経路が一致
cat > "$TMP/v40t.c" <<'V40TEST'
int32 tbl[5] = { 10, -20, 0x100, 0, 0 };
char msg[6] = "hello";
int mat[2][3] = { {1, 2, 3}, {4, 5, 6} };
long big[] = { 123456789, -1 };
int get(int i) { return tbl[i]; }
int main() { return get(1) + msg[1] + mat[1][2]; }
V40TEST
"$IWAC" --kosaba "$TMP/v40t.c" > "$TMP/v40d.elf" || fail "v40: 一括出力に失敗"
"$IWAC" --kosaba --elf "$TMP/v40t.c" > "$TMP/v40.eo" || fail "v40: ELF 出力に失敗"
"$TMP/nori" "$TMP/v40.eo" > "$TMP/v40b.elf" || fail "v40: ELF のリンクに失敗"
cmp -s "$TMP/v40d.elf" "$TMP/v40b.elf" || fail "v40: ELF 経由が一括と違う"
"$IWAC" --kosaba --asari "$TMP/v40t.c" > "$TMP/v40.ias" || fail "v40: Asari 形式の出力に失敗"
"$TMP/asari" "$TMP/v40.ias" > "$TMP/v40s.eo" || fail "v40: Asari が失敗"
"$TMP/nori" "$TMP/v40s.eo" > "$TMP/v40c.elf" || fail "v40: Asari 経由のリンクに失敗"
cmp -s "$TMP/v40d.elf" "$TMP/v40c.elf" || fail "v40: Asari 経由が一括と違う"
echo "ok: v40: 配列初期化子の一括と ELF・Asari 経由が一致"
asari_check 配列初期化子 "$TMP/v40t.c"
elf_check 配列初期化子 "$TMP/v40t.c"
# 分割 TU: 仮宣言と初期値つき定義が合併する (両順)
cat > "$TMP/v40s1.c" <<'V40TEST'
int tbl[4];
int get(int i) { return tbl[i]; }
V40TEST
cat > "$TMP/v40s2.c" <<'V40TEST'
int get(int i);
int tbl[4] = { 5, 6, 7, 8 };
int main() { return get(2); }
V40TEST
"$IWAC" --kosaba "$TMP/v40s1.c" "$TMP/v40s2.c" > "$TMP/v40sd.elf" || fail "v40: 分割の一括に失敗"
"$IWAC" --kosaba --elf "$TMP/v40s1.c" > "$TMP/v40s1.eo" || fail "v40: 分割1の ELF 出力に失敗"
"$IWAC" --kosaba --elf "$TMP/v40s2.c" > "$TMP/v40s2.eo" || fail "v40: 分割2の ELF 出力に失敗"
"$TMP/nori" "$TMP/v40s1.eo" "$TMP/v40s2.eo" > "$TMP/v40se12.elf" || fail "v40: 分割 ELF のリンクに失敗"
cmp -s "$TMP/v40sd.elf" "$TMP/v40se12.elf" || fail "v40: 分割 ELF 経由が一括と違う"
"$TMP/nori" "$TMP/v40s2.eo" "$TMP/v40s1.eo" > "$TMP/v40se21.elf" || fail "v40: 分割 ELF 逆順のリンクに失敗"
cmp -s "$TMP/v40sd.elf" "$TMP/v40se21.elf" || fail "v40: 分割 ELF 逆順が一括と違う"
echo "ok: v40: 分割 TU の仮宣言と初期値つき定義が合併する"
# エラーになる形
check_error 'int a[2] = { 1, 2, 3 }; a[0]' "v40: 初期化子が多すぎる"
check_error 'int f() { int a[2] = { 1, 2 }; return a[0]; } f()' "v40: 関数の中の配列初期化"
check_error 'char s[3] = "toolong"; s[0]' "v40: 文字列が配列より長い"

# --- v41: 行継続 (スプライス、doc/splice.md) ---
# 行末のバックスラッシュ+改行が入力層で消える。
# 値で確かめる (文字列・コメント・#define・数値)
cat > "$TMP/v41a.c" <<'V41TEST'
#define V 12\
3
char *s = "abc\
def";
i = 0; while (s[i]) i = i + 1;
if (i == 6) putc(65);
x = 1; // comment \
 x = 99;
if (x == 1) putc(66);
if (V == 123) putc(67);
y = 4\
2;
if (y == 42) putc(68);
V41TEST
"$IWAC" "$TMP/v41a.c" > "$TMP/v41a.bin"
check_tail "$TMP/v41a.bin" 'ABCD' "v41: 行継続が文字列・コメント・#define・数値で効く"
# 癖4の再現: 代入文の行またぎ (バックスラッシュつき)
cat > "$TMP/v41b.c" <<'V41TEST'
int a[8];
int b[8];
int main() { int i; i = 0; b[1] = 7;
    while (i < 3) { a[i * 2 + 1] \
 = b[i]; i = i + 1; } return a[1]; }
V41TEST
"$IWAC" --kosaba "$TMP/v41b.c" > "$TMP/v41b.elf" || fail "v41: 行またぎの代入がコンパイルできない"
echo "ok: v41: 代入文の行またぎ (バックスラッシュつき) が通る"
# バックスラッシュ無しの行またぎは引き続きエラー
check_error 'int a[8]; int b[8];
int main() { int i; i = 0; a[1]
 = b[0]; return a[1]; }' "v41: バックスラッシュ無しの行またぎ"

# --- v42: .iwo の引退 (doc/iwo-retirement.md) ---
# 旧 --object は廃止され、nori は ELF 以外を受けない。
# オブジェクト経路の一括一致は v31〜v40 の各節が
# ELF で担う。ここでは「引退したこと」自体を確かめる
"$IWAC" --kosaba --object examples/hello-kosaba.c >/dev/null 2>&1 && fail "v42: 廃止した --object が通ってしまった"
echo "ok: v42: --object は廃止されている"
printf 'iwacobj 1\ntarget kosaba\nend\n' > "$TMP/v42fake.iwo"
"$TMP/nori" "$TMP/v42fake.iwo" >/dev/null 2>&1 && fail "v42: nori が .iwo を受けてしまった"
echo "ok: v42: nori は ELF オブジェクトだけを読む"

# --- v43: switch (doc/switch.md) ---
# 振る舞いはベア QEMU の出力で確かめる
"$IWAC" 'x=2; switch (x) { case 1: putc(65); break; case 2: putc(66); break; default: putc(67); }' > "$TMP/v43a.bin"
check_tail "$TMP/v43a.bin" 'B' "v43: switch の基本の振り分け"
"$IWAC" 'x=1; switch (x) { case 1: putc(65); case 2: putc(66); case 3: putc(67); }' > "$TMP/v43b.bin"
check_tail "$TMP/v43b.bin" 'ABC' "v43: fallthrough"
"$IWAC" 'x=9; switch (x) { case 1: putc(65); default: putc(68); case 2: putc(66); }' > "$TMP/v43c.bin"
check_tail "$TMP/v43c.bin" 'DB' "v43: default が真ん中 (後ろへ落ちる)"
"$IWAC" 'i=0; while (i < 5) { switch (i) { case 1: i = i + 1; continue; case 3: putc(35); break; default: putc(48 + i); } putc(42); i = i + 1; }' > "$TMP/v43d.bin"
check_tail "$TMP/v43d.bin" '0*2*#*4*' "v43: ループ中の switch (continue はループへ、break は switch を抜ける)"
"$IWAC" 'x=1; switch (x) { case 1: j=0; while (j < 3) { if (j == 1) break; putc(120); j = j + 1; } putc(89); case 2: putc(90); }' > "$TMP/v43e.bin"
check_tail "$TMP/v43e.bin" 'xYZ' "v43: case の中のループの break はループを抜ける"
"$IWAC" 'x=1; y=2; switch (x) { case 1: switch (y) { case 2: putc(73); break; default: putc(63); } putc(79); break; default: putc(63); }' > "$TMP/v43f.bin"
check_tail "$TMP/v43f.bin" 'IO' "v43: switch の入れ子"
"$IWAC" 'uint32 w; w = 0xFFFFFFFF; switch (w) { case -1: putc(85); break; default: putc(63); }' > "$TMP/v43g.bin"
check_tail "$TMP/v43g.bin" 'U' "v43: 幅つき型の制御式 (uint32 と case -1)"
check_error 'x=1; switch (x) { case 1: putc(65); case 1: putc(66); }' "v43: case の重複"
check_error 'x=1; switch (x) { default: putc(65); default: putc(66); }' "v43: default の重複"
check_error 'x=1; switch (x) { putc(65); case 1: putc(66); }' "v43: ラベルの前の文"
check_error 'x=1; y=2; switch (x) { case y: putc(65); }' "v43: 定数でない case"
check_error 'x=1; switch (x) case 1: putc(65);' "v43: 本体の { } が無い"

# --- v44: 関数ポインタ (doc/funcptr.md) ---
# 振る舞いはベア QEMU の出力で確かめる
cat > "$TMP/v44mini.c" <<'V44MINI'
typedef int (*op)(int);
int st;
int op_inc(int pc) { st = st + 1; return pc + 1; }
int op_dbl(int pc) { st = st * 2; return pc + 1; }
op table[3] = { op_inc, op_dbl, 0 };
int main() {
    int prog[4];
    int pc;
    prog[0] = 0; prog[1] = 0; prog[2] = 1; prog[3] = 9;
    st = 1; pc = 0;
    while (prog[pc] != 9) {
        op h = table[prog[pc]];
        pc = h(pc);
    }
    putc(st == 6 ? 'Y' : 'N');
    return 0;
}
V44MINI
"$IWAC" "$TMP/v44mini.c" > "$TMP/v44a.bin"
check_tail "$TMP/v44a.bin" 'Y' "v44: ハンドラ表で回す小さなインタプリタ"
"$IWAC" 'int inc(int x) { return x + 1; } int dbl(int x) { return x * 2; } int (*fp)(int) = inc; int (*tbl[2])(int) = { inc, dbl }; putc(tbl[1](fp(10)) == 22 ? 89 : 78);' > "$TMP/v44b.bin"
check_tail "$TMP/v44b.bin" 'Y' "v44: 直接宣言子 (変数と配列、初期化子つき)"
"$IWAC" 'int inc(int x) { return x + 1; } typedef int (*op)(int); int apply(op f, int v) { return f(v); } putc(apply(inc, 41) == 42 ? 89 : 78);' > "$TMP/v44c.bin"
check_tail "$TMP/v44c.bin" 'Y' "v44: コールバック (関数ポインタの引数)"
cat > "$TMP/v44d.c" <<'V44D'
typedef int (*op)(int);
int inc(int x) { return x + 1; }
op tbl[2];
int main() {
    tbl[0] = inc;
    op h = tbl[0];
    putc((*h)(1) == 2 && tbl[1] == 0 ? 'Y' : 'N');
    return 0;
}
V44D
"$IWAC" "$TMP/v44d.c" > "$TMP/v44d.bin"
check_tail "$TMP/v44d.bin" 'Y' "v44: (*h)(x) の形と NULL チェック"
cat > "$TMP/v44mem.c" <<'V44MEM'
typedef int (*op)(int);
int inc(int x) { return x + 1; }
int dbl(int x) { return x * 2; }
struct Dev { op f; op tbl[2]; int base; };
int main() {
    struct Dev d;
    struct Dev *p;
    p = &d;
    d.f = dbl;
    d.tbl[0] = inc;
    d.tbl[1] = dbl;
    d.base = 10;
    putc(p->f(3) + d.tbl[0](1) + p->tbl[1](d.base) == 28 ? 'Y' : 'N');
    return 0;
}
V44MEM
"$IWAC" "$TMP/v44mem.c" > "$TMP/v44e.bin"
check_tail "$TMP/v44e.bin" 'Y' "v44: 構造体のメンバの関数ポインタ"
cat > "$TMP/v44rec.c" <<'V44REC'
typedef int (*op)(int);
int fact_step(int x);
op cur = fact_step;
int fact_step(int x) {
    if (x <= 1) return 1;
    return x * cur(x - 1);
}
op pick(int i) { return cur; }
int main() {
    op h = pick(0);
    putc(h(5) == 120 ? 'Y' : 'N');
    return 0;
}
V44REC
"$IWAC" "$TMP/v44rec.c" > "$TMP/v44f.bin"
check_tail "$TMP/v44f.bin" 'Y' "v44: 再帰するハンドラと、ハンドラを返す関数"
check_error 'typedef int (*A)(int); typedef int (*B)(int, int); int f(int x) { return x; } A a = f; B b; b = a;' "v44: 署名の違う代入"
check_error 'typedef int (*op)(int); int f(int x) { return x; } op h = f; h(1, 2);' "v44: 引数の数が違う間接呼び出し"
check_error 'typedef int (*op)(int); int f(int x); op h = f;' "v44: 本体の無い関数の住所 (初期化子)"
check_error 'typedef int (*op)(int); int g(int x, int y) { return x; } op tbl[1] = { g };' "v44: 署名の違う関数を表の初期化子に"
check_error 'int x; x(1);' "v44: 関数ポインタでないものの呼び出し"
check_error 'int f(int x) { return x; } int g = f;' "v44: 関数の住所を int の初期化子に"
# オブジェクト経路: 初期化子つきの関数ポインタでも
# リンク結果が一括ビルドと一致し、Asari のオブジェクトが
# iwac と一致すること
cat > "$TMP/v44obj.c" <<'V44OBJ'
typedef int (*handler)(int);
int op_inc(int x) { return x + 1; }
int op_dbl(int x) { return x * 2; }
handler cur = op_inc;
handler table[3] = { op_inc, op_dbl, 0 };
int main() {
    handler h = op_dbl;
    return h(10) + table[0](cur(1));
}
V44OBJ
cat > "$TMP/v44rt.c" <<'V44RT'
int flushed;
void kosaba_flush() { flushed = 1; }
V44RT
printf 'void kosaba_flush();\n' > "$TMP/v44pre.c"
"$IWAC" --kosaba "$TMP/v44pre.c" "$TMP/v44obj.c" "$TMP/v44rt.c" > "$TMP/v44direct.elf" || fail "v44: 一括ビルドに失敗"
"$IWAC" --kosaba --elf "$TMP/v44pre.c" "$TMP/v44obj.c" > "$TMP/v44obj.eo" || fail "v44: オブジェクト出力に失敗"
"$IWAC" --kosaba --elf "$TMP/v44pre.c" "$TMP/v44rt.c" > "$TMP/v44rt.eo" || fail "v44: rt のオブジェクト出力に失敗"
"$TMP/nori" "$TMP/v44obj.eo" "$TMP/v44rt.eo" > "$TMP/v44link.elf" || fail "v44: nori でのリンクに失敗"
cmp -s "$TMP/v44direct.elf" "$TMP/v44link.elf" || fail "v44: リンク結果が一括ビルドと違う"
echo "ok: v44: 関数ポインタ (初期化子つき) のリンクが一括と一致"
"$IWAC" --kosaba --asari "$TMP/v44pre.c" "$TMP/v44obj.c" "$TMP/v44rt.c" > "$TMP/v44.ias" || fail "v44: Asari 形式の出力に失敗"
"$TMP/asari" "$TMP/v44.ias" > "$TMP/v44asari.eo" || fail "v44: Asari が失敗"
"$IWAC" --kosaba --elf "$TMP/v44pre.c" "$TMP/v44obj.c" "$TMP/v44rt.c" > "$TMP/v44iwac.eo" || fail "v44: オブジェクト出力 (一括) に失敗"
cmp -s "$TMP/v44iwac.eo" "$TMP/v44asari.eo" || fail "v44: Asari のオブジェクトが iwac と違う"
echo "ok: v44: Asari のオブジェクトが iwac と一致 (関数ポインタ)"


# --- v45: 構造体の値セマンティクス (doc/struct-value.md) ---
# 振る舞いはベア QEMU の出力で確かめる
cat > "$TMP/v45a.c" <<'V45A'
struct P { int x; int y; };
struct In { int a; };
struct Out { struct In in; int b; };
struct P arr[2];
int main() {
    struct P a; struct P b; struct Out o1; struct Out o2;
    struct P *pp;
    a.x = 1; a.y = 2; b = a; b.x = 9;
    o1.in.a = 5; o1.b = 6; o2 = o1; o2.in.a = 7;
    arr[0] = a; arr[1].x = 3; arr[1].y = 4; a = arr[1];
    o2.in = o1.in;
    pp = &a; *pp = b;
    putc(a.x == 9 && b.y == 2 && o1.in.a == 5 &&
         o2.in.a == 5 && o2.b == 6 && arr[0].y == 2 ? 'Y' : 'N');
    return 0;
}
V45A
"$IWAC" "$TMP/v45a.c" > "$TMP/v45a.bin"
check_tail "$TMP/v45a.bin" 'Y' "v45: 丸ごと代入 (変数・要素・メンバ・ポインタ越し)"
cat > "$TMP/v45b.c" <<'V45B'
struct P { int x; int y; };
struct P make(int x, int y) { struct P r; r.x = x; r.y = y; return r; }
int sum(struct P p) { return p.x + p.y; }
struct P swap(struct P p) { struct P r; r.x = p.y; r.y = p.x; return r; }
int main() {
    struct P a; struct P b;
    a = make(3, 4);
    b = swap(a);
    putc(a.x == 3 && a.y == 4 && sum(a) == 7 &&
         b.x == 4 && b.y == 3 && sum(swap(a)) == 7 ? 'Y' : 'N');
    return 0;
}
V45B
"$IWAC" "$TMP/v45b.c" > "$TMP/v45b.bin"
check_tail "$TMP/v45b.bin" 'Y' "v45: 値渡しと値返し (B 方式、呼び出し結果の引数)"
cat > "$TMP/v45c.c" <<'V45C'
struct Big { int a; int b; int c; };
int mix(int s1, struct Big b, int s2) { return s1 + b.a + b.b + b.c + s2; }
int mut(struct Big b) { b.a = 9; return 0; }
struct Big idb(struct Big b) { return b; }
struct Big chain(struct Big b) { return idb(b); }
int rec(struct Big b, int n) { if (n == 0) return b.a; return rec(b, n - 1) + 1; }
int main() {
    struct Big b; struct Big c;
    b.a = 1; b.b = 2; b.c = 3;
    c.a = 4; c.b = 5; c.c = 6;
    mut(b);
    int ok = mix(1, b, 1) == 8 && b.a == 1;
    b = chain(c);
    putc(ok && b.b == 5 && rec(b, 3) == 7 ? 'Y' : 'N');
    return 0;
}
V45C
"$IWAC" "$TMP/v45c.c" > "$TMP/v45c.bin"
check_tail "$TMP/v45c.bin" 'Y' "v45: スカラとの混在・複写の独立・return の受け渡し・再帰"
cat > "$TMP/v45d.c" <<'V45D'
struct P { int x; int y; };
typedef struct P (*mk_t)(int, int);
typedef int (*sum_t)(struct P);
typedef struct P (*xf_t)(struct P);
struct P make(int x, int y) { struct P r; r.x = x; r.y = y; return r; }
int sum(struct P p) { return p.x + p.y; }
struct P swap(struct P p) { struct P r; r.x = p.y; r.y = p.x; return r; }
struct P dbl(struct P p) { struct P r; r.x = p.x * 2; r.y = p.y * 2; return r; }
xf_t ops[2] = { swap, dbl };
int main() {
    struct P a; struct P b; mk_t mk; sum_t sm;
    mk = make; sm = sum;
    a = mk(3, 4);
    b = ops[1](a);
    putc(a.x == 3 && sm(a) == 7 && b.x == 6 && b.y == 8 &&
         sm(ops[0](a)) == 7 ? 'Y' : 'N');
    return 0;
}
V45D
"$IWAC" "$TMP/v45d.c" > "$TMP/v45d.bin"
check_tail "$TMP/v45d.bin" 'Y' "v45: 関数ポインタ越しの値渡し・値返し (ICALL の B 方式)"
cat > "$TMP/v45e.c" <<'V45E'
struct P { int x; int y; };
struct In { int a; int b; };
struct Out { struct In in; int tail; int arr[3]; char name[6]; };
struct Bag { struct P pts[2]; int n; };
typedef int (*op_t)(int);
int inc(int n) { return n + 1; }
struct WF { op_t op; int v; };
int main() {
    struct P p = { 3, 4 };
    struct P q = { 5 };
    struct Out o = { { 1, 2 }, 9, { 7, 8 }, "ab" };
    struct Bag b = { { { 1, 2 }, { 3, 4 } }, 2 };
    struct WF w = { inc, 4 };
    struct P r = p;
    int ok = p.x == 3 && p.y == 4 && q.x == 5 && q.y == 0;
    ok = ok && o.in.a == 1 && o.in.b == 2 && o.tail == 9;
    ok = ok && o.arr[0] == 7 && o.arr[1] == 8 && o.arr[2] == 0;
    ok = ok && o.name[0] == 'a' && o.name[1] == 'b' && o.name[2] == 0;
    ok = ok && b.pts[1].x == 3 && b.pts[1].y == 4 && b.n == 2;
    ok = ok && w.op(w.v) == 5 && r.y == 4;
    putc(ok ? 'Y' : 'N');
    return 0;
}
V45E
"$IWAC" "$TMP/v45e.c" > "$TMP/v45e.bin"
check_tail "$TMP/v45e.bin" 'Y' "v45: ローカル初期化子 (入れ子・配列・文字列・0埋め・関数ポインタ)"
check_error 'struct P { int x; }; struct Q { int x; }; int main() { struct P a; struct Q b; a = b; return 0; }' "v45: タグの違う代入"
check_error 'struct P { int x; }; int main() { struct P a; int n; n = a; return 0; }' "v45: 構造体をスカラへ代入"
check_error 'struct P { int x; }; int main() { struct P a; a = 5; return 0; }' "v45: スカラを構造体へ代入"
check_error 'struct P { int x; }; struct Q { int x; }; int sum(struct P p) { return p.x; } int main() { struct Q q; return sum(q); }' "v45: タグの違う引数"
check_error 'struct P { int x; }; int sum(struct P p) { return p.x; } int main() { return sum(5); }' "v45: スカラを構造体の引数に"
check_error 'struct P { int x; }; struct Q { int x; }; struct P f() { struct Q q; return q; } int main() { struct P a; a = f(); return 0; }' "v45: タグの違う return"
check_error 'struct P { int x; }; struct P f() { return 5; } int main() { struct P a; a = f(); return 0; }' "v45: 構造体の関数のスカラ return"
check_error 'struct P { int x; }; struct P g = { 1 }; int main() { return g.x; }' "v45: ファイル範囲の構造体の初期化子"
check_error 'struct P { int x; int y; }; int main() { struct P p = { 1, 2, 3 }; return 0; }' "v45: 初期化子が多すぎる"
check_error 'struct P { int x; }; struct P mk(int x) { struct P r; r.x = x; return r; } int digit(int n) { return n; } int main() { return digit(mk(1)); }' "v45: 式の位置での構造体の戻り値"
# オブジェクト経路: 値渡し・値返しと初期化子でも、リンク結果が
# 一括ビルドと一致し、Asari のオブジェクトが iwac と一致すること
cat > "$TMP/v45obj.c" <<'V45OBJ'
struct P { int x; int y; };
struct P make(int x, int y) { struct P r = { x, y }; return r; }
int sum(struct P p) { return p.x + p.y; }
int main() {
    struct P a;
    a = make(3, 4);
    return sum(a);
}
V45OBJ
cat > "$TMP/v45rt.c" <<'V45RT'
int flushed;
void kosaba_flush() { flushed = 1; }
V45RT
printf 'void kosaba_flush();\n' > "$TMP/v45pre.c"
"$IWAC" --kosaba "$TMP/v45pre.c" "$TMP/v45obj.c" "$TMP/v45rt.c" > "$TMP/v45direct.elf" || fail "v45: 一括ビルドに失敗"
"$IWAC" --kosaba --elf "$TMP/v45pre.c" "$TMP/v45obj.c" > "$TMP/v45obj.eo" || fail "v45: オブジェクト出力に失敗"
"$IWAC" --kosaba --elf "$TMP/v45pre.c" "$TMP/v45rt.c" > "$TMP/v45rt.eo" || fail "v45: rt のオブジェクト出力に失敗"
"$TMP/nori" "$TMP/v45obj.eo" "$TMP/v45rt.eo" > "$TMP/v45link.elf" || fail "v45: nori でのリンクに失敗"
cmp -s "$TMP/v45direct.elf" "$TMP/v45link.elf" || fail "v45: リンク結果が一括ビルドと違う"
echo "ok: v45: 構造体の値セマンティクス (初期化子つき) のリンクが一括と一致"
"$IWAC" --kosaba --asari "$TMP/v45pre.c" "$TMP/v45obj.c" "$TMP/v45rt.c" > "$TMP/v45.ias" || fail "v45: Asari 形式の出力に失敗"
"$TMP/asari" "$TMP/v45.ias" > "$TMP/v45asari.eo" || fail "v45: Asari が失敗"
"$IWAC" --kosaba --elf "$TMP/v45pre.c" "$TMP/v45obj.c" "$TMP/v45rt.c" > "$TMP/v45iwac.eo" || fail "v45: オブジェクト出力 (一括) に失敗"
cmp -s "$TMP/v45iwac.eo" "$TMP/v45asari.eo" || fail "v45: Asari のオブジェクトが iwac と違う"
echo "ok: v45: Asari のオブジェクトが iwac と一致 (構造体の値セマンティクス)"


# --- v46: 浮動小数点 (double、doc/float.md) ---
# リテラルの変換の正確さ: ビットパターンを16進で出し、
# ホストの strtod と一致する値 (事前に照合済み) と比べる
cat > "$TMP/v46a.c" <<'V46A'
void puthex(long v) {
    int i; int sh; int d;
    for (i = 0; i < 16; i++) {
        sh = 60 - i * 4;
        d = (int)((v >> sh) & 15);
        if (d < 10) putc('0' + d); else putc('a' + d - 10);
    }
    putc(10);
}
long bits_of(double d) {
    double *dp = &d;
    long *bp = (long *)dp;
    return *bp;
}
int main() {
    puthex(bits_of(1.5));
    puthex(bits_of(0.1));
    puthex(bits_of(0.2));
    puthex(bits_of(0.3));
    puthex(bits_of(3.141592653589793));
    puthex(bits_of(1e10));
    puthex(bits_of(1e-10));
    puthex(bits_of(1e308));
    puthex(bits_of(1.7976931348623157e308));
    puthex(bits_of(5e-324));
    puthex(bits_of(2.2250738585072014e-308));
    puthex(bits_of(2.2250738585072011e-308));
    puthex(bits_of(9007199254740993.0));
    puthex(bits_of(9007199254740995.0));
    puthex(bits_of(1e23));
    puthex(bits_of(0.5));
    puthex(bits_of(2.5));
    puthex(bits_of(123456789.123456789));
    puthex(bits_of(1e-5));
    puthex(bits_of(6.62607015e-34));
    puthex(bits_of(4.9406564584124654e-324));
    puthex(bits_of(2.4703282292062327e-324));
    puthex(bits_of(0.30000000000000004));
    puthex(bits_of(1e-310));
    puthex(bits_of(123.456e-7));
    putc('E');
    return 0;
}
V46A
"$IWAC" "$TMP/v46a.c" > "$TMP/v46a.bin"
check_tail "$TMP/v46a.bin" '3ff8000000000000
3fb999999999999a
3fc999999999999a
3fd3333333333333
400921fb54442d18
4202a05f20000000
3ddb7cdfd9d7bdbb
7fe1ccf385ebc8a0
7fefffffffffffff
0000000000000001
0010000000000000
000fffffffffffff
4340000000000000
4340000000000002
44b52d02c7e14af6
3fe0000000000000
4004000000000000
419d6f34547e6b75
3ee4f8b588e368f1
390b860bde023111
0000000000000001
0000000000000000
3fd3333333333334
000012688b70e62b
3ee9e3fe580f5494
E' "v46: リテラルの変換 (境界・subnormal・丸めの tie 含む)"
# 四則・混合演算・キャスト
cat > "$TMP/v46b.c" <<'V46B'
long bits_of(double d) {
    double *dp = &d;
    long *bp = (long *)dp;
    return *bp;
}
int main() {
    double a = 1.5;
    double b = 2.25;
    int ok = bits_of(a + b) == bits_of(3.75);
    ok = ok && bits_of(a - b) == bits_of(-0.75);
    ok = ok && bits_of(a * b) == bits_of(3.375);
    ok = ok && bits_of(b / a) == bits_of(1.5);
    ok = ok && bits_of(1.0 / 3.0) == (((long)0x3FD55555 << 32) | (long)0x55555555);
    ok = ok && bits_of(-a) == bits_of(-1.5);
    ok = ok && bits_of(1 + 0.25) == bits_of(1.25);
    ok = ok && bits_of(10 - 2.5) == bits_of(7.5);
    ok = ok && bits_of(3 * 1.5) == bits_of(4.5);
    ok = ok && bits_of(7.5 / 2) == bits_of(3.75);
    ok = ok && (int)3.99 == 3 && (int)(-3.99) == -3;
    ok = ok && (int32)1e10 == 1410065408;
    ok = ok && (uint64)1e20 == (uint64)-1;
    ok = ok && bits_of((double)(uint64)-1) == ((long)0x43F00000 << 32);
    ok = ok && bits_of((double)-3) == bits_of(-3.0);
    ok = ok && (long)(a + b) == 3;
    ok = ok && bits_of(1.0 / 0.0) == ((long)0x7FF00000 << 32);
    ok = ok && (0.0 / 0.0) != (0.0 / 0.0);
    ok = ok && (int)(0.0 / 0.0) == (int)(((long)0x7FFFFFFF << 32) | (long)0xFFFFFFFF);
    a += 0.5;
    ok = ok && bits_of(a) == bits_of(2.0);
    a *= 3.0;
    a -= 1.0;
    a /= 2.0;
    ok = ok && bits_of(a) == bits_of(2.5);
    putc(ok ? 'Y' : 'N');
    return 0;
}
V46B
"$IWAC" "$TMP/v46b.c" > "$TMP/v46b.bin"
check_tail "$TMP/v46b.bin" 'Y' "v46: 四則・混合演算・複合代入・キャスト・Inf/NaN"
# 比較・条件・ループ
cat > "$TMP/v46c.c" <<'V46C'
int main() {
    double a = 0.0;
    int i = 0;
    int ok = (1.5 < 2.5) && (2.5 > 1.5) && (1.5 <= 1.5) &&
             (1.5 >= 1.5) && (1.5 == 1.5) && (1.5 != 2.5);
    ok = ok && !(1.5 < 1.5) && !(1.5 == 2.5);
    ok = ok && !0.0 && !(!2.5);
    ok = ok && (1.5 && 2.5) && !(1.5 && 0.0) && (0.0 || 2.5);
    while (a < 2.0) {
        a += 0.5;
        i++;
    }
    ok = ok && i == 4 && a == 2.0;
    for (a = 0.0; a < 1.0; a = a + 0.25)
        i++;
    ok = ok && i == 8;
    if (a - 1.0 == 0.0)
        ok = ok && 1;
    else
        ok = 0;
    ok = ok && (1.5 > 1.0 ? 2.5 : 3.5) == 2.5;
    ok = ok && (1.5 < 1.0 ? 2.5 : 3.5) == 3.5;
    ok = ok && (0.0 ? 2.5 : 3.5) == 3.5;
    ok = ok && (1 ? 2.5 : 3) == 2.5;
    putc(ok ? 'Y' : 'N');
    return 0;
}
V46C
"$IWAC" "$TMP/v46c.c" > "$TMP/v46c.bin"
check_tail "$TMP/v46c.bin" 'Y' "v46: 比較・論理・条件式・ループ条件"
# 関数・配列・構造体・関数ポインタ・ポインタ
cat > "$TMP/v46d.c" <<'V46D'
typedef double real;
struct P { char tag; double x; int n; double y; };
typedef double (*binop_t)(double, double);
double op_add(double a, double b) { return a + b; }
double op_mul(double a, double b) { return a * b; }
binop_t ops[2] = { op_add, op_mul };
struct P mk(real x) {
    struct P p;
    p.tag = 'A'; p.x = x; p.n = 7; p.y = x * 2.0;
    return p;
}
double use_struct(struct P p) { return p.x + p.y; }
double globalsum;
real g = 1.5;
double h = 2;
double arr[3];
int main() {
    struct P p;
    double loc[2];
    double d = 6.25;
    double *dp = &d;
    binop_t f = ops[1];
    int ok = g == 1.5 && h == 2.0;
    arr[0] = 1.25; arr[1] = 2.5; arr[2] = arr[0] + arr[1];
    ok = ok && arr[2] == 3.75;
    arr[1]++;
    ++arr[1];
    ok = ok && arr[1] == 4.5;
    loc[0] = 0.5; loc[1] = loc[0] * 4.0;
    ok = ok && loc[1] == 2.0;
    p = mk(1.5);
    ok = ok && p.x == 1.5 && p.y == 3.0 && p.n == 7;
    ok = ok && use_struct(p) == 4.5;
    p.x = p.x + 0.5;
    ok = ok && p.x == 2.0;
    *dp = *dp + 0.75;
    ok = ok && d == 7.0 && *dp == 7.0;
    ok = ok && f(1.5, 4.0) == 6.0 && ops[0](1.5, 4.0) == 5.5;
    globalsum = op_add(arr[2], loc[1]);
    ok = ok && globalsum == 5.75;
    ok = ok && sizeof(double) == 8 && sizeof(struct P) == 32;
    putc(ok ? 'Y' : 'N');
    return 0;
}
V46D
"$IWAC" "$TMP/v46d.c" > "$TMP/v46d.bin"
check_tail "$TMP/v46d.bin" 'Y' "v46: 関数・配列・構造体・関数ポインタ・typedef・初期化子"
check_error 'int main() { double d = 1.5; return (int)(d % 2.0); }' "v46: 浮動小数点の %"
check_error 'int main() { double d = 1.5; return (int)(d & 2.0); }' "v46: 浮動小数点のビット演算"
check_error 'int main() { double d = 1.5; return (int)(d << 1); }' "v46: 浮動小数点のシフト"
check_error 'int main() { double d = 1.5; return (int)(~d); }' "v46: 浮動小数点の ~"
check_error 'int main() { double d = 1.5; switch (d) { case 1: return 1; } return 0; }' "v46: switch の制御式に浮動小数点"
check_error 'int main() { int x = 1; switch (x) { case 1.5: return 1; } return 0; }' "v46: case ラベルに浮動小数点"
check_error 'double a[2] = { 1.5, 2.5 }; int main() { return 0; }' "v46: 浮動小数点の配列初期化子"
check_error 'int main() { double d = .5; return 0; }' "v46: . から始まるリテラル"
check_error 'int main() { char *s = "x"; double d = s + 1.5; return 0; }' "v46: ポインタと浮動小数点の演算"
check_error 'int x = 1.5; int main() { return 0; }' "v46: 整数変数への浮動小数点の初期化子"
# オブジェクト経路: double を含むプログラムでも、リンク結果が
# 一括ビルドと一致し、Asari のオブジェクトが iwac と一致すること
cat > "$TMP/v46obj.c" <<'V46OBJ'
struct P { double x; int n; };
struct P mk(double x) { struct P p; p.x = x * 1.5; p.n = 3; return p; }
double add(double a, double b) { return a + b; }
int main() {
    struct P p = mk(2.0);
    return (int)add(p.x, 1.25);
}
V46OBJ
cat > "$TMP/v46rt.c" <<'V46RT'
void kosaba_flush(void) { }
V46RT
printf 'void kosaba_flush();\n' > "$TMP/v46pre.c"
"$IWAC" --kosaba "$TMP/v46pre.c" "$TMP/v46obj.c" "$TMP/v46rt.c" > "$TMP/v46direct.elf" || fail "v46: 一括ビルドに失敗"
"$IWAC" --kosaba --elf "$TMP/v46pre.c" "$TMP/v46obj.c" > "$TMP/v46obj.eo" || fail "v46: オブジェクト出力に失敗"
"$IWAC" --kosaba --elf "$TMP/v46pre.c" "$TMP/v46rt.c" > "$TMP/v46rt.eo" || fail "v46: rt のオブジェクト出力に失敗"
"$TMP/nori" "$TMP/v46obj.eo" "$TMP/v46rt.eo" > "$TMP/v46link.elf" || fail "v46: nori でのリンクに失敗"
cmp -s "$TMP/v46direct.elf" "$TMP/v46link.elf" || fail "v46: リンク結果が一括ビルドと違う"
echo "ok: v46: double を含むリンクが一括と一致"
"$IWAC" --kosaba --asari "$TMP/v46pre.c" "$TMP/v46obj.c" "$TMP/v46rt.c" > "$TMP/v46.ias" || fail "v46: Asari 形式の出力に失敗"
"$TMP/asari" "$TMP/v46.ias" > "$TMP/v46asari.eo" || fail "v46: Asari が失敗"
"$IWAC" --kosaba --elf "$TMP/v46pre.c" "$TMP/v46obj.c" "$TMP/v46rt.c" > "$TMP/v46iwac.eo" || fail "v46: オブジェクト出力 (一括) に失敗"
cmp -s "$TMP/v46iwac.eo" "$TMP/v46asari.eo" || fail "v46: Asari のオブジェクトが iwac と違う"
echo "ok: v46: Asari のオブジェクトが iwac と一致 (浮動小数点)"

# --- v47: enum と union (doc/enum-union.md) ---
cat > "$TMP/v47a.c" <<'V47A'
enum Color { RED, GREEN, BLUE };
enum { BUFSZ = 16 };
enum Val { A = 5, B, C = 20, D, NEG = -3, BIG = 1 << 10, FROMB = B + 2 };
int g = BLUE;
int table[3] = { RED, GREEN, BLUE };
char buf[BUFSZ];
int main() {
    int ok = 1;
    if (RED != 0 || GREEN != 1 || BLUE != 2) ok = 0;
    if (A != 5 || B != 6 || C != 20 || D != 21) ok = 0;
    if (NEG != -3 || NEG + 3 != 0) ok = 0;
    if (BIG != 1024 || FROMB != 8) ok = 0;
    if (g != 2 || table[0] != 0 || table[1] != 1 || table[2] != 2) ok = 0;
    buf[BUFSZ - 1] = 'Z';
    if (buf[15] != 'Z') ok = 0;
    putc(ok ? 'Y' : 'N');
    return 0;
}
V47A
"$IWAC" "$TMP/v47a.c" > "$TMP/v47a.bin"
check_tail "$TMP/v47a.bin" 'Y' "v47: enum の値・自動増加・定数式・初期化子・大きさ"
cat > "$TMP/v47b.c" <<'V47B'
enum Color { RED, GREEN, BLUE };
typedef enum Color Color;
enum Color pick(int i) { if (i == 0) return RED; return BLUE; }
int main() {
    int ok = 1;
    Color c = pick(1);
    if (c != BLUE) ok = 0;
    switch (c) { case RED: ok = 0; break; case BLUE: break; default: ok = 0; }
    if (sizeof(enum Color) != 8) ok = 0;
    int cnt = 0;
    for (Color k = RED; k <= BLUE; k++) cnt++;
    if (cnt != 3) ok = 0;
    putc(ok ? 'Y' : 'N');
    return 0;
}
V47B
"$IWAC" "$TMP/v47b.c" > "$TMP/v47b.bin"
check_tail "$TMP/v47b.bin" 'Y' "v47: enum の型・関数・switch・sizeof・typedef"
cat > "$TMP/v47c.c" <<'V47C'
enum Neg { NG = -3 };
int main() {
    int ok = 1;
    switch (NG) { case -3: break; default: ok = 0; }
    int x = NG;
    if (x != -3) ok = 0;
    putc(ok ? 'Y' : 'N');
    return 0;
}
V47C
"$IWAC" "$TMP/v47c.c" > "$TMP/v47c.bin"
check_tail "$TMP/v47c.bin" 'Y' "v47: 負の列挙子 (case ラベルと初期化)"
cat > "$TMP/v47d.c" <<'V47D'
union U { int i; char c[8]; double d; };
union W { char c[12]; int i; };
union U pass(union U u) { return u; }
int main() {
    int ok = 1;
    if (sizeof(union U) != 8) ok = 0;
    if (sizeof(union W) != 16) ok = 0;
    union U u;
    u.i = 65;
    if (u.c[0] != 'A' || u.i != 65) ok = 0;
    u.d = 1.5;
    if (u.d != 1.5) ok = 0;
    union U u2 = { 42 };
    if (u2.i != 42) ok = 0;
    union U u3;
    u3 = pass(u2);
    if (u3.i != 42) ok = 0;
    putc(ok ? 'Y' : 'N');
    return 0;
}
V47D
"$IWAC" "$TMP/v47d.c" > "$TMP/v47d.bin"
check_tail "$TMP/v47d.bin" 'Y' "v47: union の大きさ・読み替え・初期化子・値渡し"
cat > "$TMP/v47e.c" <<'V47E'
union U { int i; char c[8]; };
struct S { int tag; union U val; };
union V { struct S s; long l; };
typedef union U Num;
enum Color { RED, GREEN, BLUE };
enum Color palette[2] = { RED, BLUE };
struct Item { enum Color col; Num num; };
int main() {
    int ok = 1;
    struct S s;
    s.tag = 7;
    s.val.i = 99;
    if (s.tag != 7 || s.val.i != 99 || s.val.c[0] != 'c') ok = 0;
    union V v;
    v.l = 1234;
    if (v.l != 1234) ok = 0;
    v.s.tag = 5;
    if (v.s.tag != 5) ok = 0;
    union U arr[3];
    arr[1].i = 77;
    if (arr[1].i != 77 || arr[1].c[0] != 'M') ok = 0;
    struct Item it;
    it.col = BLUE;
    it.num.i = 66;
    if (it.col != 2 || it.num.c[0] != 'B') ok = 0;
    if (palette[0] != RED || palette[1] != BLUE) ok = 0;
    putc(ok ? 'Y' : 'N');
    return 0;
}
V47E
"$IWAC" "$TMP/v47e.c" > "$TMP/v47e.bin"
check_tail "$TMP/v47e.bin" 'Y' "v47: union と構造体の入れ子・配列・typedef"
cat > "$TMP/v47f.c" <<'V47F'
enum E { A, B };
enum E { A, B };
union U { int i; };
union U { int i; };
int main() {
    int ok = 1;
    if (A != 0 || B != 1) ok = 0;
    union U u;
    u.i = 3;
    if (u.i != 3) ok = 0;
    putc(ok ? 'Y' : 'N');
    return 0;
}
V47F
"$IWAC" "$TMP/v47f.c" > "$TMP/v47f.bin"
check_tail "$TMP/v47f.bin" 'Y' "v47: 同じ構成の再定義は受け付ける"
check_error 'enum E { A }; int main() { return B; }' "v47: 未定義の列挙子"
check_error 'enum E { A = 1 << 31 }; int main() { return 0; }' "v47: 列挙子の値が大きすぎる"
check_error 'enum E { A = 1 }; enum F { A = 2 }; int main() { return 0; }' "v47: 列挙子の同名・別値"
check_error 'int main() { enum E { A }; return 0; }' "v47: enum の定義が関数の中"
check_error 'enum E { A, B }; enum E { A, C }; int main() { return 0; }' "v47: enum の再定義が違う構成"
check_error 'enum E { A }; int main() { A = 3; return 0; }' "v47: 列挙子への代入"
check_error 'enum E { A }; int main() { int A; return 0; }' "v47: 列挙子と同名の変数"
check_error 'union U { int i; }; int main() { struct U x; return x.i; }' "v47: union のタグを struct で使う"
check_error 'struct S { int i; }; int main() { union S x; return x.i; }' "v47: struct のタグを union で使う"
check_error 'union U { int i; char c; }; int main() { union U u = { 1, 2 }; return 0; }' "v47: union の初期化子が2つ"
check_error 'struct P { int i; }; enum P { A }; int main() { return 0; }' "v47: enum と構造体のタグの衝突"
check_error 'int main() { union U { int i; }; return 0; }' "v47: union の定義が関数の中"
# オブジェクト経路: enum / union を含むプログラムでも、リンク結果が
# 一括ビルドと一致し、Asari のオブジェクトが iwac と一致すること
cat > "$TMP/v47obj.c" <<'V47OBJ'
enum Color { RED, GREEN, BLUE };
union U { int i; char c[8]; };
struct Item { enum Color col; union U val; };
struct Item mk(int i) { struct Item it; it.col = BLUE; it.val.i = i; return it; }
int main() {
    struct Item it = mk(65);
    return it.val.c[0] + it.col;
}
V47OBJ
cat > "$TMP/v47rt.c" <<'V47RT'
void kosaba_flush(void) { }
V47RT
printf 'void kosaba_flush();\n' > "$TMP/v47pre.c"
"$IWAC" --kosaba "$TMP/v47pre.c" "$TMP/v47obj.c" "$TMP/v47rt.c" > "$TMP/v47direct.elf" || fail "v47: 一括ビルドに失敗"
"$IWAC" --kosaba --elf "$TMP/v47pre.c" "$TMP/v47obj.c" > "$TMP/v47obj.eo" || fail "v47: オブジェクト出力に失敗"
"$IWAC" --kosaba --elf "$TMP/v47pre.c" "$TMP/v47rt.c" > "$TMP/v47rt.eo" || fail "v47: rt のオブジェクト出力に失敗"
"$TMP/nori" "$TMP/v47obj.eo" "$TMP/v47rt.eo" > "$TMP/v47link.elf" || fail "v47: nori でのリンクに失敗"
cmp -s "$TMP/v47direct.elf" "$TMP/v47link.elf" || fail "v47: リンク結果が一括ビルドと違う"
echo "ok: v47: enum / union を含むリンクが一括と一致"
"$IWAC" --kosaba --asari "$TMP/v47pre.c" "$TMP/v47obj.c" "$TMP/v47rt.c" > "$TMP/v47.ias" || fail "v47: Asari 形式の出力に失敗"
"$TMP/asari" "$TMP/v47.ias" > "$TMP/v47asari.eo" || fail "v47: Asari が失敗"
"$IWAC" --kosaba --elf "$TMP/v47pre.c" "$TMP/v47obj.c" "$TMP/v47rt.c" > "$TMP/v47iwac.eo" || fail "v47: オブジェクト出力 (一括) に失敗"
cmp -s "$TMP/v47iwac.eo" "$TMP/v47asari.eo" || fail "v47: Asari のオブジェクトが iwac と違う"
echo "ok: v47: Asari のオブジェクトが iwac と一致 (enum / union)"

# --- v48: do-while と sizeof(式) (doc/dowhile-sizeof.md) ---
cat > "$TMP/v48a.c" <<'V48A'
int main() {
    int ok = 1;
    int i = 0;
    int n = 0;
    do { n++; } while (n < 5);
    if (n != 5) ok = 0;
    do { i++; } while (i < 0);
    if (i != 1) ok = 0;
    i = 0;
    do i++; while (i < 3);
    if (i != 3) ok = 0;
    i = 0;
    do { i++; if (i == 4) break; } while (1);
    if (i != 4) ok = 0;
    i = 0; n = 0;
    do { i++; if (i % 2) continue; n++; } while (i < 10);
    if (n != 5) ok = 0;
    i = 0; n = 0;
    do { int j = 0; do { j++; n++; } while (j < 3); i++; } while (i < 2);
    if (n != 6) ok = 0;
    i = 0; n = 0;
    while (i < 3) { do { n++; } while (n % 2); i++; }
    if (n != 6) ok = 0;
    putc(ok ? 'Y' : 'N');
    return 0;
}
V48A
"$IWAC" "$TMP/v48a.c" > "$TMP/v48a.bin"
check_tail "$TMP/v48a.bin" 'Y' "v48: do-while (最低1回・break・continue・入れ子)"
cat > "$TMP/v48b.c" <<'V48B'
struct Point { int32 x; int32 y; };
struct Big { char c; int v; char buf[8]; struct Point p; };
int arr[10];
char cbuf[7];
int mat[3][4];
int16 sarr[5];
double darr[3];
int g;
char gc;
int16 gs;
uint32 gu;
double gd;
int *gp;
char *gcp;
struct Point gpt;
struct Big gb;
int geti() { return 42; }
double getd() { return 1.5; }
int16 gets() { return 7; }
char *getcp() { return "x"; }
struct Point getpt() { struct Point r; r.x = 1; r.y = 2; return r; }
int main() {
    int ok = 1;
    int i = 3;
    int *p = &g;
    char *cp = "hello";
    if (sizeof(g) != 8) ok = 0;
    if (sizeof(gc) != 1) ok = 0;
    if (sizeof(gs) != 2) ok = 0;
    if (sizeof(gu) != 4) ok = 0;
    if (sizeof(gd) != 8) ok = 0;
    if (sizeof(gp) != 8) ok = 0;
    if (sizeof(gpt) != 8) ok = 0;
    if (sizeof(gb) != 32) ok = 0;
    if (sizeof(*p) != 8) ok = 0;
    if (sizeof(*cp) != 1) ok = 0;
    if (sizeof(&g) != 8) ok = 0;
    if (sizeof(p + 1) != 8) ok = 0;
    if (sizeof(*(p + 1)) != 8) ok = 0;
    if (sizeof(*getcp()) != 1) ok = 0;
    if (sizeof(arr) != 80) ok = 0;
    if (sizeof(arr[0]) != 8) ok = 0;
    if (sizeof(cbuf) != 7) ok = 0;
    if (sizeof(mat) != 96) ok = 0;
    if (sizeof(mat[1]) != 32) ok = 0;
    if (sizeof(mat[1][2]) != 8) ok = 0;
    if (sizeof(sarr) != 10) ok = 0;
    if (sizeof(sarr[0]) != 2) ok = 0;
    if (sizeof(darr) != 24) ok = 0;
    if (sizeof(arr + 0) != 8) ok = 0;
    if (sizeof(42) != 8) ok = 0;
    if (sizeof('a') != 8) ok = 0;
    if (sizeof(1.5) != 8) ok = 0;
    if (sizeof(gs + gs) != 2) ok = 0;
    if (sizeof(gs + g) != 2) ok = 0;
    if (sizeof(gd + 1) != 8) ok = 0;
    if (sizeof(g == 1) != 8) ok = 0;
    if (sizeof(-gs) != 2) ok = 0;
    if (sizeof(gs << 1) != 2) ok = 0;
    if (sizeof(i > 0 ? gs : gu) != 2) ok = 0;
    if (sizeof((int32)g) != 4) ok = 0;
    if (sizeof(geti()) != 8) ok = 0;
    if (sizeof(getd()) != 8) ok = 0;
    if (sizeof(gets()) != 2) ok = 0;
    if (sizeof(getcp()) != 8) ok = 0;
    if (sizeof(getpt()) != 8) ok = 0;
    if (sizeof(gb.c) != 1) ok = 0;
    if (sizeof(gb.buf) != 8) ok = 0;
    if (sizeof(gb.buf[0]) != 1) ok = 0;
    if (sizeof(gb.p.x) != 4) ok = 0;
    if (sizeof("hello") != 6) ok = 0;
    if (sizeof("") != 1) ok = 0;
    if (sizeof g != 8) ok = 0;
    if (sizeof *p != 8) ok = 0;
    if (sizeof arr[0] != 8) ok = 0;
    i = 0;
    if (sizeof(i++) != 8) ok = 0;
    if (i != 0) ok = 0;
    putc(ok ? 'Y' : 'N');
    return 0;
}
V48B
"$IWAC" "$TMP/v48b.c" > "$TMP/v48b.bin"
check_tail "$TMP/v48b.bin" 'Y' "v48: sizeof(式) (変数・配列・メンバ・式・呼び出し・評価しない)"
cat > "$TMP/v48c.c" <<'V48C'
enum Color { RED, GREEN, BLUE };
union U { int i; char c[8]; double d; };
typedef int (*BinOp)(int, int);
int add(int a, int b) { return a + b; }
int buf2[sizeof(int)];
int x = sizeof(buf2);
int main() {
    int ok = 1;
    enum Color col = BLUE;
    union U u;
    BinOp fp = add;
    double d = 0.5;
    int i = 0;
    if (sizeof(col) != 8) ok = 0;
    if (sizeof(u) != 8) ok = 0;
    if (sizeof(u.c) != 8) ok = 0;
    if (sizeof(fp) != 8) ok = 0;
    if (sizeof(fp(1, 2)) != 8) ok = 0;
    if (sizeof(buf2) != 64) ok = 0;
    if (x != 64) ok = 0;
    switch (sizeof(char)) { case 1: break; default: ok = 0; }
    if (sizeof(sizeof(buf2)) != 8) ok = 0;
    i = 0;
    do { i++; d = d + 0.25; } while (d < 1.0);
    if (i != 2) ok = 0;
    i = 0;
    if (i == 0) do { i++; } while (i < 3); else i = 99;
    if (i != 3) ok = 0;
    i = 0;
    do {
        switch (i) { case 0: i = 5; break; default: i = 9; }
    } while (i < 5);
    if (i != 5) ok = 0;
    putc(ok ? 'Y' : 'N');
    return 0;
}
V48C
"$IWAC" "$TMP/v48c.c" > "$TMP/v48c.bin"
check_tail "$TMP/v48c.bin" 'Y' "v48: sizeof の定数文脈・enum/union・関数ポインタ・FP 条件の do-while"
cat > "$TMP/v48d.c" <<'V48D'
int main() {
    int i = 0;
    int n = 0;
    do {
        i++;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        n = n + 1;
        if (i == 2) continue;
        n = n + 1000;
    } while (i < 3);
    if (n != 2900) { putc(78); return 0; }
    i = 0;
    do { i++; if (i == 5) break; } while (1);
    if (i != 5) { putc(78); return 0; }
    putc(89);
    return 0;
}
V48D
"$IWAC" "$TMP/v48d.c" > "$TMP/v48d.bin"
check_tail "$TMP/v48d.bin" 'Y' "v48: 4KB を超える本体を持つ do-while (分岐の緩和)"
check_error 'int main() { int i = 0; do i++; return 0; }' "v48: do の後に while が無い"
check_error 'int main() { int i = 0; do { i++; } while (i < 3) return 0; }' "v48: do-while の終わりの ; が無い"
check_error 'int main() { return sizeof(); }' "v48: sizeof の中身が空"
check_error 'int main() { return sizeof(nosuch); }' "v48: sizeof の中の未定義の名前"
check_error 'typedef int (*F)(int); int f(int a) { return a; } int main() { F fp = f; return sizeof(*fp); }' "v48: 関数の sizeof"
# オブジェクト経路: do-while と sizeof を含むプログラムでも、リンク結果が
# 一括ビルドと一致し、Asari のオブジェクトが iwac と一致すること
cat > "$TMP/v48obj.c" <<'V48OBJ'
int tbl[4];
int sum() {
    int i = 0;
    int s = 0;
    do { s = s + tbl[i]; i++; } while (i < sizeof(tbl) / sizeof(tbl[0]));
    return s;
}
int main() {
    int i = 0;
    do { tbl[i] = i + 1; i++; } while (i < 4);
    return sum();
}
V48OBJ
cat > "$TMP/v48rt.c" <<'V48RT'
void kosaba_flush(void) { }
V48RT
printf 'void kosaba_flush();\n' > "$TMP/v48pre.c"
"$IWAC" --kosaba "$TMP/v48pre.c" "$TMP/v48obj.c" "$TMP/v48rt.c" > "$TMP/v48direct.elf" || fail "v48: 一括ビルドに失敗"
"$IWAC" --kosaba --elf "$TMP/v48pre.c" "$TMP/v48obj.c" > "$TMP/v48obj.eo" || fail "v48: オブジェクト出力に失敗"
"$IWAC" --kosaba --elf "$TMP/v48pre.c" "$TMP/v48rt.c" > "$TMP/v48rt.eo" || fail "v48: rt のオブジェクト出力に失敗"
"$TMP/nori" "$TMP/v48obj.eo" "$TMP/v48rt.eo" > "$TMP/v48link.elf" || fail "v48: nori でのリンクに失敗"
cmp -s "$TMP/v48direct.elf" "$TMP/v48link.elf" || fail "v48: リンク結果が一括ビルドと違う"
echo "ok: v48: do-while / sizeof を含むリンクが一括と一致"
"$IWAC" --kosaba --asari "$TMP/v48pre.c" "$TMP/v48obj.c" "$TMP/v48rt.c" > "$TMP/v48.ias" || fail "v48: Asari 形式の出力に失敗"
"$TMP/asari" "$TMP/v48.ias" > "$TMP/v48asari.eo" || fail "v48: Asari が失敗"
"$IWAC" --kosaba --elf "$TMP/v48pre.c" "$TMP/v48obj.c" "$TMP/v48rt.c" > "$TMP/v48iwac.eo" || fail "v48: オブジェクト出力 (一括) に失敗"
cmp -s "$TMP/v48iwac.eo" "$TMP/v48asari.eo" || fail "v48: Asari のオブジェクトが iwac と違う"
echo "ok: v48: Asari のオブジェクトが iwac と一致 (do-while / sizeof)"

# --- v49: ポインタ演算のスケール修正 (doc/ptr-arith.md) ---
cat > "$TMP/v49a.c" <<'V49A'
int64 a64[4] = { 100, 200, 300, 400 };
int32 a32[4] = { 100000, 200000, 300000, 400000 };
int16 a16[4] = { 100, 200, 300, 400 };
uint32 au[4] = { 70000, 80000, 90000, 100000 };
char ac[5] = { 10, 20, 30, 40, 50 };
int main() {
    int r = 0;
    int64 *p64 = &a64[0]; p64 = p64 + 1; if (*p64 == 200) r = r + 1;
    int32 *p32 = &a32[0]; p32 = p32 + 1; if (*p32 == 200000) r = r + 1;
    int16 *p16 = &a16[0]; p16 = p16 + 1; if (*p16 == 200) r = r + 1;
    uint32 *pu = &au[0]; pu = pu + 1; if (*pu == 80000) r = r + 1;
    char *pc = &ac[0]; pc = pc + 1; if (*pc == 20) r = r + 1;
    int32 *q = &a32[3]; q = q - 2; if (*q == 200000) r = r + 1;
    int32 *q2 = &a32[0]; q2++; if (*q2 == 200000) r = r + 1;
    int32 *q3 = &a32[3]; q3--; if (*q3 == 300000) r = r + 1;
    int32 *q4 = &a32[0]; q4 += 3; if (*q4 == 400000) r = r + 1;
    int32 *q5 = &a32[3]; q5 -= 1; if (*q5 == 300000) r = r + 1;
    int32 *q6 = &a32[0]; if (*(q6 + 2) == 300000) r = r + 1;
    int32 *q7 = &a32[3]; if (*(q7 - 1) == 300000) r = r + 1;
    int32 *q8 = a32 + 1; if (*q8 == 200000) r = r + 1;
    int32 *q9 = 1 + a32; if (*q9 == 200000) r = r + 1;
    int32 tab[16];
    int i = 0;
    while (i < 16) { tab[i] = i + 1; i = i + 1; }
    int32 *w = &tab[0];
    i = 0;
    while (i < 16) { w = w + 1; i = i + 1; }
    if (*(w - 1) == 16) r = r + 1;
    if (r == 15) putc(89); else putc(78);
    return 0;
}
V49A
"$IWAC" "$TMP/v49a.c" > "$TMP/v49a.bin"
check_tail "$TMP/v49a.bin" 'Y' "v49: 幅のある型の p±n・++/--・+=/-= ・*(p±n)・配列名+整数"
cat > "$TMP/v49b.c" <<'V49B'
struct Big { int64 a; int64 b; int64 c; };
struct Big bigs[3];
struct Huge { int64 x[300]; };
struct Huge hs[2];
int32 t32[6] = { 111, 222, 333, 444, 555, 666 };
int32 m2[3][4] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
struct Holder { int32 *p; int32 x; };
int32 *gparr[3];
int main() {
    int r = 0;
    bigs[1].a = 1000; bigs[2].a = 2000;
    struct Big *bp = &bigs[0]; bp = bp + 1; if (bp->a == 1000) r = r + 1;
    bp++; if (bp->a == 2000) r = r + 1;
    struct Big *bq = &bigs[2]; if (bq - bp == 0) r = r + 1;
    bq = &bigs[0]; if (bp - bq == 2) r = r + 1;
    int32 *p = &t32[5]; int32 *q = &t32[1];
    if (p - q == 4) r = r + 1;
    if (q - p == 0 - 4) r = r + 1;
    int32 *a2 = m2 + 1; if (*a2 == 4) r = r + 1;
    hs[1].x[0] = 777;
    struct Huge *hp = &hs[0]; hp++; if (hp->x[0] == 777) r = r + 1;
    hp--; hp++; if (hp->x[0] == 777) r = r + 1;
    int32 *base = &t32[0];
    int32 **pp = &base; *pp = *pp + 1; if (*base == 222) r = r + 1;
    int32 *arr[3];
    arr[0] = &t32[0]; arr[0]++; if (*arr[0] == 222) r = r + 1;
    struct Holder h; h.p = &t32[0]; h.p++; if (*h.p == 222) r = r + 1;
    gparr[0] = &t32[2]; if (*gparr[0] == 333) r = r + 1;
    if (r == 13) putc(89); else putc(78);
    return 0;
}
V49B
"$IWAC" "$TMP/v49b.c" > "$TMP/v49b.bin"
check_tail "$TMP/v49b.bin" 'Y' "v49: ポインタ差・構造体ポインタ・二重ポインタ・要素/メンバの ++"
cat > "$TMP/v49c.c" <<'V49C'
int32 t32[6] = { 111, 222, 333, 444, 555, 666 };
int32 *getp() { return &t32[2]; }
struct P { int64 a; };
int32 *pick(struct P p) { return &t32[p.a]; }
typedef int32 *(*FP)(void);
int main() {
    int r = 0;
    int32 *a = getp(); if (*a == 333) r = r + 1;
    a = a + 2; if (*a == 555) r = r + 1;
    FP fp = getp;
    int32 *b = fp(); if (*b == 333) r = r + 1;
    struct P p; p.a = 4;
    int32 *c = pick(p); if (*c == 555) r = r + 1;
    if (r == 4) putc(89); else putc(78);
    return 0;
}
V49C
"$IWAC" "$TMP/v49c.c" > "$TMP/v49c.bin"
check_tail "$TMP/v49c.bin" 'Y' "v49: ポインタを返す関数 (直接・関数ポインタ・B方式) の戻り値"
cat > "$TMP/v49obj.c" <<'V49OBJ'
int32 tbl[6] = { 111, 222, 333, 444, 555, 666 };
int32 *second() { return &tbl[1]; }
int walk() {
    int32 *p = &tbl[0];
    int s = 0;
    int i = 0;
    while (i < 6) { s = s + *p; p = p + 1; i = i + 1; }
    return s;
}
int main() {
    int32 *p = second();
    p = p + 3;
    if (*p != 555) return 1;
    if (walk() != 2331) return 2;
    return 0;
}
V49OBJ
cat > "$TMP/v49rt.c" <<'V49RT'
void kosaba_flush(void) { }
V49RT
printf 'void kosaba_flush();\n' > "$TMP/v49pre.c"
"$IWAC" --kosaba "$TMP/v49pre.c" "$TMP/v49obj.c" "$TMP/v49rt.c" > "$TMP/v49direct.elf" || fail "v49: 一括ビルドに失敗"
"$IWAC" --kosaba --elf "$TMP/v49pre.c" "$TMP/v49obj.c" > "$TMP/v49obj.eo" || fail "v49: オブジェクト出力に失敗"
"$IWAC" --kosaba --elf "$TMP/v49pre.c" "$TMP/v49rt.c" > "$TMP/v49rt.eo" || fail "v49: rt のオブジェクト出力に失敗"
"$TMP/nori" "$TMP/v49obj.eo" "$TMP/v49rt.eo" > "$TMP/v49link.elf" || fail "v49: nori でのリンクに失敗"
cmp -s "$TMP/v49direct.elf" "$TMP/v49link.elf" || fail "v49: リンク結果が一括ビルドと違う"
echo "ok: v49: ポインタ演算を含むリンクが一括と一致"
"$IWAC" --kosaba --asari "$TMP/v49pre.c" "$TMP/v49obj.c" "$TMP/v49rt.c" > "$TMP/v49.ias" || fail "v49: Asari 形式の出力に失敗"
"$TMP/asari" "$TMP/v49.ias" > "$TMP/v49asari.eo" || fail "v49: Asari が失敗"
"$IWAC" --kosaba --elf "$TMP/v49pre.c" "$TMP/v49obj.c" "$TMP/v49rt.c" > "$TMP/v49iwac.eo" || fail "v49: オブジェクト出力 (一括) に失敗"
cmp -s "$TMP/v49iwac.eo" "$TMP/v49asari.eo" || fail "v49: Asari のオブジェクトが iwac と違う"
echo "ok: v49: Asari のオブジェクトが iwac と一致 (ポインタ演算)"

# --- v50: #define の上限 (16 → 64。設計は doc/define-limit.md) ---
# 40 個の define を使う (16 の壁を超えることが要点)
{ i=0; while [ $i -lt 40 ]; do echo "#define K$i $((i + 1))"; i=$((i + 1)); done;
  echo 'putc(K0 + K19 + K39 + 4);'; } > "$TMP/v50a.c"
"$IWAC" "$TMP/v50a.c" > "$TMP/v50a.bin"
check_tail "$TMP/v50a.bin" A "v50: #define 40 個 (K0+K19+K39+4 = 65)"

# ちょうど 64 個までは通る
{ i=0; while [ $i -lt 64 ]; do echo "#define L$i 1"; i=$((i + 1)); done;
  echo 'putc(66);'; } > "$TMP/v50b.c"
"$IWAC" "$TMP/v50b.c" > "$TMP/v50b.bin"
check_tail "$TMP/v50b.bin" B "v50: #define 64 個ちょうど"

# --kosaba では定義済みが 2 個 (__IWAC__ / __KOSABA__) あるが、
# 利用者分はそちらでも 64 個ちょうど書ける
"$IWAC" --kosaba "$TMP/v50b.c" > "$TMP/v50b.elf" || fail "v50: --kosaba で #define 64 個が通らなかった"
echo "ok: v50: #define 64 個 (--kosaba でも利用者分は 64 個)"

# 65 個目は上限エラー (文言も確かめる)
{ i=0; while [ $i -lt 65 ]; do echo "#define M$i 1"; i=$((i + 1)); done;
  echo 'putc(65);'; } > "$TMP/v50c.c"
if "$IWAC" "$TMP/v50c.c" > "$TMP/v50c.bin" 2> "$TMP/v50c.err"; then
    fail "v50: 65 個目の #define がエラーにならなかった"
fi
[ ! -s "$TMP/v50c.bin" ] || fail "v50: エラーなのにバイナリが出力された"
grep -q '64個まで' "$TMP/v50c.err" || fail "v50: 上限のエラー文言が違う"
echo "ok: v50: 65 個目の #define は上限エラーになり、出力は空だった"

# --- v51: #include (設計は doc/include.md) ---
# 取り込みは実ファイルが要るので、この節は $TMP の下に
# ヘッダと本体を用意してコンパイルする
mkdir -p "$TMP/inc/sub"
cat > "$TMP/inc/common.h" <<'EOF'
#ifndef COMMON_H
#define COMMON_H
#define IVAL 65
#endif
EOF
cat > "$TMP/inc/helper.h" <<'EOF'
int twice(int x) { return x * 2; }
EOF
cat > "$TMP/inc/sub/inner.h" <<'EOF'
#define DEEPV 66
EOF
cat > "$TMP/inc/sub/outer.h" <<'EOF'
#include "inner.h"
EOF
cat > "$TMP/inc/main.c" <<'EOF'
#include "common.h"
#include "common.h"
#include "helper.h"
putc(IVAL - 1 + twice(1));
EOF
"$IWAC" "$TMP/inc/main.c" > "$TMP/v51a.bin"
check_run "$TMP/v51a.bin" B "v51: #include (define と関数、guard で二重取り込み可)"
cat > "$TMP/inc/main2.c" <<'EOF'
#include "sub/outer.h"
putc(DEEPV);
EOF
"$IWAC" "$TMP/inc/main2.c" > "$TMP/v51b.bin"
check_run "$TMP/v51b.bin" B "v51: 入れ子の #include (取り込む側のディレクトリから解決)"
cat > "$TMP/inc/main3.c" <<'EOF'
#ifdef NOPE
#include "missing.h"
#endif
#include <common.h>
putc(IVAL);
EOF
(cd "$TMP/inc" && "$IWAC" main3.c > "$TMP/v51c.bin")
check_run "$TMP/v51c.bin" A "v51: <...> はカレントから、無効な範囲の取り込みは読まない"
cat > "$TMP/inc/main4.c" <<'EOF'
#include "missing.h"
putc(65);
EOF
if "$IWAC" "$TMP/inc/main4.c" > "$TMP/v51d.bin" 2>"$TMP/v51d.err"; then
    fail "v51: 無いファイルの #include はエラーになるはず"
fi
grep -q "#include のファイルが開けません" "$TMP/v51d.err" || fail "v51: 無いファイルのエラー文が違う"
echo "ok: v51: 無いファイルの #include はエラー"
cat > "$TMP/inc/self.h" <<'EOF'
#include "self.h"
EOF
cat > "$TMP/inc/main5.c" <<'EOF'
#include "self.h"
putc(65);
EOF
if "$IWAC" "$TMP/inc/main5.c" > "$TMP/v51e.bin" 2>"$TMP/v51e.err"; then
    fail "v51: guard 無しの自己取り込みはエラーになるはず"
fi
grep -q "#include の入れ子が深すぎます" "$TMP/v51e.err" || fail "v51: 深さエラーの文が違う"
echo "ok: v51: guard 無しの自己取り込みは深さエラー"
if "$IWAC" --kosaba "$TMP/inc/main2.c" > "$TMP/v51f.elf" 2>"$TMP/v51f.err"; then
    [ -s "$TMP/v51f.elf" ] || fail "v51: --kosaba の #include で ELF が空"
    echo "ok: v51: --kosaba でも #include できる"
else
    fail "v51: --kosaba の #include が失敗: $(cat "$TMP/v51f.err")"
fi

# --- v52: peephole 最適化 (設計は doc/peephole.md) ---
# 振る舞いが変わらないこと (混合プログラムの出力) と、
# 最適化が実際に効いていること (--asari の命令数と
# ロードの mv への転送) の両方を見る
cat > "$TMP/v52a.c" <<'EOF'
int add(int a, int b) { return a + b; }
int arr[4];
int i;
int s;
s = 0;
for (i = 0; i < 4; i++) {
    arr[i] = add(i, 10);
}
for (i = 0; i < 4; i++) {
    s = s + arr[i];
}
putc(65 + (s - 46));
EOF
"$IWAC" "$TMP/v52a.c" > "$TMP/v52a.bin"
check_run "$TMP/v52a.bin" A "v52: peephole 後も混合プログラムの出力は不変"
cat > "$TMP/v52b.c" <<'EOF'
int add(int a, int b) { return a + b; }
int x;
int y;
x = add(40, 2);
y = x;
putc(y);
x = add(x, 23);
putc(x);
EOF
"$IWAC" --kosaba --asari "$TMP/v52b.c" > "$TMP/v52b.asm"
n52=$(grep -c '^i ' "$TMP/v52b.asm")
[ "$n52" -le 66 ] || fail "v52: 命令数が増えている ($n52、最適化前は 66)"
ld52=$(grep -c '^i \(ld\|lw\) ' "$TMP/v52b.asm")
[ "$ld52" -le 9 ] || fail "v52: peephole の転送が効いていない (ロード $ld52、最適化前は 12)"
echo "ok: v52: peephole が効いている (命令数 $n52、ロード $ld52)"

# --- v53: 条件分岐の融合 (設計は doc/branch-fusion.md) ---
# 比較・&& || !・三項・do-while・uint64 の符号なし比較を
# 混ぜたプログラムの出力が不変なことと、条件の位置で
# slt 系が出ず分岐に直ることを見る
cat > "$TMP/v53a.c" <<'EOF'
int check(int a, int b) {
    int r = 0;
    if (a < b) r = r + 1;
    if (a <= b) r = r + 2;
    if (a > b) r = r + 4;
    if (a >= b) r = r + 8;
    if (a == b) r = r + 16;
    if (a != b) r = r + 32;
    return r;
}
int s;
int i;
uint64 u;
s = check(3, 5) + check(5, 5) + check(7, 5);
i = 0;
do { i = i + 1; } while (i < 3 && s > 100);
s = s + i;
s = (s >= 108 && !(s == 109)) ? s + 1 : 0;
u = 0 - 1;
if (u > 1) s = s + 1;
if (u < 1) s = s + 50;
putc(65 + (s - 110));
EOF
"$IWAC" "$TMP/v53a.c" > "$TMP/v53a.bin"
check_run "$TMP/v53a.bin" A "v53: 融合後も条件まわりの出力は不変"
cat > "$TMP/v53b.c" <<'EOF'
int a;
int b;
int s;
a = 3;
b = 5;
s = 0;
if (a < b) s = s + 1;
if (a <= b) s = s + 1;
if (a > b) s = s + 1;
if (a >= b) s = s + 1;
if (a == b) s = s + 1;
if (a != b) s = s + 1;
while (s < 100) s = s + 10;
putc(65 + (s - 103));
EOF
"$IWAC" --kosaba --asari "$TMP/v53b.c" > "$TMP/v53b.asm"
n53=$(grep -c '^i ' "$TMP/v53b.asm")
[ "$n53" -le 134 ] || fail "v53: 命令数が増えている ($n53、融合前は 145)"
slt53=$(grep -c '^i \(slt\|sltu\|sltiu\) ' "$TMP/v53b.asm" || true)
[ "$slt53" -eq 0 ] || fail "v53: 条件の位置に slt 系が残っている ($slt53、融合前は 7)"
echo "ok: v53: 分岐の融合が効いている (命令数 $n53、slt 系 $slt53)"

# --- v54: 古典スペルの unsigned の修正 ---
# unsigned long 等が符号つきとして扱われていた
# (型の言葉を読み捨てるモデルで、unsigned の印が
# 落ちていた)。比較・除算・剰余・右シフトが
# 符号なしになること、符号つきは不変なこと
cat > "$TMP/v54a.c" <<'EOF'
unsigned long u;
long s;
int r;
u = 0 - 1;
s = 0 - 1;
r = 0;
if (u > 1) r = r + 1;
if (s > 1) r = r + 2;
if ((u / 2) > 1000000) r = r + 4;
if ((s / 2) > 1000000) r = r + 8;
if ((u >> 60) == 15) r = r + 16;
if ((u % 10) == 5) r = r + 32;
putc(65 + (r - 53));
EOF
"$IWAC" "$TMP/v54a.c" > "$TMP/v54a.bin"
check_run "$TMP/v54a.bin" A "v54: unsigned long は符号なし、long は符号つきのまま"
cat > "$TMP/v54b.c" <<'EOF'
typedef unsigned long usz;
usz u;
unsigned char c;
unsigned int w;
int r;
u = 0 - 1;
r = (u > 1) ? 1 : 0;
c = 200;
c = c + 100;
if (c == 44) r = r + 1;
w = 0 - 1;
if ((w % 7) == 1) r = r + 1;
putc(65 + (r - 3));
EOF
"$IWAC" "$TMP/v54b.c" > "$TMP/v54b.bin"
check_run "$TMP/v54b.bin" A "v54: typedef・unsigned char・unsigned int でも符号なし"

# --- v55: 式評価の改善 (保持レジスタと即値形) ---
# 入れ子の二項演算で左の結果を t3 に保持する
# (スタック往復にしない) こと、定数相手は即値
# 命令になること、呼び出しを含む右辺では従来の
# スタック退避に落ちることを、命令列で確かめる
cat > "$TMP/v55a.c" <<'EOF'
int calc(int a, int b) {
    int r;
    r = (a + b) * (a - b);
    r = r + a * 8 + (a & 12) + (b | 3) + (a < 20);
    return r;
}
int withcall(int a, int b) {
    return (a + b) * calc(a, b);
}
EOF
"$IWAC" --kosaba --asari "$TMP/v55a.c" > "$TMP/v55a.txt"
hold55=$(grep -c '^i addi 28 6 0 0' "$TMP/v55a.txt" || true)
spill55=$(grep -c '^i sd 0 2 6 0' "$TMP/v55a.txt" || true)
andi55=$(grep -c '^i andi' "$TMP/v55a.txt" || true)
ori55=$(grep -c '^i ori' "$TMP/v55a.txt" || true)
slti55=$(grep -c '^i slti' "$TMP/v55a.txt" || true)
if [ "$hold55" -ge 1 ] && [ "$spill55" -ge 1 ] && [ "$andi55" -ge 1 ] && [ "$ori55" -ge 1 ] && [ "$slti55" -ge 1 ]; then
    echo "ok: v55: 保持レジスタ $hold55 か所・退避 $spill55 か所・即値形 andi $andi55 / ori $ori55 / slti $slti55"
else
    echo "FAIL: v55: 保持 $hold55・退避 $spill55・andi $andi55・ori $ori55・slti $slti55"
    NG=1
fi
# 機能面: 入れ子・即値形・三項・呼び出しを混ぜた式
cat > "$TMP/v55b.c" <<'EOF'
int f(int x) { return x * 3 + 1; }
int a;
int b;
int r;
a = 10;
b = 4;
r = (a + b) * (a - b);
r = r + ((a + 2) * (b + 3));
r = r + f(a + b);
r = r + (a > 5 ? (a + b) * 2 : 0);
r = r + a * 8 + b * 1 + a * 0;
r = r + (a << 3) + (b >> 1) + (a & 12) + (b | 3) + (a ^ 5);
r = r + (a < 20) + (a < 5);
putc('0' + (r / 100) % 10);
putc('0' + (r / 10) % 10);
putc('0' + r % 10);
EOF
"$IWAC" "$TMP/v55b.c" > "$TMP/v55b.bin"
check_run "$TMP/v55b.bin" 6 "v55: 式評価の混ぜ合わせ (末尾の桁、答えは 436)"

# --- v56: レジスタ割り付け (設計は doc/regalloc.md) ---
# 参照の多いローカル変数が s レジスタに割り付き、
# 序文で退避されること、住所を取る変数だけの関数
# では割り付かないことを命令列で確かめる
cat > "$TMP/v56a.c" <<'EOF'
int accumulate(int n) {
    int sum = 0;
    int i = 0;
    while (i < n) {
        sum = sum + i;
        i++;
    }
    return sum;
}
int withptr(void) {
    int x = 5;
    int *p = &x;
    *p = 9;
    return x;
}
EOF
"$IWAC" --kosaba --asari "$TMP/v56a.c" > "$TMP/v56a.txt"
save56=$(grep -c '^i sd 0 8 \(9\|1[89]\|2[0-7]\) -' "$TMP/v56a.txt" || true)
cat > "$TMP/v56b.c" <<'EOF'
int withptr(void) {
    int x = 5;
    int *p = &x;
    *p = 9;
    return x;
}
EOF
"$IWAC" --kosaba --asari "$TMP/v56b.c" > "$TMP/v56b.txt"
save56b=$(grep -c '^i sd 0 8 \(9\|1[89]\|2[0-7]\) -' "$TMP/v56b.txt" || true)
if [ "$save56" -ge 1 ] && [ "$save56b" -eq 0 ]; then
    echo "ok: v56: s レジスタの退避 $save56 か所 (住所を取る変数だけの関数では 0)"
else
    echo "FAIL: v56: 退避 $save56 か所 / 住所のみ $save56b か所"
    NG=1
fi
# 機能面: 再帰・呼び出し跨ぎ・住所を取る変数・
# 幅つき型の ++ (桁あふれ)・後置 -- を混ぜる
cat > "$TMP/v56c.c" <<'EOF'
typedef unsigned char u8;
typedef unsigned int u32;
int fib(int n) {
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}
int accumulate(int n) {
    int sum = 0;
    int i = 0;
    while (i < n) {
        sum = sum + i;
        i++;
    }
    return sum;
}
int withptr(void) {
    int x = 5;
    int *p = &x;
    *p = 9;
    int y = 0;
    int i = 0;
    while (i < 10) {
        y = y + x;
        i++;
    }
    return y;
}
int u8wrap(void) {
    u8 c = 0;
    int i = 0;
    while (i < 300) {
        c++;
        i++;
    }
    return c;
}
int postdec(void) {
    u32 v = 100;
    int total = 0;
    int i = 0;
    while (i < 4) {
        total = total + (int)(v--);
        i++;
    }
    return total;
}
int main(void) {
    int r = fib(15);
    putc('0' + r / 100 % 10);
    putc('0' + r / 10 % 10);
    putc('0' + r % 10);
    putc(' ');
    r = accumulate(100);
    putc('0' + r / 1000 % 10);
    putc('0' + r / 100 % 10);
    putc('0' + r / 10 % 10);
    putc('0' + r % 10);
    putc(' ');
    r = withptr();
    putc('0' + r / 10 % 10);
    putc('0' + r % 10);
    putc(' ');
    r = u8wrap();
    putc('0' + r / 10 % 10);
    putc('0' + r % 10);
    putc(' ');
    r = postdec();
    putc('0' + r / 100 % 10);
    putc('0' + r / 10 % 10);
    putc('0' + r % 10);
    return 0;
}
EOF
"$IWAC" "$TMP/v56c.c" > "$TMP/v56c.bin"
check_tail "$TMP/v56c.bin" "610 4950 90 44 394" "v56: レジスタ割り付けの機能一式"

# --- v57: ループ最適化 (設計は doc/loop-opt.md) ---
# 帰納変数を添字にするアクセスが歩くポインタに
# 替わること (slli が初期化の1回だけになる) と、
# 対象外のループでは従来の生成が残ることを見る
cat > "$TMP/v57a.c" <<'EOF'
int g[8];
int sum8(void) {
    int s = 0;
    int i = 0;
    while (i < 8) {
        s = s + g[i];
        i++;
    }
    return s;
}
int ctrl(void) {
    int s = 0;
    int i = 0;
    int j = 3;
    while (i < 8) {
        s = s + g[j];
        i++;
    }
    return s;
}
EOF
"$IWAC" --kosaba --asari "$TMP/v57a.c" > "$TMP/v57a.txt"
slli57=$(grep -c '^i slli' "$TMP/v57a.txt" || true)
if [ "$slli57" -eq 2 ]; then
    echo "ok: v57: slli は初期化と対象外ループの2か所だけ"
else
    echo "FAIL: v57: slli が $slli57 か所 (2のはず)"
    NG=1
fi
# 機能面: for / while / do-while、ポインタ基底、
# 負の歩幅、定数オフセット、入れ子、条件の中の
# ++、分岐の中の変更、対象外の添字、char 配列、
# 書き込みを混ぜる (nested の期待値は 24 + 9 = 33)
cat > "$TMP/v57c.c" <<'EOF'
int g[8] = {3, 1, 4, 1, 5, 9, 2, 6};
int h[3] = {1, 2, 3};
char ca[5] = {65, 66, 67, 68, 69};
void print_int(int v) {
    int d[8];
    int n = 0;
    if (v == 0) {
        putc('0');
        return;
    }
    while (v > 0) {
        d[n] = v % 10;
        n++;
        v = v / 10;
    }
    while (n > 0) {
        n--;
        putc('0' + d[n]);
    }
}
int fwd(void) {
    int s = 0;
    int i = 0;
    for (i = 0; i < 8; i++)
        s = s + g[i];
    return s;
}
int ptrsum(void) {
    int a[6];
    int i = 0;
    int s = 0;
    int *p = a;
    for (i = 0; i < 6; i++)
        a[i] = i * i;
    for (i = 0; i < 6; i++)
        s = s + p[i];
    return s;
}
int back(void) {
    int s = 0;
    int i = 0;
    for (i = 7; i >= 0; i--)
        s = s + g[i];
    return s;
}
int offset(void) {
    int s = 0;
    int i = 0;
    while (i < 7) {
        s = s + g[i + 1];
        i++;
    }
    return s;
}
int nested(void) {
    int s = 0;
    int i = 0;
    int j = 0;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 3; j++)
            s = s + h[j];
        s = s + g[i];
    }
    return s;
}
int condmod(void) {
    int s = 0;
    int i = 0;
    while (i++ < 5)
        s = s + g[i];
    return s;
}
int branchmod(void) {
    int s = 0;
    int i = 0;
    while (i < 8) {
        s = s + g[i];
        if (i == 2)
            i++;
        i++;
    }
    return s;
}
int fallback(void) {
    int s = 0;
    int i = 0;
    while (i < 7) {
        s = s + g[i + (i % 2)];
        i++;
    }
    return s;
}
int dowhile(void) {
    int s = 0;
    int i = 0;
    do {
        s = s + g[i];
        i++;
    } while (i < 4);
    return s;
}
int bigstep(void) {
    int s = 0;
    int i = 0;
    for (i = 0; i < 8; i = i + 2)
        s = s + g[i];
    return s;
}
int charsum(void) {
    int s = 0;
    int i = 0;
    for (i = 0; i < 5; i++)
        s = s + ca[i];
    return s;
}
int writeread(void) {
    int a[8];
    int *p = a;
    int i = 0;
    int s = 0;
    for (i = 0; i < 8; i++)
        p[i] = i * 3;
    for (i = 0; i < 8; i++)
        s = s + a[i];
    return s;
}
int main(void) {
    print_int(fwd());
    putc(' ');
    print_int(ptrsum());
    putc(' ');
    print_int(back());
    putc(' ');
    print_int(offset());
    putc(' ');
    print_int(nested());
    putc(' ');
    print_int(condmod());
    putc(' ');
    print_int(branchmod());
    putc(' ');
    print_int(fallback());
    putc(' ');
    print_int(dowhile());
    putc(' ');
    print_int(bigstep());
    putc(' ');
    print_int(charsum());
    putc(' ');
    print_int(writeread());
    return 0;
}
EOF
"$IWAC" "$TMP/v57c.c" > "$TMP/v57c.bin"
check_tail "$TMP/v57c.bin" "31 55 31 28 33 20 30 25 9 14 335 84" "v57: ループ最適化の機能一式"

# --- v58: インライン化 (設計は doc/inline.md) ---
# 構造: 対象の呼び出しでは jal が出ないこと。
# 前置きの jal (main への入口) だけが残る
cat > "$TMP/v58a.c" <<'EOF'
int sq(int x) { return x * x; }
int main() {
    int s = 0;
    int i = 0;
    for (i = 0; i < 4; i++)
        s = s + sq(i);
    return s;
}
EOF
"$IWAC" --kosaba --asari "$TMP/v58a.c" > "$TMP/v58a.txt"
jal58=$(grep -c '^i jal 1 ' "$TMP/v58a.txt" || true)
if [ "$jal58" -eq 1 ]; then
    echo "ok: v58: 展開した呼び出しに jal が無い"
else
    echo "FAIL: v58: jal が $jal58 か所 (1のはず)"
    NG=1
fi
# 構造: 入れ子 (mulp → sat32) も両段とも広がること
cat > "$TMP/v58b.c" <<'EOF'
int32 sat32(int64 v)
{
    if (v > 2147483647) {
        return 2147483647;
    }
    if (v < -2147483647 - 1) {
        return -2147483647 - 1;
    }
    return (int32)v;
}
int32 mulp(int32 a, int32 c)
{
    int64 v;
    v = ((int64)a * c + ((int64)1 << 29)) >> 30;
    return sat32(v);
}
int32 acc;
int main()
{
    int32 s;
    int i;
    s = 0;
    for (i = 0; i < 100; i++) {
        s = s + mulp(s + i, 1234567);
    }
    acc = s;
    return 0;
}
EOF
"$IWAC" --kosaba --asari "$TMP/v58b.c" > "$TMP/v58b.txt"
jal58b=$(grep -c '^i jal 1 ' "$TMP/v58b.txt" || true)
if [ "$jal58b" -eq 1 ]; then
    echo "ok: v58: 入れ子の展開でも jal が無い"
else
    echo "FAIL: v58: 入れ子の jal が $jal58b か所 (1のはず)"
    NG=1
fi
# 機能面: mulp の飽和・丸め、入れ子の引数、再帰と
# 相互再帰 (対象外で従来の呼び出し)、ループを含む
# 本体、ポインタ引数、文としての void 呼び出し、
# 早期 return、double と配列ローカル (対象外)、
# 引数を書き換える関数と引数の住所を取る関数
# (直結されても意味が変わらないこと)
cat > "$TMP/v58c.c" <<'EOF'
int32 sat32(int64 v)
{
    if (v > 2147483647) {
        return 2147483647;
    }
    if (v < -2147483647 - 1) {
        return -2147483647 - 1;
    }
    return (int32)v;
}
int32 mulp(int32 a, int32 c)
{
    int64 v;
    v = ((int64)a * c + ((int64)1 << 29)) >> 30;
    return sat32(v);
}
void print64(int64 v) {
    int64 d[24];
    int n = 0;
    if (v == 0) {
        putc('0');
        return;
    }
    if (v < 0) {
        putc('-');
        v = -v;
    }
    while (v > 0) {
        d[n] = v % 10;
        n++;
        v = v / 10;
    }
    while (n > 0) {
        n--;
        putc('0' + (int)d[n]);
    }
}
void sp() { putc(' '); }
int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
int is_even(int n);
int is_odd(int n) {
    if (n == 0) return 0;
    return is_even(n - 1);
}
int is_even(int n) {
    if (n == 0) return 1;
    return is_odd(n - 1);
}
int sum_to(int n) {
    int s = 0;
    int i = 0;
    for (i = 1; i <= n; i++)
        s = s + i;
    return s;
}
int gx;
int getv(int *p) { return *p + 1; }
int cnt;
void bump(void) { cnt = cnt + 1; }
void setg(int v) {
    if (v < 0) return;
    gx = v;
}
double dh(double x) { return x * 2.0; }
char chv(void) { return 65; }
int arrf(void) {
    int a[4];
    a[0] = 10;
    a[1] = 20;
    a[2] = 12;
    return a[0] + a[1] + a[2];
}
int incw(int x) { x = x + 1; return x; }
int via_ptr(int x) {
    int *p = &x;
    *p = *p + 5;
    return x;
}
int main() {
    print64(mulp(16384, 1073741824)); sp();
    print64(mulp(1073741824, 1073741824)); sp();
    print64(mulp(2147483647, 1073741824)); sp();
    print64(mulp(-2147483647 - 1, 1073741824)); sp();
    print64(mulp(1000, 1234567)); sp();
    print64(mulp(100000, 1234567)); sp();
    print64(mulp(mulp(16384, 1073741824), 1073741824)); sp();
    print64(fib(15)); sp();
    print64(is_even(10) * 10 + is_odd(7)); sp();
    print64(sum_to(10)); sp();
    gx = 41;
    print64(getv(&gx)); sp();
    cnt = 0;
    bump();
    bump();
    print64(cnt); sp();
    gx = 7;
    setg(-1);
    print64(gx); sp();
    setg(33);
    print64(gx); sp();
    print64((int)dh(21)); sp();
    print64(chv()); sp();
    print64(arrf()); sp();
    gx = 10;
    print64(incw(gx) * 100 + gx); sp();
    print64(via_ptr(gx) * 100 + gx);
    return 0;
}
EOF
"$IWAC" "$TMP/v58c.c" > "$TMP/v58c.bin"
check_tail "$TMP/v58c.bin" "16384 1073741824 2147483647 -2147483648 1 115 16384 610 11 55 42 2 7 33 42 65 42 1110 1510" "v58: インライン化の機能一式"

# --- v59: レジスタートレードオフ (設計は doc/reg-tradeoff.md) ---
# 割り付けが 11 本満杯でも、帰納変数でない割り付けを
# 外して歩くポインタが入ること (ポインタの前進
# addi r r 0 8 が 1 か所出る。v58 では 0 か所) と、
# 外した変数を含めて機能が不変なことを見る
cat > "$TMP/v59a.c" <<'EOF'
int g[16];
int hot(void) {
    int a = 1;
    int b = 2;
    int c = 3;
    int d = 4;
    int e = 5;
    int f = 6;
    int h = 7;
    int j = 8;
    int k = 9;
    int l = 10;
    int m = 11;
    a = a + 1; a = a + 2; a = a + 3;
    b = b + 1; b = b + 2; b = b + 3;
    c = c + 1; c = c + 2; c = c + 3;
    d = d + 1; d = d + 2; d = d + 3;
    e = e + 1; e = e + 2; e = e + 3;
    f = f + 1; f = f + 2; f = f + 3;
    h = h + 1; h = h + 2; h = h + 3;
    j = j + 1; j = j + 2; j = j + 3;
    k = k + 1; k = k + 2; k = k + 3;
    l = l + 1; l = l + 2; l = l + 3;
    m = m + 1; m = m + 2; m = m + 3;
    int i = 0;
    int s = 0;
    while (i < 16) {
        s = s + g[i];
        i = i + 1;
    }
    return s + a + b + c + d + e + f + h + j + k + l + m;
}
EOF
"$IWAC" --kosaba --asari "$TMP/v59a.c" > "$TMP/v59a.txt"
adv59=$(grep -cE '^i addi ([0-9]+) \1 0 8' "$TMP/v59a.txt" || true)
if [ "$adv59" -eq 1 ]; then
    echo "ok: v59: 満杯でも歩くポインタが入る (前進が1か所)"
else
    echo "FAIL: v59: ポインタの前進が $adv59 か所 (1のはず)"
    NG=1
fi
# 機能面: synth_granule の形のミニチュア (寄せ集め
# ループ + m ループ配下の tau 2 連ループで di が
# 歩く)。割り付け満杯のまま、外した変数の値も
# 含めて結果が合うこと
cat > "$TMP/v59b.c" <<'EOF'
int tab[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
int coef[8] = {1, 2, 3, 4, 5, 6, 7, 8};
void printint(int v) {
    if (v >= 10) {
        printint(v / 10);
    }
    putc('0' + v % 10);
}
int work(int seed) {
    int a = seed;
    int b = seed + 1;
    int c = seed + 2;
    int d = seed + 3;
    int e = seed + 4;
    int f = seed + 5;
    int h = seed + 6;
    int j = seed + 7;
    int k = seed + 8;
    int l = seed + 9;
    a = a + b; a = a + c; a = a + d;
    b = b + e; b = b + f; b = b + h;
    c = c + j; c = c + k; c = c + l;
    int i = 0;
    int s = 0;
    while (i < 16) {
        s = s + tab[i];
        i = i + 1;
    }
    int m = 0;
    while (m < 4) {
        int di = 0;
        int tau = 0;
        while (tau <= m) {
            s = s + coef[di];
            di = di + 1;
            tau = tau + 1;
        }
        while (tau < 8) {
            s = s + coef[di];
            di = di + 1;
            tau = tau + 1;
        }
        m = m + 1;
    }
    return s + a + b + c;
}
int main(void) {
    printint(work(3));
    return 0;
}
EOF
"$IWAC" "$TMP/v59b.c" > "$TMP/v59b.bin"
check_tail "$TMP/v59b.bin" "364" "v59: トレードオフ後の機能一式"

# --- 容量レポート: --stats で使用量を集め、上限との余りを出す ---
# 合否ではなく点検のための表示。一括の代表 (nori 本体) と
# 断片 TU (decls.c + 各断片) の両方を測り、項目ごとに
# 大きい方を採る (関数や再配置は断片側が縛る)。
# (段3で上限が TU 基準になり、iwac 自身の一括は上限
# 超えになったため、一括側は nori 本体で代表する)
"$IWAC" --kosaba --stats nori/nori.c libdashi/libdashi.c kosaba/kosaba_rt.c > /dev/null 2> "$TMP/cap-all.txt"
for f in 01-head 02-parse 02b-decl 02c-fpconv 03-gen 03-geninl 03-genstmt 03b-genfp 04-encode 05-input 06-preproc 07-main; do
    "$IWAC" --kosaba --elf --stats src/decls.c "src/$f.c" > /dev/null 2> "$TMP/cap-$f.txt"
done
capmax() {
    awk -v k="$1" '$1 == "stats" && $2 == k { if ($3 > m) m = $3 } END { print m + 0 }' "$TMP"/cap-*.txt "$TMP/cap-all.txt"
}
capmax2() {
    awk -v k="$1" '$1 == "stats" && $2 == k { if ($4 > m) m = $4 } END { print m + 0 }' "$TMP"/cap-*.txt "$TMP/cap-all.txt"
}
capof() {
    grep -o "$1" "$2" | head -1 | grep -o '[0-9]\+'
}
capline() {
    if [ -n "$3" ]; then
        echo "capacity: $1 $2 / $3 (余り $(( $3 - $2 )))"
    else
        echo "capacity: $1 $2 / ?"
    fi
}
echo "--- 容量レポート (使用量 / 上限 / 余り) ---"
capline src "$(capmax src)" "$(capof 'src_buf\[[0-9]*\]' src/05-input.c)"
capline exp "$(capmax exp)" "$(capof 'exp_buf\[[0-9]*\]' src/06-preproc.c)"
capline nodes "$(capmax nodes)" "$(capof 'node_kind\[[0-9]*\]' src/02-parse.c)"
capline vars "$(capmax vars)" "$(capof 'var_etype\[[0-9]*\]' src/02-parse.c)"
capline funcs "$(capmax funcs)" "$(capof 'func_names\[[0-9]*\]' src/02-parse.c)"
capline arrays "$(capmax arrays)" "$(capof 'arr_etype\[[0-9]*\]' src/02-parse.c)"
capline sigs "$(capmax sigs)" "$(capof 'sig_ret_etype\[[0-9]*\]' src/02-parse.c)"
capline arrinit "$(capmax arrinit)" "$(capof 'arr_init_pool\[[0-9]\+\]' src/02-parse.c)"
capline strings "$(capmax strings)" "$(capof 'str_off\[[0-9]*\]' src/02-parse.c)"
capline strbytes "$(capmax2 strings)" "$(capof 'str_pool\[[0-9]*\]' src/02-parse.c)"
capline typedefs "$(capmax typedefs)" "$(capof 'ntdef >= [0-9]*' src/02b-decl.c)"
capline structs "$(capmax structs)" "$(capof 'nstag >= [0-9]*' src/02b-decl.c)"
capline insts "$(capmax insts)" "$(capof 'asm_op\[[0-9]*\]' src/03-gen.c)"
capline relocs "$(capmax relocs)" "$(capof 'obj_reloc_site\[[0-9]*\]' src/03-gen.c)"
capline dfaddr "$(capmax dfaddr)" "$(capof 'dfaddr_site\[[0-9]*\]' src/03-gen.c)"

# --- libdashi: 共通関数の束 ---
# ホストで小さなテストプログラムと一緒に組み、出力を見る
cat > "$TMP/ds_test.c" <<'DASHITEST'
#include <stdio.h>
int ds_strlen(char *s);
int ds_streq(char *a, char *b);
int ds_append(char *dst, int pos, char *src);
int ds_dec(char *buf, long v);
int ds_hex(char *buf, long v);
long ds_atoi(char *s);
int ds_hexval(int c);
void ds_puts(int fd, char *s);
void ds_putdec(int fd, long v);
int main(void) {
    char b[64];
    char dst[64];
    int p;
    p = 0;
    p = ds_append(dst, p, "ab");
    p = ds_append(dst, p, "cd");
    printf("%s %d %d %d\n", dst, ds_strlen(dst),
           ds_streq(dst, "abcd"), ds_streq(dst, "abc"));
    printf("%d %s\n", ds_dec(b, -12345), b);
    printf("%d %s\n", ds_hex(b, 6699), b);
    printf("%ld %d %d %d\n", ds_atoi("  -42x"),
           ds_hexval('f'), ds_hexval('F'),
           ds_hexval('z'));
    fflush(stdout);
    ds_puts(1, "puts-ok\n");
    ds_putdec(1, 777);
    printf("\n");
    return 0;
}
DASHITEST
if cc -std=c11 -O2 -o "$TMP/ds_test" "$TMP/ds_test.c" libdashi/libdashi.c 2>/dev/null; then
    "$TMP/ds_test" > "$TMP/ds_out.txt"
    cat > "$TMP/ds_expect.txt" <<'DASHIEXPECT'
abcd 4 1 0
6 -12345
4 1a2b
-42 15 15 -1
puts-ok
777
DASHIEXPECT
    cmp -s "$TMP/ds_expect.txt" "$TMP/ds_out.txt" || fail "libdashi: 関数の出力が期待と違う"
    echo "ok: libdashi: 文字列・数値変換・出力ヘルパ"
else
    echo "skip: libdashi (cc が無いため)"
fi

# --- 段6: 断片プロファイル (小メモリでのセルフホスト) ---
# iwac 本体と nori を「断片1つ分の容量」に絞ったプロファイルで
# 作り、静的データが 1MB 未満に収まることと、容量だけが違う
# こと (オブジェクト/リンク結果の一致) を確かめる。スクリプト
# 自身が判定して止まるので、ここでは成功したかだけを見る
if [ -n "$RISCV_CC" ]; then
    (cd kosaba && CC="$RISCV_CC" ./build-frag.sh > "$TMP/frag-iwac.log" 2>&1) || fail "段6: iwac の断片プロファイルに失敗"
    echo "ok: 段6: iwac 断片プロファイル (データ 1MB 未満、全断片のオブジェクト一致)"
    (cd nori && ./build-frag.sh > "$TMP/frag-nori.log" 2>&1) || fail "段6: nori の断片プロファイルに失敗"
    echo "ok: 段6: nori 断片プロファイル (データ 1MB 未満、リンク結果一致)"
else
    echo "skip: 段6: 断片プロファイル (RISC-V 用 gcc が無いため)"
fi

# --- 小容量ビルド「shirasu」(K210 プロファイル、v15 の節と同じ流儀) ---
# RISC-V 用 gcc が見つかるときだけ、kosaba/iwac-shirasu.elf が
# 作れて、データ量が 1MB に収まることを確かめる (スクリプト
# 自身が「標準と小容量でコンパイル結果が一致」も確認する。
# OS 上での実行確認は統合検証として別途実施)
if [ -n "$RISCV_CC" ]; then
    (cd kosaba && CC="$RISCV_CC" ./build-shirasu.sh >/dev/null 2>&1) || fail "小容量ビルド (shirasu) に失敗"
    memsz=$(readelf -l kosaba/iwac-shirasu.elf | grep -A1 'LOAD.*0x0000000040200000' | tail -1 | awk '{print $2}')
    [ -n "$memsz" ] || fail "小容量ビルド (shirasu): データセグメントが読めない"
    dec=$(printf '%d' "$memsz")
    [ "$dec" -lt 1048576 ] || fail "小容量ビルド (shirasu): データが 1MB を超えている ($dec バイト)"
    echo "ok: 小容量ビルド (shirasu): iwac-shirasu.elf のデータ $dec バイト (1MB 未満)"
else
    echo "skip: 小容量ビルド (shirasu、RISC-V 用 gcc が無いため)"
fi

echo "PASS"
