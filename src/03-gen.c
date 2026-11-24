/* ================================================================
 * 第2層 gen: AST → 命令列
 *
 * 「命令列」は機械語の一歩手前の形。命令を、種類と
 * レジスタ番号と即値の組として配列に並べたもの:
 *   asm_op[i]  : 命令の種類 (OP_LUI など)
 *   asm_rd[i]  : 行き先レジスタ番号 (使わない命令では 0)
 *   asm_rs1[i] : 元レジスタ1 (同上)
 *   asm_rs2[i] : 元レジスタ2 (同上)
 *   asm_imm[i] : 即値 (同上)
 * この層はビット列のことは何も知らない。「lui する」
 * 「add する」という言葉だけで考える。ビット列への
 * 変換は第3層の仕事。
 *
 * 容量の約束: 命令は最大でもノード数の数倍程度 (演算
 * ノード1つが数命令、住所作りも命令を使う)。ノード
 * 20480 に対し命令表 40960 を用意する (v30 で実測に
 * 合わせて拡大: iwac 自身のコンパイルでノード
 * 11,783、命令 28,118、関数 109 を使った)。
 * ================================================================ */

static int OP_LUI = 0;
static int OP_ADDI = 1;
static int OP_ADD = 2;
static int OP_SUB = 3;
static int OP_SB = 4;
static int OP_WFI = 5;
static int OP_LBU = 6;  /* メモリから1バイト読む (符号なし) */
static int OP_AUIPC = 7; /* 自分の住所 + 即値 (PC 相対) */
static int OP_BEQ = 8;   /* 2つのレジスタが等しければ飛ぶ。
                          * asm_imm には「飛び先の命令番号」を
                          * 入れる (バイト数ではない)。
                          * バイト単位の距離への変換は第3層 */
static int OP_JAL = 9;   /* 無条件に飛ぶ。asm_imm は同上、
                          * asm_rd は 0 (戻り先は保存しない)。
                          * 関数呼び出しでは asm_rd を ra (1) にし、
                          * asm_imm には関数番号を負数で入れる
                          * (-(関数番号+1))。生成の最後に、
                          * 関数の入口番号へ書き直す */
static int OP_SD = 10;   /* メモリへ8バイト書く (ra の退避用) */
static int OP_LD = 11;   /* メモリから8バイト読む (ra の復帰用) */
static int OP_JALR = 12; /* レジスタの指す住所へ飛ぶ (関数からの
                          * 戻り。rd=0, rs1=ra, imm=0) */
static int OP_SLTU = 13; /* 符号なし比較: rs1 < rs2 なら 1
                          * (0 と比べれば「0 でないか」になる) */
static int OP_SLTIU = 14;/* 即値との符号なし比較
                          * (1 と比べれば「0 かどうか」になる) */
static int OP_AND = 15;  /* ビットごとの AND (両方 1 の桁だけ 1) */
static int OP_MUL = 16;  /* 掛け算 (M 拡張。RV64G 前提) */
static int OP_DIV = 17;  /* 割り算 (符号付き、M 拡張) */
static int OP_SLT = 18;  /* 符号付き比較: rs1 < rs2 なら 1
                          * (関係演算子用。SLTU と funct3 だけ違う) */
static int OP_XORI = 19; /* 即値との XOR。1 との XOR は
                          * 「1か0の反転」に使える */
static int OP_SLL = 21;  /* 左シフト (v19)。R 形式 funct3=1 */
static int OP_SRA = 22;  /* 算術右シフト (v19)。funct3=5、
                          * funct7=0x20 (符号を保つ) */
static int OP_OR = 23;   /* ビット OR (v19)。funct3=6 */
static int OP_XOR = 24;  /* ビット XOR (v19)。funct3=4 */
static int OP_REM = 25;  /* 剰余 (v19、M 拡張)。funct7=1、
                          * funct3=6。除数が 0 なら被除数が残る */
static int OP_SLLI = 26; /* 左シフト即値 (v23)。I 形式、
                          * funct3=1、imm はシフト量。
                          * 添字の ×8 (要素の大きさ) に使う */
static int OP_ECALL = 20; /* ecall (環境呼び出し)。KosabaOS
                           * ターゲットで OS のシステムコールを
                           * 呼ぶ命令。符号化は固定の1語 (v14) */
static int OP_BNE = 27;  /* 2つのレジスタが等しくなければ飛ぶ
                          * (v27)。beq と funct3 だけが違う。
                          * 届かない条件分岐を「反転 + jal」の
                          * 形に伸ばすときに使う (emit_beq_t1z) */
static int OP_ADDIW = 28; /* addiw (v27): 32ビットで加えて
                          * 符号拡張する。大きな定数を作る
                          * (lui + addiw) とき専用 */
static int OP_SRLI = 29; /* 論理右シフト即値 (v27)。I 形式、
                          * funct3=5、imm はシフト量。
                          * 32ビット定数を 0 埋めで 64 ビットに
                          * 広げる (gen_load_u32) ときに使う */
static int OP_LH = 30;   /* 2バイト読み (符号拡張、v35) */
static int OP_LHU = 31;  /* 2バイト読み (ゼロ拡張、v35) */
static int OP_LW = 32;   /* 4バイト読み (符号拡張、v35) */
static int OP_LWU = 33;  /* 4バイト読み (ゼロ拡張、v35) */
static int OP_SH = 34;   /* 2バイト書き (v35) */
static int OP_SW = 35;   /* 4バイト書き (v35) */
static int OP_DIVU = 36; /* 符号なし割り算 (v35、M 拡張) */
static int OP_REMU = 37; /* 符号なし剰余 (v35、M 拡張) */
static int OP_SRL = 38;  /* 論理右シフト (v35) */
static int OP_SRAI = 39; /* 算術右シフト即値 (v35) */
static int OP_FLD = 40;     /* fld (v46): メモリから double を
                             * 浮動小数点レジスタへ読む。I 形式、
                             * opcode 0x07、funct3 が 3 */
static int OP_FSD = 41;     /* fsd (v46): S 形式、opcode 0x27、
                             * funct3 が 3 */
static int OP_FADD_D = 42;  /* fadd.d (v46): R 形式、opcode
                             * 0x53、funct7 が 0x01 */
static int OP_FSUB_D = 43;  /* fsub.d (v46): funct7 が 0x05 */
static int OP_FMUL_D = 44;  /* fmul.d (v46): funct7 が 0x09 */
static int OP_FDIV_D = 45;  /* fdiv.d (v46): funct7 が 0x0D */
static int OP_FEQ_D = 46;   /* feq.d (v46): funct7 が 0x51、
                             * funct3 が 2。結果は整数レジスタ */
static int OP_FLT_D = 47;   /* flt.d (v46): funct7 が 0x51、
                             * funct3 が 1 */
static int OP_FLE_D = 48;   /* fle.d (v46): funct7 が 0x51、
                             * funct3 が 0 */
static int OP_FMV_D_X = 49; /* fmv.d.x (v46): 整数レジスタの
                             * ビットを浮動小数点レジスタへ。
                             * funct7 が 0x79 */
static int OP_FMV_X_D = 50; /* fmv.x.d (v46): 逆向き。
                             * funct7 が 0x71 */
static int OP_FCVT_D_L = 51;  /* fcvt.d.l (v46): 整数 → double。
                               * funct7 が 0x69、rs2 の欄が
                               * 変換元の幅と符号 (l = 2) */
static int OP_FCVT_D_LU = 52; /* fcvt.d.lu (v46): rs2 の欄が 3 */
static int OP_FCVT_L_D = 53;  /* fcvt.l.d (v46): double → 整数
                               * (切り捨て)。funct7 が 0x61、
                               * rs2 の欄が 2、funct3 は丸め
                               * (切り捨て = 1) */
static int OP_FCVT_LU_D = 54; /* fcvt.lu.d (v46): rs2 の欄が 3 */
static int OP_FNEG_D = 55;    /* fneg.d (v46): fsgnj.d
                               * (funct7 が 0x11) の funct3 が
                               * 1 の形 (符号の反転) */
static int OP_FMV_D = 56;     /* fmv.d (v46): fsgnj.d の
                               * funct3 が 0 の形 (レジスタ間の
                               * 写し) */
static int OP_CSRS = 57;      /* csrs (v46): CSR にビットを
                               * 立てる (csrrs)。asm_imm が
                               * CSR 番号。ベアメタルの入口で
                               * sstatus の FS (FPU 有効) を
                               * 立てるためだけに使う */
static int OP_BLT = 58;       /* blt (v53): rs1 < rs2 (符号付き)
                               * なら飛ぶ。B 形式、funct3 は 4 */
static int OP_BGE = 59;       /* bge (v53): rs1 >= rs2 (符号
                               * 付き) なら飛ぶ。funct3 は 5 */
static int OP_BLTU = 60;      /* bltu (v53): 符号なしの < 。
                               * funct3 は 6 */
static int OP_BGEU = 61;      /* bgeu (v53): 符号なしの >= 。
                               * funct3 は 7 */
static int OP_ANDI = 62;      /* andi (v55): 即値との AND。
                               * I 形式、funct3 は 7 */
static int OP_ORI = 63;       /* ori (v55): 即値との OR。
                               * funct3 は 6 */
static int OP_SLTI = 64;      /* slti (v55): 即値との符号付き
                               * 比較。funct3 は 2 */
static int OP_ADDW = 65;      /* addw (v60): 32ビット加算。
                               * R 形式、opcode 0x3B、funct3 は 0。
                               * 結果は符号拡張される */
static int OP_SUBW = 66;      /* subw (v60): funct7 が 0x20 */
static int OP_MULW = 67;      /* mulw (v60): M 拡張、funct7 が
                               * 1、funct3 は 0 */
static int OP_DIVW = 68;      /* divw (v60): funct3 は 4 */
static int OP_REMW = 69;      /* remw (v60): funct3 は 6 */
static int OP_SLLW = 70;      /* sllw (v60): funct3 は 1。
                               * シフト量は下位5ビット */
static int OP_SRAW = 71;      /* sraw (v60): funct7 が 0x20、
                               * funct3 は 5 */
static int OP_SLLIW = 72;     /* slliw (v60): I 形式、opcode
                               * 0x1B、funct3 は 1 */
static int OP_SRAIW = 73;     /* sraiw (v60): funct3 は 5、
                               * 即値の上位に funct7 (0x20) */

/* 命令表の上限は 24576 (v39 で 40960 → 49152、
 * v44 で → 57344、v45 で → 59392 と増量し、
 * 2026-11-18 の段3 (doc/split-selfhost.md) で
 * TU 基準化して 24576 へ下げた。断片 TU の
 * 実測最大は 03-gen の 20,244)。
 * v38 の時点で一括セルフコンパイルが約44K命令に
 * 達して上限を超え、asm_push が無言で表の外へ
 * 書いて隣の表を壊していた (点検は asm_push と
 * gen_program にある)。op / rd / rs1 / rs2 は
 * 小さい値しか入らないので char にして、増量の
 * 対価 (imm と patch_next の増分) を払った */
static char asm_op[24576];
static char asm_rd[24576];
static char asm_rs1[24576];
static char asm_rs2[24576];
static int asm_imm[24576];
static int nasm;  /* 並べた命令の数 */

/* 命令を1つ、命令列の末尾に積む */
static void asm_push(int op, int rd, int rs1, int rs2, int imm) {
    if (nasm >= 24576) {
        /* 上限あふれ: 表の外へは書かない (v39)。
         * 生成は最後まで回るが、gen_program が
         * nasm を見て失敗にする */
        return;
    }
    int i = nasm++;
    asm_op[i] = (char)op;
    asm_rd[i] = (char)rd;
    asm_rs1[i] = (char)rs1;
    asm_rs2[i] = (char)rs2;
    asm_imm[i] = imm;
}

/* --- 条件分岐の届く範囲 (v27) ---
 * if / while / && などの条件分岐は「t1 が 0 なら飛ぶ」
 * beq で置くが、B 形式の届く範囲は前後 約4KB しかない。
 * 大きな関数では飛び先が範囲の外になり、オフセットが
 * 黙って丸まって無関係な場所へ飛んでしまう (セルフ
 * コンパイルで expand_defines のループ脱出が壊れて発覚)。
 * そこで、届かない分岐は次の2命令の形に伸ばす:
 *   bne t1, zero, 次   (0 でないなら jal を飛び越える)
 *   jal zero, 飛び先   (jal は前後 約1MB 届く)
 * 伸ばすと後ろの命令がずれて別の分岐が届かなくなる
 * ことがあるので、gen_program が「生成 → 点検」を
 * 繰り返し、届かない分岐が無くなるまで印を増やす。
 * 印は条件分岐の通し番号 (branch_ord) で管理する。
 * 生成は決定的なので、通し番号は生成のたびに同じ分岐を
 * 指す。届く分岐は従来どおり beq 1命令のままなので、
 * 今まで正しく作れていたプログラムの出力は変わらない */
static int branch_ord;         /* 条件分岐の通し番号 */
static char branch_long[24576]; /* 伸ばす分岐の印 */

/* 条件分岐の命令か (飛び先は asm_imm の命令番号) */
static int is_cbranch(int op) {
    return op == OP_BEQ || op == OP_BNE || op == OP_BLT ||
           op == OP_BGE || op == OP_BLTU || op == OP_BGEU;
}

/* 分岐の反対 (長い形で jal を飛び越える側に使う) */
static int branch_inv(int op) {
    if (op == OP_BEQ)
        return OP_BNE;
    if (op == OP_BNE)
        return OP_BEQ;
    if (op == OP_BLT)
        return OP_BGE;
    if (op == OP_BGE)
        return OP_BLT;
    if (op == OP_BLTU)
        return OP_BGEU;
    return OP_BLTU;
}

/* 条件分岐を置く。戻り値は、飛び先 (命令番号) を
 * asm_imm に書き込むべき命令の番号: 短い形なら
 * 分岐自身、長い形なら後ろの jal の方 */
static int emit_branch(int op, int rs1, int rs2) {
    int ord = branch_ord++;
    if (branch_long[ord]) {
        asm_push(branch_inv(op), 0, rs1, rs2, nasm + 2);
        int j = nasm;
        asm_push(OP_JAL, 0, 0, 0, 0);
        return j;
    }
    int b = nasm;
    asm_push(op, 0, rs1, rs2, 0);
    return b;
}

/* 「t1 が 0 なら飛ぶ」条件分岐を置く (emit_branch の
 * 特殊形。戻り値の意味は同じ) */
static int emit_beq_t1z(void) {
    return emit_branch(OP_BEQ, REG_T1, REG_ZERO);
}

/* 生成し終えた命令列を眺め、範囲 (±4KB) の外へ飛ぶ
 * 条件分岐があれば branch_long に印を付ける。
 * 新しい印が付いたら 1 を返す。長い形かどうかは
 * branch_long の印で知る (v53 で一般化: 以前は
 * 「OP_BNE なら長い形の相方」という決め打ちだった) */
static int relax_scan(void) {
    int ord = 0;
    int changed = 0;
    for (int i = 0; i < nasm; i++) {
        if (!is_cbranch(asm_op[i]))
            continue;
        if (branch_long[ord]) {
            i++;  /* 長い形: 相方の jal も読み飛ばす */
            ord++;
            continue;
        }
        int off = (asm_imm[i] - i) * 4;
        if ((off < -4096 || off > 4092) && !branch_long[ord]) {
            branch_long[ord] = 1;
            changed = 1;
        }
        ord++;
    }
    return changed;
}

/* --- 変数の置き場所 (v21) ---
 * 変数は全部メモリに住む。グローバルは静的領域 (文字列と
 * 配列の後ろ、globals_base から 8 バイトずつ)、ローカルと
 * 引数は関数のフレームの中 (fp 相対)。フレームの形:
 *   入口で sp をフレーム長だけ引き、ra を一番上 (fp-8)、
 *   呼び出し側の fp をその下 (fp-16) に退避し、fp を張る。
 *   枠 k の住所は sp(引き後) + 8k = fp - フレーム長 + 8k。
 *   ローカル配列 (v26) は枠の並びの上 (fp-16 の下) に
 *   置く: 先頭は sp(引き後) + 枠数 x 8 + arr_foff。
 * フレーム長は「16 + 退避する s の本数 x 8 (v56) + 枠数 x 8
 * + ローカル配列の合計」を16バイト単位に切り上げたもの。
 * s の退避場所は fp-24 から下向きで、枠のずれは全部
 * フレーム長から計算されるので自動で揃う */
static unsigned int globals_base;  /* gen_program が確定させる */

static int var_value_etype(int var);  /* 定義は下 (v56 の
                                       * 計画が先に使う) */

/* ================================================================
 * レジスタ割り付け (v56、設計は doc/regalloc.md)
 *
 * 参照の多いスカラのローカル変数と引数を、callee-saved の
 * s レジスタに割り付ける。割り付けた変数はフレームでなく
 * レジスタが住み家になり、gen_load_var / gen_store_var が
 * ld / sd の代わりに move を出す。フレームの枠は従来どおり
 * 確保したままにする (住所を取る変数は割り付けないので、
 * 枠が表に出ることはない)。
 *
 * 対象外 (安全側の割り切り):
 *   住所を取られる変数 (&var が関数内にあれば丸ごと外す。
 *   ポインタ越しの読み書きと競合させないため)
 *   構造体の値そのものの変数、double の変数
 *   参照が 4 回に満たない変数 (退避・復帰と相殺される)
 * 選び方は参照回数の多い順、同数なら変数番号の小さい順で
 * 最大11個。割り付けは AST の純粋関数なので、生成の
 * 2パスと relax の再生成で結果がブレない。
 * フレームは退避する s レジスタの分だけ広がり、退避場所は
 * fp-24 から下向き (ra と fp の退避場所の下)。
 * ================================================================ */
static char var_reg[1280];       /* 変数 → レジスタ番号。
                                  * 0 は割り付けなし。
                                  * 番号は 27 までなので
                                  * char に収まる (置き場の
                                  * 節約。断片プロファイルの
                                  * 1MB 枠が逼迫している) */
static char func_saved_regs[288]; /* 関数 → 割り付けた数
                                  * (= 退避する s の本数。
                                  * 最大11なので char) */
static char reg_plan_done[288];  /* 計画済みの印 */
static char var_refcount[1280];  /* 計画専用の数え置き場。
                                  * 127 で飽和させる (順位付け
                                  * に要るのは大きな方の
                                  * 桁だけなので、char で足りる) */
static char var_addrtaken[1280]; /* 計画専用: & を取られた印 */

/* 割り付け順 i (0〜10) に対応するレジスタ番号:
 * s1 (x9)、s2〜s11 (x18〜x27) */
static int saved_reg_no(int i) {
    if (i == 0)
        return REG_S1;
    return 17 + i;
}

/* 参照回数を1つ増やす (127 で飽和。var_refcount が
 * char のため) */
static void reg_count_up(int v) {
    if (var_refcount[v] < 127)
        var_refcount[v] = var_refcount[v] + 1;
}

/* 本体の AST を歩き、変数の参照回数と「住所を取られた」
 * 印を集める。辿る欄は種類ごとに決める (使わない欄には
 * 0 が入っていて、辿ると実在の葉であるノード0 に
 * 吸い寄せられ、数えすぎる)。文と引数の並びは
 * node_next で繋がっているので、最後に必ず辿る */
