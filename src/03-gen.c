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

static int asm_op[40960];
static int asm_rd[40960];
static int asm_rs1[40960];
static int asm_rs2[40960];
static int asm_imm[40960];
static int nasm;  /* 並べた命令の数 */

/* 命令を1つ、命令列の末尾に積む */
static void asm_push(int op, int rd, int rs1, int rs2, int imm) {
    int i = nasm++;
    asm_op[i] = op;
    asm_rd[i] = rd;
    asm_rs1[i] = rs1;
    asm_rs2[i] = rs2;
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
static char branch_long[40960]; /* 伸ばす分岐の印 */

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
    if (k == NODE_NUM || k == NODE_VAR || k == NODE_STR ||
        k == NODE_ARRAY)
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
static int obj_prefix_insts;  /* 入口の断片の命令数
                               * (最終パスの値が残る) */
static int obj_flush_site;    /* 入口の kosaba_flush
                               * 呼び出しの命令番号。
                               * 無ければ -1 */
/* 住所作りの穴 (auipc + addi の組) の控え。
 * 種類 (番号は命令番号、idx は対象の番号):
 *   1 = 文字列 (文字列番号)、2 = グローバル変数、
 *   3 = グローバル配列。
 * 絶対アドレス (デバイスの窓口) は数が少ないので
 * 別の小さな表に分ける (値そのものを持つ) */
static int RLOC_STR = 1;
static int RLOC_VAR = 2;
static int RLOC_ARR = 3;
static int obj_reloc_site[8192];
static int obj_reloc_kind[8192];
static int obj_reloc_idx[8192];
static int obj_nreloc;
static int obj_abs_site[256];
static int obj_abs_val[256];
static int obj_nabs;

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
    } else if (obj_nreloc < 8192) {
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

/* ポインタ読み書き (*p) で触る先の大きさ (v27)。
 * 先が変数 (ポインタ) なら elem_esize と同じ決め方:
 * 段数 2 以上が指すのはポインタ自体なので 8、段数 1 は
 * 宣言の基本の大きさ (char* は 1、int* / long* は 8)。
 * 先が配列の名前なら要素の大きさ。絶対アドレス (数) と
 * 文字列は従来どおり 1 バイト (デバイスの窓口や文字の
 * 読み書きが前提のため) */
static int deref_esize(int addr) {
    if (node_kind[addr] == NODE_VAR) {
        int var = node_val[addr];
        if (var_plevel[var] >= 2)
            return 8;
        if (var_plevel[var] == 1)
            return var_bsize[var];
        /* 段数 0 はポインタとして宣言されていない
         * 変数 (代入で作られた変数が文字列を持つ、
         * 昔からの使い方): 従来どおり 1 バイト */
        return 1;
    }
    if (node_kind[addr] == NODE_ARRAY)
        return arr_esize[node_val[addr]];
    return 1;
}

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
    if (var_owner[var] < 0) {
        gen_load_addr_sym(reg, RLOC_VAR, var);
        asm_push(OP_LD, reg, reg, 0, 0);
    } else {
        asm_push(OP_LD, reg, REG_FP, 0, var_frame_off(var));
    }
}

/* レジスタの値を変数に書く (v21)。グローバルの場合は
 * 住所作りに t2 を使う */
