#!/bin/sh
# build-frag.sh -- 断片プロファイルの nori をビルドする (段6)
#
# iwac の断片をリンクする用途に絞った、nori の
# 小容量プロファイル。kosaba/build-frag.sh (iwac 側)
# と対で、KosabaOS の 1MB 級データ領域でも
# Misoni のチェーン全体が回るようにする。
#
# 実測のピーク (2026-11-20、v49、iwac 自身のリンク):
#   最大のオブジェクト 224,232 バイト (02-parse.eo。
#     v49 で 03-gen を分割し、最大は 03-gen.eo の
#     295,112 から下がった)
#   コード合計 281,528 / 文字列合計 18,315
#   再配置 7,477 / 名前 592 (登録順の控え 2,594。
#     断片が増えた分、登録順は v48 の 2,320 から増えた)
#   名前プール 6,777 バイト / オブジェクト1つの
#   シンボル表は最大 2,085 件 (02-parse.eo)
#   配列初期値の穴埋め (afix) は 0 件
#
# 容量プロファイル (標準 → 断片):
#   名前表         4096 → 608 (配列 15 本とも)
#   登録順         4096 → 2688
#   再配置表      16384 → 7552
#   シンボル表の鏡 8192 → 2176 (esym、v46 で新設。
#     実測 2,085 に合わせた)
#   入力バッファ 約384KB → 280KB (入力オブジェクトは
#     最大 224,232 まで下がったが、相乗りする out_code
#     がコード合計 281,528 を抱えるので、こちらが上限)
#   コード置き場  288KB → 276KB (コード合計の
#     実測 281,528 に合わせた)
#   文字列置き場  128KB → 18KB
#   名前プール     64KB → 6.75KB (実測 6,777)
#   配列初期値    256KB → 128B (v49 で 512B から下げる。
#     断片プロファイルの仕事は iwac 自身のリンクで、
#     初期値は現れない)
#   afix の控え   1024 → 32 (実測 0 件)
#   edata / eabs  256 → 64 / 32 (edata はオブジェクト
#     ごとに数える初期値つき変数で、実測のピーク 63。
#     eabs は実測 0 件)
# 狙いは静的データ 1MB 未満。最後に測り、以上なら
# 失敗として止まる。
#
# 書き換えの約束は kosaba/build-frag.sh と同じ:
# 表とガード側の数字を一緒に替え、件数を数える。
#
# 使い方:
#   ./nori/build-frag.sh
#     → nori/nori-frag.elf ができる
#   作る前に、断片プロファイルの nori (ホスト版) と
#   標準の nori で iwac のオブジェクト一式をリンクし、
#   出力が一致することを確かめる。
set -e
cd "$(dirname "$0")"

HOSTCC=${HOSTCC:-gcc}
echo "build-frag (nori): HOSTCC=$HOSTCC"

WORK=frag-build
rm -rf "$WORK"
mkdir -p "$WORK"
cp nori.c "$WORK/"

apply() {
    file=$1; from=$2; to=$3; want=$4
    n=$(grep -c -F -- "$from" "$WORK/$file" || true)
    if [ "$n" != "$want" ]; then
        echo "build-frag (nori): $file の '$from' が $n 件 (期待 $want 件)。本体が変わったかもしれないので止まります" >&2
        exit 1
    fi
    FROM="$from" TO="$to" perl -pi -e 's/\Q$ENV{FROM}\E/$ENV{TO}/g' "$WORK/$file"
    echo "build-frag (nori): $file: '$from' → '$to' ($n 件)"
}