static void reg_walk(int node) {
    if (node < 0)
        return;
    int k = node_kind[node];
    if (k == NODE_VAR || k == NODE_PINDEX)
        /* PINDEX はポインタ変数が val に入る形で、
         * その読みも参照として数える */
        reg_count_up(node_val[node]);
    if (k == NODE_ASSIGN || k == NODE_PREINC ||
        k == NODE_POSTINC || k == NODE_PREDEC ||
        k == NODE_POSTDEC)
        /* 代入先と ++ / -- の相手はノードの val に
         * 変数が入る形で、NODE_VAR の子にはならない。
         * 書き込みも参照として数える */
        reg_count_up(node_val[node]);
    if (k == NODE_ADDR) {
        int operand = node_lhs[node];
        if (operand >= 0 && node_kind[operand] == NODE_VAR)
            var_addrtaken[node_val[operand]] = 1;
    }
    if (k == NODE_NUM || k == NODE_VAR || k == NODE_STR ||
        k == NODE_ARRAY || k == NODE_FADDR ||
        k == NODE_NUMFP || k == NODE_BREAK ||
        k == NODE_CONTINUE || k == NODE_DECL ||
        k == NODE_VDECL || k == NODE_PREINC ||
        k == NODE_POSTINC || k == NODE_PREDEC ||
        k == NODE_POSTDEC) {
        /* 葉と宣言: 子の欄は使われていない */
    } else if (k == NODE_DEREF || k == NODE_ASSIGN ||
               k == NODE_PUTC || k == NODE_CALL ||
               k == NODE_NOT || k == NODE_NEG ||
               k == NODE_RETURN || k == NODE_ADDR ||
               k == NODE_EPRE || k == NODE_EPOST ||
               k == NODE_BITNOT || k == NODE_SYSCALL3 ||
               k == NODE_CAST || k == NODE_MEMBER ||
               k == NODE_CASE) {
        reg_walk(node_lhs[node]);
    } else if (k == NODE_INDEX || k == NODE_ROWADDR ||
               k == NODE_PINDEX) {
        reg_walk(node_rhs[node]);
    } else if (k == NODE_FORSTEP) {
        reg_walk(node_val[node]);
    } else if (k == NODE_IF || k == NODE_FOR ||
               k == NODE_COND) {
        /* val も子のリンク (else / 初期化 / 三項の枝) */
        reg_walk(node_lhs[node]);
        reg_walk(node_rhs[node]);
        reg_walk(node_val[node]);
    } else {
        /* 二項演算・while / do-while・メモリ書き込み・
         * 要素の読み書き・switch・間接呼び出し:
         * lhs と rhs が子 */
        reg_walk(node_lhs[node]);
        reg_walk(node_rhs[node]);
    }
    reg_walk(node_next[node]);
}

/* 変数 v が関数 f の割り付け候補かどうか */
static int reg_eligible(int v, int f) {
    if (var_owner[v] != f)
        return 0;
    if (var_refcount[v] < 4)
        return 0;
    if (var_addrtaken[v])
        return 0;
    if (var_struct[v] > 0 && var_plevel[v] == 0)
        return 0;  /* 構造体の値そのもの */
    if (var_value_etype(v) & 64)
        return 0;  /* double */
    return 1;
}

/* ================================================================
 * ループ最適化の計画 (v57、設計は doc/loop-opt.md)
 *
 * ループの中で、帰納変数 (定数歩幅でだけ変わる
 * スカラ変数) を添字にするアクセス a[i] / p[i] を
 * 見つけ、ループ入口で「歩くポインタ」を作る計画を
 * 立てる。ポインタは帰納変数の変更箇所で歩幅ぶん
 * 進めるので、本体のアクセスはポインタ + 定数
 * オフセットで済み、添字の拡大 (slli) と基底との
 * 加算が消える。
 *
 * 計画は AST の純粋関数で、割り付け計画 (v56) の
 * 末尾で作る。ポインタの置き場は s レジスタで、
 * 割り付けが使わなかった分を、ループの入れ子の
 * 深さに合わせて使う。退避本数は func_saved_regs へ
 * 合算される (割り付け分より増えることがある)。
 * 割り付けが使い切っているときは、本体で使われて
 * いない変数を退避して席を貸す (v62、設計は
 * doc/reg-pressure.md)。
 *
 * 安全の要: 帰納変数の変更は、ループの部分木の中の
 * 全部の箇所が「定数歩幅の代入と ++ / --」の形で
 * あるときだけ計画する。1つでも違う形があれば、
 * その変数は帰納変数にしない。変更箇所は
 * (ノード・変数・変化量) の事実として記録し、
 * 生成側は変更の生成直後にポインタを進める。
 * 記録はループが無効になっても残す (事実としては
 * 正しく、ポインタが無い変数では使われない)。
 * ================================================================ */
static int xform_loop_node[256];  /* ループのノード */
static int xform_loop_first[256]; /* ポインタ表の先頭 */
static char xform_loop_cnt[256];  /* ポインタの数 */
static int xform_nloops;
static int xform_ptr_base[512];   /* 基底 (配列番号か
                                   * ポインタ変数の番号) */
static char xform_ptr_kind[512];  /* 0 = 配列、1 = 変数 */
static int xform_ptr_ind[512];    /* 帰納変数 */
static char xform_ptr_esz[512];   /* 要素の大きさ */
static char xform_ptr_slot[512];  /* ポインタの割り付け順
                                   * (v62: 末尾の空き席と
                                   * 貸し出し席が混ざる
                                   * ので、ポインタごとに
                                   * 持つ) */
static int xform_nptrs;
/* レジスタ圧の緩和 (v62、設計は doc/reg-pressure.md):
 * 割り付けが 11 本を使い切っているループでは、
 * 本体で使われていない割り付け済み変数を入口で
 * 自分の置き場所へ退避し、その席をポインタに貸す。
 * 被害変数はループごとに貯める */
static int xform_loop_vfirst[256]; /* 被害変数表の先頭 */
static char xform_loop_vcnt[256];  /* 被害変数の数 */
static int xform_vict_var[256];    /* 被害変数の番号 */
static int xform_nvict;
static int xform_mod_node[1024];  /* 変更箇所のノード */
static int xform_mod_var[1024];   /* 帰納変数 */
static int xform_mod_delta[1024]; /* 値の変化量 */
static int xform_nmods;

/* 要素の大きさ (本体は後ろにある) */
static int elem_esize(int node);

/* 調べ置き場 (候補ごとに使い回す) */
static int scan_var;              /* 対象の帰納変数。
                                   * -1 は候補集め */
static int scan_bad;              /* 候補を落とす印 */
static int scan_nmod;
static int scan_mnode[128];
static int scan_mdelta[128];
static int scan_nbase;
static char scan_bkind[8];
static int scan_bid[8];
static char scan_besz[8];
static int scan_cand[24];
static int scan_ncand;

/* トレードオフの見回り用: ループの中で参照される
 * 変数の印 (ビット集合。変数表 1280 件分でも
 * 160 バイトで収まる)。scan_mark_hot が立って
 * いる間の loop_scan が立てる */
static char evict_hot_bits[160];
static int scan_mark_hot;
/* v62 の被害変数の見回りだけ、PINDEX (ポインタ変数
 * の読み) も熱いものとして数える印。v59 の外し
 * 判定の挙動は変えないよう、見回り側で立てる */
static int scan_mark_pindex;

static int evict_is_hot(int v) {
    if (v < 0 || v >= 1280)
        return 0;
    return (evict_hot_bits[v >> 3] >> (v & 7)) & 1;
}

/* 変更される変数を候補として控える (取りこぼしは
 * 安全側: 最適化を諦めるだけ) */
static void scan_add_cand(int v) {
    for (int i = 0; i < scan_ncand; i++) {
        if (scan_cand[i] == v)
            return;
    }
    if (scan_ncand >= 24)
        return;
    scan_cand[scan_ncand] = v;
    scan_ncand = scan_ncand + 1;
}

/* 変更箇所をステージする。変化量が大きすぎる、
 * または箇所が多すぎる候補は落とす */
static void scan_add_mod(int node, int delta) {
    if (delta > 268435456 || delta < -268435456) {
        scan_bad = 1;
        return;
    }
    if (scan_nmod >= 128) {
        scan_bad = 1;
        return;
    }
    scan_mnode[scan_nmod] = node;
    scan_mdelta[scan_nmod] = delta;
    scan_nmod = scan_nmod + 1;
}

/* 基底をステージする (重複は寄せない) */
static void scan_add_base(int kind, int id, int esz) {
    for (int i = 0; i < scan_nbase; i++) {
        if (scan_bkind[i] == kind && scan_bid[i] == id)
            return;
    }
    if (scan_nbase >= 8) {
        scan_bad = 1;
        return;
    }
    scan_bkind[scan_nbase] = kind;
    scan_bid[scan_nbase] = id;
    scan_besz[scan_nbase] = esz;
    scan_nbase = scan_nbase + 1;
}

/* 添字の形が「帰納変数」「帰納変数 + 定数」
 * 「帰納変数 - 定数」なら 1 を返し、定数の分を
 * *off に入れる */
static int scan_index_form(int idx, int *off) {
    if (node_kind[idx] == NODE_VAR &&
        node_val[idx] == scan_var) {
        *off = 0;
        return 1;
    }
    if (node_kind[idx] == NODE_ADD) {
        int l = node_lhs[idx];
        int r = node_rhs[idx];
        if (node_kind[l] == NODE_VAR &&
            node_val[l] == scan_var &&
            node_kind[r] == NODE_NUM) {
            *off = node_val[r];
            return 1;
        }
        if (node_kind[r] == NODE_VAR &&
            node_val[r] == scan_var &&
            node_kind[l] == NODE_NUM) {
            *off = node_val[l];
            return 1;
        }
    }
    if (node_kind[idx] == NODE_SUB) {
        int l = node_lhs[idx];
        if (node_kind[l] == NODE_VAR &&
            node_val[l] == scan_var &&
            node_kind[node_rhs[idx]] == NODE_NUM) {
            *off = -node_val[node_rhs[idx]];
            return 1;
        }
    }
    return 0;
}

/* NODE_ASSIGN の右辺が「帰納変数 ± 定数」の形か
 * 見て、合えば変更箇所としてステージする。
 * 合わなければ候補を落とす */
static void scan_assign(int node) {
    int rhs = node_lhs[node];
    int k = node_kind[rhs];
    if (k == NODE_ADD) {
        int l = node_lhs[rhs];
        int r = node_rhs[rhs];
        if (node_kind[l] == NODE_VAR &&
            node_val[l] == scan_var &&
            node_kind[r] == NODE_NUM) {
            scan_add_mod(node, node_val[r]);
            return;
        }
        if (node_kind[r] == NODE_VAR &&
            node_val[r] == scan_var &&
            node_kind[l] == NODE_NUM) {
            scan_add_mod(node, node_val[l]);
            return;
        }
    }
    if (k == NODE_SUB) {
        int l = node_lhs[rhs];
        if (node_kind[l] == NODE_VAR &&
            node_val[l] == scan_var &&
            node_kind[node_rhs[rhs]] == NODE_NUM) {
            scan_add_mod(node, -node_val[node_rhs[rhs]]);
            return;
        }
    }
    scan_bad = 1;
}

/* ループの部分木を歩く。候補集め (scan_var = -1)
 * では変更される変数を集め、候補が決まった後は
 * 変更箇所の検証とステージ、基底集めを同時に行う。
 * 辿る欄の規則は reg_walk と同じ (使わない欄の 0 を
 * 辿ると、実在の葉であるノード0 に吸い寄せられる) */
static void loop_scan(int node) {
    if (node < 0)
        return;
    int k = node_kind[node];
    if (scan_mark_hot &&
        (k == NODE_VAR || k == NODE_ASSIGN ||
         k == NODE_PREINC || k == NODE_POSTINC ||
         k == NODE_PREDEC || k == NODE_POSTDEC ||
         (k == NODE_PINDEX && scan_mark_pindex))) {
        /* 見回り中: この部分木 (= ループの中) で
         * 参照・変更される変数に印を付ける */
        int hv = node_val[node];
        if (hv >= 0 && hv < 1280)
            evict_hot_bits[hv >> 3] =
                (char)(evict_hot_bits[hv >> 3] |
                       (1 << (hv & 7)));
    }
    if (k == NODE_ASSIGN) {
        if (scan_var < 0)
            scan_add_cand(node_val[node]);
        else if (node_val[node] == scan_var)
            scan_assign(node);
    }
    if (k == NODE_PREINC || k == NODE_POSTINC ||
        k == NODE_PREDEC || k == NODE_POSTDEC) {
        if (scan_var < 0)
            scan_add_cand(node_val[node]);
        else if (node_val[node] == scan_var) {
            if (k == NODE_PREINC || k == NODE_POSTINC)
                scan_add_mod(node, 1);
            else
                scan_add_mod(node, -1);
        }
    }
    if (scan_var >= 0 && k == NODE_INDEX) {
        int off = 0;
        if (arr_cols[node_val[node]] == 0 &&
            scan_index_form(node_rhs[node], &off))
            scan_add_base(0, node_val[node],
                          arr_esize[node_val[node]]);
    }
    if (scan_var >= 0 && k == NODE_PINDEX) {
        int off = 0;
        if (scan_index_form(node_rhs[node], &off))
            scan_add_base(1, node_val[node],
                          elem_esize(node));
    }
    if (scan_var >= 0 && k == NODE_INDEX_STORE) {
        int off = 0;
        if (arr_cols[node_val[node]] == 0 &&
            scan_index_form(node_lhs[node], &off))
            scan_add_base(0, node_val[node],
                          arr_esize[node_val[node]]);
    }
    if (k == NODE_NUM || k == NODE_VAR || k == NODE_STR ||
        k == NODE_ARRAY || k == NODE_FADDR ||
        k == NODE_NUMFP || k == NODE_BREAK ||
        k == NODE_CONTINUE || k == NODE_DECL ||
        k == NODE_VDECL || k == NODE_PREINC ||
        k == NODE_POSTINC || k == NODE_PREDEC ||
        k == NODE_POSTDEC) {
        /* 葉と宣言: 子の欄は使われていない */
    } else if (k == NODE_DEREF || k == NODE_ASSIGN ||
               k == NODE_PUTC || k == NODE_CALL ||
               k == NODE_NOT || k == NODE_NEG ||
               k == NODE_RETURN || k == NODE_ADDR ||
               k == NODE_EPRE || k == NODE_EPOST ||
               k == NODE_BITNOT || k == NODE_SYSCALL3 ||
               k == NODE_CAST || k == NODE_MEMBER ||
               k == NODE_CASE) {
        loop_scan(node_lhs[node]);
    } else if (k == NODE_INDEX || k == NODE_ROWADDR ||
               k == NODE_PINDEX) {
        loop_scan(node_rhs[node]);
    } else if (k == NODE_FORSTEP) {
        loop_scan(node_val[node]);
    } else if (k == NODE_IF || k == NODE_FOR ||
               k == NODE_COND) {
        loop_scan(node_lhs[node]);
        loop_scan(node_rhs[node]);
        loop_scan(node_val[node]);
    } else {
        loop_scan(node_lhs[node]);
        loop_scan(node_rhs[node]);
    }
    loop_scan(node_next[node]);
}

/* 変数 v が帰納変数の候補かどうか */
static int loop_ind_eligible(int v, int f) {
    if (var_owner[v] != f)
        return 0;
    if (var_plevel[v] != 0)
        return 0;  /* ポインタ変数は ++ の歩幅が
                    * 値の変化量と一致しない */
    if (var_struct[v] > 0)
        return 0;  /* 構造体の値 */
    if (var_value_etype(v) & 64)
        return 0;  /* double */
    if (var_addrtaken[v])
        return 0;
    return 1;
}

/* ループ1つのポインタ表のステージ */
static int st_np;
static char st_pkind[24];
static int st_pbase[24];
static int st_pind[24];
static char st_pesz[24];
static int plan_f;

/* ループ (node) を調べ、ポインタの組をステージに
 * 寄せて数を返す。変更箇所の記録はその場で
 * 全体表へ入れる */
static int loop_plan_one(int node) {
    st_np = 0;
    scan_var = -1;
    scan_ncand = 0;
    loop_scan(node_lhs[node]);
    loop_scan(node_rhs[node]);
    for (int ci = 0; ci < scan_ncand; ci++) {
        int v = scan_cand[ci];
        if (!loop_ind_eligible(v, plan_f))
            continue;
        scan_var = v;
        scan_bad = 0;
        scan_nmod = 0;
        scan_nbase = 0;
        loop_scan(node_lhs[node]);
        loop_scan(node_rhs[node]);
        if (scan_bad || scan_nmod == 0 ||
            scan_nbase == 0)
            continue;
        if (xform_nmods + scan_nmod > 1024)
            continue;  /* 記録が入り切らない候補は
                        * 諦める (安全側) */
        for (int m = 0; m < scan_nmod; m++) {
            int found = 0;
            for (int e = 0; e < xform_nmods; e++) {
                if (xform_mod_node[e] == scan_mnode[m] &&
                    xform_mod_var[e] == v &&
                    xform_mod_delta[e] == scan_mdelta[m])
                    found = 1;
            }
            if (!found) {
                xform_mod_node[xform_nmods] =
                    scan_mnode[m];
                xform_mod_var[xform_nmods] = v;
                xform_mod_delta[xform_nmods] =
                    scan_mdelta[m];
                xform_nmods = xform_nmods + 1;
            }
        }
        for (int b = 0; b < scan_nbase; b++) {
            int found = 0;
            for (int j = 0; j < st_np; j++) {
                if (st_pkind[j] == scan_bkind[b] &&
                    st_pbase[j] == scan_bid[b] &&
                    st_pind[j] == v)
                    found = 1;
            }
            if (!found && st_np < 24) {
                st_pkind[st_np] = scan_bkind[b];
                st_pbase[st_np] = scan_bid[b];
                st_pind[st_np] = v;
                st_pesz[st_np] = scan_besz[b];
                st_np = st_np + 1;
            }
        }
    }
    return st_np;
}

/* 文の並びを歩いてループを探す。enclosing は
 * 外側の有効なループが末尾の空き席に使っている
 * ポインタ数、lent_mask は外側のループが貸し出し
 * 中の席の印 (割り付け順のビット集合。見回りでは
 * 使わない) */
static int plan_assigned;
static int plan_max_end;
/* レジスタートレードオフ (v59): 見回りの状態。
 * 見回り中はポインタ表へ入れず、予算で落ちる
 * ループの不足分 (survey_need) だけを数える */
static int plan_survey;
static int survey_need;

/* レジスタ番号から割り付け順 (0〜10) へ戻す */
static int reg_slot_index(int reg) {
    if (reg == REG_S1)
        return 0;
    return reg - 17;
}

/* ループ (node) の中で参照される割り付け済み変数の
 * 席の印を返す (v62)。走査は loop_scan の見回りの
 * 印 (evict_hot_bits) を使い回す。歩くポインタの
 * 材料 (帰納変数と、基底になるポインタ変数) は
 * 入口の初期化で読むので、熱いものとして足す */
static int plan_loop_hotmask(int node) {
    for (int b = 0; b < 160; b++)
        evict_hot_bits[b] = 0;
    scan_mark_hot = 1;
    scan_mark_pindex = 1;
    loop_scan(node_lhs[node]);
    loop_scan(node_rhs[node]);
    scan_mark_hot = 0;
    scan_mark_pindex = 0;
    int mask = 0;
    for (int v = 0; v < nvar; v++) {
        if (var_owner[v] != plan_f || var_reg[v] == 0)
            continue;
        if (evict_is_hot(v))
            mask = mask | (1 << reg_slot_index(var_reg[v]));
    }
    for (int j = 0; j < st_np; j++) {
        if (var_reg[st_pind[j]] != 0)
            mask = mask |
                (1 << reg_slot_index(var_reg[st_pind[j]]));
        if (st_pkind[j] == 1 &&
            var_reg[st_pbase[j]] != 0)
            mask = mask |
                (1 << reg_slot_index(var_reg[st_pbase[j]]));
    }
    return mask;
}

