/* ================================================================
 * 第2層の部品: インライン化 (v58)
 *
 * 小さい関数の呼び出しを、呼び出し側へ本体を複製して
 * 広げる計画と生成 (設計は doc/inline.md)。03-gen.c の
 * func_reg_plan / gen_expr と、03-genstmt.c の
 * gen_program / return 文から使われる。v49 で 03-gen を
 * 分けたときと同じ理由で独立の断片にした (03-gen の
 * 翻訳単位が容量の上限に届いたため)
 * ================================================================ */

/* ================================================================
 * インライン化 (v58、設計は doc/inline.md)
 *
 * 小さい関数の呼び出しを、呼び出し側へ本体を複製して
 * 広げる。計画は func_reg_plan の頭で関数ごとに1回だけ
 * 走り (inline_plan)、対象の呼び出し (NODE_CALL) ごとに:
 *   1. 被呼び側の引数・ローカル変数を、呼び出し側の
 *      新しい変数として変数表へ足す (先頭は戻り値用)
 *   2. 本体 AST を複製し、変数参照を 1. へ付け替える
 *   3. 複製本体がさらに呼び出しを含む場合、深さ 2
 *      まで入れ子に展開する (mulp → sat32 がこれ)
 * gen 側は gen_expr の NODE_CALL と gen_stmt の
 * NODE_RETURN の2か所だけが変わる。複製変数は呼び出し
 * 側の普通のローカル変数なので、変数の読み書きの
 * 経路は一切触らない。
 *
 * 対象の条件 (外れたら従来の呼び出しに落ちる):
 *   本体のある A 方式の関数で、double・構造体の値・
 *   ローカル配列・関数ポインタ引数を持たず、本体が
 *   ノード 64 以下・文 12 以下・ローカル枠 32 以下、
 *   関数ポインタ経由の呼び出しを含まず、再帰でない
 *
 * 上限は断片プロファイルとのオブジェクト一致のため
 * 絶対値で決める (プロファイルで書き換えない):
 *   ノードは 9216、変数は 640 までで展開を止める。
 *   これは断片プロファイルの表の上限に合わせた値で、
 *   標準・断片のどちらの iwac でも同じ判断になる。
 *   変数表そのものの上限 (プロファイルごとに違う)
 *   は var_table_full で別に守る
 * ================================================================ */

static int inl_node[288];   /* 展開する呼び出しノード */
static int inl_base[288];   /* 複製変数の先頭 (引数・
                             * ローカルをスロット順) */
static int inl_result[288]; /* 戻り値変数の番号 */
static int inl_body[288];   /* 複製した本体の先頭 */
static int inl_ninst;       /* 展開の数 */
/* 展開の上限はファイルごと (呼び出し側の定義
 * ファイル): 一括コンパイルで前のファイルが
 * 上限を使い切ると、後ろのファイルの展開が
 * 分割コンパイルと食い違うため */
static int inl_file_insts[128];
/* 変数予算のファイルごとの数え置き: 元の変数は
 * ファイルごと (inl_file_orig)、複製変数も
 * ファイルごとの実績 (inl_file_cvars)。予算は
 * 「そのファイルの元の変数 + そのファイルの
 * 複製変数 + 共有の土台」で数える。土台は
 * ファイル0に本体が無いとき (decls.c のような
 * 宣言だけのファイル) の元の変数で、どの TU
 * にも必ず含まれる分。一括コンパイルで他の
 * ファイルの分が混ざると、分割コンパイルと
 * 展開の判断が食い違うため */
static int inl_file_orig[128];
static int inl_file_cvars[128];
static int inl_shared_base;
static int inl_prefix_done;
static int inl_bind_inst[128]; /* 引数の直結: 展開の番号 */
static char inl_bind_idx[128]; /* 直結した引数の番号 */
static int inl_bind_var[128];  /* 直結先の変数 */
static int inl_nbind;         /* 直結の数 */
static char inl_pwrite[288];  /* 関数ごとに、書き換え
                             * られる引数の印 (ビット) */
static int inl_m_func;      /* 計測中の関数 */
static char inl_bind_mask;  /* 次の展開の引数直結の印 */
static int inl_bind_vars[8]; /* 直結先の変数 (引数ごと) */
static int inl_next_override; /* 次の展開の戻り値変数の
                             * 上書き (-1 で通常) */
