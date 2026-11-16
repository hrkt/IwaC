/* ================================================================
 * 第1層 parse (宣言まわり): 文の並び・typedef・構造体定義・宣言文
 *
 * 02-parse.c から分かれた断片 (v37)。文の並びを読む
 * parse_stmt_list、typedef (v36)、構造体の定義 (v37)、
 * 宣言文 parse_decl_stmt、定数式の評価 eval_const を置く。
 * 分けた理由は容量: 02-parse.c が大きくなり、断片
 * プロファイル (1MB) のソースバッファに TU
 * (decls.c + 断片) が収まらなくなったため。連結順は
 * 02-parse.c の直後で、decls.c の宣言順もそれに合わせる。
 * ================================================================ */

/* 文の並びを読む。in_brace が 1 のときは while の本体で、
 * '}' まで読んで '}' を消費する。0 のときはプログラムの
 * 頭から、最後の式か入力の終わりまで読む。
 * 読めたら先頭の文ノード (文が無ければ -1)、
 * 失敗したら -2 を返す (エラー理由は errmsg)。
 * プログラムの頭 (in_brace=0) では、最後の式に出会ったら
 * prog_final / prog_has_final に置いて終わる */
static int parse_stmt_list(int in_brace) {
    int head = -1;
    int tail = -1;
    for (;;) {
        skip_spaces();
        if (in_brace) {
            if (*cur == '}') {
                cur++;
                return head;
            }
            if (*cur == '\0') {
                errmsg = "文の並びが } で閉じていません";
                return -2;
            }
        } else {
            if (*cur == '\0')
                return head;  /* 文だけのプログラムもよい */
        }
        /* 裸のブロック (v25): parse_stmt が本体を読む。
         * 並びは複数ノードの鎖で返るので、末尾も進める */
        if (*cur == '{') {
            int stmt = parse_stmt();
            if (stmt < 0)
                return -2;
            if (head < 0)
                head = stmt;
            else
                node_next[tail] = stmt;
            tail = stmt;
            while (node_next[tail] >= 0)
                tail = node_next[tail];
            continue;
        }
        /* '*' で始まる場合の先読み: 相手の後ろに '=' が
         * あればメモリ書き込み文、無ければ * で始まる式
         * (プログラムの最後の式) である。相手は変数か数
         * だけを試す (副作用の無い読み方しかしない) */
        if (*cur == '*') {
            char *save_star = cur;
            cur++;
            skip_spaces();
            int is_store = 0;
            if (is_digit(*cur) || is_ident_start(*cur)) {
                int operand = parse_primary_node();
                if (operand >= 0) {
                    skip_spaces();
                    if (*cur == '=' && cur[1] != '=')
                        is_store = 1;
                }
            }
            cur = save_star;
            if (is_store) {
                int stmt = parse_stmt();
                if (stmt < 0)
                    return -2;
                if (head < 0)
                    head = stmt;
                else
                    node_next[tail] = stmt;
                tail = stmt;
                continue;
            }
            /* 式だった。先読みを巻き戻して下へ進む */
        }
        /* ++ / -- で始まる文 (前置の増減文、v19) */
        if ((cur[0] == '+' && cur[1] == '+') ||
            (cur[0] == '-' && cur[1] == '-')) {
            int stmt = parse_stmt();
            if (stmt < 0)
                return -2;
            if (head < 0)
                head = stmt;
            else
                node_next[tail] = stmt;
            tail = stmt;
            continue;
        }
        /* 文の始まりかどうかを先読みする: 識別子で始まる場合、
         * 名前とその次第で「代入文・while・putc」のどれか、
         * それ以外 (式の始まり) もありうる */
        char *save = cur;
        if (is_ident_start(*cur)) {
            if (!parse_ident())
                return -2;
            if (ident_is("typedef")) {
                /* typedef はファイルの範囲だけ (v36)。
                 * 文ではないので並びには入れず、名簿への
                 * 登録だけして次へ進む */
                cur = save;
                if (in_brace) {
                    errmsg = "typedef は関数の外でだけ書けます";
                    return -2;
                }
                if (parse_typedef() < 0)
                    return -2;
                continue;
            }
            if (ident_is("struct")) {
                /* struct タグ { なら定義 (v37)。それ以外
                 * (struct タグ 名前 ; の宣言など) は下の
                 * is_type の判定に委ねる */
                skip_spaces();
                int is_def = 0;
                if (parse_ident()) {
                    skip_spaces();
                    if (*cur == '{')
                        is_def = 1;
                }
                cur = save;
                if (is_def) {
                    if (in_brace) {
                        errmsg = "構造体の定義は関数の外でだけ書けます";
                        return -2;
                    }
                    if (parse_struct_def() < 0)
                        return -2;
                    continue;
                }
                /* 先読みで ident_buf がタグ名に化けたので、
                 * 最初の単語 (struct) を読み直して戻す */
                if (!parse_ident())
                    return -2;
            }
            int is_keyword = ident_is("while") || ident_is("putc") ||
                             ident_is("if") || ident_is("for") ||
                             ident_is("break") || ident_is("continue") ||
                             ident_is("return") || ident_is("switch") ||
                             ident_is("case") || ident_is("default");
            int is_type = is_type_word() || ident_is("const") || ident_is("static") ||
                          is_typedef_name() || ident_is("struct");
            if (is_type) {
                /* 型で始まるのは、関数定義か宣言の文。
                 * 名前の後ろが '[' (配列) か '=' (初期化) か
                 * ';' (宣言だけ) なら宣言の文で、こちらは
                 * どこにでも置ける。'(' なら関数定義で、
                 * 置けるのはプログラムの頭だけ (v21) */
                cur = save;
                int is_decl_stmt = 0;
                if (parse_type()) {
                    skip_spaces();
                    if (*cur == '(') {
                        /* 型の直後の '(' は関数ポインタの
                         * 宣言 (v44)。関数定義は名前で
                         * 始まるので、こちらは宣言の文 */
                        is_decl_stmt = 1;
                    } else if (parse_ident()) {
                        skip_spaces();
                        is_decl_stmt = (*cur == '[' || *cur == '=' ||
                                        *cur == ';');
                    }
                }
                cur = save;
                if (is_decl_stmt) {
                    int stmt = parse_stmt();
                    if (stmt < 0)
                        return -2;
                    if (!in_brace) {
                        /* トップレベルの宣言は実行文では
                         * ないので、文の並びには入れない
                         * (v24)。これで宣言と main が
                         * 混在できる。初期化の値は初期
                         * イメージに記録済み */
                        continue;
                    }
                    if (head < 0)
                        head = stmt;
                    else
                        node_next[tail] = stmt;
                    /* 宣言が文の鎖で返ることもある
                     * (構造体の初期化子の展開、v45)。
                     * 末尾まで進める */
                    tail = stmt;
                    while (node_next[tail] >= 0)
                        tail = node_next[tail];
                    continue;
                }
                if (in_brace) {
                    errmsg = "関数定義はプログラムの頭にだけ書けます";
                    return -2;
                }
                if (!parse_funcdef())
                    return -2;
                continue;
            }
            skip_spaces();
            int is_assign = (*cur == '=' && cur[1] != '=') ||
                            (cur[1] == '=' && (*cur == '+' || *cur == '-' ||
                             *cur == '*' || *cur == '/' || *cur == '%' ||
                             *cur == '&' || *cur == '|' || *cur == '^')) ||
                            ((*cur == '<' || *cur == '>') &&
                             cur[1] == *cur && cur[2] == '=');
                            /* 複合代入 op= も代入文 (v25) */
            int is_call = (*cur == '(');
            int is_incdec = (cur[0] == '+' && cur[1] == '+') ||
                            (cur[0] == '-' && cur[1] == '-');
            int is_index_store = 0;
            if (*cur == '[') {
                /* 名前 [ ... ] の後ろに '=' があれば、要素への
                 * 書き込み文 (式の要素読みと見分ける)。
                 * ']' までを数えて先読みするだけ。2次元の
                 * [ 行 ] [ 列 ] では、']' の後に '[' が続けば
                 * もう一段数え直す (v23) */
                int depth = 0;
                char *p = cur;
                for (;;) {
                    depth = 0;
                    while (*p != '\0') {
                        if (*p == '[')
                            depth++;
                        else if (*p == ']') {
                            depth--;
                            if (depth == 0) {
                                p++;
                                break;
                            }
                        }
                        p++;
                    }
                    if (depth != 0)
                        break;
                    while (*p == ' ' || *p == '\t')
                        p++;
                    if (*p != '[')
                        break;
                }
                is_index_store = (depth == 0 && *p == '=' &&
                                  p[1] != '=');
                if (depth == 0 &&
                    ((p[0] == '+' && p[1] == '+') ||
                     (p[0] == '-' && p[1] == '-')))
                    is_incdec = 1;  /* 要素への増減文 (v25) */
            }
            int is_member_stmt = 0;
            if (*cur == '.' || (cur[0] == '-' && cur[1] == '>') ||
                *cur == '[') {
                /* designator を辿る先読み (v37): [ ] と
                 * . メンバ / -> メンバ を数え、. か -> を
                 * 含んでいて末尾が = か ++ / -- なら、
                 * メンバへの書き込み・増減文 */
                char *p = cur;
                int has = 0;
                int ok = 1;
                for (;;) {
                    while (*p == ' ' || *p == '\t')
                        p++;
                    if (*p == '[') {
                        int d = 0;
                        while (*p != '\0') {
                            if (*p == '[')
                                d++;
                            else if (*p == ']') {
                                d--;
                                if (d == 0) {
                                    p++;
                                    break;
                                }
                            }
                            p++;
                        }
                        if (d != 0) {
                            ok = 0;
                            break;
                        }
                        continue;
                    }
                    if (*p == '.' || (p[0] == '-' && p[1] == '>')) {
                        has = 1;
                        p += (*p == '.') ? 1 : 2;
                        while (*p == ' ' || *p == '\t')
                            p++;
                        while (is_ident_start(*p) || is_digit(*p))
                            p++;
                        continue;
                    }
                    break;
                }
                if (ok) {
                    while (*p == ' ' || *p == '\t')
                        p++;
                    if (has && ((*p == '=' && p[1] != '=') ||
                                (p[0] == '+' && p[1] == '+') ||
                                (p[0] == '-' && p[1] == '-')))
                        is_member_stmt = 1;
                }
            }
            if (is_keyword || is_assign || is_call || is_index_store ||
                is_incdec || is_member_stmt) {
                cur = save;  /* parse_stmt に頭から読ませる */
                int stmt = parse_stmt();
                if (stmt < 0)
                    return -2;
                if (head < 0)
                    head = stmt;
                else
                    node_next[tail] = stmt;
                tail = stmt;
                continue;
            }
            cur = save;  /* 式だった。先読みを巻き戻す */
        }
        if (in_brace) {
            errmsg = "while の本体には文だけが書けます";
            return -2;
        }
        /* 最後の式。プログラムはここで終わる */
        int expr = parse_expr();
        if (expr < 0)
            return -2;
        skip_spaces();
        if (*cur != '\0') {
            errmsg = "最後の式の後に、余分な文字があります";
            return -2;
        }
        prog_final = expr;
        prog_has_final = 1;
        return head;
    }
}

