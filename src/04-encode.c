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
    if (op == OP_ADDIW) {
        /* addiw rd, rs1, imm -- I 形式 (v27)。addi との
         * 違いは opcode が 0x1B で、計算が下位32ビット
         * で行われ結果が符号拡張されること。lui と組で
         * 大きな定数を作るときに使う (gen_load_const) */
        return ((unsigned int)imm << 20) | ((unsigned int)rs1 << 15) |
               ((unsigned int)rd << 7) | 0x1Bu;
    }
    if (op == OP_SLLI) {
        /* slli rd, rs1, shamt -- I 形式 (v23)。addi との
         * 違いは funct3 が 1 で、imm がシフト量 (RV64 では
         * 下位 6 ビット) なこと */
        return ((unsigned int)(imm & 63) << 20) |
               ((unsigned int)rs1 << 15) | (1u << 12) |
               ((unsigned int)rd << 7) | 0x13u;
    }
    if (op == OP_SRLI) {
        /* srli rd, rs1, shamt -- I 形式 (v27)。slli との
         * 違いは funct3 が 5 で、空いた上位が 0 で埋まる
         * こと (算術版 srai は funct7 が 0x20 になる) */
        return ((unsigned int)(imm & 63) << 20) |
               ((unsigned int)rs1 << 15) | (5u << 12) |
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
    if (op == OP_BEQ || op == OP_BNE) {
        /* beq / bne rs1, rs2, offset -- B 形式 (分岐)。
         * 命令列での飛び先は「命令番号」なので、まず
         * バイト単位の相対オフセットに直す:
         *   offset = (飛び先番号 - 自分の番号) x 4
         * 即値 (オフセット) のビットは形式の都合で
         * バラバラの場所に散らばっているのが B 形式の特徴:
         * [ imm[12] << 31 | imm[10:5] << 25 | rs2 << 20
         *   | rs1 << 15 | funct3 << 12 | imm[4:1] << 8
         *   | imm[11] << 7 | opcode 0x63 ]
         * オフセットの下1ビットは常に 0 (命令は4バイト単位)
         * なので格納されない。届く範囲は 前後 約4KB。
         * beq は funct3 が 0、bne (等しくなければ飛ぶ) は 1。
         * bne は v27 で導入: 届かない条件分岐を「反転した
         * 分岐 + jal」に伸ばすときの反転側に使う */
        int off = (imm - i) * 4;
        unsigned int f3 = (op == OP_BNE) ? 1u : 0u;
        return ((unsigned int)((off >> 12) & 1) << 31) |
               ((unsigned int)((off >> 5) & 0x3f) << 25) |
               ((unsigned int)rs2 << 20) |
               ((unsigned int)rs1 << 15) | (f3 << 12) |
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

/* KosabaOS 用 ELF のデータセグメントのファイル内オフセット
 * (v24): コードの後ろを 4096 境界に切り上げた位置。
 * p_offset と p_vaddr (0x40200000) の剰余を 4096 で揃える
 * ELF の約束のため、データはページ境界から始める */
static long elf_data_offset(void) {
    long end = 4096 + (long)code_insts * 4;
    return (end + 4095) & ~4095L;
}

/* KosabaOS 用の ELF ヘッダを書く (v14、v24 で2本化)。
 * OS の ELF ローダ (elf.c) が読む約束に合わせた最小形:
 *   ELF64 / リトルエンディアン / ET_EXEC / RISC-V
 *   プログラムヘッダ2本 (どちらも PT_LOAD):
 *     1本目 (コード): ファイルの 4096 バイト目からを
 *       仮想アドレス 0x40000000 へ置く。入口も同じ住所。
 *       権限は読み＋実行 (R+X)。中身は命令だけ
 *     2本目 (データ): 仮想アドレス 0x40200000 へ置く。
 *       権限は読み＋書き (R+W)。中身は文字列プールと、
 *       初期値つきグローバルがあるときだけ初期イメージ
 *       (配列のゼロ＋グローバルの初期値)。残り (配列・
 *       グローバル・argv の領域) は p_memsz との差分を
 *       ローダがゼロ埋めする
 *   v23 までは1本 (権限 RWE) で、静的データがコード領域
 *   (R+X) に載って書き込みが fault していた。2本に分けた
 *   ことで W^X を保ったまま書き込めるようになる */
static void write_elf_header(void) {
    unsigned long code_filesz = (unsigned long)code_insts * 4ul;
    /* データ側のファイル実体: 文字列プールは常に入る。
     * 初期イメージ (配列＋グローバル) は初期値つき
     * グローバルがあるときだけ付ける */
    unsigned long data_filesz = (unsigned long)str_used;
    if (has_var_inits)
        data_filesz += (unsigned long)arr_used +
                       (unsigned long)var_nglobal * 8ul;
    /* データ側の確保量: 文字列＋配列＋グローバル＋argv。
     * 差分はローダがゼロ埋めする = 0 で始まる */
    unsigned long data_memsz = (unsigned long)str_used +
                               (unsigned long)arr_used +
                               (unsigned long)var_nglobal * 8ul +
                               (unsigned long)argv_used;
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
    out16le(2);       /* e_phnum = 2 本 (v24) */
    out16le(0);       /* e_shentsize */
    out16le(0);       /* e_shnum */
    out16le(0);       /* e_shstrndx */
    /* プログラムヘッダ1本目 (56 バイト): コード */
    out32le(1);       /* p_type = PT_LOAD */
    out32le(5);       /* p_flags = 読み＋実行 (R+X) */
    out64le(4096);    /* p_offset */
    out64le(0x40000000ul);  /* p_vaddr */
    out64le(0x40000000ul);  /* p_paddr */
    out64le(code_filesz);   /* p_filesz */
    out64le(code_filesz);   /* p_memsz (コードは差分なし) */
    out64le(0x1000);  /* p_align */
    /* プログラムヘッダ2本目 (56 バイト): データ */
    out32le(1);       /* p_type = PT_LOAD */
    out32le(6);       /* p_flags = 読み＋書き (R+W) */
    out64le((unsigned long)elf_data_offset());  /* p_offset */
    out64le(0x40200000ul);  /* p_vaddr */
    out64le(0x40200000ul);  /* p_paddr */
    out64le(data_filesz);   /* p_filesz */
    out64le(data_memsz);    /* p_memsz (残りはゼロ埋め) */
    out64le(0x1000);  /* p_align */
    /* 実データの開始 (4096 バイト目) まで 0 で埋める。
     * ヘッダ部は 64 + 56 x 2 = 176 バイト */
    for (int i = 176; i < 4096; i++)
        out_byte(0);
}

/* グローバル変数の初期イメージを書く (v24): 静的領域の
 * うち、配列の分 (arr_used バイト、ゼロ) の後ろに、
 * グローバル変数を枠 (slot) の順に8バイトずつ並べる。
 * 初期値のあるものはその値を (文字列なら、生成で確定
 * したその住所を)、無いものは 0 を書く。ベアメタル版
 * (静的領域がコードの後ろに連続する形) と KosabaOS 版
 * (データセグメントの実体) で共用する */
static void write_global_image(void) {
    for (int i = 0; i < arr_used; i++)
        out_byte(0);
    for (int s = 0; s < var_nglobal; s++) {
        unsigned long val = 0;
        for (int v = 0; v < nvar; v++) {
            if (var_owner[v] < 0 && var_slot[v] == s) {
                if (var_init_kind[v] == 1)
                    val = (unsigned long)var_init_val[v];
                else if (var_init_kind[v] == 2)
                    val = (unsigned long)str_addr[var_init_val[v]];
                break;
            }
        }
        out64le(val);
    }
}

static void write_binary(void) {
    if (target_kosaba)
        write_elf_header();
    for (int i = 0; i < nasm; i++)
        write32(encode_inst(i));
    if (target_kosaba) {
        /* データセグメントの開始オフセットまで 0 で
         * 埋める (v24)。ここから先がデータの実体 */
        long written = 4096 + (long)nasm * 4;
        long data_off = elf_data_offset();
        for (long i = written; i < data_off; i++)
            out_byte(0);
    }
    for (int i = 0; i < str_used; i++)
        out_byte((unsigned char)str_pool[i]);
    /* 初期値つきグローバルがあるときだけ、配列＋
     * グローバルの初期イメージを実体として付ける (v24)。
     * 無いときは従来どおり付けない (ベアメタル版の出力
     * は v23 とバイト単位で同じまま) */
    if (has_var_inits)
        write_global_image();
}