# 名前表: まず登録順 (seq) を名指しで替えてから残りを一括
apply nori.c 'seq_sym[4096]' 'seq_sym[2688]' 1
apply nori.c 'MAX_SEQ = 4096' 'MAX_SEQ = 2688' 1
apply nori.c '[4096]' '[608]' 15
apply nori.c 'MAX_SYM = 4096' 'MAX_SYM = 608' 1
# 再配置表
apply nori.c '[16384]' '[7552]' 5
apply nori.c 'MAX_REL = 16384' 'MAX_REL = 7552' 1
# オブジェクト1つのシンボル表の鏡 (esym、v46 で穴の表と分離)
apply nori.c 'esym_tag[8192]' 'esym_tag[2176]' 1
apply nori.c 'esym_sym[8192]' 'esym_sym[2176]' 1
apply nori.c 'nsym_e > 8192' 'nsym_e > 2176' 1
# 共有バッファ (in_buf と out_code の相乗り、v44) と読みのガード
apply nori.c 'share_buf[393216]' 'share_buf[286720]' 1
apply nori.c '393216 - in_len' '286720 - in_len' 1
apply nori.c 'in_len >= 393216' 'in_len >= 286720' 1
# コード置き場 (入力側・出力側) とその上限
apply nori.c 'code_pool[294912]' 'code_pool[282624]' 1
apply nori.c 'code_used >= 294912' 'code_used >= 282624' 1
apply nori.c '> 294912' '> 282624' 1
# 配列の初期値の置き場 (v40) と、その穴埋めの控え (afix)
apply nori.c 'ainit_buf[262144]' 'ainit_buf[128]' 1
apply nori.c 'MAX_AINIT = 262144' 'MAX_AINIT = 128' 1
apply nori.c 'afix_arr[1024]' 'afix_arr[32]' 1
apply nori.c 'afix_off[1024]' 'afix_off[32]' 1
apply nori.c 'afix_func[1024]' 'afix_func[32]' 1
apply nori.c 'nafix >= 1024' 'nafix >= 32' 1
# .data の受け口と絶対シンボルの控え (edata / eabs):
# iwac 自身のリンクではどちらも 0 件 (実測) なので小さく
apply nori.c 'eabs_idx[256]' 'eabs_idx[32]' 1
apply nori.c 'eabs_val[256]' 'eabs_val[32]' 1
apply nori.c 'neabs < 256' 'neabs < 32' 1
apply nori.c 'edata_off[256]' 'edata_off[64]' 1
apply nori.c 'edata_sym[256]' 'edata_sym[64]' 1
apply nori.c 'edata_val[256]' 'edata_val[64]' 1
apply nori.c 'edata_hit[256]' 'edata_hit[64]' 1
apply nori.c 'nedata >= 256' 'nedata >= 64' 1
# 文字列置き場と名前プール
apply nori.c 'str_pool[131072]' 'str_pool[18432]' 1
apply nori.c 'str_used >= 131072' 'str_used >= 18432' 1
apply nori.c 'name_pool[65536]' 'name_pool[6912]' 1
apply nori.c 'name_used >= 65536' 'name_used >= 6912' 1

# 標準の iwac (ホスト版) でオブジェクト一式を用意する
cat ../src/0*.c > "$WORK/iwac_all.c"
"$HOSTCC" -std=c11 -Wall -Wextra -O2 -o "$WORK/iwac-std" "$WORK/iwac_all.c"
mkdir -p "$WORK/objs"
OBJS=""
for f in 01-head 02-parse 02b-decl 02c-fpconv 03-gen 03-genstmt 03b-genfp 04-encode 05-input 06-preproc 07-main; do
    "$WORK/iwac-std" --kosaba --elf ../src/decls.c ../src/$f.c > "$WORK/objs/$f.eo"
    OBJS="$OBJS objs/$f.eo"
done
"$WORK/iwac-std" --kosaba --elf ../kosaba/kosaba_rt.c > "$WORK/objs/rt.eo"
OBJS="$OBJS objs/rt.eo"

# 標準 nori と断片 nori (どちらもホスト版) でリンクして比べる
"$HOSTCC" -std=c11 -Wall -Wextra -O2 -o "$WORK/nori-std" nori.c ../libdashi/libdashi.c
"$HOSTCC" -std=c11 -Wall -Wextra -O2 -o "$WORK/nori-frag" "$WORK/nori.c" ../libdashi/libdashi.c
(cd "$WORK" && ./nori-std $OBJS > linked-std.elf)
(cd "$WORK" && ./nori-frag $OBJS > linked-frag.elf)
if ! cmp -s "$WORK/linked-std.elf" "$WORK/linked-frag.elf"; then
    echo "build-frag (nori): 標準と断片でリンク結果が違います。止まります" >&2
    exit 1
fi
echo "build-frag (nori): 標準と断片でリンク結果が一致"

# KosabaOS 用の ELF を組む (iwac + rt、標準の iwac で)
"$WORK/iwac-std" --kosaba "$WORK/nori.c" ../libdashi/libdashi.c ../kosaba/kosaba_rt.c > nori-frag.elf
echo "build-frag (nori): nori-frag.elf を作りました"

# 判定: データセグメントが 1MB 未満か
memsz=$(readelf -l nori-frag.elf | grep -A1 'LOAD.*0x0000000040200000' | tail -1 | awk '{print $2}')
if [ -n "$memsz" ]; then
    dec=$(printf '%d' "$memsz")
    echo "build-frag (nori): データ $dec バイト"
    if [ "$dec" -ge 1048576 ]; then
        echo "build-frag (nori): データが 1MB 以上あります" >&2
        exit 1
    fi
    echo "build-frag (nori): 1MB 未満に収まりました"
else
    echo "build-frag (nori): データ量が読めませんでした" >&2
    exit 1
fi