static int inl_cur_result[3]; /* 深さごとの、複製中の
                             * 展開の戻り値変数 */
static char inl_ok[288];    /* 被呼び側の適格: 0 未判定、
                             * 1 不可、2 可 */
static char inl_done[288];  /* 呼び出し側の計画済み */
static char inl_cyclic[288]; /* 再帰: 0 未判定、1 非、2 環 */
static char inl_snap[288];  /* ローカル枠数の控え済み */
static int inl_orig_nlocals[288]; /* 計画前のローカル枠数 */
static char inl_bnodes[288]; /* 本体のノード数 (255 飽和) */
static char inl_seen[288];  /* 再帰判定の訪問印 */
static int inl_caller;      /* 計画中の呼び出し側 */
static int inl_used;        /* 呼び出し側ごとの複製ノード数 */
static int inl_usedarr[288]; /* 呼び出し側ごとの複製
                             * ノード数 (2段階の計画で引き継ぐ) */
static int inl_phase;       /* 計画の段階: 1 ループの中の
                             * 呼び出し先行、2 残り */
static int inl_ldepth;      /* 歩行中: ループの深さ */
static int inl_all_done;    /* 一括計画済み */
static int inl_mode;        /* 歩行の用途: 1 計測、2 呼び出し
                             * 探し、3 再帰判定 */
static int inl_m_nodes;     /* 計測: ノード数 */
static int inl_m_stmts;     /* 計測: 文の数 */
static int inl_m_icall;     /* 計測: ICALL を含むか */
static int inl_target;      /* 再帰判定: 探す関数 */
static int inl_found;       /* 再帰判定: 届いたか */
static int clone_fail;      /* 複製が上限で止まった */
static int map_src[3][40];  /* 変数の付け替え表 (深さごと) */
static int map_dst[3][40];
static char map_n[3];
static int inl_depth;       /* 生成中: 展開の深さ */
static int inl_ret_var[16]; /* 生成中: 戻り値変数 */
static int inl_patch_head[16]; /* 生成中: 終端への跳躍 */

static int inl_create(int node, int g, int depth,
                      int result_override);
static int inl_eligible(int g);
static int inline_find(int node);
static int call_b_mode(int sig);
static void apply_patches(int head, int target);
static int gen_value_as(int node, int dst);
static int gen_stmt(int node);
static void type_list(int head);
static void gen_load_var(int reg, int var);
static void gen_store_var(int var, int reg, int value_et);
static void emit_canon(int et);

/* 関数 f のローカル枠数を、計画で増える前に控える。
 * 被呼び側の変数を数えるときはこの控えを使う
 * (計画済みの関数は自分の複製変数で枠が増えている) */
static void inl_snapshot(int f) {
    if (f < 0 || inl_snap[f])
        return;
    inl_snap[f] = 1;
    inl_orig_nlocals[f] = func_nlocals[f];
}

/* 文の種類か (文の数を数えるため) */
static int inl_is_stmt(int k) {
    return k == NODE_ASSIGN || k == NODE_PUTC ||
           k == NODE_WHILE || k == NODE_CALL ||
           k == NODE_IF || k == NODE_STORE ||
           k == NODE_INDEX_STORE || k == NODE_FOR ||
           k == NODE_BREAK || k == NODE_CONTINUE ||
           k == NODE_RETURN || k == NODE_FORSTEP ||
           k == NODE_VDECL || k == NODE_DECL ||
           k == NODE_ELEM_STORE || k == NODE_SWITCH ||
           k == NODE_DOWHILE;
}

static void inl_walk_chain(int head);

/* ノード n とその構造的な子を辿る。連鎖 (node_next)
 * は inl_walk_chain が辿る。子の欄の意味は種類ごとに
 * 違う (02-parse の先頭の表が正本。使わない欄には
 * 0 が入っていて、ノード 0 は本物の葉なので、
 * 決まった欄だけを辿る) */
