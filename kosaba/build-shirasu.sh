#!/bin/sh
# build-shirasu.sh -- 小容量版の iwac「shirasu」をビルドする
#
# この小容量版の名前は「shirasu」(2026-11-03 に命名)。
#
# 標準の iwac.elf は静的データが約1.75MB あり、データ領域が
# 1MB 級しかないボード (K210 の KosabaOS はデータ 1MB) には
# 入らない。実機 (Sipeed Maix Dock / K210) で Hello World を
# 達成したときに使われた容量縮小の構成 (2026-11-03、
# KosabaOS 側で実証) を、正式なビルドとして固定するのが
# このスクリプト。本体 (../src/) には手を入れず、コピーした
# 断片に「容量プロファイル」を書き換えで適用してから、
# 標準と同じ kosaba_crt.S / kosaba_lib.c / kosaba.ld で組む。
#
# 容量プロファイル (標準 → 小容量):
#   ノード表        20480 → 2048
#   命令表          40960 → 4096 (パッチ表と分岐緩和の印も同じ数)
#   変数表          1024  → 256
#   文字列プール    16384 → 4096 バイト (個数 512 は不変)
#   ソース上限      約384KB → 約32KB
#   展開バッファ    約384KB → 約32KB
# このとき出来る iwac の静的データは約371KB。コンパイル
# できるのは「小さいプログラム」だけになる (hello.c 級を
# ボード上でコンパイルするための構成で、iwac 自身の
# セルフコンパイルは標準構成の仕事)。
#
# 書き換えの約束:
#   表の大きさだけでなく、上限を調べる側の数字も一緒に
#   書き換える。片方だけ縮めると、大きい入力でバッファを
#   溢れて壊れる (実機構成のその場コピーでは実際に
#   ガード側が標準のまま残っていた。ここでは直してある)。
#   置換は対象ファイルごとに「何件替わったか」を数えて
#   確かめ、合わなければ止まる。本体側で数字の書き方が
#   変わったら、それに気付くための仕掛け。
#
# 使い方:
#   ./kosaba/build-shirasu.sh
#     → kosaba/iwac-shirasu.elf ができる
#   作る前に、小容量の iwac (ホスト版) と標準の iwac で
#   examples/hello-kosaba.c をコンパイルし、出力が一致
#   することを確かめる (容量だけが違うことの確認)。
#
# ツール選択は build-kosaba.sh と同じ順 ($CC、隣の
# KosabaOS の tools、システムの riscv gcc)。ホスト版の
# ビルドには別に $HOSTCC (既定 gcc) を使う。
set -e
cd "$(dirname "$0")"

if [ -z "$CC" ] && [ -x "../../../kosaba-os/tools/riscv-gcc/bin/riscv-none-elf-gcc" ]; then
    CC="../../../kosaba-os/tools/riscv-gcc/bin/riscv-none-elf-gcc"
fi
CC=${CC:-riscv64-linux-gnu-gcc}
HOSTCC=${HOSTCC:-gcc}
echo "build-shirasu: CC=$CC HOSTCC=$HOSTCC"

WORK=shirasu-build
rm -rf "$WORK"
mkdir -p "$WORK"
for f in ../src/0*.c; do
    cp "$f" "$WORK/"
done

# 書き換えの道具: ファイルと「元の文字列 → 後の文字列」と
# 期待する件数を渡す。件数が合わなければ止まる。
# 文字列は固定文字列として扱う (perl の \Q...\E)
apply() {
    file=$1; from=$2; to=$3; want=$4
    n=$(grep -c -F -- "$from" "$WORK/$file" || true)
    if [ "$n" != "$want" ]; then
        echo "build-shirasu: $file の '$from' が $n 件 (期待 $want 件)。本体が変わったかもしれないので止まります" >&2
        exit 1
    fi
    FROM="$from" TO="$to" perl -pi -e 's/\Q$ENV{FROM}\E/$ENV{TO}/g' "$WORK/$file"
    echo "build-shirasu: $file: '$from' → '$to' ($n 件)"
}

# 02-parse.c: ノード表と変数表と文字列プール。
# str_pool は [16384] が他と重なるので先に名指しで替える
apply 02-parse.c 'str_pool[16384]' 'str_pool[4096]' 1
apply 02-parse.c '[20480]' '[2048]' 6
apply 02-parse.c 'nnode >= 20480' 'nnode >= 2048' 1
apply 02-parse.c 'str_used >= 16383' 'str_used >= 4095' 1
apply 02-parse.c '[1024]' '[256]' 9
apply 02-parse.c 'nvar >= 1024' 'nvar >= 256' 1
# 03-gen.c: 命令表 (5つ) とパッチ表
apply 03-gen.c '[40960]' '[4096]' 7
# branch_long を消すループの上限も命令表に合わせる (v27 で追加
# されたループ。これを忘れると小さい配列をはみ出して書く)
apply 03-gen.c 'i < 40960' 'i < 4096' 1
# 05-input.c: ソースの箱と上限のガード (3か所)。
# エラー表示の数字も合わせる (2か所)
apply 05-input.c 'src_buf[393216]' 'src_buf[32768]' 1
apply 05-input.c '393215' '32767' 3
apply 05-input.c '約384KBまで' '約32KBまで' 2
# 06-preproc.c: 展開バッファと上限のガード
apply 06-preproc.c 'exp_buf[393216]' 'exp_buf[32768]' 1
apply 06-preproc.c 'exp_len >= 393215' 'exp_len >= 32767' 1

# 小容量の iwac (ホスト版) と、標準の iwac (ホスト版) を作る
cat "$WORK"/0*.c > "$WORK/iwac_all.c"
"$HOSTCC" -std=c11 -Wall -Wextra -O2 -o "$WORK/iwac-shirasu" "$WORK/iwac_all.c"
cat ../src/0*.c > "$WORK/iwac_std_all.c"
"$HOSTCC" -std=c11 -Wall -Wextra -O2 -o "$WORK/iwac-std" "$WORK/iwac_std_all.c"

# 容量だけが違うことの確認: 同じ入力からは同じ出力
"$WORK/iwac-std" --kosaba ../examples/hello-kosaba.c > "$WORK/hello-std.elf"
"$WORK/iwac-shirasu" --kosaba ../examples/hello-kosaba.c > "$WORK/hello-shirasu.elf"
if ! cmp -s "$WORK/hello-std.elf" "$WORK/hello-shirasu.elf"; then
    echo "build-shirasu: 標準と小容量でコンパイル結果が違います。止まります" >&2
    exit 1
fi
echo "build-shirasu: 標準と小容量で hello-kosaba.c の出力が一致"

# KosabaOS 用の ELF を組む (標準版と同じ舞台)
"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding \
    -nostdlib -no-pie -Wl,--build-id=none -O2 \
    -T kosaba.ld -o iwac-shirasu.elf "$WORK/iwac_all.c" kosaba_lib.c kosaba_crt.S
echo "build-shirasu: iwac-shirasu.elf を作りました"