static void gen_store_var(int var, int reg) {
    if (var_owner[var] < 0) {
        gen_load_addr_sym(REG_T2, RLOC_VAR, var);
        asm_push(OP_SD, 0, REG_T2, reg, 0);
    } else {
        asm_push(OP_SD, 0, REG_FP, reg, var_frame_off(var));
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
static void gen_load_leaf(int reg, int leaf) {
    int k = node_kind[leaf];
    if (k == NODE_NUM) {
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
        /* 読む幅は指す先の大きさで決める (v27: 以前は
         * 常に1バイトで、int* の読みが切れていた) */
        int op = deref_esize(operand) == 8 ? OP_LD : OP_LBU;
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

/* 呼び出しの引数を積む道具 (v22)。gen_expr と相互に使う */
static int gen_call_args(int arg);

/* 要素の住所を作る道具 (v23)。gen_expr と相互に使う */
static int gen_elem_addr(int node);

/* 式の AST から命令列を作る。結果は t1 に残る約束。
 * 失敗したら 0 を返す (理由は errmsg)。
 *
 * 作り方: 左の子を先に生成する (結果は t1)。右の子が葉
 * なら t2 に入れて演算する。右の子も式 (木) の場合は、
 * 左の結果をスタックに退避してから右を計算し、t2 へ
 * 写して左を戻す (v7 で入れた一般形の評価)。
 * スタックが要るプログラムでは、gen_all が先に sp を
 * 用意する (関数のあるプログラムと同じ仕組み) */
static int gen_expr(int node) {
    if (is_leaf(node)) {
        /* 葉: t1 にその値 (変数なら今の値、文字列なら住所、
         * ポインタ読みなら指された先の1バイト) が入る */
        gen_load_leaf(REG_T1, node);
        return 1;
    }
    if (node_kind[node] == NODE_DEREF) {
        /* 相手が計算式のポインタ読み: 住所を t1 で計算し、
         * その住所から1バイト読む。計算式だと指す先の型が
         * 分からないので、従来どおり1バイトのまま
         * (変数が先の読みは gen_load_leaf が大きさで読む) */
        if (!gen_expr(node_lhs[node]))
            return 0;
        asm_push(OP_LBU, REG_T1, REG_T1, 0, 0);
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
        if (elem_esize(node) == 8)
            asm_push(OP_LD, REG_T1, REG_T2, 0, 0);
        else
            asm_push(OP_LBU, REG_T1, REG_T2, 0, 0);
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
        if (!gen_call_args(node_lhs[node]))
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
        if (!gen_call_args(node_lhs[node]))
            return 0;
        if (nargs > 0) {
            for (int k = 0; k < nargs; k++)
                /* a0〜a3 は番号が連番 (10〜13) */
                asm_push(OP_LD, REG_A0 + k, REG_SP, 0, k * 16);
            asm_push(OP_ADDI, REG_SP, REG_SP, 0, nargs * 16);
        }
        asm_push(OP_JAL, REG_RA, 0, 0, -(node_val[node] + 1));
        asm_push(OP_ADDI, REG_T1, REG_A0, 0, 0);
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
        int f_jump = emit_beq_t1z();
        if (!gen_expr(node_rhs[node]))
            return 0;
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
        int r_jump = emit_beq_t1z();
        asm_push(OP_ADDI, REG_T1, REG_ZERO, 0, 1);
        int e_jump = nasm;
        asm_push(OP_JAL, 0, 0, 0, 0);
        asm_imm[r_jump] = nasm;
        if (!gen_expr(node_rhs[node]))
            return 0;
        asm_push(OP_SLTU, REG_T1, REG_ZERO, REG_T1, 0);
        asm_imm[e_jump] = nasm;
        return 1;
    }
    if (node_kind[node] == NODE_NOT) {
        /* !x: x が 0 のときだけ 1。sltiu t1, t1, 1 が
         * 「1 未満か (符号なし)」=「0 か」になる */
        if (!gen_expr(node_lhs[node]))
            return 0;
        asm_push(OP_SLTIU, REG_T1, REG_T1, 0, 1);
        return 1;
    }
    if (node_kind[node] == NODE_BITNOT) {
        /* ~x (v25): 全ビット反転は -1 との XOR */
        if (!gen_expr(node_lhs[node]))
            return 0;
        asm_push(OP_XORI, REG_T1, REG_T1, 0, -1);
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
        int f_jump = emit_beq_t1z();
        if (!gen_expr(node_val[node]))
            return 0;
        int e_jump = nasm;
        asm_push(OP_JAL, 0, 0, 0, 0);
        asm_imm[f_jump] = nasm;
        if (!gen_expr(node_rhs[node]))
            return 0;
        asm_imm[e_jump] = nasm;
        return 1;
    }
    if (node_kind[node] == NODE_NEG) {
        /* -x: 0 から引く (sub t1, zero, t1) */
        if (!gen_expr(node_lhs[node]))
            return 0;
        asm_push(OP_SUB, REG_T1, REG_ZERO, REG_T1, 0);
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
        asm_push(OP_LD, REG_T1, REG_T2, 0, 0);
        if (pre) {
            asm_push(OP_ADDI, REG_T1, REG_T1, 0, delta);
            asm_push(OP_SD, 0, REG_T2, REG_T1, 0);
        } else {
            asm_push(OP_ADDI, REG_A0, REG_T1, 0, delta);
            asm_push(OP_SD, 0, REG_T2, REG_A0, 0);
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
        int esize = elem_esize(operand);
        if (esize == 1) {
            asm_push(OP_LBU, REG_T1, REG_T2, 0, 0);
        } else {
            asm_push(OP_LD, REG_T1, REG_T2, 0, 0);
        }
        if (node_kind[node] == NODE_EPRE) {
            asm_push(OP_ADDI, REG_T1, REG_T1, 0, node_val[node]);
            if (esize == 1)
                asm_push(OP_SB, 0, REG_T2, REG_T1, 0);
            else
                asm_push(OP_SD, 0, REG_T2, REG_T1, 0);
        } else {
            asm_push(OP_ADDI, REG_A0, REG_T1, 0, node_val[node]);
            if (esize == 1)
                asm_push(OP_SB, 0, REG_T2, REG_A0, 0);
            else
                asm_push(OP_SD, 0, REG_T2, REG_A0, 0);
        }
        return 1;
    }
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
    if (node_kind[node] == NODE_ADD)
        asm_push(OP_ADD, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_SUB)
        asm_push(OP_SUB, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_AND)
        asm_push(OP_AND, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_MUL)
        asm_push(OP_MUL, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_DIV)
        asm_push(OP_DIV, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_MOD)
        asm_push(OP_REM, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_SHL)
        asm_push(OP_SLL, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_SHR)
        /* >> は算術シフト (符号を保つ) */
        asm_push(OP_SRA, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_OR)
        asm_push(OP_OR, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_XOR)
        asm_push(OP_XOR, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_LT)
        /* slt: 左 (t1) < 右 (t2) なら 1 (符号付き比較) */
        asm_push(OP_SLT, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_GT)
        /* > は両辺を入れ替えた < 。slt の両辺を逆に置く */
        asm_push(OP_SLT, REG_T1, REG_T2, REG_T1, 0);
    else if (node_kind[node] == NODE_LE ||
             node_kind[node] == NODE_GE) {
        /* <= は「> でない」、>= は「< でない」。
         * 反対の比較をしてから 1 と XOR して反転する
         * (値は 1 か 0 なので、XOR 1 がちょうど反転になる) */
        if (node_kind[node] == NODE_LE)
            asm_push(OP_SLT, REG_T1, REG_T2, REG_T1, 0);
        else
            asm_push(OP_SLT, REG_T1, REG_T1, REG_T2, 0);
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

/* 呼び出しの引数をスタックに積む (v22): リストの後ろ
 * (右の引数) から先に評価して積む。再帰で「残りを先に」
 * 処理するので、右の引数ほど先に積まれ、スタックの上
 * (sp の近く) には左の引数が来る並びになる。取り出す
 * 側は sp + k*16 を a(k) へ読めば、左から順に対応する。
 * 1つ積むごとに 16 バイト (式の評価の退避と同じ単位) */
static int gen_call_args(int arg) {
    if (arg < 0)
        return 1;  /* 積み終わり */
    if (!gen_call_args(node_next[arg]))
        return 0;
    if (!gen_expr(arg))
        return 0;
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
static int gen_elem_addr(int node) {
    int k = node_kind[node];
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
        if (arr_esize[a] == 8)
            asm_push(OP_SLLI, REG_T1, REG_T1, 0, 3);
        gen_load_arr_base(REG_T2, a);
        asm_push(OP_ADD, REG_T2, REG_T2, REG_T1, 0);
        return 1;
    }
    if (k == NODE_PINDEX) {
        /* ポインタの値 (住所) は t2 へ。添字を先に t1 で
         * 計算する (変数の読みが t2 を使うため) */
        if (!gen_expr(node_rhs[node]))
            return 0;
        if (elem_esize(node) == 8)
            asm_push(OP_SLLI, REG_T1, REG_T1, 0, 3);
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
        if (elem_esize(node) == 8)
            asm_push(OP_SLLI, REG_T1, REG_T1, 0, 3);
        gen_load_arr_base(REG_T2, node_val[node]);
        asm_push(OP_ADD, REG_T2, REG_T2, REG_T1, 0);
        return 1;
    }
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
/* ループの break / continue の飛び先を後から書き直すための
 * 道具 (v20)。飛び先は「そのループの生成が終わるまで」
 * 分からないので、jal を仮置きし、その命令番号をリストに
 * 繋いでおく。リストは patch_next[] で命令同士を繋ぐ
 * (命令番号がそのままリストの要素になる)。
 * ループの入れ子には、深さごとの先頭で対応する */
static int loop_depth;
static int loop_break_head[32];
static int loop_cont_head[32];
static int patch_next[40960];

/* 関数の return の飛び先 (出口は関数の生成の最後まで
 * 分からない) も同じ道具で積む */
static int return_patch_head;
static int gen_in_func;  /* 関数の生成中なら 1 */

/* 仮の jal を置き、番号をリストの先頭に繋ぐ */
static void emit_patch_jump(int *head) {
    int i = nasm;
    asm_push(OP_JAL, 0, 0, 0, 0);
    patch_next[i] = *head;
    *head = i;
}

/* リストに積んだ jal 全部の飛び先を target に書き直す */
static void apply_patches(int head, int target) {
    for (int i = head; i != -1; i = patch_next[i])
        asm_imm[i] = target;
}

static int gen_stmt(int node) {
    int kind = node_kind[node];
    if (kind == NODE_PREINC || kind == NODE_POSTINC ||
        kind == NODE_PREDEC || kind == NODE_POSTDEC ||
        kind == NODE_EPRE || kind == NODE_EPOST) {
        /* 増減文: 式として生成し、結果 (t1) は捨てる
         * (v19。要素への増減は v25 で追加) */
        return gen_expr(node);
    }
    if (kind == NODE_DECL) {
        /* 配列の宣言: 置き場所はコンパイラが静的に確保する
         * だけなので、実行時のコードは何も出ない */
        return 1;
    }
    if (kind == NODE_INDEX_STORE || kind == NODE_ELEM_STORE) {
        /* 要素への書き込み: 要素の住所を t2 に作り
         * (gen_elem_addr)、そこへ値を書く。要素の大きさ
         * が 1 なら sb、8 なら sd (v23)。
         * 書く値が葉なら「住所を t2 → 値を t1 → 書く」。
         * 値も式なら、住所をスタックへ退避してから値を
         * 計算する (値の計算が t2 を使うため)。
         * INDEX_STORE はノード自体が住所の材料で、値は
         * rhs。ELEM_STORE は lhs が要素を表す式ノードで、
         * 値は同じく rhs */
        int addr_node = (kind == NODE_INDEX_STORE) ? node
                                                   : node_lhs[node];
        int val_node = node_rhs[node];
        if (!gen_elem_addr(addr_node))
            return 0;
        int esize = elem_esize(addr_node);
        if (!is_leaf(val_node)) {
            asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
            asm_push(OP_SD, 0, REG_SP, REG_T2, 0);
            if (!gen_expr(val_node))
                return 0;
            asm_push(OP_LD, REG_T2, REG_SP, 0, 0);
            asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
        } else {
            gen_load_leaf(REG_T1, val_node);
        }
        if (esize == 8)
            asm_push(OP_SD, 0, REG_T2, REG_T1, 0);
        else
            asm_push(OP_SB, 0, REG_T2, REG_T1, 0);
        return 1;
    }
    if (kind == NODE_VDECL) {
        /* 変数の宣言だけ: 置き場所は parse で予約済みで、
         * 実行時のコードは何も出ない (v21) */
        return 1;
    }
    if (kind == NODE_ASSIGN) {
        if (!gen_expr(node_lhs[node]))
            return 0;
        /* 結果 (t1) を変数の置き場所へ書く (代入、v21) */
        gen_store_var(node_val[node], REG_T1);
        return 1;
    }
    if (kind == NODE_PUTC) {
        if (!gen_expr(node_lhs[node]))
            return 0;
        if (target_kosaba) {
            /* KosabaOS では U モードから UART のレジスタを
             * 触れないので、putc システムコール (番号 1) で
             * 出す。a0 = 文字、a7 = 1、ecall */
            asm_push(OP_ADDI, REG_A0, REG_T1, 0, 0);
            asm_push(OP_ADDI, REG_A7, REG_ZERO, 0, 1);
            asm_push(OP_ECALL, 0, 0, 0, 0);
            return 1;
        }
        /* sb t1, 0(t0) -- 式の値の下1バイトを UART へ */
        asm_push(OP_SB, 0, REG_T0, REG_T1, 0);
        return 1;
    }
    if (kind == NODE_STORE) {
        /* メモリ書き込み: 先に「書く値」を t1 で計算し、
         * 次に「書き込み先の住所」を t2 に作って書き込む。
         * この順番なのは、住所作りが t1 を壊さないため
         * (値の計算は t2 を下書きに使うことがある)。
         * 書く幅は指す先の大きさで決める (v27: 以前は
         * 常に sb で、int* への書き込みが1バイトに切れて
         * いた。セルフコンパイルの append_file で発覚)。
         * 絶対アドレス (デバイスの窓口) は従来どおり
         * 1バイト */
        if (!gen_expr(node_rhs[node]))
            return 0;
        int addr = node_lhs[node];
        if (node_kind[addr] == NODE_NUM)
            gen_load_addr_sym(REG_T2, 0, node_val[addr]);
        else /* NODE_VAR: 変数の値そのものが住所 (ポインタ) */
            gen_load_var(REG_T2, node_val[addr]);
        if (deref_esize(addr) == 8)
            asm_push(OP_SD, 0, REG_T2, REG_T1, 0);
        else
            asm_push(OP_SB, 0, REG_T2, REG_T1, 0);
        return 1;
    }
    if (kind == NODE_CALL || kind == NODE_SYSCALL3) {
        /* 呼び出し文: 式として生成し、戻り値 (t1) は
         * 捨てる (v22 で呼び出しが式になったための形。
         * syscall3 の文も同じ、v27) */
        return gen_expr(node);
    }
    if (kind == NODE_IF) {
        /* if の作り方:
         *   条件を計算 (結果は t1)
         *   beq t1, zero, L_else (または L_end)  (0 なら飛ぶ)
         *   成り立つときの文...
         *   (else がある場合) jal zero, L_end
         * L_else:
         *   else の文...
         * L_end:
         * 飛び先の書き直しは while と同じやり方 */
        if (!gen_expr(node_lhs[node]))
            return 0;
        int beq_index = emit_beq_t1z();
        int then_body = node_rhs[node];
        for (int s = then_body; s != -1; s = node_next[s]) {
            if (!gen_stmt(s))
                return 0;
        }
        int else_body = node_val[node];
        if (else_body < 0) {
            /* else 無し: ここが L_end */
            asm_imm[beq_index] = nasm;
            return 1;
        }
        /* else あり: 本体を飛び越えるジャンプを置き、
         * beq の飛び先 (else の頭) を確定させる */
        int jal_index = nasm;
        asm_push(OP_JAL, 0, 0, 0, 0);
        asm_imm[beq_index] = nasm;
        for (int s = else_body; s != -1; s = node_next[s]) {
            if (!gen_stmt(s))
                return 0;
        }
        asm_imm[jal_index] = nasm;
        return 1;
    }
    if (kind == NODE_WHILE) {
        int l_cond = nasm;  /* 条件判定の命令番号 */
        if (!gen_expr(node_lhs[node]))
            return 0;
        /* beq t1, zero, L_end -- 飛び先はまだ分からないので
         * 仮に 0 を入れておき、下で書き直す */
        int beq_index = emit_beq_t1z();
        if (loop_depth >= 32) {
            errmsg = "ループの入れ子が深すぎます";
            return 0;
        }
        loop_depth++;
        loop_break_head[loop_depth] = -1;
        loop_cont_head[loop_depth] = -1;
        int body = node_rhs[node];
        for (int s = body; s != -1; s = node_next[s]) {
            if (!gen_stmt(s))
                return 0;
        }
        /* continue の飛び先は条件判定。jal の直前が現在の
         * 末尾なので、continue は「末尾へ飛ぶ = そのまま
         * jal で戻る」の形でも同じだが、番号で揃える */
        apply_patches(loop_cont_head[loop_depth], l_cond);
        /* jal zero, L_cond -- 条件の判定に戻る */
        asm_push(OP_JAL, 0, 0, 0, l_cond);
        /* ここ (末尾) が L_end。beq と break の飛び先を
         * 書き直す */
        asm_imm[beq_index] = nasm;
        apply_patches(loop_break_head[loop_depth], nasm);
        loop_depth--;
        return 1;
    }
    if (kind == NODE_FOR) {
        /* for の作り方 (while への読み替え):
         *   初期化の文 (あれば)
         * L_cond: 条件を計算 (t1)
         *         beq t1, zero, L_end
         *         本体の文...
         * L_step: 更新の文 (あれば。本体の並びの最後に
         *         NODE_FORSTEP で包んで繋いである)
         *         jal zero, L_cond
         * L_end:
         * continue の飛び先は L_step (更新を飛ばすと
         * 終わらないループになるため) */
        if (node_val[node] >= 0) {
            if (!gen_stmt(node_val[node]))
                return 0;
        }
        int l_cond = nasm;
        if (!gen_expr(node_lhs[node]))
            return 0;
        int beq_index = emit_beq_t1z();
        if (loop_depth >= 32) {
            errmsg = "ループの入れ子が深すぎます";
            return 0;
        }
        loop_depth++;
        loop_break_head[loop_depth] = -1;
        loop_cont_head[loop_depth] = -1;
        int cont_done = 0;
        int body = node_rhs[node];
        for (int s = body; s != -1; s = node_next[s]) {
            if (node_kind[s] == NODE_FORSTEP) {
                apply_patches(loop_cont_head[loop_depth], nasm);
                cont_done = 1;
                if (!gen_stmt(node_val[s]))
                    return 0;
            } else {
                if (!gen_stmt(s))
                    return 0;
            }
        }
        if (!cont_done)
            /* 更新が無い for: continue は条件判定へ直行 */
            apply_patches(loop_cont_head[loop_depth], l_cond);
        asm_push(OP_JAL, 0, 0, 0, l_cond);
        asm_imm[beq_index] = nasm;
        apply_patches(loop_break_head[loop_depth], nasm);
        loop_depth--;
        return 1;
    }
    if (kind == NODE_BREAK) {
        /* 脱出: 飛び先はループの生成の最後に書き直す */
        if (loop_depth == 0) {
            errmsg = "break はループの中でだけ使えます";
            return 0;
        }
        emit_patch_jump(&loop_break_head[loop_depth]);
        return 1;
    }
    if (kind == NODE_CONTINUE) {
        if (loop_depth == 0) {
            errmsg = "continue はループの中でだけ使えます";
            return 0;
        }
        emit_patch_jump(&loop_cont_head[loop_depth]);
        return 1;
    }
    if (kind == NODE_RETURN) {
        /* return: 値があれば式を計算して a0 (戻り値の
         * 場所) へ置き、関数の出口 (後始末) へ跳ぶ。
         * 出口の番号は関数の生成の最後に書き直す (v20)。
         * 戻り値つきは v22 */
        if (!gen_in_func) {
            errmsg = "return は関数の中でだけ使えます";
            return 0;
        }
        if (node_lhs[node] >= 0) {
            if (!gen_expr(node_lhs[node]))
                return 0;
            asm_push(OP_ADDI, REG_A0, REG_T1, 0, 0);
        }
        emit_patch_jump(&return_patch_head);
        return 1;
    }
    errmsg = "内部エラー: 知らない種類の文です";
    return 0;
}

/* スタックの天井の住所 (第2層が2パス間で確定させる)。
 * スタックはバイナリの後ろの空き地に置き、下へ伸ばす */
static unsigned int stack_top_addr;

/* main の argc / argv (v29): KosabaOS が入口の約束で
 * a0 = argc、a1 = argv を渡すようになった (OS 側
 * 2aa149b) ので、入口はレジスタをそのまま main へ
 * 渡す。v22 の決め打ち argv (静的領域に組み立てる
 * 方式) は廃止し、argv のための静的領域も無くなった */

/* 関数1つ分の命令列を作る: 入口の決まり文句 (プロローグ)
 * → 本体 → 出口の決まり文句 (エピローグ)。
 *   プロローグ: スタックに場所を取り、戻り先 ra を退避。
 *               引数があれば a0 から引数の変数へ写す
 *   エピローグ: ra をスタックから戻し、場所を返して、
 *               jalr で呼び出し側へ戻る
 * ra を退避するのが肝心で、これがあるから関数の中で
 * さらに別の関数を呼んでも (ra が上書きされても)
 * 自分の戻り先を失わない。失敗したら 0 */
static int gen_func(int f) {
    /* オブジェクト出力 (v31) では、宣言だけの関数に
     * 中身を作らない (一括では空の体ができるが、
     * オブジェクトでは「定義は別ファイル」が普通で、
     * 空の体が場所を取ると配置がずれる) */
    if (obj_mode && !func_has_body[f]) {
        func_entry[f] = nasm;
        return 1;
    }
    func_entry[f] = nasm;
    int frame = func_frame_size(f);
    /* 序文 (v21): フレームを取り、ra と呼び出し側の fp を
     * 退避し、fp を張る。ローカルと引数は fp 相対で触る
     * ので、式の評価で sp が上下しても住所が変わらない。
     *   addi sp, sp, -frame
     *   sd ra, frame-8(sp) / sd fp, frame-16(sp)
     *   addi fp, sp, frame */
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, -frame);
    asm_push(OP_SD, 0, REG_SP, REG_RA, frame - 8);
    asm_push(OP_SD, 0, REG_SP, REG_FP, frame - 16);
    asm_push(OP_ADDI, REG_FP, REG_SP, 0, frame);
    /* 引数を受け取る: a0〜a3 を引数の枠へ順に書く (v22) */
    for (int k = 0; k < func_nparams[f]; k++)
        asm_push(OP_SD, 0, REG_FP, REG_A0 + k,
                 var_frame_off(func_param_var[f][k]));
    return_patch_head = -1;
    gen_in_func = 1;
    for (int s = func_body[f]; s != -1; s = node_next[s]) {
        if (!gen_stmt(s))
            return 0;
    }
    gen_in_func = 0;
    /* 早期 return の飛び先はここ (後始末の頭) */
    apply_patches(return_patch_head, nasm);
    /* 後始末: ra と fp を戻し (fp の読みは古い fp で
     * 行う)、フレームを返して、呼び出し側へ戻る
     *   ld ra, -8(fp) / ld fp, -16(fp) / addi sp, sp, frame
     *   jalr zero, ra, 0 */
    asm_push(OP_LD, REG_RA, REG_FP, 0, -8);
    asm_push(OP_LD, REG_FP, REG_FP, 0, -16);
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, frame);
    asm_push(OP_JALR, 0, REG_RA, 0, 0);
    return 1;
}

/* スタックが要るかどうかを、AST を歩いて先に調べる。
 * 要るのは「関数がある」か「右の子が葉でない二項演算が
 * ある (評価に退避が要る)」場合。その場合だけ gen_all が
 * sp の用意を出すので、要らないプログラムの出力は
 * 従来と1バイトも変わらない */
static int stack_needed;

static int expr_needs_stack(int node) {
    int k = node_kind[node];
    if (k == NODE_ADD || k == NODE_SUB || k == NODE_AND ||
        k == NODE_EQ || k == NODE_NE || k == NODE_MUL ||
        k == NODE_DIV || k == NODE_LT || k == NODE_GT ||
        k == NODE_LE || k == NODE_GE || k == NODE_MOD ||
        k == NODE_SHL || k == NODE_SHR || k == NODE_OR ||
        k == NODE_XOR) {
        if (!is_leaf(node_rhs[node]))
            return 1;
        return expr_needs_stack(node_lhs[node]) ||
               expr_needs_stack(node_rhs[node]);
    }
    if (k == NODE_LAND || k == NODE_LOR)
        /* 短絡評価は左を退避しないが、中の式が退避を
         * 要することはある */
        return expr_needs_stack(node_lhs[node]) ||
               expr_needs_stack(node_rhs[node]);
    if (k == NODE_NOT || k == NODE_NEG || k == NODE_ADDR ||
        k == NODE_BITNOT)
        return expr_needs_stack(node_lhs[node]);
    if (k == NODE_COND)
        /* 三項 (v25): 条件と両枝のどこかで退避が要るか */
        return expr_needs_stack(node_lhs[node]) ||
               expr_needs_stack(node_val[node]) ||
               expr_needs_stack(node_rhs[node]);
    if (k == NODE_EPRE || k == NODE_EPOST)
        /* 要素の ++ / -- (v25): 住所作りの中の式を見る */
        return expr_needs_stack(node_lhs[node]);
    if (k == NODE_CALL || k == NODE_SYSCALL3) {
        /* 呼び出しの引数の式が退避を要するか (v22)。
         * syscall3 も引数の並び方は同じ (v27) */
        for (int a = node_lhs[node]; a != -1; a = node_next[a]) {
            if (expr_needs_stack(a))
                return 1;
        }
        return 0;
    }
    if (k == NODE_DEREF)
        return expr_needs_stack(node_lhs[node]);
    if (k == NODE_INDEX)
        return expr_needs_stack(node_rhs[node]);
    if (k == NODE_INDEX2)
        /* 2次元の住所作りは行をスタックに退避する (v23) */
        return 1;
    if (k == NODE_ROWADDR || k == NODE_PINDEX)
        return expr_needs_stack(node_rhs[node]);
    return 0;
}

static int list_needs_stack(int head);  /* 相互に使うので先行宣言 */

static int stmt_needs_stack(int node) {
    int k = node_kind[node];
    if (k == NODE_ASSIGN || k == NODE_PUTC)
        return expr_needs_stack(node_lhs[node]);
    if (k == NODE_WHILE)
        return expr_needs_stack(node_lhs[node]) ||
               list_needs_stack(node_rhs[node]);
    if (k == NODE_IF)
        return expr_needs_stack(node_lhs[node]) ||
               list_needs_stack(node_rhs[node]) ||
               (node_val[node] >= 0 &&
                list_needs_stack(node_val[node]));
    if (k == NODE_CALL) {
        /* 引数の式の並びのどれかが退避を要するか (v22) */
        for (int a = node_lhs[node]; a != -1; a = node_next[a]) {
            if (expr_needs_stack(a))
                return 1;
        }
        return 0;
    }
    if (k == NODE_FOR)
        /* 初期化・更新は文、本体は文の並び、条件は式 */
        return (node_val[node] >= 0 &&
                stmt_needs_stack(node_val[node])) ||
               expr_needs_stack(node_lhs[node]) ||
               list_needs_stack(node_rhs[node]);
    if (k == NODE_FORSTEP)
        return stmt_needs_stack(node_val[node]);
    if (k == NODE_STORE)
        return expr_needs_stack(node_rhs[node]);
    if (k == NODE_INDEX_STORE)
        /* 値が葉でない書き込みは、住所の退避にスタックを使う */
        return expr_needs_stack(node_lhs[node]) ||
               !is_leaf(node_rhs[node]) ||
               expr_needs_stack(node_rhs[node]);
    if (k == NODE_ELEM_STORE)
        /* lhs は要素を表す式ノード (式として数える)。
         * 値が葉でない書き込みは住所の退避にスタックを
         * 使う。2次元なら住所作り自体も退避する (v23) */
        return expr_needs_stack(node_lhs[node]) ||
               !is_leaf(node_rhs[node]) ||
               expr_needs_stack(node_rhs[node]);
    return 0;
}

static int list_needs_stack(int head) {
    for (int s = head; s != -1; s = node_next[s]) {
        if (stmt_needs_stack(s))
            return 1;
    }
    return 0;
}

/* 命令列を全部作る (1回分): 土台 (UART の住所) → (関数が
 * あれば) スタックの用意 → 文の並び → 最後の式 (あれば) →
 * UART へ書く → 止まる → その後に関数の本体を並べる。
 * 関数の本体は「止まる」の後ろに置くので、上から実行して
 * 関数の中に落ちてくることはない (関数は呼び出しでのみ入る)。
 * 最後に、呼び出しの飛び先 (負数の印) を関数の入口番号へ
 * 書き直す。失敗したら 0 */
static int gen_all(void) {
    nasm = 0;
    obj_nreloc = 0;    /* 穴の控えも数え直す (v31) */
    obj_nabs = 0;
    obj_flush_site = -1;
    loop_depth = 0;    /* 2パス生成のたびに数え直す (v20) */
    branch_ord = 0;    /* 条件分岐の通し番号も数え直す (v27) */
    gen_in_func = 0;
    if (!target_kosaba) {
        /* lui t0, 0x10000 -- UART ベース 0x10000000 を t0 に作る
         * (KosabaOS ターゲットでは UART を直接触らないので不要) */
        asm_push(OP_LUI, REG_T0, 0, 0, 0x10000);
    }
    if (stack_needed && !target_kosaba) {
        /* スタックの用意: 天井の住所を sp に作る
         * (要らないプログラムでは出さない。昔のプログラムの
         * 出力が変わらないようにするため。KosabaOS では
         * スタックは OS が用意するので、これも出さない) */
        gen_load_addr(REG_SP, stack_top_addr);
    }
    for (int s = prog_stmts; s != -1; s = node_next[s]) {
        if (!gen_stmt(s))
            return 0;
    }
    if (prog_has_final) {
        if (!gen_expr(prog_final))
            return 0;
        if (target_kosaba) {
            /* 最後の式の出力も putc システムコールで */
            asm_push(OP_ADDI, REG_A0, REG_T1, 0, 0);
            asm_push(OP_ADDI, REG_A7, REG_ZERO, 0, 1);
            asm_push(OP_ECALL, 0, 0, 0, 0);
        } else {
            /* sb t1, 0(t0) -- 最後の式の下1バイトを UART へ */
            asm_push(OP_SB, 0, REG_T0, REG_T1, 0);
        }
    }
    if (prog_main >= 0) {
        /* main の形 (v12): 入口として main を呼ぶ。
         * 飛び先は通常の呼び出しと同じ印 (-(番号+1)) で置き、
         * この後の一括書き直しで確定する。main から戻ると
         * ra の指す次の命令 = 終わりの命令に落ちる。
         * main の入口では、戻り先が ra に入っているのを
         * 通常の関数と同じく退避するので、呼び出しの作法
         * そのままで動く */
        /* main の引数 (v29): KosabaOS では入口の a0 に
         * argc、a1 に argv が入って届く (OS の約束)。
         * 関数の作法でも引数は a0 から順なので、触らずに
         * main を呼べば main のプロローグが枠へ写す。
         * ベアメタルでは引数の届け先が無いので、main に
         * 引数があっても値は不定 (従来どおり) */
        asm_push(OP_JAL, REG_RA, 0, 0, -(prog_main + 1));
        if (target_kosaba) {
            /* main から戻った後 (v29): 終了ステータス
             * (OS 側 d28c7b8 で exit の a0 = ステータスに
             * なった) として、main の戻り値 (t1) を
             * exit の a0 へ渡す。
             * kosaba_flush の約束 (v27): プログラムが
             * kosaba_flush という名前の関数を持っていれば、
             * exit の前に1回呼ぶ。kosaba/ の crt が GCC 版
             * の iwac で呼んでいるのと同じ約束で、
             * ランタイム側の「終了時にやること」(出力
             * バッファの書き切りなど) の置き場所にする。
             * 呼ぶ場合は、戻り値が呼び出しで消えないよう
             * 先にスタックへ退避してから呼ぶ */
            int flush_f = -1;
            for (int f = 0; f < nfunc; f++) {
                if (!func_has_body[f])
                    continue;
                if (func_name_len[f] != 12)
                    continue;
                /* "kosaba_flush" と1文字ずつ比べる
                 * (文字列リテラルの添字は iwac の言語に
                 * 無いので、この形で書く) */
                if (func_names[f][0] == 'k' &&
                    func_names[f][1] == 'o' &&
                    func_names[f][2] == 's' &&
                    func_names[f][3] == 'a' &&
                    func_names[f][4] == 'b' &&
                    func_names[f][5] == 'a' &&
                    func_names[f][6] == '_' &&
                    func_names[f][7] == 'f' &&
                    func_names[f][8] == 'l' &&
                    func_names[f][9] == 'u' &&
                    func_names[f][10] == 's' &&
                    func_names[f][11] == 'h') {
                    flush_f = f;
                    break;
                }
            }
            /* オブジェクト出力 (v31) では、flush が
             * 「このファイルで宣言されている」場合も
             * 呼び出しの形を残す (定義は別ファイルが
             * 持つ。宣言も無いファイルは、flush の
             * ない一括版と出力が一致するよう、従来
             * どおり mv だけにする) */
            int flush_decl = -1;
            if (obj_mode && flush_f < 0) {
                char *want = "kosaba_flush";
                for (int f = 0; f < nfunc && flush_decl < 0; f++) {
                    if (func_name_len[f] != 12)
                        continue;
                    int same = 1;
                    for (int k = 0; k < 12; k++)
                        if (func_names[f][k] != want[k])
                            same = 0;
                    if (same)
                        flush_decl = f;
                }
            }
            if (flush_f >= 0 || flush_decl >= 0) {
                asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
                asm_push(OP_SD, 0, REG_SP, REG_T1, 0);
                /* オブジェクト出力 (v31) では、この
                 * ファイルに flush が無くても呼び出し
                 * の形を残す (相手は別ファイルかも
                 * しれない)。行き先はダミーの印
                 * -1000003 で置き、リンカへのメモは
                 * callopt になる */
                asm_push(OP_JAL, REG_RA, 0, 0,
                         flush_f >= 0 ? -(flush_f + 1)
                                      : -1000003);
                if (obj_mode)
                    obj_flush_site = nasm - 1;
                asm_push(OP_LD, REG_A0, REG_SP, 0, 0);
                asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
            } else {
                /* mv a0, t1 -- main の戻り値がステータス */
                asm_push(OP_ADDI, REG_A0, REG_T1, 0, 0);
            }
        }
    }
    if (target_kosaba) {
        /* KosabaOS: exit システムコール (番号 3) で終わる。
         * 戻ってくることはない。シェルから起動された場合は
         * シェルへ、'u' で起動された場合はカーネルへ戻る。
         * ステータス (a0) は、main のあるプログラムでは
         * main の戻り値が入れてある (上の v29 の処理)。
         * main の無いプログラムは 0 で終わる */
        if (prog_main < 0)
            asm_push(OP_ADDI, REG_A0, REG_ZERO, 0, 0);
        asm_push(OP_ADDI, REG_A7, REG_ZERO, 0, 3);
        asm_push(OP_ECALL, 0, 0, 0, 0);
    } else {
        /* wfi -- 止まる */
        asm_push(OP_WFI, 0, 0, 0, 0);
    }
    /* 入口の断片はここまで (オブジェクト出力が
     * 先頭の長さとして使う。v31) */
    obj_prefix_insts = nasm;
    for (int f = 0; f < nfunc; f++) {
        if (!gen_func(f))
            return 0;
    }
    /* 呼び出しの飛び先を書き直す (印: -(関数番号+1))。
     * オブジェクト出力では書き直さず、印のまま
     * リンカへのメモ (call) にする */
    if (!obj_mode) {
        for (int i = 0; i < nasm; i++) {
            if (asm_op[i] == OP_JAL && asm_imm[i] < 0)
                asm_imm[i] = func_entry[-asm_imm[i] - 1];
        }
    }
    return 1;
}

/* プログラム全体の生成。文字列の住所を確定させるため、
 * 生成を2回行う (2パス生成):
 *   1回目: 命令の個数を数えることが目的。この時点では
 *          文字列の住所は仮の値だが、住所を作る命令の
 *          個数 (auipc + addi) は本物と同じになる
 *   住所の確定: 静的データの先頭 (data_base) を決める。
 *          ベアメタルではコードの後ろ (LOAD_BASE +
 *          命令数 x 4)、KosabaOS ではデータ領域
 *          0x40200000 (v24)。文字列・配列・グローバルは
 *          data_base からの並びで住所が決まる
 *   2回目: 確定した住所で作り直す。これが最終成果になる */
static int gen_program(void) {
    code_insts = 0;
    /* 宣言だけで定義のない関数が呼ばれていたら、ここで
     * 止める (v24)。行き先のない呼び出しは作れない。
     * 外部の関数は、ソースの連結で定義が一緒に入って
     * くる形でのみ解決する (v26 のランタイム結合)。
     * オブジェクト出力 (v31) では止めない: 定義は
     * 別ファイルが持っているのが普通で、点検は
     * リンカの仕事になる (メモは undef として出す) */
    if (!obj_mode) {
        for (int f = 0; f < nfunc; f++) {
            if (func_called[f] && !func_has_body[f]) {
                errmsg = "定義のない関数が呼ばれています (宣言だけの関数です)";
                return 0;
            }
        }
    }
    /* スタックが要るかは生成の前に AST から決める */
    stack_needed = nfunc > 0 || list_needs_stack(prog_stmts) ||
                   (prog_has_final && expr_needs_stack(prog_final));
    for (int f = 0; f < nfunc && !stack_needed; f++)
        stack_needed = list_needs_stack(func_body[f]);
    stack_top_addr = LOAD_BASE;  /* 1回目は仮の値 (個数は同じ) */
    globals_base = LOAD_BASE;    /* 同上 (グローバルの住所) */
    for (int i = 0; i < narr; i++)
        arr_addr[i] = LOAD_BASE;  /* 同上 (配列の住所も仮置き) */
    /* 条件分岐の範囲の点検 (v27): 届かない分岐は長い形に
     * 伸ばす。伸ばすと別の分岐が届かなくなることがある
     * ので、印が増えなくなるまで1回目の生成を繰り返す
     * (emit_beq_t1z の説明参照) */
    for (int i = 0; i < 40960; i++)
        branch_long[i] = 0;
    for (;;) {
        if (!gen_all())
            return 0;
        if (!relax_scan())
            break;
    }
    code_insts = nasm;
    if (obj_mode && (obj_nreloc < 0 || obj_nabs < 0)) {
        errmsg = "穴のメモが多すぎます (オブジェクト出力の上限)";
        return 0;
    }
    /* 静的データ (文字列 → 配列 → グローバル) の
     * 置き場所の先頭 (v24 でターゲット別になった):
     *   ベアメタル: コードの後ろ (従来どおり連続配置)
     *   KosabaOS  : OS のデータ領域 0x40200000。コード
     *               領域 (0x40000000、読み・実行のみ) と
     *               分けることで、静的データへの書き
     *               込みが OS 上でもできるようになる
     *               (ELF は PT_LOAD 3本で申告する) */
    unsigned int data_base;
    if (target_kosaba)
        data_base = 0x40200000u;
    else
        data_base = LOAD_BASE + (unsigned int)nasm * 4u;
    for (int i = 0; i < nstr; i++)
        str_addr[i] = data_base + (unsigned int)str_off[i];
    /* グローバル配列の領域は文字列の後ろに置く。
     * ローカル配列はフレームの中なので住所は持たない
     * (gen_load_arr_base が fp 相対で作る、v26) */
    for (int i = 0; i < narr; i++)
        arr_addr[i] = arr_owner[i] < 0
            ? data_base + (unsigned int)str_used +
              (unsigned int)arr_off[i]
            : 0u;
    /* グローバル変数の領域は配列の後ろ (v21)。
     * 8 バイトずつ。初期値のあるものは encode が初期
     * イメージとして埋め、無いものは 0 で始まる
     * (kosaba では memsz の差分としてゼロ埋めされる。
     * ベアメタルでは初期イメージが無い場合、RAM の
     * 更地なので 0 とは限らない。プログラムは代入
     * してから使う約束) */
    globals_base = data_base + (unsigned int)str_used +
                   (unsigned int)arr_used;
    {
        /* スタックは静的領域の後ろ 4KB 先から下へ伸ばす。
         * 16 バイト単位に切り上げておく (置き場所の約束)。
         * 静的領域の末尾はグローバルの後ろ (v29 で argv
         * の領域が無くなった) */
        unsigned int tail = globals_base +
                            (unsigned int)var_nglobal * 8u;
        stack_top_addr = tail + 4096u;
        stack_top_addr = (stack_top_addr + 15u) & ~15u;
    }
    if (!gen_all())
        return 0;
    return 1;
}

