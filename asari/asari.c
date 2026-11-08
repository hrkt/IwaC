/* ================================================================
 * Asari: iwac の第3層 (encode) の独立プログラム
 *
 * 入力は iwac --asari が出す命令列のテキスト (.ias)、
 * 出力は ELF オブジェクトで、標準出力へ書く
 * (v42 で .iwo から切替。決定の記録は
 * doc/iwo-retirement.md)。
 * 設計は doc/asari.md。仕事は3つ:
 *   1. 命令列を読む (op は名前、穴は注記で別に来る)
 *   2. 命令を符号化する (RISC-V。ここだけがビット列を
 *      知っている。iwac 本体の src/04-encode.c の移植)
 *   3. データ側のレコードから ELF オブジェクトを
 *      組む (iwac 本体の write_elf_object の移植)
 * 物差しは、iwac が出す ELF オブジェクトとの
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
 *   (本体のオブジェクト書き手が揃えた正規形。
 *   doc/asari.md) */
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

/* ================= ELF の書き道具 =================
 * 出力先は out_ch と共通で、こちらは書いたバイト数を
 * 数える (置き場所の計算に使う)。iwac 本体の ebyte
 * 系 (src/04-encode.c) の移植 */
static long eoff;

static void ebyte(int b) {
    out_ch(b);
    eoff++;
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

static void epad8(void) {
    while (eoff % 8 != 0)
        ebyte(0);
}

static void ebytes(char *p, long n) {
    for (long i = 0; i < n; i++)
        ebyte(p[i] & 0xff);
}

/* ================= レコードの数えと検索 =================
 * ELF のシンボル表は「関数は名簿の順、変数は枠の順、
 * 配列は名簿の順」で並ぶ。レコードはその順で届いて
 * いるので、種類ごとに数えれば順位が分かる */
static int cnt_func(void) {
    int n = 0;
    for (int r = 0; r < nrec; r++)
        if (rec_kind[r] == 'f' || rec_kind[r] == 'd')
            n++;
    return n;
}

static int cnt_var(void) {
    int n = 0;
    for (int r = 0; r < nrec; r++)
        if (rec_kind[r] == 'v' || rec_kind[r] == 'c')
            n++;
    return n;
}

static int cnt_arr(void) {
    int n = 0;
    for (int r = 0; r < nrec; r++)
        if (rec_kind[r] == 'a')
            n++;
    return n;
}

static int cnt_iptr(void) {
    int n = 0;
    for (int r = 0; r < nrec; r++)
        if (rec_kind[r] == 'p')
            n++;
    return n;
}

/* kosaba_flush が関数として届いているか */
static int flush_in_recs(void) {
    for (int r = 0; r < nrec; r++)
        if ((rec_kind[r] == 'f' || rec_kind[r] == 'd') &&
            ds_streq(name_pool + rec_name[r], "kosaba_flush"))
            return 1;
    return 0;
}

static int has_callopt(void) {
    for (int r = 0; r < ncall; r++)
        if (call_kind[r] == 2)
            return 1;
    return 0;
}

/* 変数全体の枠数 (仮定義の領域の大きさ) */
static long total_var_slots(void) {
    long n = 0;
    for (int r = 0; r < nrec; r++)
        if (rec_kind[r] == 'v' || rec_kind[r] == 'c')
            n += rec_n1[r];
    return n;
}

/* 初期値つき変数が届く最後の枠 (初期イメージの大きさ) */
static long img_slots(void) {
    long best = 0;
    long pos = 0;
    for (int r = 0; r < nrec; r++) {
        if (rec_kind[r] != 'v' && rec_kind[r] != 'c')
            continue;
        if (rec_kind[r] == 'v' && pos + rec_n1[r] > best)
            best = pos + rec_n1[r];
        pos += rec_n1[r];
    }
    return best;
}

/* 名前から順位と場所を引く (再配置の相手決めに使う) */
static int func_rank(char *name) {
    int seen = -1;
    for (int r = 0; r < nrec; r++) {
        if (rec_kind[r] != 'f' && rec_kind[r] != 'd')
            continue;
        seen++;
        if (ds_streq(name_pool + rec_name[r], name))
            return seen;
    }
    return -1;
}

static int var_rank(char *name) {
    int seen = -1;
    for (int r = 0; r < nrec; r++) {
        if (rec_kind[r] != 'v' && rec_kind[r] != 'c')
            continue;
        seen++;
        if (ds_streq(name_pool + rec_name[r], name))
            return seen;
    }
    return -1;
}

static int arr_rank(char *name) {
    int seen = -1;
    for (int r = 0; r < nrec; r++) {
        if (rec_kind[r] != 'a')
            continue;
        seen++;
        if (ds_streq(name_pool + rec_name[r], name))
            return seen;
    }
    return -1;
}

/* 変数の枠の位置 (先行する変数の枠数の合計) */
static long var_slotpos(char *name) {
    long pos = 0;
    for (int r = 0; r < nrec; r++) {
        if (rec_kind[r] != 'v' && rec_kind[r] != 'c')
            continue;
        if (ds_streq(name_pool + rec_name[r], name))
            return pos;
        pos += rec_n1[r];
    }
    return 0;
}

/* 変数の初期値 (init なら値、iptr なら文字列の位置。
 * 初期値の無い変数では呼ばない) */
static long var_init_val(char *name) {
    for (int r = 0; r < nrec; r++)
        if ((rec_kind[r] == 'i' || rec_kind[r] == 'p') &&
            ds_streq(name_pool + rec_name[r], name))
            return rec_n1[r];
    return 0;
}

/* 配列の領域内オフセット */
static long arr_off_of(char *name) {
    for (int r = 0; r < nrec; r++)
        if (rec_kind[r] == 'a' &&
            ds_streq(name_pool + rec_name[r], name))
            return rec_n1[r];
    return 0;
}

/* 初期値の実体が届く末尾 (iwac 本体の arr_init_end と
 * 同じ数え方: 書かれた範囲の最後の 0 でないバイトまで) */
static long ainit_end(void) {
    long best = 0;
    for (int r = 0; r < nrec; r++) {
        if (rec_kind[r] != 'A')
            continue;
        long t = rec_n2[r];
        while (t > 0 && ainit_bytes[rec_n1[r] + t - 1] == 0)
            t--;
        long cand = arr_off_of(name_pool + rec_name[r]) + t;
        if (cand > best)
            best = cand;
    }
    return best;
}

/* ================= シンボルの素性 =================
 * iwac 本体の elf_describe_* の移植。並びは
 * [0]=空、[1..5]=セクション、再配置の組ごとの
 * ラベル、絶対アドレスのシンボル (以上ローカル)、
 * iwac_entry (あれば)、関数、変数、配列、
 * kosaba_flush の弱い未定義 (余分があれば) */
static char *ed_name;
static int ed_info;
static int ed_shndx;
static long ed_value;
static long ed_size;

static char *esec_name(int i) {
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

static void edescribe_local(int i, int start) {
    ed_name = "";
    ed_info = 0;
    ed_shndx = 0;
    ed_value = 0;
    ed_size = 0;
    if (i == 0)
        return;
    if (i <= 5) {
        ed_info = 3;  /* LOCAL / SECTION */
        ed_shndx = i;
        return;
    }
    if (i < 6 + naddr) {
        int r = i - 6;
        ed_shndx = 1;  /* .text */
        ed_value = (long)(addr_site[r] - start) * 4;
        return;
    }
    {
        int r = i - 6 - naddr;
        ed_shndx = 0xfff1;  /* SHN_ABS */
        ed_value = abs_val[r];
    }
}

static void edescribe_global(int g, int has_entry, int start) {
    int off0 = has_entry ? 1 : 0;
    ed_name = "";
    ed_info = 0x10;  /* GLOBAL / NOTYPE */
    ed_shndx = 0;
    ed_value = 0;
    ed_size = 0;
    if (has_entry && g == 0) {
        ed_name = "iwac_entry";
        ed_shndx = 1;
        ed_size = (long)prefix_n * 4;
        return;
    }
    if (g < off0 + cnt_func()) {
        int want = g - off0;
        int seen = -1;
        for (int r = 0; r < nrec; r++) {
            if (rec_kind[r] != 'f' && rec_kind[r] != 'd')
                continue;
            seen++;
            if (seen == want) {
                ed_name = name_pool + rec_name[r];
                if (rec_kind[r] == 'f') {
                    ed_info = 0x12;  /* GLOBAL / FUNC */
                    ed_shndx = 1;
                    ed_value = (rec_n1[r] - start) * 4;
                    ed_size = rec_n2[r] * 4;
                } else if (ds_streq(ed_name, "kosaba_flush")) {
                    ed_info = 0x20;  /* WEAK / NOTYPE */
                }
                return;
            }
        }
        return;
    }
    if (g < off0 + cnt_func() + cnt_var()) {
        int want = g - off0 - cnt_func();
        int seen = -1;
        long slotpos = 0;
        for (int r = 0; r < nrec; r++) {
            if (rec_kind[r] != 'v' && rec_kind[r] != 'c')
                continue;
            seen++;
            if (seen == want) {
                ed_name = name_pool + rec_name[r];
                ed_info = 0x11;  /* GLOBAL / OBJECT */
                if (rec_kind[r] == 'v') {
                    ed_shndx = 2;  /* .data */
                    ed_value = slotpos * 8;
                } else {
                    ed_shndx = 0xfff2;  /* SHN_COMMON */
                    ed_value = 8;
                }
                ed_size = rec_n1[r] * 8;
                return;
            }
            slotpos += rec_n1[r];
        }
        return;
    }
    if (g < off0 + cnt_func() + cnt_var() + cnt_arr()) {
        int want = g - off0 - cnt_func() - cnt_var();
        int seen = -1;
        for (int r = 0; r < nrec; r++) {
            if (rec_kind[r] != 'a')
                continue;
            seen++;
            if (seen == want) {
                ed_name = name_pool + rec_name[r];
                ed_info = 0x11;
                ed_shndx = 5;  /* .iwac.arrays */
                ed_value = rec_n1[r];
                ed_size = rec_n2[r];
                return;
            }
        }
        return;
    }
    /* 余分の kosaba_flush (弱い未定義) */
    ed_name = "kosaba_flush";
    ed_info = 0x20;
}

static char *esym_name(int s, int nlocals, int has_entry,
                       int start) {
    if (s < nlocals)
        edescribe_local(s, start);
    else
        edescribe_global(s - nlocals, has_entry, start);
    return ed_name;
}

/* (旧 .iwo 出力は v42 で引退。出力は ELF に一本化) */

/* ================= ELF を出す =================
 * iwac 本体の write_elf_object とバイト一致させる。
 * 材料はレコードと注記から組み直す。配置の規則は
 * nori が握る (ここは置き場所を決めない)。
 * 段ごとに値を受けるための置き場 */
static int el_start;
static int el_off0;
static int el_nf;
static int el_nv;
static int el_na;
static int el_nlocals;
static int el_nsym;
static int el_nrela_data;
static long el_data_size;
static long el_bss_size;
static long el_strings_off;
static long el_strings_size;
static long el_arrays_off;
static long el_arrays_size;
static long el_aend;
static long el_data_off;
static long el_rela_text_off;
static long el_rela_data_off;
static long el_symtab_off;
static long el_strtab_off;
static long el_strtab_size;
static long el_shstrtab_off;
static long el_shstrtab_size;

/* link / info / entsize は呼び出し前に置く (引数を
 * 8個以内に収めるため。本体と同じやり方) */
static int esh_link;
static int esh_info;
static long esh_entsize;

static void eshdr(int name, int type, long flags, long off,
                  long size, long align) {
    eu32(name);
    eu32(type);
    eu64(flags);
    eu64(0);
    eu64(off);
    eu64(size);
    eu32(esh_link);
    eu32(esh_info);
    eu64(align);
    eu64(esh_entsize);
    esh_link = 0;
    esh_info = 0;
    esh_entsize = 0;
}

/* 段1: 数えて置き場所を決め、ヘッダ・.text・
 * .iwac.strings・.iwac.arrays の中身を書く */
static void elf_part1(void) {
    el_start = has_entry ? 0 : prefix_n;
    el_off0 = has_entry ? 1 : 0;
    el_aend = ainit_end();
    int nf = cnt_func();
    int nv = cnt_var();
    int na = cnt_arr();
    int flush_extra = (has_callopt() && !flush_in_recs()) ? 1 : 0;
    int nlocals = 6 + naddr + nabs;
    int nglob = el_off0 + nf + nv + na + flush_extra;
    long img = img_slots();
    long text_size = (long)(ninst - el_start) * 4;
    long strtab_size = 1;
    for (int s2 = 0; s2 < nlocals + nglob; s2++)
        strtab_size += ds_strlen(esym_name(s2, nlocals,
                             has_entry, el_start)) + 1;
    long shstrtab_size = 0;
    for (int i = 0; i < 11; i++)
        shstrtab_size += ds_strlen(esec_name(i)) + 1;
    long text_off = 64;
    long strings_off = text_off + text_size;
    long arrays_off = strings_off + str_len;
    long data_off = (arrays_off + 7) / 8 * 8;
    if (el_aend > 0)
        data_off = (arrays_off + arrays_n + 7) / 8 * 8;
    long rela_text_off = (data_off + img * 8 + 7) / 8 * 8;
    el_nf = nf;
    el_nv = nv;
    el_na = na;
    el_nlocals = nlocals;
    el_nsym = nlocals + nglob;
    el_nrela_data = cnt_iptr();
    el_data_size = img * 8;
    el_bss_size = total_var_slots() * 8 - img * 8;
    el_strings_off = strings_off;
    el_strings_size = str_len;
    el_arrays_off = arrays_off;
    el_arrays_size = arrays_n;
    el_data_off = data_off;
    el_rela_text_off = rela_text_off;
    el_rela_data_off = rela_text_off +
        (long)(ncall + 2 * naddr + 2 * nabs) * 24;
    el_symtab_off = (el_rela_data_off +
        (long)el_nrela_data * 24 + 7) / 8 * 8;
    el_strtab_off = el_symtab_off + (long)el_nsym * 24;
    el_strtab_size = strtab_size;
    el_shstrtab_off = el_strtab_off + strtab_size;
    el_shstrtab_size = shstrtab_size;
    long shoff = (el_shstrtab_off + shstrtab_size + 7) / 8 * 8;

    eoff = 0;
    ebyte(0x7f); ebyte('E'); ebyte('L'); ebyte('F');
    ebyte(2); ebyte(1); ebyte(1); ebyte(0);
    for (int k = 0; k < 8; k++)
        ebyte(0);
    eu16(1);          /* ET_REL */
    eu16(243);        /* EM_RISCV */
    eu32(1);
    eu64(0);
    eu64(0);
    eu64(shoff);
    eu32(0);
    eu16(64);
    eu16(0);
    eu16(0);
    eu16(64);
    eu16(11);
    eu16(10);
    /* .text (呼び出しの穴は encode_inst が正規形に) */
    for (int i = el_start; i < ninst; i++)
        eu32(encode_inst(i));
    /* .iwac.strings */
    ebytes(str_bytes, str_len);
    /* .iwac.arrays の中身 (初期値があるときだけ) */
    if (el_aend > 0) {
        long pos = 0;
        for (int r = 0; r < nrec; r++) {
            if (rec_kind[r] != 'A')
                continue;
            long off = arr_off_of(name_pool + rec_name[r]);
            while (pos < off) {
                ebyte(0);
                pos++;
            }
            ebytes(ainit_bytes + rec_n1[r], rec_n2[r]);
            pos = off + rec_n2[r];
        }
        while (pos < arrays_n) {
            ebyte(0);
            pos++;
        }
    }
}

/* 段2: .data の初期イメージと再配置 (.rela.text /
 * .rela.data) を書く */
static void elf_part2(void) {
    epad8();
    /* .data: 枠ごとに、その枠から始まる変数の初期値 */
    long img = img_slots();
    for (long slot = 0; slot < img; slot++) {
        long val = 0;
        long pos = 0;
        for (int r = 0; r < nrec; r++) {
            if (rec_kind[r] != 'v' && rec_kind[r] != 'c')
                continue;
            if (pos == slot && rec_kind[r] == 'v') {
                val = var_init_val(name_pool + rec_name[r]);
                break;
            }
            pos += rec_n1[r];
        }
        eu64(val);
    }
    /* .rela.text: 呼び出し */
    epad8();
    for (int r = 0; r < ncall; r++) {
        int sym;
        if (call_kind[r] == 2) {
            if (flush_in_recs())
                sym = el_nlocals + el_off0 +
                      func_rank("kosaba_flush");
            else
                sym = el_nlocals + el_off0 + el_nf + el_nv +
                      el_na;
        } else {
            sym = el_nlocals + el_off0 +
                  func_rank(name_pool + call_name[r]);
        }
        eu64((long)(call_site[r] - el_start) * 4);
        eu64(((long)sym << 32) | 17);  /* R_RISCV_JAL */
        eu64(0);
    }
    /* .rela.text: 住所作りの組 (HI20 + LO12) */
    for (int r = 0; r < naddr; r++) {
        int sym;
        long add = 0;
        if (addr_kind[r] == 1) {
            sym = 4;  /* .iwac.strings のセクション */
            add = addr_val[r];
        } else if (addr_kind[r] == 2) {
            sym = el_nlocals + el_off0 + el_nf +
                  var_rank(name_pool + addr_name[r]);
        } else {
            sym = el_nlocals + el_off0 + el_nf + el_nv +
                  arr_rank(name_pool + addr_name[r]);
        }
        eu64((long)(addr_site[r] - el_start) * 4);
        eu64(((long)sym << 32) | 23);  /* PCREL_HI20 */
        eu64(add);
        eu64((long)(addr_site[r] - el_start) * 4 + 4);
        eu64(((long)(6 + r) << 32) | 24);  /* PCREL_LO12_I */
        eu64(0);
    }
    /* .rela.text: 絶対アドレスの組 */
    for (int r = 0; r < nabs; r++) {
        int sym = 6 + naddr + r;
        eu64((long)(abs_site[r] - el_start) * 4);
        eu64(((long)sym << 32) | 26);  /* R_RISCV_HI20 */
        eu64(0);
        eu64((long)(abs_site[r] - el_start) * 4 + 4);
        eu64(((long)sym << 32) | 27);  /* R_RISCV_LO12_I */
        eu64(0);
    }
    /* .rela.data: 文字列へのポインタの初期値 */
    for (int r = 0; r < nrec; r++) {
        if (rec_kind[r] != 'p')
            continue;
        eu64(var_slotpos(name_pool + rec_name[r]) * 8);
        eu64(((long)4 << 32) | 2);  /* R_RISCV_64 */
        eu64(rec_n1[r]);
    }
}

/* 段3: .symtab・.strtab・.shstrtab・セクション
 * ヘッダを書く */
static void elf_part3(void) {
    epad8();
    for (int s2 = 0; s2 < el_nsym; s2++) {
        long name_off = 1;
        for (int t = 0; t < s2; t++)
            name_off += ds_strlen(esym_name(t, el_nlocals,
                              has_entry, el_start)) + 1;
        esym_name(s2, el_nlocals, has_entry, el_start);
        eu32(name_off);
        ebyte(ed_info);
        ebyte(0);
        eu16(ed_shndx);
        eu64(ed_value);
        eu64(ed_size);
    }
    ebyte(0);
    for (int s2 = 0; s2 < el_nsym; s2++) {
        char *nm = esym_name(s2, el_nlocals, has_entry,
                             el_start);
        ebytes(nm, ds_strlen(nm));
        ebyte(0);
    }
    for (int i = 0; i < 11; i++) {
        ebytes(esec_name(i), ds_strlen(esec_name(i)));
        ebyte(0);
    }
    epad8();
    eshdr(0, 0, 0, 0, 0, 0);
    {
        long noff[11];
        long acc = 0;
        for (int i = 0; i < 11; i++) {
            noff[i] = acc;
            acc += ds_strlen(esec_name(i)) + 1;
        }
        eshdr((int)noff[1], 1, 6, 64,
              (long)(ninst - el_start) * 4, 4);
        eshdr((int)noff[2], 1, 3, el_data_off, el_data_size, 8);
        eshdr((int)noff[3], 8, 3, el_data_off + el_data_size,
              el_bss_size, 8);
        eshdr((int)noff[4], 1, 3, el_strings_off,
              el_strings_size, 1);
        if (el_aend > 0)
            eshdr((int)noff[5], 1, 3, el_arrays_off,
                  el_arrays_size, 8);
        else
            eshdr((int)noff[5], 8, 3,
                  el_strings_off + el_strings_size,
                  el_arrays_size, 8);
        esh_link = 8;
        esh_info = 1;
        esh_entsize = 24;
        eshdr((int)noff[6], 4, 0, el_rela_text_off,
              (long)(ncall + 2 * naddr + 2 * nabs) * 24, 8);
        esh_link = 8;
        esh_info = 2;
        esh_entsize = 24;
        eshdr((int)noff[7], 4, 0, el_rela_data_off,
              (long)el_nrela_data * 24, 8);
        esh_link = 9;
        esh_info = el_nlocals;
        esh_entsize = 24;
        eshdr((int)noff[8], 2, 0, el_symtab_off,
              (long)el_nsym * 24, 8);
        eshdr((int)noff[9], 3, 0, el_strtab_off,
              el_strtab_size, 1);
        eshdr((int)noff[10], 3, 0, el_shstrtab_off,
              el_shstrtab_size, 1);
    }
}

static int write_elf_out(void) {
    elf_part1();
    elf_part2();
    elf_part3();
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
        ds_puts(2, "usage: asari [--out <name>] <ファイル.ias> > out.eo\n");
        return 1;
    }
    if (!read_input(argv[base])) {
        ds_puts(2, "asari: ");
        ds_puts(2, errmsg);
        ds_puts(2, "\n");
        return 1;
    }
    write_elf_out();
    return 0;
}