static void loop_plan_visit(int head, int enclosing,
                            int lent_mask) {
    for (int s = head; s != -1; s = node_next[s]) {
        int k = node_kind[s];
        if (k == NODE_IF) {
            loop_plan_visit(node_rhs[s], enclosing,
                            lent_mask);
            loop_plan_visit(node_val[s], enclosing,
                            lent_mask);
        } else if (k == NODE_SWITCH) {
            for (int g = node_rhs[s]; g != -1;
                 g = node_next[g])
                loop_plan_visit(node_lhs[g], enclosing,
                                lent_mask);
        } else if (k == NODE_WHILE ||
                   k == NODE_DOWHILE ||
                   k == NODE_FOR) {
            int np = loop_plan_one(s);
            int add_tail = 0;
            int add_mask = 0;
            if (plan_survey) {
                /* 見回り (v59): 不足分 = そのループと
                 * 祖先の np の合計 - 空き席、の最大値。
                 * enclosing は祖先の np の合計として
                 * 渡ってくる (上限の見積もり) */
                if (np > 0) {
                    int need = enclosing + np -
                        (11 - plan_assigned);
                    if (need > survey_need)
                        survey_need = need;
                    add_tail = np;
                }
            } else if (np > 0 &&
                xform_nloops < 256 &&
                xform_nptrs + np <= 512) {
                /* レジスタ圧の緩和 (v62): 末尾の空き席
                 * で足りなければ、本体で使われて
                 * いない割り付け済み変数を最大3個
                 * 退避して席を貸す */
                int freetail = 11 - plan_assigned -
                    enclosing;
                int tail_take = np;
                if (tail_take > freetail)
                    tail_take = freetail;
                int need = np - tail_take;
                int vict[3];
                int nvict = 0;
                int ok = 1;
                if (need > 3)
                    ok = 0;
                if (ok && need > 0) {
                    int hot = plan_loop_hotmask(s);
                    while (nvict < need) {
                        int best = -1;
                        for (int v = 0; v < nvar; v++) {
                            if (var_owner[v] != plan_f ||
                                var_reg[v] == 0)
                                continue;
                            int si =
                                reg_slot_index(var_reg[v]);
                            if (si < 0 || si > 10)
                                continue;
                            if ((hot >> si) & 1)
                                continue;
                            if ((lent_mask >> si) & 1)
                                continue;
                            if ((add_mask >> si) & 1)
                                continue;
                            if (best < 0 ||
                                var_refcount[v] <
                                    var_refcount[best])
                                best = v;
                        }
                        if (best < 0) {
                            ok = 0;
                            break;
                        }
                        vict[nvict] = best;
                        nvict = nvict + 1;
                        add_mask = add_mask |
                            (1 << reg_slot_index(
                                 var_reg[best]));
                    }
                    if (xform_nvict + nvict > 256)
                        ok = 0;
                }
                if (ok) {
                    int li = xform_nloops;
                    xform_nloops = xform_nloops + 1;
                    xform_loop_node[li] = s;
                    xform_loop_first[li] = xform_nptrs;
                    xform_loop_cnt[li] = np;
                    xform_loop_vfirst[li] = xform_nvict;
                    xform_loop_vcnt[li] = nvict;
                    for (int j = 0; j < np; j++) {
                        xform_ptr_kind[xform_nptrs] =
                            st_pkind[j];
                        xform_ptr_base[xform_nptrs] =
                            st_pbase[j];
                        xform_ptr_ind[xform_nptrs] =
                            st_pind[j];
                        xform_ptr_esz[xform_nptrs] =
                            st_pesz[j];
                        if (j < tail_take)
                            xform_ptr_slot[xform_nptrs] =
                                plan_assigned +
                                enclosing + j;
                        else
                            xform_ptr_slot[xform_nptrs] =
                                reg_slot_index(var_reg[
                                    vict[j - tail_take]]);
                        xform_nptrs = xform_nptrs + 1;
                    }
                    for (int j = 0; j < nvict; j++) {
                        xform_vict_var[xform_nvict] =
                            vict[j];
                        xform_nvict = xform_nvict + 1;
                    }
                    if (plan_assigned + enclosing +
                        tail_take > plan_max_end)
                        plan_max_end = plan_assigned +
                            enclosing + tail_take;
                    add_tail = tail_take;
                } else
                    add_mask = 0;
            }
            loop_plan_visit(node_rhs[s],
                            enclosing + add_tail,
                            lent_mask | add_mask);
        }
    }
}

/* 関数 f のループ最適化の計画。割り付け (assigned 本)
 * の後ろの s レジスタをポインタに使い、退避本数が
 * 増えたら func_saved_regs へ反映する */
static void loop_plan(int f, int assigned) {
    plan_f = f;
    plan_assigned = assigned;
    plan_max_end = assigned;
    loop_plan_visit(func_body[f], 0, 0);
    if (plan_max_end > assigned)
        func_saved_regs[f] = plan_max_end;
}

/* インライン化 (v58): 本体は 03-geninl.c。
 * func_reg_plan の頭で計画し、gen_expr の呼び出しで
 * 展開する (設計は doc/inline.md) */
static void inline_plan(int f);
static int inline_find(int node);
static int gen_inline_call(int node, int ie);

/* 関数 f の割り付け計画 (一度だけ作り、使い回す) */
static void func_reg_plan(int f) {
    if (f < 0 || reg_plan_done[f])
        return;
    reg_plan_done[f] = 1;
    /* インライン化 (v58): 複製変数でフレームが
     * 増えるので、割り付けより先に計画する */
    inline_plan(f);
    func_saved_regs[f] = 0;
    if (func_body[f] < 0)
        return;  /* 宣言だけの関数 */
    int v;
    for (v = 0; v < nvar; v++) {
        if (var_owner[v] == f) {
            var_refcount[v] = 0;
            var_addrtaken[v] = 0;
            var_reg[v] = 0;
        }
    }
    reg_walk(func_body[f]);
    /* 参照の多い順に最大11個。同数なら変数番号の
     * 小さい方を先に (決定的にするため) */
    int assigned = 0;
    while (assigned < 11) {
        int best = -1;
        for (v = 0; v < nvar; v++) {
            if (!reg_eligible(v, f) || var_reg[v] != 0)
                continue;
            if (best < 0 ||
                var_refcount[v] > var_refcount[best])
                best = v;
        }
        if (best < 0)
            break;
        var_reg[best] = saved_reg_no(assigned);
        assigned = assigned + 1;
    }
    func_saved_regs[f] = assigned;
    /* レジスタートレードオフ (v59、設計は
     * doc/reg-tradeoff.md): 割り付けが場所を
     * 使い切って歩くポインタが入らないとき、
     * 帰納変数ではない割り付けを参照の少ない
     * 順に最大2個外し、席を詰め直してポインタに
     * 明け渡す。まず見回りで不足分を数える
     * (変更箇所の記録は見回りの副作用で残り、
     * 外す相手の判定に使う) */
    if (assigned > 0) {
        int mark = xform_nmods;
        plan_f = f;
        plan_assigned = assigned;
        plan_survey = 1;
        survey_need = 0;
        for (int b = 0; b < 160; b++)
            evict_hot_bits[b] = 0;
        scan_mark_hot = 1;
        loop_plan_visit(func_body[f], 0, 0);
        scan_mark_hot = 0;
        plan_survey = 0;
        if (survey_need > 0) {
            int want = survey_need;
            if (want > 2)
                want = 2;
            int evicted = 0;
            while (evicted < want) {
                int best = -1;
                for (v = 0; v < nvar; v++) {
                    if (var_owner[v] != f ||
                        var_reg[v] == 0)
                        continue;
                    /* 帰納変数は外さない (ループの
                     * 中で毎周触るので、メモリへ
                     * 落とす損がポインタの得を
                     * 食い潰す) */
                    int is_ind = 0;
                    for (int e = mark;
                         e < xform_nmods; e++) {
                        if (xform_mod_var[e] == v)
                            is_ind = 1;
                    }
                    if (is_ind)
                        continue;
                    /* ループの中で参照される変数も
                     * 外さない (v59 の実測の教訓:
                     * 静的な参照回数が少なくても、
                     * 参照がループの中にあれば動的な
                     * 交通が多く、メモリへ落とす損が
                     * ポインタの得を食い潰す。
                     * doc/reg-tradeoff.md の実測節) */
                    if (evict_is_hot(v))
                        continue;
                    if (best < 0 ||
                        var_refcount[v] <
                            var_refcount[best] ||
                        (var_refcount[v] ==
                             var_refcount[best] &&
                         var_reg[v] > var_reg[best]))
                        best = v;
                }
                if (best < 0)
                    break;
                var_reg[best] = 0;
                evicted = evicted + 1;
            }
            if (evicted > 0) {
                /* 席を詰め直す: 序文・終文は
                 * saved_reg_no(0..本数-1) を連続で
                 * 退避する決まりなので、穴を残せない。
                 * レジスタ番号の大小は割り付け順と
                 * 一致する (s1=9、s2..s11=18..27) ので、
                 * 番号の小さい順に詰めれば順序が保てる */
                int slot = 0;
                for (int s2 = 0; s2 < assigned; s2++) {
                    for (v = 0; v < nvar; v++) {
                        if (var_owner[v] == f &&
                            var_reg[v] ==
                                saved_reg_no(s2)) {
                            var_reg[v] =
                                saved_reg_no(slot);
                            slot = slot + 1;
                        }
                    }
                }
                assigned = slot;
                func_saved_regs[f] = assigned;
            }
        }
    }
    /* ループ最適化 (v57): 歩くポインタの計画。
     * 退避本数が増えることがある */
    loop_plan(f, assigned);
}

static int func_frame_size(int f) {
    func_reg_plan(f);
    int size = 16 + func_saved_regs[f] * 8 +
               func_nlocals[f] * 8 + func_arr_bytes[f];
    return (size + 15) & ~15;
}

static int var_frame_off(int var) {
    return var_slot[var] * 8 - func_frame_size(var_owner[var]);
}

static unsigned int var_global_addr(int var) {
    return globals_base + (unsigned int)var_slot[var] * 8u;
}

/* 葉かどうか: 式の中で「そのまま値として使える材料」は
 * 数・変数・文字列 (住所)・ポインタ読みの4種。ただし
 * ポインタ読みは、相手 (住所) が材料そのもののときだけ
 * 葉扱いする。相手が計算式の場合は、住所の計算に t1 を
 * 使うので、葉としては扱えない (gen_expr で別に処理する) */
static int is_leaf(int node) {
    int k = node_kind[node];
    if (k == NODE_NUM || k == NODE_NUMFP || k == NODE_VAR ||
        k == NODE_STR || k == NODE_ARRAY)
        return 1;
    if (k == NODE_DEREF) {
        int ok = node_kind[node_lhs[node]];
        return ok == NODE_NUM || ok == NODE_VAR || ok == NODE_STR ||
               ok == NODE_ARRAY;
    }
    return 0;
}

/* 文字列の住所をレジスタに作る命令 (2つ) を積む。
 *
 * 最初は lui + addi (絶対アドレス) で作ろうとして失敗した:
 * RV64 の lui は、作った32ビットの値を符号拡張して64ビット
 * にする。0x8020001c のような住所は32ビット目が立っている
 * ため、0xffffffff8020001c という「変な住所」になり、
 * lbu が例外 (load fault) になった (QEMU の例外ログで確認)。
 *
 * そこで auipc (PC 相対) を使う。「今いる命令の住所 + 差分」
 * で住所を作るので、差分は小さく、符号拡張に邪魔されない。
 * KosabaOS の C 版が -mcmodel=medany で auipc を使うのも
 * 同じ理由である。
 *   auipc reg, hi   -- reg = 自分の住所 + (hi << 12)
 *   addi  reg, reg, lo
 * 差分の分け方は addi の符号付き即値に合わせる定石:
 * hi = (差分 + 0x800) >> 12、lo = 差分 - (hi << 12) */
static unsigned int LOAD_BASE = 0x80200000u;  /* 配置先頭 (下記) */
static int code_insts;  /* 1回目の生成で数えた命令の総数 */

/* 出力ターゲット (v14): 0 = ベアメタル (QEMU の -kernel 用。
 * 配置 0x80200000、出力は UART 直書き、終わりは wfi)、
 * 1 = KosabaOS のユーザープログラム (ELF、配置 0x40000000、
 * 出力は putc システムコール、終わりは exit システムコール。
 * スタックは OS が用意するので自分では用意しない)。
 * main が --kosaba の指定で切り替える */
static int target_kosaba;

/* オブジェクト出力 (v31、分割コンパイルの段2。
 * 設計は doc/object-format.md) のための控え。
 * main が --object で obj_mode を立てる。生成の
 * やり方は一括と同じだが、ファイルの外のことは
 * 決めず、穴の場所をここへ控えて encode の
 * オブジェクト書き出しに渡す */
static int obj_mode;
static int asari_mode;  /* Asari 形式で出す (v38、doc/asari.md)。
                         * 立て方は obj_mode と同じで、出口だけ
                         * write_asari に替わる */
static int elf_mode;    /* ELF オブジェクトで出す (v39、
                         * doc/elf-object.md)。立て方は同じで
                         * 出口が write_elf_object に替わる */
static int obj_prefix_insts;  /* 入口の断片の命令数
                               * (最終パスの値が残る) */
static int obj_flush_site;    /* 入口の kosaba_flush
                               * 呼び出しの命令番号。
                               * 無ければ -1 */
/* 住所作りの穴 (auipc + addi の組) の控え。
 * 種類 (番号は命令番号、idx は対象の番号):
 *   1 = 文字列 (文字列番号)、2 = グローバル変数、
 *   3 = グローバル配列、4 = 関数 (v44)。
 * 絶対アドレス (デバイスの窓口) は数が少ないので
 * 別の小さな表に分ける (値そのものを持つ) */
static int RLOC_STR = 1;
static int RLOC_VAR = 2;
static int RLOC_ARR = 3;
static int RLOC_FUNC = 4;  /* 関数 (添字 = 関数番号、v44) */
static int obj_reloc_site[6144];
static int obj_reloc_kind[6144];
static int obj_reloc_idx[6144];
static int obj_nreloc;
static int obj_abs_site[256];
static int obj_abs_val[256];
static int obj_nabs;
/* 一括出力で関数の住所を作った場所の控え (v44):
 * 関数の住所は分岐緩和が落ち着くまで決まらないので、
 * auipc + addi の組だけ置いておき、命令列の最後に
 * (JAL の飛び先と同じ段階で) 即値を埋める。
 * dfaddr_site[i] = auipc の命令番号、dfaddr_func[i]
 * = 関数番号。溢れたら dfaddr_n が -1 になる */
static int dfaddr_site[1024];
static int dfaddr_func[1024];
static int dfaddr_n;

static void gen_load_addr(int reg, unsigned int addr) {
    /* この auipc 命令自身が置かれる住所 (命令列の今の末尾) */
    unsigned int pc = LOAD_BASE + (unsigned int)nasm * 4u;
    long diff = (long)addr - (long)pc;
    long hi = (diff + 0x800) >> 12;
    long lo = diff - (hi << 12);
    asm_push(OP_AUIPC, reg, 0, 0, (int)hi);
    asm_push(OP_ADDI, reg, reg, 0, (int)lo);
}

/* データの住所を kind と idx から引く (v52)。
 * gen_load_addr_sym の控えの解決と、溢れたときの
 * その場焼きの両方で使う */
static unsigned int daddr_resolve(int kind, int idx) {
    if (kind == RLOC_STR)
        return str_addr[idx];
    if (kind == RLOC_VAR)
        return var_global_addr(idx);
    if (kind == RLOC_ARR)
        return arr_addr[idx];
    return (unsigned int)idx;
}

/* 住所作りを「何の住所か」付きで出す (v31)。
 * 一括では gen_load_addr と同じ (今のパスで確定
 * している住所を使う)。オブジェクト出力では
 * 住所はまだ決まらないので、即値を 0 にした組を
 * 出し、穴として控える (埋めるのはリンカ)。
 * kind は RLOC_*、idx は対象の番号。絶対アドレス
 * (kind の代わりに 0) は idx が住所そのもの */
static void gen_load_addr_sym(int reg, int kind, int idx) {
    if (!obj_mode) {
        if (kind == RLOC_FUNC) {
            /* 関数の住所 (v44): コード側の住所は分岐
             * 緩和が落ち着くまで決まらないので、組だけ
             * 置いて控えに残し、最後に埋める */
            if (dfaddr_n >= 0 && dfaddr_n < 1024) {
                dfaddr_site[dfaddr_n] = nasm;
                dfaddr_func[dfaddr_n] = idx;
                dfaddr_n++;
            } else
                dfaddr_n = -1;  /* あふれの印 */
            asm_push(OP_AUIPC, reg, 0, 0, 0);
            asm_push(OP_ADDI, reg, reg, 0, 0);
            return;
        }
        /* データの住所 (v52): その場で焼くと、peephole
         * が命令を消したときに auipc の位置がずれて
         * hi/lo が狂う。関数の住所 (dfaddr) と同じく
         * 組だけ置いて控え、gen_all の最後 (peephole
         * の後) に bake_daddr が埋める。控えの置き場は
         * オブジェクト出力の obj_reloc を借りる
         * (一括では使われていない)。溢れたら印を
         * 付けてその場焼きに切り替える: peephole は
         * 丸ごと止まるので位置は動かず、焼いた値も
         * 控えの解決も正しいままになる */
        if (obj_nreloc >= 0 && obj_nreloc < 6144) {
            obj_reloc_site[obj_nreloc] = nasm;
            obj_reloc_kind[obj_nreloc] = kind;
            obj_reloc_idx[obj_nreloc] = idx;
            obj_nreloc++;
            asm_push(OP_AUIPC, reg, 0, 0, 0);
            asm_push(OP_ADDI, reg, reg, 0, 0);
        } else {
            obj_nreloc = -1;  /* あふれの印 (peephole 停止) */
            gen_load_addr(reg, daddr_resolve(kind, idx));
        }
        return;
    }
    if (kind == 0) {
        /* 絶対アドレスは別の小さな表へ */
        if (obj_nabs < 256) {
            obj_abs_site[obj_nabs] = nasm;
            obj_abs_val[obj_nabs] = idx;
            obj_nabs++;
        } else
            obj_nabs = -1;  /* あふれの印 (gen_program が止める) */
    } else if (obj_nreloc < 6144) {
        obj_reloc_site[obj_nreloc] = nasm;
        obj_reloc_kind[obj_nreloc] = kind;
        obj_reloc_idx[obj_nreloc] = idx;
        obj_nreloc++;
    } else
        obj_nreloc = -1;    /* あふれの印 */
    asm_push(OP_AUIPC, reg, 0, 0, 0);
    asm_push(OP_ADDI, reg, reg, 0, 0);
}

/* 一括出力で、控えておいたデータ住所の組 (auipc +
 * addi) の即値を埋める (v52)。命令列が確定した後
 * (peephole の後) に gen_all から呼ばれる。組の
 * 位置から pc を数え直すので、命令が消えて位置が
 * ずれても住所は正しい。溢れてその場焼きに切り
 * 替わった分は obj_nreloc が -1 になっていて、
 * ここには残っていない */
void bake_daddr(void) {
    int i;
    for (i = 0; i < obj_nreloc; i++) {
        int site = obj_reloc_site[i];
        unsigned int addr = daddr_resolve(obj_reloc_kind[i],
                                          obj_reloc_idx[i]);
        unsigned int pc = LOAD_BASE + (unsigned int)site * 4u;
        long diff = (long)addr - (long)pc;
        long hi = (diff + 0x800) >> 12;
        long lo = diff - (hi << 12);
        asm_imm[site] = (int)hi;
        asm_imm[site + 1] = (int)lo;
    }
}

