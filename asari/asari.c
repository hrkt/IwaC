/* ================================================================
 * Asari: iwac の第3層 (encode) の独立プログラム
 *
 * 入力は iwac --asari が出す命令列のテキスト (.ias)、
 * 出力はオブジェクト (.iwo) で、標準出力へ書く。
 * 設計は doc/asari.md。仕事は3つ:
 *   1. 命令列を読む (op は名前、穴は注記で別に来る)
 *   2. 命令を符号化する (RISC-V。ここだけがビット列を
 *      知っている。iwac 本体の src/04-encode.c の移植)
 *   3. データ側のレコードを写し、穴の位置を命令番号から
 *      バイト位置に直して .iwo を出す
 * 物差しは、iwac 内部の encode が出す .iwo との
 * バイト一致。
 *
 * このプログラムも iwac の言語の範囲で書いてあり、
 * ホストの gcc でも、iwac --kosaba でも訳せる
 * (nori と同じ作り)。外に頼るのは write / open /
 * read / close の4つだけ
 * ================================================================ */

/* 外部関数の宣言 (ヘッダの代わりに自分で書く) */
long write(int fd, char *buf, long len);
int open(char *path, int flags);
long read(int fd, char *buf, long len);
int close(int fd);

/* libdashi (共通関数の束、doc/libdashi.md) */
int ds_streq(char *a, char *b);
int ds_strcpy(char *dst, char *src);
void ds_puts(int fd, char *s);
int ds_hexval(int c);
int ds_strlen(char *s);

/* ================= 大きさの約束 ================= */
static int MAX_INST = 40960;   /* 命令の数 (iwac 本体と同じ) */
static int MAX_CALL = 8192;    /* call / callopt 注記の数 */
static int MAX_ADDR = 8192;    /* addr 注記の数 */
static int MAX_ABS = 256;      /* addabs 注記の数 */
static int MAX_REC = 8192;     /* データ側レコードの数 */
static int MAX_UNDEF = 1024;   /* undef の数 */

/* ================= 命令の番号 =================
 * .ias では op は名前で来る。番号は iwac 本体の
 * OP_* と同じ値にしておく (符号化の移植を素直に
 * するため。外に出るのは名前だけ) */
static int OP_LUI = 0;
static int OP_ADDI = 1;
static int OP_ADD = 2;
static int OP_SUB = 3;
static int OP_SB = 4;
static int OP_WFI = 5;
static int OP_LBU = 6;
static int OP_AUIPC = 7;
static int OP_BEQ = 8;
static int OP_JAL = 9;
static int OP_SD = 10;
static int OP_LD = 11;
static int OP_JALR = 12;
static int OP_SLTU = 13;
static int OP_SLTIU = 14;
static int OP_AND = 15;
static int OP_MUL = 16;
static int OP_DIV = 17;
static int OP_SLT = 18;
static int OP_XORI = 19;
static int OP_ECALL = 20;
static int OP_SLL = 21;
static int OP_SRA = 22;
static int OP_OR = 23;
static int OP_XOR = 24;
static int OP_REM = 25;
static int OP_SLLI = 26;
static int OP_BNE = 27;
static int OP_ADDIW = 28;
static int OP_SRLI = 29;
static int OP_LH = 30;
static int OP_LHU = 31;
static int OP_LW = 32;
static int OP_LWU = 33;
static int OP_SH = 34;
static int OP_SW = 35;
static int OP_DIVU = 36;
static int OP_REMU = 37;
static int OP_SRL = 38;
static int OP_SRAI = 39;

/* op の名前 → 番号。名前の表は iwac 本体の
 * op_name (src/04-encode.c) と対になる */