static void inl_walk(int n) {
    if (n < 0)
        return;
    int k = node_kind[n];
    if (inl_mode == 1) {
        inl_m_nodes++;
        if (inl_is_stmt(k))
            inl_m_stmts++;
        if (k == NODE_ICALL)
            inl_m_icall = 1;
        /* 書き換えられる引数の印 (引数の直結の
         * 可否に使う。住所を取られる場合も含む) */
        if ((k == NODE_ASSIGN || k == NODE_PREINC ||
             k == NODE_POSTINC || k == NODE_PREDEC ||
             k == NODE_POSTDEC) &&
            var_owner[node_val[n]] == inl_m_func &&
            var_slot[node_val[n]] <
                func_nparams[inl_m_func])
            inl_pwrite[inl_m_func] =
                inl_pwrite[inl_m_func] |
                (1 << var_slot[node_val[n]]);
        if (k == NODE_ADDR && node_lhs[n] >= 0 &&
            node_kind[node_lhs[n]] == NODE_VAR &&
            var_owner[node_val[node_lhs[n]]] ==
                inl_m_func &&
            var_slot[node_val[node_lhs[n]]] <
                func_nparams[inl_m_func])
            inl_pwrite[inl_m_func] =
                inl_pwrite[inl_m_func] |
                (1 << var_slot[node_val[node_lhs[n]]]);
    }
    if (k == NODE_CALL) {
        if (inl_mode == 2) {
            /* 段階1はループの中の呼び出しだけを
             * 展開する (予算を熱い場所へ先に回す) */
            if ((inl_phase == 2 || inl_ldepth > 0) &&
                inline_find(n) < 0)
                inl_create(n, node_val[n], 0, -1);
        } else if (inl_mode == 3) {
            int g = node_val[n];
            if (g == inl_target) {
                inl_found = 1;
            } else if (g >= 0 && g < nfunc &&
                       !inl_seen[g] && func_body[g] >= 0) {
                inl_seen[g] = 1;
                inl_walk_chain(func_body[g]);
            }
        }
    }
    if (k == NODE_WHILE || k == NODE_DOWHILE) {
        inl_walk(node_lhs[n]);
        inl_ldepth++;
        inl_walk_chain(node_rhs[n]);
        inl_ldepth--;
    } else if (k == NODE_IF) {
        inl_walk(node_lhs[n]);
        inl_walk_chain(node_rhs[n]);
        if (node_val[n] >= 0)
            inl_walk_chain(node_val[n]);
    } else if (k == NODE_FOR) {
        if (node_val[n] >= 0)
            inl_walk_chain(node_val[n]);
        inl_walk(node_lhs[n]);
        inl_ldepth++;
        inl_walk_chain(node_rhs[n]);
        inl_ldepth--;
    } else if (k == NODE_FORSTEP) {
        inl_walk(node_val[n]);
    } else if (k == NODE_SWITCH) {
        inl_walk(node_lhs[n]);
        inl_walk_chain(node_rhs[n]);
    } else if (k == NODE_CASE) {
        inl_walk_chain(node_lhs[n]);
    } else if (k == NODE_CALL || k == NODE_SYSCALL3) {
        inl_walk_chain(node_lhs[n]);
    } else if (k == NODE_ICALL) {
        inl_walk(node_lhs[n]);
        inl_walk_chain(node_rhs[n]);
    } else if (k == NODE_COND) {
        inl_walk(node_lhs[n]);
        inl_walk(node_val[n]);
        inl_walk(node_rhs[n]);
    } else if (k == NODE_MEMBER) {
        inl_walk(node_lhs[n]);
    } else if (k == NODE_MELEM || k == NODE_INDEX2 ||
               k == NODE_ELEM_STORE || k == NODE_STORE ||
               k == NODE_INDEX_STORE || k == NODE_ADD ||
               k == NODE_SUB || k == NODE_MUL ||
               k == NODE_DIV || k == NODE_EQ ||
               k == NODE_NE || k == NODE_AND ||
               k == NODE_OR || k == NODE_XOR ||
               k == NODE_MOD || k == NODE_LT ||
               k == NODE_GT || k == NODE_LE ||
               k == NODE_GE || k == NODE_SHL ||
               k == NODE_SHR || k == NODE_LAND ||
               k == NODE_LOR) {
        inl_walk(node_lhs[n]);
        inl_walk(node_rhs[n]);
    } else if (k == NODE_INDEX || k == NODE_ROWADDR ||
               k == NODE_PINDEX) {
        inl_walk(node_rhs[n]);
    } else if (k == NODE_DEREF || k == NODE_NOT ||
               k == NODE_NEG || k == NODE_BITNOT ||
               k == NODE_CAST || k == NODE_ADDR ||
               k == NODE_ASSIGN || k == NODE_RETURN ||
               k == NODE_PUTC || k == NODE_EPRE ||
               k == NODE_EPOST) {
        inl_walk(node_lhs[n]);
    }
    /* それ以外は葉 (子なし) */
}