/* 小さな定数をレジスタに作る (v23): 2次元の列数や行の
 * 歩幅のような、コンパイラだけが知っている定数専用。
 * 2047 までは addi 1つ、それより大きいときは lui + addiw
 * の定石 (hi / lo の分け方は gen_load_addr と同じ考え方
 * だが、こちらは住所でなく「値そのもの」を作るので
 * auipc でなく lui を使う)
 * v27 の直し (セルフコンパイルで発覚): 大きな正の数
 * (式の中の数リテラル) もここを通るようになっていた
 * (v25) ので、INT_MAX 付近まで正しく作れること:
 *   - hi / lo の計算は long で行う。int のままだと
 *     v + 0x800 が溢れて hi が滅茶苦茶になる
 *   - 下位は addi でなく addiw。lui は作った32ビットを
 *     符号拡張するので、hi の最上位ビットが立つ値
 *     (0x7FFFF800 以上) では addi だと上位が全部1に
 *     なってしまう。addiw は32ビットで計算し直して
 *     から符号拡張するので、意図した値になる */
static void gen_load_const(int reg, int v) {
    if (v <= 2047) {
        asm_push(OP_ADDI, reg, REG_ZERO, 0, v);
        return;
    }
    long hi = ((long)v + 0x800) >> 12;
    long lo = (long)v - (hi << 12);
    asm_push(OP_LUI, reg, 0, 0, (int)hi);
    asm_push(OP_ADDIW, reg, reg, 0, (int)lo);
}

/* 32ビットのパターンを、符号なしの値として 0 埋めで
 * 64 ビットに作る (v27)。式の中の大きな16進リテラル
 * (0xFFFFFFFF 等) のための道具。引数 v は node_val の
 * int (2^31 以上は負の数として入っている) そのまま。
 * 作り方: マスクで正のパターンに戻し、lui + addiw で
 * 32ビットの値を作り、slli / srli 32 で上位を 0 に
 * 戻す。slli / srli は「最上位ビットが立つときだけ」
 * と条件分岐せず常に出す: パターンの読み方 (符号
 * つきで見るか符号なしで見るか) は、作ったコンパイラ
 * が GCC か iwac かで食い違うことがあり (キャストは
 * iwac では読み捨て)、分岐すると自分で自分をコン
 * パイルしたときに命令列が変わってしまうため。
 * セルフコンパイルで発覚した昔の不具合: 以前はこの
 * 場合を住所作り (auipc 相対) で代用しており、配置
 * 0x40000000 から遠い 0xFFFFFFFF が作れなかった */
static void gen_load_u32(int reg, int v) {
    long uv = v & 0xFFFFFFFFL;
    long hi = (uv + 0x800) >> 12;
    long lo = uv - (hi << 12);
    asm_push(OP_LUI, reg, 0, 0, (int)hi);
    asm_push(OP_ADDIW, reg, reg, 0, (int)lo);
    asm_push(OP_SLLI, reg, reg, 0, 32);
    asm_push(OP_SRLI, reg, reg, 0, 32);
}

/* 64ビットのビットパターンをレジスタに作る関数の本体は
 * 新断片 src/03b-genfp.c (v46 の浮動小数点生成を集めた
 * 断片) にある。ここには前方宣言だけを元の位置に残す
 * (名前の登録順を変えないため。02c-fpconv.c の節と同様) */
static void emit_load_i64(int reg, long v);
/* 要素ノード (INDEX / INDEX2 / PINDEX / INDEX_STORE) が
 * 指す要素の大きさ (v23)。配列なら宣言で決めた要素の
 * 大きさ、ポインタなら「指す先の大きさ」: ポインタの
 * ポインタ (段数 2 以上) が指す先はポインタなので 8、
 * 段数 1 なら宣言の基本の大きさ (char* は 1、int* は 8) */
static int elem_esize(int node) {
    if (node_kind[node] == NODE_PINDEX) {
        int var = node_val[node];
        if (var_plevel[var] >= 2)
            return 8;
        if (var_plevel[var] == 1)
            return var_bsize[var];
        /* 段数 0 (宣言されていない変数) は deref と
         * 同じく従来どおり 1 バイト (v27) */
        return 1;
    }
    return arr_esize[node_val[node]];
}

/* ---- 型タグ (etype、v35) の道具 ----
 * etype は基本サイズ (1/2/4/8) + 符号なし 16 +
 * 幅つき 32。値は正規形 (符号つきは符号拡張、
 * 符号なしはゼロ拡張した 64ビット) で保持する */

/* 要素ノードが指す要素の型タグ */
static int elem_etype(int node) {
    if (node_kind[node] == NODE_MEMBER ||
        node_kind[node] == NODE_MELEM) {
        /* メンバの型タグ (v37): ポインタ・構造体・配列の
         * メンバは、値がアドレスかポインタなので 8。
         * それ以外はメンバの宣言の型タグ */
        int mi = node_val[node];
        if (smem_plevel[mi] >= 1 || smem_struct[mi] > 0)
            return 8;
        if (node_kind[node] == NODE_MEMBER && smem_count[mi] > 0)
            return 8;
        return smem_etype[mi];
    }
    if (node_kind[node] == NODE_PINDEX) {
        int var = node_val[node];
        if (var_plevel[var] >= 2)
            return 8;
        if (var_plevel[var] == 1)
            return var_etype[var];
        return 1;  /* 段数 0 は従来どおり 1バイト */
    }
    /* 配列の要素: 要素がポインタなら、値は住所なので 8
     * (v49: 以前は基底の型タグをそのまま返しており、
     * int32* の配列の要素の読み書きが 4 バイトに
     * 切れていた。メンバの版には最初からこの判定が
     * ある) */
    if (arr_plevel[node_val[node]] >= 1)
        return 8;
    return arr_etype[node_val[node]];
}

/* ノードの値がポインタなら、指す先の大きさ (バイト)
 * を返す。ポインタでなければ 0 (v49: ポインタ演算の
 * スケール用)。型の見積もりは parse の size_type_of
 * (sizeof と共有) で行う。例外 2 件は size_type_of
 * の「値の型」では歩幅が分からないので先に扱う:
 *   - 2次元配列名と行 (ROWADDR): 指す先は行なので
 *     歩幅は 列数 × 要素の大きさ
 *   - 配列メンバ: 値は先頭要素へのポインタなので
 *     歩幅は要素の大きさ (member_elem_size と同じ
 *     規則。登録順を変えないため呼び出しでなく
 *     ここに写す) */
static int ptr_elem_size(int node) {
    int k = node_kind[node];
    if (k == NODE_ROWADDR) {
        int a = node_val[node];
        return arr_cols[a] * arr_esize[a];
    }
    if (k == NODE_ARRAY && arr_cols[node_val[node]] > 0) {
        int a = node_val[node];
        return arr_cols[a] * arr_esize[a];
    }
    if (k == NODE_MEMBER && smem_count[node_val[node]] > 0) {
        int mi = node_val[node];
        if (smem_plevel[mi] >= 1)
            return 8;
        if (smem_struct[mi] > 0)
            return stag_size[smem_struct[mi] - 1];
        return smem_etype[mi] & 15;
    }
    if (!size_type_of(node))
        return 0;
    if (sot_plevel < 1)
        return 0;
    if (sot_plevel >= 2)
        return 8;  /* 指す先はポインタ */
    if (sot_struct >= 0)
        return stag_size[sot_struct];
    return sot_etype & 15;
}

/* ポインタ読み書きで触る先の型タグ。v49 で相手の
 * 形を広げた (以前は変数と配列名だけ見て、残りは
 * 1 バイトに落としていた)。ポインタの配列の要素は、
 * 宣言で基底の型が残るようになったので (02b-decl)
 * ここでも辿れる */
static int deref_etype(int addr) {
    if (node_kind[addr] == NODE_VAR) {
        int var = node_val[addr];
        if (var_plevel[var] >= 2)
            return 8;
        if (var_plevel[var] == 1)
            return var_etype[var];
        return 1;
    }
    if (node_kind[addr] == NODE_ARRAY) {
        /* 配列名の読み: 要素がポインタなら値は住所 */
        if (arr_plevel[node_val[addr]] >= 1)
            return 8;
        return arr_etype[node_val[addr]];
    }
    if (node_kind[addr] == NODE_PINDEX) {
        int var = node_val[addr];
        if (var_plevel[var] >= 3)
            return 8;
        if (var_plevel[var] == 2)
            return var_etype[var];
        return 1;
    }
    if (node_kind[addr] == NODE_INDEX ||
        node_kind[addr] == NODE_INDEX2) {
        int a = node_val[addr];
        if (arr_plevel[a] >= 2)
            return 8;
        if (arr_plevel[a] == 1)
            return arr_etype[a];
        return 1;
    }
    if (node_kind[addr] == NODE_MEMBER ||
        node_kind[addr] == NODE_MELEM) {
        int mi = node_val[addr];
        if (smem_plevel[mi] >= 2)
            return 8;
        if (smem_plevel[mi] == 1)
            return smem_etype[mi];
        return 1;
    }
    /* 残りの形 (計算式・呼び出し・行アドレス等) は
     * 型の見積もりで辿る */
    if (size_type_of(addr) && sot_plevel >= 1)
        return (sot_plevel >= 2) ? 8 : sot_etype;
    return 1;
}

/* メモリ読みの命令: 幅つきは幅の命令、要素の
 * 1バイトは lbu、それ以外は ld */
static int mem_load_op(int et) {
    if (et & 32) {
        if ((et & 15) == 2)
            return (et & 16) ? OP_LHU : OP_LH;
        if ((et & 15) == 4)
            return (et & 16) ? OP_LWU : OP_LW;
    }
    if ((et & 15) == 1)
        return OP_LBU;
    return OP_LD;
}

/* メモリ書きの命令 (mem_load_op と対) */
static int mem_store_op(int et) {
    if (et & 32) {
        if ((et & 15) == 2)
            return OP_SH;
        if ((et & 15) == 4)
            return OP_SW;
    }
    if ((et & 15) == 1)
        return OP_SB;
    return OP_SD;
}

/* 変数の「値」の型タグ: ポインタ変数の値は住所
 * なので 8、そうでなければ宣言のタグ */
static int var_value_etype(int var) {
    return var_plevel[var] > 0 ? 8 : var_etype[var];
}

/* t1 の値を型タグの正規形に直す (v35)。幅つきで
 * 8バイト未満のときだけ命令が出る: 符号なしは
 * slli/srli で上位を落とし、符号つきは slli/srai
 * で符号を広げる */
static void emit_canon(int et) {
    if (!(et & 32))
        return;
    int bs = et & 15;
    if (bs >= 8)
        return;
    int sh = 64 - bs * 8;
    asm_push(OP_SLLI, REG_T1, REG_T1, 0, sh);
    if (et & 16)
        asm_push(OP_SRLI, REG_T1, REG_T1, 0, sh);
    else
        asm_push(OP_SRAI, REG_T1, REG_T1, 0, sh);
}

/* 結果の型が「符号つき・幅つき・32ビット」か (v60、
 * 設計は doc/wform.md)。この形の演算は W 形式命令が
 * 下位32ビットで計算して符号拡張まで済ませるので、
 * 64ビット演算 + emit_canon のペアを 1 命令にできる。
 * W 形式は相手の下位32ビットしか見ないため、相手の
 * 正規形の形 (符号拡張か 0 埋めか) は問わない */
static int et_is_s32(int et) {
    return (et & 48) == 32 && (et & 15) == 4;
}

/* 整数と double の変換 (fcvt) と、double の真偽化の
 * 本体は src/03b-genfp.c にある (前方宣言だけを元の
 * 位置に残す) */
static void emit_cvt_d(int freg, int greg, int src_et);
static void emit_cvt_l(int dst_et);
static void emit_fp_bool(void);

/* 配列の先頭アドレスをレジスタに作る (v26)。
 * グローバル配列は静的領域の住所 (gen_load_addr)。
 * ローカル配列はフレームの中、変数の枠の並びの上に
 * あるので、fp から「枠の分 + フレームの底からの
 * 位置 − フレーム長」を足した所になる。
 * オフセットが addi の範囲 (±2048) を超えたら、
 * 定数を作って足す形に分ける */
static void gen_load_arr_base(int reg, int a) {
    if (arr_owner[a] < 0) {
        gen_load_addr_sym(reg, RLOC_ARR, a);
        return;
    }
    int off = func_nlocals[arr_owner[a]] * 8 + arr_foff[a] -
              func_frame_size(arr_owner[a]);
    if (off >= -2048 && off <= 2047) {
        asm_push(OP_ADDI, reg, REG_FP, 0, off);
        return;
    }
    int hi = (off + 0x800) >> 12;
    int lo = off - (hi << 12);
    asm_push(OP_LUI, reg, 0, 0, hi);
    asm_push(OP_ADD, reg, REG_FP, reg, 0);
    asm_push(OP_ADDI, reg, reg, 0, lo);
}

/* 変数の住所をレジスタに作る (v21) */
static void gen_var_addr(int reg, int var) {
    if (var_owner[var] < 0)
        gen_load_addr_sym(reg, RLOC_VAR, var);
    else
        asm_push(OP_ADDI, reg, REG_FP, 0, var_frame_off(var));
}

/* 変数の値をレジスタに読む (v21)。読み先のレジスタだけを
 * 使うので、葉の読み出しから呼べる */
static void gen_load_var(int reg, int var) {
    /* レジスタ割り付け (v56): 値はいつも正規形で
     * レジスタに保たれるので、そのまま移すだけ */
    if (var_owner[var] >= 0 && var_reg[var] != 0) {
        asm_push(OP_ADDI, reg, var_reg[var], 0, 0);
        return;
    }
    /* 読む幅は変数の型で決める (v35: 幅つきの型は
     * 幅の命令で読み、正規形でレジスタに入る。
     * それ以外は従来どおり 8バイト読み) */
    int et = var_value_etype(var);
    int op = (et & 32) ? mem_load_op(et) : OP_LD;
    if (var_owner[var] < 0) {
        gen_load_addr_sym(reg, RLOC_VAR, var);
        asm_push(op, reg, reg, 0, 0);
    } else {
        asm_push(op, reg, REG_FP, 0, var_frame_off(var));
    }
}

/* レジスタの値を変数に書く (v21)。グローバルの場合は
 * 住所作りに t2 を使う */
static void gen_store_var(int var, int reg, int value_et) {
    /* レジスタ割り付け (v56): メモリ版の切り捨て
     * (sb / sw 等) が無くなるので、幅つき型は移す前に
     * 正規形へ直す。ただし値が既に変数と同じ型の
     * 正規形で届いているとき (式の型付けがそう
     * 保証する) は直す必要がない。value_et は値が
     * どの型の正規形になっているかで、呼び側が
     * 知っている。正規化は t1 に対する道具なので、
     * 値が t1 でなければ寄せてから直す */
    if (var_owner[var] >= 0 && var_reg[var] != 0) {
        int et = var_value_etype(var);
        if ((et & 32) && (et & 15) < 8 && value_et != et) {
            if (reg != REG_T1)
                asm_push(OP_ADDI, REG_T1, reg, 0, 0);
            emit_canon(et);
            asm_push(OP_ADDI, var_reg[var], REG_T1, 0, 0);
        } else {
            asm_push(OP_ADDI, var_reg[var], reg, 0, 0);
        }
        return;
    }
    /* 書く幅は変数の型で決める (v35、gen_load_var
     * と対。幅つきの型は幅の命令で書き、上の桁は
     * 切り捨てられる) */
    int et = var_value_etype(var);
    int op = (et & 32) ? mem_store_op(et) : OP_SD;
    if (var_owner[var] < 0) {
        gen_load_addr_sym(REG_T2, RLOC_VAR, var);
        asm_push(op, 0, REG_T2, reg, 0);
    } else {
        asm_push(op, 0, REG_FP, reg, var_frame_off(var));
    }
}

/* 葉を、指定レジスタへ読み出す命令を積む。
 *   数:       小さければ即値で入れる。大きければ (16進の
 *             アドレスなど) 住所の材料と同じ作り方で作る
 *   変数:     メモリから値を読む (v21。gen_load_var)
 *   文字列:   その住所を作る (auipc + addi の2命令)
 *   ポインタ読み: 指す先の大きさで読む (v27。char* は
 *             1バイト、int* は 8バイト)。住所は、
 *             変数ならその値、数なら絶対アドレスそのもの */
/* *式 の相手が関数ポインタそのものか (v44): 関数
 * ポインタの * は関数そのものを指す (C と同じ) ので、
 * メモリの読み出しは起きず、住所がそのまま値になる */
static int deref_is_funcptr(int node) {
    int o = node_lhs[node];
    int k = node_kind[o];
    if (k == NODE_FADDR)
        return 1;
    if (k == NODE_VAR)
        return var_plevel[node_val[o]] == 0 &&
               var_fsig[node_val[o]] != 0;
    if (k == NODE_PINDEX)
        return var_plevel[node_val[o]] == 1 &&
               var_fsig[node_val[o]] != 0;
    if (k == NODE_INDEX || k == NODE_INDEX2)
        return arr_plevel[node_val[o]] == 0 &&
               arr_fsig[node_val[o]] != 0;
    if (k == NODE_MEMBER || k == NODE_MELEM)
        return smem_plevel[node_val[o]] == 0 &&
               smem_fsig[node_val[o]] != 0;
    return 0;
}

static void gen_load_leaf(int reg, int leaf) {
    int k = node_kind[leaf];
    if (k == NODE_NUMFP) {
        /* 浮動小数点リテラル (v46): ビットパターンを
         * 整数として作る。浮動小数点レジスタへ移すのは
         * 呼び出し側 (文脈が決める) */
        emit_load_i64(reg, fp_bits[node_val[leaf]]);
    } else if (k == NODE_NUM) {
        /* 分岐は node_val の符号つきの値で決める
         * (キャストで符号なしとして見てはいけない:
         * iwac ではキャストは読み捨てなので、GCC で
         * 作った iwac と iwac で作った iwac で判断が
         * 食い違い、固定点にならない。v27 のセルフ
         * ホストで発覚)。16進リテラルは parse の時点で
         * int の範囲に畳んであるので、符号つきの見え
         * 方は作ったコンパイラによらず同じ */
        int v = node_val[leaf];
        if (v >= 0 && v <= 2047)
            asm_push(OP_ADDI, reg, REG_ZERO, 0, v);
        else
            /* 値として作る (住所作りの auipc 相対では
             * ない)。gen_load_u32 が 0 埋めで広げる */
            gen_load_u32(reg, v);
    } else if (k == NODE_VAR)
        /* 変数の値はメモリから読む (v21) */
        gen_load_var(reg, node_val[leaf]);
    else if (k == NODE_STR)
        gen_load_addr_sym(reg, RLOC_STR, node_val[leaf]);
    else if (k == NODE_ARRAY)
        /* 配列の名前の「値」は先頭アドレス */
        gen_load_arr_base(reg, node_val[leaf]);
    else { /* NODE_DEREF: lhs が住所を表すノード */
        int operand = node_lhs[leaf];
        if (deref_is_funcptr(leaf)) {
            /* 関数ポインタの * は住所そのもの (v44):
             * 読み出さず、相手の値をそのまま使う */
            if (node_kind[operand] == NODE_VAR)
                gen_load_var(reg, node_val[operand]);
            else /* NODE_FADDR */
                gen_load_addr_sym(reg, RLOC_FUNC,
                                  node_val[operand]);
            return;
        }
        /* 読む幅は指す先の大きさで決める (v27: 以前は
         * 常に1バイトで、int* の読みが切れていた) */
        int op = mem_load_op(deref_etype(operand));
        if (node_kind[operand] == NODE_VAR) {
            /* 変数の持つ住所から読む: まず変数の値 (住所)
             * を読み、その先を読む (v21) */
            gen_load_var(reg, node_val[operand]);
            asm_push(op, reg, reg, 0, 0);
        } else {
            /* 絶対アドレス読み: 住所を reg に作ってから読む。
             * 住所の材料は auipc 相対なので、0x10000005 の
             * ようなデバイスの住所でも壊れない */
            if (node_kind[operand] == NODE_NUM)
                gen_load_addr_sym(reg, 0, node_val[operand]);
            else if (node_kind[operand] == NODE_ARRAY)
                gen_load_arr_base(reg, node_val[operand]);
            else /* NODE_STR: 文字列の住所から読む */
                gen_load_addr_sym(reg, RLOC_STR, node_val[operand]);
            asm_push(op, reg, reg, 0, 0);
        }
    }
}

