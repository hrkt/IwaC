/* ================================================================
 * 第3層 encode: 命令列 → 機械語 → ファイル出力
 *
 * ここだけが「命令のビット列の形」を知っている層。
 * RISC-V の命令は4バイトで、フィールド (区画) の並びは
 * 命令の種類ごとに決まっている。各命令の式のコメントに、
 * どの区画に何が入るかを書いておく。
 * ================================================================ */

/* 命令列の i 番目の命令を、32ビットの機械語に変換する */
static unsigned int encode_inst(int i) {
    int op = asm_op[i];
    int rd = asm_rd[i];
    int rs1 = asm_rs1[i];
    int rs2 = asm_rs2[i];
    int imm = asm_imm[i];

    if (op == OP_LUI) {
        /* lui rd, imm -- 上の20ビットに imm を置く U 形式
         * [ imm << 12 | rd << 7 | opcode 0x37 ]
         * lui t0, 0x10000 なら 0x10000000 が t0 に入る */
        return ((unsigned int)imm << 12) | ((unsigned int)rd << 7) | 0x37u;
    }
    if (op == OP_ADDI) {
        /* addi rd, rs1, imm -- I 形式
         * [ imm << 20 | rs1 << 15 | funct3(0) << 12
         *   | rd << 7 | opcode 0x13 ] */
        return ((unsigned int)imm << 20) | ((unsigned int)rs1 << 15) |
               ((unsigned int)rd << 7) | 0x13u;
    }
    if (op == OP_ADD) {
        /* add rd, rs1, rs2 -- R 形式 (レジスタ同士)
         * [ funct7(0) << 25 | rs2 << 20 | rs1 << 15
         *   | funct3(0) << 12 | rd << 7 | opcode 0x33 ] */
        return ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_SUB) {
        /* sub rd, rs1, rs2 -- add との違いは funct7 が 0x20 */
        return (0x20u << 25) | ((unsigned int)rs2 << 20) |
               ((unsigned int)rs1 << 15) | ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_SB) {
        /* sb rs2, imm(rs1) -- S 形式 (メモリへ書く)
         * 即値は上下に分かれて入るのが S 形式の特徴:
         * [ imm[11:5] << 25 | rs2 << 20 | rs1 << 15
         *   | funct3(0) << 12 | imm[4:0] << 7 | opcode 0x23 ]
         * 今の使い方では imm は常に 0 なので、即値の区画は
         * 全部 0 になる */
        return ((unsigned int)(imm >> 5) << 25) |
               ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               ((unsigned int)(imm & 31) << 7) | 0x23u;
    }
    if (op == OP_AUIPC) {
        /* auipc rd, imm -- U 形式 (lui と同じ並び、opcode だけ
         * 違う: 0x17)。rd = 実行中の命令の住所 + (imm << 12) */
        return ((unsigned int)imm << 12) | ((unsigned int)rd << 7) | 0x17u;
    }
    if (op == OP_LBU) {
        /* lbu rd, imm(rs1) -- I 形式 (addi と同じ並び)。
         * addi との違いは opcode が 0x03 (読み込み系) で、
         * funct3 が 4 (符号なしバイト読み) なこと */
        return ((unsigned int)imm << 20) | ((unsigned int)rs1 << 15) |
               (4u << 12) | ((unsigned int)rd << 7) | 0x03u;
    }
    if (op == OP_SD) {
        /* sd rs2, imm(rs1) -- S 形式 (sb と同じ並び)。
         * sb との違いは funct3 が 3 (8バイト幅) なこと */
        return ((unsigned int)(imm >> 5) << 25) |
               ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               (3u << 12) | ((unsigned int)(imm & 31) << 7) | 0x23u;
    }
    if (op == OP_LD) {
        /* ld rd, imm(rs1) -- I 形式。lbu との違いは funct3 が 3 */
        return ((unsigned int)imm << 20) | ((unsigned int)rs1 << 15) |
               (3u << 12) | ((unsigned int)rd << 7) | 0x03u;
    }
    if (op == OP_JALR) {
        /* jalr rd, rs1, imm -- I 形式 (opcode 0x67)。
         * rd=0, rs1=ra, imm=0 で「呼び出し側へ戻る」になる */
        return ((unsigned int)imm << 20) | ((unsigned int)rs1 << 15) |
               ((unsigned int)rd << 7) | 0x67u;
    }
    if (op == OP_MUL) {
        /* mul rd, rs1, rs2 -- R 形式。M 拡張の命令は
         * funct7 が 1。funct3 は 0 */
        return (1u << 25) | ((unsigned int)rs2 << 20) |
               ((unsigned int)rs1 << 15) |
               ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_DIV) {
        /* div rd, rs1, rs2 -- R 形式、funct7 が 1 で
         * funct3 が 4。除数が 0 のときの結果は -1
         * (RISC-V の決まり。例外にはならない) */
        return (1u << 25) | ((unsigned int)rs2 << 20) |
               ((unsigned int)rs1 << 15) | (4u << 12) |
               ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_AND) {
        /* and rd, rs1, rs2 -- R 形式。add との違いは funct3 が 7 */
        return ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               (7u << 12) | ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_SLL) {
        /* sll rd, rs1, rs2 -- R 形式 (v19)。funct3 が 1。
         * シフト量は rs2 の下6ビット (RV64) */
        return ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               (1u << 12) | ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_SRA) {
        /* sra rd, rs1, rs2 -- R 形式 (v19)。funct7 が 0x20 で
         * funct3 が 5。空いた上のビットには符号ビットが
         * 入る (算術シフト)。論理シフト srl は funct7 が 0 */
        return (0x20u << 25) | ((unsigned int)rs2 << 20) |
               ((unsigned int)rs1 << 15) | (5u << 12) |
               ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_OR) {
        /* or rd, rs1, rs2 -- R 形式 (v19)。funct3 が 6 */
        return ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               (6u << 12) | ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_XOR) {
        /* xor rd, rs1, rs2 -- R 形式 (v19)。funct3 が 4 */
        return ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               (4u << 12) | ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_REM) {
        /* rem rd, rs1, rs2 -- R 形式 (v19、M 拡張)。
         * funct7 が 1 で funct3 が 6。除数が 0 のときは
         * 被除数がそのまま残る (div と対の決まり) */
        return (1u << 25) | ((unsigned int)rs2 << 20) |
               ((unsigned int)rs1 << 15) | (6u << 12) |
               ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_SLT) {
        /* slt rd, rs1, rs2 -- R 形式。sltu との違いは
         * funct3 が 2 (符号付きで比べる) */
        return ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               (2u << 12) | ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_XORI) {
        /* xori rd, rs1, imm -- I 形式。addi との違いは funct3 が 4 */
        return ((unsigned int)imm << 20) | ((unsigned int)rs1 << 15) |
               (4u << 12) | ((unsigned int)rd << 7) | 0x13u;
    }
    if (op == OP_SLTU) {
        /* sltu rd, rs1, rs2 -- R 形式。add との違いは funct3 が 3 */
        return ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               (3u << 12) | ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_SLTIU) {
        /* sltiu rd, rs1, imm -- I 形式。addi との違いは funct3 が 3 */
        return ((unsigned int)imm << 20) | ((unsigned int)rs1 << 15) |
               (3u << 12) | ((unsigned int)rd << 7) | 0x13u;
    }
    if (op == OP_BEQ) {
        /* beq rs1, rs2, offset -- B 形式 (分岐)。
         * 命令列での飛び先は「命令番号」なので、まず
         * バイト単位の相対オフセットに直す:
         *   offset = (飛び先番号 - 自分の番号) x 4
         * 即値 (オフセット) のビットは形式の都合で
         * バラバラの場所に散らばっているのが B 形式の特徴:
         * [ imm[12] << 31 | imm[10:5] << 25 | rs2 << 20
         *   | rs1 << 15 | funct3(0) << 12 | imm[4:1] << 8
         *   | imm[11] << 7 | opcode 0x63 ]
         * オフセットの下1ビットは常に 0 (命令は4バイト単位)
         * なので格納されない。届く範囲は 前後 約4KB */
        int off = (imm - i) * 4;
        return ((unsigned int)((off >> 12) & 1) << 31) |
               ((unsigned int)((off >> 5) & 0x3f) << 25) |
               ((unsigned int)rs2 << 20) |
               ((unsigned int)rs1 << 15) |
               ((unsigned int)((off >> 1) & 0xf) << 8) |
               ((unsigned int)((off >> 11) & 1) << 7) | 0x63u;
    }
    if (op == OP_JAL) {
        /* jal rd, offset -- J 形式 (ジャンプ)。
         * 相対オフセットへの直し方は beq と同じ。ビットの
         * 散らばり方は J 形式の流儀:
         * [ imm[20] << 31 | imm[10:1] << 21 | imm[11] << 20
         *   | imm[19:12] << 12 | rd << 7 | opcode 0x6f ]
         * 届く範囲は 前後 約1MB。rd が 0 のときは「戻り先を
         * 保存しない単なるジャンプ」(アセンブリの j) になる */
        int off = (imm - i) * 4;
        return ((unsigned int)((off >> 20) & 1) << 31) |
               ((unsigned int)((off >> 1) & 0x3ff) << 21) |
               ((unsigned int)((off >> 11) & 1) << 20) |
               ((unsigned int)((off >> 12) & 0xff) << 12) |
               ((unsigned int)rd << 7) | 0x6fu;
    }
    if (op == OP_ECALL) {
        /* ecall: 引数を取らない固定パターン (v14)。
         * wfi と同じ末尾 0x73 の系統で、上位のビットが違う */
        return 0x00000073u;
    }
    /* OP_WFI: 引数を取らない固定パターン */
    return 0x10500073u;
}

