/* nori -- IwaC のためのリンカ (分割コンパイルへの
 * 階段の段3。形式の設計は doc/elf-object.md)
 *
 * 使い方: nori a.eo b.eo ... > prog.elf
 *
 * ELF オブジェクト (v39 の形式。v42 で .iwo の
 * 読み口は引退し、ELF に一本化した) を読み、
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
static int MAX_AINIT = 262144; /* 配列の初期値の置き場 (v40) */
static int MAX_SEQ = 4096;     /* 登録順の控え */
/* バイト列の置き場 (静的データとして持つ) */
/* ストリーミング化 (doc/nori-streaming.md):
 * オブジェクトは2回読む。パスA はシンボルと
 * 大きさだけを集め、コードと文字列は溜めない。
 * パスB は配置順に読み直して、穴を埋めて、
 * そのまま書く。溜まるのは名前表と配列の
 * 初期値だけで、share_buf は最大の
 * オブジェクト1つの全文が入れば足りる */
static char share_buf[393216]; /* オブジェクト1つの全文 */
static char *in_buf;           /* = share_buf */
static char ainit_buf[262144]; /* 配列の初期値のバイト列 (v40) */
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
static char *obj_path[32];     /* パスB で読み直すための名前 */
static long obj_file_len[32];  /* パスA で読んだ長さ (照合用) */
static long obj_text_off[32];  /* ファイルの中の .text の位置 */
static int obj_code_len[32];
static int obj_entry[32];      /* 入口の長さ。無ければ -1 */
static long obj_str_foff[32];  /* ファイルの中の文字列節の位置 */
static int obj_str_len[32];
static int obj_str_final[32];  /* 配置後の文字列のずれ */

/* ================= 名前表 =================
 * 種類: 0 = 未定 (参照だけ)、1 = 関数、2 = 変数、3 = 配列 */
static int nsym;
static int sym_name[4096];     /* name_pool へのずれ */
static int sym_kind[4096];
static int sym_obj[4096];      /* 定義 (関数・配列) の出所 */
static int sym_off[4096];      /* 関数: 本体の位置、
                                * 変数: 枠番号、配列: 領域のずれ */
static int sym_size[4096];     /* 関数・変数・配列の大きさ */
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
static int sym_called[4096];   /* 強い呼び出し (call) の相手に
                                * なったか。未定義の点検用 */
static int sym_addref[4096];   /* 住所作り (addr) の相手に
                                * なったか。未定義の点検用 */
static int ainit_used;         /* ainit_buf の使用量 */
/* 関数ポインタ配列の初期値の穴埋め (v44):
 * .rela.iwac.arrays の記録。配置が決まってから
 * ainit_buf の中に住所を書く */
static int afix_arr[1024];     /* 配列の名前 */
static int afix_off[1024];     /* 配列の像の中でのずれ */
static int afix_func[1024];    /* 関数の名前 */
static int nafix;
/* 読み込み中のオブジェクトの配列と、その像の
 * 中での値 (再配置の相手を探すため) */
static int obj_arr_sym[128];
static int obj_arr_val[128];
static int obj_arr_n;
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
    sym_called[s] = 0;
    sym_addref[s] = 0;
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

/* ================= 登録順の控え ================= */
static int nseq;
static int seq_sym[4096];

/* ================= オブジェクトを読む ================= */
static long in_len;
/* ================= ELF オブジェクトを読む =================
 * 形式の設計は doc/elf-object.md。中身は内部構造
 * (名前表・登録順・穴の控え) に写し、配置と
 * 穴埋めは後段が担う */
static int esym_tag[8192];  /* 0 = 名前あり、1 = 文字列
                              * セクション、2 = 絶対、3 = その他、
                              * 4 = 名前あり (弱い) */
static int esym_sym[8192];  /* 名前 → 名前表の番号。
                             * 大きさは「オブジェクト1つの
                             * シンボル表の最大」用 (v46 で
                             * 16384 → 8192 に分離: 実測最大は
                             * 2,655。穴の表とは別枠にした) */
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
 * 末尾の 0 バイトは実体に入れない (書き手側と
 * 同じ約束)。全部 0 の写しは「初期値なし」と同じで、
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

