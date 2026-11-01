#!/bin/sh
# build-kosaba.sh -- iwac 自体を KosabaOS のユーザープログラム
# (iwac.elf) としてビルドする (v15)
#
# iwac.c には手を入れない。横に置く3つのファイルで舞台を変える:
#   kosaba_crt.S  入口 _start。決め打ちの argv
#                 { "iwac", "--kosaba", "hello.c" } を合成して
#                 main を呼び、最後に exit システムコールで帰る
#   kosaba_lib.c  iwac が頼る4つの外部関数 (write / open /
#                 read / close) を KosabaOS のシステムコールで
#                 実装したもの。fd=1 の出力は hello.elf という
#                 ファイルへ、fd=2 はコンソールの putc へ
#   kosaba.ld     配置 (0x40000000、OS 側 user.ld と同じ約束)
#
# 使い方:
#   ./kosaba/build-kosaba.sh   → kosaba/iwac.elf ができる
# できた ELF を OS のファイルシステムに置き、OS のファイル
# hello.c を用意してシェルから `run iwac.elf` すると、
# hello.elf が作られる。続けて `run hello.elf` で実行できる。
#
# メモリ配置は OS の約束どおり: コード 0x40000000
# (読み+実行)、データ 0x40200000 (読み書き)。OS 側で
# データ領域が新設された (KosabaOS 9416180) ため、
# iwac.elf は OS 本線でそのまま動く (統合検証済み)。
#
# ツール選択 (OS の build.sh と同じ順):
#   1. $CC 環境変数
#   2. 隣の KosabaOS プロジェクトの tools (../../../kosaba-os/tools)
#   3. システムの riscv64-linux-gnu-gcc
set -e
cd "$(dirname "$0")"

if [ -z "$CC" ] && [ -x "../../../kosaba-os/tools/riscv-gcc/bin/riscv-none-elf-gcc" ]; then
    CC="../../../kosaba-os/tools/riscv-gcc/bin/riscv-none-elf-gcc"
fi
CC=${CC:-riscv64-linux-gnu-gcc}
echo "build-kosaba: CC=$CC"

# -mcmodel=medany / -ffreestanding / -nostdlib は OS の
# ユーザープログラムと同じ決め事 (0x40000000 に置かれるため)
"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding \
    -nostdlib -no-pie -Wl,--build-id=none -O2 \
    -T kosaba.ld -o iwac.elf ../iwac.c kosaba_lib.c kosaba_crt.S
echo "build-kosaba: iwac.elf を作りました"