/* 定数式を評価する (v24): ファイル範囲の初期化子の値を、
 * コンパイルの時点で計算するための小さな評価器。扱える
 * のは数、文字列、単項 - !、二項の算術・ビット演算だけ
 * (変数や呼び出しは定数ではない)。戻り値: 1 = 数値定数
 * (*out に値)、2 = 文字列 (*out に文字列番号。実際の値は
 * 文字列の住所で、生成のときに決まる)、0 = 定数でない */
static int eval_const(int node, long *out) {
    int k = node_kind[node];
    if (k == NODE_FADDR) {
        /* 関数の住所 (v44): 値は関数番号、種別は 3 */
        *out = node_val[node];
        return 3;
    }
    if (k == NODE_NUM) {
        /* 16進リテラルは parse の時点で int の範囲に
         * 畳んであるが、値としては符号なし32ビットの
         * 数 (式の中での作り方、gen_load_u32 と同じ)。
         * 符号拡張のまま初期値にすると、式で読んだ
         * ときの値と食い違う (v27 のテストで発覚) */
        *out = node_val[node] & 0xFFFFFFFFL;
        return 1;
    }
    if (k == NODE_STR) {
        *out = node_val[node];
        return 2;
    }
    if (k == NODE_NEG) {
        if (eval_const(node_lhs[node], out) != 1)
            return 0;
        *out = -*out;
        return 1;
    }
    if (k == NODE_NOT) {
        if (eval_const(node_lhs[node], out) != 1)
            return 0;
        *out = *out == 0 ? 1 : 0;
        return 1;
    }
    if (k == NODE_BITNOT) {
        if (eval_const(node_lhs[node], out) != 1)
            return 0;
        *out = ~*out;
        return 1;
    }
    if (k == NODE_COND) {
        /* 三項も定数同士なら畳める (v25)。文字列の枝は
         * 選ばれた側がそのまま答えになる */
        long c;
        if (eval_const(node_lhs[node], &c) != 1)
            return 0;
        return eval_const(c != 0 ? node_val[node] : node_rhs[node],
                          out);
    }
    if (k == NODE_ADD || k == NODE_SUB || k == NODE_MUL ||
        k == NODE_DIV || k == NODE_MOD || k == NODE_SHL ||
        k == NODE_SHR || k == NODE_AND || k == NODE_OR ||
        k == NODE_XOR) {
        long l;
        long r;
        if (eval_const(node_lhs[node], &l) != 1 ||
            eval_const(node_rhs[node], &r) != 1)
            return 0;
        if (k == NODE_ADD)
            *out = l + r;
        else if (k == NODE_SUB)
            *out = l - r;
        else if (k == NODE_MUL)
            *out = l * r;
        else if (k == NODE_DIV) {
            if (r == 0)
                return 0;
            *out = l / r;
        } else if (k == NODE_MOD) {
            if (r == 0)
                return 0;
            *out = l % r;
        } else if (k == NODE_SHL)
            *out = l << r;
        else if (k == NODE_SHR)
            *out = l >> r;
        else if (k == NODE_AND)
            *out = l & r;
        else if (k == NODE_OR)
            *out = l | r;
        else
            *out = l ^ r;
        return 1;
    }
    return 0;
}

