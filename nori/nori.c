/* nori -- IwaC のためのリンカ (分割コンパイルへの
 * 階段の段3。設計は doc/object-format.md)
 *
 * 使い方: nori a.iwo b.iwo ... > prog.elf
 *
 * オブジェクト (テキスト形式、iwacobj 1) を読み、
 *   1. コードを並べる (入口の断片を先頭に、
 *      関数の本体は登録順)
 *   2. データを並べる (文字列 → 配列 → グローバル。
 *      仮定義は名前ごとに合併する)
 *   3. 穴を埋める (call / callopt / addr / addrstr /
 *      addabs / iptr)
 *   4. KosabaOS 用 ELF を標準出力へ書く
 * という4つの仕事をする。iwac 本体と同じく、
 * このプログラムも iwac の言語の範囲で書いてあり、
 * ホストの gcc でも、iwac --kosaba でも訳せる。
 * 外に頼るのは write / open / read / close の4つだけ */

/* 外部関数の宣言 (ヘッダの代わりに自分で書く。
 * iwac 本体と同じ約束) */
long write(int fd, char *buf, long len);
int open(char *path, int flags);
long read(int fd, char *buf, long len);
int close(int fd);

/* ================= 大きさの約束 ================= */
static int MAX_OBJ = 32;       /* オブジェクトの数 */
static int MAX_SYM = 4096;     /* 名前表 */
static int MAX_REL = 16384;    /* 穴の数 */
static int MAX_SEQ = 4096;     /* 登録順の控え */
/* バイト列の置き場 (静的データとして持つ) */
static char in_buf[393216];    /* オブジェクト1つの全文 */
static char code_pool[262144]; /* 全オブジェクトのコード */
static char str_pool[131072];  /* 全オブジェクトの文字列 */
static char out_code[262144];  /* 並べた後のコード */
static char name_pool[65536];  /* 名前の文字列 */

/* 配置の約束 (KosabaOS のユーザープログラム) */
static long CODE_BASE = 0x40000000;
static long DATA_BASE = 0x40200000;

/* ================= 道具 ================= */
static void puts_fd(int fd, char *s) {
    long len = 0;
    while (s[len] != '\0')
        len++;
    write(fd, s, len);
}

static int str_eq(char *a, char *b) {
    int i = 0;
    while (a[i] != '\0' && a[i] == b[i])
        i++;
    return a[i] == b[i];
}

/* エラー: 理由を置いて 0 を返す (main が表示する) */
static char *errmsg;
char *kosaba_out_name;  /* --out の名前。rt が読む (v34) */
static int fail(char *msg) {
    errmsg = msg;
    return 0;
}

/* ================= オブジェクトの控え ================= */
static int nobj;
static int obj_code_off[32];   /* code_pool の中での位置 */
static int obj_code_len[32];
static int obj_entry[32];      /* 入口の長さ。無ければ -1 */
static int obj_str_off[32];    /* str_pool の中での位置 */
static int obj_str_len[32];
static int obj_str_final[32];  /* 配置後の文字列のずれ */
static int code_used;          /* code_pool の使用量 */
static int str_used;           /* str_pool の使用量 */

/* ================= 名前表 =================
 * 種類: 0 = 未定 (参照だけ)、1 = 関数、2 = 変数、3 = 配列 */
static int nsym;
static int sym_name[4096];     /* name_pool へのずれ */
static int sym_kind[4096];
static int sym_obj[4096];      /* 定義 (関数・配列) の出所 */
static int sym_off[4096];      /* 関数: 本体の位置、
                                * 変数: 枠番号、配列: 領域のずれ */
static int sym_size[4096];     /* 関数・配列の大きさ */
static int sym_firm[4096];     /* 変数: 本定義 (var) か */
static int sym_hasinit[4096];  /* 変数: 初期値があるか */
static int sym_init_kind[4096];/* 1 = 数、2 = 文字列、3 = 名前 */
static long sym_init_val[4096];
static int sym_init_obj[4096]; /* 文字列の出所のオブジェクト */
static int sym_init_sym[4096]; /* 種類3の相手の名前 */
static int sym_place[4096];    /* 関数: 配置後の位置 */
static long sym_addr[4096];    /* 最終的な住所 */
static int name_used;

