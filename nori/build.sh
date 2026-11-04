#!/bin/sh
# build.sh -- ホスト用の nori を作る (gcc)
# nori 本体は iwac の言語の範囲で書いてあるので、
# KosabaOS 用には iwac 自身で訳す:
#   iwac --kosaba nori/nori.c kosaba/kosaba_rt.c > nori.elf
set -e
cd "$(dirname "$0")"
cc -std=c11 -Wall -Wextra -O2 -o nori nori.c
echo "build: nori を作りました"