/* typedef を読む (v36): typedef <型> <名前> ;
 * ファイルの範囲でだけ呼ばれる (振り分けは
 * parse_stmt_list)。名簿に登録するだけで、
 * ノードは作らない。成功したら 0、失敗は -1 */
static int parse_typedef(void) {
    if (!parse_ident())  /* "typedef" 自体を読み直す */
        return -1;
    skip_spaces();
    if (!parse_type()) {
        errmsg = "typedef の後には型が必要です";
        return -1;
    }
    int et = parse_type_etype;
    int bs = parse_type_bsize;
    int pl = parse_type_plevel;
    int st = parse_type_struct;
    int fs = parse_type_fsig;
    char name_buf[32];
    int name_len;
    skip_spaces();
    if (*cur == '(') {
        /* 関数ポインタの typedef (v44):
         * typedef 戻り値 (*名前) (引数の型の並び) ; */
        int arr;
        int sig;
        if (!parse_fptr_decl(et, pl, st, fs, name_buf,
                             &name_len, &arr, &sig))
            return -1;
        if (arr >= 0) {
            errmsg = "配列の typedef はできません";
            return -1;
        }
        if (name_len == 0) {
            errmsg = "typedef には付ける名前が必要です";
            return -1;
        }
        et = 8;
        bs = 8;
        pl = 0;
        st = -1;
        fs = sig + 1;
        /* 名前の衝突検査が ident_buf を見るので戻しておく */
        ident_len = name_len;
        for (int i = 0; i < name_len; i++)
            ident_buf[i] = name_buf[i];
    } else {
        if (!parse_ident()) {
            errmsg = "typedef には付ける名前が必要です";
            return -1;
        }
        name_len = ident_len;
        for (int i = 0; i < name_len; i++)
            name_buf[i] = ident_buf[i];
    }
    skip_spaces();
    if (*cur != ';') {
        errmsg = "typedef の終わりには ; が必要です (名前は1つだけ付けられます)";
        return -1;
    }
    cur++;
    /* 名前の衝突を確かめる: 型の言葉そのもの、既存の
     * typedef、既存の変数・配列・関数と同名は不可 */
    if (is_type_word() || ident_is("const") || ident_is("static") ||
        ident_is("typedef")) {
        errmsg = "その名前は型の言葉なので typedef 名にできません";
        return -1;
    }
    int existing = find_typedef(name_buf, name_len);
    if (existing >= 0) {
        /* 同じ型への付け直しは受ける (C11 と同じ。ヘッダを
         * 複数のファイルで共有する形のため)。違う型への
         * 付け直しはエラー */
        if (tdef_etype[existing] == et && tdef_bsize[existing] == bs &&
            tdef_plevel[existing] == pl && tdef_struct[existing] == st + 1 &&
            tdef_fsig[existing] == fs)
            return 0;
        errmsg = "その typedef 名は違う型で既にあります";
        return -1;
    }
    if (find_var(name_buf, name_len) >= 0 ||
        find_array(name_buf, name_len) >= 0 ||
        find_func(name_buf, name_len) >= 0) {
        errmsg = "その名前は変数・配列・関数に使われています";
        return -1;
    }
    if (ntdef >= 64) {
        errmsg = "typedef は64個までです";
        return -1;
    }
    int td = ntdef++;
    for (int i = 0; i < name_len; i++)
        tdef_names[td][i] = name_buf[i];
    tdef_names[td][name_len] = '\0';
    tdef_name_len[td] = name_len;
    tdef_etype[td] = et;
    tdef_bsize[td] = bs;
    tdef_plevel[td] = pl;
    tdef_struct[td] = st + 1;
    tdef_fsig[td] = (char)fs;
    return 0;
}

/* 構造体の定義を読む (v37): struct タグ { メンバ... } ;
 * ファイルの範囲でだけ呼ばれる (振り分けは
 * parse_stmt_list)。タグはメンバを読む前に登録する
 * (自分のタグへのポインタメンバのため)。レイアウトは
 * C 互換: メンバはアラインメントで切り上げ、末尾も
 * 構造体のアラインメントで切り上げる (doc/struct.md)。
 * 同じタグの再定義は、構成が完全に同じなら受ける
 * (分割コンパイルで共有ファイルが重なるため)。
 * 成功したら 0、失敗は -1 */