/* 配列の初期値の置き場を need バイトまで広げる
 * (v44)。末尾の 0 を切り捨てた分や、像が全部 0
 * だった分は、0 で埋めた新しい置き場に移す。
 * 関数ポインタの住所を書く場所を確保するため */
static int ainit_reserve(int s, long need) {
    if (sym_ainit_len[s] >= need)
        return 1;
    if (ainit_used + need > MAX_AINIT)
        return fail("配列の初期値が大きすぎます");
    int noff = ainit_used;
    for (long i = 0; i < need; i++) {
        int b = 0;
        if (sym_ainit_len[s] > 0 && i < sym_ainit_len[s])
            b = ainit_buf[sym_ainit_off[s] + (int)i] & 0xff;
        ainit_buf[ainit_used] = (char)b;
        ainit_used++;
    }
    sym_ainit_off[s] = noff;
    sym_ainit_len[s] = (int)need;
    return 1;
}

/* .text のオフセットが入口か関数の中に収まるか
 * (パスA の検証。穴の最終位置そのものは
 * 配置が決まるまで分からないが、収まるか
 * どうかは sym_off と sym_size で判定できる) */
static int off_covered(int obj, long off) {
    if (obj_entry[obj] >= 0 && off < obj_entry[obj])
        return 1;
    for (int s = 0; s < nsym; s++) {
        if (sym_kind[s] == 1 && sym_obj[s] == obj &&
            sym_off[s] <= off &&
            off < (long)sym_off[s] + sym_size[s])
            return 1;
    }
    return 0;
}