static void inl_walk_chain(int head) {
    for (int n = head; n != -1; n = node_next[n])
        inl_walk(n);
}

/* 関数 g が呼び出しを辿って自分自身へ戻るか
 * (直接・相互の再帰)。戻る関数は展開しない */
static int inl_is_cyclic(int g) {
    if (inl_cyclic[g] != 0)
        return inl_cyclic[g] == 2;
    int saved = inl_mode;
    for (int i = 0; i < nfunc; i++)
        inl_seen[i] = 0;
    inl_mode = 3;
    inl_target = g;
    inl_found = 0;
    inl_seen[g] = 1;
    inl_walk_chain(func_body[g]);
    inl_mode = saved;
    if (inl_found)
        inl_cyclic[g] = 2;
    else
        inl_cyclic[g] = 1;
    return inl_found;
}

/* 関数 g が展開の対象になるか (doc/inline.md の
 * 条件を全部を満たすときだけ 1)。結果は控える */
static int inl_eligible(int g) {
    if (g < 0 || g >= nfunc)
        return 0;
    if (inl_ok[g] != 0)
        return inl_ok[g] == 2;
    inl_ok[g] = 1;  /* 判定の途中も不可 (再入に備える) */
    int saved = inl_mode;
    int ok = 1;
    if (!func_has_body[g] || func_body[g] < 0)
        ok = 0;
    int sig = func_sig[g];
    if (ok && call_b_mode(sig))
        ok = 0;  /* 構造体が絡む B 方式は対象外 */
    if (ok && sig_ret_struct[sig] > 0)
        ok = 0;
    if (ok && (func_ret_etype[g] & 64))
        ok = 0;  /* double の戻り値は対象外 */
    if (ok) {
        inl_snapshot(g);
        if (inl_orig_nlocals[g] > 32)
            ok = 0;
    }
    if (ok) {
        /* 引数: 関数ポインタ・double・構造体の値は不可 */
        for (int k = 0; k < func_nparams[g]; k++) {
            int v = func_param_var[g][k];
            if (var_fsig[v] != 0 || (var_etype[v] & 64) ||
                (var_struct[v] > 0 && var_plevel[v] == 0))
                ok = 0;
        }
    }
    if (ok) {
        /* ローカル: double・構造体の値は不可 */
        int nloc = inl_orig_nlocals[g];
        for (int v = 0; v < nvar; v++) {
            if (var_owner[v] != g || var_slot[v] >= nloc)
                continue;
            if ((var_etype[v] & 64) ||
                (var_struct[v] > 0 && var_plevel[v] == 0))
                ok = 0;
        }
    }
    if (ok) {
        /* ローカル配列がある関数は対象外 (配列の
         * 置き場所は変数表と別の仕組みのため) */
        for (int a = 0; a < narr; a++) {
            if (arr_owner[a] == g)
                ok = 0;
        }
    }
    if (ok) {
        /* 本体の大きさと、関数ポインタ経由の
         * 呼び出しの有無を測る。ついでに書き換え
         * られる引数の印も付ける */
        inl_mode = 1;
        inl_m_func = g;
        inl_m_nodes = 0;
        inl_m_stmts = 0;
        inl_m_icall = 0;
        inl_pwrite[g] = 0;
        inl_walk_chain(func_body[g]);
        if (inl_m_nodes > 255)
            inl_bnodes[g] = 255;
        else
            inl_bnodes[g] = inl_m_nodes;
        if (inl_m_nodes > 64 || inl_m_stmts > 12 ||
            inl_m_icall)
            ok = 0;
    }
    if (ok && inl_is_cyclic(g))
        ok = 0;
    inl_mode = saved;
    if (ok)
        inl_ok[g] = 2;
    return ok;
}

/* 呼び出し側 (inl_caller) の変数を1つ足し、番号を
 * 返す。src >= 0 ならその変数の写し、src < 0 なら
 * 戻り値用 (型は ret_et、段数は ret_pl)。
 * 変数表が一杯なら -1 (プロファイルごとの上限は
 * var_table_full が守る) */