static int name_eq_at(int off, char *s) {
    int i = 0;
    while (s[i] != '\0' && name_pool[off + i] == s[i])
        i++;
    return s[i] == '\0' && name_pool[off + i] == '\0';
}

static int find_sym(char *name) {
    for (int i = 0; i < nsym; i++)
        if (name_eq_at(sym_name[i], name))
            return i;
    return -1;
}

static int new_sym(char *name) {
    int s = find_sym(name);
    if (s >= 0)
        return s;
    if (nsym >= MAX_SYM) {
        errmsg = "名前が多すぎます";
        return -1;
    }
    int off = name_used;
    int i = 0;
    while (name[i] != '\0') {
        if (name_used >= 65536) {
            errmsg = "名前の置き場が足りません";
            return -1;
        }
        name_pool[name_used++] = name[i++];
    }
    name_pool[name_used++] = '\0';
    s = nsym++;
    sym_name[s] = off;
    sym_kind[s] = 0;
    sym_obj[s] = -1;
    sym_off[s] = 0;
    sym_size[s] = 0;
    sym_firm[s] = 0;
    sym_hasinit[s] = 0;
    sym_init_kind[s] = 0;
    sym_init_val[s] = 0;
    sym_init_obj[s] = -1;
    sym_init_sym[s] = -1;
    sym_place[s] = -1;
    sym_addr[s] = 0;
    return s;
}

static char *sym_str(int s) {
    return name_pool + sym_name[s];
}

/* 種類を決める。違う種類で既に決まっていたら失敗 */
static int set_kind(int s, int kind) {
    if (sym_kind[s] != 0 && sym_kind[s] != kind) {
        errmsg = "名前の種類が食い違います";
        return 0;
    }
    sym_kind[s] = kind;
    return 1;
}

/* 変数の枠と配列領域の勘定 (parse の途中で数える) */
static int var_slots;
static long arr_area;
static int err_sym;            /* エラーが指す名前 (-1 = 無し) */

/* ================= 登録順と穴の控え ================= */
static int nseq;
static int seq_sym[4096];

static int nrel;
static int rel_obj[16384];
static int rel_off[16384];
static int rel_kind[16384];    /* 1 call、2 callopt、3 addr、
                                * 4 addrstr、5 addabs */
static int rel_sym[16384];
static long rel_val[16384];

static int add_rel(int obj, int off, int kind, int sym, long val) {
    if (nrel >= MAX_REL)
        return fail("穴が多すぎます");
    rel_obj[nrel] = obj;
    rel_off[nrel] = off;
    rel_kind[nrel] = kind;
    rel_sym[nrel] = sym;
    rel_val[nrel] = val;
    nrel++;
    return 1;
}

/* ================= オブジェクトを読む ================= */
static long in_len;
static long in_pos;
static char tok_buf[128];

