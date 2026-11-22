/* ================================================================
 * 前処理 (v13 で #define、v18 で条件付きコンパイル)
 *
 * 行頭 (空白の後ろでもよい) の「#define 名前 置き換え」の行を
 * 読み取り、以降の文に出てくる「名前」を単語単位で置き換えた
 * 新しいソースを作る。parse は展開後のソースだけを見る。
 * 決め事 (計画で「実装するときに決める」と置いた点):
 *   - 置き換えは単語 (識別子) 単位。文字列の中と // コメントの
 *     中は置き換えない (C と同じ)
 *   - 置き換えた結果はもう一度走査しない (入れ子は展開しない)
 *   - 再定義はエラー。define は最大64個 (v50 で 16 から緩和)、
 *   置き換えは127文字まで
 *   - # で始まる行は #define / #ifdef / #ifndef / #else /
 *     #endif と #include (v51) だけ。それ以外はエラー
 * 条件付きコンパイル (v18):
 *   - #ifdef 名前 / #ifndef 名前 / #else / #endif。入れ子は
 *     32段まで。無効になった範囲の行は出力されず、その中の
 *     #define も登録されない (条件の指令だけは数える)
 *   - コンパイラが用意する定義済みマクロ: __IWAC__ は常に、
 *     __KOSABA__ は --kosaba のときに、どちらも 1 として
 *     定義済み。ターゲットでソースを切り替えるのに使う
 * #include (v51、設計は doc/include.md):
 *   - 行ループが #include に当たったら、ファイルを
 *     読んで同じ行ループを再帰で回し、出力をその場へ
 *     差し込む。#define の表は大域に1つなので境界を
 *     越えて共有され、#ifndef の guard がそのまま効く
 *   - 解決: "..." は取り込む側のファイルのディレクトリ
 *     を先に探し、無ければカレント。<...> はカレント
 *     のみ。入れ子は深さ8まで。取り込んだテキストの
 *     置き場 (inc_arena) は入れ子の深さぶん重ねて使う
 *   - ファイルの中で #ifdef の対応が閉じていない、
 *     ブロックコメントが閉じていない場合はエラー
 * なお iwac 自身のソースは今もプリプロセッサ無しのままで、
 * これは「コンパイルされる側のプログラム」のための機能。
 * ================================================================ */

static char def_names[66][32];  /* 利用者分 64 + 定義済み 2 */
static int def_name_len[66];
static char def_text[66][128];
static int def_text_len[66];
static int ndef;
static int ndef_base;  /* 定義済みマクロの分 (上限の数え方から外す) */

static char exp_buf[475136];
static int exp_len;
static int exp_failed;

/* 名前 (場所と長さで渡す) の define があればその番号、無ければ -1 */
static int find_define(char *name, int len) {
    for (int i = 0; i < ndef; i++) {
        if (def_name_len[i] != len)
            continue;
        int j = 0;
        while (j < len && def_names[i][j] == name[j])
            j++;
        if (j == len)
            return i;
    }
    return -1;
}

/* define を1つ登録する。重なり・上限・長さのエラーは
 * errmsg を置いて 0 を返す */
static int add_define(char *name, int nlen, char *text, int tlen) {
    if (nlen > 31) {
        errmsg = "#define の名前が長すぎます";
        return 0;
    }
    if (find_define(name, nlen) >= 0) {
        errmsg = "#define の名前が重なっています";
        return 0;
    }
    if (ndef - ndef_base >= 64) {
        errmsg = "#define は64個までです";
        return 0;
    }
    if (tlen > 127) {
        errmsg = "#define の置き換えが長すぎます";
        return 0;
    }
    int d = ndef++;
    for (int k = 0; k < nlen; k++)
        def_names[d][k] = name[k];
    def_name_len[d] = nlen;
    for (int k = 0; k < tlen; k++)
        def_text[d][k] = text[k];
    def_text_len[d] = tlen;
    return 1;
}

/* src の位置 p の単語が kw (長さ kwlen) と同じか。
 * 後ろに識別子の文字が続いていたら別の単語として扱う */
static int kw_eq(char *src, int p, char *kw, int kwlen) {
    for (int k = 0; k < kwlen; k++)
        if (src[p + k] != kw[k])
            return 0;
    return !is_ident_char(src[p + kwlen]);
}

/* 条件付きコンパイル (v18) の状態。段ごとに「いまの枝が
 * 有効か」「有効な枝が既に来たか」「#else を見たか」を持つ */
static int cond_on[32];
static int cond_taken[32];
static int cond_else[32];
static int cond_n;

/* 全段を通して、いま有効な範囲の中にいるか */
static int cond_active(void) {
    return cond_n == 0 || cond_on[cond_n - 1];
}