static int inl_add_var(int src, int ret_et, int ret_pl) {
    if (var_table_full())
        return -1;
    int i = nvar++;
    if (src >= 0) {
        for (int j = 0; j < 32; j++)
            var_names[i][j] = var_names[src][j];
        var_name_len[i] = var_name_len[src];
        var_bsize[i] = var_bsize[src];
        var_etype[i] = var_etype[src];
        var_struct[i] = var_struct[src];
        var_plevel[i] = var_plevel[src];
        var_fsig[i] = var_fsig[src];
        var_addrtaken[i] = var_addrtaken[src];
    } else {
        var_names[i][0] = 'i';
        var_names[i][1] = 'n';
        var_names[i][2] = 'l';
        for (int j = 3; j < 32; j++)
            var_names[i][j] = 0;
        var_name_len[i] = 3;
        if ((ret_et & 15) == 1)
            var_bsize[i] = 1;
        else
            var_bsize[i] = 8;
        var_etype[i] = ret_et;
        var_struct[i] = 0;
        var_plevel[i] = ret_pl;
        var_fsig[i] = 0;
        var_addrtaken[i] = 0;
    }
    var_owner[i] = inl_caller;
    var_file[i] = func_file[inl_caller];
    inl_file_cvars[func_file[inl_caller]]++;
    var_slot[i] = func_nlocals[inl_caller];
    func_nlocals[inl_caller] = func_nlocals[inl_caller] + 1;
    var_init_kind[i] = 0;
    var_reg[i] = 0;
    var_refcount[i] = 0;
    return i;
}

/* 付け替え表 (深さごと) で被呼び側の変数を探す */
static int map_find(int depth, int src) {
    for (int i = 0; i < map_n[depth]; i++) {
        if (map_src[depth][i] == src)
            return map_dst[depth][i];
    }
    return -1;
}

static int clone_chain(int head, int depth);

/* ノード n を複製する (連鎖は clone_chain)。
 * 変数参照は深さ depth の付け替え表で直し、
 * 子の欄は inl_walk と同じ種類ごとの意味で辿る。
 * 複製した呼び出しは、深さが許せば入れ子に展開
 * する (inl_create)。上限で止まったら clone_fail */