static int parse_struct_def(void) {
    if (!parse_ident())  /* "struct" 自体を読み直す */
        return -1;
    skip_spaces();
    if (!parse_ident()) {
        errmsg = "struct にはタグ名が必要です";
        return -1;
    }
    char tag_buf[32];
    int tag_len = ident_len;
    for (int i = 0; i < tag_len; i++)
        tag_buf[i] = ident_buf[i];
    skip_spaces();
    if (*cur != '{') {
        errmsg = "構造体の定義は struct 名前 { ... }; の形です";
        return -1;
    }
    cur++;
    int existing = find_struct_tag(tag_buf, tag_len);
    int tg;
    if (existing >= 0) {
        if (!stag_complete[existing]) {
            errmsg = "その構造体は定義の途中です";
            return -1;
        }
        tg = existing;  /* メンバは仮置きに読んで比べる */
    } else {
        if (nstag >= 32) {
            errmsg = "構造体は32個までです";
            return -1;
        }
        tg = nstag++;
        for (int i = 0; i < tag_len; i++)
            stag_names[tg][i] = tag_buf[i];
        stag_names[tg][tag_len] = '\0';
        stag_name_len[tg] = tag_len;
        stag_mfirst[tg] = nsmem;
        stag_mcount[tg] = 0;
        stag_size[tg] = 0;
        stag_align[tg] = 1;
        stag_complete[tg] = 0;
    }
    int mstart = nsmem;
    int offset = 0;
    int maxalign = 1;
    int mcount = 0;
    for (;;) {
        skip_spaces();
        if (*cur == '}') {
            cur++;
            break;
        }
        if (*cur == '\0') {
            errmsg = "構造体の定義が閉じていません";
            return -1;
        }
        if (!parse_type()) {
            errmsg = "メンバは 型 名前 ; の形で書きます";
            return -1;
        }
        int met = parse_type_etype;
        int mpl = parse_type_plevel;
        int mst = parse_type_struct;
        int mfs = parse_type_fsig;
        skip_spaces();
        if (!parse_ident()) {
            errmsg = "メンバには名前が必要です";
            return -1;
        }
        char mname[32];
        int mlen = ident_len;
        for (int i = 0; i < mlen; i++)
            mname[i] = ident_buf[i];
        int marr = 0;
        skip_spaces();
        if (*cur == '[') {
            cur++;
            skip_spaces();
            marr = parse_number_value();
            if (marr < 1) {
                errmsg = "メンバの配列の大きさが不正です";
                return -1;
            }
            if (!expect_char(']', "配列の大きさの後には ] が必要です"))
                return -1;
            skip_spaces();
        }
        if (!expect_char(';', "メンバの終わりには ; が必要です"))
            return -1;
        int esz;
        int eal;
        if (mpl >= 1) {
            esz = 8;
            eal = 8;
        } else if (mst >= 0) {
            if (!stag_complete[mst]) {
                errmsg = "未完成の構造体は値メンバにできません (ポインタなら可)";
                return -1;
            }
            esz = stag_size[mst];
            eal = stag_align[mst];
        } else {
            esz = met & 15;
            eal = met & 15;
            if (eal > 8)
                eal = 8;
        }
        if (offset % eal)
            offset += eal - (offset % eal);
        int moff = offset;
        offset += marr > 0 ? esz * marr : esz;
        if (eal > maxalign)
            maxalign = eal;
        for (int i = mstart; i < nsmem; i++) {
            if (smem_name_len[i] == mlen) {
                int j = 0;
                while (j < mlen && smem_names[i][j] == mname[j])
                    j++;
                if (j == mlen) {
                    errmsg = "メンバ名が重なっています";
                    return -1;
                }
            }
        }
        if (nsmem >= 160) {
            errmsg = "構造体のメンバが多すぎます (合計160まで)";
            return -1;
        }
        int mi = nsmem++;
        for (int i = 0; i < mlen; i++)
            smem_names[mi][i] = mname[i];
        smem_names[mi][mlen] = '\0';
        smem_name_len[mi] = mlen;
        smem_off[mi] = moff;
        smem_etype[mi] = met;
        smem_plevel[mi] = mpl;
        smem_struct[mi] = mst + 1;
        smem_fsig[mi] = (char)mfs;
        smem_count[mi] = marr;
        mcount++;
    }
    if (!expect_char(';', "構造体の定義の終わりには ; が必要です"))
        return -1;
    if (mcount == 0) {
        errmsg = "メンバの無い構造体は作れません";
        return -1;
    }
    int total = offset;
    if (total % maxalign)
        total += maxalign - (total % maxalign);
    if (existing >= 0) {
        /* 同じ構成か比べ、違えば食い違いとして止める。
         * 同じなら仮置きしたメンバを捨てる */
        int same = (mcount == stag_mcount[tg] &&
                    total == stag_size[tg] &&
                    maxalign == stag_align[tg]);
        if (same) {
            for (int k = 0; k < mcount; k++) {
                int a = stag_mfirst[tg] + k;
                int b = mstart + k;
                if (smem_name_len[a] != smem_name_len[b] ||
                    smem_off[a] != smem_off[b] ||
                    smem_etype[a] != smem_etype[b] ||
                    smem_plevel[a] != smem_plevel[b] ||
                    smem_struct[a] != smem_struct[b] ||
                    smem_fsig[a] != smem_fsig[b] ||
                    smem_count[a] != smem_count[b])
                    same = 0;
                if (same) {
                    int j = 0;
                    while (j < smem_name_len[a] &&
                           smem_names[a][j] == smem_names[b][j])
                        j++;
                    if (j != smem_name_len[a])
                        same = 0;
                }
            }
        }
        nsmem = mstart;
        if (!same) {
            errmsg = "構造体が違う構成で再定義されています";
            return -1;
        }
        return 0;
    }
    stag_size[tg] = total;
    stag_align[tg] = maxalign;
    stag_mcount[tg] = mcount;
    stag_complete[tg] = 1;
    return 0;
}

/* 配列初期化子の要素1つを読む (v40、設計は
 * doc/array-init.md): ノードは作らない (表が何万
 * 要素にもなるので、式の木を作っていたらノード表
 * が溢れる)。読めるのは符号つきの10進数・16進数・
 * 文字リテラルだけ。成功したら 1、失敗は 0 */
static int parse_init_const(long *out) {
    skip_spaces();
    int neg = 0;
    if (*cur == '-') {
        neg = 1;
        cur++;
        skip_spaces();
    } else if (*cur == '+') {
        cur++;
        skip_spaces();
    }
    long v;
    if (*cur == '\'') {
        /* 文字リテラル (parse_char_node と同じ読み方) */
        cur++;
        if (*cur == '\\') {
            cur++;
            if (*cur == 'n')
                v = '\n';
            else if (*cur == 't')
                v = '\t';
            else if (*cur == 'r')
                v = '\r';
            else if (*cur == '0')
                v = 0;
            else if (*cur == '\\')
                v = '\\';
            else if (*cur == '\'')
                v = '\'';
            else if (*cur == '"')
                v = '"';
            else {
                errmsg = "使えるエスケープは \\n \\t \\r \\0 \\\\ \\' \\\" だけです";
                return 0;
            }
            cur++;
        } else if (*cur == '\'' || *cur == '\0') {
            errmsg = "文字リテラルが空か、閉じていません";
            return 0;
        } else {
            v = (unsigned char)*cur;
            cur++;
        }
        if (*cur != '\'') {
            errmsg = "文字リテラルは1文字だけです (閉じの ' が必要です)";
            return 0;
        }
        cur++;
    } else if (cur[0] == '0' && (cur[1] == 'x' || cur[1] == 'X')) {
        /* 16進数 (parse_number_node と同じ畳み方) */
        cur += 2;
        if (hex_value(*cur) < 0) {
            errmsg = "0x の後には16進数の数字が必要です";
            return 0;
        }
        unsigned long uv = 0;
        while (hex_value(*cur) >= 0) {
            uv = uv * 16ul + (unsigned long)hex_value(*cur);
            if (uv > 0xFFFFFFFFul) {
                errmsg = "16進数が大きすぎます (32ビットまで)";
                return 0;
            }
            cur++;
        }
        while (*cur == 'u' || *cur == 'U' || *cur == 'l' ||
               *cur == 'L')
            cur++;
        v = (long)(unsigned int)uv;
        if (v >= 0x80000000L)
            v = v - 0x80000000L - 0x80000000L;
    } else if (is_digit(*cur)) {
        int dv = parse_number_value();
        if (dv < 0)
            return 0;
        v = dv;
    } else {
        errmsg = "配列の初期化子は数値の定数だけです";
        return 0;
    }
    *out = neg ? -v : v;
    return 1;
}

/* 初期化子の { ... } の中身を読む (v40)。開きの
 * { は消費済みの前提で、閉じの } まで消費する。
 * 値は値プールへ来た順に足し、読んだ要素数を返す
 * (失敗は -1)。行ごとの { } は入れ子で呼んで
 * 平らに読む (2次元の書き方の吸収) */
