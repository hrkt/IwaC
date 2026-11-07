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
    if (op == OP_LH || op == OP_LHU || op == OP_LW ||
        op == OP_LWU) {
        /* lh / lhu / lw / lwu rd, imm(rs1) -- I 形式 (v35)。
         * lbu と同じ並びで、funct3 が幅と符号を決める:
         * lh=1 (2バイト符号拡張)、lw=2 (4バイト符号
         * 拡張)、lhu=5 (2バイトゼロ拡張)、lwu=6
         * (4バイトゼロ拡張) */
        unsigned int f3 = op == OP_LH ? 1u :
                          op == OP_LW ? 2u :
                          op == OP_LHU ? 5u : 6u;
        return ((unsigned int)imm << 20) | ((unsigned int)rs1 << 15) |
               (f3 << 12) | ((unsigned int)rd << 7) | 0x03u;
    }
    if (op == OP_SH || op == OP_SW) {
        /* sh / sw rs2, imm(rs1) -- S 形式 (v35)。sb と
         * 同じ並びで、funct3 が sh=1、sw=2 */
        unsigned int f3 = op == OP_SH ? 1u : 2u;
        return ((unsigned int)(imm >> 5) << 25) |
               ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               (f3 << 12) | ((unsigned int)(imm & 31) << 7) | 0x23u;
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
    if (op == OP_DIVU || op == OP_REMU) {
        /* divu / remu rd, rs1, rs2 -- R 形式 (v35、
         * M 拡張)。funct7 が 1、funct3 は divu=5、
         * remu=7。除数が 0 のときは div 系と同じ
         * 決まり (divu は全ビット 1、remu は被除数) */
        unsigned int f3 = op == OP_DIVU ? 5u : 7u;
        return (1u << 25) | ((unsigned int)rs2 << 20) |
               ((unsigned int)rs1 << 15) | (f3 << 12) |
               ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_SRL) {
        /* srl rd, rs1, rs2 -- R 形式 (v35)。sra との
         * 違いは funct7 が 0 (空いた上位は 0 で埋まる) */
        return ((unsigned int)rs2 << 20) |
               ((unsigned int)rs1 << 15) | (5u << 12) |
               ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_SRAI) {
        /* srai rd, rs1, shamt -- I 形式 (v35)。srli と
         * の違いは即値の上位 (funct7) が 0x20 で、符号
         * ビットが空いた上位に入る */
        return ((unsigned int)(0x400 | (imm & 63)) << 20) |
               ((unsigned int)rs1 << 15) | (5u << 12) |
               ((unsigned int)rd << 7) | 0x13u;
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

/* KosabaOS 用 ELF のデータのファイル内オフセット (v24):
 * コードの後ろを 4096 境界に切り上げた位置 (p_offset と
 * p_vaddr の剰余を揃える ELF の約束のため) */
static long elf_data_offset(void) {
    long end = 4096 + (long)code_insts * 4;
    return (end + 4095) & ~4095L;
}

/* KosabaOS 用の ELF ヘッダを書く (v14、v24 で2本化、v28 で3本化)。
 * OS の ELF ローダ (elf.c) が読む約束に合わせた最小形:
 *   ELF64 / リトルエンディアン / ET_EXEC / RISC-V
 *   プログラムヘッダ3本 (どれも PT_LOAD):
 *     1本目 (コード): ファイルの 4096 バイト目からを
 *       仮想アドレス 0x40000000 へ。入口も同じ住所。
 *       権限 R+X、中身は命令だけ
 *     2本目 (文字列と配列): 0x40200000 へ、権限 R+W。
 *       実体は文字列プールだけ。配列の領域は p_memsz
 *       との差分をローダがゼロ埋めする (v28。中身は
 *       ゼロしか無いのでファイルに含める意味が無い)
 *     3本目 (グローバル): 配列の直後へ。実体は
 *       グローバルの初期イメージ (初期値つきのとき
 *       だけ)。ファイル内では文字列プールの直後に置く
 *       (v29 で argv の領域は廃止)
 *   v23 までは1本 (権限 RWE) で、静的データがコード領域
 *   (R+X) に載って書き込みが fault していた。2本に分けた
 *   ことで W^X を保ったまま書き込めるようになる */
static void write_elf_header(void) {
    unsigned long code_filesz = (unsigned long)code_insts * 4ul;
    /* データ側の実体と確保量 (v28): 2本目は文字列の
     * 実体 + 配列の確保分、3本目はグローバルの実体と
     * 確保分。差分はローダがゼロ埋めする */
    unsigned long data_filesz = (unsigned long)str_used;
    unsigned long data_memsz = (unsigned long)str_used +
                               (unsigned long)arr_used;
    unsigned long gbl_filesz = 0;
    if (has_var_inits)
        gbl_filesz = (unsigned long)var_nglobal * 8ul;
    unsigned long gbl_memsz = (unsigned long)var_nglobal * 8ul;
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
    out16le(3);       /* e_phnum = 3 本 (v28) */
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
    /* プログラムヘッダ2本目 (56 バイト): 文字列と配列 */
    out32le(1);       /* p_type = PT_LOAD */
    out32le(6);       /* p_flags = 読み＋書き (R+W) */
    out64le((unsigned long)elf_data_offset());  /* p_offset */
    out64le(0x40200000ul);  /* p_vaddr */
    out64le(0x40200000ul);  /* p_paddr */
    out64le(data_filesz);   /* p_filesz */
    out64le(data_memsz);    /* p_memsz (配列分はゼロ埋め) */
    out64le(0x1000);  /* p_align */
    /* プログラムヘッダ3本目 (56 バイト): グローバルと argv */
    out32le(1);       /* p_type = PT_LOAD */
    out32le(6);       /* p_flags = 読み＋書き (R+W) */
    out64le((unsigned long)elf_data_offset() +
            (unsigned long)str_used);  /* p_offset (文字列の直後) */
    out64le(0x40200000ul + data_memsz);  /* p_vaddr (配列の直後) */
    out64le(0x40200000ul + data_memsz);  /* p_paddr */
    out64le(gbl_filesz);   /* p_filesz */
    out64le(gbl_memsz);    /* p_memsz (argv 分はゼロ埋め) */
    out64le(1);       /* p_align (開始位置はページ境界とは限らない) */
    /* 実データの開始 (4096 バイト目) まで 0 で埋める。
     * ヘッダ部は 64 + 56 x 3 = 232 バイト */
    for (int i = 232; i < 4096; i++)
        out_byte(0);
}

/* グローバル変数の枠のイメージを書く (v24、v28 で
 * 配列分と分離): 枠の順に8バイトずつ。初期値のある
 * ものはその値を、無いものは 0 を書く */
static void write_var_image(void) {
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

/* ベアメタル版の初期イメージ: 配列のゼロの後ろに
 * グローバルの枠を並べる (バイナリそのものがメモリ
 * イメージなので、ゼロも含めて全部書く) */
static void write_global_image(void) {
    for (int i = 0; i < arr_used; i++)
        out_byte(0);
    write_var_image();
}


/* ================================================================
 * オブジェクト出力 (v31、分割コンパイルの段2)
 * 設計は doc/object-format.md。1ファイル分の
 * コードとデータの材料を、名前と穴のメモ付きの
 * テキストとして標準出力へ書く。最終形 (配置と
 * 住所) は知らないので、穴は即値 0 のまま。
 * 埋めるのはリンカ (段3 の nori) の仕事
 * ================================================================ */

/* 10進数を書く (負もあり) */
static void obj_num(long v) {
    char buf[24];
    int n = 0;
    if (v < 0) {
        out_byte('-');
        v = -v;
    }
    if (v == 0)
        buf[n++] = '0';
    while (v > 0) {
        buf[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    for (int i = n - 1; i >= 0; i--)
        out_byte(buf[i]);
}

static void obj_str(char *s) {
    print_str(1, s);
}

/* バイト列を16進数で書く (1行に32バイトまで) */
static void obj_hex(unsigned char *p, long n) {
    char *hex = "0123456789abcdef";
    for (long i = 0; i < n; i++) {
        if (i > 0 && i % 32 == 0)
            out_byte('\n');
        out_byte(hex[(p[i] >> 4) & 15]);
        out_byte(hex[p[i] & 15]);
    }
    if (n > 0)
        out_byte('\n');
}

static void write_object(void) {
    /* 入口の断片を持つか: main か、トップレベルの
     * 文か、最後の式があるファイルが持つ。持たない
     * ファイル (ライブラリだけ) の出口命令は
     * オブジェクトには含めない */
    int has_entry = (prog_main >= 0 || prog_stmts != -1 ||
                     prog_has_final);
    int start = has_entry ? 0 : obj_prefix_insts;

    obj_str("iwacobj 1\n");
    obj_str("target kosaba\n");
    if (has_entry) {
        obj_str("entry ");
        obj_num((long)obj_prefix_insts * 4);
        obj_str("\n");
    }
    /* コード (穴は encode_inst が印のまま符号化
     * するので、呼び出しと住所作りは即値 0 の形に
     * なる。呼び出しの印は下の call のメモで読む) */
    obj_str("code ");
    obj_num((long)(nasm - start) * 4);
    obj_str("\n");
    {
        /* 1命令ずつ符号化して16進に */
        unsigned char wb[4];
        long col = 0;
        char *hex = "0123456789abcdef";
        for (int i = start; i < nasm; i++) {
            /* 呼び出しの jal は、飛び先の代わりに印
             * (負数) が入っている。符号化すると印の
             * 値でオフセットの桁が埋まるが、リンカは
             * その桁を捨てて自分で書き直す (nori は
             * 下12ビットだけ残す)。そこで、穴の形は
             * 「オフセット 0」で揃える (Asari の出力と
             * バイト一致させるための正規化、v38) */
            int saved_imm = asm_imm[i];
            if (asm_op[i] == OP_JAL && saved_imm < 0)
                asm_imm[i] = i;
            unsigned int w = encode_inst(i);
            asm_imm[i] = saved_imm;
            wb[0] = (unsigned char)(w & 0xff);
            wb[1] = (unsigned char)((w >> 8) & 0xff);
            wb[2] = (unsigned char)((w >> 16) & 0xff);
            wb[3] = (unsigned char)((w >> 24) & 0xff);
            for (int j = 0; j < 4; j++) {
                if (col > 0 && col % 32 == 0)
                    out_byte('\n');
                out_byte(hex[(wb[j] >> 4) & 15]);
                out_byte(hex[wb[j] & 15]);
                col++;
            }
        }
        if (col > 0)
            out_byte('\n');
    }
    /* 文字列の実体 */
    obj_str("strings ");
    obj_num(str_used);
    obj_str("\n");
    obj_hex((unsigned char *)str_pool, str_used);
    /* グローバル配列の合計 (実体はゼロだけ) */
    obj_str("arrays ");
    obj_num(arr_used);
    obj_str("\n");
    /* 関数 (名簿の順)。定義は func (位置と大きさ)、
     * 宣言だけのものは decl。この並び順が、リンク時
     * の配置順 (登録順) の材料になる: 一括では本体は
     * 登録順に並ぶので、宣言が先にあった関数の本体は
     * 宣言のあった場所に置かれる */
    for (int f = 0; f < nfunc; f++) {
        if (func_has_body[f]) {
            /* 大きさは次の定義までの距離 (最後は末尾まで) */
            long size = (long)(nasm - func_entry[f]);
            for (int g = f + 1; g < nfunc; g++) {
                if (func_has_body[g]) {
                    size = (long)(func_entry[g] - func_entry[f]);
                    break;
                }
            }
            obj_str("func ");
            obj_str(func_names[f]);
            obj_str(" ");
            obj_num((long)(func_entry[f] - start) * 4);
            obj_str(" ");
            obj_num(size * 4);
            obj_str("\n");
        } else {
            obj_str("decl ");
            obj_str(func_names[f]);
            obj_str("\n");
        }
    }
    /* グローバルの定義 (枠の順)。初期値があれば
     * 本定義 (var)、無ければ仮定義 (varc)。占める
     * 枠の数も書く (v37、構造体は複数枠) */
    for (int s = 0; s < var_nglobal; s++) {
        for (int v = 0; v < nvar; v++) {
            if (var_owner[v] >= 0 || var_slot[v] != s)
                continue;
            /* 枠の数 = 次のグローバルまでの距離 */
            int ns = var_nglobal - s;
            for (int w = 0; w < nvar; w++) {
                if (var_owner[w] >= 0 || var_slot[w] <= s)
                    continue;
                if (var_slot[w] - s < ns)
                    ns = var_slot[w] - s;
            }
            obj_str(var_init_kind[v] != 0 ? "var " : "varc ");
            obj_str(var_names[v]);
            obj_str(" ");
            obj_num(ns);
            obj_str("\n");
            break;
        }
    }
    /* グローバル配列の定義 (登録順) */
    for (int a = 0; a < narr; a++) {
        if (arr_owner[a] >= 0)
            continue;
        /* 大きさは次の配列までの距離 (最後は合計まで) */
        long bytes = (long)arr_used - (long)arr_off[a];
        for (int b = 0; b < narr; b++) {
            if (arr_owner[b] >= 0 || b == a)
                continue;
            if (arr_off[b] > arr_off[a] &&
                (long)arr_off[b] - (long)arr_off[a] < bytes)
                bytes = (long)arr_off[b] - (long)arr_off[a];
        }
        obj_str("arr ");
        obj_str(arr_names[a]);
        obj_str(" ");
        obj_num(arr_off[a]);
        obj_str(" ");
        obj_num(bytes);
        obj_str("\n");
    }
    /* 初期値 (枠の順)。文字列は iptr として出す */
    for (int s = 0; s < var_nglobal; s++) {
        for (int v = 0; v < nvar; v++) {
            if (var_owner[v] >= 0 || var_slot[v] != s)
                continue;
            if (var_init_kind[v] == 1) {
                obj_str("init ");
                obj_str(var_names[v]);
                obj_str(" ");
                obj_num(var_init_val[v]);
                obj_str("\n");
            } else if (var_init_kind[v] == 2) {
                obj_str("iptr ");
                obj_str(var_names[v]);
                obj_str(" str ");
                obj_num(str_off[var_init_val[v]]);
                obj_str("\n");
            }
            break;
        }
    }
    /* 穴: 呼び出し (命令の印を読む) */
    for (int i = start; i < nasm; i++) {
        if (asm_op[i] != OP_JAL || asm_imm[i] >= 0)
            continue;
        if (i == obj_flush_site || asm_imm[i] == -1000003) {
            obj_str("callopt ");
            obj_num((long)(i - start) * 4);
            obj_str(" kosaba_flush\n");
        } else {
            obj_str("call ");
            obj_num((long)(i - start) * 4);
            obj_str(" ");
            obj_str(func_names[-asm_imm[i] - 1]);
            obj_str("\n");
        }
    }
    /* 穴: 住所作り (gen の控えの順) */
    for (int r = 0; r < obj_nreloc; r++) {
        long off = (long)(obj_reloc_site[r] - start) * 4;
        if (obj_reloc_kind[r] == RLOC_STR) {
            obj_str("addrstr ");
            obj_num(off);
            obj_str(" ");
            obj_num(str_off[obj_reloc_idx[r]]);
            obj_str("\n");
        } else if (obj_reloc_kind[r] == RLOC_VAR) {
            obj_str("addr ");
            obj_num(off);
            obj_str(" ");
            obj_str(var_names[obj_reloc_idx[r]]);
            obj_str(" 0\n");
        } else {
            obj_str("addr ");
            obj_num(off);
            obj_str(" ");
            obj_str(arr_names[obj_reloc_idx[r]]);
            obj_str(" 0\n");
        }
    }
    for (int r = 0; r < obj_nabs; r++) {
        obj_str("addabs ");
        obj_num((long)(obj_abs_site[r] - start) * 4);
        obj_str(" ");
        obj_num((long)(unsigned int)obj_abs_val[r]);
        obj_str("\n");
    }
    /* 外に頼る名前 (宣言だけの関数のうち呼ばれたもの) */
    for (int f = 0; f < nfunc; f++) {
        if (func_called[f] && !func_has_body[f]) {
            obj_str("undef ");
            obj_str(func_names[f]);
            obj_str("\n");
        }
    }
    obj_str("end\n");
}

/* ================================================================
 * Asari 形式の出力 (v38、設計は doc/asari.md)
 * 符号化する前の命令列を、付属情報と一緒にテキストで
 * 出す。符号化は独立プログラム Asari の仕事になる。
 * 材料は write_object と同じで、違うのは「コードの
 * バイト列」の代わりに「命令列そのもの」を書くことと、
 * 穴の位置をバイト数でなく命令番号で書くこと
 * ================================================================ */

/* op の名前。番号は iwac 内部の事情なので持ち出さず、
 * 名前を仕様の通貨にする (doc/asari.md) */
static char *op_name(int op) {
    if (op == OP_LUI) return "lui";
    if (op == OP_ADDI) return "addi";
    if (op == OP_ADD) return "add";
    if (op == OP_SUB) return "sub";
    if (op == OP_SB) return "sb";
    if (op == OP_WFI) return "wfi";
    if (op == OP_LBU) return "lbu";
    if (op == OP_AUIPC) return "auipc";
    if (op == OP_BEQ) return "beq";
    if (op == OP_JAL) return "jal";
    if (op == OP_SD) return "sd";
    if (op == OP_LD) return "ld";
    if (op == OP_JALR) return "jalr";
    if (op == OP_SLTU) return "sltu";
    if (op == OP_SLTIU) return "sltiu";
    if (op == OP_AND) return "and";
    if (op == OP_MUL) return "mul";
    if (op == OP_DIV) return "div";
    if (op == OP_SLT) return "slt";
    if (op == OP_XORI) return "xori";
    if (op == OP_ECALL) return "ecall";
    if (op == OP_SLL) return "sll";
    if (op == OP_SRA) return "sra";
    if (op == OP_OR) return "or";
    if (op == OP_XOR) return "xor";
    if (op == OP_REM) return "rem";
    if (op == OP_SLLI) return "slli";
    if (op == OP_BNE) return "bne";
    if (op == OP_ADDIW) return "addiw";
    if (op == OP_SRLI) return "srli";
    if (op == OP_LH) return "lh";
    if (op == OP_LHU) return "lhu";
    if (op == OP_LW) return "lw";
    if (op == OP_LWU) return "lwu";
    if (op == OP_SH) return "sh";
    if (op == OP_SW) return "sw";
    if (op == OP_DIVU) return "divu";
    if (op == OP_REMU) return "remu";
    if (op == OP_SRL) return "srl";
    if (op == OP_SRAI) return "srai";
    return "?";
}

static void write_asari(void) {
    /* 入口の断片を持つか (write_object と同じ判定) */
    int has_entry = (prog_main >= 0 || prog_stmts != -1 ||
                     prog_has_final);
    int start = has_entry ? 0 : obj_prefix_insts;

    obj_str("asari 1\n");
    obj_str("target kosaba\n");
    obj_str("prefix ");
    obj_num(obj_prefix_insts);
    obj_str("\n");
    obj_str("flush ");
    obj_num(obj_flush_site);
    obj_str("\n");
    if (has_entry)
        obj_str("entry 1\n");
    /* 命令列 (全部を出す。入口の無い断片では、先頭の
     * prefix 個を Asari が落とす)。呼び出しの jal は
     * imm を 0 にし、行き先は下の call 注記で渡す */
    for (int i = 0; i < nasm; i++) {
        int imm = asm_imm[i];
        if (asm_op[i] == OP_JAL && imm < 0)
            imm = 0;
        obj_str("i ");
        obj_str(op_name(asm_op[i]));
        obj_str(" ");
        obj_num(asm_rd[i]);
        obj_str(" ");
        obj_num(asm_rs1[i]);
        obj_str(" ");
        obj_num(asm_rs2[i]);
        obj_str(" ");
        obj_num(imm);
        obj_str("\n");
    }
    /* 注記: 呼び出し (write_object の振り分けと同じ。
     * 位置は命令番号) */
    for (int i = start; i < nasm; i++) {
        if (asm_op[i] != OP_JAL || asm_imm[i] >= 0)
            continue;
        if (i == obj_flush_site || asm_imm[i] == -1000003) {
            obj_str("callopt ");
            obj_num(i);
            obj_str(" kosaba_flush\n");
        } else {
            obj_str("call ");
            obj_num(i);
            obj_str(" ");
            obj_str(func_names[-asm_imm[i] - 1]);
            obj_str("\n");
        }
    }
    /* 注記: 住所作り (gen の控えの順、位置は命令番号) */
    for (int r = 0; r < obj_nreloc; r++) {
        if (obj_reloc_kind[r] == RLOC_STR) {
            obj_str("addrstr ");
            obj_num(obj_reloc_site[r]);
            obj_str(" ");
            obj_num(str_off[obj_reloc_idx[r]]);
            obj_str("\n");
        } else if (obj_reloc_kind[r] == RLOC_VAR) {
            obj_str("addr ");
            obj_num(obj_reloc_site[r]);
            obj_str(" var ");
            obj_str(var_names[obj_reloc_idx[r]]);
            obj_str("\n");
        } else {
            obj_str("addr ");
            obj_num(obj_reloc_site[r]);
            obj_str(" arr ");
            obj_str(arr_names[obj_reloc_idx[r]]);
            obj_str("\n");
        }
    }
    for (int r = 0; r < obj_nabs; r++) {
        obj_str("addabs ");
        obj_num(obj_abs_site[r]);
        obj_str(" ");
        obj_num((long)(unsigned int)obj_abs_val[r]);
        obj_str("\n");
    }
    /* データ側の写し (write_object と同じ材料を、
     * func の位置と大きさだけ命令の単位で書く) */
    obj_str("strings ");
    obj_num(str_used);
    obj_str("\n");
    obj_hex((unsigned char *)str_pool, str_used);
    obj_str("arrays ");
    obj_num(arr_used);
    obj_str("\n");
    for (int f = 0; f < nfunc; f++) {
        if (func_has_body[f]) {
            long size = (long)(nasm - func_entry[f]);
            for (int g = f + 1; g < nfunc; g++) {
                if (func_has_body[g]) {
                    size = (long)(func_entry[g] - func_entry[f]);
                    break;
                }
            }
            obj_str("func ");
            obj_str(func_names[f]);
            obj_str(" ");
            obj_num(func_entry[f]);
            obj_str(" ");
            obj_num(size);
            obj_str("\n");
        } else {
            obj_str("decl ");
            obj_str(func_names[f]);
            obj_str("\n");
        }
    }
    for (int s = 0; s < var_nglobal; s++) {
        for (int v = 0; v < nvar; v++) {
            if (var_owner[v] >= 0 || var_slot[v] != s)
                continue;
            int ns = var_nglobal - s;
            for (int w = 0; w < nvar; w++) {
                if (var_owner[w] >= 0 || var_slot[w] <= s)
                    continue;
                if (var_slot[w] - s < ns)
                    ns = var_slot[w] - s;
            }
            obj_str(var_init_kind[v] != 0 ? "var " : "varc ");
            obj_str(var_names[v]);
            obj_str(" ");
            obj_num(ns);
            obj_str("\n");
            break;
        }
    }
    for (int a = 0; a < narr; a++) {
        if (arr_owner[a] >= 0)
            continue;
        long bytes = (long)arr_used - (long)arr_off[a];
        for (int b = 0; b < narr; b++) {
            if (arr_owner[b] >= 0 || b == a)
                continue;
            if (arr_off[b] > arr_off[a] &&
                (long)arr_off[b] - (long)arr_off[a] < bytes)
                bytes = (long)arr_off[b] - (long)arr_off[a];
        }
        obj_str("arr ");
        obj_str(arr_names[a]);
        obj_str(" ");
        obj_num(arr_off[a]);
        obj_str(" ");
        obj_num(bytes);
        obj_str("\n");
    }
    for (int s = 0; s < var_nglobal; s++) {
        for (int v = 0; v < nvar; v++) {
            if (var_owner[v] >= 0 || var_slot[v] != s)
                continue;
            if (var_init_kind[v] == 1) {
                obj_str("init ");
                obj_str(var_names[v]);
                obj_str(" ");
                obj_num(var_init_val[v]);
                obj_str("\n");
            } else if (var_init_kind[v] == 2) {
                obj_str("iptr ");
                obj_str(var_names[v]);
                obj_str(" str ");
                obj_num(str_off[var_init_val[v]]);
                obj_str("\n");
            }
            break;
        }
    }
    for (int f = 0; f < nfunc; f++) {
        if (func_called[f] && !func_has_body[f]) {
            obj_str("undef ");
            obj_str(func_names[f]);
            obj_str("\n");
        }
    }
    obj_str("end\n");
}

/* ================================================================
 * ELF オブジェクトの出力 (形式 version 2、v39。
 * 設計は doc/elf-object.md)
 * 材料は write_object と同じで、テキストの代わりに
 * ELF64 の ET_REL を書く。セクションは .text /
 * .data / .bss / .iwac.strings / .iwac.arrays、
 * 再配置は .rela.text / .rela.data。配置の規則は
 * nori が握る (このファイルは置き場所を決めない)
 * ================================================================ */
static long elf_off;  /* ここまで書いたバイト数 */

static void ebyte(int b) {
    out_byte(b);
    elf_off = elf_off + 1;
}

static void eu16(int v) {
    ebyte(v & 0xff);
    ebyte((v >> 8) & 0xff);
}

static void eu32(long v) {
    ebyte((int)(v & 0xff));
    ebyte((int)((v >> 8) & 0xff));
    ebyte((int)((v >> 16) & 0xff));
    ebyte((int)((v >> 24) & 0xff));
}

static void eu64(long v) {
    for (int k = 0; k < 8; k++)
        ebyte((int)((v >> (k * 8)) & 0xff));
}

static void epad(long align) {
    while (elf_off % align != 0)
        ebyte(0);
}

static void ebytes(char *p, long n) {
    for (long i = 0; i < n; i++)
        ebyte(p[i] & 0xff);
}

static int ename_len(char *s) {
    int n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

/* 変数 v の枠数 (write_object の var 書き出しと
 * 同じ「次の変数までの間隔」の数え方) */
static int elf_var_slots(int v) {
    int ns = var_nglobal - var_slot[v];
    for (int w = 0; w < nvar; w++) {
        if (var_owner[w] >= 0 || var_slot[w] <= var_slot[v])
            continue;
        if (var_slot[w] - var_slot[v] < ns)
            ns = var_slot[w] - var_slot[v];
    }
    return ns;
}

/* 配列 a のバイト数 (write_object と同じ数え方) */
static long elf_arr_bytes(int a) {
    long bytes = (long)arr_used - (long)arr_off[a];
    for (int b = 0; b < narr; b++) {
        if (arr_owner[b] >= 0 || b == a)
            continue;
        if (arr_off[b] > arr_off[a] &&
            (long)arr_off[b] - (long)arr_off[a] < bytes)
            bytes = (long)arr_off[b] - (long)arr_off[a];
    }
    return bytes;
}

/* シンボルの素性置き場 (describe が埋める) */
static char *elf_d_name;
static int elf_d_info;
static int elf_d_shndx;
static long elf_d_value;
static long elf_d_size;

/* セクション番号の約束 (このファイルの中だけ) */
static int ELF_SEC_TEXT = 1;
static int ELF_SEC_DATA = 2;
static int ELF_SEC_BSS = 3;
static int ELF_SEC_STRINGS = 4;
static int ELF_SEC_ARRAYS = 5;

/* ローカルシンボル i の素性。並びは [0]=空、
 * [1..5]=セクション、次に再配置の組ごとの
 * ラベル (PCREL_LO12 の相手)、最後に絶対
 * アドレスのシンボル */
static void elf_describe_local(int i, int start) {
    elf_d_name = "";
    elf_d_info = 0;
    elf_d_shndx = 0;
    elf_d_value = 0;
    elf_d_size = 0;
    if (i == 0)
        return;
    if (i <= 5) {
        elf_d_info = 3;  /* LOCAL / SECTION */
        elf_d_shndx = i;
        return;
    }
    if (i < 6 + obj_nreloc) {
        int r = i - 6;
        elf_d_shndx = ELF_SEC_TEXT;
        elf_d_value = (long)(obj_reloc_site[r] - start) * 4;
        return;
    }
    {
        int r = i - 6 - obj_nreloc;
        elf_d_shndx = 0xfff1;  /* SHN_ABS */
        elf_d_value = (long)(unsigned int)obj_abs_val[r];
    }
}

/* 関数 f が kosaba_flush か (名前で正確に判定) */
static int elf_is_flush(int f) {
    char *want = "kosaba_flush";
    if (func_name_len[f] != 12)
        return 0;
    for (int k = 0; k < 12; k++)
        if (func_names[f][k] != want[k])
            return 0;
    return 1;
}

/* kosaba_flush が関数表にあるか */
static int elf_flush_in_table(void) {
    for (int f = 0; f < nfunc; f++)
        if (elf_is_flush(f))
            return 1;
    return 0;
}

static int elf_nglob_var(void) {
    int n = 0;
    for (int v = 0; v < nvar; v++)
        if (var_owner[v] < 0)
            n++;
    return n;
}

static int elf_nglob_arr(void) {
    int n = 0;
    for (int a = 0; a < narr; a++)
        if (arr_owner[a] < 0)
            n++;
    return n;
}

/* 大域名シンボル g の素性。並びは iwac_entry
 * (あれば)、関数 (表の順)、変数 (表の順)、
 * 配列 (表の順)、最後に kosaba_flush の弱い
 * 未定義 (表に無いのに呼び出しがあるとき) */
static void elf_describe_global(int g, int has_entry, int start,
                                int flush_extra) {
    int off0 = has_entry ? 1 : 0;
    elf_d_name = "";
    elf_d_info = 0x10;  /* GLOBAL / NOTYPE */
    elf_d_shndx = 0;
    elf_d_value = 0;
    elf_d_size = 0;
    if (has_entry && g == 0) {
        elf_d_name = "iwac_entry";
        elf_d_shndx = ELF_SEC_TEXT;
        elf_d_size = (long)obj_prefix_insts * 4;
        return;
    }
    if (g < off0 + nfunc) {
        int f = g - off0;
        elf_d_name = func_names[f];
        if (func_has_body[f]) {
            long size = (long)(nasm - func_entry[f]);
            for (int h = f + 1; h < nfunc; h++) {
                if (func_has_body[h]) {
                    size = (long)(func_entry[h] - func_entry[f]);
                    break;
                }
            }
            elf_d_info = 0x12;  /* GLOBAL / FUNC */
            elf_d_shndx = ELF_SEC_TEXT;
            elf_d_value = (long)(func_entry[f] - start) * 4;
            elf_d_size = size * 4;
        } else if (elf_is_flush(f)) {
            /* 定義のない kosaba_flush は弱くする
             * (callopt の意味、doc/elf-object.md) */
            elf_d_info = 0x20;  /* WEAK / NOTYPE */
        }
        return;
    }
    if (g < off0 + nfunc + elf_nglob_var()) {
        /* 変数は「枠の順」に並べる (nori が .iwo の
         * var レコードと同じ順で登録できるように。
         * 配置の一致に効く) */
        int want = g - off0 - nfunc;
        int v = -1;
        for (int u = 0; u < nvar; u++) {
            if (var_owner[u] >= 0)
                continue;
            int rank = 0;
            for (int w = 0; w < nvar; w++)
                if (var_owner[w] < 0 && var_slot[w] < var_slot[u])
                    rank++;
            if (rank == want) {
                v = u;
                break;
            }
        }
        elf_d_name = var_names[v];
        elf_d_info = 0x11;  /* GLOBAL / OBJECT */
        if (var_init_kind[v] != 0) {
            elf_d_shndx = ELF_SEC_DATA;
            elf_d_value = (long)var_slot[v] * 8;
        } else {
            elf_d_shndx = 0xfff2;  /* SHN_COMMON (仮定義) */
            elf_d_value = 8;       /* 並び (alignment) */
        }
        elf_d_size = (long)elf_var_slots(v) * 8;
        return;
    }
    if (g < off0 + nfunc + elf_nglob_var() + elf_nglob_arr()) {
        int want = g - off0 - nfunc - elf_nglob_var();
        int seen = -1;
        int a = -1;
        for (int u = 0; u < narr; u++) {
            if (arr_owner[u] >= 0)
                continue;
            seen++;
            if (seen == want) {
                a = u;
                break;
            }
        }
        elf_d_name = arr_names[a];
        elf_d_info = 0x11;  /* GLOBAL / OBJECT */
        elf_d_shndx = ELF_SEC_ARRAYS;
        elf_d_value = arr_off[a];
        elf_d_size = elf_arr_bytes(a);
        return;
    }
    /* flush_extra の置き場 (呼び出し側が数える) */
    elf_d_name = "kosaba_flush";
    elf_d_info = 0x20;  /* WEAK / NOTYPE */
}

/* シンボル番号 → 名前 (オフセット計算と .strtab
 * 書き出しで使う) */
static char *elf_sym_name(int s, int nlocals, int has_entry,
                          int start, int flush_extra) {
    if (s < nlocals) {
        elf_describe_local(s, start);
    } else {
        elf_describe_global(s - nlocals, has_entry, start,
                            flush_extra);
    }
    return elf_d_name;
}


/* セクション名 (番号順。0 は空) */
static char *elf_sec_name(int i) {
    if (i == 1) return ".text";
    if (i == 2) return ".data";
    if (i == 3) return ".bss";
    if (i == 4) return ".iwac.strings";
    if (i == 5) return ".iwac.arrays";
    if (i == 6) return ".rela.text";
    if (i == 7) return ".rela.data";
    if (i == 8) return ".symtab";
    if (i == 9) return ".strtab";
    if (i == 10) return ".shstrtab";
    return "";
}

/* 変数・配列のシンボル番号 (表の順の数え直し) */
static int elf_var_symidx(int v, int base) {
    /* 変数の並びは枠の順 (elf_describe_global と同じ) */
    int rank = 0;
    for (int u = 0; u < nvar; u++)
        if (var_owner[u] < 0 && var_slot[u] < var_slot[v])
            rank++;
    return base + rank;
}

static int elf_arr_symidx(int a, int base) {
    int rank = 0;
    for (int u = 0; u < a; u++)
        if (arr_owner[u] < 0)
            rank++;
    return base + rank;
}

/* link / info / entsize は呼び出し前に置く
 * (elf_shdr の引数を8個以内に収めるため) */
static int elf_sh_link;
static int elf_sh_info;
static long elf_sh_entsize;

static void elf_shdr(int name, int type, long flags, long off,
                     long size, long align) {
    eu32(name);
    eu32(type);
    eu64(flags);
    eu64(0);
    eu64(off);
    eu64(size);
    eu32(elf_sh_link);
    eu32(elf_sh_info);
    eu64(align);
    eu64(elf_sh_entsize);
    elf_sh_link = 0;
    elf_sh_info = 0;
    elf_sh_entsize = 0;
}

static void write_elf_object(void) {
    int has_entry = (prog_main >= 0 || prog_stmts != -1 ||
                     prog_has_final);
    int start = has_entry ? 0 : obj_prefix_insts;
    int off0 = has_entry ? 1 : 0;

    /* 数える */
    int ncalls = 0;
    for (int i = start; i < nasm; i++)
        if (asm_op[i] == OP_JAL && asm_imm[i] < 0)
            ncalls++;
    int flush_extra = (obj_flush_site >= 0 &&
                       !elf_flush_in_table()) ? 1 : 0;
    int nlocals = 6 + obj_nreloc + obj_nabs;
    int nglob = off0 + nfunc + elf_nglob_var() +
                elf_nglob_arr() + flush_extra;
    int nsym = nlocals + nglob;
    int nrela_text = ncalls + 2 * obj_nreloc + 2 * obj_nabs;
    int nrela_data = 0;
    for (int v = 0; v < nvar; v++)
        if (var_owner[v] < 0 && var_init_kind[v] == 2)
            nrela_data++;
    int img_slots = 0;
    for (int v = 0; v < nvar; v++) {
        if (var_owner[v] >= 0 || var_init_kind[v] == 0)
            continue;
        int end = var_slot[v] + elf_var_slots(v);
        if (end > img_slots)
            img_slots = end;
    }
    long text_size = (long)(nasm - start) * 4;
    long data_size = (long)img_slots * 8;
    long bss_size = (long)var_nglobal * 8 - data_size;
    long strings_size = str_used;
    long arrays_size = arr_used;
    long strtab_size = 1;
    for (int s2 = 0; s2 < nsym; s2++)
        strtab_size += ename_len(elf_sym_name(s2, nlocals,
                             has_entry, start, flush_extra)) + 1;
    long shstrtab_size = 0;
    for (int i = 0; i < 11; i++)
        shstrtab_size += ename_len(elf_sec_name(i)) + 1;

    /* 置き場所 */
    long text_off = 64;
    long strings_off = text_off + text_size;
    long data_off = (strings_off + strings_size + 7) & ~7L;
    long rela_text_off = (data_off + data_size + 7) & ~7L;
    long rela_data_off = rela_text_off + (long)nrela_text * 24;
    long symtab_off = (rela_data_off + (long)nrela_data * 24 + 7) & ~7L;
    long strtab_off = symtab_off + (long)nsym * 24;
    long shstrtab_off = strtab_off + strtab_size;
    long shoff = (shstrtab_off + shstrtab_size + 7) & ~7L;

    /* ELF ヘッダ */
    elf_off = 0;
    ebyte(0x7f); ebyte('E'); ebyte('L'); ebyte('F');
    ebyte(2); ebyte(1); ebyte(1); ebyte(0);
    for (int k = 0; k < 8; k++)
        ebyte(0);
    eu16(1);          /* ET_REL */
    eu16(243);        /* EM_RISCV */
    eu32(1);          /* version */
    eu64(0);          /* entry */
    eu64(0);          /* phoff */
    eu64(shoff);
    eu32(0);          /* flags */
    eu16(64);         /* ehsize */
    eu16(0);          /* phentsize */
    eu16(0);          /* phnum */
    eu16(64);         /* shentsize */
    eu16(11);         /* shnum */
    eu16(10);         /* shstrndx */

    /* .text (穴の正規化は write_object と同じ) */
    for (int i = start; i < nasm; i++) {
        int saved_imm = asm_imm[i];
        if (asm_op[i] == OP_JAL && saved_imm < 0)
            asm_imm[i] = i;
        eu32(encode_inst(i));
        asm_imm[i] = saved_imm;
    }
    /* .iwac.strings */
    ebytes((char *)str_pool, str_used);
    /* .data (グローバルの初期イメージ) */
    epad(8);
    for (int slot = 0; slot < img_slots; slot++) {
        long val = 0;
        for (int v = 0; v < nvar; v++) {
            if (var_owner[v] >= 0 || var_slot[v] != slot)
                continue;
            if (var_init_kind[v] == 1)
                val = var_init_val[v];
            else if (var_init_kind[v] == 2)
                val = str_off[var_init_val[v]];
            break;
        }
        eu64(val);
    }
    /* .rela.text */
    epad(8);
    for (int i = start; i < nasm; i++) {
        if (asm_op[i] != OP_JAL || asm_imm[i] >= 0)
            continue;
        int sym;
        if (i == obj_flush_site || asm_imm[i] == -1000003) {
            if (elf_flush_in_table()) {
                int f = 0;
                while (!elf_is_flush(f))
                    f++;
                sym = nlocals + off0 + f;
            } else {
                sym = nlocals + off0 + nfunc + elf_nglob_var() +
                      elf_nglob_arr();
            }
        } else {
            sym = nlocals + off0 + (-asm_imm[i] - 1);
        }
        eu64((long)(i - start) * 4);
        eu64(((long)sym << 32) | 17);  /* R_RISCV_JAL */
        eu64(0);
    }
    for (int r = 0; r < obj_nreloc; r++) {
        int sym;
        long add = 0;
        if (obj_reloc_kind[r] == RLOC_STR) {
            sym = 4;  /* .iwac.strings のセクションシンボル */
            add = str_off[obj_reloc_idx[r]];
        } else if (obj_reloc_kind[r] == RLOC_VAR) {
            sym = elf_var_symidx(obj_reloc_idx[r],
                                 nlocals + off0 + nfunc);
        } else {
            sym = elf_arr_symidx(obj_reloc_idx[r],
                                 nlocals + off0 + nfunc +
                                 elf_nglob_var());
        }
        eu64((long)(obj_reloc_site[r] - start) * 4);
        eu64(((long)sym << 32) | 23);  /* R_RISCV_PCREL_HI20 */
        eu64(add);
        eu64((long)(obj_reloc_site[r] - start) * 4 + 4);
        eu64(((long)(6 + r) << 32) | 24);  /* PCREL_LO12_I */
        eu64(0);
    }
    for (int r = 0; r < obj_nabs; r++) {
        int sym = 6 + obj_nreloc + r;
        eu64((long)(obj_abs_site[r] - start) * 4);
        eu64(((long)sym << 32) | 26);  /* R_RISCV_HI20 */
        eu64(0);
        eu64((long)(obj_abs_site[r] - start) * 4 + 4);
        eu64(((long)sym << 32) | 27);  /* R_RISCV_LO12_I */
        eu64(0);
    }
    /* .rela.data (文字列へのポインタの初期値) */
    for (int v = 0; v < nvar; v++) {
        if (var_owner[v] >= 0 || var_init_kind[v] != 2)
            continue;
        eu64((long)var_slot[v] * 8);
        eu64(((long)4 << 32) | 2);  /* R_RISCV_64、相手は文字列 */
        eu64(str_off[var_init_val[v]]);
    }
    /* .symtab */
    epad(8);
    for (int s2 = 0; s2 < nsym; s2++) {
        long name_off = 1;
        for (int t = 0; t < s2; t++)
            name_off += ename_len(elf_sym_name(t, nlocals,
                                has_entry, start, flush_extra)) + 1;
        if (s2 < nlocals)
            elf_describe_local(s2, start);
        else
            elf_describe_global(s2 - nlocals, has_entry, start,
                                flush_extra);
        eu32(name_off);
        ebyte(elf_d_info);
        ebyte(0);
        eu16(elf_d_shndx);
        eu64(elf_d_value);
        eu64(elf_d_size);
    }
    /* .strtab */
    ebyte(0);
    for (int s2 = 0; s2 < nsym; s2++) {
        char *nm = elf_sym_name(s2, nlocals, has_entry, start,
                                flush_extra);
        ebytes(nm, ename_len(nm));
        ebyte(0);
    }
    /* .shstrtab */
    for (int i = 0; i < 11; i++) {
        ebytes(elf_sec_name(i), ename_len(elf_sec_name(i)));
        ebyte(0);
    }
    /* セクションヘッダ */
    epad(8);
    elf_sh_link = 0;
    elf_sh_info = 0;
    elf_sh_entsize = 0;
    elf_shdr(0, 0, 0, 0, 0, 0);
    {
        long noff[11];
        long acc = 0;
        for (int i = 0; i < 11; i++) {
            noff[i] = acc;
            acc += ename_len(elf_sec_name(i)) + 1;
        }
        elf_shdr((int)noff[1], 1, 6, text_off, text_size, 4);
        elf_shdr((int)noff[2], 1, 3, data_off, data_size, 8);
        elf_shdr((int)noff[3], 8, 3, data_off + data_size,
                 bss_size, 8);
        elf_shdr((int)noff[4], 1, 3, strings_off, strings_size, 1);
        elf_shdr((int)noff[5], 8, 3, strings_off + strings_size,
                 arrays_size, 8);
        elf_sh_link = 8;
        elf_sh_info = 1;
        elf_sh_entsize = 24;
        elf_shdr((int)noff[6], 4, 0, rela_text_off,
                 (long)nrela_text * 24, 8);
        elf_sh_link = 8;
        elf_sh_info = 2;
        elf_sh_entsize = 24;
        elf_shdr((int)noff[7], 4, 0, rela_data_off,
                 (long)nrela_data * 24, 8);
        elf_sh_link = 9;
        elf_sh_info = nlocals;
        elf_sh_entsize = 24;
        elf_shdr((int)noff[8], 2, 0, symtab_off,
                 (long)nsym * 24, 8);
        elf_shdr((int)noff[9], 3, 0, strtab_off, strtab_size, 1);
        elf_shdr((int)noff[10], 3, 0, shstrtab_off, shstrtab_size, 1);
    }
}

static void write_binary(void) {
    if (elf_mode) {
        write_elf_object();
        return;
    }
    if (asari_mode) {
        write_asari();
        return;
    }
    if (obj_mode) {
        write_object();
        return;
    }
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
    /* 初期値つきグローバルがあるときだけ初期イメージを
     * 付ける (v24)。ベアメタル版はバイナリそのものが
     * メモリイメージなので配列のゼロも全部書く。
     * KosabaOS 版は枠だけを続けて書き、配列のゼロは
     * ローダのゼロ埋めに任せる (v28) */
    if (has_var_inits) {
        if (target_kosaba)
            write_var_image();
        else
            write_global_image();
    }
}