/* 呼び出しの引数を積む道具 (v22)。gen_expr と相互に使う。
 * v46 で引数の型に合わせた変換が要るため、関数の番号
 * (f、無ければ -1) と署名の番号 (sig、無ければ -1) も
 * 渡すようになった */
static int gen_call_args(int arg, int f, int sig);

/* 式を、受け側の型タグに合わせた置き場 (整数は t1、
 * double は ft0) へ生成する道具 (v46)。gen_expr と
 * 相互に使う */
static int gen_value_as(int node, int dst);

/* 浮動小数点の二項演算の生成 (v46)。gen_expr と相互に使う */
static int gen_fp_binop(int node);

/* 二項演算の両辺を t1 / t2 に揃える道具 (v53)。
 * gen_expr と相互に使う */
static int gen_binop_operands(int node);
static int gen_imm_binop(int node);

/* 条件を分岐として生成する道具 (v53)。本体は
 * src/03-genstmt.c の gen_cond_expr の隣にある */
static int gen_cond_branch(int node, int *head, int sense);

/* 飛び先のパッチリストを確定させる道具。本体は
 * src/03-genstmt.c (v53 で三項演算子からも使う) */
static void apply_patches(int head, int target);

/* 要素の住所を作る道具 (v23)。gen_expr と相互に使う */
static int gen_elem_addr(int node);
static int xform_try_addr(int node);

/* ループ最適化 (v57): 帰納変数の変更の直後に
 * ポインタを進める道具。本体は gen_elem_addr の前 */
static void loop_xform_on_mod(int node);

/* ポインタ演算のスケールの道具 (v49)。本体は
 * emit_scale の近くに置く (名前の登録順を変えない
 * ため、ここには前方宣言だけ) */
static void emit_scale_r(int reg, int esize);
static void emit_add_step(int dst, int src, int step);

/* B 方式の呼び出しの道具 (v45)。gen_expr と相互に使う */
static int gen_push_args_b(int arg, int sig, int ai);
static int gen_b_call(int node, int mode);

/* gen_call_args が今どの引数を処理しているか (左から
 * 数えた番号)。呼び出し側が引数の数 - 1 を入れてから
 * 呼ぶ。再帰が右の引数から戻るたびに減っていく。
 * 引数の式の中の入れ子の呼び出しが上書きするので、
 * 値を取るたびに控え直す (gen_call_args の中身参照) */
static int call_args_idx;

/* 式の AST から命令列を作る。結果は t1 に残る約束。
 * 失敗したら 0 を返す (理由は errmsg)。
 *
 * 作り方: 左の子を先に生成する (結果は t1)。右の子が葉
 * なら t2 に入れて演算する。右の子も式 (木) の場合は、
 * 左の結果をスタックに退避してから右を計算し、t2 へ
 * 写して左を戻す (v7 で入れた一般形の評価)。
 * スタックが要るプログラムでは、gen_all が先に sp を
 * 用意する (関数のあるプログラムと同じ仕組み) */
/* 式の型付け (v35): AST を歩いて、各式ノードに
 * 型タグ (node_etype) を付ける。生成の前に体ごと
 * 呼ぶ。規則は design.md の v35 の節:
 *   二項演算 (ビット演算を含む) は、幅つきがあれば
 *   その型 (両方なら左)、なければ従来の 8
 *   シフトは左の型、比較と論理演算は従来の 8
 *   変換 (CAST) は変換先、呼び出しは戻り値の型
 * 子への歩き方はノードの種類ごとに決める (使わない
 * 欄に 0 が入っているノードがあるため、一律には
 * 歩けない) */
static void type_node(int node);
static void type_list(int head) {
    for (int n = head; n != -1; n = node_next[n])
        type_node(n);
}
static void type_node(int node) {
    if (node < 0)
        return;
    int k = node_kind[node];
    /* まず子を型付けする */
    if (k == NODE_WHILE || k == NODE_DOWHILE) {
        type_node(node_lhs[node]);
        type_list(node_rhs[node]);
    } else if (k == NODE_IF) {
        type_node(node_lhs[node]);
        type_list(node_rhs[node]);
        type_list(node_val[node]);
    } else if (k == NODE_FOR) {
        if (node_val[node] >= 0)
            type_node(node_val[node]);
        type_node(node_lhs[node]);
        type_list(node_rhs[node]);
    } else if (k == NODE_FORSTEP) {
        type_node(node_val[node]);
    } else if (k == NODE_SWITCH) {
        /* switch (v43): 制御式と、各 case 群の文の並び */
        type_node(node_lhs[node]);
        for (int g = node_rhs[node]; g != -1; g = node_next[g])
            type_list(node_lhs[g]);
    } else if (k == NODE_COND) {
        type_node(node_lhs[node]);
        type_node(node_val[node]);
        type_node(node_rhs[node]);
    } else if (k == NODE_NUMFP) {
        /* 浮動小数点リテラル (v46): 子はない */
    } else if (k == NODE_CALL || k == NODE_SYSCALL3) {
        type_list(node_lhs[node]);
    } else if (k == NODE_ICALL) {
        /* 間接呼び出し (v44): lhs = 呼び先の式、
         * rhs = 引数の並び */
        type_node(node_lhs[node]);
        type_list(node_rhs[node]);
    } else if (k == NODE_FADDR) {
        /* 関数の住所 (v44): 子はない */
    } else if (k == NODE_NUM || k == NODE_VAR ||
               k == NODE_STR || k == NODE_ARRAY ||
               k == NODE_DECL || k == NODE_VDECL ||
               k == NODE_BREAK || k == NODE_CONTINUE ||
               k == NODE_PREINC || k == NODE_PREDEC ||
               k == NODE_POSTINC || k == NODE_POSTDEC) {
        /* 葉: 子はない */
    } else {
        /* 残りの種類は lhs (と rhs) が式 */
        type_node(node_lhs[node]);
        if (k == NODE_ADD || k == NODE_SUB || k == NODE_MUL ||
            k == NODE_DIV || k == NODE_MOD || k == NODE_AND ||
            k == NODE_OR || k == NODE_XOR || k == NODE_EQ ||
            k == NODE_NE || k == NODE_LT || k == NODE_GT ||
            k == NODE_LE || k == NODE_GE || k == NODE_SHL ||
            k == NODE_SHR || k == NODE_LAND || k == NODE_LOR ||
            k == NODE_STORE || k == NODE_ELEM_STORE ||
            k == NODE_INDEX_STORE || k == NODE_INDEX2 ||
            k == NODE_INDEX || k == NODE_PINDEX ||
            k == NODE_MELEM ||
            k == NODE_ROWADDR)
            type_node(node_rhs[node]);
    }
    int et = 8;
    if (k == NODE_NUMFP)
        et = 8 + 64;  /* 浮動小数点リテラルは double (v46) */
    else if (k == NODE_VAR)
        et = var_value_etype(node_val[node]);
    else if (k == NODE_INDEX || k == NODE_INDEX2)
        et = arr_etype[node_val[node]];
    else if (k == NODE_PINDEX)
        et = elem_etype(node);
    else if (k == NODE_MEMBER || k == NODE_MELEM)
        et = elem_etype(node);
    else if (k == NODE_DEREF)
        et = deref_etype(node_lhs[node]);
    else if (k == NODE_CALL)
        et = func_ret_etype[node_val[node]];
    else if (k == NODE_ICALL)
        et = sig_ret_etype[node_val[node]];
    else if (k == NODE_FADDR)
        et = 8;  /* 住所は8バイト */
    else if (k == NODE_CAST)
        et = node_val[node];
    else if (k == NODE_ADD || k == NODE_SUB || k == NODE_MUL ||
             k == NODE_DIV || k == NODE_MOD || k == NODE_AND ||
             k == NODE_OR || k == NODE_XOR) {
        int l = node_etype[node_lhs[node]];
        int r = node_etype[node_rhs[node]];
        /* どちらかが double なら double に揃える (v46。
         * 使えない演算の拒否は生成側の仕事) */
        if ((l & 64) || (r & 64))
            et = 8 + 64;
        else
            et = (l & 32) ? l : (r & 32) ? r : 8;
    }
    else if (k == NODE_SHL || k == NODE_SHR) {
        int l = node_etype[node_lhs[node]];
        if (l & 64)
            et = 8 + 64;  /* 生成側でエラーにする (v46) */
        else
            et = (l & 32) ? l : 8;
    }
    else if (k == NODE_NEG || k == NODE_BITNOT)
        et = node_etype[node_lhs[node]];
    else if (k == NODE_ASSIGN)
        et = var_value_etype(node_val[node]);
    else if (k == NODE_PREINC || k == NODE_PREDEC ||
             k == NODE_POSTINC || k == NODE_POSTDEC)
        et = var_value_etype(node_val[node]);
    else if (k == NODE_EPRE || k == NODE_EPOST)
        et = elem_etype(node_lhs[node]);
    else if (k == NODE_COND) {
        int t = node_etype[node_val[node]];
        int e = node_etype[node_rhs[node]];
        if ((t & 64) || (e & 64))
            et = 8 + 64;  /* 枝のどちらかが double なら
                           * double に揃える (v46) */
        else
            et = (t & 32) ? t : (e & 32) ? e : 8;
    }
    node_etype[node] = et;
}

/* 式の値に結び付く関数ポインタの署名 (v44): 値そのものが
 * 関数ポインタ、または関数ポインタへのポインタのとき、
 * その署名番号 + 1 を返す。関係なければ 0。代入や
 * 引数の受け渡しの照合に使う */
static int value_fsig(int node) {
    int k = node_kind[node];
    if (k == NODE_VAR)
        return var_fsig[node_val[node]];
    if (k == NODE_FADDR)
        return func_sig[node_val[node]] + 1;
    if (k == NODE_ARRAY || k == NODE_ROWADDR ||
        k == NODE_INDEX || k == NODE_INDEX2)
        return arr_fsig[node_val[node]];
    if (k == NODE_PINDEX)
        return var_fsig[node_val[node]];
    if (k == NODE_MEMBER || k == NODE_MELEM)
        return smem_fsig[node_val[node]];
    if (k == NODE_CALL)
        return sig_ret_fsig[func_sig[node_val[node]]];
    if (k == NODE_ICALL)
        return sig_ret_fsig[node_val[node]];
    if (k == NODE_DEREF || k == NODE_ADDR || k == NODE_CAST)
        return value_fsig(node_lhs[node]);
    if (k == NODE_COND) {
        int t = value_fsig(node_val[node]);
        int e = value_fsig(node_rhs[node]);
        return t == e ? t : 0;
    }
    return 0;
}

/* 要素を表すノード (要素への書き込みの住所側) に
 * 結び付く関数ポインタの署名 (v44)。無ければ 0 */
static int elem_fsig_of(int node) {
    int k = node_kind[node];
    if (k == NODE_INDEX || k == NODE_INDEX2)
        return arr_fsig[node_val[node]];
    if (k == NODE_PINDEX)
        return var_fsig[node_val[node]];
    if (k == NODE_MEMBER || k == NODE_MELEM)
        return smem_fsig[node_val[node]];
    return 0;
}

/* 関数ポインタの受け渡しの照合 (v44): 受け側の署名
 * (want、+1 符号化) が分かっていて、渡す側の署名も
 * 分かっているのに食い違うときだけ止める。渡す側が
 * 数や素のポインタなど署名の分からない値なら、
 * 今の緩い型の流儀どおり通す */
static int check_fsig(int want, int expr) {
    if (want == 0)
        return 1;
    int got = value_fsig(expr);
    if (got != 0 && got != want) {
        errmsg = "関数ポインタの型 (署名) が合いません";
        return 0;
    }
    return 1;
}

/* 式の値に結び付く構造体のタグ (v45): 値そのものが
 * 構造体のとき、そのタグ番号 + 1 を返す。関係なければ
 * 0。代入や引数の受け渡しの照合に使う */
static int value_struct(int node) {
    int k = node_kind[node];
    if (k == NODE_VAR) {
        if (var_plevel[node_val[node]] == 0)
            return var_struct[node_val[node]];
        return 0;
    }
    if (k == NODE_INDEX || k == NODE_INDEX2) {
        if (arr_plevel[node_val[node]] == 0)
            return arr_struct[node_val[node]];
        return 0;
    }
    if (k == NODE_MEMBER || k == NODE_MELEM) {
        if (smem_plevel[node_val[node]] == 0)
            return smem_struct[node_val[node]];
        return 0;
    }
    if (k == NODE_PINDEX) {
        if (var_plevel[node_val[node]] == 1)
            return var_struct[node_val[node]];
        return 0;
    }
    if (k == NODE_DEREF) {
        int b = node_lhs[node];
        if (node_kind[b] == NODE_VAR &&
            var_plevel[node_val[b]] == 1)
            return var_struct[node_val[b]];
        if ((node_kind[b] == NODE_MEMBER ||
             node_kind[b] == NODE_MELEM) &&
            smem_plevel[node_val[b]] == 1)
            return smem_struct[node_val[b]];
        return 0;
    }
    if (k == NODE_CALL)
        return sig_ret_struct[func_sig[node_val[node]]];
    if (k == NODE_ICALL)
        return sig_ret_struct[node_val[node]];
    return 0;
}

/* 構造体の受け渡しの照合 (v45): 受け側のタグ (want、
 * +1 符号化) が分かっていて、渡す側のタグも分かって
 * いるのに食い違うときだけ止める (check_fsig と同じ
 * 方針) */
static int check_struct(int want, int expr) {
    if (want == 0)
        return 1;
    int got = value_struct(expr);
    if (got != 0 && got != want) {
        errmsg = "構造体の型 (タグ) が合いません";
        return 0;
    }
    return 1;
}

/* 構造体のタグ (+1 符号化) が占めるスロット数 (v45) */
static int struct_slots(int tag) {
    return (stag_size[tag - 1] + 7) / 8;
}

/* 呼び出しが B 方式かどうか (v45): 構造体の引数か
 * 構造体の戻り値が1つでもあれば B 方式。署名だけから
 * 決まるので、定義側と呼び出し側で必ず一致する */
static int call_b_mode(int sig) {
    if (sig_ret_struct[sig] > 0)
        return 1;
    for (int i = 0; i < sig_nparams[sig]; i++)
        if (sig_pstruct[sig][i] > 0 && sig_pplevel[sig][i] == 0)
            return 1;
    return 0;
}

/* 引数 i が引数領域に占めるスロット数 (v45) */
static int arg_slots(int sig, int i) {
    if (sig_pstruct[sig][i] > 0 && sig_pplevel[sig][i] == 0)
        return struct_slots(sig_pstruct[sig][i]);
    return 1;
}

/* 引数領域の全スロット数 (sret のスロットを含む、v45) */
static int call_total_slots(int sig) {
    int n = sig_ret_struct[sig] > 0 ? 1 : 0;
    for (int i = 0; i < sig_nparams[sig]; i++)
        n += arg_slots(sig, i);
    return n;
}

