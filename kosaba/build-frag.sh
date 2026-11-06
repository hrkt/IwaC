#!/bin/sh
# build-frag.sh -- 断片プロファイルの iwac をビルドする (段6)
#
# 段6 (小メモリでのセルフホスト、doc/linking.md) の作戦A:
# 容量の定数を「iwac の断片1つを訳すのに要る大きさ」に
# 合わせた第2の小容量プロファイル。shirasu (小さい
# プログラム用) では iwac 自身の断片は訳せないので、
# 実測した断片ピークに余裕を乗せた値にしてある。
# 名前は当面の呼び名で、正式な命名は保留。
#
# 実測のピーク (2026-11-06、TU = decls.c + 断片):
#   ノード 6,085 (02-parse) / 命令 12,251 (02-parse)
#   再配置 994 (03-gen) / 変数 349 / 関数 117
#   文字列 5,521 バイト / 入力 113,357 バイト
#     (展開後もほぼ同量)
#
# 容量プロファイル (標準 → 断片):
#   ノード表        20480 → 8192
#   命令表          40960 → 16384 (パッチ表と分岐緩和の印も)
#   再配置表         8192 → 2048
#   変数表           1024 → 512
#   文字列プール    16384 → 8192 バイト (個数 512 は不変)
#   ソース上限      約384KB → 128KB
#   展開バッファ    約384KB → 128KB
# 狙いは静的データ 1MB 未満 (K210 のデータ領域)。
# 出来た ELF のデータ量を最後に測り、1MB 以上なら
# 失敗として止まる (段6 の判定そのもの)。
#
# 書き換えの約束は build-shirasu.sh と同じ: 表と
# ガード側の数字を一緒に替え、件数を数えて確かめる。
#
# 使い方:
#   ./kosaba/build-frag.sh
#     → kosaba/iwac-frag.elf ができる
#   作る前に、断片プロファイルの iwac (ホスト版) と
#   標準の iwac で iwac の全断片をオブジェクト化し、
#   出力が一致することを確かめる (容量だけが違う
#   ことの確認。shirasu の hello 一致確認に相当)。
set -e
cd "$(dirname "$0")"

if [ -z "$CC" ] && [ -x "../../../kosaba-os/tools/riscv-gcc/bin/riscv-none-elf-gcc" ]; then
    CC="../../../kosaba-os/tools/riscv-gcc/bin/riscv-none-elf-gcc"
fi
CC=${CC:-riscv64-linux-gnu-gcc}
HOSTCC=${HOSTCC:-gcc}
echo "build-frag: CC=$CC HOSTCC=$HOSTCC"

WORK=frag-build
rm -rf "$WORK"
mkdir -p "$WORK"
for f in ../src/0*.c; do
    cp "$f" "$WORK/"
done

apply() {
    file=$1; from=$2; to=$3; want=$4
    n=$(grep -c -F -- "$from" "$WORK/$file" || true)
    if [ "$n" != "$want" ]; then
        echo "build-frag: $file の '$from' が $n 件 (期待 $want 件)。本体が変わったかもしれないので止まります" >&2
        exit 1
    fi
    FROM="$from" TO="$to" perl -pi -e 's/\Q$ENV{FROM}\E/$ENV{TO}/g' "$WORK/$file"
    echo "build-frag: $file: '$from' → '$to' ($n 件)"
}

# 02-parse.c: ノード表・変数表・文字列プール
apply 02-parse.c 'str_pool[16384]' 'str_pool[8192]' 1
apply 02-parse.c '[20480]' '[8192]' 5
apply 02-parse.c 'nnode >= 20480' 'nnode >= 8192' 1
apply 02-parse.c 'str_used >= 16383' 'str_used >= 8191' 1
apply 02-parse.c '[1024]' '[512]' 8
apply 02-parse.c 'nvar >= 1024' 'nvar >= 512' 1
# 03-gen.c: 命令表 (5つ+パッチ表+緩和の印) と再配置表
apply 03-gen.c '[40960]' '[16384]' 7
apply 03-gen.c 'i < 40960' 'i < 16384' 1
apply 03-gen.c '[8192]' '[2048]' 3
apply 03-gen.c 'obj_nreloc < 8192' 'obj_nreloc < 2048' 1
# 05-input.c: ソースの箱と上限のガード (3か所)、表示 (2か所)
apply 05-input.c 'src_buf[393216]' 'src_buf[131072]' 1
apply 05-input.c '393215' '131071' 3
apply 05-input.c '約384KBまで' '約128KBまで' 2
# 06-preproc.c: 展開バッファと上限のガード
apply 06-preproc.c 'exp_buf[393216]' 'exp_buf[131072]' 1
apply 06-preproc.c 'exp_len >= 393215' 'exp_len >= 131071' 1

# 断片プロファイルの iwac (ホスト版) と標準の iwac を作る
cat "$WORK"/0*.c > "$WORK/iwac_all.c"
"$HOSTCC" -std=c11 -Wall -Wextra -O2 -o "$WORK/iwac-frag" "$WORK/iwac_all.c"
cat ../src/0*.c > "$WORK/iwac_std_all.c"
"$HOSTCC" -std=c11 -Wall -Wextra -O2 -o "$WORK/iwac-std" "$WORK/iwac_std_all.c"

# 容量だけが違うことの確認: 全断片のオブジェクトが
# 標準と一致すること (断片プロファイルの本分)
mkdir -p "$WORK/objs"
for f in 01-head 02-parse 03-gen 04-encode 05-input 06-preproc 07-main; do
    "$WORK/iwac-std" --kosaba --object ../src/decls.c ../src/$f.c > "$WORK/objs/$f-std.iwo"
    "$WORK/iwac-frag" --kosaba --object ../src/decls.c ../src/$f.c > "$WORK/objs/$f-frag.iwo"
    if ! cmp -s "$WORK/objs/$f-std.iwo" "$WORK/objs/$f-frag.iwo"; then
        echo "build-frag: $f のオブジェクトが標準と違います。止まります" >&2
        exit 1
    fi
done
"$WORK/iwac-std" --kosaba --object ../kosaba/kosaba_rt.c > "$WORK/objs/rt-std.iwo"
"$WORK/iwac-frag" --kosaba --object ../kosaba/kosaba_rt.c > "$WORK/objs/rt-frag.iwo"
if ! cmp -s "$WORK/objs/rt-std.iwo" "$WORK/objs/rt-frag.iwo"; then
    echo "build-frag: rt のオブジェクトが標準と違います。止まります" >&2
    exit 1
fi
echo "build-frag: 全断片のオブジェクトが標準と一致"

# KosabaOS 用の ELF を組む (標準版と同じ舞台)
"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding \
    -nostdlib -no-pie -Wl,--build-id=none -O2 \
    -T kosaba.ld -o iwac-frag.elf "$WORK/iwac_all.c" kosaba_lib.c kosaba_crt.S
echo "build-frag: iwac-frag.elf を作りました"

# 段6 の判定: データセグメントが 1MB 未満か
memsz=$(readelf -l iwac-frag.elf | grep -A1 'LOAD.*0x0000000040200000' | tail -1 | awk '{print $2}')
if [ -n "$memsz" ]; then
    dec=$(printf '%d' "$memsz")
    echo "build-frag: データ $dec バイト"
    if [ "$dec" -ge 1048576 ]; then
        echo "build-frag: データが 1MB 以上あります。段6 の狙いに届いていません" >&2
        exit 1
    fi
    echo "build-frag: 1MB 未満に収まりました"
else
    echo "build-frag: データ量が読めませんでした (readelf の出力形式を確認)" >&2
    exit 1
fi