static int parse_init_values(void) {
    int count = 0;
    skip_spaces();
    if (*cur == '}') {
        cur++;
        return 0;
    }
    for (;;) {
        skip_spaces();
        if (*cur == '{') {
            cur++;
            int sub = parse_init_values();
            if (sub < 0)
                return -1;
            count = count + sub;
        } else {
            long v;
            if (!parse_init_const(&v))
                return -1;
            if (arr_init_used >= 49152) {
                errmsg = "配列の初期化子が多すぎます (合計49152要素まで)";
                return -1;
            }
            arr_init_pool[arr_init_used] = v;
            arr_init_used = arr_init_used + 1;
            count++;
        }
        skip_spaces();
        if (*cur == ',') {
            cur++;
            skip_spaces();
            if (*cur == '}') {
                cur++;
                return count;
            }
            continue;
        }
        if (*cur == '}') {
            cur++;
            return count;
        }
        errmsg = "初期化子は , か } で区切ります";
        return -1;
    }
}

/* 配列宣言の初期化子を読む (v40)。= は消費済み。
 * 値をプールに貯めてから大きさを確定し、配列を
 * 名簿に載せて初期値を結び付ける。rows が -1 なら
 * 大きさの省略で、要素数から決める (2次元は
 * 1次元目だけ)。成功したら宣言ノード、失敗は -1 */
static int parse_array_init(char *name, int len, int rows, int cols,
                            int esize, int etype, int strct,
                            int plevel) {
    int start = arr_init_used;
    int count = 0;
    skip_spaces();
    if (*cur == '"') {
        /* 文字列リテラル (1次元の char 配列だけ) */
        if (esize != 1 || plevel != 0 || cols > 0) {
            errmsg = "文字列で初期化できるのは char の配列だけです";
            return -1;
        }
        int node = parse_string_node();
        if (node < 0)
            return -1;
        int si = node_val[node];
        int slen = str_len[si];
        if (rows >= 0 && slen > rows) {
            errmsg = "文字列が配列より長いです";
            return -1;
        }
        /* 末尾の 0 は、大きさに余りがあるときだけ
         * 数に入れる (C の約束どおり) */
        count = slen;
        if (rows < 0 || rows > slen)
            count = slen + 1;
        for (int i = 0; i < count; i++) {
            if (arr_init_used >= 49152) {
                errmsg = "配列の初期化子が多すぎます (合計49152要素まで)";
                return -1;
            }
            if (i < slen)
                arr_init_pool[arr_init_used] =
                    (unsigned char)str_pool[str_off[si] + i];
            else
                arr_init_pool[arr_init_used] = 0;
            arr_init_used = arr_init_used + 1;
        }
    } else if (*cur == '{') {
        cur++;
        count = parse_init_values();
        if (count < 0)
            return -1;
    } else {
        errmsg = "配列の初期化子は { } か文字列です";
        return -1;
    }
    if (!expect_char(';', "宣言の終わりには ; が必要です"))
        return -1;
    if (rows < 0) {
        /* 大きさの省略: 要素数から決める */
        if (cols > 0)
            rows = (count + cols - 1) / cols;
        else
            rows = count;
        if (rows < 1) {
            errmsg = "初期化子が空です";
            return -1;
        }
    }
    int cap = cols > 0 ? rows * cols : rows;
    if (count > cap) {
        errmsg = "初期化子が配列より多いです";
        return -1;
    }
    int a = define_array_s(name, len, rows, cols, esize, etype,
                           strct, plevel);
    if (a < 0)
        return -1;
    if (arr_init_cnt[a] > 0) {
        errmsg = "初期値は一つだけです";
        return -1;
    }
    arr_init_off[a] = start;
    arr_init_cnt[a] = count;
    /* 実体が届く末尾は「最後の 0 でないバイト」まで。
     * 末尾の 0 はローダのゼロ埋めに任せる (一括版と
     * リンク版でファイルの形を揃えるための約束) */
    int tend = 0;
    for (int e = count - 1; e >= 0 && tend == 0; e--) {
        unsigned long v = (unsigned long)arr_init_pool[start + e];
        for (int b = esize - 1; b >= 0; b--) {
            if (((v >> (8 * b)) & 0xff) != 0) {
                tend = e * esize + b + 1;
                break;
            }
        }
    }
    int end = arr_off[a] + tend;
    if (end > arr_init_end)
        arr_init_end = end;
    return new_node(NODE_DECL, a, 0, 0);
}

/* 関数ポインタの配列の初期化子を読む (v44)。= は
 * 消費済み。要素は関数名 (その住所) か 0 だけ。
 * 住所はリンク時にしか決まらないので、池には 0 を
 * 置き、関数参照は別表 (arr_fref_off / arr_fref_func)
 * に控える。末尾の扱いは parse_array_init と同じ
 * 約束だが、関数参照のある要素は「0 でない」もの
 * として数える (住所が入るので実体が届く) */
static int parse_funcptr_array_init(char *name, int len,
                                    int rows, int cols,
                                    int sig) {
    if (cols > 0) {
        errmsg = "関数ポインタの配列の初期化は1次元だけです";
        return -1;
    }
    int start = arr_init_used;
    int count = 0;
    skip_spaces();
    if (*cur != '{') {
        errmsg = "配列の初期化子は { } で書きます";
        return -1;
    }
    cur++;
    skip_spaces();
    if (*cur == '}') {
        errmsg = "初期化子が空です";
        return -1;
    }
    for (;;) {
        skip_spaces();
        if (arr_init_used >= 49152) {
            errmsg = "配列の初期化子が多すぎます (合計49152要素まで)";
            return -1;
        }
        if (is_ident_start(*cur)) {
            if (!parse_ident())
                return -1;
            int f = find_func(ident_buf, ident_len);
            if (f < 0) {
                errmsg = "初期化子に置けるのは定義済みの関数名か 0 だけです";
                return -1;
            }
            if (func_sig[f] != sig) {
                errmsg = "関数ポインタの型 (署名) が合いません";
                return -1;
            }
            if (arr_nfref >= 1024) {
                errmsg = "関数ポインタの初期化子が多すぎます";
                return -1;
            }
            arr_fref_off[arr_nfref] = arr_init_used;
            arr_fref_func[arr_nfref] = f;
            arr_nfref++;
            func_called[f] = 1;
            arr_init_pool[arr_init_used] = 0;
            arr_init_used++;
        } else if (is_digit(*cur)) {
            long v;
            if (!parse_init_const(&v))
                return -1;
            if (v != 0) {
                errmsg = "初期化子に置けるのは定義済みの関数名か 0 だけです";
                return -1;
            }
            arr_init_pool[arr_init_used] = 0;
            arr_init_used++;
        } else {
            errmsg = "初期化子に置けるのは定義済みの関数名か 0 だけです";
            return -1;
        }
        count++;
        skip_spaces();
        if (*cur == ',') {
            cur++;
            skip_spaces();
            if (*cur == '}')
                break;  /* 末尾のカンマは受ける */
            continue;
        }
        break;
    }
    if (!expect_char('}', "初期化子の終わりには } が必要です"))
        return -1;
    if (!expect_char(';', "宣言の終わりには ; が必要です"))
        return -1;
    if (rows < 0)
        rows = count;
    if (count > rows) {
        errmsg = "初期化子が配列より多いです";
        return -1;
    }
    int a = define_array_s(name, len, rows, 0, 8, 8, -1, 0);
    if (a < 0)
        return -1;
    if (arr_init_cnt[a] > 0) {
        errmsg = "初期値は一つだけです";
        return -1;
    }
    arr_fsig[a] = (char)(sig + 1);
    arr_init_off[a] = start;
    arr_init_cnt[a] = count;
    int tend = 0;
    for (int e = count - 1; e >= 0 && tend == 0; e--) {
        if (arr_init_pool[start + e] != 0 ||
            fref_at(start + e) >= 0)
            tend = (e + 1) * 8;
    }
    int end = arr_off[a] + tend;
    if (end > arr_init_end)
        arr_init_end = end;
    return new_node(NODE_DECL, a, 0, 0);
}