static char *op_names[40];
static int op_inited;
static void op_init(void) {
    op_names[OP_LUI] = "lui";
    op_names[OP_ADDI] = "addi";
    op_names[OP_ADD] = "add";
    op_names[OP_SUB] = "sub";
    op_names[OP_SB] = "sb";
    op_names[OP_WFI] = "wfi";
    op_names[OP_LBU] = "lbu";
    op_names[OP_AUIPC] = "auipc";
    op_names[OP_BEQ] = "beq";
    op_names[OP_JAL] = "jal";
    op_names[OP_SD] = "sd";
    op_names[OP_LD] = "ld";
    op_names[OP_JALR] = "jalr";
    op_names[OP_SLTU] = "sltu";
    op_names[OP_SLTIU] = "sltiu";
    op_names[OP_AND] = "and";
    op_names[OP_MUL] = "mul";
    op_names[OP_DIV] = "div";
    op_names[OP_SLT] = "slt";
    op_names[OP_XORI] = "xori";
    op_names[OP_ECALL] = "ecall";
    op_names[OP_SLL] = "sll";
    op_names[OP_SRA] = "sra";
    op_names[OP_OR] = "or";
    op_names[OP_XOR] = "xor";
    op_names[OP_REM] = "rem";
    op_names[OP_SLLI] = "slli";
    op_names[OP_BNE] = "bne";
    op_names[OP_ADDIW] = "addiw";
    op_names[OP_SRLI] = "srli";
    op_names[OP_LH] = "lh";
    op_names[OP_LHU] = "lhu";
    op_names[OP_LW] = "lw";
    op_names[OP_LWU] = "lwu";
    op_names[OP_SH] = "sh";
    op_names[OP_SW] = "sw";
    op_names[OP_DIVU] = "divu";
    op_names[OP_REMU] = "remu";
    op_names[OP_SRL] = "srl";
    op_names[OP_SRAI] = "srai";
    op_inited = 1;
}

static int op_lookup(char *name) {
    for (int i = 0; i < 40; i++)
        if (ds_streq(op_names[i], name))
            return i;
    return -1;
}

char *kosaba_out_name;  /* --out の名前。rt が読む (nori と同じ受け口) */

/* ================= エラー ================= */
static char *errmsg;
static int fail(char *msg) {
    errmsg = msg;
    return 0;
}

/* ================= 読んだものの置き場 ================= */
static char in_buf[1048576];   /* 入力 (.ias) の全文 */
static long in_len;
static long in_pos;
static char tok_buf[128];

/* 命令列 */
static int iop[40960];
static int ird[40960];
static int irs1[40960];
static int irs2[40960];
static long iimm[40960];
static char is_call[40960];    /* 呼び出しの jal の印 */
static int ninst;

/* 入口の情報 */
static int prefix_n;
static int has_entry;

/* 呼び出しの注記 (来た順) */
static int call_site[8192];
static int call_kind[8192];    /* 1 = call、2 = callopt */
static int call_name[8192];    /* name_pool へのずれ */
static int ncall;

/* 住所作りの注記 (来た順) */
static int addr_site[8192];
static int addr_kind[8192];    /* 1 = addrstr、2 = var、3 = arr */
static int addr_name[8192];    /* var / arr の名前 (pool へのずれ) */
static long addr_val[8192];    /* addrstr のオフセット */
static int naddr;

/* 絶対アドレスの注記 (来た順) */
static int abs_site[256];
static long abs_val[256];
static int nabs;

/* データ側のレコード (来た順)。
 * 種類: f = func、d = decl、v = var、c = varc、
 * a = arr、A = ainit (v40)、i = init、p = iptr */
static char rec_kind[8192];
static int rec_name[8192];     /* name_pool へのずれ */
static long rec_n1[8192];      /* func: 入口の命令番号、
                                * var/varc: 枠数、arr: オフセット、
                                * init: 値、iptr: 文字列の位置 */
static long rec_n2[8192];      /* func: 大きさ (命令数)、
                                * arr: バイト数 */
static int nrec;

/* undef (出す順序が最後なので分けて持つ) */
static int undef_name[1024];
static int nundef;

/* 文字列の実体と、名前の置き場 */
static char str_bytes[16384];
static int str_len;
static long arrays_n;
/* 配列の初期値のバイト列 (v40、ainit レコードの素通し) */
static char ainit_bytes[262144];
static int ainit_len;
static char name_pool[131072];
static int name_used;

static int put_name(char *s) {
    int off = name_used;
    int i = 0;
    while (s[i] != '\0') {
        if (name_used >= 131072)
            return -1;
        name_pool[name_used] = s[i];
        name_used++;
        i++;
    }
    if (name_used >= 131072)
        return -1;
    name_pool[name_used] = '\0';
    name_used++;
    return off;
}