static int read_object_elf(int obj) {
    obj_arr_n = 0;
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

    /* コードと文字列は溜めない (ストリーミング化)。
     * 大きさと、ファイルの中の位置だけ記録する。
     * 実体はパスB で読み直す */
    eget_shdr(shoff, text_i);
    obj_text_off[obj] = esec_off;
    obj_code_len[obj] = (int)esec_size;
    if (strings_i >= 0) {
        eget_shdr(shoff, strings_i);
        obj_str_foff[obj] = esec_off;
        obj_str_len[obj] = (int)esec_size;
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
    if (nsym_e > 8192)
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
            /* 変数の仮定義 (SHN_COMMON)。大きさの食い違いは
             * 止める (配列と同じ扱い。宣言側と定義側で表の
             * 次元がずれたまま通すと、置き場所がずれて
             * メモリを壊す。doc/split-selfhost.md 段1) */
            if (!set_kind(s, 2))
                return 0;
            if (sym_obj[s] < 0) {
                sym_obj[s] = obj;
                sym_off[s] = var_slots;
                var_slots += (int)(size / 8);
                sym_size[s] = (int)size;
            } else if (sym_size[s] != (int)size) {
                return fail("変数の大きさが食い違います");
            }
        } else if (type == 1 && shndx == data_i) {
            /* 初期値のある変数 (本定義)。仮定義との間でも
             * 大きさが食い違えば止める (同上) */
            if (!set_kind(s, 2))
                return 0;
            if (sym_obj[s] < 0) {
                sym_obj[s] = obj;
                sym_off[s] = var_slots;
                var_slots += (int)(size / 8);
                sym_size[s] = (int)size;
            } else if (sym_size[s] != (int)size) {
                return fail("変数の大きさが食い違います");
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
                /* 配列の先頭は 8 バイト境界に揃える
                 * (v63、doc/data-align.md)。詰めた
                 * ままでは実機 (K210) でワード
                 * アクセスが misaligned のトラップ
                 * エミュレーションに落ちる */
                arr_area = (arr_area + 7) & ~7L;
                sym_off[s] = (int)arr_area;
                arr_area += size;
            }
            if (obj_arr_n < 128) {
                obj_arr_sym[obj_arr_n] = s;
                obj_arr_val[obj_arr_n] = (int)value;
                obj_arr_n++;
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
        int is_arrays = ds_streq(tnm, ".iwac.arrays");
        if (!is_text && !is_data && !is_arrays)
            continue;
        long cnt = esec_size / 24;
        for (long ri = 0; ri < cnt; ri++) {
            long rb = esec_off + ri * 24;
            long roff = eget64(rb);
            long rinfo = eget64(rb + 8);
            long rsym = rinfo / (1L << 32);
            long rtype = rinfo % (1L << 32);
            long radd = eget64(rb + 16);
            if (is_arrays) {
                /* 関数ポインタ配列の初期値 (v44、
                 * R_RISCV_64): 相手は関数。埋めるのは
                 * 配置が決まってから (afix に記録) */
                if (rtype != 2 || rsym < 0 ||
                    esym_sym[rsym] < 0 ||
                    sym_kind[esym_sym[rsym]] != 1)
                    return fail("知らない配列再配置があります");
                int as = -1;
                int aoff = 0;
                for (int e = 0; e < obj_arr_n; e++) {
                    int sz = sym_size[obj_arr_sym[e]];
                    if ((long)obj_arr_val[e] <= roff &&
                        roff < (long)obj_arr_val[e] + sz) {
                        as = obj_arr_sym[e];
                        aoff = (int)(roff - obj_arr_val[e]);
                    }
                }
                if (as < 0)
                    return fail("配列再配置の受け口がありません");
                if (nafix >= 1024)
                    return fail("関数ポインタの初期値が多すぎます");
                afix_arr[nafix] = as;
                afix_off[nafix] = aoff;
                afix_func[nafix] = esym_sym[rsym];
                nafix++;
                continue;
            }
            if (is_data) {
                /* 文字列か関数へのポインタ初期値
                 * (R_RISCV_64) */
                if (rtype != 2)
                    return fail("知らないデータ再配置があります");
                int is_str = (esym_tag[rsym] == 1);
                int fsym = -1;
                if (!is_str && rsym >= 0 && esym_sym[rsym] >= 0 &&
                    sym_kind[esym_sym[rsym]] == 1)
                    fsym = esym_sym[rsym];
                if (!is_str && fsym < 0)
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
                if (is_str) {
                    sym_init_kind[vs] = 2;
                    sym_init_val[vs] = radd;
                    sym_init_obj[vs] = obj;
                } else {
                    /* 関数へのポインタ (v44): 種類3
                     * (名前) で、相手は関数 */
                    sym_init_kind[vs] = 3;
                    sym_init_sym[vs] = fsym;
                    sym_init_val[vs] = radd;
                }
                edata_hit[hit] = 1;
                continue;
            }
            if (rtype == 17) {
                /* R_RISCV_JAL: 関数呼び出し。パスA では
                 * 検証と印だけ (穴埋めはパスB) */
                if (esym_tag[rsym] != 0 && esym_tag[rsym] != 4)
                    return fail("呼び出しの相手が違います");
                if (!off_covered(obj, roff))
                    return fail("穴の場所が決まりません");
                if (esym_tag[rsym] == 0)
                    sym_called[esym_sym[rsym]] = 1;
            } else if (rtype == 23) {
                /* R_RISCV_PCREL_HI20: 住所作りの組の頭。
                 * 組は2語なので、後半の収まりも見る */
                if (esym_tag[rsym] != 1 &&
                    esym_tag[rsym] != 0 && esym_tag[rsym] != 4)
                    return fail("住所作りの相手が違います");
                if (!off_covered(obj, roff) ||
                    !off_covered(obj, roff + 4))
                    return fail("穴の場所が決まりません");
                if (esym_tag[rsym] == 0 ||
                    esym_tag[rsym] == 4)
                    sym_addref[esym_sym[rsym]] = 1;
            } else if (rtype == 26) {
                /* R_RISCV_HI20: 絶対アドレスの組の頭 */
                int found = 0;
                for (int e = 0; e < neabs; e++) {
                    if (eabs_idx[e] == (int)rsym)
                        found = 1;
                }
                if (!found)
                    return fail("絶対アドレスが見つかりません");
                if (!off_covered(obj, roff) ||
                    !off_covered(obj, roff + 4))
                    return fail("穴の場所が決まりません");
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

/* ファイルを share_buf に読み込む (in_len に長さ) */
static int slurp(char *path) {
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
    return 1;
}

/* オブジェクト1つを読む (番号 obj、パスA) */
static int read_object(int obj, char *path) {
    if (!slurp(path))
        return 0;
    obj_file_len[obj] = in_len;
    obj_entry[obj] = -1;
    obj_str_len[obj] = 0;
    /* 読めるのは ELF オブジェクトだけ (v42 で .iwo の
     * 読み口は引退。決定の記録は doc/iwo-retirement.md) */
    if (in_len >= 4 && eget8(0) == 0x7f && in_buf[1] == 'E' &&
        in_buf[2] == 'L' && in_buf[3] == 'F')
        return read_object_elf(obj);
    return fail("ELF オブジェクトではありません");
}

/* パスB 用に読み直す。パスA のときと長さが
 * 同じか照合する (途中で変わっていたら止める) */
static int load_file(int obj) {
    if (!slurp(obj_path[obj]))
        return 0;
    if (in_len != obj_file_len[obj])
        return fail("オブジェクトが変わっています");
    return 1;
}

/* ================= 配置 ================= */
static int entry_obj;
static int out_len;            /* 最終コードの長さ */
static long str_total;
static long arr_base;          /* 配列領域の基底
                                * (str_total を 8 に
                                * 切り上げたもの、v63) */
static int big_link;           /* コードが JAL の届く
                                * 範囲 (約1MB) を超えたか。
                                * 超えたリンクだけ、呼び出しの
                                * 届きをパスA2 で点検する */

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
    /* コード: 入口の断片を先頭に、本体は登録順に。
     * ここでは位置決めだけ (実体はパスB が流す) */
    out_len = obj_entry[entry_obj];
    for (int i = 0; i < nseq; i++) {
        int s = seq_sym[i];
        if (sym_kind[s] != 1 || sym_obj[s] < 0 || sym_place[s] >= 0)
            continue;
        sym_place[s] = out_len;
        sym_addr[s] = CODE_BASE + out_len;  /* 0x40000000 */
        out_len += sym_size[s];
    }
    big_link = out_len > 1048572;
    /* 文字列: オブジェクトの並び順に */
    long spos = 0;
    for (int o = 0; o < nobj; o++) {
        obj_str_final[o] = (int)spos;
        spos += obj_str_len[o];
    }
    str_total = spos;
    /* 配列領域の基底と締めを 8 バイト境界に揃える
     * (v63)。文字列自身の住所は従来どおりで、
     * ずれるのは後ろの領域だけ */
    arr_base = (str_total + 7) & ~7L;
    arr_area = (arr_area + 7) & ~7L;
    /* データの住所 */
    for (int s = 0; s < nsym; s++) {
        if (sym_kind[s] == 3)
            sym_addr[s] = DATA_BASE + arr_base + sym_off[s];
        /*                              0x40200000 */
        else if (sym_kind[s] == 2)
            sym_addr[s] = DATA_BASE + arr_base + arr_area +
                          (long)sym_off[s] * 8;
    }
    /* 関数ポインタ配列の初期値 (v44): 関数の住所が
     * 決まったので、ainit の像に書く。書く場所は
     * 先に確保する (末尾 0 の切り捨てで消えた分も
     * ここで戻る) */
    for (int i = 0; i < nafix; i++) {
        if (!ainit_reserve(afix_arr[i], (long)afix_off[i] + 8))
            return 0;
    }
    for (int i = 0; i < nafix; i++) {
        long v = sym_addr[afix_func[i]];
        int p = sym_ainit_off[afix_arr[i]] + afix_off[i];
        for (int b = 0; b < 8; b++) {
            ainit_buf[p + b] = (char)(v & 0xff);
            v = v >> 8;
        }
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

/* ================= 参照の点検 (配置の後) =================
 * パスA で集めた印 (sym_called / sym_addref) を
 * 最終的な名前表で点検する。穴埋め (パスB) が
 * 失敗しないための門番 */
static int check_refs(void) {
    for (int s = 0; s < nsym; s++) {
        if (sym_called[s] && sym_obj[s] < 0) {
            err_sym = s;
            return fail("未定義の関数が呼ばれています");
        }
        if (sym_addref[s] && sym_kind[s] == 0) {
            err_sym = s;
            return fail("定義のない名前が使われています");
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

/* ================= パスB の準備 =================
 * 読み直したオブジェクトについて、穴埋めに要る
 * 控え (esym の鏡と絶対アドレスの控え) を
 * 作り直す。パスA のシンボル表の走査のうち、
 * 再配置の解釈に要る部分だけの再現 */
static long pb_text_off;       /* .text のファイル内位置 */

static int passb_prepare(int obj) {
    if (eget8(4) != 2 || eget8(5) != 1)
        return fail("ELF の形が違います (64ビット LE のみ)");
    long shoff = eget64(40);
    int shnum = eget16(60);
    int shstrndx = eget16(62);
    eget_shdr(shoff, shstrndx);
    long shstr_off = esec_off;
    int text_i = -1;
    int symtab_i = -1;
    for (int i = 0; i < shnum; i++) {
        char nm[32];
        esec_name(shoff, shstr_off, i, nm);
        if (ds_streq(nm, ".text"))
            text_i = i;
        eget_shdr(shoff, i);
        if (esec_type == 2)
            symtab_i = i;
    }
    if (text_i < 0 || symtab_i < 0)
        return fail("ELF に .text / .symtab がありません");
    eget_shdr(shoff, text_i);
    pb_text_off = esec_off;
    if (pb_text_off != obj_text_off[obj])
        return fail("オブジェクトが変わっています");
    eget_shdr(shoff, symtab_i);
    long symtab_off = esec_off;
    long nsym_e = esec_size / 24;
    int strtab_i = (int)esec_link;
    eget_shdr(shoff, strtab_i);
    long strtab_off = esec_off;
    if (nsym_e > 8192)
        return fail("シンボルが多すぎます");
    neabs = 0;
    for (long si = 0; si < nsym_e; si++) {
        long b = symtab_off + si * 24;
        long name_off = eget32(b);
        int info = eget8(b + 4);
        int bind = info >> 4;
        int type = info & 15;
        int shndx = eget16(b + 6);
        long value = eget64(b + 8);
        esym_tag[si] = 3;
        esym_sym[si] = -1;
        if (type == 3) {
            if (shndx >= 0 && shndx < shnum) {
                char snm[32];
                esec_name(shoff, shstr_off, shndx, snm);
                if (ds_streq(snm, ".iwac.strings"))
                    esym_tag[si] = 1;
            }
            continue;
        }
        if (shndx == 0xfff1) {
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
        int s = find_sym(gnm);
        if (s < 0)
            continue;   /* iwac_entry など名前表に無いもの */
        esym_sym[si] = s;
        esym_tag[si] = bind == 2 ? 4 : 0;
    }
    return 1;
}

/* ================= 穴埋め (パスB) =================
 * get32 / put32 は in_buf のファイル内位置で触る
 * (読み直したオブジェクトの .text の中) */
static long get32(long foff) {
    long b0 = in_buf[foff] & 0xff;
    long b1 = in_buf[foff + 1] & 0xff;
    long b2 = in_buf[foff + 2] & 0xff;
    long b3 = in_buf[foff + 3] & 0xff;
    return b0 + b1 * 256 + b2 * 65536 + b3 * 16777216;
}

static void put32(long foff, long v) {
    in_buf[foff] = (char)(v & 0xff);
    in_buf[foff + 1] = (char)((v >> 8) & 0xff);
    in_buf[foff + 2] = (char)((v >> 16) & 0xff);
    in_buf[foff + 3] = (char)((v >> 24) & 0xff);
}

/* 穴1つを埋める (obj の .text 内オフセット off、
 * 最終位置 place、種類 kind、相手 s、加算値 val)。
 * 埋め方は旧 fill_all と同じ */
static int patch_one(int obj, long off, int place, int kind,
                     int s, long val) {
    long pc = CODE_BASE + place;
    long foff = pb_text_off + off;
    if (kind == 1 || kind == 2) {
        if (sym_obj[s] < 0) {
            if (kind == 1) {
                err_sym = s;
                return fail("未定義の関数が呼ばれています");
            }
            put32(foff, 0x13);  /* nop (addi x0, x0, 0) */
            return 1;
        }
        long diff = sym_addr[s] - pc;
        if (diff < -1048576L || diff > 1048572L)
            return fail("呼び出しが届きません (約1MBまで)");
        long w = get32(foff) & 0xFFF;
        w |= ((diff >> 20) & 1) << 31;
        w |= ((diff >> 1) & 0x3FF) << 21;
        w |= ((diff >> 11) & 1) << 20;
        w |= ((diff >> 12) & 0xFF) << 12;
        put32(foff, w);
        return 1;
    }
    long target;
    if (kind == 3)
        target = sym_addr[s] + val;
    else if (kind == 4)
        target = DATA_BASE + obj_str_final[obj] + val;
    else
        target = val;
    long diff = target - pc;
    long hi = (diff + 0x800) >> 12;
    long lo = diff - (hi << 12);
    long w0 = get32(foff);
    w0 = (w0 & 0xFFFFF) | ((hi & 0xFFFFF) << 12);
    put32(foff, w0);
    long w1 = get32(foff + 4);
    w1 = (w1 & 0xFFFFF) | ((lo & 0xFFF) << 20);
    put32(foff + 4, w1);
    return 1;
}

/* 読み直したオブジェクトの .text の穴を全部
 * 埋める。再配置の読み方はパスA と同じで、
 * .text 向けだけをその場で埋める */
static int patch_object(int obj) {
    long shoff = eget64(40);
    int shnum = eget16(60);
    int shstrndx = eget16(62);
    eget_shdr(shoff, shstrndx);
    long shstr_off = esec_off;
    for (int i = 0; i < shnum; i++) {
        eget_shdr(shoff, i);
        if (esec_type != 4)
            continue;
        int target_i = (int)esec_info;
        char tnm[32];
        esec_name(shoff, shstr_off, target_i, tnm);
        if (!ds_streq(tnm, ".text"))
            continue;
        long cnt = esec_size / 24;
        for (long ri = 0; ri < cnt; ri++) {
            long rb = esec_off + ri * 24;
            long roff = eget64(rb);
            long rinfo = eget64(rb + 8);
            long rsym = rinfo / (1L << 32);
            long rtype = rinfo % (1L << 32);
            long radd = eget64(rb + 16);
            if (rtype == 17) {
                int place = site_final(obj, (int)roff);
                if (place < 0)
                    return fail("穴の場所が決まりません");
                int kind = esym_tag[rsym] == 4 ? 2 : 1;
                if (!patch_one(obj, roff, place, kind,
                               esym_sym[rsym], 0))
                    return 0;
            } else if (rtype == 23) {
                int place = site_final(obj, (int)roff);
                if (place < 0)
                    return fail("穴の場所が決まりません");
                if (esym_tag[rsym] == 1) {
                    if (!patch_one(obj, roff, place, 4, -1,
                                   radd))
                        return 0;
                } else {
                    if (!patch_one(obj, roff, place, 3,
                                   esym_sym[rsym], radd))
                        return 0;
                }
            } else if (rtype == 26) {
                int place = site_final(obj, (int)roff);
                if (place < 0)
                    return fail("穴の場所が決まりません");
                long val = 0;
                for (int e = 0; e < neabs; e++)
                    if (eabs_idx[e] == (int)rsym)
                        val = eabs_val[e];
                if (!patch_one(obj, roff, place, 5, -1, val))
                    return 0;
            }
            /* 24 / 27 / 51 は組の後半または印 */
        }
    }
    return 1;
}

/* 大きなリンク (big_link) だけの事前点検:
 * 呼び出しの届きは場所ごとに違うので、全
 * オブジェクトを読み直して穴埋めを試し、
 * 結果は捨てる (本番は emit が読み直す)。
 * 出力はまだ何も書いていない */
static int range_validate(void) {
    for (int o = 0; o < nobj; o++) {
        if (!load_file(o) || !passb_prepare(o) ||
            !patch_object(o))
            return 0;
    }
    return 1;
}

/* ================= コードを流す (パスB) ================= */
static int loaded_obj;         /* share_buf に穴埋め済みで
                                * 入っているオブジェクト。
                                * -1 = 無し */

/* obj を読み直して穴を埋め、loaded_obj にする */
static int ensure_obj(int obj) {
    if (loaded_obj == obj)
        return 1;
    if (!load_file(obj) || !passb_prepare(obj) ||
        !patch_object(obj))
        return 0;
    loaded_obj = obj;
    return 1;
}

/* 入口 → seq 順に関数を流す。書く位置が配置と
 * 一致するか、走りながら照合する (配置の順と
 * seq の初出の順は一致する) */
static int emit_code(void) {
    loaded_obj = -1;
    if (!ensure_obj(entry_obj))
        return 0;
    write(1, in_buf + obj_text_off[entry_obj],
          obj_entry[entry_obj]);
    int running = obj_entry[entry_obj];
    for (int i = 0; i < nseq; i++) {
        int s = seq_sym[i];
        if (sym_kind[s] != 1 || sym_obj[s] < 0 ||
            sym_place[s] != running)
            continue;
        if (!ensure_obj(sym_obj[s]))
            return 0;
        write(1, in_buf + obj_text_off[sym_obj[s]] + sym_off[s],
              sym_size[s]);
        running += sym_size[s];
    }
    if (running != out_len)
        return fail("配置がずれました");
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

static int write_elf(void) {
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
    out_phdr(6, data_off, data_base, arr_base + ainit_span,
             arr_base + arr_area, 0x1000);
    out_phdr(6, data_off + arr_base + ainit_span,
             data_base + arr_base + arr_area,
             has_inits ? gbl_bytes : 0, gbl_bytes, 1);
    for (int i = 232; i < 4096; i++)
        out_byte(0);
    if (!emit_code())
        return 0;
    for (long i = 4096L + out_len; i < data_off; i++)
        out_byte(0);
    if (str_total > 0) {
        /* 文字列: オブジェクトの並び順に、節を
         * 読み直してそのまま流す (並びは旧
         * str_pool の中身と一致する) */
        for (int o = 0; o < nobj; o++) {
            if (obj_str_len[o] <= 0)
                continue;
            if (!load_file(o))
                return 0;
            write(1, in_buf + obj_str_foff[o], obj_str_len[o]);
        }
    }
    /* 配列領域の基底までの詰め物 (v63)。後の像は
     * 全部 arr_base 基準なので、初期値が無い
     * ときも含めて必ず出す */
    for (long i = str_total; i < arr_base; i++)
        out_byte(0);
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
    return 1;
}

int main(int argc, char **argv) {
    int base;
    in_buf = share_buf;
    base = 1;
    /* nori --out <名前> obj... : OS 上での出力名 (v34) */
    if (argc >= 4 && ds_streq(argv[1], "--out")) {
        kosaba_out_name = argv[2];
        base = 3;
    }
    if (argc - base < 1) {
        ds_puts(2, "usage: nori [--out <name>] <obj.eo> ... > prog.elf\n");
        return 1;
    }
    if (argc - base > MAX_OBJ) {
        ds_puts(2, "nori: オブジェクトが多すぎます\n");
        return 1;
    }
    err_sym = -1;
    nobj = argc - base;
    for (int i = 0; i < nobj; i++) {
        obj_path[i] = argv[i + base];
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
    if (!layout() || !check_refs() ||
        (big_link && !range_validate()) || !write_elf()) {
        ds_puts(2, "nori: ");
        ds_puts(2, errmsg);
        if (err_sym >= 0) {
            ds_puts(2, ": ");
            ds_puts(2, sym_str(err_sym));
        }
        ds_puts(2, "\n");
        return 1;
    }
    return 0;
}