static int clone_node(int n, int depth) {
    if (clone_fail)
        return -1;
    int k = node_kind[n];
    int val = node_val[n];
    int lhs = node_lhs[n];
    int rhs = node_rhs[n];
    if (k == NODE_VAR || k == NODE_ASSIGN ||
        k == NODE_PREINC || k == NODE_POSTINC ||
        k == NODE_PREDEC || k == NODE_POSTDEC ||
        k == NODE_PINDEX || k == NODE_VDECL) {
        int mv = map_find(depth, val);
        if (mv >= 0)
            val = mv;
    }
    if (k == NODE_WHILE || k == NODE_DOWHILE) {
        lhs = clone_node(lhs, depth);
        rhs = clone_chain(rhs, depth);
    } else if (k == NODE_IF) {
        lhs = clone_node(lhs, depth);
        rhs = clone_chain(rhs, depth);
        if (val >= 0)
            val = clone_chain(val, depth);
    } else if (k == NODE_FOR) {
        if (val >= 0)
            val = clone_chain(val, depth);
        lhs = clone_node(lhs, depth);
        rhs = clone_chain(rhs, depth);
    } else if (k == NODE_FORSTEP) {
        val = clone_node(val, depth);
    } else if (k == NODE_SWITCH) {
        lhs = clone_node(lhs, depth);
        rhs = clone_chain(rhs, depth);
    } else if (k == NODE_CASE) {
        lhs = clone_chain(lhs, depth);
    } else if (k == NODE_CALL || k == NODE_SYSCALL3) {
        lhs = clone_chain(lhs, depth);
    } else if (k == NODE_ICALL) {
        lhs = clone_node(lhs, depth);
        rhs = clone_chain(rhs, depth);
    } else if (k == NODE_COND) {
        lhs = clone_node(lhs, depth);
        val = clone_node(val, depth);
        rhs = clone_node(rhs, depth);
    } else if (k == NODE_MEMBER) {
        lhs = clone_node(lhs, depth);
    } else if (k == NODE_MELEM || k == NODE_INDEX2 ||
               k == NODE_ELEM_STORE || k == NODE_STORE ||
               k == NODE_INDEX_STORE || k == NODE_ADD ||
               k == NODE_SUB || k == NODE_MUL ||
               k == NODE_DIV || k == NODE_EQ ||
               k == NODE_NE || k == NODE_AND ||
               k == NODE_OR || k == NODE_XOR ||
               k == NODE_MOD || k == NODE_LT ||
               k == NODE_GT || k == NODE_LE ||
               k == NODE_GE || k == NODE_SHL ||
               k == NODE_SHR || k == NODE_LAND ||
               k == NODE_LOR) {
        lhs = clone_node(lhs, depth);
        rhs = clone_node(rhs, depth);
    } else if (k == NODE_INDEX || k == NODE_ROWADDR ||
               k == NODE_PINDEX) {
        rhs = clone_node(rhs, depth);
    } else if (k == NODE_RETURN) {
        /* return f(...) の形なら、f の戻り値変数を
         * この展開の戻り値変数へ直結する (転送) */
        if (node_lhs[n] >= 0 &&
            node_kind[node_lhs[n]] == NODE_CALL)
            inl_next_override = inl_cur_result[depth];
        lhs = clone_node(lhs, depth);
        inl_next_override = -1;
    } else if (k == NODE_DEREF || k == NODE_NOT ||
               k == NODE_NEG || k == NODE_BITNOT ||
               k == NODE_CAST || k == NODE_ADDR ||
               k == NODE_ASSIGN ||
               k == NODE_PUTC || k == NODE_EPRE ||
               k == NODE_EPOST) {
        lhs = clone_node(lhs, depth);
    }
    if (clone_fail)
        return -1;
    int m = new_node(k, val, lhs, rhs);
    if (m < 0) {
        clone_fail = 1;
        return -1;
    }
    if (k == NODE_CALL && depth < 2) {
        /* 入れ子の展開。引数が単なる変数読みで、
         * 被呼び側がその引数を書き換えず、型タグも
         * 同じなら、引数変数をその変数へ直結する
         * (複製変数を作らない)。失敗しても外側は
         * 止めない (呼び出しは通常のまま残る) */
        int g = node_val[n];
        inl_bind_mask = 0;
        if (inl_eligible(g)) {
            int ai = 0;
            for (int a = lhs; a != -1 && ai < 8 &&
                 ai < func_nparams[g];
                 a = node_next[a]) {
                if (node_kind[a] == NODE_VAR &&
                    !(inl_pwrite[g] & (1 << ai)) &&
                    var_etype[node_val[a]] ==
                        var_etype[func_param_var[g][ai]]) {
                    inl_bind_mask |= 1 << ai;
                    inl_bind_vars[ai] = node_val[a];
                }
                ai++;
            }
        }
        inl_create(m, g, depth + 1, inl_next_override);
        inl_next_override = -1;
        inl_bind_mask = 0;
        clone_fail = 0;
    }
    return m;
}

static int clone_chain(int head, int depth) {
    int first = -1;
    int prev = -1;
    for (int n = head; n != -1; n = node_next[n]) {
        int m = clone_node(n, depth);
        if (clone_fail)
            return -1;
        if (first < 0)
            first = m;
        else
            node_next[prev] = m;
        prev = m;
    }
    return first;
}

/* 呼び出しノード node (被呼び側 g) の展開を1つ作る。
 * 深さ depth は 0 が呼び出し側の本体、以下入れ子。
 * result_override >= 0 なら戻り値変数は作らずその
 * 変数を使う (return f(...) の転送)。引数の直結は
 * inl_bind_mask / inl_bind_vars で受け取る。
 * 作れたら展開の番号、諦めたら -1 (呼び出しは
 * 通常の呼び出しのまま) */