/* --- 構造体のローカル初期化子 (v45、設計は
 * doc/struct-value.md) ---
 *   struct P p = { 値, { 入れ子 }, ... };
 * を、宣言の直後に実行される「メンバへの代入文の並び」
 * に parse の時点で展開する。生成側に新しい仕組みは
 * 要らない (代入文は v45 の丸ごと代入と要素への
 * 書き込みで処理できる)。書かれなかったメンバは
 * C の作法どおり 0 で埋める。入れ子の構造体や配列の
 * メンバは { } で囲む形だけを受ける (波かっこの
 * 省略は受けない)。基底 (base) は変数やメンバを表す
 * ノードで、代入文の並びは node_next の鎖で繋ぎ、
 * 末尾のノードを返す (失敗は -1) */
static int parse_struct_init(int base, int tag, int tail);
static int parse_member_init(int base, int mi, int tail);
static int parse_array_member_init(int base, int mi, int tail);
static int parse_string_member_init(int base, int mi, int tail);
static int parse_elem_init(int base, int mi, int idx, int tail);
static int init_zero_struct(int base, int tag, int tail);
static int init_zero_member(int base, int mi, int tail);
static int init_zero_elem(int base, int mi, int idx, int tail);

/* 代入文を1つ鎖の末尾に足し、新しい末尾を返す */
static int init_chain_store(int tail, int elem, int val) {
    int st = new_node(NODE_ELEM_STORE, 0, elem, val);
    if (st < 0)
        return -1;
    node_next[tail] = st;
    return st;
}

static int parse_struct_init(int base, int tag, int tail) {
    cur++;  /* '{' を消費 (呼び側で確認済み) */
    int nmem = stag_mcount[tag];
    int first = stag_mfirst[tag];
    int i = 0;
    skip_spaces();
    if (*cur == '}') {
        cur++;
        /* 値が無い: 全メンバ 0 埋め */
        for (int j = 0; j < nmem; j++) {
            tail = init_zero_member(base, first + j, tail);
            if (tail < 0)
                return -1;
        }
        return tail;
    }
    for (;;) {
        if (i >= nmem) {
            errmsg = "初期化子がメンバの数より多いです";
            return -1;
        }
        tail = parse_member_init(base, first + i, tail);
        if (tail < 0)
            return -1;
        i++;
        skip_spaces();
        if (*cur == ',') {
            cur++;
            skip_spaces();
            if (*cur == '}') {
                cur++;
                break;
            }
            continue;
        }
        if (*cur == '}') {
            cur++;
            break;
        }
        errmsg = "初期化子は , か } で区切ります";
        return -1;
    }
    /* 書かれなかったメンバは 0 埋め */
    for (int j = i; j < nmem; j++) {
        tail = init_zero_member(base, first + j, tail);
        if (tail < 0)
            return -1;
    }
    return tail;
}

static int parse_member_init(int base, int mi, int tail) {
    skip_spaces();
    if (smem_count[mi] > 0) {
        /* 配列メンバ: { } か、char の配列なら文字列 */
        if (*cur == '{')
            return parse_array_member_init(base, mi, tail);
        if (*cur == '"' && smem_plevel[mi] == 0 &&
            smem_struct[mi] == 0 && smem_etype[mi] == 1)
            return parse_string_member_init(base, mi, tail);
        errmsg = "配列メンバの初期化は { } か文字列で書きます";
        return -1;
    }
    if (smem_struct[mi] > 0 && smem_plevel[mi] == 0 &&
        *cur == '{') {
        /* 構造体メンバ: 入れ子の初期化子 */
        int mbase = new_node(NODE_MEMBER, mi, base, 0);
        if (mbase < 0)
            return -1;
        return parse_struct_init(mbase, smem_struct[mi] - 1,
                                 tail);
    }
    /* スカラ・ポインタ・関数ポインタのメンバ、または
     * 構造体の値の式: 普通の式として読む */
    int expr = parse_expr();
    if (expr < 0)
        return -1;
    int elem = new_node(NODE_MEMBER, mi, base, 0);
    if (elem < 0)
        return -1;
    return init_chain_store(tail, elem, expr);
}

static int parse_array_member_init(int base, int mi, int tail) {
    cur++;  /* '{' を消費 */
    int n = smem_count[mi];
    int i = 0;
    skip_spaces();
    if (*cur == '}') {
        cur++;
        for (int j = 0; j < n; j++) {
            tail = init_zero_elem(base, mi, j, tail);
            if (tail < 0)
                return -1;
        }
        return tail;
    }
    for (;;) {
        if (i >= n) {
            errmsg = "初期化子が要素の数より多いです";
            return -1;
        }
        tail = parse_elem_init(base, mi, i, tail);
        if (tail < 0)
            return -1;
        i++;
        skip_spaces();
        if (*cur == ',') {
            cur++;
            skip_spaces();
            if (*cur == '}') {
                cur++;
                break;
            }
            continue;
        }
        if (*cur == '}') {
            cur++;
            break;
        }
        errmsg = "初期化子は , か } で区切ります";
        return -1;
    }
    for (int j = i; j < n; j++) {
        tail = init_zero_elem(base, mi, j, tail);
        if (tail < 0)
            return -1;
    }
    return tail;
}

static int parse_string_member_init(int base, int mi, int tail) {
    int s = parse_string_node();
    if (s < 0)
        return -1;
    int idx = node_val[s];
    int len = str_len[idx];
    int n = smem_count[mi];
    /* 文字の並びを置き、残りは 0 (最初の一つが終端) */
    if (len > n) {
        errmsg = "文字列がメンバの配列より長いです";
        return -1;
    }
    for (int j = 0; j < n; j++) {
        int b = j < len ? (unsigned char)str_pool[str_off[idx] + j]
                        : 0;
        int num = new_node(NODE_NUM, b, 0, 0);
        if (num < 0)
            return -1;
        int inum = new_node(NODE_NUM, j, 0, 0);
        if (inum < 0)
            return -1;
        /* MELEM の基底はメンバそのものを表すノード
         * (parse_postfix の形と同じ) */
        int mnode = new_node(NODE_MEMBER, mi, base, 0);
        if (mnode < 0)
            return -1;
        int elem = new_node(NODE_MELEM, mi, mnode, inum);
        if (elem < 0)
            return -1;
        tail = init_chain_store(tail, elem, num);
        if (tail < 0)
            return -1;
    }
    return tail;
}

