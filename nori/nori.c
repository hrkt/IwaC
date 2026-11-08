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

/* libdashi (共通関数の束、doc/libdashi.md) */
void ds_puts(int fd, char *s);
int ds_streq(char *a, char *b);
int ds_hexval(int c);

/* ================= 大きさの約束 ================= */
static int MAX_OBJ = 32;       /* オブジェクトの数 */
static int MAX_SYM = 4096;     /* 名前表 */
static int MAX_REL = 16384;    /* 穴の数 */
static int MAX_AINIT = 262144; /* 配列の初期値の置き場 (v40) */
static int MAX_SEQ = 4096;     /* 登録順の控え */
/* バイト列の置き場 (静的データとして持つ) */
static char in_buf[393216];    /* オブジェクト1つの全文 */
static char code_pool[262144]; /* 全オブジェクトのコード */
static char str_pool[131072];  /* 全オブジェクトの文字列 */
static char ainit_buf[262144]; /* 配列の初期値のバイト列 (v40) */
static char out_code[262144];  /* 並べた後のコード */
static char name_pool[65536];  /* 名前の文字列 */

/* 配置の約束 (KosabaOS のユーザープログラム) */
static long CODE_BASE = 0x40000000;
static long DATA_BASE = 0x40200000;

/* ================= 道具 ================= */
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
static int sym_ainit_off[4096];/* 配列: 初期値の置き場 (v40) */
static int sym_ainit_len[4096];/* 配列: 初期値のバイト数。
                                * -1 = 初期値なし */
static int ainit_used;         /* ainit_buf の使用量 */
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
    sym_ainit_off[s] = 0;
    sym_ainit_len[s] = -1;
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
    if (!ds_streq(tok_buf, want))
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


/* ================= ELF オブジェクトを読む =================
 * 形式 version 2 (doc/elf-object.md)。中身は .iwo と
 * 同じ内部構造 (名前表・登録順・穴の控え) に写す。
 * 配置と穴埋めは .iwo と共通の後段が担う */
static int esym_tag[16384];  /* 0 = 名前あり、1 = 文字列
                              * セクション、2 = 絶対、3 = その他、
                              * 4 = 名前あり (弱い) */
static int esym_sym[16384];  /* 名前 → 名前表の番号 */
static int eabs_idx[256];
static long eabs_val[256];
static int neabs;
static int edata_off[256];   /* .data の変数 (初期値の受け口。
                              * 実測ピークは iwac の断片で 57) */
static int edata_sym[256];
static long edata_val[256];
static int edata_hit[256];
static int nedata;

static int eget8(long off) {
    return in_buf[off] & 0xff;
}

static int eget16(long off) {
    return eget8(off) + eget8(off + 1) * 256;
}

static long eget32(long off) {
    return (long)eget16(off) + (long)eget16(off + 2) * 65536;
}

static long eget64(long off) {
    return eget32(off) + eget32(off + 4) * (1L << 32);
}

/* セクションヘッダの置き場 (1つ分ずつ読む) */
static long esec_type;
static long esec_off;
static long esec_size;
static long esec_link;
static long esec_info;

static void eget_shdr(long shoff, int i) {
    long b = shoff + (long)i * 64;
    esec_type = eget32(b + 4);
    esec_off = eget64(b + 24);
    esec_size = eget64(b + 32);
    esec_link = eget32(b + 40);
    esec_info = eget32(b + 44);
}

/* セクション i の名前を buf に写す (32文字まで) */
static void esec_name(long shoff, long shstr_off, int i, char *buf) {
    long b = shoff + (long)i * 64;
    long noff = shstr_off + eget32(b);
    int n = 0;
    while (n < 31) {
        char c = in_buf[noff + n];
        buf[n] = c;
        n++;
        if (c == '\0')
            break;
    }
    buf[31] = '\0';
}