static void skip_space(void) {
    while (in_pos < in_len) {
        char c = in_buf[in_pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        in_pos++;
    }
}

/* 空白で区切られた言葉を1つ読む。無ければ空に */
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

/* 次の言葉が期待どおりか確かめる */
static int expect_tok(char *want) {
    read_tok();
    if (!str_eq(tok_buf, want))
        return fail("オブジェクトの形が違います");
    return 1;
}

/* 10進数を読む (符号あり、64ビット) */
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

static int hex_val(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* 16進数のバイト列を n バイト読んで置き場へ足す */
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
            hi = hex_val(c);
            if (hi < 0)
                return fail("16進数でない文字があります");
        }
        while (lo < 0) {
            if (in_pos >= in_len)
                return fail("16進数が途中で終わっています");
            char c = in_buf[in_pos++];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                continue;
            lo = hex_val(c);
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

/* オブジェクト1つを読む (番号 obj) */
static int read_object(int obj, char *path) {
    int fd = open(path, 0);
    if (fd < 0)
        return fail("ファイルが開けません");
    in_len = 0;
    for (;;) {
        long r = read(fd, in_buf + in_len, 393216 - in_len);
        if (r < 0) {
            close(fd);
            return fail("ファイルが読めません");
        }
        if (r == 0)
            break;
        in_len += r;
        if (in_len >= 393216) {
            close(fd);
            return fail("オブジェクトが大きすぎます");
        }
    }
    close(fd);
    in_pos = 0;

    obj_entry[obj] = -1;
    obj_code_off[obj] = code_used;
    obj_str_off[obj] = str_used;

    if (!expect_tok("iwacobj"))
        return 0;
    if (read_num() != 1)
        return fail("オブジェクトの版が違います");
    if (!expect_tok("target"))
        return 0;
    read_tok();
    if (!str_eq(tok_buf, "kosaba"))
        return fail("対象が kosaba ではありません");

    for (;;) {
        read_tok();
        if (tok_buf[0] == '\0')
            return fail("end がありません");
        if (str_eq(tok_buf, "end"))
            break;
        if (str_eq(tok_buf, "entry")) {
            obj_entry[obj] = (int)read_num();
        } else if (str_eq(tok_buf, "code")) {
            long n = read_num();
            obj_code_len[obj] = (int)n;
            if (!read_hex_bytes(code_pool, &code_used, 262144, n))
                return 0;
        } else if (str_eq(tok_buf, "strings")) {
            long n = read_num();
            obj_str_len[obj] = (int)n;
            if (!read_hex_bytes(str_pool, &str_used, 131072, n))
                return 0;
        } else if (str_eq(tok_buf, "arrays")) {
            read_num();  /* 合計は記録だけで、配置は arr から */
        } else if (str_eq(tok_buf, "func")) {
            read_tok();
            int s = new_sym(tok_buf);
            if (s < 0)
                return 0;
            int off = (int)read_num();
            int size = (int)read_num();
            if (!set_kind(s, 1))
                return 0;
            if (sym_obj[s] >= 0)
                return fail("関数の定義が二つあります");
            sym_obj[s] = obj;
            sym_off[s] = off;
            sym_size[s] = size;
            if (nseq >= MAX_SEQ)
                return fail("関数が多すぎます");
            seq_sym[nseq++] = s;
        } else if (str_eq(tok_buf, "decl")) {
            read_tok();
            int s = new_sym(tok_buf);
            if (s < 0)
                return 0;
            if (!set_kind(s, 1))
                return 0;
            if (nseq >= MAX_SEQ)
                return fail("関数が多すぎます");
            seq_sym[nseq++] = s;
        } else if (str_eq(tok_buf, "var") ||
                   str_eq(tok_buf, "varc")) {
            int firm = str_eq(tok_buf, "var");
            read_tok();
            int s = new_sym(tok_buf);
            if (s < 0)
                return 0;
            if (!set_kind(s, 2))
                return 0;
            if (sym_obj[s] < 0) {
                /* 初出: 枠番号を割り当てる (初出の順) */
                sym_obj[s] = obj;
                sym_off[s] = var_slots++;
            }
            if (firm) {
                if (sym_firm[s])
                    return fail("変数の本定義が二つあります");
                sym_firm[s] = 1;
            }
        } else if (str_eq(tok_buf, "arr")) {
            read_tok();
            int s = new_sym(tok_buf);
            if (s < 0)
                return 0;
            read_num();  /* 自分の領域でのずれは使わない */
            int size = (int)read_num();
            if (!set_kind(s, 3))
                return 0;
            if (sym_obj[s] >= 0) {
                if (sym_size[s] != size)
                    return fail("配列の大きさが食い違います");
            } else {
                sym_obj[s] = obj;
                sym_size[s] = size;
                sym_off[s] = (int)arr_area;
                arr_area += size;
            }
        } else if (str_eq(tok_buf, "init")) {
            read_tok();
            int s = new_sym(tok_buf);
            if (s < 0)
                return 0;
            if (!set_kind(s, 2))
                return 0;
            if (sym_hasinit[s])
                return fail("初期値が二つあります");
            sym_hasinit[s] = 1;
            sym_init_kind[s] = 1;
            sym_init_val[s] = read_num();
        } else if (str_eq(tok_buf, "iptr")) {
            read_tok();
            int s = new_sym(tok_buf);
            if (s < 0)
                return 0;
            if (!set_kind(s, 2))
                return 0;
            if (sym_hasinit[s])
                return fail("初期値が二つあります");
            read_tok();
            sym_hasinit[s] = 1;
            if (str_eq(tok_buf, "str")) {
                sym_init_kind[s] = 2;
                sym_init_val[s] = read_num();
                sym_init_obj[s] = obj;
            } else if (str_eq(tok_buf, "sym")) {
                read_tok();
                int t = new_sym(tok_buf);
                if (t < 0)
                    return 0;
                sym_init_kind[s] = 3;
                sym_init_sym[s] = t;
                sym_init_val[s] = read_num();
            } else
                return fail("iptr の形が違います");
        } else if (str_eq(tok_buf, "call") ||
                   str_eq(tok_buf, "callopt")) {
            int kind = str_eq(tok_buf, "call") ? 1 : 2;
            int off = (int)read_num();
            read_tok();
            int s = new_sym(tok_buf);
            if (s < 0)
                return 0;
            if (!set_kind(s, 1))
                return 0;
            if (!add_rel(obj, off, kind, s, 0))
                return 0;
        } else if (str_eq(tok_buf, "addr")) {
            int off = (int)read_num();
            read_tok();
            int s = new_sym(tok_buf);
            if (s < 0)
                return 0;
            long add = read_num();
            if (!add_rel(obj, off, 3, s, add))
                return 0;
        } else if (str_eq(tok_buf, "addrstr")) {
            int off = (int)read_num();
            long v = read_num();
            if (!add_rel(obj, off, 4, -1, v))
                return 0;
        } else if (str_eq(tok_buf, "addabs")) {
            int off = (int)read_num();
            long v = read_num();
            if (!add_rel(obj, off, 5, -1, v))
                return 0;
        } else if (str_eq(tok_buf, "undef")) {
            read_tok();
            int s = new_sym(tok_buf);
            if (s < 0)
                return 0;
            if (!set_kind(s, 1))
                return 0;
        } else
            return fail("知らないレコードがあります");
    }
    return 1;
}

/* ================= 配置 ================= */
static int entry_obj;
static int out_len;            /* out_code の長さ */
static long str_total;

static void copy_bytes(char *dst, int doff, char *src, int soff, int n) {
    for (int i = 0; i < n; i++)
        dst[doff + i] = src[soff + i];
}

static int layout(void) {
    entry_obj = -1;
    for (int o = 0; o < nobj; o++) {
        if (obj_entry[o] >= 0) {
            if (entry_obj >= 0)
                return fail("入口のあるオブジェクトが二つあります");
            entry_obj = o;
        }
    }
    if (entry_obj < 0)
        return fail("入口 (main など) のあるオブジェクトがありません");
    /* コード: 入口の断片を先頭に、本体は登録順に */
    out_len = 0;
    copy_bytes(out_code, 0, code_pool, obj_code_off[entry_obj],
               obj_entry[entry_obj]);
    out_len = obj_entry[entry_obj];
    for (int i = 0; i < nseq; i++) {
        int s = seq_sym[i];
        if (sym_kind[s] != 1 || sym_obj[s] < 0 || sym_place[s] >= 0)
            continue;
        if (out_len + sym_size[s] > 262144)
            return fail("コードが大きすぎます");
        sym_place[s] = out_len;
        sym_addr[s] = CODE_BASE + out_len;  /* 0x40000000 */
        copy_bytes(out_code, out_len, code_pool,
                   obj_code_off[sym_obj[s]] + sym_off[s], sym_size[s]);
        out_len += sym_size[s];
    }
    /* 文字列: オブジェクトの並び順に */
    long spos = 0;
    for (int o = 0; o < nobj; o++) {
        obj_str_final[o] = (int)spos;
        spos += obj_str_len[o];
    }
    str_total = spos;
    /* データの住所 */
    for (int s = 0; s < nsym; s++) {
        if (sym_kind[s] == 3)
            sym_addr[s] = DATA_BASE + str_total + sym_off[s];
        /*                              0x40200000 */
        else if (sym_kind[s] == 2)
            sym_addr[s] = DATA_BASE + str_total + arr_area +
                          (long)sym_off[s] * 8;
    }
    return 1;
}

/* 穴の場所 (オブジェクト内) → 最終コードでの位置 */
static int site_final(int obj, int off) {
    if (obj == entry_obj && off < obj_entry[obj])
        return off;
    for (int s = 0; s < nsym; s++) {
        if (sym_kind[s] == 1 && sym_obj[s] == obj &&
            sym_place[s] >= 0 &&
            sym_off[s] <= off && off < sym_off[s] + sym_size[s])
            return sym_place[s] + (off - sym_off[s]);
    }
    return -1;
}

/* ================= 穴埋め ================= */
static long get32(int off) {
    long b0 = out_code[off] & 0xff;
    long b1 = out_code[off + 1] & 0xff;
    long b2 = out_code[off + 2] & 0xff;
    long b3 = out_code[off + 3] & 0xff;
    return b0 + b1 * 256 + b2 * 65536 + b3 * 16777216;
}

static void put32(int off, long v) {
    out_code[off] = (char)(v & 0xff);
    out_code[off + 1] = (char)((v >> 8) & 0xff);
    out_code[off + 2] = (char)((v >> 16) & 0xff);
    out_code[off + 3] = (char)((v >> 24) & 0xff);
}

static int fill_all(void) {
    for (int r = 0; r < nrel; r++) {
        int place = site_final(rel_obj[r], rel_off[r]);
        if (place < 0)
            return fail("穴の場所が決まりません");
        long pc = CODE_BASE + place;
        int kind = rel_kind[r];
        if (kind == 1 || kind == 2) {
            int s = rel_sym[r];
            if (sym_obj[s] < 0) {
                if (kind == 1) {
                    err_sym = s;
                    return fail("未定義の関数が呼ばれています");
                }
                put32(place, 0x13);  /* nop (addi x0, x0, 0) */
                continue;
            }
            long diff = sym_addr[s] - pc;
            if (diff < -1048576L || diff > 1048572L)
                return fail("呼び出しが届きません (約1MBまで)");
            long w = get32(place) & 0xFFF;
            w |= ((diff >> 20) & 1) << 31;
            w |= ((diff >> 1) & 0x3FF) << 21;
            w |= ((diff >> 11) & 1) << 20;
            w |= ((diff >> 12) & 0xFF) << 12;
            put32(place, w);
        } else {
            long target;
            if (kind == 3) {
                int s = rel_sym[r];
                if (sym_kind[s] == 0) {
                    err_sym = s;
                    return fail("定義のない名前が使われています");
                }
                target = sym_addr[s] + rel_val[r];
            } else if (kind == 4)
                target = DATA_BASE + obj_str_final[rel_obj[r]] +
                         rel_val[r];
            else
                target = rel_val[r];
            long diff = target - pc;
            long hi = (diff + 0x800) >> 12;
            long lo = diff - (hi << 12);
            long w0 = get32(place);
            w0 = (w0 & 0xFFFFF) | ((hi & 0xFFFFF) << 12);
            put32(place, w0);
            long w1 = get32(place + 4);
            w1 = (w1 & 0xFFFFF) | ((lo & 0xFFF) << 20);
            put32(place + 4, w1);
        }
    }
    /* iptr の相手の点検 */
    for (int s = 0; s < nsym; s++) {
        if (sym_kind[s] == 2 && sym_hasinit[s] &&
            sym_init_kind[s] == 3 && sym_kind[sym_init_sym[s]] == 0) {
            err_sym = sym_init_sym[s];
            return fail("初期値の指す名前が定義されていません");
        }
    }
    return 1;
}

/* ================= ELF を書く =================
 * iwac の encode と同じ形 (PT_LOAD 3本、BSS の流儀) */
static void out_byte(int b) {
    char c = (char)b;
    write(1, &c, 1);
}

static void out16(long v) {
    out_byte((int)(v & 0xff));
    out_byte((int)((v >> 8) & 0xff));
}

static void out32(long v) {
    out16(v & 0xffff);
    out16((v >> 16) & 0xffff);
}

static void out64(long v) {
    out32(v & 0xffffffffL);
    out32((v >> 32) & 0xffffffffL);
}

static void out_phdr(long flags, long off, long va, long fsz,
                     long msz, long align) {
    out32(1);
    out32(flags);
    out64(off);
    out64(va);
    out64(va);
    out64(fsz);
    out64(msz);
    out64(align);
}

static void write_elf(void) {
    int has_inits = 0;
    for (int s = 0; s < nsym; s++)
        if (sym_kind[s] == 2 && sym_hasinit[s])
            has_inits = 1;
    long code_base = CODE_BASE;
    long data_base = DATA_BASE;
    long data_off = (4096L + out_len + 4095) & ~4095L;
    long gbl_bytes = (long)var_slots * 8;

    out_byte(0x7f);
    out_byte('E');
    out_byte('L');
    out_byte('F');
    out_byte(2);
    out_byte(1);
    out_byte(1);
    out_byte(0);
    for (int i = 0; i < 8; i++)
        out_byte(0);
    out16(2);         /* ET_EXEC */
    out16(243);       /* RISC-V */
    out32(1);
    out64(code_base); /* e_entry */
    out64(64);        /* e_phoff */
    out64(0);
    out32(0);
    out16(64);
    out16(56);
    out16(3);         /* phdr 3本 */
    out16(0);
    out16(0);
    out16(0);
    out_phdr(5, 4096, code_base, out_len, out_len, 0x1000);
    out_phdr(6, data_off, data_base, str_total,
             str_total + arr_area, 0x1000);
    out_phdr(6, data_off + str_total, data_base + str_total + arr_area,
             has_inits ? gbl_bytes : 0, gbl_bytes, 1);
    for (int i = 232; i < 4096; i++)
        out_byte(0);
    write(1, out_code, out_len);
    for (long i = 4096L + out_len; i < data_off; i++)
        out_byte(0);
    if (str_total > 0)
        write(1, str_pool, str_total);
    if (has_inits) {
        for (int slot = 0; slot < var_slots; slot++) {
            long v = 0;
            for (int s = 0; s < nsym; s++) {
                if (sym_kind[s] != 2 || sym_off[s] != slot ||
                    !sym_hasinit[s])
                    continue;
                if (sym_init_kind[s] == 1)
                    v = sym_init_val[s];
                else if (sym_init_kind[s] == 2)
                    v = data_base + obj_str_final[sym_init_obj[s]] +
                        sym_init_val[s];
                else
                    v = sym_addr[sym_init_sym[s]] + sym_init_val[s];
                break;
            }
            out64(v);
        }
    }
}

int main(int argc, char **argv) {
    int base;
    base = 1;
    /* nori --out <名前> obj... : OS 上での出力名 (v34) */
    if (argc >= 4 && str_eq(argv[1], "--out")) {
        kosaba_out_name = argv[2];
        base = 3;
    }
    if (argc - base < 1) {
        puts_fd(2, "usage: nori [--out <name>] <obj.iwo> ... > prog.elf\n");
        return 1;
    }
    if (argc - base > MAX_OBJ) {
        puts_fd(2, "nori: オブジェクトが多すぎます\n");
        return 1;
    }
    err_sym = -1;
    nobj = argc - base;
    for (int i = 0; i < nobj; i++) {
        if (!read_object(i, argv[i + base])) {
            puts_fd(2, "nori: ");
            puts_fd(2, errmsg);
            if (err_sym >= 0) {
                puts_fd(2, ": ");
                puts_fd(2, sym_str(err_sym));
            }
            puts_fd(2, "\n");
            return 1;
        }
    }
    if (!layout() || !fill_all()) {
        puts_fd(2, "nori: ");
        puts_fd(2, errmsg);
        if (err_sym >= 0) {
            puts_fd(2, ": ");
            puts_fd(2, sym_str(err_sym));
        }
        puts_fd(2, "\n");
        return 1;
    }
    write_elf();
    return 0;
}
