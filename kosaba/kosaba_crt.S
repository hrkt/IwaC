# kosaba_crt.S -- KosabaOS 用ユーザープログラムの入口 (v15)
#
# OS のローダは ELF の入口 (_start) から実行を始める。
# このとき用意されているのはスタック (sp) だけ。argc / argv
# を渡す仕組みは OS にまだ無いので、ここで「決め打ちの
# argv」を合成して iwac の main を呼ぶ:
#     iwac --kosaba hello.c
# 入力は hello.c、ターゲットは KosabaOS。出力先の名前
# (hello.elf) は kosaba_lib.c 側の約束で決まる。
# main から戻ったら、貯めてあった出力をファイルへ
# 書き切ってから exit システムコールで OS に帰る。

    .section .text.start,"ax",@progbits
    .globl _start
_start:
    li a0, 3                # argc = 3
    la a1, argv_data        # argv = { "iwac", "--kosaba", "hello.c" }
    call main
    call kosaba_flush       # 出力バッファをファイルへ書き切る
    li a7, 3                # システムコール 3 = exit
    ecall
1:  wfi                     # exit は戻らないはずだが念のため
    j 1b

    .section .rodata
argv_data:
    .dword str_argv0
    .dword str_argv1
    .dword str_argv2
str_argv0: .asciz "iwac"
str_argv1: .asciz "--kosaba"
str_argv2: .asciz "hello.c"