static int inl_create(int node, int g, int depth,
                      int result_override) {
    /* ファイルをまたぐ呼び出しは展開しない:
     * 分割コンパイル (被呼び側の本体が無い TU) と
     * 一括で生成が食い違ってしまうため (v32 の
     * 物差し)。実用上も、別ファイルの関数は
     * リンカ越しの呼び出しに残るのが普通の形 */
    if (func_file[g] != func_file[inl_caller])
        return -1;
    if (inl_file_insts[func_file[inl_caller]] >= 160)
        return -1;
    if (inl_ninst >= 288)
        return -1;
    char bmask = inl_bind_mask;
    int bvars[8];
    for (int i = 0; i < 8; i++)
        bvars[i] = inl_bind_vars[i];
    int saved = inl_mode;
    int ok = inl_eligible(g);
    inl_mode = saved;
    if (!ok)
        return -1;
    inl_snapshot(g);
    int nloc = inl_orig_nlocals[g];
    int vend = nvar;
    int cnt = 0;
    for (int v = 0; v < vend; v++) {
        if (var_owner[v] == g && var_slot[v] < nloc)
            cnt++;
    }
    if (cnt > 40)
        return -1;  /* 付け替え表に収まらない */
    {
        /* 変数の予算はファイルごと (物理的な表の
         * 上限は inl_add_var の var_table_full が
         * 守る) */
        int cf = func_file[inl_caller];
        int eff = inl_file_orig[cf] + inl_file_cvars[cf];
        if (cf != 0)
            eff = eff + inl_shared_base;
        if (eff + cnt + 1 > 640)
            return -1;
    }
    if (nnode + inl_bnodes[g] > 9216)
        return -1;  /* ノードの絶対上限 */
    if (inl_used + inl_bnodes[g] > 2048)
        return -1;  /* 呼び出し側ごとの予算 */
    /* 展開の番号を先に押さえる (入れ子の展開が
     * 後から記録されるため) */
    int k = inl_ninst++;
    inl_file_insts[func_file[inl_caller]]++;
    inl_node[k] = node;
    inl_base[k] = nvar;
    map_n[depth] = 0;
    int vfail = 0;
    for (int v = 0; v < vend; v++) {
        if (var_owner[v] != g || var_slot[v] >= nloc)
            continue;
        int slot = var_slot[v];
        if (slot < func_nparams[g] && slot < 8 &&
            (bmask & (1 << slot)) &&
            inl_nbind < 128) {
            /* 引数の直結: 複製変数は作らず、本体の
             * 参照を直結先の変数へ付け替える */
            inl_bind_inst[inl_nbind] = k;
            inl_bind_idx[inl_nbind] = slot;
            inl_bind_var[inl_nbind] = bvars[slot];
            inl_nbind++;
            map_src[depth][map_n[depth]] = v;
            map_dst[depth][map_n[depth]] = bvars[slot];
            map_n[depth]++;
            continue;
        }
        int nv = inl_add_var(v, 0, 0);
        if (nv < 0) {
            vfail = 1;
            break;
        }
        map_src[depth][map_n[depth]] = v;
        map_dst[depth][map_n[depth]] = nv;
        map_n[depth]++;
    }
    if (vfail) {
        inl_node[k] = -1;
        return -1;
    }
    if (result_override >= 0) {
        inl_result[k] = result_override;
    } else {
        inl_result[k] = inl_add_var(-1, func_ret_etype[g],
                                    sig_ret_plevel[func_sig[g]]);
        if (inl_result[k] < 0) {
            inl_node[k] = -1;
            return -1;
        }
    }
    inl_cur_result[depth] = inl_result[k];
    int before = nnode;
    clone_fail = 0;
    int body = clone_chain(func_body[g], depth);
    inl_used = inl_used + (nnode - before);
    if (clone_fail) {
        /* 上限で止まった: 展開は無効にする。
         * new_node が置いたエラー文は消す */
        inl_node[k] = -1;
        errmsg = 0;
        return -1;
    }
    inl_body[k] = body;
    return k;
}

/* 関数 f の計画を1段階ぶん走らせる */
static void inl_plan_func(int f, int ph) {
    inl_snapshot(f);
    inl_caller = f;
    if (ph == 1)
        inl_used = 0;
    else
        inl_used = inl_usedarr[f];
    inl_next_override = -1;
    inl_bind_mask = 0;
    inl_phase = ph;
    inl_mode = 2;
    inl_walk_chain(func_body[f]);
    inl_usedarr[f] = inl_used;
    inl_done[f] = 1;
}

/* インライン化の一括計画: 全関数について、段階1
 * (ループの中の呼び出し) → 段階2 (残り) の順に
 * 展開を作る。gen_program の頭で1回だけ走る。
 * 予算 (変数・ノード) は全関数で共有なので、
 * 熱い場所の呼び出しが先に予算を使える */
/* 元の変数をファイルごとに数え、共有の土台を
 * 決める (計画の前に1回だけ。複製変数が混ざる
 * 前に呼ぶこと) */