static int parse_elem_init(int base, int mi, int idx, int tail) {
    skip_spaces();
    int inum = new_node(NODE_NUM, idx, 0, 0);
    if (inum < 0)
        return -1;
    int mnode = new_node(NODE_MEMBER, mi, base, 0);
    if (mnode < 0)
        return -1;
    int ebase = new_node(NODE_MELEM, mi, mnode, inum);
    if (ebase < 0)
        return -1;
    if (smem_struct[mi] > 0 && smem_plevel[mi] == 0 &&
        *cur == '{')
        /* 構造体の要素: 入れ子の初期化子 */
        return parse_struct_init(ebase, smem_struct[mi] - 1,
                                 tail);
    int expr = parse_expr();
    if (expr < 0)
        return -1;
    return init_chain_store(tail, ebase, expr);
}

static int init_zero_struct(int base, int tag, int tail) {
    int nmem = stag_mcount[tag];
    int first = stag_mfirst[tag];
    for (int j = 0; j < nmem; j++) {
        tail = init_zero_member(base, first + j, tail);
        if (tail < 0)
            return -1;
    }
    return tail;
}

static int init_zero_member(int base, int mi, int tail) {
    if (smem_count[mi] > 0) {
        for (int j = 0; j < smem_count[mi]; j++) {
            tail = init_zero_elem(base, mi, j, tail);
            if (tail < 0)
                return -1;
        }
        return tail;
    }
    if (smem_struct[mi] > 0 && smem_plevel[mi] == 0) {
        int mbase = new_node(NODE_MEMBER, mi, base, 0);
        if (mbase < 0)
            return -1;
        return init_zero_struct(mbase, smem_struct[mi] - 1,
                                tail);
    }
    int elem = new_node(NODE_MEMBER, mi, base, 0);
    if (elem < 0)
        return -1;
    int zero = new_node(NODE_NUM, 0, 0, 0);
    if (zero < 0)
        return -1;
    return init_chain_store(tail, elem, zero);
}

static int init_zero_elem(int base, int mi, int idx, int tail) {
    int inum = new_node(NODE_NUM, idx, 0, 0);
    if (inum < 0)
        return -1;
    int mnode = new_node(NODE_MEMBER, mi, base, 0);
    if (mnode < 0)
        return -1;
    int ebase = new_node(NODE_MELEM, mi, mnode, inum);
    if (ebase < 0)
        return -1;
    if (smem_struct[mi] > 0 && smem_plevel[mi] == 0)
        return init_zero_struct(ebase, smem_struct[mi] - 1,
                                tail);
    int zero = new_node(NODE_NUM, 0, 0, 0);
    if (zero < 0)
        return -1;
    return init_chain_store(tail, ebase, zero);
}

/* 宣言の文を読む (v21 で変数宣言を追加):
 *   型 名前 [ 大きさ ] ;         配列の宣言 (要素の大きさは
 *   型 名前 [ 行 ] [ 列 ] ;       型から決まる、v23。2次元も)
 *   型 名前 [ 大きさ ] = { ... }; 配列の初期化子 (v40、
 *   型 名前 [ ] = "..." ;         ファイル範囲だけ)
 *   型 名前 = 式 ;         初期化つきの変数宣言
 *   型 名前 ;              変数の宣言だけ (値は不定。グローバル
 *                          は静的領域で 0 始まり)
 * 変数の宣言は、その場所 (関数の中ならローカル、外なら
 * グローバル) に変数を作る。成功したらノード番号、失敗は -1 */
