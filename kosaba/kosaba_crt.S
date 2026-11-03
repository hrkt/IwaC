# kosaba_crt.S -- KosabaOS 用ユーザープログラムの入口
#
# OS のローダは ELF の入口 (_start) から実行を始める。
# このとき、入口の約束 (OS 側 2aa149b) で a0 = argc、
# a1 = argv が渡される。v28 までは OS にその仕組みが
# 無く、ここで決め打ちの argv {"iwac", "--kosaba",
# "hello.c"} を合成していたが、約束ができたので
# レジスタをそのまま iwac の main へ渡す。
# main から戻ったら、戻り値 (= 終了ステータス) を
# 退避してから、貯めてあった出力をファイルへ
# 書き切る (kosaba_flush)。最後に exit システム
# コールで OS に帰る。ステータスは exit の a0 で渡り
# (OS 側 d28c7b8)、親の exec の戻り値になる。

    .section .text.start,"ax",@progbits
    .globl _start
_start:
    call main               # a0 = argc, a1 = argv は入口のまま
    mv s0, a0               # main の戻り値 (ステータス) を退避
    call kosaba_flush       # 出力バッファをファイルへ書き切る
    mv a0, s0               # exit の a0 = ステータス
    li a7, 3                # システムコール 3 = exit
    ecall
1:  wfi                     # exit は戻らないはずだが念のため
    j 1b
