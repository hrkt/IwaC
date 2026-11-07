#!/bin/sh
# build.sh -- ホスト用の Asari を作る (gcc)
# Asari 本体は iwac の言語の範囲で書いてあるので、
# KosabaOS 用には iwac 自身で訳す:
#   iwac --kosaba asari/asari.c libdashi/libdashi.c kosaba/kosaba_rt.c > asari.elf
set -e
cd "$(dirname "$0")"
cc -std=c11 -Wall -Wextra -O2 -o asari asari.c ../libdashi/libdashi.c
echo "build: asari を作りました"