/* 展開先へ1文字書く (あふれたら印だけ付ける) */
static void exp_ch(char c) {
    if (exp_len >= 475135) {
        exp_failed = 1;
        return;
    }
    exp_buf[exp_len++] = c;
}

static void exp_str(char *s, int len) {
    for (int i = 0; i < len; i++)
        exp_ch(s[i]);
}

/* #include (v51) の置き場と作業領域。置き場は入れ子の
 * 深さぶん重ねて使い (inc_used が積み位置)、兄弟の
 * 取り込みは同じ場所を使い回す */
static char inc_arena[131072];
static int inc_used;
static char inc_path[256];
static char inc_cand[512];

static int expand_text(char *src, char *dir, int depth);

/* #include の実処理: inc_path のファイルを読んで、
 * その場へ展開する。dir は取り込む側のファイルの
 * ディレクトリ (末尾に '/' を含む。無ければ空)。
 * angled は <...> の形だったか。成功したら 1 */
static int do_include(char *dir, int depth, int angled) {
    if (depth >= 8) {
        errmsg = "#include の入れ子が深すぎます";
        return 0;
    }
    int mark = inc_used;
    int cap = (int)sizeof(inc_arena) - mark;
    int len = -1;
    char *used_path = inc_path;
    if (!angled && dir[0] != '\0') {
        /* "..." は取り込む側のディレクトリを先に探す */
        int k = 0;
        while (dir[k] != '\0' && k < 500) {
            inc_cand[k] = dir[k];
            k++;
        }
        int m = 0;
        while (inc_path[m] != '\0' && k < 510) {
            inc_cand[k] = inc_path[m];
            k++;
            m++;
        }
        inc_cand[k] = '\0';
        len = load_include(inc_cand, inc_arena + mark, cap);
        if (len >= 0)
            used_path = inc_cand;
    }
    if (len == -2) {
        errmsg = "#include のファイルが大きすぎます";
        return 0;
    }
    if (len < 0) {
        len = load_include(inc_path, inc_arena + mark, cap);
        if (len == -2) {
            errmsg = "#include のファイルが大きすぎます";
            return 0;
        }
        if (len < 0) {
            errmsg = "#include のファイルが開けません";
            return 0;
        }
        used_path = inc_path;
    }
    /* 取り込めたパスのディレクトリを子の基準にする */
    char child_dir[512];
    int last = -1;
    int k = 0;
    while (used_path[k] != '\0') {
        if (used_path[k] == '/')
            last = k;
        k++;
    }
    if (last < 0) {
        child_dir[0] = '\0';
    } else {
        int m = 0;
        while (m <= last && m < 511) {
            child_dir[m] = used_path[m];
            m++;
        }
        child_dir[m] = '\0';
    }
    inc_used = mark + len + 1;
    int ok = expand_text(inc_arena + mark, child_dir, depth + 1);
    inc_used = mark;
    return ok;
}

/* 前処理の本体: 1つのテキストを走査し、#define の展開と
 * #ifdef 等の枝選びをしながら exp_buf へ書く。#include は
 * do_include 経由でこの関数を再帰に呼ぶ。成功したら 1 */
