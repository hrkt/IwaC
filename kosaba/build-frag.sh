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
# 実測のピーク (2026-11-19、v48、TU = decls.c + 断片):
#   ノード 9,044 (02-parse) / 命令 22,595 (03-gen)
#   再配置 2,115 (03-gen) / 変数 556 (02-parse)
#   関数 233 / 文字列 7,005 バイト
#   入力 168,564 バイト (02-parse、展開後も同量)
#
# 容量プロファイル (標準 → 断片):
#   ノード表        12288 → 9216 (v48 で 8448 から引き上げ:
#                   実測ピーク 9,044 は 02-parse)
#   命令表          24576 → 24064 (v48 で初めて下げる:
#                   実測ピーク 22,595 は 03-gen。下げた分で
#                   ノード表とソース上限の引き上げを相殺)
#   再配置表         6144 → 2304 (v46 で 2048 から引き上げ:
#                   実測ピーク 2,115 は 03-gen)
#   変数表           1280 → 576 (v46 で 512 から引き上げ:
#                   実測ピーク 556 は 02-parse)
#   文字列プール    24576 → 8192 バイト
#   ソース上限      約464KB → 168KB (v48 で 160KB から
#                   引き上げ: 実測ピーク 168,564)
#   展開バッファ    約464KB → 168KB (同上)
#   配列初期値プール 49152 → 1024 要素 (v48 で 4096 から
#                   下げる: iwac 自身のソースでは現れず、
#                   shirasu と同じ値。相殺の残り)
#   浮動小数点リテラル表 1024 → 512 個 (v46)
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
apply 02-parse.c 'str_pool[24576]' 'str_pool[8192]' 1
apply 02-parse.c '[12288]' '[9216]' 6
apply 02-parse.c 'nnode >= 12288' 'nnode >= 9216' 1
apply 02-parse.c 'str_used >= 24575' 'str_used >= 8191' 1
apply 02-parse.c '[1280]' '[576]' 11
apply 02-parse.c '[1024]' '[512]' 3
apply 02b-decl.c 'arr_nfref >= 1024' 'arr_nfref >= 512' 1
apply 02-parse.c 'fp_nbits >= 1024' 'fp_nbits >= 512' 1
# enum の列挙子の表 (v47): iwac 自身のソースは enum を
# 使わないので、断片側は小さくする (タグ表は小さいので
# そのまま)
apply 02-parse.c 'econ_names[256]' 'econ_names[32]' 1
apply 02-parse.c 'econ_name_len[256]' 'econ_name_len[32]' 1
apply 02-parse.c 'econ_val[256]' 'econ_val[32]' 1
apply 02b-decl.c 'necon >= 256' 'necon >= 32' 1
apply 02-parse.c 'nvar >= 1280' 'nvar >= 576' 1
# 配列初期値プール (v40): 表と 02b 側のガードを一緒に
apply 02-parse.c 'arr_init_pool[49152]' 'arr_init_pool[1024]' 1
apply 02b-decl.c 'arr_init_used >= 49152' 'arr_init_used >= 1024' 3
# 03-gen.c: 命令表 (5つ+パッチ表+緩和の印) と再配置表
apply 03-gen.c '[6144]' '[2304]' 3
apply 03-gen.c 'obj_nreloc < 6144' 'obj_nreloc < 2304' 1
apply 03-gen.c '[24576]' '[24064]' 7
apply 03-gen.c 'nasm >= 24576' 'nasm >= 24064' 2
apply 03-gen.c 'i < 24576' 'i < 24064' 1
# 05-input.c: ソースの箱と上限のガード (3か所)、表示 (2か所)
apply 05-input.c 'src_buf[475136]' 'src_buf[172032]' 1
apply 05-input.c '475135' '172031' 4
apply 05-input.c '約464KBまで' '約168KBまで' 3
# 06-preproc.c: 展開バッファと上限のガード
apply 06-preproc.c 'exp_buf[475136]' 'exp_buf[172032]' 1
apply 06-preproc.c 'exp_len >= 475135' 'exp_len >= 172031' 1

# 断片プロファイルの iwac (ホスト版) と標準の iwac を作る
cat "$WORK"/0*.c > "$WORK/iwac_all.c"
"$HOSTCC" -std=c11 -Wall -Wextra -O2 -o "$WORK/iwac-frag" "$WORK/iwac_all.c"
cat ../src/0*.c > "$WORK/iwac_std_all.c"
"$HOSTCC" -std=c11 -Wall -Wextra -O2 -o "$WORK/iwac-std" "$WORK/iwac_std_all.c"

# 容量だけが違うことの確認: 全断片のオブジェクトが
# 標準と一致すること (断片プロファイルの本分)
mkdir -p "$WORK/objs"
for f in 01-head 02-parse 02b-decl 02c-fpconv 03-gen 03b-genfp 04-encode 05-input 06-preproc 07-main; do
    "$WORK/iwac-std" --kosaba --elf ../src/decls.c ../src/$f.c > "$WORK/objs/$f-std.eo"
    "$WORK/iwac-frag" --kosaba --elf ../src/decls.c ../src/$f.c > "$WORK/objs/$f-frag.eo"
    if ! cmp -s "$WORK/objs/$f-std.eo" "$WORK/objs/$f-frag.eo"; then
        echo "build-frag: $f のオブジェクトが標準と違います。止まります" >&2
        exit 1
    fi
done
"$WORK/iwac-std" --kosaba --elf ../kosaba/kosaba_rt.c > "$WORK/objs/rt-std.eo"
"$WORK/iwac-frag" --kosaba --elf ../kosaba/kosaba_rt.c > "$WORK/objs/rt-frag.eo"
if ! cmp -s "$WORK/objs/rt-std.eo" "$WORK/objs/rt-frag.eo"; then
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