/* ================= 読む道具 (nori と同じ作り) ================= */
static void skip_space(void) {
    while (in_pos < in_len) {
        char c = in_buf[in_pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        in_pos++;
    }
}

static void read_tok(void) {
    skip_space();
    int n = 0;
    while (in_pos < in_len) {
        char c = in_buf[in_pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            break;
        if (n < 127)
            tok_buf[n++] = c;
        in_pos++;
    }
    tok_buf[n] = '\0';
}

static int expect_tok(char *want) {
    read_tok();
    if (!ds_streq(tok_buf, want))
        return fail("入力の形が違います");
    return 1;
}

static long read_num(void) {
    read_tok();
    long v = 0;
    int i = 0;
    int neg = 0;
    if (tok_buf[0] == '-') {
        neg = 1;
        i = 1;
    }
    while (tok_buf[i] >= '0' && tok_buf[i] <= '9') {
        v = v * 10 + (tok_buf[i] - '0');
        i++;
    }
    return neg ? -v : v;
}

/* 16進数のバイト列を n バイト読む (nori と同じ) */
static int read_hex_bytes(char *pool, int *used, int max, long n) {
    for (long i = 0; i < n; i++) {
        int hi = -1;
        int lo = -1;
        while (hi < 0) {
            if (in_pos >= in_len)
                return fail("16進数が途中で終わっています");
            char c = in_buf[in_pos++];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                continue;
            hi = ds_hexval(c);
            if (hi < 0)
                return fail("16進数でない文字があります");
        }
        while (lo < 0) {
            if (in_pos >= in_len)
                return fail("16進数が途中で終わっています");
            char c = in_buf[in_pos++];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                continue;
            lo = ds_hexval(c);
            if (lo < 0)
                return fail("16進数でない文字があります");
        }
        if (*used >= max)
            return fail("置き場が足りません");
        pool[*used] = (char)(hi * 16 + lo);
        *used = *used + 1;
    }
    return 1;
}

/* ================= 入力を読む ================= */
static int add_rec(int kind, char *name, long n1, long n2) {
    if (nrec >= MAX_REC)
        return fail("レコードが多すぎます");
    int off = put_name(name);
    if (off < 0)
        return fail("名前の置き場が足りません");
    rec_kind[nrec] = (char)kind;
    rec_name[nrec] = off;
    rec_n1[nrec] = n1;
    rec_n2[nrec] = n2;
    nrec++;
    return 1;
}

static int read_input(char *path) {
    /* 名前の退避場所。iwac のローカルは関数単位で
     * 名前が付くので、分岐ごとに宣言せず1つを使い回す */
    char nm[128];
    int fd = open(path, 0);
    if (fd < 0)
        return fail("ファイルが開けません");
    in_len = 0;
    for (;;) {
        long r = read(fd, in_buf + in_len, 1048576 - in_len);
        if (r < 0) {
            close(fd);
            return fail("ファイルが読めません");
        }
        if (r == 0)
            break;
        in_len += r;
        if (in_len >= 1048576) {
            close(fd);
            return fail("入力が大きすぎます");
        }
    }
    close(fd);
    in_pos = 0;

    if (!expect_tok("asari"))
        return 0;
    if (read_num() != 1)
        return fail("asari の版が違います");
    if (!expect_tok("target"))
        return 0;
    read_tok();  /* kosaba (今は1つだけ) */

    for (;;) {
        read_tok();
        if (tok_buf[0] == '\0')
            return fail("end がありません");
        if (ds_streq(tok_buf, "end"))
            break;
        if (ds_streq(tok_buf, "prefix")) {
            prefix_n = (int)read_num();
        } else if (ds_streq(tok_buf, "flush")) {
            read_num();  /* 振り分けは書き手が済ませている */
        } else if (ds_streq(tok_buf, "entry")) {
            has_entry = (int)read_num();
        } else if (ds_streq(tok_buf, "i")) {
            if (ninst >= MAX_INST)
                return fail("命令が多すぎます");
            read_tok();
            int op = op_lookup(tok_buf);
            if (op < 0)
                return fail("知らない op です");
            iop[ninst] = op;
            ird[ninst] = (int)read_num();
            irs1[ninst] = (int)read_num();
            irs2[ninst] = (int)read_num();
            iimm[ninst] = read_num();
            ninst++;
        } else if (ds_streq(tok_buf, "call") ||
                   ds_streq(tok_buf, "callopt")) {
            if (ncall >= MAX_CALL)
                return fail("call 注記が多すぎます");
            int kind = ds_streq(tok_buf, "call") ? 1 : 2;
            call_site[ncall] = (int)read_num();
            read_tok();
            int off = put_name(tok_buf);
            if (off < 0)
                return fail("名前の置き場が足りません");
            call_kind[ncall] = kind;
            call_name[ncall] = off;
            if (call_site[ncall] >= 0 && call_site[ncall] < MAX_INST)
                is_call[call_site[ncall]] = 1;
            ncall++;
        } else if (ds_streq(tok_buf, "addrstr")) {
            if (naddr >= MAX_ADDR)
                return fail("addr 注記が多すぎます");
            addr_site[naddr] = (int)read_num();
            addr_kind[naddr] = 1;
            addr_val[naddr] = read_num();
            naddr++;
        } else if (ds_streq(tok_buf, "addr")) {
            if (naddr >= MAX_ADDR)
                return fail("addr 注記が多すぎます");
            addr_site[naddr] = (int)read_num();
            read_tok();
            addr_kind[naddr] = ds_streq(tok_buf, "var") ? 2 : 3;
            read_tok();
            int off = put_name(tok_buf);
            if (off < 0)
                return fail("名前の置き場が足りません");
            addr_name[naddr] = off;
            naddr++;
        } else if (ds_streq(tok_buf, "addabs")) {
            if (nabs >= MAX_ABS)
                return fail("addabs 注記が多すぎます");
            abs_site[nabs] = (int)read_num();
            abs_val[nabs] = read_num();
            nabs++;
        } else if (ds_streq(tok_buf, "strings")) {
            long n = read_num();
            str_len = 0;
            if (!read_hex_bytes(str_bytes, &str_len, 16384, n))
                return 0;
        } else if (ds_streq(tok_buf, "arrays")) {
            arrays_n = read_num();
        } else if (ds_streq(tok_buf, "func")) {
            read_tok();
            ds_strcpy(nm, tok_buf);
            long e = read_num();
            long sz = read_num();
            if (!add_rec('f', nm, e, sz))
                return 0;
        } else if (ds_streq(tok_buf, "decl")) {
            read_tok();
            if (!add_rec('d', tok_buf, 0, 0))
                return 0;
        } else if (ds_streq(tok_buf, "var") ||
                   ds_streq(tok_buf, "varc")) {
            int kind = ds_streq(tok_buf, "var") ? 'v' : 'c';
            read_tok();
            ds_strcpy(nm, tok_buf);
            long slots = read_num();
            if (!add_rec(kind, nm, slots, 0))
                return 0;
        } else if (ds_streq(tok_buf, "arr")) {
            read_tok();
            ds_strcpy(nm, tok_buf);
            long off = read_num();
            long bytes = read_num();
            if (!add_rec('a', nm, off, bytes))
                return 0;
        } else if (ds_streq(tok_buf, "ainit")) {
            /* 配列の初期値 (v40): バイト列を貯めて、
             * レコードには置き場と長さを控える */
            read_tok();
            ds_strcpy(nm, tok_buf);
            long n = read_num();
            int astart = ainit_len;
            if (!read_hex_bytes(ainit_bytes, &ainit_len, 262144, n))
                return 0;
            if (!add_rec('A', nm, astart, n))
                return 0;
        } else if (ds_streq(tok_buf, "init")) {
            read_tok();
            ds_strcpy(nm, tok_buf);
            long v = read_num();
            if (!add_rec('i', nm, v, 0))
                return 0;
        } else if (ds_streq(tok_buf, "iptr")) {
            read_tok();
            ds_strcpy(nm, tok_buf);
            if (!expect_tok("str"))
                return 0;
            long off = read_num();
            if (!add_rec('p', nm, off, 0))
                return 0;
        } else if (ds_streq(tok_buf, "undef")) {
            if (nundef >= MAX_UNDEF)
                return fail("undef が多すぎます");
            read_tok();
            int off = put_name(tok_buf);
            if (off < 0)
                return fail("名前の置き場が足りません");
            undef_name[nundef] = off;
            nundef++;
        } else {
            return fail("知らないレコードです");
        }
    }
    return 1;
}

/* ================= 符号化 =================
 * iwac 本体の encode_inst (src/04-encode.c) の移植。
 * 違いは2つだけ:
 * - 計算は long で行い、最後に32ビットに丸める
 *   (本体は unsigned int の回り込みに頼っている)
 * - 呼び出しの jal は「オフセット 0」で符号化する
 *   (本体の write_object が揃えた正規形。doc/asari.md) */
static long encode_inst(int i) {
    int op = iop[i];
    long rd = ird[i];
    long rs1 = irs1[i];
    long rs2 = irs2[i];
    long imm = iimm[i];
    long w = 0;

    if (op == OP_JAL && is_call[i])
        imm = i;  /* 呼び出しの穴: オフセット 0 が正規形 */
    if (op == OP_LUI) {
        w = (imm << 12) | (rd << 7) | 0x37;
    } else if (op == OP_ADDI) {
        w = (imm << 20) | (rs1 << 15) | (rd << 7) | 0x13;
    } else if (op == OP_ADDIW) {
        w = (imm << 20) | (rs1 << 15) | (rd << 7) | 0x1B;
    } else if (op == OP_SLLI) {
        w = ((imm & 63) << 20) | (rs1 << 15) | (1 << 12) |
            (rd << 7) | 0x13;
    } else if (op == OP_SRLI) {
        w = ((imm & 63) << 20) | (rs1 << 15) | (5 << 12) |
            (rd << 7) | 0x13;
    } else if (op == OP_SRAI) {
        w = ((0x400 | (imm & 63)) << 20) | (rs1 << 15) |
            (5 << 12) | (rd << 7) | 0x13;
    } else if (op == OP_ADD) {
        w = (rs2 << 20) | (rs1 << 15) | (rd << 7) | 0x33;
    } else if (op == OP_SUB) {
        w = (0x20 << 25) | (rs2 << 20) | (rs1 << 15) |
            (rd << 7) | 0x33;
    } else if (op == OP_AND) {
        w = (rs2 << 20) | (rs1 << 15) | (7 << 12) |
            (rd << 7) | 0x33;
    } else if (op == OP_OR) {
        w = (rs2 << 20) | (rs1 << 15) | (6 << 12) |
            (rd << 7) | 0x33;
    } else if (op == OP_XOR) {
        w = (rs2 << 20) | (rs1 << 15) | (4 << 12) |
            (rd << 7) | 0x33;
    } else if (op == OP_SLL) {
        w = (rs2 << 20) | (rs1 << 15) | (1 << 12) |
            (rd << 7) | 0x33;
    } else if (op == OP_SRA) {
        w = (0x20 << 25) | (rs2 << 20) | (rs1 << 15) |
            (5 << 12) | (rd << 7) | 0x33;
    } else if (op == OP_SRL) {
        w = (rs2 << 20) | (rs1 << 15) | (5 << 12) |
            (rd << 7) | 0x33;
    } else if (op == OP_MUL) {
        w = (1 << 25) | (rs2 << 20) | (rs1 << 15) |
            (rd << 7) | 0x33;
    } else if (op == OP_DIV) {
        w = (1 << 25) | (rs2 << 20) | (rs1 << 15) |
            (4 << 12) | (rd << 7) | 0x33;
    } else if (op == OP_DIVU) {
        w = (1 << 25) | (rs2 << 20) | (rs1 << 15) |
            (5 << 12) | (rd << 7) | 0x33;
    } else if (op == OP_REM) {
        w = (1 << 25) | (rs2 << 20) | (rs1 << 15) |
            (6 << 12) | (rd << 7) | 0x33;
    } else if (op == OP_REMU) {
        w = (1 << 25) | (rs2 << 20) | (rs1 << 15) |
            (7 << 12) | (rd << 7) | 0x33;
    } else if (op == OP_SLT) {
        w = (rs2 << 20) | (rs1 << 15) | (2 << 12) |
            (rd << 7) | 0x33;
    } else if (op == OP_SLTU) {
        w = (rs2 << 20) | (rs1 << 15) | (3 << 12) |
            (rd << 7) | 0x33;
    } else if (op == OP_XORI) {
        w = (imm << 20) | (rs1 << 15) | (4 << 12) |
            (rd << 7) | 0x13;
    } else if (op == OP_SLTIU) {
        w = (imm << 20) | (rs1 << 15) | (3 << 12) |
            (rd << 7) | 0x13;
    } else if (op == OP_SB) {
        w = ((imm >> 5) << 25) | (rs2 << 20) | (rs1 << 15) |
            ((imm & 31) << 7) | 0x23;
    } else if (op == OP_SH || op == OP_SW) {
        long f3 = op == OP_SH ? 1 : 2;
        w = ((imm >> 5) << 25) | (rs2 << 20) | (rs1 << 15) |
            (f3 << 12) | ((imm & 31) << 7) | 0x23;
    } else if (op == OP_SD) {
        w = ((imm >> 5) << 25) | (rs2 << 20) | (rs1 << 15) |
            (3 << 12) | ((imm & 31) << 7) | 0x23;
    } else if (op == OP_LBU) {
        w = (imm << 20) | (rs1 << 15) | (4 << 12) |
            (rd << 7) | 0x03;
    } else if (op == OP_LH || op == OP_LHU || op == OP_LW ||
               op == OP_LWU) {
        long f3 = op == OP_LH ? 1 : op == OP_LW ? 2 :
                  op == OP_LHU ? 5 : 6;
        w = (imm << 20) | (rs1 << 15) | (f3 << 12) |
            (rd << 7) | 0x03;
    } else if (op == OP_LD) {
        w = (imm << 20) | (rs1 << 15) | (3 << 12) |
            (rd << 7) | 0x03;
    } else if (op == OP_AUIPC) {
        w = (imm << 12) | (rd << 7) | 0x17;
    } else if (op == OP_JALR) {
        w = (imm << 20) | (rs1 << 15) | (rd << 7) | 0x67;
    } else if (op == OP_BEQ || op == OP_BNE) {
        long off = (imm - i) * 4;
        long f3 = op == OP_BNE ? 1 : 0;
        w = (((off >> 12) & 1) << 31) | (((off >> 5) & 0x3f) << 25) |
            (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
            (((off >> 1) & 0xf) << 8) | (((off >> 11) & 1) << 7) |
            0x63;
    } else if (op == OP_JAL) {
        long off = (imm - i) * 4;
        w = (((off >> 20) & 1) << 31) | (((off >> 1) & 0x3ff) << 21) |
            (((off >> 11) & 1) << 20) | (((off >> 12) & 0xff) << 12) |
            (rd << 7) | 0x6f;
    } else if (op == OP_ECALL) {
        w = 0x73;
    } else {
        /* OP_WFI */
        w = 0x10500073;
    }
    return w & 0xffffffff;
}

/* ================= 書く道具 =================
 * 出力は小さな置き場に溜めて write で流す
 * (iwac 本体の obj_* と同じ字面になるよう揃える) */
static char obuf[8192];
static int olen;

static void oflush(void) {
    if (olen > 0) {
        write(1, obuf, olen);
        olen = 0;
    }
}

static void out_ch(int c) {
    if (olen >= 8192)
        oflush();
    obuf[olen] = (char)c;
    olen++;
}

static void out_str(char *s) {
    int i = 0;
    while (s[i] != '\0') {
        out_ch(s[i]);
        i++;
    }
}

/* 10進数 (負もあり。本体の obj_num と同じ書き方) */
static void out_num(long v) {
    char buf[24];
    int n = 0;
    if (v < 0) {
        out_ch('-');
        v = -v;
    }
    if (v == 0)
        buf[n++] = '0';
    while (v > 0) {
        buf[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    for (int i = n - 1; i >= 0; i--)
        out_ch(buf[i]);
}

static void out_hex_byte(int b) {
    char *hex = "0123456789abcdef";
    out_ch(hex[(b >> 4) & 15]);
    out_ch(hex[b & 15]);
}

/* バイト列を16進数で (1行に32バイトまで。本体の
 * obj_hex と同じ改行の置き方) */
static void out_hex(char *p, long n) {
    for (long i = 0; i < n; i++) {
        if (i > 0 && i % 32 == 0)
            out_ch('\n');
        out_hex_byte(p[i] & 0xff);
    }
    if (n > 0)
        out_ch('\n');
}

/* ================= .iwo を出す =================
 * 並びと字面は iwac 本体の write_object と揃える。
 * 位置は「命令番号 - 先頭」で引いて4倍する */
static int write_object_out(void) {
    int start = has_entry ? 0 : prefix_n;

    out_str("iwacobj 1\n");
    out_str("target kosaba\n");
    if (has_entry) {
        out_str("entry ");
        out_num((long)prefix_n * 4);
        out_str("\n");
    }
    out_str("code ");
    out_num((long)(ninst - start) * 4);
    out_str("\n");
    {
        long col = 0;
        for (int i = start; i < ninst; i++) {
            long w = encode_inst(i);
            for (int j = 0; j < 4; j++) {
                if (col > 0 && col % 32 == 0)
                    out_ch('\n');
                out_hex_byte((int)((w >> (j * 8)) & 0xff));
                col++;
            }
        }
        if (col > 0)
            out_ch('\n');
    }
    out_str("strings ");
    out_num(str_len);
    out_str("\n");
    out_hex(str_bytes, str_len);
    out_str("arrays ");
    out_num(arrays_n);
    out_str("\n");
    for (int r = 0; r < nrec; r++) {
        char *nm = name_pool + rec_name[r];
        if (rec_kind[r] == 'f') {
            out_str("func ");
            out_str(nm);
            out_str(" ");
            out_num((rec_n1[r] - start) * 4);
            out_str(" ");
            out_num(rec_n2[r] * 4);
            out_str("\n");
        } else if (rec_kind[r] == 'd') {
            out_str("decl ");
            out_str(nm);
            out_str("\n");
        } else if (rec_kind[r] == 'v' || rec_kind[r] == 'c') {
            out_str(rec_kind[r] == 'v' ? "var " : "varc ");
            out_str(nm);
            out_str(" ");
            out_num(rec_n1[r]);
            out_str("\n");
        } else if (rec_kind[r] == 'a') {
            out_str("arr ");
            out_str(nm);
            out_str(" ");
            out_num(rec_n1[r]);
            out_str(" ");
            out_num(rec_n2[r]);
            out_str("\n");
        } else if (rec_kind[r] == 'A') {
            out_str("ainit ");
            out_str(nm);
            out_str(" ");
            out_num(rec_n2[r]);
            out_str("\n");
            out_hex(ainit_bytes + rec_n1[r], rec_n2[r]);
        } else if (rec_kind[r] == 'i') {
            out_str("init ");
            out_str(nm);
            out_str(" ");
            out_num(rec_n1[r]);
            out_str("\n");
        } else if (rec_kind[r] == 'p') {
            out_str("iptr ");
            out_str(nm);
            out_str(" str ");
            out_num(rec_n1[r]);
            out_str("\n");
        }
    }
    for (int r = 0; r < ncall; r++) {
        out_str(call_kind[r] == 1 ? "call " : "callopt ");
        out_num((long)(call_site[r] - start) * 4);
        out_str(" ");
        out_str(name_pool + call_name[r]);
        out_str("\n");
    }
    for (int r = 0; r < naddr; r++) {
        if (addr_kind[r] == 1) {
            out_str("addrstr ");
            out_num((long)(addr_site[r] - start) * 4);
            out_str(" ");
            out_num(addr_val[r]);
            out_str("\n");
        } else {
            out_str("addr ");
            out_num((long)(addr_site[r] - start) * 4);
            out_str(" ");
            out_str(name_pool + addr_name[r]);
            out_str(" 0\n");
        }
    }
    for (int r = 0; r < nabs; r++) {
        out_str("addabs ");
        out_num((long)(abs_site[r] - start) * 4);
        out_str(" ");
        out_num(abs_val[r]);
        out_str("\n");
    }
    for (int r = 0; r < nundef; r++) {
        out_str("undef ");
        out_str(name_pool + undef_name[r]);
        out_str("\n");
    }
    out_str("end\n");
    oflush();
    return 1;
}

/* ================= 本体 ================= */
int main(int argc, char **argv) {
    int base = 1;
    op_init();
    /* asari --out <名前> in.ias : OS 上での出力名 (nori と同じ) */
    if (argc >= 4 && ds_streq(argv[1], "--out")) {
        kosaba_out_name = argv[2];
        base = 3;
    }
    if (argc - base < 1) {
        ds_puts(2, "usage: asari [--out <name>] <ファイル.ias> > out.iwo\n");
        return 1;
    }
    if (!read_input(argv[base])) {
        ds_puts(2, "asari: ");
        ds_puts(2, errmsg);
        ds_puts(2, "\n");
        return 1;
    }
    write_object_out();
    return 0;
}