/* 32ビットの機械語を、リトルエンディアンで標準出力に書く
 * (小さい方のバイトから順に書くのがリトルエンディアン) */
static void write32(unsigned int inst) {
    out_byte(inst & 0xff);
    out_byte((inst >> 8) & 0xff);
    out_byte((inst >> 16) & 0xff);
    out_byte((inst >> 24) & 0xff);
}

/* 命令列を全部変換して書き出し、続けて文字列のデータ
 * (str_pool の中身そのもの) を書き出す。これが最終成果物。
 * バイナリの姿: [コード (命令 x 4バイト)] [文字列の並び]
 * 実行が wfi で止まるため、後ろの文字列が命令として
 * 実行されることはない。文字列の住所は第2層がこの並びに
 * 合わせて決めてある */
/* ELF の数値をリトルエンディアンで書く道具 (v14) */
static void out16le(unsigned int v) {
    out_byte(v & 0xff);
    out_byte((v >> 8) & 0xff);
}
static void out32le(unsigned int v) {
    out16le(v & 0xffff);
    out16le((v >> 16) & 0xffff);
}
static void out64le(unsigned long v) {
    out32le((unsigned int)(v & 0xffffffffu));
    out32le((unsigned int)(v >> 32));
}

/* KosabaOS 用の ELF ヘッダを書く (v14)。OS の ELF ローダ
 * (elf.c) が読む約束に合わせた最小形:
 *   ELF64 / リトルエンディアン / ET_EXEC / RISC-V
 *   プログラムヘッダ1本 (PT_LOAD): ファイルの 4096 バイト目
 *   からを仮想アドレス 0x40000000 へ置く。入口も同じ住所。
 *   p_filesz はコード＋文字列の長さ、p_memsz はそれに配列の
 *   領域を足した長さ (ローダが差分をゼロ埋めする = 配列は
 *   0 で始まる)。4096 までの間は 0 で埋める (p_offset と
 *   p_vaddr の 4096 での剰余を揃える、ELF の約束) */
