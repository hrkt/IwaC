#!/bin/sh
# misoni.sh -- 分割ビルドのドライバ (階段の段5、Misoni)
#
# 使い方: misoni/misoni.sh [マニフェスト]
#   既定のマニフェストは ./misoni.txt
#
# マニフェスト (テキスト、パスはマニフェストのある
# ディレクトリからの相対):
#   # で始まる行はコメント
#   out <名前.elf>                  成果物 (1回だけ)
#   obj <名前.eo> <file.c> [...]    1つの翻訳単位を
#                                   オブジェクトにする
# obj の並びがリンク順になる。各 obj について
# iwac --kosaba --elf を、最後に nori を起動する
# (v42 で .iwo から ELF に切替)。
# オブジェクトは misoni-tmp/ に置く。
#
# 道具の場所: 環境変数 IWAC / NORI があればそれ、
# 無ければマニフェストの場所の ./iwac と ./nori/nori
# (nori は nori/build.sh で作っておく)。
set -e
MANIFEST=${1:-misoni.txt}
cd "$(dirname "$MANIFEST")"
MANIFEST=$(basename "$MANIFEST")
[ -f "$MANIFEST" ] || { echo "misoni: マニフェスト $MANIFEST がありません" >&2; exit 1; }
IWAC=${IWAC:-./iwac}
NORI=${NORI:-./nori/nori}
[ -x "$IWAC" ] || { echo "misoni: iwac がありません (./build.sh で作る)" >&2; exit 1; }
[ -x "$NORI" ] || { echo "misoni: nori がありません (nori/build.sh で作る)" >&2; exit 1; }

mkdir -p misoni-tmp
OUT=""
OBJS=""
while read -r kind a b; do
    case "$kind" in
        \#*|"") ;;
        out)
            [ -z "$OUT" ] || { echo "misoni: out は1つだけです" >&2; exit 1; }
            OUT=$a
            ;;
        obj)
            # b には行の残り全部 (入力ファイルの並び) が入る
            files=$b
            echo "misoni: $a を訳します"
            # shellcheck disable=SC2086
            "$IWAC" --kosaba --elf $files > "misoni-tmp/$a"
            OBJS="$OBJS misoni-tmp/$a"
            ;;
        *)
            echo "misoni: 知らない行です: $kind" >&2
            exit 1
            ;;
    esac
done < "$MANIFEST"
[ -n "$OUT" ] || { echo "misoni: out がありません" >&2; exit 1; }
[ -n "$OBJS" ] || { echo "misoni: obj がありません" >&2; exit 1; }
echo "misoni: つないで $OUT を作ります"
# shellcheck disable=SC2086
"$NORI" $OBJS > "$OUT.misoni.tmp"
mv "$OUT.misoni.tmp" "$OUT"
echo "misoni: $OUT ができました"