static int parse_decl_stmt(void) {
    if (!parse_type()) {
        errmsg = "宣言は 型 名前 の形で書きます";
        return -1;
    }
    int bsize = parse_type_bsize;
    int plevel = parse_type_plevel;
    int etype = parse_type_etype;
    int strct = parse_type_struct;
    int fsig = parse_type_fsig;
    skip_spaces();
    if (*cur == '(') {
        /* 関数ポインタの宣言 (v44):
         *   型 (*名前) (引数) ;          変数
         *   型 (*名前) (引数) = 関数名 ;  初期化つき
         *   型 (*名前[N]) (引数) ;       配列 */
        char dname[32];
        int dlen;
        int darr;
        int dsig;
        if (!parse_fptr_decl(etype, plevel, strct, fsig,
                             dname, &dlen, &darr, &dsig))
            return -1;
        if (dlen == 0) {
            errmsg = "宣言には名前が必要です";
            return -1;
        }
        if (darr >= 0) {
            skip_spaces();
            if (*cur == '=') {
                cur++;
                if (parse_cur_func >= 0) {
                    errmsg = "関数の中の配列初期化はまだできません";
                    return -1;
                }
                return parse_funcptr_array_init(dname, dlen,
                                                darr, 0, dsig);
            }
            if (!expect_char(';', "配列の宣言の終わりには ; が必要です"))
                return -1;
            int a = define_array_s(dname, dlen, darr, 0,
                                   8, 8, -1, 0);
            if (a < 0)
                return -1;
            arr_fsig[a] = (char)(dsig + 1);
            return new_node(NODE_DECL, a, 0, 0);
        }
        skip_spaces();
        if (*cur == '=') {
            cur++;
            int expr = parse_expr();
            if (expr < 0)
                return -1;
            int var = define_var_full(dname, dlen, 8, 0, 1,
                                      8, -1, 1);
            if (var < 0)
                return -1;
            var_fsig[var] = (char)(dsig + 1);
            if (!expect_char(';', "宣言の終わりには ; が必要です"))
                return -1;
            if (parse_cur_func < 0) {
                /* ファイル範囲: 定数の初期化だけ (下の
                 * 通常の形と同じ約束)。置けるのは
                 * 署名の合う関数名か 0 */
                long v;
                int kind = eval_const(expr, &v);
                if (var_init_kind[var] != 0) {
                    errmsg = "初期値は一つだけです";
                    return -1;
                }
                if (kind == 3) {
                    if (func_sig[v] != dsig) {
                        errmsg = "関数ポインタの型 (署名) が合いません";
                        return -1;
                    }
                    var_init_kind[var] = 3;
                    var_init_val[var] = v;
                } else if (kind == 1 && v == 0) {
                    var_init_kind[var] = 1;
                    var_init_val[var] = 0;
                } else {
                    errmsg = "関数ポインタの初期化子は関数名か 0 だけです";
                    return -1;
                }
                has_var_inits = 1;
                return new_node(NODE_VDECL, var, 0, 0);
            }
            return new_node(NODE_ASSIGN, var, expr, 0);
        }
        if (*cur == ';') {
            cur++;
            int var = define_var_full(dname, dlen, 8, 0, 1,
                                      8, -1, 1);
            if (var < 0)
                return -1;
            var_fsig[var] = (char)(dsig + 1);
            return new_node(NODE_VDECL, var, 0, 0);
        }
        errmsg = "宣言は 型 (*名前) (引数) ; の形です";
        return -1;
    }
    if (!parse_ident()) {
        errmsg = "宣言には名前が必要です";
        return -1;
    }
    char name_buf[32];
    int name_len = ident_len;
    for (int i = 0; i < name_len; i++)
        name_buf[i] = ident_buf[i];
    skip_spaces();
    if (*cur == '[') {
        /* 配列の宣言: [ 大きさ ] または [ 行 ] [ 列 ] (v23)。
         * 大きさの省略は初期化子があるときだけ (v40) */
        cur++;
        skip_spaces();
        int size = -1;   /* -1 = 省略 (初期化子から決める) */
        if (*cur != ']') {
            size = parse_number_value();
            if (size < 0)
                return -1;
            if (size < 1 || size > 1048576) {
                errmsg = "配列の大きさは 1 から 1048576 までです";
                return -1;
            }
        }
        if (!expect_char(']', "配列の大きさの後には ] が必要です"))
            return -1;
        int cols = 0;
        skip_spaces();
        if (*cur == '[') {
            /* 2次元目: 列の数 */
            cur++;
            skip_spaces();
            cols = parse_number_value();
            if (cols < 0)
                return -1;
            if (cols < 1 || cols > 1048576) {
                errmsg = "配列の大きさは 1 から 1048576 までです";
                return -1;
            }
            if (!expect_char(']', "配列の大きさの後には ] が必要です"))
                return -1;
        }
        /* 要素の大きさ (v23): 型に * が付いていれば要素は
         * ポインタなので 8 バイト。そうでなければ型の
         * 基本の大きさ (char は 1、それ以外は 8) */
        int esize = plevel >= 1 ? 8 : bsize;
        int eet = plevel >= 1 ? 8 : etype;
        skip_spaces();
        if (*cur == '=') {
            /* 配列の初期化子 (v40、設計は doc/array-init.md)。
             * 値は実行コードではなくデータで運ぶので、
             * 置けるのはファイル範囲の、ポインタでも
             * 構造体でもない要素の配列だけ */
            cur++;
            if (parse_cur_func >= 0) {
                errmsg = "関数の中の配列初期化はまだできません";
                return -1;
            }
            if (fsig > 0) {
                /* 関数ポインタの配列の初期化子 (v44) */
                return parse_funcptr_array_init(name_buf, name_len,
                                                size, cols, fsig - 1);
            }
            if (plevel >= 1) {
                errmsg = "ポインタの配列の初期化はまだできません";
                return -1;
            }
            if (strct >= 0) {
                errmsg = "構造体の配列の初期化はまだできません";
                return -1;
            }
            return parse_array_init(name_buf, name_len, size, cols,
                                    esize, eet, strct, plevel);
        }
        if (size < 0) {
            errmsg = "配列の大きさが必要です (省略できるのは初期化子があるときだけです)";
            return -1;
        }
        if (!expect_char(';', "配列の宣言の終わりには ; が必要です"))
            return -1;
        int a = define_array_s(name_buf, name_len, size, cols,
                               esize, eet, strct, plevel);
        if (a < 0)
            return -1;
        arr_fsig[a] = (char)fsig;
        return new_node(NODE_DECL, a, 0, 0);
    }
    if (*cur == '=') {
        cur++;
        skip_spaces();
        if (strct >= 0 && plevel == 0 && *cur == '{') {
            /* 構造体の初期化子 (v45): ローカルだけ。
             * メンバへの代入文の並びに展開する。変数を
             * 先に名簿に載せるのは、展開がその変数を
             * 基底として使うため (式の形の初期化と違い、
             * 初期化子の中で自分の名前が見える) */
            if (parse_cur_func < 0) {
                errmsg = "ファイル範囲の構造体の初期化はまだできません";
                return -1;
            }
            int nslots = (stag_size[strct] + 7) / 8;
            int var = define_var_full(name_buf, name_len, bsize,
                                      plevel, 1, etype, strct,
                                      nslots);
            if (var < 0)
                return -1;
            var_fsig[var] = (char)fsig;
            int head = new_node(NODE_VDECL, var, 0, 0);
            if (head < 0)
                return -1;
            int base = new_node(NODE_VAR, var, 0, 0);
            if (base < 0)
                return -1;
            if (parse_struct_init(base, strct, head) < 0)
                return -1;
            if (!expect_char(';', "宣言の終わりには ; が必要です"))
                return -1;
            return head;
        }
        if (strct >= 0 && plevel == 0 && parse_cur_func < 0) {
            /* ファイル範囲の構造体の初期化は、初期値を
             * リンクで運ぶ口が無いため見送り (v45) */
            errmsg = "ファイル範囲の構造体の初期化はまだできません";
            return -1;
        }
        /* 初期化つき宣言: 式を読んでから名簿に載せる
         * (代入文と同じ理由。初期化の式の中で自分は使えない) */
        int expr = parse_expr();
        if (expr < 0)
            return -1;
        /* 構造体の値は大きさの分のスロットを占める (v37) */
        int nslots = 1;
        if (strct >= 0 && plevel == 0)
            nslots = (stag_size[strct] + 7) / 8;
        int var = define_var_full(name_buf, name_len, bsize, plevel, 1,
                                  etype, strct, nslots);
        if (var < 0)
            return -1;
        var_fsig[var] = (char)fsig;
        if (!expect_char(';', "宣言の終わりには ; が必要です"))
            return -1;
        if (parse_cur_func < 0) {
            /* ファイル範囲の初期化 (v24): 値は実行コード
             * ではなく、静的領域の初期イメージに埋める。
             * 埋められるのは定数だけ (C の作法と同じ)。
             * ノードは「何もしない」宣言として返す */
            long v;
            int kind = eval_const(expr, &v);
            if (kind == 0) {
                errmsg = "ファイル範囲の初期化子は定数 (数や文字列) だけです";
                return -1;
            }
            if (kind == 3) {
                /* 関数の住所 (v44): 置けるのは関数ポインタの
                 * 変数に、署名の合う関数だけ */
                if (fsig == 0 || func_sig[v] + 1 != fsig) {
                    errmsg = "関数ポインタの型 (署名) が合いません";
                    return -1;
                }
            } else if (fsig != 0 && !(kind == 1 && v == 0)) {
                errmsg = "関数ポインタの初期化子は関数名か 0 だけです";
                return -1;
            }
            if (var_init_kind[var] != 0) {
                /* 仮定義の後に初期値が来るのはよいが、
                 * 初期値が二つは食い違い (v33) */
                errmsg = "初期値は一つだけです";
                return -1;
            }
            var_init_kind[var] = kind;
            var_init_val[var] = v;
            has_var_inits = 1;
            return new_node(NODE_VDECL, var, 0, 0);
        }
        return new_node(NODE_ASSIGN, var, expr, 0);
    }
    if (*cur == ';') {
        cur++;
        /* 構造体の値は大きさの分のスロットを占める (v37) */
        int nslots = 1;
        if (strct >= 0 && plevel == 0)
            nslots = (stag_size[strct] + 7) / 8;
        int var = define_var_full(name_buf, name_len, bsize, plevel, 1,
                                  etype, strct, nslots);
        if (var < 0)
            return -1;
        var_fsig[var] = (char)fsig;
        return new_node(NODE_VDECL, var, 0, 0);
    }
    errmsg = "宣言は 型 名前 ; か 型 名前 = 式 ; の形です";
    return -1;
}