static void write_elf_header(void) {
    unsigned long filesz = (unsigned long)code_insts * 4ul +
                           (unsigned long)str_used;
    unsigned long memsz = filesz + (unsigned long)arr_used;
    /* ELF ヘッダ (64 バイト) */
    out_byte(0x7f);
    out_byte('E');
    out_byte('L');
    out_byte('F');
    out_byte(2);  /* 64 ビット */
    out_byte(1);  /* リトルエンディアン */
    out_byte(1);  /* バージョン */
    out_byte(0);  /* OS ABI: 指定なし */
    for (int i = 0; i < 8; i++)
        out_byte(0);  /* 残りの識別バイト */
    out16le(2);       /* e_type = ET_EXEC */
    out16le(243);     /* e_machine = RISC-V */
    out32le(1);       /* e_version */
    out64le(0x40000000ul);  /* e_entry = 先頭 */
    out64le(64);      /* e_phoff = ヘッダの直後 */
    out64le(0);       /* e_shoff = セクションヘッダ無し */
    out32le(0);       /* e_flags */
    out16le(64);      /* e_ehsize */
    out16le(56);      /* e_phentsize */
    out16le(1);       /* e_phnum = 1 本 */
    out16le(0);       /* e_shentsize */
    out16le(0);       /* e_shnum */
    out16le(0);       /* e_shstrndx */
    /* プログラムヘッダ (56 バイト) */
    out32le(1);       /* p_type = PT_LOAD */
    out32le(7);       /* p_flags = 読み・書き・実行 */
    out64le(4096);    /* p_offset */
    out64le(0x40000000ul);  /* p_vaddr */
    out64le(0x40000000ul);  /* p_paddr */
    out64le(filesz);  /* p_filesz */
    out64le(memsz);   /* p_memsz (配列の分はゼロ埋め) */
    out64le(0x1000);  /* p_align */
    /* 実データの開始 (4096 バイト目) まで 0 で埋める */
    for (int i = 120; i < 4096; i++)
        out_byte(0);
}

static void write_binary(void) {
    if (target_kosaba)
        write_elf_header();
    for (int i = 0; i < nasm; i++)
        write32(encode_inst(i));
    for (int i = 0; i < str_used; i++)
        out_byte((unsigned char)str_pool[i]);
}

