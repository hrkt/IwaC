/* iwac -- Cコンパイラのサブセット (v0)
 *
 * v0の言語は「1バイトだけ」: 引数で渡されたプログラム文字列が
 *   - 1文字 (例: 'A') なら、その文字コード
 *   - 10進数 (例: '65') なら、その数値 (0..255)
 * を意味し、その1バイトをUARTに出して止まる、RISC-V (rv64i) の
 * 生バイナリ (16バイト) を標準出力に書き出す。
 *
 * 出力バイナリは KosabaOS と同じ土俵で動く: QEMU virt で
 * -kernel に渡すと OpenSBI が 0x80200000 に置き、そこへ飛んでくる。
 *
 * 出力する4命令 (1命令4バイト, リトルエンディアン):
 *   1. lui  t0, 0x10000     -- UART (NS16550A) のベース 0x10000000 を作る
 *   2. addi t1, zero, code  -- 出したい1バイトを t1 に入れる
 *   3. sb   t1, 0(t0)       -- UART送信レジスタ (ベース+0) に書く
 *   4. wfi                  -- 止まる (割り込み待ちで駐車)
 * QEMUでの第一歩なので、送信可能を待つポーリングは入れていない
 * (「一番小さい」を優先。実機向けには後で足す小さな一歩)。
 */

#include <stdio.h>

/* 32ビットの命令を、バイナリとしてリトルエンディアンで出す */
static void emit32(unsigned int inst) {
    putchar(inst & 0xff);
    putchar((inst >> 8) & 0xff);
    putchar((inst >> 16) & 0xff);
    putchar((inst >> 24) & 0xff);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: iwac <1文字 または 0..255 の10進数>\n");
        return 1;
    }

    char *p = argv[1];
    int code;

    if (p[0] != '\0' && p[1] == '\0' && !(p[0] >= '0' && p[0] <= '9')) {
        /* 1文字のプログラム: その文字コードそのもの */
        code = (unsigned char)p[0];
    } else {
        /* 10進数のプログラム: 手で読む (将来のセルフホストでも
         * 使える範囲の書き方に寄せるため、ライブラリ変換は使わない) */
        code = 0;
        if (p[0] == '\0') {
            fprintf(stderr, "iwac: 空のプログラム\n");
            return 1;
        }
        for (int i = 0; p[i] != '\0'; i++) {
            if (!(p[i] >= '0' && p[i] <= '9')) {
                fprintf(stderr, "iwac: 数字でない文字: %c\n", p[i]);
                return 1;
            }
            code = code * 10 + (p[i] - '0');
        }
        if (code > 255) {
            fprintf(stderr, "iwac: 1バイトに収まる 0..255 にしてね\n");
            return 1;
        }
    }

    /* 1. lui t0, 0x10000
     *    0x10000 << 12 = 0x10000000 (UARTベース)。t0 は x5。
     *    形式: imm[31:12] | rd(5) | opcode 0x37 */
    emit32(0x100002b7u);

    /* 2. addi t1, zero, code
     *    t1 は x6。形式: imm[11:0] << 20 | rs1(0) | funct3(0)
     *    | rd(6) << 7 | opcode 0x13。code は 0..255 なので
     *    12ビット即値にそのまま収まる */
    emit32(((unsigned int)code << 20) | (6u << 7) | 0x13u);

    /* 3. sb t1, 0(t0)
     *    形式: rs2(6) << 20 | rs1(5) << 15 | funct3(0) | opcode 0x23 */
    emit32(0x00628023u);

    /* 4. wfi (固定の32ビットパターン) */
    emit32(0x10500073u);

    return 0;
}