static int expand_text(char *src, char *dir, int depth) {
    /* このファイルに入った時点の条件の段数。ファイルの
     * 中で開いた #ifdef は、そのファイルの中で閉じる
     * こと (チェックは関数の終わりで) */
    int cond_base = cond_n;
    int i = 0;
    int sol = 1;  /* 行頭 (その行にまだ空白しか無い) か */
    int in_comment = 0;  /* ブロックコメントの中か (v18) */
    while (src[i] != '\0') {
        if (sol && !in_comment) {
            /* 行頭の空白の先が # なら指令行として処理する */
            int j = i;
            while (src[j] == ' ' || src[j] == '\t')
                j++;
            if (src[j] == '#') {
                j++;
                while (src[j] == ' ' || src[j] == '\t')
                    j++;
                if (kw_eq(src, j, "ifdef", 5) ||
                    kw_eq(src, j, "ifndef", 6)) {
                    int negate = kw_eq(src, j, "ifndef", 6);
                    j += negate ? 6 : 5;
                    while (src[j] == ' ' || src[j] == '\t')
                        j++;
                    if (!is_ident_start(src[j])) {
                        errmsg = "#ifdef には名前が必要です";
                        return 0;
                    }
                    int ns = j;
                    while (is_ident_char(src[j]))
                        j++;
                    int on = find_define(src + ns, j - ns) >= 0;
                    if (negate)
                        on = !on;
                    on = cond_active() && on;
                    if (cond_n >= 32) {
                        errmsg = "#ifdef の入れ子が深すぎます";
                        return 0;
                    }
                    cond_on[cond_n] = on;
                    cond_taken[cond_n] = on;
                    cond_else[cond_n] = 0;
                    cond_n++;
                } else if (kw_eq(src, j, "else", 4)) {
                    if (cond_n == 0) {
                        errmsg = "#else に対応する #ifdef がありません";
                        return 0;
                    }
                    if (cond_else[cond_n - 1]) {
                        errmsg = "#else が重なっています";
                        return 0;
                    }
                    cond_else[cond_n - 1] = 1;
                    int parent = cond_n >= 2 ? cond_on[cond_n - 2] : 1;
                    cond_on[cond_n - 1] =
                        parent && !cond_taken[cond_n - 1];
                    cond_taken[cond_n - 1] = 1;
                } else if (kw_eq(src, j, "endif", 5)) {
                    if (cond_n == 0) {
                        errmsg = "#endif に対応する #ifdef がありません";
                        return 0;
                    }
                    cond_n--;
                } else if (kw_eq(src, j, "define", 6)) {
                    /* 有効な範囲の中の #define だけを登録する */
                    if (cond_active()) {
                        j += 6;
                        while (src[j] == ' ' || src[j] == '\t')
                            j++;
                        if (!is_ident_start(src[j])) {
                            errmsg = "#define には名前が必要です";
                            return 0;
                        }
                        int ns = j;
                        while (is_ident_char(src[j]))
                            j++;
                        int nlen = j - ns;
                        while (src[j] == ' ' || src[j] == '\t')
                            j++;
                        int ts = j;
                        while (src[j] != '\0' && src[j] != '\n')
                            j++;
                        int te = j;
                        while (te > ts && (src[te - 1] == ' ' ||
                                           src[te - 1] == '\t' ||
                                           src[te - 1] == '\r'))
                            te--;
                        if (!add_define(src + ns, nlen,
                                        src + ts, te - ts))
                            return 0;
                    }
                } else if (kw_eq(src, j, "include", 7)) {
                    /* 有効な範囲の中の #include だけが
                     * ファイルを読む (v51) */
                    if (cond_active()) {
                        j += 7;
                        while (src[j] == ' ' || src[j] == '\t')
                            j++;
                        int angled = 0;
                        if (src[j] == '<') {
                            angled = 1;
                        } else if (src[j] != '"') {
                            errmsg = "#include には \"...\" か <...> でパスを書きます";
                            return 0;
                        }
                        j++;
                        int ps = j;
                        char term = angled ? '>' : '"';
                        while (src[j] != '\0' && src[j] != '\n' &&
                               src[j] != term)
                            j++;
                        if (src[j] != term) {
                            errmsg = "#include のパスが閉じていません";
                            return 0;
                        }
                        int plen = j - ps;
                        if (plen == 0 || plen >= 256) {
                            errmsg = "#include のパスが長すぎます";
                            return 0;
                        }
                        for (int k = 0; k < plen; k++)
                            inc_path[k] = src[ps + k];
                        inc_path[plen] = '\0';
                        if (!do_include(dir, depth, angled))
                            return 0;
                    }
                } else if (cond_active()) {
                    errmsg = "# で始まる行は #define / #ifdef / #ifndef / #else / #endif / #include だけが使えます";
                    return 0;
                }
                /* 指令行そのものは出力しない。行末まで進み、
                 * 改行だけ残す (行の区切りを保つため)。
                 * 無効な範囲での知らない指令は読み飛ばす */
                while (src[j] != '\0' && src[j] != '\n')
                    j++;
                i = j;
                if (src[i] == '\n') {
                    exp_ch('\n');
                    i++;
                }
                sol = 1;
                continue;
            }
        }
        if (!cond_active()) {
            /* 無効な範囲: 行末まで読み飛ばす。ブロックコメントの
             * 中にある行頭は指令に見せない (状態だけ追う) */
            while (src[i] != '\0' && src[i] != '\n') {
                if (in_comment) {
                    if (src[i] == '*' && src[i + 1] == '/') {
                        in_comment = 0;
                        i += 2;
                    } else {
                        i++;
                    }
                } else if (src[i] == '/' && src[i + 1] == '*') {
                    in_comment = 1;
                    i += 2;
                } else if (src[i] == '/' && src[i + 1] == '/') {
                    break;
                } else if (src[i] == '"' || src[i] == '\'') {
                    /* リテラルの中はコメントの始まりを
                     * 探さない (v27、アクティブ側と同じ) */
                    int q = src[i];
                    i++;
                    while (src[i] != '\0' && src[i] != q &&
                           src[i] != '\n') {
                        if (src[i] == '\\' && src[i + 1] != '\0')
                            i++;
                        i++;
                    }
                    if (src[i] == q)
                        i++;
                } else {
                    i++;
                }
            }
            while (src[i] != '\0' && src[i] != '\n')
                i++;
            if (src[i] == '\n') {
                exp_ch('\n');
                i++;
            }
            sol = 1;
            continue;
        }
        if (in_comment) {
            /* ブロックコメントの中: そのまま写すだけ。
             * 行頭に # があっても指令として扱わない */
            if (src[i] == '*' && src[i + 1] == '/') {
                exp_ch('*');
                exp_ch('/');
                i += 2;
                in_comment = 0;
                sol = 0;
                continue;
            }
            exp_ch(src[i]);
            if (src[i] == '\n')
                sol = 1;
            else if (src[i] != ' ' && src[i] != '\t' &&
                     src[i] != '\r')
                sol = 0;
            i++;
            continue;
        }
        /* 通常の文字: 文字列とコメントを守りながら写す */
        char c = src[i];
        if (c == '"') {
            /* 文字列の中は置き換えない */
            exp_ch(c);
            i++;
            while (src[i] != '\0' && src[i] != '"') {
                if (src[i] == '\\' && src[i + 1] != '\0') {
                    exp_ch(src[i]);
                    i++;
                }
                exp_ch(src[i]);
                i++;
            }
            if (src[i] == '"') {
                exp_ch('"');
                i++;
            }
            sol = 0;
            continue;
        }
        if (c == '\'') {
            /* 文字リテラル (v27): 文字列と同じく中身を
             * 守る。これが無いと '"' (ダブルクォートの
             * 文字リテラル) の " で「文字列が始まった」
             * と誤認し、その先のコメント判定や行頭の
             * 状態が壊れる。iwac 自身のソース (字句解析
             * の文字比較) を前処理して初めて発覚した */
            exp_ch(c);
            i++;
            while (src[i] != '\0' && src[i] != '\'') {
                if (src[i] == '\\' && src[i + 1] != '\0') {
                    exp_ch(src[i]);
                    i++;
                }
                exp_ch(src[i]);
                i++;
            }
            if (src[i] == '\'') {
                exp_ch('\'');
                i++;
            }
            sol = 0;
            continue;
        }
        if (c == '/' && src[i + 1] == '/') {
            /* コメントの中も置き換えない */
            while (src[i] != '\0' && src[i] != '\n') {
                exp_ch(src[i]);
                i++;
            }
            sol = 0;
            continue;
        }
        if (c == '/' && src[i + 1] == '*') {
            /* ブロックコメントの始まり: 中身はそのまま写す。
             * 中の行頭の # を指令と間違えないよう印を付ける */
            exp_ch('/');
            exp_ch('*');
            i += 2;
            in_comment = 1;
            sol = 0;
            continue;
        }
        if (is_ident_start(c)) {
            /* 単語: define 済みなら置き換える */
            int s = i;
            while (is_ident_char(src[i]))
                i++;
            int len = i - s;
            int d = find_define(src + s, len);
            if (d >= 0)
                exp_str(def_text[d], def_text_len[d]);
            else
                exp_str(src + s, len);
            sol = 0;
            continue;
        }
        exp_ch(c);
        i++;
        if (c == '\n')
            sol = 1;
        else if (c != ' ' && c != '\t' && c != '\r')
            sol = 0;
    }
    if (cond_n != cond_base) {
        if (depth == 0)
            errmsg = "#endif が足りません";
        else
            errmsg = "#ifdef / #endif の対応がファイルの中で閉じていません";
        return 0;
    }
    if (depth > 0 && in_comment) {
        errmsg = "ブロックコメントが閉じていません";
        return 0;
    }
    return 1;
}

/* 前処理の入口: 状態を用意し、定義済みマクロを登録
 * してから本体 (expand_text) を回す。dir は最初の
 * ファイルのあるディレクトリ (#include の解決用) */
static int expand_defines(char *src, char *dir) {
    ndef = 0;
    exp_len = 0;
    exp_failed = 0;
    cond_n = 0;
    inc_used = 0;
    /* 定義済みマクロ (v18): __IWAC__ は常に、__KOSABA__ は
     * KosabaOS ターゲットのときに、1 として用意する */
    if (!add_define("__IWAC__", 8, "1", 1))
        return 0;
    if (target_kosaba && !add_define("__KOSABA__", 10, "1", 1))
        return 0;
    /* 定義済みマクロは上限 (利用者分 64) の数に入れない */
    ndef_base = ndef;
    if (!expand_text(src, dir, 0))
        return 0;
    if (exp_failed) {
        errmsg = "展開後のソースが大きすぎます";
        return 0;
    }
    exp_buf[exp_len] = '\0';
    return 1;
}