static int gen_expr(int node) {
    if (node_kind[node] == NODE_VAR &&
        var_struct[node_val[node]] > 0 &&
        var_plevel[node_val[node]] == 0) {
        /* 構造体の値そのものを式の値にはできない (v37) */
        errmsg = "構造体は値として扱えません (メンバかポインタで使ってください)";
        return 0;
    }
    if ((node_kind[node] == NODE_INDEX ||
         node_kind[node] == NODE_INDEX2) &&
        arr_struct[node_val[node]] > 0 &&
        arr_plevel[node_val[node]] == 0) {
        errmsg = "構造体は値として扱えません (メンバかポインタで使ってください)";
        return 0;
    }
    if (is_leaf(node)) {
        /* 葉: t1 にその値 (変数なら今の値、文字列なら住所、
         * ポインタ読みなら指された先の1バイト) が入る */
        gen_load_leaf(REG_T1, node);
        if (node_etype[node] & 64) {
            /* double の葉 (v46): ビットとして t1 に
             * 入っているので、浮動小数点の置き場 (ft0)
             * へ移す */
            asm_push(OP_FMV_D_X, REG_FT0, REG_T1, 0, 0);
        }
        return 1;
    }
    if (node_kind[node] == NODE_DEREF) {
        if (deref_is_funcptr(node))
            /* 関数ポインタの * は住所そのもの (v44) */
            return gen_expr(node_lhs[node]);
        /* 相手が計算式のポインタ読み: 住所を t1 で計算し、
         * その住所から読む。幅は相手の型から指す先が
         * 分かればその幅 (v49: 以前は計算式だと常に
         * 1バイトだった)、分からなければ従来どおり
         * 1バイト (変数が先の読みは gen_load_leaf が
         * 大きさで読む) */
        if (!gen_expr(node_lhs[node]))
            return 0;
        asm_push(mem_load_op(deref_etype(node_lhs[node])),
                 REG_T1, REG_T1, 0, 0);
        return 1;
    }
    if (node_kind[node] == NODE_INDEX ||
        node_kind[node] == NODE_INDEX2 ||
        node_kind[node] == NODE_PINDEX) {
        /* 要素読み (v23 で 2次元とポインタ添字も同じ道具に):
         * 要素の住所を t2 に作り (gen_elem_addr)、要素の
         * 大きさに合わせて読む。1バイトなら lbu、8バイト
         * なら ld */
        if (!gen_elem_addr(node))
            return 0;
        asm_push(mem_load_op(elem_etype(node)),
                 REG_T1, REG_T2, 0, 0);
        if (node_etype[node] & 64)
            /* double の要素 (v46): ビットを ft0 へ */
            asm_push(OP_FMV_D_X, REG_FT0, REG_T1, 0, 0);
        return 1;
    }
    if (node_kind[node] == NODE_MEMBER ||
        node_kind[node] == NODE_MELEM) {
        /* メンバ読み (v37): 住所を作り、数値・ポインタの
         * メンバなら読み出す。構造体・配列のメンバは
         * 住所そのものが値 (配列の decay と同じ) */
        if (!gen_elem_addr(node))
            return 0;
        int mi = node_val[node];
        int is_addr = (smem_struct[mi] > 0 && smem_plevel[mi] == 0) ||
                      (node_kind[node] == NODE_MEMBER &&
                       smem_count[mi] > 0);
        if (is_addr) {
            asm_push(OP_ADDI, REG_T1, REG_T2, 0, 0);
            return 1;
        }
        asm_push(mem_load_op(elem_etype(node)),
                 REG_T1, REG_T2, 0, 0);
        if (node_etype[node] & 64)
            /* double のメンバ (v46): ビットを ft0 へ */
            asm_push(OP_FMV_D_X, REG_FT0, REG_T1, 0, 0);
        return 1;
    }
    if (node_kind[node] == NODE_ROWADDR) {
        /* 2次元配列の行の先頭アドレス (v23): 基底 +
         * 行 × (列数 × 要素の大きさ)。歩幅は定数なので、
         * 2の冪ならシフト、そうでなければ定数との掛け算 */
        int a = node_val[node];
        if (!gen_expr(node_rhs[node]))
            return 0;
        int stride = arr_cols[a] * arr_esize[a];
        if (stride > 1) {
            int shift = 0;
            int s = stride;
            while ((s & 1) == 0) {
                s >>= 1;
                shift++;
            }
            if (s == 1) {
                asm_push(OP_SLLI, REG_T1, REG_T1, 0, shift);
            } else {
                gen_load_const(REG_A0, stride);
                asm_push(OP_MUL, REG_T1, REG_T1, REG_A0, 0);
            }
        }
        gen_load_arr_base(REG_T2, a);
        asm_push(OP_ADD, REG_T1, REG_T2, REG_T1, 0);
        return 1;
    }
    if (node_kind[node] == NODE_FADDR) {
        /* 関数の住所 (v44): 住所作りの組を出す。
         * 一括では命令列の最後に埋め、オブジェクトでは
         * リンカが埋める (gen_load_addr_sym の説明参照) */
        gen_load_addr_sym(REG_T1, RLOC_FUNC, node_val[node]);
        return 1;
    }
    if (node_kind[node] == NODE_SYSCALL3) {
        /* 組み込み syscall3 (v27、KosabaOS ターゲット専用):
         *   引数を右から評価してスタックに積む (関数呼び
         *   出しと同じ gen_call_args。積んだ引数は
         *   sp + 番号*16 に並ぶ)
         *   a7 = 第0引数 (番号)、a0 / a1 / a2 = 第1〜3引数
         *   スタックを戻して ecall、戻り値 a0 を t1 へ
         * ベア版にはシステムコールが無いのでエラーにする */
        if (!target_kosaba) {
            errmsg = "syscall3 は KosabaOS ターゲット専用です";
            return 0;
        }
        call_args_idx = 3;  /* 引数は4つ固定 (v46) */
        if (!gen_call_args(node_lhs[node], -1, -1))
            return 0;
        asm_push(OP_LD, REG_A7, REG_SP, 0, 0);
        asm_push(OP_LD, REG_A0, REG_SP, 0, 16);
        asm_push(OP_LD, REG_A1, REG_SP, 0, 32);
        asm_push(OP_LD, REG_A2, REG_SP, 0, 48);
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, 64);
        asm_push(OP_ECALL, 0, 0, 0, 0);
        asm_push(OP_ADDI, REG_T1, REG_A0, 0, 0);
        return 1;
    }
    if (node_kind[node] == NODE_CALL) {
        /* インライン化 (v58): 計画で展開が決まって
         * いる呼び出しは、本体をその場に広げる。
         * 展開の生成が深くなりすぎたとき (引数の中の
         * 呼び出しが重なったとき) は inline_find が
         * -1 を返し、通常の呼び出しに落ちる */
        {
            int ie = inline_find(node);
            if (ie >= 0)
                return gen_inline_call(node, ie);
        }
        if (call_b_mode(func_sig[node_val[node]])) {
            /* B 方式 (v45): 構造体が絡む呼び出し。
             * 構造体の戻り値は式の値にはならないので、
             * ここではスカラを返すものだけを扱う
             * (構造体の戻り値は代入・引数・return の
             * 位置で gen_b_call が直接受ける) */
            if (sig_ret_struct[func_sig[node_val[node]]] > 0) {
                errmsg = "構造体の戻り値は代入・引数・return の位置で使ってください";
                return 0;
            }
            return gen_b_call(node, 2);
        }
        /* 関数呼び出し (v22 で式になった):
         *   引数を右から評価してスタックに積む
         *   積んだものを a0〜a3 へ順に移し、スタックを戻す
         *   jal ra, 関数 (飛び先は生成の最後に書き直す)
         *   addi t1, a0, 0 (戻り値を式の値の場所 t1 へ)
         * a0〜a3 へ移すのは全引数の評価が終わった後なので、
         * 引数の式の中に別の呼び出し (f(g(x)) の形) が
         * あっても、a レジスタの受け渡しが壊れない。
         * だから a レジスタの退避は要らない */
        int nargs = 0;
        for (int a = node_lhs[node]; a != -1; a = node_next[a])
            nargs++;
        if (func_has_body[node_val[node]]) {
            /* 引数が関数ポインタなら署名を照合する
             * (v44。本体のある関数だけ引数の変数が
             * 名簿に載っている) */
            int ai = 0;
            for (int a = node_lhs[node]; a != -1; a = node_next[a]) {
                if (!check_fsig(var_fsig[func_param_var[node_val[node]][ai]], a))
                    return 0;
                ai++;
            }
        }
        call_args_idx = nargs - 1;
        if (!gen_call_args(node_lhs[node], node_val[node], -1))
            return 0;
        if (nargs > 0) {
            for (int k = 0; k < nargs; k++)
                /* a0〜a3 は番号が連番 (10〜13) */
                asm_push(OP_LD, REG_A0 + k, REG_SP, 0, k * 16);
            asm_push(OP_ADDI, REG_SP, REG_SP, 0, nargs * 16);
        }
        asm_push(OP_JAL, REG_RA, 0, 0, -(node_val[node] + 1));
        asm_push(OP_ADDI, REG_T1, REG_A0, 0, 0);
        /* 戻り値の型が幅つきなら正規形に直す (v35)。
         * ただしポインタを返す関数では値が住所なので
         * 直さない (v49: 指す先が幅つきだと住所が
         * 切り詰められていた) */
        if (sig_ret_plevel[func_sig[node_val[node]]] == 0 &&
            sig_ret_struct[func_sig[node_val[node]]] == 0)
            emit_canon(func_ret_etype[node_val[node]]);
        if (func_ret_etype[node_val[node]] & 64)
            /* double の戻り値 (v46): ビットを ft0 へ */
            asm_push(OP_FMV_D_X, REG_FT0, REG_T1, 0, 0);
        return 1;
    }
    if (node_kind[node] == NODE_ICALL) {
        if (call_b_mode(node_val[node])) {
            /* B 方式 (v45): 直接呼び出しと同じ扱い */
            if (sig_ret_struct[node_val[node]] > 0) {
                errmsg = "構造体の戻り値は代入・引数・return の位置で使ってください";
                return 0;
            }
            return gen_b_call(node, 2);
        }
        /* 関数ポインタ経由の呼び出し (v44):
         *   引数を右から評価してスタックに積む
         *   呼び先の式を評価して t1 に住所を得る
         *   積んだ引数を a0〜 へ移してスタックを戻す
         *   jalr ra, t1, 0
         *   addi t1, a0, 0 (戻り値を式の値の場所へ)
         * 引数の積み下ろし (a0〜 への ld) は t1 に
         * 触らないので、呼び先の住所は壊れない */
        int sig = node_val[node];
        int nargs = 0;
        for (int a = node_rhs[node]; a != -1; a = node_next[a])
            nargs++;
        {
            int ai = 0;
            for (int a = node_rhs[node]; a != -1; a = node_next[a]) {
                if (!check_fsig(sig_pfsig[sig][ai], a))
                    return 0;
                ai++;
            }
        }
        call_args_idx = nargs - 1;
        if (!gen_call_args(node_rhs[node], -1, sig))
            return 0;
        if (!gen_expr(node_lhs[node]))
            return 0;
        if (nargs > 0) {
            for (int k = 0; k < nargs; k++)
                asm_push(OP_LD, REG_A0 + k, REG_SP, 0, k * 16);
            asm_push(OP_ADDI, REG_SP, REG_SP, 0, nargs * 16);
        }
        asm_push(OP_JALR, REG_RA, REG_T1, 0, 0);
        asm_push(OP_ADDI, REG_T1, REG_A0, 0, 0);
        /* 戻り値の型が幅つきなら正規形に直す。
         * ポインタを返す場合は直さない (v49、
         * 直接呼び出しの版と同じ) */
        if (sig_ret_plevel[sig] == 0 &&
            sig_ret_struct[sig] == 0)
            emit_canon(sig_ret_etype[sig]);
        if (sig_ret_etype[sig] & 64)
            /* double の戻り値 (v46): ビットを ft0 へ */
            asm_push(OP_FMV_D_X, REG_FT0, REG_T1, 0, 0);
        return 1;
    }
    if (node_kind[node] == NODE_LAND) {
        /* && (短絡評価): 左が 0 なら右は評価しない。
         *   左を計算 (t1)
         *   beq t1, zero, L_false (右を飛ばす)
         *   右を計算 (t1)
         *   sltu t1, zero, t1 (右の値を 1 か 0 に直す)
         *   jal zero, L_end
         * L_false: addi t1, zero, 0
         * L_end:
         * 左の値は分岐で消費されるので、退避は要らない */
        if (!gen_expr(node_lhs[node]))
            return 0;
        if (node_etype[node_lhs[node]] & 64)
            emit_fp_bool();  /* double の条件 (v46) */
        int f_jump = emit_beq_t1z();
        if (!gen_expr(node_rhs[node]))
            return 0;
        if (node_etype[node_rhs[node]] & 64)
            emit_fp_bool();
        asm_push(OP_SLTU, REG_T1, REG_ZERO, REG_T1, 0);
        int e_jump = nasm;
        asm_push(OP_JAL, 0, 0, 0, 0);
        asm_imm[f_jump] = nasm;
        asm_push(OP_ADDI, REG_T1, REG_ZERO, 0, 0);
        asm_imm[e_jump] = nasm;
        return 1;
    }
    if (node_kind[node] == NODE_LOR) {
        /* || (短絡評価): 左が 0 でなければ右は評価しない。
         *   左を計算 (t1)
         *   beq t1, zero, L_rhs (右の評価へ)
         *   addi t1, zero, 1
         *   jal zero, L_end
         * L_rhs: 右を計算 (t1)
         *        sltu t1, zero, t1 (1 か 0 に直す)
         * L_end: */
        if (!gen_expr(node_lhs[node]))
            return 0;
        if (node_etype[node_lhs[node]] & 64)
            emit_fp_bool();  /* double の条件 (v46) */
        int r_jump = emit_beq_t1z();
        asm_push(OP_ADDI, REG_T1, REG_ZERO, 0, 1);
        int e_jump = nasm;
        asm_push(OP_JAL, 0, 0, 0, 0);
        asm_imm[r_jump] = nasm;
        if (!gen_expr(node_rhs[node]))
            return 0;
        if (node_etype[node_rhs[node]] & 64)
            emit_fp_bool();
        asm_push(OP_SLTU, REG_T1, REG_ZERO, REG_T1, 0);
        asm_imm[e_jump] = nasm;
        return 1;
    }
    if (node_kind[node] == NODE_NOT) {
        /* !x: x が 0 のときだけ 1。sltiu t1, t1, 1 が
         * 「1 未満か (符号なし)」=「0 か」になる */
        if (!gen_expr(node_lhs[node]))
            return 0;
        if (node_etype[node_lhs[node]] & 64)
            emit_fp_bool();  /* double の相手 (v46):
                              * 0/1 に直してから反転する */
        asm_push(OP_SLTIU, REG_T1, REG_T1, 0, 1);
        return 1;
    }
    if (node_kind[node] == NODE_CAST) {
        /* 型変換: 幅つきの型へは正規形に直す (v35)。
         * 浮動小数点が絡む変換 (v46) は、値の置き場
         * (t1 と ft0) を跨ぐので fcvt で値を変換する。
         * 変換先は node_val、相手の型は型付けの結果 */
        int dst = node_val[node];
        int src = node_etype[node_lhs[node]];
        if (!gen_expr(node_lhs[node]))
            return 0;
        if (dst & 64) {
            if (!(src & 64))
                /* 整数 → double */
                emit_cvt_d(REG_FT0, REG_T1, src);
            return 1;
        }
        if (src & 64) {
            /* double → 整数 (切り捨て + 正規形) */
            emit_cvt_l(dst);
            emit_canon(dst);
            return 1;
        }
        emit_canon(dst);
        return 1;
    }
    if (node_kind[node] == NODE_BITNOT) {
        /* ~x (v25): 全ビット反転は -1 との XOR */
        if (node_etype[node_lhs[node]] & 64) {
            errmsg = "浮動小数点に ~ は使えません";
            return 0;
        }
        if (!gen_expr(node_lhs[node]))
            return 0;
        asm_push(OP_XORI, REG_T1, REG_T1, 0, -1);
        emit_canon(node_etype[node]);
        return 1;
    }
    if (node_kind[node] == NODE_COND) {
        /* 三項演算子 (v25): 短絡評価。&& と同じく、
         * 条件の値で飛び先を分ける:
         *   条件を分岐として生成 (偽なら L_false)
         *   真の式を計算 (t1)
         *   jal zero, L_end
         * L_false: 偽の式を計算 (t1)
         * L_end:
         * 条件の値は分岐で消費されるので退避は不要 */
        /* 条件は分岐として生成する (v53): 偽なら
         * L_false へ。飛び先はリストで受ける */
        int f_head = -1;
        if (!gen_cond_branch(node_lhs[node], &f_head, 0))
            return 0;
        if (!gen_expr(node_val[node]))
            return 0;
        if ((node_etype[node] & 64) &&
            !(node_etype[node_val[node]] & 64))
            /* 枝の型が int で全体が double (v46):
             * 枝の値を double に揃える */
            emit_cvt_d(REG_FT0, REG_T1, node_etype[node_val[node]]);
        int e_jump = nasm;
        asm_push(OP_JAL, 0, 0, 0, 0);
        apply_patches(f_head, nasm);
        if (!gen_expr(node_rhs[node]))
            return 0;
        if ((node_etype[node] & 64) &&
            !(node_etype[node_rhs[node]] & 64))
            emit_cvt_d(REG_FT0, REG_T1, node_etype[node_rhs[node]]);
        asm_imm[e_jump] = nasm;
        return 1;
    }
    if (node_kind[node] == NODE_NEG) {
        /* -x: 0 から引く (sub t1, zero, t1)。
         * double なら fneg.d (符号の反転、v46) */
        if (!gen_expr(node_lhs[node]))
            return 0;
        if (node_etype[node] & 64) {
            asm_push(OP_FNEG_D, REG_FT0, REG_FT0, REG_FT0, 0);
            return 1;
        }
        /* 符号つき int32 は subw 1 命令 (v60) */
        if (et_is_s32(node_etype[node])) {
            asm_push(OP_SUBW, REG_T1, REG_ZERO, REG_T1, 0);
        } else {
            asm_push(OP_SUB, REG_T1, REG_ZERO, REG_T1, 0);
            emit_canon(node_etype[node]);
        }
        return 1;
    }
    if (node_kind[node] == NODE_ADDR) {
        /* 単項 &: 変数ならその置き場所の住所、配列なら
         * 先頭アドレス、要素なら要素の住所 (v25) が
         * 値になる */
        int operand = node_lhs[node];
        if (node_kind[operand] == NODE_VAR)
            gen_var_addr(REG_T1, node_val[operand]);
        else if (node_kind[operand] == NODE_ARRAY)
            gen_load_arr_base(REG_T1, node_val[operand]);
        else {
            /* 要素: 住所作りの道具が t2 に住所を置く */
            if (!gen_elem_addr(operand))
                return 0;
            asm_push(OP_ADDI, REG_T1, REG_T2, 0, 0);
        }
        return 1;
    }
    if (node_kind[node] == NODE_PREINC ||
        node_kind[node] == NODE_POSTINC ||
        node_kind[node] == NODE_PREDEC ||
        node_kind[node] == NODE_POSTDEC) {
        /* ++ / --: 変数の住所を t2 に作り、読み書きする。
         * 前置は「増減させてから、その値」、後置は「今の
         * 値を残してから増減」。結果はどちらも t1 に置く。
         * 後置の新しい値は a0 に作る (式の評価の途中では
         * a0 は使われていない。v21 でメモリ化に追随) */
        int delta = (node_kind[node] == NODE_PREINC ||
                     node_kind[node] == NODE_POSTINC) ? 1 : -1;
        int pre = (node_kind[node] == NODE_PREINC ||
                   node_kind[node] == NODE_PREDEC);
        int vet = var_value_etype(node_val[node]);
        /* 歩幅 (v49): ポインタ変数は指す先の大きさだけ
         * 進める。それ以外は従来どおり 1 */
        int vstep = delta;
        int vpes = ptr_elem_size(node);
        if (vpes > 1)
            vstep = delta * vpes;
        if (!(vet & 64) && var_reg[node_val[node]] != 0) {
            /* レジスタ割り付け (v56): 住所を作らず、
             * レジスタの中で増減する。前置は新しい値、
             * 後置は古い値が結果 (t1) になる。後置の
             * 入れ替えの置き場は t2 (住所作りに使って
             * いた置き場で、この経路では空いている)。
             * 大きな歩幅のとき emit_add_step が a0 を
             * 使うが、この経路では a0 は使わない */
            int sreg = var_reg[node_val[node]];
            /* 符号つき int32 は addiw で正規化まで済む (v60) */
            if (et_is_s32(vet) && vstep >= -2048 &&
                vstep <= 2047) {
                asm_push(OP_ADDIW, REG_T1, sreg, 0, vstep);
            } else {
                emit_add_step(REG_T1, sreg, vstep);
                emit_canon(vet);
            }
            if (pre) {
                asm_push(OP_ADDI, sreg, REG_T1, 0, 0);
            } else {
                asm_push(OP_ADDI, REG_T2, sreg, 0, 0);
                asm_push(OP_ADDI, sreg, REG_T1, 0, 0);
                asm_push(OP_ADDI, REG_T1, REG_T2, 0, 0);
            }
            /* ループ最適化 (v57): 歩くポインタを進める */
            loop_xform_on_mod(node);
            return 1;
        }
        gen_var_addr(REG_T2, node_val[node]);
        if (vet & 64) {
            /* double の変数の増減 (v46): 1.0 を ft1 に
             * 作り、fadd.d / fsub.d で増減する。結果の
             * 置き場は ft0 (前置は新しい値、後置は古い
             * 値)。1.0 のビットは定数として作る */
            asm_push(OP_FLD, REG_FT0, REG_T2, 0, 0);
            emit_load_i64(REG_A0, 0x3FF00000L << 32);
            asm_push(OP_FMV_D_X, REG_FT1, REG_A0, 0, 0);
            if (pre) {
                asm_push(delta > 0 ? OP_FADD_D : OP_FSUB_D,
                         REG_FT0, REG_FT0, REG_FT1, 0);
                asm_push(OP_FSD, 0, REG_T2, REG_FT0, 0);
            } else {
                asm_push(delta > 0 ? OP_FADD_D : OP_FSUB_D,
                         REG_FT1, REG_FT0, REG_FT1, 0);
                asm_push(OP_FSD, 0, REG_T2, REG_FT1, 0);
            }
            return 1;
        }
        asm_push((vet & 32) ? mem_load_op(vet) : OP_LD,
                 REG_T1, REG_T2, 0, 0);
        if (pre) {
            if (et_is_s32(vet) && vstep >= -2048 &&
                vstep <= 2047) {
                asm_push(OP_ADDIW, REG_T1, REG_T1, 0, vstep);
            } else {
                emit_add_step(REG_T1, REG_T1, vstep);
                emit_canon(vet);
            }
            asm_push((vet & 32) ? mem_store_op(vet) : OP_SD,
                     0, REG_T2, REG_T1, 0);
        } else {
            emit_add_step(REG_A0, REG_T1, vstep);
            asm_push((vet & 32) ? mem_store_op(vet) : OP_SD,
                     0, REG_T2, REG_A0, 0);
        }
        /* ループ最適化 (v57): 歩くポインタを進める */
        loop_xform_on_mod(node);
        return 1;
    }
    if (node_kind[node] == NODE_EPRE ||
        node_kind[node] == NODE_EPOST) {
        /* 要素への ++ / -- (v25): 変数の版と同じ流れで、
         * 住所は要素の住所作り (gen_elem_addr) が t2 に置く。
         * 読み書きの幅は要素の大きさで決まる (char なら
         * lbu/sb)。後置は元の値を a0 に作ってから書く */
        int operand = node_lhs[node];
        if (!gen_elem_addr(operand))
            return 0;
        int eet = elem_etype(operand);
        if (eet & 64) {
            /* double の要素の増減 (v46): 変数の版と
             * 同じ形で、住所は gen_elem_addr が t2 に
             * 置いたものを使う */
            asm_push(OP_FLD, REG_FT0, REG_T2, 0, 0);
            emit_load_i64(REG_A0, 0x3FF00000L << 32);
            asm_push(OP_FMV_D_X, REG_FT1, REG_A0, 0, 0);
            if (node_kind[node] == NODE_EPRE) {
                asm_push(node_val[node] > 0 ? OP_FADD_D : OP_FSUB_D,
                         REG_FT0, REG_FT0, REG_FT1, 0);
                asm_push(OP_FSD, 0, REG_T2, REG_FT0, 0);
            } else {
                asm_push(node_val[node] > 0 ? OP_FADD_D : OP_FSUB_D,
                         REG_FT1, REG_FT0, REG_FT1, 0);
                asm_push(OP_FSD, 0, REG_T2, REG_FT1, 0);
            }
            return 1;
        }
        asm_push(mem_load_op(eet), REG_T1, REG_T2, 0, 0);
        /* 歩幅 (v49): 要素がポインタなら指す先の
         * 大きさだけ進める (変数の版と同じ) */
        int estep = node_val[node];
        int epes = ptr_elem_size(operand);
        if (epes > 1)
            estep = estep * epes;
        if (node_kind[node] == NODE_EPRE) {
            if (et_is_s32(eet) && estep >= -2048 &&
                estep <= 2047) {
                asm_push(OP_ADDIW, REG_T1, REG_T1, 0, estep);
            } else {
                emit_add_step(REG_T1, REG_T1, estep);
                emit_canon(eet);
            }
            asm_push(mem_store_op(eet), 0, REG_T2, REG_T1, 0);
        } else {
            emit_add_step(REG_A0, REG_T1, estep);
            asm_push(mem_store_op(eet), 0, REG_T2, REG_A0, 0);
        }
        return 1;
    }
    /* 浮動小数点の二項演算 (v46): どちらかの相手が
     * double なら、値の置き場が ft0 になる専用の生成へ */
    if ((node_etype[node_lhs[node]] & 64) ||
        (node_etype[node_rhs[node]] & 64))
        return gen_fp_binop(node);
    /* 二項演算: 右辺が定数なら即値形を先に試す (v55) */
    int im = gen_imm_binop(node);
    if (im < 0)
        return 0;
    if (im)
        return 1;
    /* 両辺を t1 (左) と t2 (右) に揃える */
    if (!gen_binop_operands(node))
        return 0;
    int rhs = node_rhs[node];
    /* ポインタ演算のスケール (v49): + / - の相手の
     * 片方がポインタなら、整数側を指す先の大きさ倍
     * してから合わせる。ポインタ同士の引き算は、
     * 引いた後で大きさで割って要素数にする (pdiff)。
     * ポインタが絡むときは結果がアドレス (か要素数)
     * なので、正規形への切り詰め (emit_canon) は
     * 出さない (幅つき配列名 + 整数でアドレスが
     * 32ビットに切れる事故もここで塞いだ) */
    int lpes = 0;
    int rpes = 0;
    int pdiff = 0;
    if (node_kind[node] == NODE_ADD ||
        node_kind[node] == NODE_SUB) {
        lpes = ptr_elem_size(node_lhs[node]);
        rpes = ptr_elem_size(rhs);
        if (lpes > 0 && rpes == 0)
            emit_scale_r(REG_T2, lpes);
        else if (rpes > 0 && lpes == 0 &&
                 node_kind[node] == NODE_ADD)
            emit_scale_r(REG_T1, rpes);
        else if (lpes > 0 && rpes > 0 &&
                 node_kind[node] == NODE_SUB)
            pdiff = lpes;
    }
    /* 比較の符号 (v35): どちらかが uint64 なら
     * 符号なしで比べる。それ以外は正規形のおかげで
     * 符号付き比較がそのまま正しい */
    int ucmp = (node_etype[node_lhs[node]] == (8 + 16 + 32)) ||
               (node_etype[rhs] == (8 + 16 + 32));
    int cmpop = ucmp ? OP_SLTU : OP_SLT;
    if (node_kind[node] == NODE_ADD) {
        /* 符号つき int32 は addw 1 命令 (v60) */
        int w32 = et_is_s32(node_etype[node]) &&
                  lpes == 0 && rpes == 0;
        asm_push(w32 ? OP_ADDW : OP_ADD,
                 REG_T1, REG_T1, REG_T2, 0);
        if (!w32 && lpes == 0 && rpes == 0)
            emit_canon(node_etype[node]);
    }
    else if (node_kind[node] == NODE_SUB) {
        int w32 = et_is_s32(node_etype[node]) &&
                  lpes == 0 && rpes == 0 && pdiff == 0;
        asm_push(w32 ? OP_SUBW : OP_SUB,
                 REG_T1, REG_T1, REG_T2, 0);
        if (pdiff > 1) {
            /* バイト差 → 要素数: 2の冪なら算術
             * シフト、それ以外は定数との割り算 */
            int e = pdiff;
            int shift = 0;
            while ((e & 1) == 0) {
                e >>= 1;
                shift++;
            }
            if (e == 1) {
                asm_push(OP_SRAI, REG_T1, REG_T1, 0, shift);
            } else {
                gen_load_const(REG_A0, pdiff);
                asm_push(OP_DIV, REG_T1, REG_T1, REG_A0, 0);
            }
        }
        if (!w32 && lpes == 0 && rpes == 0)
            emit_canon(node_etype[node]);
    }
    else if (node_kind[node] == NODE_AND) {
        asm_push(OP_AND, REG_T1, REG_T1, REG_T2, 0);
        /* ビット演算は、正規形同士なら結果も正規形
         * (符号ビット同士の演算が拡張と一致する)。
         * 両辺とも符号つき int32 のときだけ正規化を
         * 省く (v60)。数リテラルは 0 埋めで作られる
         * ことがあり、その場合は従来どおり要る */
        if (!(et_is_s32(node_etype[node]) &&
              et_is_s32(node_etype[node_lhs[node]]) &&
              et_is_s32(node_etype[node_rhs[node]])))
            emit_canon(node_etype[node]);
    }
    else if (node_kind[node] == NODE_MUL) {
        int w32 = et_is_s32(node_etype[node]);
        asm_push(w32 ? OP_MULW : OP_MUL,
                 REG_T1, REG_T1, REG_T2, 0);
        if (!w32)
            emit_canon(node_etype[node]);
    }
    else if (node_kind[node] == NODE_DIV)
        /* 割り算は型の符号で命令を出す (v35)。結果は
         * 正規形の範囲に収まるので正規化は要らない。
         * 符号つき int32 は divw (v60): 64ビットの
         * div では INT_MIN / -1 が溢れて正規形から
         * 外れるが、divw は 32 ビットで折り返す */
        asm_push((node_etype[node] & 48) == 48 ? OP_DIVU :
                 et_is_s32(node_etype[node]) ? OP_DIVW : OP_DIV,
                 REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_MOD)
        asm_push((node_etype[node] & 48) == 48 ? OP_REMU :
                 et_is_s32(node_etype[node]) ? OP_REMW : OP_REM,
                 REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_SHL) {
        int w32 = et_is_s32(node_etype[node]);
        asm_push(w32 ? OP_SLLW : OP_SLL,
                 REG_T1, REG_T1, REG_T2, 0);
        if (!w32)
            emit_canon(node_etype[node]);
    }
    else if (node_kind[node] == NODE_SHR)
        /* >> は型の符号で出す (v35): 符号なしは論理
         * シフト、それ以外は算術シフト (符号を保つ)。
         * 符号つき int32 は sraw (v60、シフト量は
         * 下位5ビット = int32 の意味どおり) */
        asm_push((node_etype[node] & 48) == 48 ? OP_SRL :
                 et_is_s32(node_etype[node]) ? OP_SRAW : OP_SRA,
                 REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_OR) {
        asm_push(OP_OR, REG_T1, REG_T1, REG_T2, 0);
        if (!(et_is_s32(node_etype[node]) &&
              et_is_s32(node_etype[node_lhs[node]]) &&
              et_is_s32(node_etype[node_rhs[node]])))
            emit_canon(node_etype[node]);
    }
    else if (node_kind[node] == NODE_XOR) {
        asm_push(OP_XOR, REG_T1, REG_T1, REG_T2, 0);
        if (!(et_is_s32(node_etype[node]) &&
              et_is_s32(node_etype[node_lhs[node]]) &&
              et_is_s32(node_etype[node_rhs[node]])))
            emit_canon(node_etype[node]);
    }
    else if (node_kind[node] == NODE_LT)
        /* slt: 左 (t1) < 右 (t2) なら 1 */
        asm_push(cmpop, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_GT)
        /* > は両辺を入れ替えた < 。slt の両辺を逆に置く */
        asm_push(cmpop, REG_T1, REG_T2, REG_T1, 0);
    else if (node_kind[node] == NODE_LE ||
             node_kind[node] == NODE_GE) {
        /* <= は「> でない」、>= は「< でない」。
         * 反対の比較をしてから 1 と XOR して反転する
         * (値は 1 か 0 なので、XOR 1 がちょうど反転になる) */
        if (node_kind[node] == NODE_LE)
            asm_push(cmpop, REG_T1, REG_T2, REG_T1, 0);
        else
            asm_push(cmpop, REG_T1, REG_T1, REG_T2, 0);
        asm_push(OP_XORI, REG_T1, REG_T1, 0, 1);
    }
    else {
        /* 比較 (== / !=): まず引き算して「差」にする。
         * 差が 0 なら等しい。あとは差を見て 1 か 0 を作る:
         *   != : sltu t1, zero, t1  (差が 0 でない = 1)
         *   == : sltiu t1, t1, 1    (差が 1 未満 = 差 0 = 1) */
        asm_push(OP_SUB, REG_T1, REG_T1, REG_T2, 0);
        if (node_kind[node] == NODE_NE)
            asm_push(OP_SLTU, REG_T1, REG_ZERO, REG_T1, 0);
        else
            asm_push(OP_SLTIU, REG_T1, REG_T1, 0, 1);
    }
    return 1;
}