/* 配列の初期値を in_buf の位置から取り込む (v40)。
 * 末尾の 0 バイトは実体に入れない (.iwo 側と同じ
 * 約束)。全部 0 の写しは「初期値なし」と同じで、
 * 仮定義側のオブジェクトが中身つきの節を持って
 * いた場合に当たる。既に初期値がある名前とは、
 * 同じ中身ならよい */
static int adopt_ainit(int s, long off, long n) {
    while (n > 0 && in_buf[off + n - 1] == 0)
        n--;
    if (n == 0)
        return 1;
    if (sym_ainit_len[s] >= 0) {
        if (sym_ainit_len[s] != n)
            return fail("配列の初期値が食い違います");
        for (long i = 0; i < n; i++)
            if (ainit_buf[sym_ainit_off[s] + i] != in_buf[off + i])
                return fail("配列の初期値が二つあります");
        return 1;
    }
    if (ainit_used + n > MAX_AINIT)
        return fail("配列の初期値が大きすぎます");
    sym_ainit_off[s] = ainit_used;
    for (long i = 0; i < n; i++) {
        ainit_buf[ainit_used] = in_buf[off + i];
        ainit_used++;
    }
    sym_ainit_len[s] = (int)n;
    return 1;
}

static int read_object_elf(int obj) {
    if (eget8(4) != 2 || eget8(5) != 1)
        return fail("ELF の形が違います (64ビット LE のみ)");
    if (eget16(16) != 1 || eget16(18) != 243)
        return fail("ELF の形が違います (ET_REL / RISC-V のみ)");
    long shoff = eget64(40);
    int shnum = eget16(60);
    int shstrndx = eget16(62);
    if (shnum > 32)
        return fail("セクションが多すぎます");
    eget_shdr(shoff, shstrndx);
    long shstr_off = esec_off;

    /* セクションを探す */
    int text_i = -1;
    int data_i = -1;
    int strings_i = -1;
    int arrays_i = -1;
    int symtab_i = -1;
    for (int i = 0; i < shnum; i++) {
        char nm[32];
        esec_name(shoff, shstr_off, i, nm);
        if (ds_streq(nm, ".text"))
            text_i = i;
        else if (ds_streq(nm, ".data"))
            data_i = i;
        else if (ds_streq(nm, ".iwac.strings"))
            strings_i = i;
        else if (ds_streq(nm, ".iwac.arrays"))
            arrays_i = i;
        eget_shdr(shoff, i);
        if (esec_type == 2)
            symtab_i = i;
    }
    if (text_i < 0 || symtab_i < 0)
        return fail("ELF に .text / .symtab がありません");

    /* コードと文字列を写す */
    eget_shdr(shoff, text_i);
    obj_code_len[obj] = (int)esec_size;
    for (long k = 0; k < esec_size; k++) {
        if (code_used >= 262144)
            return fail("コードが入りません");
        code_pool[code_used] = in_buf[esec_off + k];
        code_used++;
    }
    if (strings_i >= 0) {
        eget_shdr(shoff, strings_i);
        obj_str_len[obj] = (int)esec_size;
        for (long k = 0; k < esec_size; k++) {
            if (str_used >= 131072)
                return fail("文字列が入りません");
            str_pool[str_used] = in_buf[esec_off + k];
            str_used++;
        }
    }

    /* シンボル表 */
    eget_shdr(shoff, symtab_i);
    long symtab_off = esec_off;
    long nsym_e = esec_size / 24;
    int strtab_i = (int)esec_link;
    eget_shdr(shoff, strtab_i);
    long strtab_off = esec_off;
    long data_off = 0;
    if (data_i >= 0) {
        eget_shdr(shoff, data_i);
        data_off = esec_off;
    }
    /* 配列の節: 中身 (PROGBITS) があるか (v40) */
    long earr_off = 0;
    int earr_type = 0;
    if (arrays_i >= 0) {
        eget_shdr(shoff, arrays_i);
        earr_off = esec_off;
        earr_type = (int)esec_type;
    }
    if (nsym_e > 16384)
        return fail("シンボルが多すぎます");
    neabs = 0;
    nedata = 0;
    for (long si = 0; si < nsym_e; si++) {
        long b = symtab_off + si * 24;
        long name_off = eget32(b);
        int info = eget8(b + 4);
        int bind = info >> 4;
        int type = info & 15;
        int shndx = eget16(b + 6);
        long value = eget64(b + 8);
        long size = eget64(b + 16);
        esym_tag[si] = 3;
        esym_sym[si] = -1;
        if (type == 3) {
            /* SECTION: 文字列セクションだけ印を付ける */
            if (shndx >= 0 && shndx < shnum) {
                char snm[32];
                esec_name(shoff, shstr_off, shndx, snm);
                if (ds_streq(snm, ".iwac.strings"))
                    esym_tag[si] = 1;
            }
            continue;
        }
        if (shndx == 0xfff1) {
            /* SHN_ABS: 値そのものが住所 */
            esym_tag[si] = 2;
            if (neabs < 256) {
                eabs_idx[neabs] = (int)si;
                eabs_val[neabs] = value;
                neabs++;
            }
            continue;
        }
        if (name_off == 0)
            continue;
        char *gnm = in_buf + strtab_off + name_off;
        if (ds_streq(gnm, "iwac_entry")) {
            obj_entry[obj] = (int)size;
            continue;
        }
        int s = new_sym(gnm);
        if (s < 0)
            return 0;
        esym_sym[si] = s;
        esym_tag[si] = bind == 2 ? 4 : 0;
        if (type == 2 || (type == 0 && shndx == 0)) {
            /* 関数 (定義・宣言・未定義) */
            if (!set_kind(s, 1))
                return 0;
            if (nseq >= MAX_SEQ)
                return fail("関数が多すぎます");
            seq_sym[nseq++] = s;
            if (shndx == text_i) {
                if (sym_obj[s] >= 0)
                    return fail("関数の定義が二つあります");
                sym_obj[s] = obj;
                sym_off[s] = (int)value;
                sym_size[s] = (int)size;
            }
        } else if (type == 1 && shndx == 0xfff2) {
            /* 変数の仮定義 (SHN_COMMON) */
            if (!set_kind(s, 2))
                return 0;
            if (sym_obj[s] < 0) {
                sym_obj[s] = obj;
                sym_off[s] = var_slots;
                var_slots += (int)(size / 8);
            }
        } else if (type == 1 && shndx == data_i) {
            /* 初期値のある変数 (本定義) */
            if (!set_kind(s, 2))
                return 0;
            if (sym_obj[s] < 0) {
                sym_obj[s] = obj;
                sym_off[s] = var_slots;
                var_slots += (int)(size / 8);
            }
            if (sym_firm[s])
                return fail("変数の本定義が二つあります");
            sym_firm[s] = 1;
            if (nedata >= 256)
                return fail("変数が多すぎます");
            edata_off[nedata] = (int)value;
            edata_sym[nedata] = s;
            edata_val[nedata] = eget64(data_off + value);
            edata_hit[nedata] = 0;
            nedata++;
        } else if (type == 1 && shndx == arrays_i) {
            /* グローバル配列 */
            if (!set_kind(s, 3))
                return 0;
            if (sym_obj[s] >= 0) {
                if (sym_size[s] != (int)size)
                    return fail("配列の大きさが食い違います");
            } else {
                sym_obj[s] = obj;
                sym_size[s] = (int)size;
                sym_off[s] = (int)arr_area;
                arr_area += size;
            }
            if (earr_type == 1) {
                /* 中身のある節 (v40): この配列の分を
                 * 初期値として取り込む */
                if (!adopt_ainit(s, earr_off + value, size))
                    return 0;
            }
        }
    }

    /* 再配置 */
    for (int i = 0; i < shnum; i++) {
        eget_shdr(shoff, i);
        if (esec_type != 4)
            continue;
        int target_i = (int)esec_info;
        char tnm[32];
        esec_name(shoff, shstr_off, target_i, tnm);
        int is_text = ds_streq(tnm, ".text");
        int is_data = ds_streq(tnm, ".data");
        if (!is_text && !is_data)
            continue;
        long cnt = esec_size / 24;
        for (long ri = 0; ri < cnt; ri++) {
            long rb = esec_off + ri * 24;
            long roff = eget64(rb);
            long rinfo = eget64(rb + 8);
            long rsym = rinfo / (1L << 32);
            long rtype = rinfo % (1L << 32);
            long radd = eget64(rb + 16);
            if (is_data) {
                /* 文字列へのポインタ初期値 (R_RISCV_64) */
                if (rtype != 2 || esym_tag[rsym] != 1)
                    return fail("知らないデータ再配置があります");
                int hit = -1;
                for (int e = 0; e < nedata; e++)
                    if (edata_off[e] == (int)roff)
                        hit = e;
                if (hit < 0)
                    return fail("初期値の受け口がありません");
                int vs = edata_sym[hit];
                if (sym_hasinit[vs])
                    return fail("初期値が二つあります");
                sym_hasinit[vs] = 1;
                sym_init_kind[vs] = 2;
                sym_init_val[vs] = radd;
                sym_init_obj[vs] = obj;
                edata_hit[hit] = 1;
                continue;
            }
            if (rtype == 17) {
                /* R_RISCV_JAL: 関数呼び出し */
                if (esym_tag[rsym] != 0 && esym_tag[rsym] != 4)
                    return fail("呼び出しの相手が違います");
                int kind = esym_tag[rsym] == 4 ? 2 : 1;
                if (!add_rel(obj, (int)roff, kind,
                             esym_sym[rsym], 0))
                    return 0;
            } else if (rtype == 23) {
                /* R_RISCV_PCREL_HI20: 住所作りの組の頭 */
                if (esym_tag[rsym] == 1) {
                    if (!add_rel(obj, (int)roff, 4, -1, radd))
                        return 0;
                } else if (esym_tag[rsym] == 0 ||
                           esym_tag[rsym] == 4) {
                    if (!add_rel(obj, (int)roff, 3,
                                 esym_sym[rsym], radd))
                        return 0;
                } else
                    return fail("住所作りの相手が違います");
            } else if (rtype == 26) {
                /* R_RISCV_HI20: 絶対アドレスの組の頭 */
                long val = 0;
                int found = 0;
                for (int e = 0; e < neabs; e++) {
                    if (eabs_idx[e] == (int)rsym) {
                        val = eabs_val[e];
                        found = 1;
                    }
                }
                if (!found)
                    return fail("絶対アドレスが見つかりません");
                if (!add_rel(obj, (int)roff, 5, -1, val))
                    return 0;
            }
            /* 24 / 27 (LO12) と 51 (RELAX) は組の後半
             * または印なので読み飛ばす */
        }
    }
    /* 再配置の無かった初期値は数の初期値として確定 */
    for (int e = 0; e < nedata; e++) {
        if (edata_hit[e])
            continue;
        int fs = edata_sym[e];
        if (sym_hasinit[fs])
            return fail("初期値が二つあります");
        sym_hasinit[fs] = 1;
        sym_init_kind[fs] = 1;
        sym_init_val[fs] = edata_val[e];
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
    /* 形式 version 2 (ELF) か、テキスト (.iwo) かを
     * 先頭のマジックで見分ける (doc/elf-object.md) */
    if (in_len >= 4 && eget8(0) == 0x7f && in_buf[1] == 'E' &&
        in_buf[2] == 'L' && in_buf[3] == 'F') {
        obj_code_off[obj] = code_used;
        obj_str_off[obj] = str_used;
        return read_object_elf(obj);
    }
    obj_code_off[obj] = code_used;
    obj_str_off[obj] = str_used;

    if (!expect_tok("iwacobj"))
        return 0;
    if (read_num() != 1)
        return fail("オブジェクトの版が違います");
    if (!expect_tok("target"))
        return 0;
    read_tok();
    if (!ds_streq(tok_buf, "kosaba"))
        return fail("対象が kosaba ではありません");

    for (;;) {
        read_tok();
        if (tok_buf[0] == '\0')
            return fail("end がありません");
        if (ds_streq(tok_buf, "end"))
            break;
        if (ds_streq(tok_buf, "entry")) {
            obj_entry[obj] = (int)read_num();
        } else if (ds_streq(tok_buf, "code")) {
            long n = read_num();
            obj_code_len[obj] = (int)n;
            if (!read_hex_bytes(code_pool, &code_used, 262144, n))
                return 0;
        } else if (ds_streq(tok_buf, "strings")) {
            long n = read_num();
            obj_str_len[obj] = (int)n;
            if (!read_hex_bytes(str_pool, &str_used, 131072, n))
                return 0;
        } else if (ds_streq(tok_buf, "arrays")) {
            read_num();  /* 合計は記録だけで、配置は arr から */
        } else if (ds_streq(tok_buf, "func")) {
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
        } else if (ds_streq(tok_buf, "decl")) {
            read_tok();
            int s = new_sym(tok_buf);
            if (s < 0)
                return 0;
            if (!set_kind(s, 1))
                return 0;
            if (nseq >= MAX_SEQ)
                return fail("関数が多すぎます");
            seq_sym[nseq++] = s;
        } else if (ds_streq(tok_buf, "var") ||
                   ds_streq(tok_buf, "varc")) {
            int firm = ds_streq(tok_buf, "var");
            read_tok();
            int s = new_sym(tok_buf);
            if (s < 0)
                return 0;
            int cnt = (int)read_num();  /* 占める枠の数 (v37) */
            if (!set_kind(s, 2))
                return 0;
            if (sym_obj[s] < 0) {
                /* 初出: 枠番号を割り当てる (初出の順) */
                sym_obj[s] = obj;
                sym_off[s] = var_slots;
                var_slots += cnt;
            }
            if (firm) {
                if (sym_firm[s])
                    return fail("変数の本定義が二つあります");
                sym_firm[s] = 1;
            }
        } else if (ds_streq(tok_buf, "arr")) {
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
        } else if (ds_streq(tok_buf, "ainit")) {
            /* 配列の初期値 (v40)。直前の arr で登録された
             * 配列に、バイト列を結び付ける */
            read_tok();
            int s = new_sym(tok_buf);
            if (s < 0)
                return 0;
            long n = read_num();
            if (!set_kind(s, 3))
                return 0;
            if (sym_ainit_len[s] >= 0)
                return fail("配列の初期値が二つあります");
            int astart = ainit_used;
            if (!read_hex_bytes(ainit_buf, &ainit_used, MAX_AINIT, n))
                return 0;
            /* 末尾の 0 バイトは実体に入れない (一括版の
             * arr_init_end と同じ約束。全部 0 なら
             * 初期値なしと同じ) */
            long tn = n;
            while (tn > 0 && ainit_buf[astart + tn - 1] == 0)
                tn--;
            ainit_used = astart + (int)tn;
            if (tn > 0) {
                sym_ainit_off[s] = astart;
                sym_ainit_len[s] = (int)tn;
            }
        } else if (ds_streq(tok_buf, "init")) {
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
        } else if (ds_streq(tok_buf, "iptr")) {
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
            if (ds_streq(tok_buf, "str")) {
                sym_init_kind[s] = 2;
                sym_init_val[s] = read_num();
                sym_init_obj[s] = obj;
            } else if (ds_streq(tok_buf, "sym")) {
                read_tok();
                int t = new_sym(tok_buf);
                if (t < 0)
                    return 0;
                sym_init_kind[s] = 3;
                sym_init_sym[s] = t;
                sym_init_val[s] = read_num();
            } else
                return fail("iptr の形が違います");
        } else if (ds_streq(tok_buf, "call") ||
                   ds_streq(tok_buf, "callopt")) {
            int kind = ds_streq(tok_buf, "call") ? 1 : 2;
            int off = (int)read_num();
            read_tok();
            int s = new_sym(tok_buf);
            if (s < 0)
                return 0;
            if (!set_kind(s, 1))
                return 0;
            if (!add_rel(obj, off, kind, s, 0))
                return 0;
        } else if (ds_streq(tok_buf, "addr")) {
            int off = (int)read_num();
            read_tok();
            int s = new_sym(tok_buf);
            if (s < 0)
                return 0;
            long add = read_num();
            if (!add_rel(obj, off, 3, s, add))
                return 0;
        } else if (ds_streq(tok_buf, "addrstr")) {
            int off = (int)read_num();
            long v = read_num();
            if (!add_rel(obj, off, 4, -1, v))
                return 0;
        } else if (ds_streq(tok_buf, "addabs")) {
            int off = (int)read_num();
            long v = read_num();
            if (!add_rel(obj, off, 5, -1, v))
                return 0;
        } else if (ds_streq(tok_buf, "undef")) {
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
    /* 配列の初期値が実体として届く末尾 (v40。
     * iwac 一括版の arr_init_end と同じ考え方) */
    long ainit_span = 0;
    for (int s = 0; s < nsym; s++) {
        if (sym_kind[s] == 3 && sym_ainit_len[s] > 0) {
            long e = (long)sym_off[s] + sym_ainit_len[s];
            if (e > ainit_span)
                ainit_span = e;
        }
    }
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
    out_phdr(6, data_off, data_base, str_total + ainit_span,
             str_total + arr_area, 0x1000);
    out_phdr(6, data_off + str_total + ainit_span,
             data_base + str_total + arr_area,
             has_inits ? gbl_bytes : 0, gbl_bytes, 1);
    for (int i = 232; i < 4096; i++)
        out_byte(0);
    write(1, out_code, out_len);
    for (long i = 4096L + out_len; i < data_off; i++)
        out_byte(0);
    if (str_total > 0)
        write(1, str_pool, str_total);
    if (ainit_span > 0) {
        /* 配列の初期値: 始まる位置まで 0 を埋め、
         * バイト列を置く、を並び順に繰り返す */
        long pos = 0;
        for (;;) {
            int best = -1;
            for (int s = 0; s < nsym; s++) {
                if (sym_kind[s] != 3 || sym_ainit_len[s] <= 0)
                    continue;
                if (sym_off[s] >= pos &&
                    (best < 0 || sym_off[s] < sym_off[best]))
                    best = s;
            }
            if (best < 0)
                break;
            for (; pos < sym_off[best]; pos++)
                out_byte(0);
            write(1, ainit_buf + sym_ainit_off[best],
                  sym_ainit_len[best]);
            pos = (long)sym_off[best] + sym_ainit_len[best];
        }
        for (; pos < ainit_span; pos++)
            out_byte(0);
    }
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
    if (argc >= 4 && ds_streq(argv[1], "--out")) {
        kosaba_out_name = argv[2];
        base = 3;
    }
    if (argc - base < 1) {
        ds_puts(2, "usage: nori [--out <name>] <obj.iwo> ... > prog.elf\n");
        return 1;
    }
    if (argc - base > MAX_OBJ) {
        ds_puts(2, "nori: オブジェクトが多すぎます\n");
        return 1;
    }
    err_sym = -1;
    nobj = argc - base;
    for (int i = 0; i < nobj; i++) {
        if (!read_object(i, argv[i + base])) {
            ds_puts(2, "nori: ");
            ds_puts(2, errmsg);
            if (err_sym >= 0) {
                ds_puts(2, ": ");
                ds_puts(2, sym_str(err_sym));
            }
            ds_puts(2, "\n");
            return 1;
        }
    }
    if (!layout() || !fill_all()) {
        ds_puts(2, "nori: ");
        ds_puts(2, errmsg);
        if (err_sym >= 0) {
            ds_puts(2, ": ");
            ds_puts(2, sym_str(err_sym));
        }
        ds_puts(2, "\n");
        return 1;
    }
    write_elf();
    return 0;
}
