#!/bin/sh
# build.sh -- iwac 本体 (ホスト用) のビルド
#
# 本体は src/ の断片 (01-head.c 〜 07-main.c と 02b-decl.c) に分かれている。
# #include を使わない約束のため、番号順に連結して1つの
# 翻訳単位 (iwac_all.c) にしてから gcc でコンパイルする。
# iwac 自身にコンパイルさせるとき (セルフホスト) も、
# 複数ファイル入力で同じ並びを渡せば同じ意味になる。
#
# 使い方: ./build.sh   → ./iwac ができる
# コンパイラは $CC で差し替えられる (既定は gcc)
set -e
cd "$(dirname "$0")"

CC=${CC:-gcc}
echo "build: CC=$CC"
cat src/0*.c > iwac_all.c
"$CC" -std=c11 -Wall -Wextra -O2 -o iwac iwac_all.c
echo "build: iwac を作りました"