static void inl_compute_prefix(void) {
    if (inl_prefix_done)
        return;
    inl_prefix_done = 1;
    for (int f = 0; f < 128; f++)
        inl_file_orig[f] = 0;
    for (int v = 0; v < nvar; v++) {
        int vf = var_file[v];
        if (vf < 0)
            vf = 0;
        if (vf > 120)
            vf = 120;
        inl_file_orig[vf]++;
    }
    /* ファイル0に本体のある関数が無ければ、その
     * 変数は宣言だけのファイル (decls.c 等) の
     * 分として、土台に数える */
    inl_shared_base = 0;
    int f0_body = 0;
    for (int f = 0; f < nfunc; f++) {
        if (func_body[f] >= 0 && func_file[f] == 0)
            f0_body = 1;
    }
    if (!f0_body)
        inl_shared_base = inl_file_orig[0];
}

static void inline_plan_all(void) {
    if (inl_all_done)
        return;
    inl_all_done = 1;
    inl_compute_prefix();
    for (int ph = 1; ph <= 2; ph++) {
        for (int f = 0; f < nfunc; f++) {
            if (func_body[f] >= 0)
                inl_plan_func(f, ph);
        }
    }
}

/* 関数 f のインライン化の計画。通常は
 * inline_plan_all で済んでいる。ここは一括計画を
 * 経ない経路のための単体計画 (func_reg_plan の
 * 頭で呼ぶ: 複製変数の分だけ func_nlocals が
 * 増えるので、フレームが決まる前に走らせる) */
static void inline_plan(int f) {
    if (f < 0 || inl_done[f])
        return;
    inl_done[f] = 1;
    inl_compute_prefix();
    if (func_body[f] < 0)
        return;
    inl_plan_func(f, 1);
    inl_plan_func(f, 2);
}

/* 呼び出しノードの展開を探す (無ければ -1)。
 * 生成中の展開が深すぎるとき (引数の中の呼び出しが
 * 重なったとき) も -1: 通常の呼び出しに落とす */
static int inline_find(int node) {
    if (inl_depth >= 16)
        return -1;
    for (int i = 0; i < inl_ninst; i++) {
        if (inl_node[i] == node)
            return i;
    }
    return -1;
}

/* 展開 ie の引数 ai が直結されていれば、その
 * 変数を返す (無ければ -1) */
static int inl_bound_var(int ie, int ai) {
    for (int b = 0; b < inl_nbind; b++) {
        if (inl_bind_inst[b] == ie &&
            inl_bind_idx[b] == ai)
            return inl_bind_var[b];
    }
    return -1;
}

/* 展開した呼び出しの生成: 引数を左から順に評価して
 * 引数変数へ書き、複製本体を文として生成し、
 * 戻り値変数を t1 へ読む。return 文は gen_stmt 側が
 * 受け口 (inl_ret_var / inl_patch_head) を見て
 * 戻り値変数への書き込みと終端への跳躍に変える。
 * 直結された引数は、評価も書き込みも要らない
 * (引数は単なる変数読みで、本体が直接その変数を
 * 見る)。引数変数は複製のスロット順で、直結された
 * 分は詰めて並ぶ */
static int gen_inline_call(int node, int ie) {
    int g = node_val[node];
    int cursor = inl_base[ie];
    int ai = 0;
    for (int a = node_lhs[node]; a != -1; a = node_next[a]) {
        if (inl_bound_var(ie, ai) >= 0) {
            ai++;
            continue;
        }
        int pv = cursor;
        cursor++;
        if (!gen_value_as(a, var_etype[pv]))
            return 0;
        gen_store_var(pv, REG_T1, node_etype[a]);
        ai++;
    }
    inl_ret_var[inl_depth] = inl_result[ie];
    inl_patch_head[inl_depth] = -1;
    inl_depth++;
    type_list(inl_body[ie]);
    int ok = 1;
    for (int s = inl_body[ie]; s != -1; s = node_next[s]) {
        if (!gen_stmt(s)) {
            ok = 0;
            break;
        }
    }
    inl_depth--;
    if (!ok)
        return 0;
    apply_patches(inl_patch_head[inl_depth], nasm);
    gen_load_var(REG_T1, inl_result[ie]);
    /* 戻り値の正規化は通常の呼び出しと同じ */
    if (sig_ret_plevel[func_sig[g]] == 0 &&
        sig_ret_struct[func_sig[g]] == 0)
        emit_canon(func_ret_etype[g]);
    return 1;
}