/* 二項演算の両辺を、左は t1、右は t2 に揃える。
 * 右が葉ならそのまま t2 へ、式なら左をスタックへ
 * 退避してから右を計算する。gen_expr の二項演算と、
 * 条件分岐の融合 (gen_cond_branch、03-genstmt.c)
 * で共用する (v53) */
/* 式の部分木に呼び出しが含まれるか (v55)。
 * lhs / rhs / next をたどり、三項演算子の枝は
 * node_val も見る。数え落とすより余分に拾う側へ
 * 倒す (迷ったら呼び出しありと見る)。
 * リンクの終端は -1 だけでなく 0 のこともある:
 * 使わないリンクを 0 で埋める作り方が混ざって
 * いるため。0 番のノードは最初に作られる葉
 * (子は自分より前に作られる作法なので、0 番が
 * 呼び出しや式の親になることはない) なので、
 * 0 以下は「ここまで」として安全に止まれる */
static int subtree_has_call(int node) {
    if (node <= 0)
        return 0;
    int k = node_kind[node];
    if (k == NODE_CALL || k == NODE_ICALL)
        return 1;
    if (subtree_has_call(node_lhs[node]))
        return 1;
    if (subtree_has_call(node_rhs[node]))
        return 1;
    if (k == NODE_COND && subtree_has_call(node_val[node]))
        return 1;
    if (subtree_has_call(node_next[node]))
        return 1;
    return 0;
}

/* 二項演算で左の結果を退避する保持レジスタ (v55)。
 * 深さごとに t3, t4, t5, t6 を使う。呼び出しで
 * 壊れるレジスタなので、呼び出しを跨ぐ退避には
 * 使わない (subtree_has_call で振り分ける) */
static int hold_depth;
static int hold_reg(int i) {
    if (i == 0) return REG_T3;
    if (i == 1) return REG_T4;
    if (i == 2) return REG_T5;
    return REG_T6;
}

static int gen_binop_operands(int node) {
    if (!gen_expr(node_lhs[node]))
        return 0;
    int rhs = node_rhs[node];
    if (is_leaf(rhs)) {
        /* 右が葉: そのまま t2 に入れる */
        gen_load_leaf(REG_T2, rhs);
        return 1;
    }
    /* 右も式: 左の結果 (t1) を退避して右を計算し、
     * t2 へ写して左を戻す。退避先はまず保持
     * レジスタ (v55)。右の部分木に呼び出しが
     * あるときと、保持が4段を超えるときは
     * 従来のスタック退避にする */
    if (hold_depth < 4 && !subtree_has_call(rhs)) {
        int hr = hold_reg(hold_depth);
        hold_depth++;
        asm_push(OP_ADDI, hr, REG_T1, 0, 0);
        int ok = gen_expr(rhs);
        hold_depth--;
        if (!ok)
            return 0;
        asm_push(OP_ADDI, REG_T2, REG_T1, 0, 0);
        asm_push(OP_ADDI, REG_T1, hr, 0, 0);
        return 1;
    }
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
    asm_push(OP_SD, 0, REG_SP, REG_T1, 0);
    if (!gen_expr(rhs))
        return 0;
    asm_push(OP_ADDI, REG_T2, REG_T1, 0, 0);
    asm_push(OP_LD, REG_T1, REG_SP, 0, 0);
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
    return 1;
}

/* 定数相手の二項演算を即値形で出す (v55、設計は
 * doc/expr-eval.md)。右辺が数リテラルで即値命令に
 * 収まる形なら、左辺を t1 に作って即値命令で演算
 * する。出したら 1、対象でなければ 0、左辺の生成に
 * 失敗したら -1 を返す。ポインタ演算はスケールの
 * 経路 (v49) を迂回しないので対象外 */
static int gen_imm_binop(int node) {
    int k = node_kind[node];
    if (k != NODE_ADD && k != NODE_SUB && k != NODE_AND &&
        k != NODE_OR && k != NODE_XOR && k != NODE_SHL &&
        k != NODE_SHR && k != NODE_LT && k != NODE_MUL)
        return 0;
    int rhs = node_rhs[node];
    if (node_kind[rhs] != NODE_NUM)
        return 0;
    if (ptr_elem_size(node_lhs[node]) > 0 ||
        ptr_elem_size(rhs) > 0)
        return 0;
    /* 定数の値: node_val は int のビットパターン。
     * 葉の読み込み (gen_load_leaf) と同じく、負の
     * パターンは 32 ビットを 0 埋めで広げた値と見る */
    int v = node_val[rhs];
    long c = (v >= 0) ? (long)v : (long)(unsigned int)v;
    int op = 0;
    int imm = 0;
    int canon = 1;
    int mode = 0;  /* 0 = t1 への即値命令、1 = 0 を置く、
                    * 2 = 演算なし (1 倍) */
    int s32 = et_is_s32(node_etype[node]);
    if (k == NODE_ADD && c <= 2047) {
        /* 符号つき int32 は addiw で正規化まで済む (v60) */
        op = s32 ? OP_ADDIW : OP_ADDI; imm = (int)c;
        if (s32)
            canon = 0;
    } else if (k == NODE_SUB && c <= 2048) {
        op = s32 ? OP_ADDIW : OP_ADDI; imm = (int)-c;
        if (s32)
            canon = 0;
    } else if (k == NODE_AND && c <= 2047) {
        op = OP_ANDI; imm = (int)c;
        /* 即値の符号拡張と左辺の正規形の組み合わせで
         * 結果は常に正規形になる (v60) */
        if (s32)
            canon = 0;
    } else if (k == NODE_OR && c <= 2047) {
        op = OP_ORI; imm = (int)c;
        if (s32)
            canon = 0;
    } else if (k == NODE_XOR && c <= 2047) {
        op = OP_XORI; imm = (int)c;
        if (s32)
            canon = 0;
    } else if (k == NODE_SHL && c <= 63) {
        if (s32 && c <= 31) {
            op = OP_SLLIW; canon = 0;
        } else {
            op = OP_SLLI;
        }
        imm = (int)c;
    } else if (k == NODE_SHR && c <= 63) {
        if (s32 && c <= 31) {
            op = OP_SRAIW;
        } else {
            op = ((node_etype[node] & 48) == 48) ? OP_SRLI : OP_SRAI;
        }
        imm = (int)c; canon = 0;
    } else if (k == NODE_LT && c <= 2047) {
        int ucmp = (node_etype[node_lhs[node]] == (8 + 16 + 32)) ||
                   (node_etype[rhs] == (8 + 16 + 32));
        op = ucmp ? OP_SLTIU : OP_SLTI;
        imm = (int)c; canon = 0;
    } else if (k == NODE_MUL && c == 0) {
        mode = 1;
    } else if (k == NODE_MUL && c == 1) {
        mode = 2;
    } else if (k == NODE_MUL && c > 1 && (c & (c - 1)) == 0) {
        imm = 0;
        while ((1L << imm) < c)
            imm++;
        if (s32) {
            op = OP_SLLIW; canon = 0;
        } else {
            op = OP_SLLI;
        }
    } else {
        return 0;
    }
    if (!gen_expr(node_lhs[node]))
        return -1;
    if (mode == 1)
        asm_push(OP_ADDI, REG_T1, REG_ZERO, 0, 0);
    else if (mode == 0)
        asm_push(op, REG_T1, REG_T1, 0, imm);
    if (canon)
        emit_canon(node_etype[node]);
    return 1;
}

/* 式を受け側の型に合わせて生成する gen_value_as と、
 * 浮動小数点の二項演算 gen_fp_binop の本体は
 * src/03b-genfp.c にある (gen_value_as と gen_fp_binop
 * の前方宣言は上の「関数の前方宣言」の節に既にある)。
 * fp_operand_is_ptr の前方宣言だけここに置く (名前の
 * 登録順を変えないため) */
static int fp_operand_is_ptr(int node);

/* 呼び出しの引数をスタックに積む (v22): リストの後ろ
 * (右の引数) から先に評価して積む。再帰で「残りを先に」
 * 処理するので、右の引数ほど先に積まれ、スタックの上
 * (sp の近く) には左の引数が来る並びになる。取り出す
 * 側は sp + k*16 を a(k) へ読めば、左から順に対応する。
 * 1つ積むごとに 16 バイト (式の評価の退避と同じ単位)。
 * v46: 引数は受け側の型に合わせて変換してから積む
 * (double の引数はビットとして積む。呼び出し規約
 * そのものは変えない)。f は関数の番号 (本体のある
 * 関数だけ引数の型が分かる)、sig は署名の番号。
 * どちらも無ければ変換しない */
static int gen_call_args(int arg, int f, int sig) {
    if (arg < 0)
        return 1;  /* 積み終わり */
    if (!gen_call_args(node_next[arg], f, sig))
        return 0;
    int idx = call_args_idx;
    call_args_idx = idx - 1;
    int pet;
    if (f >= 0 && func_has_body[f] && idx < func_nparams[f])
        pet = var_etype[func_param_var[f][idx]];
    else if (sig >= 0 && idx < sig_nparams[sig])
        pet = sig_petype[sig][idx];
    else
        pet = node_etype[arg];  /* 型が分からなければ変換なし */
    if (!gen_value_as(arg, pet))
        return 0;
    /* 入れ子の呼び出しが call_args_idx を上書きした
     * 分を戻す (次の外側の引数の番号は idx - 1) */
    call_args_idx = idx - 1;
    if (pet & 64)
        /* double の引数はビットとして t1 へ戻して積む */
        asm_push(OP_FMV_X_D, REG_T1, REG_FT0, 0, 0);
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
    asm_push(OP_SD, 0, REG_SP, REG_T1, 0);
    return 1;
}

/* 要素の住所を t2 に作る (v23): 読み (式) と書き込み
 * (文) が共有する道具。相手の種類ごとに:
 *   INDEX / INDEX_STORE: 配列の基底 + 添字 × 要素の大きさ
 *   INDEX2:              基底 + (行 × 列数 + 列) × 要素の
 *                        大きさ。行をスタックに退避して
 *                        から列を計算する
 *   PINDEX:              ポインタの値 + 添字 × 指す先の
 *                        大きさ
 * 要素の大きさが 8 のときの ×8 は slli 3 (左シフト3) */
