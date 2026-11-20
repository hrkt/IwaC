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

/* 「t1 が 0 なら飛ぶ」条件分岐を置く。戻り値は、飛び先
 * (命令番号) を asm_imm に書き込むべき命令の番号:
 * 短い形なら beq 自身、長い形なら後ろの jal の方 */
static int emit_beq_t1z(void) {
    int ord = branch_ord++;
    if (branch_long[ord]) {
        asm_push(OP_BNE, 0, REG_T1, REG_ZERO, nasm + 2);
        int j = nasm;
        asm_push(OP_JAL, 0, 0, 0, 0);
        return j;
    }
    int b = nasm;
    asm_push(OP_BEQ, 0, REG_T1, REG_ZERO, 0);
    return b;
}

/* 生成し終えた命令列を眺め、範囲 (±4KB) の外へ飛ぶ
 * 条件分岐があれば branch_long に印を付ける。
 * 新しい印が付いたら 1 を返す */
static int relax_scan(void) {
    int ord = 0;
    int changed = 0;
    for (int i = 0; i < nasm; i++) {
        if (asm_op[i] == OP_BNE) {
            ord++;  /* 長い形の分岐 (相方は jal) */
            continue;
        }
        if (asm_op[i] != OP_BEQ)
            continue;
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
 * フレーム長は「16 + 枠数 x 8 + ローカル配列の合計」を
 * 16バイト単位に切り上げたもの */
static unsigned int globals_base;  /* gen_program が確定させる */

static int func_frame_size(int f) {
    int size = 16 + func_nlocals[f] * 8 + func_arr_bytes[f];
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
        if (kind == RLOC_STR)
            gen_load_addr(reg, str_addr[idx]);
        else if (kind == RLOC_VAR)
            gen_load_addr(reg, var_global_addr(idx));
        else if (kind == RLOC_ARR)
            gen_load_addr(reg, arr_addr[idx]);
        else
            gen_load_addr(reg, (unsigned int)idx);
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
static void gen_store_var(int var, int reg) {
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

/* 要素の住所を作る道具 (v23)。gen_expr と相互に使う */
static int gen_elem_addr(int node);

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
         *   条件を計算 (t1)
         *   beq t1, zero, L_false
         *   真の式を計算 (t1)
         *   jal zero, L_end
         * L_false: 偽の式を計算 (t1)
         * L_end:
         * 条件の値は分岐で消費されるので退避は不要 */
        if (!gen_expr(node_lhs[node]))
            return 0;
        if (node_etype[node_lhs[node]] & 64)
            emit_fp_bool();  /* double の条件 (v46) */
        int f_jump = emit_beq_t1z();
        if (!gen_expr(node_val[node]))
            return 0;
        if ((node_etype[node] & 64) &&
            !(node_etype[node_val[node]] & 64))
            /* 枝の型が int で全体が double (v46):
             * 枝の値を double に揃える */
            emit_cvt_d(REG_FT0, REG_T1, node_etype[node_val[node]]);
        int e_jump = nasm;
        asm_push(OP_JAL, 0, 0, 0, 0);
        asm_imm[f_jump] = nasm;
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
        asm_push(OP_SUB, REG_T1, REG_ZERO, REG_T1, 0);
        emit_canon(node_etype[node]);
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
        gen_var_addr(REG_T2, node_val[node]);
        int vet = var_value_etype(node_val[node]);
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
        /* 歩幅 (v49): ポインタ変数は指す先の大きさだけ
         * 進める。それ以外は従来どおり 1 */
        int vstep = delta;
        int vpes = ptr_elem_size(node);
        if (vpes > 1)
            vstep = delta * vpes;
        if (pre) {
            emit_add_step(REG_T1, REG_T1, vstep);
            emit_canon(vet);
            asm_push((vet & 32) ? mem_store_op(vet) : OP_SD,
                     0, REG_T2, REG_T1, 0);
        } else {
            emit_add_step(REG_A0, REG_T1, vstep);
            asm_push((vet & 32) ? mem_store_op(vet) : OP_SD,
                     0, REG_T2, REG_A0, 0);
        }
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
            emit_add_step(REG_T1, REG_T1, estep);
            emit_canon(eet);
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
    /* 二項演算: まず左を t1 に */
    if (!gen_expr(node_lhs[node]))
        return 0;
    int rhs = node_rhs[node];
    if (is_leaf(rhs)) {
        /* 右が葉: そのまま t2 に入れる */
        gen_load_leaf(REG_T2, rhs);
    } else {
        /* 右も式: 左の結果 (t1) をスタックへ退避し、右を
         * 計算して t2 へ写し、左を t1 へ戻す */
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
        asm_push(OP_SD, 0, REG_SP, REG_T1, 0);
        if (!gen_expr(rhs))
            return 0;
        asm_push(OP_ADDI, REG_T2, REG_T1, 0, 0);
        asm_push(OP_LD, REG_T1, REG_SP, 0, 0);
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
    }
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
        asm_push(OP_ADD, REG_T1, REG_T1, REG_T2, 0);
        if (lpes == 0 && rpes == 0)
            emit_canon(node_etype[node]);
    }
    else if (node_kind[node] == NODE_SUB) {
        asm_push(OP_SUB, REG_T1, REG_T1, REG_T2, 0);
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
        if (lpes == 0 && rpes == 0)
            emit_canon(node_etype[node]);
    }
    else if (node_kind[node] == NODE_AND) {
        asm_push(OP_AND, REG_T1, REG_T1, REG_T2, 0);
        emit_canon(node_etype[node]);
    }
    else if (node_kind[node] == NODE_MUL) {
        asm_push(OP_MUL, REG_T1, REG_T1, REG_T2, 0);
        emit_canon(node_etype[node]);
    }
    else if (node_kind[node] == NODE_DIV)
        /* 割り算は型の符号で命令を出す (v35)。結果は
         * 正規形の範囲に収まるので正規化は要らない */
        asm_push((node_etype[node] & 48) == 48 ? OP_DIVU : OP_DIV,
                 REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_MOD)
        asm_push((node_etype[node] & 48) == 48 ? OP_REMU : OP_REM,
                 REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_SHL) {
        asm_push(OP_SLL, REG_T1, REG_T1, REG_T2, 0);
        emit_canon(node_etype[node]);
    }
    else if (node_kind[node] == NODE_SHR)
        /* >> は型の符号で出す (v35): 符号なしは論理
         * シフト、それ以外は算術シフト (符号を保つ) */
        asm_push((node_etype[node] & 48) == 48 ? OP_SRL : OP_SRA,
                 REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_OR) {
        asm_push(OP_OR, REG_T1, REG_T1, REG_T2, 0);
        emit_canon(node_etype[node]);
    }
    else if (node_kind[node] == NODE_XOR) {
        asm_push(OP_XOR, REG_T1, REG_T1, REG_T2, 0);
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

static int gen_elem_addr(int node) {
    int k = node_kind[node];
    if (k == NODE_MEMBER) {
        /* メンバの住所 (v37): . なら基底の住所、-> なら
         * 基底の値 (ポインタ) に、メンバのオフセットを足す */
        int mi = node_val[node];
        if (node_rhs[node] == 1) {
            if (!gen_expr(node_lhs[node]))
                return 0;
            asm_push(OP_ADDI, REG_T2, REG_T1, 0, 0);
        } else {
            int b = node_lhs[node];
            if (node_kind[b] == NODE_VAR)
                gen_var_addr(REG_T2, node_val[b]);
            else if (node_kind[b] == NODE_INDEX ||
                     node_kind[b] == NODE_INDEX2 ||
                     node_kind[b] == NODE_PINDEX ||
                     node_kind[b] == NODE_MEMBER ||
                     node_kind[b] == NODE_MELEM ||
                     node_kind[b] == NODE_DEREF) {
                if (!gen_elem_addr(b))
                    return 0;
            } else {
                errmsg = "メンバアクセスの基底が変です";
                return 0;
            }
        }
        if (smem_off[mi] != 0) {
            gen_load_const(REG_A0, smem_off[mi]);
            asm_push(OP_ADD, REG_T2, REG_T2, REG_A0, 0);
        }
        return 1;
    }
    if (k == NODE_MELEM) {
        /* 配列メンバの要素の住所 (v37): メンバの住所 +
         * 添字 × 要素の大きさ。添字の計算で基底 (t2) を
         * 壊さないよう、スタックへ退避する */
        if (!gen_elem_addr(node_lhs[node]))
            return 0;
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
        asm_push(OP_SD, 0, REG_SP, REG_T2, 0);
        if (!gen_expr(node_rhs[node]))
            return 0;
        int esz = member_elem_size(node_val[node]);
        if (esz > 1) {
            int shift = 0;
            int e = esz;
            while ((e & 1) == 0) {
                e >>= 1;
                shift++;
            }
            if (e == 1) {
                asm_push(OP_SLLI, REG_T1, REG_T1, 0, shift);
            } else {
                gen_load_const(REG_A0, esz);
                asm_push(OP_MUL, REG_T1, REG_T1, REG_A0, 0);
            }
        }
        asm_push(OP_LD, REG_T2, REG_SP, 0, 0);
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
        asm_push(OP_ADD, REG_T2, REG_T2, REG_T1, 0);
        return 1;
    }
    if (k == NODE_DEREF) {
        /* *p の住所は、p の値そのもの (v37 で追加。
         * (*p).メンバ の基底に使う) */
        if (!gen_expr(node_lhs[node]))
            return 0;
        asm_push(OP_ADDI, REG_T2, REG_T1, 0, 0);
        return 1;
    }
    if (k == NODE_INDEX2) {
        int a = node_val[node];
        if (!gen_expr(node_lhs[node]))
            return 0;  /* 行 → t1 */
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
        asm_push(OP_SD, 0, REG_SP, REG_T1, 0);
        if (!gen_expr(node_rhs[node])) {
            return 0;  /* 列 → t1 */
        }
        asm_push(OP_LD, REG_T2, REG_SP, 0, 0);
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
        if (arr_cols[a] > 1) {
            gen_load_const(REG_A0, arr_cols[a]);
            asm_push(OP_MUL, REG_T2, REG_T2, REG_A0, 0);
        }
        asm_push(OP_ADD, REG_T1, REG_T2, REG_T1, 0);
        /* t1 = 線形添字 (行 × 列数 + 列) */
        emit_scale(arr_esize[a]);
        gen_load_arr_base(REG_T2, a);
        asm_push(OP_ADD, REG_T2, REG_T2, REG_T1, 0);
        return 1;
    }
    if (k == NODE_PINDEX) {
        /* ポインタの値 (住所) は t2 へ。添字を先に t1 で
         * 計算する (変数の読みが t2 を使うため) */
        if (!gen_expr(node_rhs[node]))
            return 0;
        emit_scale(elem_esize(node));
        gen_load_var(REG_T2, node_val[node]);
        asm_push(OP_ADD, REG_T2, REG_T2, REG_T1, 0);
        return 1;
    }
    /* INDEX (添字は rhs) と INDEX_STORE (添字は lhs)。
     * 形は違うが、住所の作り方は同じ */
    {
        int idx = (k == NODE_INDEX) ? node_rhs[node]
                                    : node_lhs[node];
        if (!gen_expr(idx))
            return 0;
        emit_scale(elem_esize(node));
        gen_load_arr_base(REG_T2, node_val[node]);
        asm_push(OP_ADD, REG_T2, REG_T2, REG_T1, 0);
        return 1;
    }
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
