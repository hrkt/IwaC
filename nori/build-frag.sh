#!/bin/sh
# build-frag.sh -- 断片プロファイルの nori をビルドする (段6)
#
# iwac の断片をリンクする用途に絞った、nori の
# 小容量プロファイル。kosaba/build-frag.sh (iwac 側)
# と対で、KosabaOS の 1MB 級データ領域でも
# Misoni のチェーン全体が回るようにする。
#
# 実測のピーク (2026-11-06、iwac 自身のリンク):
#   最大のオブジェクト 144,706 バイト (02-parse.iwo)
#   コード合計 120,556 バイト / 文字列合計 7,688
#   再配置 約 3,164 / 名前 約 510 (登録順の控え 約 950)
#
# 容量プロファイル (標準 → 断片):
#   名前表         4096 → 1024 (配列 15 本とも。
#     v40 で配列初期値の控え 2 本が増えた)
#   登録順         4096 → 2048
#   再配置表      16384 → 5120 (v38 で増量: iwac の
#     再配置の実測合計が 4,339 になり 4096 を超えた)
#   入力バッファ 約384KB → 180KB (v42 で増量:
#     ELF 形式の最大オブジェクト 173,544 バイト
#     (03-gen.eo) が入る大きさ)
#   コード置き場  256KB → 184KB (入力側・出力側とも。
#     iwac のコード合計の実測 184,092 に合わせた)
#   文字列置き場  128KB → 16KB
#   名前プール     64KB → 16KB
#   配列初期値    256KB → 4KB (v40。断片プロファイルの
#     仕事は iwac 自身のリンクで、初期値は現れない)
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
apply nori.c 'seq_sym[4096]' 'seq_sym[2048]' 1
apply nori.c 'MAX_SEQ = 4096' 'MAX_SEQ = 2048' 1
apply nori.c '[4096]' '[1024]' 15
apply nori.c 'MAX_SYM = 4096' 'MAX_SYM = 1024' 1
# 再配置表
apply nori.c '[16384]' '[5120]' 7
apply nori.c 'MAX_REL = 16384' 'MAX_REL = 5120' 1
# 入力バッファと読みのガード
apply nori.c 'in_buf[393216]' 'in_buf[184320]' 1
apply nori.c '393216 - in_len' '184320 - in_len' 1
apply nori.c 'in_len >= 393216' 'in_len >= 184320' 1
# コード置き場 (入力側・出力側) とその上限
apply nori.c 'code_pool[262144]' 'code_pool[188416]' 1
apply nori.c 'code_used >= 262144' 'code_used >= 188416' 1
apply nori.c 'out_code[262144]' 'out_code[188416]' 1
apply nori.c '> 262144' '> 188416' 1
# 配列の初期値の置き場 (v40)
apply nori.c 'ainit_buf[262144]' 'ainit_buf[4096]' 1
apply nori.c 'MAX_AINIT = 262144' 'MAX_AINIT = 4096' 1
# 文字列置き場と名前プール
apply nori.c 'str_pool[131072]' 'str_pool[16384]' 1
apply nori.c 'str_used >= 131072' 'str_used >= 16384' 1
apply nori.c 'name_pool[65536]' 'name_pool[16384]' 1
apply nori.c 'name_used >= 65536' 'name_used >= 16384' 1

# 標準の iwac (ホスト版) でオブジェクト一式を用意する
cat ../src/0*.c > "$WORK/iwac_all.c"
"$HOSTCC" -std=c11 -Wall -Wextra -O2 -o "$WORK/iwac-std" "$WORK/iwac_all.c"
mkdir -p "$WORK/objs"
OBJS=""
for f in 01-head 02-parse 02b-decl 03-gen 04-encode 05-input 06-preproc 07-main; do
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