/* レジスタの値を要素の大きさ倍に伸ばす (v35 で
 * 一般化、v49 でレジスタ一般にした)。大きさが
 * 2 の冪なら左シフト、1 なら何もしない。それ以外は
 * 定数との乗算 (v45 で一般化: 以前は 2 / 4 / 8 しか
 * 扱っておらず、16バイトの構造体の配列などで
 * 添字が倍にならなかった)。乗算の下書きは a0 */
static void emit_scale_r(int reg, int esize) {
    if (esize <= 1)
        return;
    int shift = 0;
    int e = esize;
    while ((e & 1) == 0) {
        e >>= 1;
        shift++;
    }
    if (e == 1) {
        asm_push(OP_SLLI, reg, reg, 0, shift);
    } else {
        gen_load_const(REG_A0, esize);
        asm_push(OP_MUL, reg, reg, REG_A0, 0);
    }
}

/* 添字を要素の大きさ倍に伸ばす (t1 版の呼び名は
 * 従来どおり残す) */
static void emit_scale(int esize) {
    emit_scale_r(REG_T1, esize);
}

/* dst = src + step を出す (v49: ポインタの ++ / --
 * の歩幅用)。即値に収まれば addi、収まらなければ
 * 定数を a0 に作って加減する。dst が a0 のときは
 * 定数を a0 に作ってから src と合わせるので壊れない */
static void emit_add_step(int dst, int src, int step) {
    if (step >= -2048 && step <= 2047) {
        asm_push(OP_ADDI, dst, src, 0, step);
    } else if (step > 0) {
        gen_load_const(REG_A0, step);
        asm_push(OP_ADD, dst, src, REG_A0, 0);
    } else {
        gen_load_const(REG_A0, -step);
        asm_push(OP_SUB, dst, src, REG_A0, 0);
    }
}

/* 構造体のメンバ1つの大きさ (バイト、v37) */
static int member_elem_size(int mi) {
    if (smem_plevel[mi] >= 1)
        return 8;
    if (smem_struct[mi] > 0)
        return stag_size[smem_struct[mi] - 1];
    return smem_etype[mi] & 15;
}

/* ================================================================
 * ループ最適化の生成側 (v57、計画は 03-gen.c の
 * loop_plan。v62 で 03-gen の断片上限の都合で
 * この断片へ引っ越した)
 *
 * 生きている歩くポインタを表に持ち、ループの生成の
 * 頭で初期値を作って積み、末尾で戻す。帰納変数の
 * 変更の生成直後には loop_xform_on_mod が呼ばれ、
 * 記録された変化量 × 要素の大きさだけポインタを
 * 進める。要素の住所作り (gen_elem_addr) は、
 * 生きているポインタに合う要素ならそちらを使う。
 *
 * レジスタ圧の緩和 (v62): 計画が被害変数を貸して
 * いたら、頭でその変数を自分の置き場所へ退避し
 * (loop_xform_begin)、末尾の出口で復帰する
 * (loop_xform_restore。条件の偽と break の飛び先は
 * 復帰の先頭に合わせる)
 * ================================================================ */
static int xact_n;                /* 生きている数 */
static char xact_kind[16];
static int xact_base[16];
static int xact_ind[16];
static char xact_esz[16];
static char xact_reg[16];

/* 帰納変数の変更 (node) の生成直後に呼ばれ、
 * 生きているポインタを歩幅ぶん進める */
static void loop_xform_on_mod(int node) {
    if (xact_n == 0)
        return;
    for (int m = 0; m < xform_nmods; m++) {
        if (xform_mod_node[m] != node)
            continue;
        for (int i = 0; i < xact_n; i++) {
            if (xact_ind[i] != xform_mod_var[m])
                continue;
            int step = xform_mod_delta[m] *
                       xact_esz[i];
            if (step == 0)
                continue;
            if (step >= -2048 && step <= 2047)
                asm_push(OP_ADDI, xact_reg[i],
                         xact_reg[i], 0, step);
            else
                emit_add_step(xact_reg[i],
                              xact_reg[i], step);
        }
    }
}

/* 計画表からループ (node) の番号を探す */
static int xform_find_loop(int node) {
    int li = -1;
    for (int i = 0; i < xform_nloops; i++) {
        if (xform_loop_node[i] == node)
            li = i;
    }
    return li;
}

/* ループの生成の頭で呼ばれ、ポインタの初期値を
 * 作って生きている表へ積む。戻り値は積む前の深さ
 * (loop_xform_end へ渡す)。被害変数の退避 (v62) は
 * ポインタの初期化より先に置く (退避はレジスタから
 * 読むだけで壊さないが、順序の意味を明確にする) */
static int loop_xform_begin(int node) {
    int mark = xact_n;
    int li = xform_find_loop(node);
    if (li < 0)
        return mark;
    for (int j = 0; j < xform_loop_vcnt[li]; j++) {
        int v = xform_vict_var[xform_loop_vfirst[li] + j];
        asm_push(OP_SD, 0, REG_FP, var_reg[v],
                 var_frame_off(v));
    }
    for (int j = 0; j < xform_loop_cnt[li]; j++) {
        int e = xform_loop_first[li] + j;
        int reg = saved_reg_no(xform_ptr_slot[e]);
        if (xform_ptr_kind[e] == 0)
            gen_load_arr_base(reg, xform_ptr_base[e]);
        else
            gen_load_var(reg, xform_ptr_base[e]);
        gen_load_var(REG_T1, xform_ptr_ind[e]);
        emit_scale_r(REG_T1, xform_ptr_esz[e]);
        asm_push(OP_ADD, reg, reg, REG_T1, 0);
        xact_kind[xact_n] = xform_ptr_kind[e];
        xact_base[xact_n] = xform_ptr_base[e];
        xact_ind[xact_n] = xform_ptr_ind[e];
        xact_esz[xact_n] = xform_ptr_esz[e];
        xact_reg[xact_n] = reg;
        xact_n = xact_n + 1;
    }
    return mark;
}

/* ループの生成の末尾: 生きている表を戻す */
static void loop_xform_end(int mark) {
    xact_n = mark;
}

/* ループの出口で呼ばれ、被害変数 (v62) の復帰を
 * 積む。積んだら復帰の先頭の命令番号を返し、
 * 被害変数が無ければ -1 を返す。呼び側は条件の偽と
 * break の飛び先を、戻り値があればそちらへ、
 * 無ければ現在の末尾へ合わせる */
static int loop_xform_restore(int node) {
    int li = xform_find_loop(node);
    if (li < 0 || xform_loop_vcnt[li] == 0)
        return -1;
    int at = nasm;
    for (int j = 0; j < xform_loop_vcnt[li]; j++) {
        int v = xform_vict_var[xform_loop_vfirst[li] + j];
        asm_push(OP_LD, var_reg[v], REG_FP, 0,
                 var_frame_off(v));
    }
    return at;
}

/* 要素ノードの住所を、生きている歩くポインタで
 * 作れるか試す。作れたら t2 に入れて 1 を返す。
 * 添字の形が合わない・オフセットが即値に収まら
 * ない要素は 0 を返し、従来の住所作りに任せる */
static int xform_try_addr(int node) {
    int k = node_kind[node];
    if (k != NODE_INDEX && k != NODE_PINDEX &&
        k != NODE_INDEX_STORE)
        return 0;
    if (xact_n == 0)
        return 0;
    int idx = (k == NODE_INDEX_STORE) ? node_lhs[node]
                                      : node_rhs[node];
    int v = -1;
    int off = 0;
    if (node_kind[idx] == NODE_VAR) {
        v = node_val[idx];
    } else if (node_kind[idx] == NODE_ADD) {
        int l = node_lhs[idx];
        int r = node_rhs[idx];
        if (node_kind[l] == NODE_VAR &&
            node_kind[r] == NODE_NUM) {
            v = node_val[l];
            off = node_val[r];
        } else if (node_kind[r] == NODE_VAR &&
                   node_kind[l] == NODE_NUM) {
            v = node_val[r];
            off = node_val[l];
        } else
            return 0;
    } else if (node_kind[idx] == NODE_SUB) {
        int l = node_lhs[idx];
        if (node_kind[l] == NODE_VAR &&
            node_kind[node_rhs[idx]] == NODE_NUM) {
            v = node_val[l];
            off = -node_val[node_rhs[idx]];
        } else
            return 0;
    } else
        return 0;
    int esz = elem_esize(node);
    if (off > 262143 || off < -262143)
        return 0;  /* 次の掛け算の溢れ避け */
    int boff = off * esz;
    if (boff < -2048 || boff > 2047)
        return 0;
    for (int i = 0; i < xact_n; i++) {
        if (xact_ind[i] != v)
            continue;
        if (xact_esz[i] != esz)
            continue;
        if (k == NODE_PINDEX) {
            if (xact_kind[i] != 1 ||
                xact_base[i] != node_val[node])
                continue;
        } else {
            if (xact_kind[i] != 0 ||
                xact_base[i] != node_val[node])
                continue;
        }
        if (boff == 0)
            asm_push(OP_ADDI, REG_T2, xact_reg[i], 0, 0);
        else
            asm_push(OP_ADDI, REG_T2, xact_reg[i], 0,
                     boff);
        return 1;
    }
    return 0;
}


/* 左辺値の住所を t2 に作る (v45): 構造体の複写の
 * 送り先・送り元に使う。変数は gen_var_addr、メンバと
 * 要素は gen_elem_addr、ポインタ越し (DEREF) は相手の
 * 式の値そのものが住所 */
static int gen_lvalue_addr(int node) {
    int k = node_kind[node];
    if (k == NODE_VAR) {
        gen_var_addr(REG_T2, node_val[node]);
        return 1;
    }
    if (k == NODE_MEMBER || k == NODE_MELEM ||
        k == NODE_INDEX || k == NODE_INDEX2 ||
        k == NODE_PINDEX)
        return gen_elem_addr(node);
    if (k == NODE_DEREF) {
        if (!gen_expr(node_lhs[node]))
            return 0;
        asm_push(OP_ADDI, REG_T2, REG_T1, 0, 0);
        return 1;
    }
    errmsg = "構造体の値の住所が作れません";
    return 0;
}

/* スロットの複写 (v45): 送り元 (t2) から送り先 (a0)
 * へ n スロットを順に写す。カウンタのためのレジスタ
 * 契約を増やさないよう、ループにせず展開する */
static void gen_copy_slots(int n) {
    for (int j = 0; j < n; j++) {
        asm_push(OP_LD, REG_T1, REG_T2, 0, 0);
        asm_push(OP_SD, 0, REG_A0, REG_T1, 0);
        if (j + 1 < n) {
            asm_push(OP_ADDI, REG_T2, REG_T2, 0, 8);
            asm_push(OP_ADDI, REG_A0, REG_A0, 0, 8);
        }
    }
}

/* 構造体の丸ごと代入の右辺の生成 (v45): 送り先の住所
 * が t2 にある状態で呼ばれる。右辺の構造体の値を
 * 送り先へ複写する。右辺が構造体を返す呼び出しの
 * ときは B 方式 (段2) で扱う */
static int gen_struct_copy_rhs(int rhs, int tag) {
    int got = value_struct(rhs);
    if (got == 0) {
        errmsg = "構造体には構造体の値だけが代入できます";
        return 0;
    }
    if (got != tag) {
        errmsg = "構造体の型 (タグ) が合いません";
        return 0;
    }
    if (node_kind[rhs] == NODE_CALL ||
        node_kind[rhs] == NODE_ICALL) {
        /* 構造体を返す呼び出し (v45): 送り先の住所を
         * sret として渡し、直接そこへ書かせる */
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
        asm_push(OP_SD, 0, REG_SP, REG_T2, 0);
        return gen_b_call(rhs, 0);
    }
    /* 送り先の住所 (t2) を退避してから送り元の住所を作る */
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
    asm_push(OP_SD, 0, REG_SP, REG_T2, 0);
    if (!gen_lvalue_addr(rhs))
        return 0;
    asm_push(OP_LD, REG_A0, REG_SP, 0, 0);
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
    gen_copy_slots(struct_slots(tag));
    return 1;
}

/* 構造体の値渡しの引数を積む (v45): 住所を作り、
 * スロットを逆順に積む (積み終わるとスロット0が
 * 一番下、つまり受け側の fp から見て先頭に来る) */
static int gen_push_struct_arg(int node) {
    if (!gen_lvalue_addr(node))
        return 0;
    int n = struct_slots(value_struct(node));
    /* 住所 (t2) を a0 へ移し、末尾のスロットから積む */
    asm_push(OP_ADDI, REG_A0, REG_T2, 0, 0);
    if (n > 1) {
        gen_load_const(REG_T2, (n - 1) * 8);
        asm_push(OP_ADD, REG_A0, REG_A0, REG_T2, 0);
    }
    for (int j = 0; j < n; j++) {
        asm_push(OP_LD, REG_T1, REG_A0, 0, 0);
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
        asm_push(OP_SD, 0, REG_SP, REG_T1, 0);
        if (j + 1 < n)
            asm_push(OP_ADDI, REG_A0, REG_A0, 0, -8);
    }
    return 1;
}

/* B 方式の引数の並びを積む (v45): gen_call_args と
 * 同じく右の引数から積む。構造体の引数はスロット
 * ごとに、スカラは値そのものを積む */
static int gen_push_args_b(int arg, int sig, int ai) {
    if (arg < 0)
        return 1;
    if (!gen_push_args_b(node_next[arg], sig, ai + 1))
        return 0;
    if (sig_pstruct[sig][ai] > 0 && sig_pplevel[sig][ai] == 0) {
        /* 構造体の値渡し */
        if (!check_struct(sig_pstruct[sig][ai], arg))
            return 0;
        if (value_struct(arg) == 0) {
            errmsg = "構造体の引数には構造体の値を渡します";
            return 0;
        }
        if (node_kind[arg] == NODE_CALL ||
            node_kind[arg] == NODE_ICALL) {
            /* 戻り値を引数の置き場へ直接受け取る:
             * 置き場を先に空けておき、そこを sret に
             * して呼ぶ。ただし戻り値は置き場に詰まって
             * (8バイト間隔) 書かれるので、引数領域の
             * 並び (16バイト間隔) に合わせて後ろから
             * 広げ直す */
            int n = struct_slots(sig_pstruct[sig][ai]);
            asm_push(OP_ADDI, REG_SP, REG_SP, 0, -n * 16);
            if (!gen_b_call(arg, 1))
                return 0;
            asm_push(OP_ADDI, REG_A0, REG_SP, 0, 0);
            for (int j = n - 1; j >= 1; j--) {
                asm_push(OP_LD, REG_T1, REG_A0, 0, j * 8);
                asm_push(OP_SD, 0, REG_A0, REG_T1, j * 16);
            }
            return 1;
        }
        return gen_push_struct_arg(arg);
    }
    if (value_struct(arg) > 0) {
        errmsg = "構造体の値は構造体の引数にだけ渡せます";
        return 0;
    }
    if (!check_fsig(sig_pfsig[sig][ai], arg))
        return 0;
    /* 引数は受け側の型に合わせて変換する (v46。
     * double の引数はビットとして積む) */
    if (!gen_value_as(arg, sig_petype[sig][ai]))
        return 0;
    if (sig_petype[sig][ai] & 64)
        asm_push(OP_FMV_X_D, REG_T1, REG_FT0, 0, 0);
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
    asm_push(OP_SD, 0, REG_SP, REG_T1, 0);
    return 1;
}

/* B 方式の呼び出しの生成 (v45): 構造体の引数か
 * 構造体の戻り値が絡む呼び出し。引数は全スロットを
 * スタックに積み (16バイト間隔)、受け側は fp 相対で
 * 自分の枠へ写す。呼び側は積んだ分を戻すだけで、
 * レジスタ渡しはしない。構造体の戻り値は隠し引数
 * sret (戻り値の置き場の住所) を引数領域の先頭
 * スロットで渡す。
 * mode: 0 = sret の値が既に1スロット積んである
 *       1 = sp が戻り値の置き場の先頭 (その値が sret)
 *       2 = 戻り値はスカラで sret は無い
 * 戻るとき sp は入口と同じに戻る。戻り値がスカラなら
 * t1 に入る (A 方式と同じ)。構造体なら値は置き場に
 * あり、a0 = sret のまま */
static int gen_b_call(int node, int mode) {
    int is_call = node_kind[node] == NODE_CALL;
    int sig = is_call ? func_sig[node_val[node]] : node_val[node];
    int args = is_call ? node_lhs[node] : node_rhs[node];
    int has_sret = sig_ret_struct[sig] > 0;
    if (has_sret && mode == 2) {
        errmsg = "内部エラー: 構造体の戻り値に置き場がありません";
        return 0;
    }
    if (!has_sret && mode != 2) {
        errmsg = "内部エラー: sret の無い呼び出しに置き場があります";
        return 0;
    }
    if (mode == 1) {
        /* 置き場の住所 (今の sp) を sret の控えとして積む */
        asm_push(OP_ADDI, REG_T1, REG_SP, 0, 0);
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
        asm_push(OP_SD, 0, REG_SP, REG_T1, 0);
        mode = 0;
    }
    if (!gen_push_args_b(args, sig, 0))
        return 0;
    if (has_sret) {
        /* 控えの sret を引数領域の先頭スロットへ写す:
         * 控えは引数の並びの上にある。間接呼び出しの
         * 呼び先の評価より先に積む (評価は引数領域の
         * 下でだけスタックを使うので、領域は壊れない) */
        int abytes = (call_total_slots(sig) - 1) * 16;
        asm_push(OP_LD, REG_T1, REG_SP, 0, abytes);
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
        asm_push(OP_SD, 0, REG_SP, REG_T1, 0);
    }
    if (is_call) {
        asm_push(OP_JAL, REG_RA, 0, 0, -(node_val[node] + 1));
    } else {
        /* 間接呼び出し: 呼び先の式を引数の後に評価する
         * (A 方式と同じ順序) */
        if (!gen_expr(node_lhs[node]))
            return 0;
        asm_push(OP_JALR, REG_RA, REG_T1, 0, 0);
    }
    /* 引数領域と控えを戻す (sp は入口と同じになる) */
    int rel = call_total_slots(sig) * 16;
    if (mode == 0)
        rel += 16;
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, rel);
    if (!has_sret) {
        /* スカラの戻り値: a0 → t1 + 正規形 (A 方式と同じ。
         * ポインタを返す場合は直さない (v49) */
        asm_push(OP_ADDI, REG_T1, REG_A0, 0, 0);
        if (sig_ret_plevel[sig] == 0 &&
            sig_ret_struct[sig] == 0)
            emit_canon(sig_ret_etype[sig]);
        if (sig_ret_etype[sig] & 64)
            /* double の戻り値 (v46): ビットを ft0 へ */
            asm_push(OP_FMV_D_X, REG_FT0, REG_T1, 0, 0);
    }
    return 1;
}

/* 文の AST から命令列を作る。失敗したら 0。
 *
 * while の作り方 (命令列の並びがループの形そのもの):
 *   L_cond: 条件を計算 (結果は t1)
 *           beq t1, zero, L_end   (0 なら繰り返しを抜ける)
 *           本体の文...
 *           jal zero, L_cond      (条件の判定に戻る)
 *   L_end:
 * 飛び先 (L_cond / L_end) は「命令の番号」で表す。L_end は
 * 本体を作り終えた時点の末尾の番号なので、beq は飛び先を
 * 後から書き直す (asm_imm の書換え)。アセンブラでラベルを
 * 使うのと同じことを、番号と書換えでやっている */

/* 文・switch・関数全体の生成と patch の道具は
 * 03-genstmt.c へ出した (v49、容量対策。
 * 03b-genfp.c の分割と同じ理由)。この断片から
 * あちらを呼ぶことはない */
