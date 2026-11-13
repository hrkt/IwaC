/* ================================================================
 * 第1層 parse: ソース文字列 → AST
 *
 * AST (抽象構文木) とは、プログラムの意味の構造を「木」で
 * 表したもの。例えば "65+1" の AST は「+ を根として、
 * 左の子が 65、右の子が 1」という木になる。
 *
 * 木の置き方: ノード (木の部品) を構造体や malloc で作らず、
 * 4本の並行配列で表す。ノード番号 i について:
 *   node_kind[i] : 種類 (NODE_NUM / NODE_ADD / NODE_SUB)
 *   node_val[i]  : NODE_NUM のときの数値
 *   node_lhs[i]  : 演算のときの、左の子のノード番号
 *   node_rhs[i]  : 演算のときの、右の子のノード番号
 * ノードを指す値は「ノード番号」であり、子は番号で繋がる。
 * (並行配列にしたのは、ポインタや構造体といった「まだ
 * iwac の言語に無いもの」を iwac 自身も使わないため)
 * ================================================================ */

static int NODE_NUM = 0;  /* 数 (葉)。node_val = その数値 */
static int NODE_ADD = 1;  /* 足し算 */
static int NODE_SUB = 2;  /* 引き算 */
static int NODE_VAR = 3;  /* 変数 (葉)。node_val = 変数番号 */
static int NODE_STR = 4;  /* 文字列 (葉)。node_val = 文字列番号。
                           * このノードの「値」は文字列の住所 */
static int NODE_DEREF = 5; /* ポインタ読み (葉扱い)。
                            * node_lhs = 住所を表すノード
                            * (変数ノード、または数のノード =
                            * 絶対アドレス読み) */
static int NODE_ASSIGN = 6; /* 代入文。node_val = 変数番号、
                             * node_lhs = 右辺の式 */
static int NODE_PUTC = 7;   /* 出力文。node_lhs = 出す式 */
static int NODE_WHILE = 8;  /* 繰り返し文。node_lhs = 条件の式、
                             * node_rhs = 本体の文の並びの先頭 */
static int NODE_CALL = 9;   /* 関数呼び出し。node_val = 関数番号、
                             * node_lhs = 引数の式の並び (node_next
                             * で繋ぐ。無ければ -1)。文としても
                             * 式としても使える (v22 で式になった) */
static int NODE_EQ = 10;    /* 比較 (==)。結果は 1 か 0 */
static int NODE_NE = 11;    /* 比較 (!=)。結果は 1 か 0 */
static int NODE_AND = 12;   /* ビット演算 (&) */
static int NODE_ARRAY = 17; /* 配列の名前そのもの (葉)。
                             * 「値」は配列の先頭アドレス。
                             * node_val = 配列番号 */
static int NODE_INDEX = 18; /* 配列の要素読み。node_val = 配列番号、
                             * node_rhs = 添字の式 (葉扱いしない) */
static int NODE_DECL = 19;  /* 配列の宣言文。node_val = 配列番号。
                             * 実行時のコードは出ない (置き場所の
                             * 予約だけが意味) */
static int NODE_LT = 21;    /* 関係演算子 (左 < 右)。結果は 1 か 0 */
static int NODE_GT = 22;    /* 関係演算子 (左 > 右)。結果は 1 か 0 */
static int NODE_LE = 23;    /* 関係演算子 (左 <= 右)。結果は 1 か 0 */
static int NODE_GE = 24;    /* 関係演算子 (左 >= 右)。結果は 1 か 0 */
static int NODE_SHL = 25;   /* 左シフト (<<) (v19) */
static int NODE_SHR = 26;   /* 右シフト (>>)。符号付き (算術) (v19) */
static int NODE_OR = 27;    /* ビット OR (|) (v19) */
static int NODE_XOR = 28;   /* ビット XOR (^) (v19) */
static int NODE_MOD = 29;   /* 剰余 (%) (v19) */
static int NODE_NOT = 30;   /* 論理 NOT (!)。0 なら 1、それ以外は 0。
                             * node_lhs = 相手の式 (v19) */
static int NODE_NEG = 31;   /* 単項マイナス (-)。node_lhs = 相手 (v19) */
static int NODE_LAND = 32;  /* 論理 AND (&&)。短絡評価で 1 か 0 (v19) */
static int NODE_LOR = 33;   /* 論理 OR (||)。短絡評価で 1 か 0 (v19) */
static int NODE_PREINC = 34;  /* 前置 ++。node_val = 変数番号 (v19) */
static int NODE_POSTINC = 35; /* 後置 ++。node_val = 変数番号 (v19) */
static int NODE_PREDEC = 36;  /* 前置 --。node_val = 変数番号 (v19) */
static int NODE_POSTDEC = 37; /* 後置 --。node_val = 変数番号 (v19) */
static int NODE_FOR = 38;   /* for 文 (v20)。node_val = 初期化の文
                             * (無ければ -1)、node_lhs = 条件の式
                             * (空なら数の 1)、node_rhs = 本体の
                             * 文の並び (更新があれば、最後に
                             * NODE_FORSTEP で包んで繋ぐ) */
static int NODE_BREAK = 39;    /* break 文 (v20) */
static int NODE_CONTINUE = 40; /* continue 文 (v20) */
static int NODE_RETURN = 41;   /* return 文 (v20)。node_lhs =
                             * 戻り値の式 (値なしなら -1、v22) */
static int NODE_FORSTEP = 42;  /* for の更新部 (v20)。node_val =
                             * 更新の文。for の本体リストの最後に
                             * だけ現れる (continue の飛び先) */
static int NODE_VDECL = 43;   /* 変数の宣言だけの文 (v21)。
                             * node_val = 変数番号。実行時の
                             * コードは出ない (置き場所の予約) */
static int NODE_INDEX2 = 45;  /* 2次元配列の要素読み (v23)。
                             * node_val = 配列番号、node_lhs =
                             * 行の式、node_rhs = 列の式 */
static int NODE_ROWADDR = 46; /* 2次元配列の行の先頭アドレス
                             * (v23)。node_val = 配列番号、
                             * node_rhs = 行の式。値は住所 */
static int NODE_PINDEX = 47;  /* ポインタの添字読み (v23)。
                             * node_val = 変数番号、node_rhs =
                             * 添字の式。p[i] は *(p + i ×
                             * 指す先の大きさ) として読む */
static int NODE_ELEM_STORE = 48; /* 要素への書き込み文 (v23)。
                             * node_lhs = 要素を表す式ノード
                             * (INDEX2 か PINDEX)、node_rhs =
                             * 書く値の式。1次元の配列要素は
                             * 従来の NODE_INDEX_STORE のまま */
static int NODE_EPRE = 49;  /* 要素への前置 ++/-- (v25)。
                             * node_lhs = 要素ノード、node_val =
                             * +1 か -1。値は増減のあとの値 */
static int NODE_EPOST = 50; /* 要素への後置 ++/-- (v25)。
                             * node_lhs = 要素ノード、node_val =
                             * +1 か -1。値は増減のまえの値 */
static int NODE_COND = 51;  /* 三項演算子 (v25)。node_lhs =
                             * 条件、node_val = 成り立つときの式、
                             * node_rhs = そうでないときの式 */
static int NODE_BITNOT = 52; /* ビット反転 ~ (v25)。
                             * node_lhs = 相手 */
static int NODE_SYSCALL3 = 53; /* 組み込み syscall3 (v27)。
                             * node_lhs = 引数の式の並び
                             * (node_next で繋ぐ、4つ固定:
                             * 番号、a0、a1、a2)。KosabaOS の
                             * システムコールを直接呼ぶ式で、
                             * 値は戻り値 (a0) */
static int NODE_CAST = 54;   /* 型変換 (v35)。node_val =
                             * 変換先の型タグ、node_lhs =
                             * 相手の式。幅つきの型への
                             * 変換だけがノードになり、
                             * 従来の型への変換は今まで
                             * どおり読み捨てる */
static int NODE_MEMBER = 55; /* メンバアクセス (v37)。
                             * node_val = メンバ番号、
                             * node_lhs = 基底、node_rhs =
                             * 0 (. 用: 基底は住所の文脈) か
                             * 1 (-> 用: 基底はポインタ値の式)。
                             * 値は、数値・ポインタのメンバ
                             * なら読み出した値、構造体・配列の
                             * メンバならその先頭アドレス */
static int NODE_MELEM = 56;  /* 配列メンバの要素 (v37)。
                             * node_val = メンバ番号、
                             * node_lhs = 基底 (住所)、
                             * node_rhs = 添字の式 */
static int NODE_SWITCH = 57; /* switch 文 (v43)。
                             * node_lhs = 制御式、
                             * node_rhs = case 群の連鎖
                             * (NODE_CASE を node_next で繋ぐ) */
static int NODE_CASE = 58;   /* switch の case 群 (v43)。
                             * node_val = case の定数値、
                             * node_lhs = その case の文の並び、
                             * node_rhs = 1 なら default
                             * (node_val は使わない) */
static int NODE_ADDR = 44;    /* 単項 & (住所、v21)。
                             * node_lhs = 変数か配列のノード */
static int NODE_INDEX_STORE = 20; /* 配列の要素への書き込み文。
                             * node_val = 配列番号、node_lhs = 添字の式、
                             * node_rhs = 書く値の式 */
static int NODE_IF = 16;    /* 条件分岐文。node_lhs = 条件の式、
                             * node_rhs = 成り立つときの文の並び、
                             * node_val = else の文の並び (無ければ -1。
                             * else if の場合は if 文ノード1つ) */
static int NODE_MUL = 14;   /* 掛け算 */
static int NODE_DIV = 15;   /* 割り算 (符号付き。除数が 0 なら
                             * RISC-V の決まりで結果は -1) */
static int NODE_STORE = 13; /* メモリ書き込み文。
                             * node_lhs = 書き込み先を表すノード
                             * (変数 = ポインタ、数 = 絶対アドレス)、
                             * node_rhs = 書く値の式 */

static int node_kind[20480];
static int node_val[20480];
static int node_lhs[20480];
static int node_rhs[20480];
static int node_next[20480]; /* 文の並び用: 次の文ノード (-1 で終わり)。
                            * 式のノードでは使わない */
static int node_etype[20480]; /* 式の型タグ (生成時に付ける、v35) */
static int nnode;  /* 作ったノードの数 */

/* --- 変数の名簿 (記号表) ---
 * 変数は「名前 → 番号」の対応表で管理する。v21 から変数は
 * 全部メモリに住む (レジスタ割り当ては廃止):
 *   var_owner[i] : 持ち主。-1 ならグローバル (静的領域)、
 *                  それ以外は関数番号 (その関数のローカル)
 *   var_slot[i]  : 置き場所の番号。グローバルなら静的領域の
 *                  何個目か、ローカルならフレームの何枠目か
 *   var_bsize[i] : 宣言の型の基本の大きさ (char なら 1、
 *                  それ以外は 8)。ポインタの歩幅に使う (v23)
 *   var_plevel[i]: 宣言のポインタ段数 (char ** なら 2)
 * スコープは関数単位: 解析中の関数 (parse_cur_func) の
 * ローカルと、グローバルだけが見える。他の関数のローカルは
 * 見えない (同名でも別物)。ローカルが同名のグローバルを
 * 隠す (シャドーイング)。名前は最大31文字 */
static char var_names[1024][32];
static int var_name_len[1024];
static int var_owner[1024];
static int var_slot[1024];
static int var_bsize[1024];
/* 型タグ (v35): 基本サイズ (1/2/4/8) に、符号なし
 * なら 16、幅つきの型なら 32 を足したもの。従来の
 * 型は char が 1、それ以外は 8 (印なし) */
static int var_etype[1024];
static char var_struct[1024];  /* 構造体番号 + 1 (v37、0 = 構造体で
                               * ない。char に詰めるための +1 符号化)。
                               * plevel 0 ならその構造体の値、
                               * 1 以上ならその構造体へのポインタ。
                               * 占めるスロット数は var_slot の差から
                               * 分かるので表は持たない) */
static int var_plevel[1024];
/* グローバル変数の初期値 (v24): ファイル範囲の宣言の
 * 初期化子を定数として記録し、静的領域の初期イメージ
 * としてバイナリに埋めるための情報。
 *   var_init_kind[i]: 0 = 初期値なし、1 = 数値、
 *                     2 = 文字列 (値はその住所になる)
 *   var_init_val[i] : 数値そのもの、または文字列番号 */
static int var_init_kind[1024];
static long var_init_val[1024];
static int has_var_inits;  /* 初期値つきグローバルがあるか */
static int nvar;       /* 登録された変数の数 (全スコープ合計) */
static int var_nglobal;  /* グローバル変数の数 (静的領域の枠数) */
static int parse_cur_func;  /* 解析中の関数 (-1 はトップレベル) */
static int func_nlocals[192];  /* 関数ごとのフレーム枠数。
                             * 本体は関数の名簿の節で説明する */

/* typedef の名簿 (v36): 型への別名。
 *   tdef_etype[i] : 型タグ (v35 の etype)
 *   tdef_bsize[i] : 基本の大きさ (char なら 1、それ以外は幅)
 *   tdef_plevel[i]: その型そのものが持つポインタ段数
 * ファイルの範囲でだけ登録でき、宣言した地点から有効。
 * スコープは持たない (割り切りは doc/roadmap.md の v36) */
static char tdef_names[64][32];
static int tdef_name_len[64];
static int tdef_etype[64];
static int tdef_bsize[64];
static int tdef_plevel[64];
static char tdef_struct[64];  /* 構造体番号 + 1 (v37、0 = なし) */
static int ntdef;

/* 名前の一致だけを見る小物 */
static int var_name_eq(int i, char *name, int len) {
    if (var_name_len[i] != len)
        return 0;
    int j = 0;
    while (j < len && var_names[i][j] == name[j])
        j++;
    return j == len;
}

/* 今見えている範囲で名前を探す: まず解析中の関数の
 * ローカル、次にグローバル。あれば番号、無ければ -1 */
static int find_var(char *name, int len) {
    if (parse_cur_func >= 0) {
        for (int i = 0; i < nvar; i++) {
            if (var_owner[i] == parse_cur_func &&
                var_name_eq(i, name, len))
                return i;
        }
    }
    for (int i = 0; i < nvar; i++) {
        if (var_owner[i] == -1 && var_name_eq(i, name, len))
            return i;
    }
    return -1;
}

/* typedef の名前の一致だけを見る小物 */
static int tdef_name_eq(int i, char *name, int len) {
    if (tdef_name_len[i] != len)
        return 0;
    int j = 0;
    while (j < len && tdef_names[i][j] == name[j])
        j++;
    return j == len;
}

/* typedef の名簿から名前を探す。あれば番号、無ければ -1 */
static int find_typedef(char *name, int len) {
    for (int i = 0; i < ntdef; i++) {
        if (tdef_name_eq(i, name, len))
            return i;
    }
    return -1;
}

/* 配列の名簿は下の方にある (名前の重なりを確かめるため
 * 先に宣言だけ置く) */
static int find_array(char *name, int len);

/* 名前を名簿に登録し、番号を返す。失敗したら -1。
 * is_decl が 1 (宣言から) のときは「今のスコープの変数」
 * として作る: 同じスコープに同名があればそれを返し、
 * 外側 (グローバル) の同名は隠れる (シャドーイング)。
 * is_decl が 0 (代入から) のときは、見えている変数が
 * あればそれ (再代入)、無ければ今の場所に新規作成 */
static int define_var_full(char *name, int len, int bsize,
                           int plevel, int is_decl, int etype,
                           int strct, int nslots) {
    if (find_array(name, len) >= 0) {
        errmsg = "その名前は配列に使われています";
        return -1;
    }
    if (is_decl) {
        for (int i = 0; i < nvar; i++) {
            if (var_owner[i] == parse_cur_func &&
                var_name_eq(i, name, len)) {
                if (var_struct[i] != strct + 1) {
                    errmsg = "同じ名前で違う型の宣言があります";
                    return -1;
                }
                return i;
            }
        }
    } else {
        int found = find_var(name, len);
        if (found >= 0)
            return found;
    }
    if (nvar >= 1024) {
        errmsg = "変数が多すぎます";
        return -1;
    }
    int i = nvar++;
    for (int j = 0; j < len; j++)
        var_names[i][j] = name[j];
    var_name_len[i] = len;
    var_owner[i] = parse_cur_func;
    var_bsize[i] = bsize;
    var_etype[i] = etype;
    var_struct[i] = strct + 1;
    var_plevel[i] = plevel;
    var_init_kind[i] = 0;  /* 初期値は宣言側が後で付ける */
    if (parse_cur_func >= 0) {
        var_slot[i] = func_nlocals[parse_cur_func];
        /* 要素への複合代入は無いので展開する (本体の約束) */
        func_nlocals[parse_cur_func] =
            func_nlocals[parse_cur_func] + nslots;
    } else {
        var_slot[i] = var_nglobal;
        var_nglobal += nslots;
    }
    return i;
}

static int define_var_typed(char *name, int len, int bsize,
                            int plevel, int is_decl, int etype) {
    return define_var_full(name, len, bsize, plevel, is_decl,
                           etype, -1, 1);
}

static int define_var(char *name, int len) {
    return define_var_typed(name, len, 8, 0, 0, 8);
}

/* --- プログラムの形 ---
 * プログラムは「文の並び + 任意の最後の式」でできている。
 * 文も式と同じく AST のノードで表す (NODE_ASSIGN など)。
 * 文の並びは node_next で数珠つなぎにしたリストで、
 * prog_stmts が先頭の文ノード (-1 なら文が無い)。
 * prog_final は最後の式の根 (無いときは -1 で、prog_has_final
 * で区別する)。どちらも parse_program が置く */
static int prog_stmts;
static int prog_final;
static int prog_has_final;
/* main 関数の番号 (v12)。プログラムに main があれば
 * その番号、無ければ -1。main があるプログラムでは、
 * 実行は「関数の並びの後に、main を呼び出して止まる」
 * 形になり、外側の文は置けない */
static int prog_main;

/* --- 関数の名簿 ---
 * 関数も変数と同じく名簿で管理する。関数 i 番について:
 *   func_names[i]     : 名前
 *   func_nparams[i]   : 引数の数 (0〜4、v22 で複数になった)
 *   func_param_var[i] : 引数の変数番号の並び [引数の数]
 *   func_body[i]      : 本体の文の並びの先頭ノード
 *   func_entry[i]     : 命令列の中での入口の命令番号
 *                       (第2層が生成のたびに決める)
 * 関数は最大128個まで */
static char func_names[192][32];
static int func_name_len[192];
static int func_nparams[192];
static int func_param_var[192][8];
static int func_ret_etype[192]; /* 戻り値の型タグ (v35) */
static char func_ret_struct[192]; /* 戻り値の構造体番号 + 1 (v37、
                             * 0 = なし。構造体を返す関数はポインタ
                             * でだけ返せるので、0 でなければ
                             * 「その構造体へのポインタを返す」) */
static int func_body[192];
static int func_entry[192];
/* プロトタイプ (v24): 宣言だけ先に書いて本体を後に置ける。
 *   func_has_body[i]: 本体まで読めたら 1
 *   func_called[i]  : 呼び出しが現れたら 1。最後まで
 *                     本体が無い関数が呼ばれていたら、
 *                     生成の入口でエラーにする */
static int func_has_body[192];
static int func_called[192];
/* func_nlocals[192] : フレームの枠数 (引数を含む、v21)。
 * 変数の名簿の節で宣言してある (define_var が使うため) */
static int nfunc;

/* --- 配列の名簿 ---
 * 配列は変数と違い、メモリに置き場所が要る。置き場所は
 * スコープで違う (v26 でローカル配列が入った):
 *   グローバル配列: バイナリの「コード＋文字列の後ろ」の
 *     静的な領域にまとめて確保し、住所は文字列と同じく
 *     2パス生成で確定させる
 *   ローカル配列  : 宣言した関数のフレームの中。置き場所
 *     はフレームの底からの位置で表し、実行時は fp 相対で
 *     辿る。呼び出しごとに別の実体になる (再帰でも安全)
 * 配列 i 番について:
 *   arr_size[i] : 最初の [ ] の数 (1次元なら要素数、
 *                 2次元なら行数)
 *   arr_cols[i] : 2次元の列数 (1次元なら 0、v23)
 *   arr_esize[i]: 要素の大きさ (1 か 8 バイト、v23)。
 *                 宣言の型から決まる: ポインタの配列
 *                 (型に * が付く) は 8、char は 1、
 *                 それ以外は 8
 *   arr_owner[i]: 持ち主の関数番号。-1 ならグローバル
 *   arr_off[i]  : グローバル配列の、配列領域の中での
 *                 位置 (バイト単位)
 *   arr_foff[i] : ローカル配列の、フレーム内の配列
 *                 置き場 (変数の枠の並びの上) の中での
 *                 位置 (バイト単位、v26)
 *   arr_addr[i] : グローバル配列の確定した先頭アドレス
 *                 (第2層が決める。ローカルでは使わない)
 * 配列は最大128個、グローバルの合計 4MB まで
 * (v25 で実測に合わせて拡大) */
static char arr_names[128][32];
static int arr_name_len[128];
static int arr_size[128];
static int arr_cols[128];
static int arr_esize[128];
static int arr_etype[128];   /* 要素の型タグ (v35) */
static char arr_struct[128];  /* 要素の構造体番号 + 1 (v37、0 = なし) */
static char arr_plevel[128];  /* 要素のポインタ段数 (v37) */
static int arr_owner[128];
static int arr_off[128];
static int arr_foff[128];
static unsigned int arr_addr[128];
static int narr;      /* 登録された配列の数 */
static int arr_used;  /* グローバル配列領域の使用済みバイト数 */
/* 配列の初期値 (v40、設計は doc/array-init.md):
 *   arr_init_off[i]: 値プールの中の先頭 (要素単位)
 *   arr_init_cnt[i]: 書かれた要素の数 (0 = 初期値なし)
 *   arr_init_pool[]: 値そのもの (8バイト/要素で貯める)
 *   arr_init_end   : 初期値のバイトが届く末尾
 *                    (配列領域の中の位置。0 = 無し) */
static int arr_init_off[128];
static int arr_init_cnt[128];
static long arr_init_pool[65536];
static int arr_init_used;
static int arr_init_end;
/* func_arr_bytes[f]: 関数 f のフレームに入るローカル
 * 配列の合計バイト数 (v26)。フレーム長の計算 (第2層の
 * func_frame_size) が使う */
static int func_arr_bytes[192];

/* 名前の一致だけを見る小物 (変数の var_name_eq と同じ) */
static int arr_name_eq(int i, char *name, int len) {
    if (arr_name_len[i] != len)
        return 0;
    int j = 0;
    while (j < len && arr_names[i][j] == name[j])
        j++;
    return j == len;
}

/* 今見えている範囲で名前の配列を探す: まず解析中の
 * 関数のローカル、次にグローバル (変数の find_var と
 * 同じ作法)。あれば番号、無ければ -1 */
static int find_array(char *name, int len) {
    if (parse_cur_func >= 0) {
        for (int i = 0; i < narr; i++) {
            if (arr_owner[i] == parse_cur_func &&
                arr_name_eq(i, name, len))
                return i;
        }
    }
    for (int i = 0; i < narr; i++) {
        if (arr_owner[i] == -1 && arr_name_eq(i, name, len))
            return i;
    }
    return -1;
}

/* --- 構造体の名簿 (v37、設計は doc/struct.md) ---
 * タグ表: 名前・大きさ・アラインメント・メンバの範囲。
 * 大きさは末尾パディング込み (C 互換レイアウト)。
 * メンバ表: 全タグのメンバを並べた1枚の表で、各メンバは
 * 名前・オフセット・型タグ・ポインタ段数・構造体番号
 * (-1 = 構造体でない)・配列の個数 (0 = 配列でない) を持つ */
static char stag_names[32][32];
static int stag_name_len[32];
static int stag_size[32];
static int stag_align[32];
static int stag_mfirst[32];
static int stag_mcount[32];
static int stag_complete[32];  /* 定義が閉じたか (定義中は 0) */
static int nstag;
static char smem_names[160][32];
static int smem_name_len[160];
static int smem_off[160];
static int smem_etype[160];
static int smem_plevel[160];
static char smem_struct[160];  /* 構造体番号 + 1 (0 = なし) */
static int smem_count[160];
static int nsmem;

/* タグ名から構造体を探す。あれば番号、無ければ -1 */
static int find_struct_tag(char *name, int len) {
    for (int i = 0; i < nstag; i++) {
        if (stag_name_len[i] != len)
            continue;
        int j = 0;
        while (j < len && stag_names[i][j] == name[j])
            j++;
        if (j == len)
            return i;
    }
    return -1;
}

/* タグ tg のメンバから名前を探す。あればメンバ番号、
 * 無ければ -1 */
static int find_member(int tg, char *name, int len) {
    for (int i = stag_mfirst[tg]; i < stag_mfirst[tg] + stag_mcount[tg]; i++) {
        if (smem_name_len[i] != len)
            continue;
        int j = 0;
        while (j < len && smem_names[i][j] == name[j])
            j++;
        if (j == len)
            return i;
    }
    return -1;
}

/* 配列を名簿に登録し、番号を返す。rows は最初の [ ] の
 * 数、cols は2つ目の [ ] の数 (1次元なら 0)、esize は
 * 要素の大きさ。確保する大きさは 要素数 × esize バイト。
 * スコープの作法は変数と同じ (v26): 今のスコープ (解析中
 * の関数、またはファイル範囲) に同名の配列や、今見えて
 * いる同名の変数があればエラー。外側の同名は隠れる。
 * 宣言場所で置き場所が決まる: 関数の中ならフレーム、
 * 外なら静的な領域。失敗したら -1 */
static int define_array_s(char *name, int len, int rows, int cols,
                          int esize, int etype, int strct, int plevel) {
    for (int i = 0; i < narr; i++) {
        if (arr_owner[i] == parse_cur_func &&
            arr_name_eq(i, name, len)) {
            /* ファイル範囲の配列は、大きさが同じなら
             * 再宣言を合併する (v33、C の仮定義と
             * 同じ考え方。分割コンパイルで共有の表を
             * 宣言し直すために要る)。大きさが違う
             * 再宣言は食い違いとして止める */
            if (parse_cur_func < 0 &&
                arr_size[i] == rows && arr_cols[i] == cols &&
                arr_esize[i] == esize && arr_etype[i] == etype &&
                arr_struct[i] == strct + 1 && arr_plevel[i] == plevel)
                return i;
            if (parse_cur_func < 0) {
                errmsg = "配列の大きさが宣言ごとに違います";
                return -1;
            }
            errmsg = "その名前は既に使われています";
            return -1;
        }
    }
    if (find_var(name, len) >= 0) {
        errmsg = "その名前は既に使われています";
        return -1;
    }
    if (narr >= 128) {
        errmsg = "配列は128個までです";
        return -1;
    }
    int elems = cols > 0 ? rows * cols : rows;
    int bytes = elems * esize;
    if (parse_cur_func < 0 &&
        arr_used + bytes > 4194304) {
        errmsg = "配列の合計が大きすぎます (4MBまで)";
        return -1;
    }
    int i = narr++;
    for (int j = 0; j < len; j++)
        arr_names[i][j] = name[j];
    arr_name_len[i] = len;
    arr_size[i] = rows;
    arr_cols[i] = cols;
    arr_esize[i] = esize;
    arr_etype[i] = etype;
    arr_struct[i] = strct + 1;
    arr_plevel[i] = plevel;
    arr_owner[i] = parse_cur_func;
    if (parse_cur_func < 0) {
        if (strct >= 0) {
            /* 構造体の配列は先頭を要素のアラインメントに
             * 揃える (v37。他の配列は従来どおり詰める) */
            int al = stag_align[strct];
            if (arr_used % al)
                arr_used += al - (arr_used % al);
        }
        arr_off[i] = arr_used;
        arr_used += bytes;
        arr_foff[i] = 0;
    } else {
        /* ローカル配列はフレームの底から詰める。要
         * 素が 8 バイトの配列の先頭が 8 バイト境界に
         * 来るよう、使う分を 8 の倍数に切り上げる */
        int alloc = (bytes + 7) & ~7;
        arr_foff[i] = func_arr_bytes[parse_cur_func];
        /* 要素への複合代入 (+= 等) は iwac の言語に
         * まだ無いので、展開した形で書く (本体の約束:
         * まだ作っていないものを使わない) */
        func_arr_bytes[parse_cur_func] =
            func_arr_bytes[parse_cur_func] + alloc;
        arr_off[i] = 0;
    }
    return i;
}

/* 名前 (場所と長さで渡す) の関数があればその番号、無ければ -1 */
static int find_func(char *name, int len) {
    for (int i = 0; i < nfunc; i++) {
        if (func_name_len[i] != len)
            continue;
        int j = 0;
        while (j < len && func_names[i][j] == name[j])
            j++;
        if (j == len)
            return i;
    }
    return -1;
}

/* --- 文字列の置き場 (文字列テーブル) ---
 * プログラム中に出てきた文字列リテラルを全部溜める。
 * str_pool には各文字列のバイト列が、末尾の 0 (文字列の
 * 終わり印) 付きで連続して並ぶ。文字列 i 番は:
 *   str_off[i] : str_pool の中の開始位置
 *   str_len[i] : 長さ (末尾の 0 は含まない)
 * 実行時の住所 (str_addr) は第2層が決める (コードの後ろに
 * このプールをそのまま置くので、住所 = 読み込み先頭 +
 * コードのバイト数 + str_off[i]) */
static char str_pool[16384];
static int str_off[512];
static int str_len[512];
static int nstr;       /* 文字列の数 */
static int str_used;   /* str_pool の使用済みバイト数 */
static unsigned int str_addr[512];  /* 第2層が確定させる住所 */

/* ノードを1つ作って、その番号を返す。失敗したら -1 */
static int new_node(int kind, int val, int lhs, int rhs) {
    if (nnode >= 20480) {
        errmsg = "プログラムが大きすぎます (ノード数上限)";
        return -1;
    }
    int i = nnode++;
    node_kind[i] = kind;
    node_val[i] = val;
    node_lhs[i] = lhs;
    node_rhs[i] = rhs;
    node_next[i] = -1;
    return i;
}

/* --- 字句を読む道具 --- */

static char *cur;  /* 入力の「今読んでいる場所」 */

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* 変数名 (識別子) に使える文字: 英字と _ で始まり、
 * その後は数字もよい (C と同じ約束) */
static int is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_ident_char(char c) {
    return is_ident_start(c) || is_digit(c);
}

/* 読み取ったばかりの識別子の置き場 */
static char ident_buf[32];
static int ident_len;

/* カーソルの位置から識別子を1つ読み、ident_buf に置く。
 * カーソルは識別子の直後まで進む。失敗したら 0 */
static int parse_ident(void) {
    if (!is_ident_start(*cur)) {
        errmsg = "変数名が必要です";
        return 0;
    }
    ident_len = 0;
    while (is_ident_char(*cur)) {
        if (ident_len >= 31) {
            errmsg = "変数名が長すぎます (31文字まで)";
            return 0;
        }
        ident_buf[ident_len++] = *cur++;
    }
    return 1;
}

/* 空白は意味を持たないので読み飛ばす (字句解析の仕事) */
/* 空白とコメントを飛ばす。v10 で、ファイルから読む
 * プログラムのために改行 (と CR) と行コメント // も
 * 飛ばせるようにした。// から行末まではコメント。
 * 除算の / と見分けるのは「/ が2つ続いたらコメント」で、
 * 本物の C と同じ決め事 (文字列の中は対象外。文字列の
 * 読み取りはこの関数を通らないので "http://..." の
 * ような文字列も壊れない) */
static void skip_spaces(void) {
    for (;;) {
        while (*cur == ' ' || *cur == '\t' || *cur == '\n' ||
               *cur == '\r')
            cur++;
        if (cur[0] == '/' && cur[1] == '/') {
            while (*cur != '\0' && *cur != '\n')
                cur++;
            continue;
        }
        if (cur[0] == '/' && cur[1] == '*') {
            /* ブロックコメント (v16): 閉じの印まで飛ばす。
             * 閉じが無ければ黙って終わりにせず字句の
             * エラーにする。この関数は戻り値で失敗を
             * 伝えられないので、印 (lex_failed) で伝える */
            cur += 2;
            while (*cur != '\0' &&
                   !(cur[0] == '*' && cur[1] == '/'))
                cur++;
            if (*cur == '\0') {
                errmsg = "コメントが閉じていません (*/ が必要です)";
                lex_failed = 1;
                return;
            }
            cur += 2;
            continue;
        }
        return;
    }
}

/* カーソルの位置から数字を1つ読み、値を返す。
 * 失敗したら -1 を返し、理由を errmsg に置く。
 * 上限が 2047 なのは、この数が後で addi 命令の即値になり、
 * 即値が符号付き12ビット (正は 2047 まで) のため */
static int parse_number_value(void) {
    if (!is_digit(*cur)) {
        errmsg = "数字が必要です";
        return -1;
    }
    /* v25: 上限を int の範囲まで広げた (大きな数は生成が
     * auipc + addi の組み合わせで作る)。接尾辞 (u / l の
     * 並び) は読み捨てる */
    long value = 0;
    while (is_digit(*cur)) {
        value = value * 10 + (*cur - '0');
        if (value > 2147483647L) {
            errmsg = "数字が大きすぎます";
            return -1;
        }
        cur++;
    }
    while (*cur == 'u' || *cur == 'U' || *cur == 'l' || *cur == 'L')
        cur++;
    return (int)value;
}

/* 16進数の1文字が表す値。16進数でなければ -1 */
static int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* 数字を読み、その NODE_NUM ノードを作って番号を返す。
 * 10進数は従来どおり 2047 まで (式の項用)。
 * 0x で始まる16進数は「アドレスを書くための数」として
 * 32ビットまで許す。値そのものが大きい数の扱いは第2層で
 * 行う (addi では入らないので住所の材料と同じ作り方で
 * レジスタに作る) */
static int parse_number_node(void) {
    if (cur[0] == '0' && (cur[1] == 'x' || cur[1] == 'X')) {
        cur += 2;
        if (hex_value(*cur) < 0) {
            errmsg = "0x の後には16進数の数字が必要です";
            return -1;
        }
        unsigned long value = 0;
        while (hex_value(*cur) >= 0) {
            value = value * 16ul + (unsigned long)hex_value(*cur);
            if (value > 0xFFFFFFFFul) {
                errmsg = "16進数が大きすぎます (32ビットまで)";
                return -1;
            }
            cur++;
        }
        /* 接尾辞 (u / l の並び) は読み捨てる (v25) */
        while (*cur == 'u' || *cur == 'U' || *cur == 'l' ||
               *cur == 'L')
            cur++;
        /* しまう前に、32ビットのパターンを int の
         * 符号付きの範囲 (-2^31 .. 2^31-1) に畳む。
         * キャスト任せにすると、GCC では負の数になる
         * のに iwac では (幅が全部64ビットなので) 正の
         * 大きな数のままになり、読む側 (eval_const や
         * gen_load_leaf) の結果が作ったコンパイラで
         * 食い違って固定点にならない (v27 のセルフ
         * ホストで発覚)。畳む計算自体が両方で同じに
         * なるよう、キャストには頼らない */
        long sval = (long)(unsigned int)value;
        if (sval >= 0x80000000)
            sval = sval - 0x80000000 - 0x80000000;
        return new_node(NODE_NUM, (int)sval, 0, 0);
    }
    int value = parse_number_value();
    if (value < 0)
        return -1;
    return new_node(NODE_NUM, value, 0, 0);
}

/* 文字列リテラルを読み、文字列テーブルに登録して、その
 * NODE_STR ノードを作る。カーソルは開きの '"' の位置にある
 * 前提。エスケープは \n (改行) \t (タブ) \r \\ \" (v25 で \r を追加)。
 * 閉じの '"' が無い、知らないエスケープ、長すぎる文字列は
 * エラー。成功したらノード番号、失敗したら -1 を返す */
static int parse_string_node(void) {
    if (nstr >= 512) {
        errmsg = "文字列が多すぎます";
        return -1;
    }
    int idx = nstr;
    int start = str_used;
    int len = 0;
    cur++;  /* 開きの '"' を飛ばす */
    for (;;) {
        char c = *cur;
        if (c == '\0') {
            errmsg = "文字列が閉じていません (\" が必要です)";
            return -1;
        }
        if (c == '"') {
            cur++;
            break;
        }
        if (c == '\\') {
            cur++;
            if (*cur == 'n')
                c = '\n';
            else if (*cur == 't')
                c = '\t';
            else if (*cur == 'r')
                c = '\r';
            else if (*cur == '\\')
                c = '\\';
            else if (*cur == '"')
                c = '"';
            else {
                errmsg = "使えるエスケープは \\n \\t \\r \\\\ \\\" だけです";
                return -1;
            }
        }
        if (str_used >= 16383) {
            errmsg = "文字列が長すぎます";
            return -1;
        }
        str_pool[str_used++] = c;
        len++;
        cur++;
    }
    /* C の文字列の約束: 末尾に 0 を置く (print loop が
     * 「0 が来たら終わり」と判定するための目印) */
    str_pool[str_used++] = '\0';
    str_off[idx] = start;
    str_len[idx] = len;
    nstr++;
    return new_node(NODE_STR, idx, 0, 0);
}

/* 式全体を読む関数は、この下で定義する (かっこの中身が
 * 式全体なので、互いに呼び合う)。先に宣言だけ置く。
 * expect_char も下で定義する道具の先行宣言 */
static int parse_expr(void);
static int parse_type(void);  /* キャストの判定で先に使う (v25) */
static int parse_type_etype;   /* 型タグ (本体は型の節、v35) */
static int parse_type_plevel;  /* ポインタの段数 (同上) */
static int parse_type_struct;  /* 構造体番号 (同上、v37) */
static int parse_rel_expr(void);  /* eq の段から呼ばれる (v11) */
static int parse_unary_expr(void); /* 単項の段 (v19)。文からも使う */
static int expect_char(char c, char *msg);
static int ident_is(char *word);  /* syscall3 の判定で先に使う (v27) */

/* 呼び出しの引数リストを読む (v22): 「(」は消費済みの前提で、
 * 引数の式をカンマ区切りで読み、「)」まで消費する。
 * 結果は node_next で繋いだ式の並びで、先頭を *head_out へ
 * 置く (引数なしは -1)。個数は関数の定義と照合する。
 * 引数は最大8つ (a0〜a7 で渡す呼び出し規約のため。
 * v25 で 4 から 8 に拡大)。
 * 式の材料 (parse_primary_node) からも、呼び出し文
 * (parse_stmt) からも使う共通の道具。成功 1、失敗 0 */
static int parse_call_list(int f, int *head_out) {
    int head = -1;
    int tail = -1;
    int n = 0;
    skip_spaces();
    if (*cur == ')') {
        cur++;
        if (func_nparams[f] != 0) {
            errmsg = "引数の数が関数の定義と合いません";
            return 0;
        }
        *head_out = -1;
        return 1;
    }
    for (;;) {
        if (n >= 8) {
            errmsg = "引数は8つまでです";
            return 0;
        }
        int arg = parse_expr();
        if (arg < 0)
            return 0;
        if (head < 0)
            head = arg;
        else
            node_next[tail] = arg;
        tail = arg;
        n++;
        skip_spaces();
        if (*cur == ',') {
            cur++;
            continue;
        }
        break;
    }
    if (!expect_char(')', "呼び出しの後には ) が必要です"))
        return 0;
    if (n != func_nparams[f]) {
        errmsg = "引数の数が関数の定義と合いません";
        return 0;
    }
    func_called[f] = 1;  /* 呼ばれた印 (未定義検査用、v24) */
    *head_out = head;
    return 1;
}

/* 式の材料 (primary) を1つ読む: 数、変数、文字列、
 * かっこで囲んだ式、または *材料 (ポインタの先の1バイト)。
 * 変数は名簿にあるものだけが使える。代入されていない変数を
 * 使うのはエラー (「いつか入るはず」の値で計算しないため。
 * この小さい言語では、使う前に必ず代入文が先にある書き方)。
 * 成功したらノード番号、失敗したら -1 を返す */
/* 文字リテラルを読む (v16): 'a' の形。中身は1文字か
 * エスケープ1つ。値はその文字コードで、数のノードになる。
 * エスケープは文字列のもの (\n \t \r \\ \") に \' と \0 を
 * 加えた6つ。空 ('')、2文字以上、閉じ忘れはエラー。
 * 成功したらノード番号、失敗したら -1 を返す */
static int parse_char_node(void) {
    cur++;  /* 開きの ' を飛ばす */
    int value;
    if (*cur == '\\') {
        cur++;
        if (*cur == 'n')
            value = '\n';
        else if (*cur == 't')
            value = '\t';
        else if (*cur == 'r')
            value = '\r';
        else if (*cur == '0')
            value = 0;
        else if (*cur == '\\')
            value = '\\';
        else if (*cur == '\'')
            value = '\'';
        else if (*cur == '"')
            value = '"';
        else {
            errmsg = "使えるエスケープは \\n \\t \\r \\0 \\\\ \\' \\\" だけです";
            return -1;
        }
        cur++;
    } else if (*cur == '\'' || *cur == '\0') {
        errmsg = "文字リテラルが空か、閉じていません";
        return -1;
    } else {
        value = (unsigned char)*cur;
        cur++;
    }
    if (*cur != '\'') {
        errmsg = "文字リテラルは1文字だけです (閉じの ' が必要です)";
        return -1;
    }
    cur++;
    return new_node(NODE_NUM, value, 0, 0);
}

static int parse_primary_node(void) {
    skip_spaces();
    if (is_digit(*cur))
        return parse_number_node();
    if (*cur == '"')
        return parse_string_node();
    if (*cur == '\'')
        return parse_char_node();
    if (*cur == '(') {
        /* かっこ: 中身は式全体。かっこ自体は木を作らず、
         * 中身の木をそのまま返す (優先順位の指定だけが役目) */
        cur++;
        int inner = parse_expr();
        if (inner < 0)
            return -1;
        if (!expect_char(')', "かっこの閉じ ) が必要です"))
            return -1;
        return inner;
    }
    /* ポインタ読み (*) は v19 から単項の段 (parse_unary_expr)
     * の仕事になった ( *p++ のような形を通すため)。
     * ここ (材料) には演算子は来ない */
    if (is_ident_start(*cur)) {
        if (!parse_ident())
            return -1;
        if (ident_is("sizeof")) {
            /* sizeof ( 型 ) (v37): 解析時に値が決まる定数。
             * ポインタは 8、構造体はパディング込みの
             * 大きさ、それ以外は型の幅 */
            skip_spaces();
            if (!expect_char('(', "sizeof の後には ( が必要です"))
                return -1;
            if (!parse_type()) {
                errmsg = "sizeof の中は型で書きます";
                return -1;
            }
            if (!expect_char(')', "sizeof の後には ) が必要です"))
                return -1;
            int sz;
            if (parse_type_plevel >= 1)
                sz = 8;
            else if (parse_type_struct >= 0)
                sz = stag_size[parse_type_struct];
            else
                sz = parse_type_etype & 15;
            return new_node(NODE_NUM, sz, 0, 0);
        }
        if (ident_is("syscall3")) {
            /* 組み込み syscall3(番号, a0, a1, a2) (v27):
             * KosabaOS のシステムコールを直接呼ぶ式。
             * putc と同じ予約の名前で、ユーザーの関数
             * より先に拾う。引数は4つ固定 */
            skip_spaces();
            if (*cur != '(') {
                errmsg = "syscall3 の後には ( が必要です";
                return -1;
            }
            cur++;
            int argnodes[4];
            for (int i = 0; i < 4; i++) {
                int a = parse_expr();
                if (a < 0)
                    return -1;
                argnodes[i] = a;
                skip_spaces();
                if (i < 3) {
                    if (*cur != ',') {
                        errmsg = "syscall3 の引数は4つです (番号, a0, a1, a2)";
                        return -1;
                    }
                    cur++;
                }
            }
            if (!expect_char(')', "syscall3 の後には ) が必要です"))
                return -1;
            for (int i = 0; i < 3; i++)
                node_next[argnodes[i]] = argnodes[i + 1];
            node_next[argnodes[3]] = -1;
            return new_node(NODE_SYSCALL3, 0, argnodes[0], 0);
        }
        /* 関数の名前で、次が '(' なら呼び出し (v22 で式に
         * なった)。戻り値が式の材料になる。呼べるのは
         * 定義済みの関数だけ (「使う前に定義」の約束) */
        int f = find_func(ident_buf, ident_len);
        if (f >= 0) {
            skip_spaces();
            if (*cur == '(') {
                cur++;
                int args;
                if (!parse_call_list(f, &args))
                    return -1;
                return new_node(NODE_CALL, f, args, 0);
            }
        }
        /* 配列の名前なら、要素読みか「先頭アドレス」か */
        int arr = find_array(ident_buf, ident_len);
        if (arr >= 0) {
            skip_spaces();
            if (*cur == '[') {
                /* 要素読み: 名前 [ 添字の式 ] */
                cur++;
                int idx = parse_expr();
                if (idx < 0)
                    return -1;
                if (!expect_char(']', "添字の後には ] が必要です"))
                    return -1;
                skip_spaces();
                if (*cur == '[') {
                    /* もう1つ添字が続けば、2次元の要素 (v23) */
                    if (arr_cols[arr] == 0) {
                        errmsg = "1次元の配列に付けられる添字は1つだけです";
                        return -1;
                    }
                    cur++;
                    int idx2 = parse_expr();
                    if (idx2 < 0)
                        return -1;
                    if (!expect_char(']', "添字の後には ] が必要です"))
                        return -1;
                    return new_node(NODE_INDEX2, arr, idx, idx2);
                }
                if (arr_cols[arr] > 0)
                    /* 2次元で添字が1つだけなら、行の先頭
                     * アドレスが値になる (v23) */
                    return new_node(NODE_ROWADDR, arr, 0, idx);
                return new_node(NODE_INDEX, arr, 0, idx);
            }
            /* 添字が無ければ、配列の先頭アドレスが値になる
             * (関数に渡したり、ポインタ変数に入れたり) */
            return new_node(NODE_ARRAY, arr, 0, 0);
        }
        int var = find_var(ident_buf, ident_len);
        if (var < 0) {
            errmsg = "代入されていない変数が使われました";
            return -1;
        }
        skip_spaces();
        if (*cur == '[') {
            /* ポインタの添字読み (v23): p[i] は
             * *(p + i × 指す先の大きさ) と同じ意味。
             * ポインタとして宣言された変数だけに付く */
            if (var_plevel[var] < 1) {
                errmsg = "添字 [ ] を付けられるのは配列かポインタだけです";
                return -1;
            }
            cur++;
            int idx = parse_expr();
            if (idx < 0)
                return -1;
            if (!expect_char(']', "添字の後には ] が必要です"))
                return -1;
            return new_node(NODE_PINDEX, var, 0, idx);
        }
        return new_node(NODE_VAR, var, 0, 0);
    }
    errmsg = "数か変数が必要です";
    return -1;
}

/* 後置の段 (v19): 材料の後ろに ++ / -- が付いたら包む。
 * 相手は変数だけ (今のところ)。*p++ の p や、添字の中の
 * i++ がここで読まれる */
/* 要素を表すノードか (添字読みの3種)。++ / -- と
 * 単項 & の相手の判定に使う (v25) */
static int is_elem_node(int node) {
    int k = node_kind[node];
    return k == NODE_INDEX || k == NODE_INDEX2 || k == NODE_PINDEX ||
           k == NODE_MEMBER || k == NODE_MELEM;
}

/* メンバを表すノードが「数値として増減できる」か (v37):
 * 構造体の値や配列のメンバは相手にできない */
static int member_is_scalar(int node) {
    int mi = node_val[node];
    if (smem_count[mi] > 0 && node_kind[node] == NODE_MEMBER)
        return 0;
    if (smem_struct[mi] > 0 && smem_plevel[mi] == 0)
        return 0;
    return 1;
}

static int parse_postfix_expr(void) {
    int node = parse_primary_node();
    if (node < 0)
        return -1;
    /* メンバアクセス (. / ->) のために、今のノードの値の
     * 型を追う (v37): ts = 構造体番号 (-1 = 構造体でない)、
     * tp = ポインタ段数、tarr = 配列か (配列メンバの直後
     * だけ 1) */
    int ts = -1;
    int tp = 0;
    int tarr = 0;
    {
        int k = node_kind[node];
        if (k == NODE_VAR) {
            int v = node_val[node];
            ts = var_struct[v] - 1;
            tp = var_plevel[v];
        } else if (k == NODE_ARRAY || k == NODE_ROWADDR) {
            /* 配列名 (と2次元の行) は先頭要素へのポインタ */
            int a = node_val[node];
            ts = arr_struct[a] - 1;
            tp = arr_plevel[a] + 1;
        } else if (k == NODE_INDEX || k == NODE_INDEX2) {
            int a = node_val[node];
            ts = arr_struct[a] - 1;
            tp = arr_plevel[a];
        } else if (k == NODE_PINDEX) {
            int v = node_val[node];
            ts = var_struct[v] - 1;
            tp = var_plevel[v] - 1;
        } else if (k == NODE_CALL) {
            int f = node_val[node];
            ts = func_ret_struct[f] - 1;
            tp = ts >= 0 ? 1 : 0;
        } else if (k == NODE_DEREF) {
            /* *p の形: 相手が変数なら、その指す先の型 */
            int o = node_lhs[node];
            if (node_kind[o] == NODE_VAR) {
                int v = node_val[o];
                ts = var_struct[v] - 1;
                tp = var_plevel[v] - 1;
            }
        }
        if (tp < 0) {
            ts = -1;
            tp = 0;
        }
    }
    for (;;) {
        skip_spaces();
        if (*cur == '.' && is_ident_start(cur[1])) {
            /* . メンバ (v37): 構造体の値にだけ付く */
            if (ts < 0 || tp != 0 || tarr) {
                errmsg = "メンバアクセス . は構造体の値にだけ付けられます";
                return -1;
            }
            cur++;
            if (!parse_ident())
                return -1;
            int mi = find_member(ts, ident_buf, ident_len);
            if (mi < 0) {
                errmsg = "そのメンバはありません";
                return -1;
            }
            node = new_node(NODE_MEMBER, mi, node, 0);
            if (node < 0)
                return -1;
            ts = smem_struct[mi] - 1;
            tp = smem_plevel[mi];
            tarr = smem_count[mi] > 0;
            continue;
        }
        if (cur[0] == '-' && cur[1] == '>') {
            /* -> メンバ (v37): 構造体へのポインタにだけ付く */
            if (ts < 0 || tp < 1 || tarr) {
                errmsg = "メンバアクセス -> は構造体へのポインタにだけ付けられます";
                return -1;
            }
            cur += 2;
            skip_spaces();
            if (!parse_ident()) {
                errmsg = "-> の後にはメンバ名が必要です";
                return -1;
            }
            int mi = find_member(ts, ident_buf, ident_len);
            if (mi < 0) {
                errmsg = "そのメンバはありません";
                return -1;
            }
            node = new_node(NODE_MEMBER, mi, node, 1);
            if (node < 0)
                return -1;
            ts = smem_struct[mi] - 1;
            tp = smem_plevel[mi];
            tarr = smem_count[mi] > 0;
            continue;
        }
        if (*cur == '[' && tarr) {
            /* 配列メンバの要素 (v37) */
            int mi = node_val[node];
            cur++;
            int idx = parse_expr();
            if (idx < 0)
                return -1;
            if (!expect_char(']', "添字の後には ] が必要です"))
                return -1;
            node = new_node(NODE_MELEM, mi, node, idx);
            if (node < 0)
                return -1;
            tarr = 0;  /* 要素の型 (ts / tp) はメンバのまま */
            continue;
        }
        int is_post_inc;
        if (cur[0] == '+' && cur[1] == '+')
            is_post_inc = 1;
        else if (cur[0] == '-' && cur[1] == '-')
            is_post_inc = 0;
        else
            return node;
        cur += 2;
        if (node_kind[node] == NODE_VAR) {
            /* 変数への後置 (v19) */
            if (var_struct[node_val[node]] > 0 &&
                var_plevel[node_val[node]] == 0) {
                errmsg = "++ / -- の相手は数値だけです";
                return -1;
            }
            node = new_node(is_post_inc ? NODE_POSTINC : NODE_POSTDEC,
                            node_val[node], 0, 0);
        } else if (is_elem_node(node)) {
            /* 要素への後置 (v25): a[i]++ の形 */
            int nk = node_kind[node];
            if ((nk == NODE_MEMBER || nk == NODE_MELEM) &&
                !member_is_scalar(node)) {
                errmsg = "++ / -- の相手は数値のメンバだけです";
                return -1;
            }
            if ((nk == NODE_INDEX || nk == NODE_INDEX2) &&
                arr_struct[node_val[node]] > 0 &&
                arr_plevel[node_val[node]] == 0) {
                errmsg = "++ / -- の相手は数値だけです";
                return -1;
            }
            node = new_node(NODE_EPOST, is_post_inc ? 1 : -1,
                            node, 0);
        } else {
            errmsg = "++ / -- の相手は変数か要素だけです";
            return -1;
        }
        if (node < 0)
            return -1;
        ts = -1;  /* 増減の結果は数値。メンバはもう辿れない */
        tarr = 0;
    }
}

/* 単項の段 (v19): 頭に付く演算子。右結合 (相手も単項の段
 * で読む) なので、- -x や !*p のような重なりが自然に読める。
 *   ++x / --x   前置。相手は変数だけ
 *   -x          符号の反転
 *   !x          論理 NOT (0 なら 1、それ以外は 0)
 *   *x          ポインタ読み (材料の段から引っ越してきた。
 *               *p++ を「*(p++)」と読むため)。** はまだ扱わない */
static int parse_unary_expr(void) {
    skip_spaces();
    if (*cur == '(') {
        /* キャスト (型) 式 (v25): '(' の直後が型なら
         * キャスト。iwac は型を区別しないので、キャストは
         * 読み捨てて相手の式をそのまま使う。切り詰めの
         * ような変換は起きない点は割り切り (iwac 自身の
         * ソースのキャストは範囲が合っているものだけ) */
        char *save = cur;
        cur++;
        skip_spaces();
        if (parse_type()) {
            skip_spaces();
            if (*cur == ')') {
                int et = parse_type_etype;
                int pl = parse_type_plevel;
                cur++;
                int operand = parse_unary_expr();
                if (operand < 0)
                    return -1;
                /* 幅つきの型への変換は、正規形への
                 * 切り詰め/拡張が要るのでノードにする
                 * (v35)。ポインタへの変換と従来の型
                 * への変換は今までどおり読み捨てる */
                if ((et & 32) && pl == 0)
                    return new_node(NODE_CAST, et, operand, 0);
                return operand;
            }
        }
        cur = save;
    }
    if (cur[0] == '+' && cur[1] == '+') {
        cur += 2;
        int operand = parse_unary_expr();
        if (operand < 0)
            return -1;
        if (node_kind[operand] == NODE_VAR)
            return new_node(NODE_PREINC, node_val[operand], 0, 0);
        if (is_elem_node(operand))
            return new_node(NODE_EPRE, 1, operand, 0);
        errmsg = "++ / -- の相手は変数か要素だけです";
        return -1;
    }
    if (cur[0] == '-' && cur[1] == '-') {
        cur += 2;
        int operand = parse_unary_expr();
        if (operand < 0)
            return -1;
        if (node_kind[operand] == NODE_VAR)
            return new_node(NODE_PREDEC, node_val[operand], 0, 0);
        if (is_elem_node(operand))
            return new_node(NODE_EPRE, -1, operand, 0);
        errmsg = "++ / -- の相手は変数か要素だけです";
        return -1;
    }
    if (*cur == '-') {
        cur++;
        int operand = parse_unary_expr();
        if (operand < 0)
            return -1;
        return new_node(NODE_NEG, 0, operand, 0);
    }
    if (*cur == '&' && cur[1] != '&') {
        /* 単項 & (住所、v21。v25 で要素も相手にできる):
         * && とは位置で見分ける (材料の頭の & は住所、
         * 式の途中の & はビット演算か論理演算) */
        cur++;
        int operand = parse_unary_expr();
        if (operand < 0)
            return -1;
        int ok = node_kind[operand];
        if (ok != NODE_VAR && ok != NODE_ARRAY &&
            !is_elem_node(operand)) {
            errmsg = "& の相手は変数か配列か要素だけです";
            return -1;
        }
        /* is_elem_node にはメンバも含まれる (v37) */
        return new_node(NODE_ADDR, 0, operand, 0);
    }
    if (*cur == '!' && cur[1] != '=') {
        cur++;
        int operand = parse_unary_expr();
        if (operand < 0)
            return -1;
        return new_node(NODE_NOT, 0, operand, 0);
    }
    if (*cur == '~') {
        /* ビット反転 (v25): 全ビットをひっくり返す */
        cur++;
        int operand = parse_unary_expr();
        if (operand < 0)
            return -1;
        return new_node(NODE_BITNOT, 0, operand, 0);
    }
    if (*cur == '*') {
        /* ポインタ読み: * の次は「住所を表すもの」。
         * 変数なら、その変数が持つ住所 (ポインタ)。
         * 数なら、その数そのものが住所になる (絶対アドレス
         * 読み)。デバイスのレジスタは決まった住所にあるので、
         * *0x10000005 と書けば LSR が読める (MMIO)。
         * かっこで囲んだ式 (住所の計算結果) も相手にできる */
        cur++;
        int operand = parse_unary_expr();
        if (operand < 0)
            return -1;
        if (node_kind[operand] == NODE_DEREF) {
            errmsg = "* を重ねる形 (**) はまだ扱えません";
            return -1;
        }
        return new_node(NODE_DEREF, 0, operand, 0);
    }
    return parse_postfix_expr();
}

/* --- 式の優先順位 (v7 で C に合わせ、v19 で全段を揃えた) ---
 *
 * 演算子の結びの固さは C と同じ順にした。固い順に:
 *   材料 (数・変数・文字列・かっこ)  … parse_primary_node
 *   後置 ++ --                     … parse_postfix_expr (v19)
 *   単項 - ! * と前置 ++ --         … parse_unary_expr (v19)
 *   * / % (掛け算・割り算・剰余)    … parse_term_expr
 *   + -                            … parse_add_expr
 *   << >> (シフト)                  … parse_shift_expr (v19)
 *   < > <= >=                      … parse_rel_expr (v11)
 *   == !=                          … parse_eq_expr
 *   &                              … parse_bitand_expr (v19)
 *   ^                              … parse_bitxor_expr (v19)
 *   |                              … parse_bitor_expr (v19)
 *   &&                             … parse_land_expr (v19)
 *   ||                             … parse_expr (一番外側、v19)
 * 「固い」ほど先に (深く) 計算される。読み方は各段とも
 * 左結合: 演算子が来たら「今の木全体を左の子、新しい材料を
 * 右の子とする新しい根」を作る。"50+10+5" は ((50+10)+5)。
 * 段を分けることで、"60+2*3" は +(60, *(2,3)) = 66 に、
 * "a & b == c" は &(a, ==(b, c)) になる (C と同じ)。
 * ポーリングの形は C と同じくかっこで書く:
 *   (*0x10000005 & 32) == 0
 * (v6 では、かっこが無い代わりに & を固くしていた。
 *  かっこが入ったので C の順に戻した) */

/* 掛け算・割り算・剰余の段 (term) を読む。相手は単項の段。
 * 演算子の * は、単項の頭に来る * (ポインタ読み) とは
 * 場所で見分ける: 材料を読んだ直後の * は掛け算。
 * % は剰余 (v19 で追加) */
static int parse_term_expr(void) {
    int lhs = parse_unary_expr();
    if (lhs < 0)
        return -1;
    for (;;) {
        skip_spaces();
        int kind;
        if (*cur == '*')
            kind = NODE_MUL;
        else if (*cur == '/')
            kind = NODE_DIV;
        else if (*cur == '%')
            kind = NODE_MOD;
        else
            return lhs;
        cur++;
        skip_spaces();
        int rhs = parse_unary_expr();
        if (rhs < 0)
            return -1;
        lhs = new_node(kind, 0, lhs, rhs);
        if (lhs < 0)
            return -1;
    }
}

/* 足し引きの段 (add) を読む。相手は term の段 */
static int parse_add_expr(void) {
    int lhs = parse_term_expr();
    if (lhs < 0)
        return -1;
    for (;;) {
        skip_spaces();
        int kind;
        if (*cur == '+')
            kind = NODE_ADD;
        else if (*cur == '-')
            kind = NODE_SUB;
        else
            return lhs;
        cur++;
        skip_spaces();
        int rhs = parse_term_expr();
        if (rhs < 0)
            return -1;
        lhs = new_node(kind, 0, lhs, rhs);
        if (lhs < 0)
            return -1;
    }
}

/* 比較の段 (eq) を読む。相手は rel の段 (v11 から)。
 * 結果は「成り立てば 1、だめなら 0」の数になる */
static int parse_eq_expr(void) {
    int lhs = parse_rel_expr();
    if (lhs < 0)
        return -1;
    for (;;) {
        skip_spaces();
        int kind;
        if (cur[0] == '=' && cur[1] == '=')
            kind = NODE_EQ;
        else if (cur[0] == '!' && cur[1] == '=')
            kind = NODE_NE;
        else
            return lhs;
        cur += 2;
        skip_spaces();
        int rhs = parse_rel_expr();
        if (rhs < 0)
            return -1;
        lhs = new_node(kind, 0, lhs, rhs);
        if (lhs < 0)
            return -1;
    }
}

/* シフトの段 (v19): << >> を読む。足し算よりゆるく、
 * 関係演算子より固い (C の順)。>> は符号付きの算術
 * シフトとして生成する */
static int parse_shift_expr(void) {
    int lhs = parse_add_expr();
    if (lhs < 0)
        return -1;
    for (;;) {
        skip_spaces();
        int kind;
        if (cur[0] == '<' && cur[1] == '<')
            kind = NODE_SHL;
        else if (cur[0] == '>' && cur[1] == '>')
            kind = NODE_SHR;
        else
            return lhs;
        cur += 2;
        skip_spaces();
        int rhs = parse_add_expr();
        if (rhs < 0)
            return -1;
        lhs = new_node(kind, 0, lhs, rhs);
        if (lhs < 0)
            return -1;
    }
}

/* 関係演算子の段 (v11): < > <= >= を読む。
 * C の優先順位では、シフトよりゆるく、== != より固い。
 * 例: "i + 1 < 10" は <(+(i, 1), 10)、"a < b == c" は
 * ==(<(a, b), c) という木になる (C と同じ読み)。
 * <= と >= は2文字で読む (< の次が = でも、代入や
 * == と取り違えないよう、ここでまとめて処理する) */
static int parse_rel_expr(void) {
    int lhs = parse_shift_expr();
    if (lhs < 0)
        return -1;
    for (;;) {
        skip_spaces();
        int kind;
        int len;
        if (cur[0] == '<' && cur[1] == '=') {
            kind = NODE_LE;
            len = 2;
        } else if (cur[0] == '>' && cur[1] == '=') {
            kind = NODE_GE;
            len = 2;
        } else if (cur[0] == '<') {
            kind = NODE_LT;
            len = 1;
        } else if (cur[0] == '>') {
            kind = NODE_GT;
            len = 1;
        } else
            return lhs;
        cur += len;
        skip_spaces();
        int rhs = parse_shift_expr();
        if (rhs < 0)
            return -1;
        lhs = new_node(kind, 0, lhs, rhs);
        if (lhs < 0)
            return -1;
    }
}

/* ビット AND の段 (v19 で独立した段になった)。
 * 例: "*p & 32 == 0" は &(*p, ==(32, 0)) という木になる
 * (C と同じ読み)。&& は上の段 (論理 AND) の担当なので、
 * ここでは手をつけずに返す */
static int parse_bitand_expr(void) {
    int lhs = parse_eq_expr();
    if (lhs < 0)
        return -1;
    for (;;) {
        skip_spaces();
        if (*cur != '&' || cur[1] == '&')
            return lhs;
        cur++;
        skip_spaces();
        int rhs = parse_eq_expr();
        if (rhs < 0)
            return -1;
        lhs = new_node(NODE_AND, 0, lhs, rhs);
        if (lhs < 0)
            return -1;
    }
}

/* ビット XOR の段 (v19): & よりゆるく、| より固い */
static int parse_bitxor_expr(void) {
    int lhs = parse_bitand_expr();
    if (lhs < 0)
        return -1;
    for (;;) {
        skip_spaces();
        if (*cur != '^')
            return lhs;
        cur++;
        skip_spaces();
        int rhs = parse_bitand_expr();
        if (rhs < 0)
            return -1;
        lhs = new_node(NODE_XOR, 0, lhs, rhs);
        if (lhs < 0)
            return -1;
    }
}

/* ビット OR の段 (v19): ^ よりゆるく、&& より固い。
 * || は上の段 (論理 OR) の担当なので、手をつけない */
static int parse_bitor_expr(void) {
    int lhs = parse_bitxor_expr();
    if (lhs < 0)
        return -1;
    for (;;) {
        skip_spaces();
        if (*cur != '|' || cur[1] == '|')
            return lhs;
        cur++;
        skip_spaces();
        int rhs = parse_bitxor_expr();
        if (rhs < 0)
            return -1;
        lhs = new_node(NODE_OR, 0, lhs, rhs);
        if (lhs < 0)
            return -1;
    }
}

/* 論理 AND の段 (v19): && は短絡評価 (左が 0 なら右を
 * 評価しない) で、結果は 1 か 0 */
static int parse_land_expr(void) {
    int lhs = parse_bitor_expr();
    if (lhs < 0)
        return -1;
    for (;;) {
        skip_spaces();
        if (cur[0] != '&' || cur[1] != '&')
            return lhs;
        cur += 2;
        skip_spaces();
        int rhs = parse_bitor_expr();
        if (rhs < 0)
            return -1;
        lhs = new_node(NODE_LAND, 0, lhs, rhs);
        if (lhs < 0)
            return -1;
    }
}

/* 論理 OR の段 (v19)。式全体の入口は、この段の上に
 * 三項演算子を載せた parse_expr (v25)。
 * 例: "*s != 0" は !=(*s, 0)、"a || b && c" は
 * ||(a, &&(b, c)) という木になる (C と同じ読み) */
static int parse_lor_expr(void) {
    int lhs = parse_land_expr();
    if (lhs < 0)
        return -1;
    for (;;) {
        skip_spaces();
        if (cur[0] != '|' || cur[1] != '|')
            return lhs;
        cur += 2;
        skip_spaces();
        int rhs = parse_land_expr();
        if (rhs < 0)
            return -1;
        lhs = new_node(NODE_LOR, 0, lhs, rhs);
        if (lhs < 0)
            return -1;
    }
}

/* 式の最上位 (v25 で三項演算子を追加):
 *   条件 ? 真の式 : 偽の式
 * 三項は右結合 (? の中と : の後はまた式を読む)。
 * 成り立たない側は実行しない (短絡) のは生成の仕事 */
static int parse_expr(void) {
    int cond = parse_lor_expr();
    if (cond < 0)
        return -1;
    skip_spaces();
    if (*cur != '?')
        return cond;
    cur++;
    int texpr = parse_expr();
    if (texpr < 0)
        return -1;
    if (!expect_char(':', "三項演算子には : が必要です"))
        return -1;
    int fexpr = parse_expr();
    if (fexpr < 0)
        return -1;
    return new_node(NODE_COND, texpr, cond, fexpr);
}

/* 読み取ったばかりの識別子 (ident_buf) が、指定した単語と
 * 一致するか。while や putc の予約語の判定に使う */
static int ident_is(char *word) {
    int i = 0;
    while (word[i] != '\0') {
        if (i >= ident_len || ident_buf[i] != word[i])
            return 0;
        i++;
    }
    return i == ident_len;
}

/* 期待する1文字があるか確かめて、あれば進む。無ければエラー */
static int expect_char(char c, char *msg) {
    skip_spaces();
    if (*cur != c) {
        errmsg = msg;
        return 0;
    }
    cur++;
    return 1;
}

/* 文を1つ読む関数は互いに呼び合う (while の本体が文の並びで、
 * その文にまた while がありうる) ので、先に宣言だけ置く */
static int parse_stmt(void);
static int parse_typedef(void);  /* typedef (本体は宣言の節、v36) */
static int parse_struct_def(void);  /* 構造体の定義 (同上、v37) */
static int parse_decl_stmt(void);   /* 宣言文 (同上、v37) */
static int parse_stmt_list(int in_brace);
static int parse_body(void);  /* 制御文の本体 (v25) */
static int eval_const(int node, long *out);  /* 定数式の評価
                             * (本体は宣言の節。switch の
                             * case ラベルで使うため、v43 で
                             * 先行宣言を追加) */

/* parse の状態 (v20): break / continue が許される深さと、
 * return が許される場所を parse の時点で確かめるために持つ */
static int parse_loop_depth;  /* while / for の本体の中なら 1 以上 */
static int parse_switch_depth; /* switch の本体の中なら 1 以上 (v43) */
static int parse_in_func;     /* 関数の本体の中なら 1 */

/* 今の位置の先 (空白の後ろ) に '*' があるか (v25)。
 * キャストと (void) 判定の先読みに使う */
static int is_ptr_ahead(void) {
    char *p = cur;
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')
        p++;
    return *p == '*';
}

/* 型を1つ読む: [ const ] 型の言葉 { 型の言葉 } { * }
 * 型は今は読み捨てるので、形だけ確かめて進む。
 * 単語が型でなければ 0 を返す */
/* 型を表す言葉かどうか (v16 で語彙を増やした: long /
 * unsigned / signed / short。型そのものは従来どおり
 * 読み捨てるが、宣言の形として受け付ける) */
static int is_type_word(void) {
    return ident_is("void") || ident_is("char") ||
           ident_is("int") || ident_is("long") ||
           ident_is("short") || ident_is("unsigned") ||
           ident_is("signed") ||
           ident_is("int16") || ident_is("uint16") ||
           ident_is("int32") || ident_is("uint32") ||
           ident_is("int64") || ident_is("uint64");
}

/* 幅つきの型の言葉なら、その型タグを返す (v35)。
 * 違えば 0 */
static int sized_type_etype(void) {
    if (ident_is("int16"))  return 2 + 32;
    if (ident_is("uint16")) return 2 + 16 + 32;
    if (ident_is("int32"))  return 4 + 32;
    if (ident_is("uint32")) return 4 + 16 + 32;
    if (ident_is("int64"))  return 8 + 32;
    if (ident_is("uint64")) return 8 + 16 + 32;
    return 0;
}

/* 直前に読んだ型の情報 (v21): 宣言が変数を作るときに使う。
 * 先読みで呼ばれた場合は上書きされるが、本読みで最後に
 * 残るのが正しい値になる */
/* 今読んだ識別子が typedef 名かどうか (v36) */
static int is_typedef_name(void) {
    return find_typedef(ident_buf, ident_len) >= 0;
}

static int parse_type_bsize;   /* char を含めば 1、それ以外は 8 */
/* parse_type_struct は前方で宣言済み (v37) */
/* parse_type_plevel と parse_type_etype は前方で宣言済み */

static int parse_type(void) {
    parse_type_struct = -1;
    if (!is_ident_start(*cur))
        return 0;
    if (!parse_ident())
        return 0;
    if (ident_is("const") || ident_is("static")) {
        /* 修飾子 (v25 で static を追加): どちらも型の
         * 意味には影響しないので、読み捨てて次へ進む。
         * iwac 自身のソースは static を多用する */
        skip_spaces();
        if (!parse_ident())
            return 0;
    }
    if (ident_is("struct")) {
        /* struct タグ (v37): タグ表を引く。型タグと
         * 大きさは構造体のものを入れ、ポインタ段数は
         * 後ろの * で数える */
        skip_spaces();
        if (!parse_ident())
            return 0;
        int tg = find_struct_tag(ident_buf, ident_len);
        if (tg < 0)
            return 0;
        skip_spaces();
        int stars = 0;
        while (*cur == '*') {  /* ポインタの印を数える */
            stars++;
            cur++;
            skip_spaces();
        }
        parse_type_struct = tg;
        parse_type_etype = 8;
        parse_type_bsize = stag_size[tg];
        parse_type_plevel = stars;
        return 1;
    }
    if (!is_type_word()) {
        /* typedef 名なら名簿を引く (v36)。型の言葉の
         * 続き (unsigned long のような組み合わせ) は
         * 無く、後ろの * だけがポインタ段数に足される */
        int td = find_typedef(ident_buf, ident_len);
        if (td < 0)
            return 0;
        skip_spaces();
        int stars = 0;
        while (*cur == '*') {  /* ポインタの印を数える */
            stars++;
            cur++;
            skip_spaces();
        }
        parse_type_etype = tdef_etype[td];
        parse_type_bsize = tdef_bsize[td];
        parse_type_plevel = tdef_plevel[td] + stars;
        parse_type_struct = tdef_struct[td] - 1;
        return 1;
    }
    int has_char = ident_is("char");
    int sized = sized_type_etype();  /* 幅つきの型 (v35) */
    /* 型の言葉が続く間は読み進める (unsigned long や
     * long long のような組み合わせを受けるため) */
    for (;;) {
        skip_spaces();
        if (!is_ident_start(*cur))
            break;
        /* 次の言葉を取ってみて、型の言葉でなければ
         * カーソルを戻す (それは変数名や関数名のはず) */
        char *save = cur;
        if (!parse_ident())
            return 0;
        if (!is_type_word()) {
            cur = save;
            break;
        }
        if (ident_is("char"))
            has_char = 1;
        int sk = sized_type_etype();
        if (sk)
            sized = sk;
    }
    skip_spaces();
    int stars = 0;
    while (*cur == '*') {  /* ポインタの印を数える (v21) */
        stars++;
        cur++;
        skip_spaces();
    }
    if (sized) {
        /* 幅つきの型があれば、それが型そのもの */
        parse_type_etype = sized;
        parse_type_bsize = sized & 15;
    } else {
        parse_type_bsize = has_char ? 1 : 8;
        parse_type_etype = parse_type_bsize;
    }
    parse_type_plevel = stars;
    return 1;
}

/* 関数定義、またはプロトタイプ (宣言) を読む:
 *   型 名前 ( [引数の並び] ) { 文の並び }   定義
 *   型 名前 ( [引数の並び] ) ;              宣言だけ (v24)
 * 成功したら 1、失敗したら 0。関数は本体を読む前に名簿へ
 * 載せるので、自分自身を呼ぶ (再帰の) 形も読める。
 * 宣言が先にあれば、定義はその名簿の続きとして読む。
 * 相互再帰 (互いを呼び合う関数) は宣言で可能になる。
 * 引数は8つまで (v22 で4つ、v25 で8つに拡大。a0〜a7)。
 * 定義では引数の名前が変数として
 * 名簿に載る。宣言では名前は省略でき、あっても捨てる */
static int parse_funcdef(void) {
    if (!parse_type()) {
        errmsg = "関数は 型 名前 ( 引数 ) { } の形で書きます";
        return 0;
    }
    skip_spaces();
    if (!parse_ident()) {
        errmsg = "関数の名前が必要です";
        return 0;
    }
    char name_buf[32];
    int name_len = ident_len;
    for (int i = 0; i < name_len; i++)
        name_buf[i] = ident_buf[i];
    /* 名簿にあれば、その関数の続き (宣言済みなら定義が
     * 来たことになる、v24)。本体が既にあれば二重定義 */
    int f = find_func(name_buf, name_len);
    int is_new = 0;
    if (f < 0) {
        if (nfunc >= 192) {
            errmsg = "関数は128個までです";
            return 0;
        }
        f = nfunc++;
        is_new = 1;
        for (int i = 0; i < name_len; i++)
            func_names[f][i] = name_buf[i];
        func_name_len[f] = name_len;
        func_nparams[f] = 0;
        func_body[f] = -1;
        func_nlocals[f] = 0;
        func_arr_bytes[f] = 0;
        func_has_body[f] = 0;
        func_called[f] = 0;
    }
    func_ret_etype[f] = parse_type_etype;  /* 戻り値の型 (v35) */
    func_ret_struct[f] = parse_type_struct + 1;  /* 戻り値の構造体 (v37) */
    if (parse_type_struct >= 0 && parse_type_plevel == 0) {
        errmsg = "構造体は値で返せません (ポインタで返してください)";
        return 0;
    }
    if (!expect_char('(', "関数名の後には ( が必要です"))
        return 0;
    /* 引数はまず「型と名前」を集めるだけにする。変数
     * としての登録は、本体のある定義だと分かった後で
     * 行う (宣言だけのプロトタイプでは、引数の名前は
     * 意味を持たないため。名前の省略も宣言では許す) */
    char pnames[8][32];
    int plens[8];
    int pbs[8];
    int pet[8];   /* 引数の型タグ (v35) */
    int ppl[8];
    int pst[8];   /* 引数の構造体番号 (v37) */
    int np = 0;
    int params_done = 0;  /* (void) は引数なしと同義 (v25) */
    skip_spaces();
    if (*cur == ')') {
        cur++;  /* 引数なし */
        params_done = 1;
    } else {
        skip_spaces();
        char *save_v = cur;
        if (parse_ident() && ident_is("void") && !is_ptr_ahead()) {
            /* 先読み '(void)': 型が void だけで、後に名前も
             * * も続かないなら引数なしの宣言 (C の書き方)。
             * void 型の引数が1つ、の形は iwac では使わない */
            skip_spaces();
            if (*cur == ')')
                params_done = 1;
        }
        if (!params_done)
            cur = save_v;  /* (void) でなければ読み直す */
    }
    if (params_done) {
        if (*cur == ')')
            cur++;  /* ')' を消費する */
    } else {
        /* 引数の並び: 型 [名前] (, 型 [名前])* (v22 で複数) */
        for (;;) {
            if (np >= 8) {
                errmsg = "引数は8つまでです";
                return 0;
            }
            skip_spaces();  /* カンマの後の空白 (parse_type は
                             * 空白を飛ばして始まらないため) */
            if (!parse_type()) {
                errmsg = "引数は 型 名前 の形で書きます";
                return 0;
            }
            pbs[np] = parse_type_bsize;
            pet[np] = parse_type_etype;
            ppl[np] = parse_type_plevel;
            pst[np] = parse_type_struct;
            if (pst[np] >= 0 && ppl[np] == 0) {
                errmsg = "構造体は値で渡せません (ポインタで渡してください)";
                return 0;
            }
            plens[np] = 0;
            skip_spaces();
            if (is_ident_start(*cur)) {
                if (!parse_ident())
                    return 0;
                plens[np] = ident_len;
                for (int i = 0; i < ident_len; i++)
                    pnames[np][i] = ident_buf[i];
            }
            np++;
            skip_spaces();
            if (*cur == ',') {
                cur++;
                continue;
            }
            break;
        }
        if (!expect_char(')', "引数の後には ) が必要です"))
            return 0;
    }
    if (!is_new && func_nparams[f] != np) {
        errmsg = "宣言と定義で引数の数が合いません";
        return 0;
    }
    func_nparams[f] = np;
    skip_spaces();
    if (*cur == ';') {
        /* プロトタイプ (宣言だけ、v24)。本体は後から
         * 定義されてもよいし、最後まで無ければ外部の
         * 関数として、呼び出しが残れば生成でエラー */
        cur++;
        return 1;
    }
    if (!expect_char('{', "関数の本体は { で始めるか、宣言は ; で終えます"))
        return 0;
    /* 定義が来た。既に本体があれば二重定義 (v33:
     * プロトタイプは定義の前でも後でもよいが、
     * 本体は1つだけ。C の作法と同じ) */
    if (func_has_body[f]) {
        errmsg = "同じ名前の関数が二つあります";
        return 0;
    }
    /* 定義: 引数の変数を登録する (名前が必須) */
    int save_cur_func = parse_cur_func;
    parse_cur_func = f;
    for (int k = 0; k < np; k++) {
        if (plens[k] == 0) {
            errmsg = "引数の名前が必要です";
            return 0;
        }
        int var = define_var_full(pnames[k], plens[k], pbs[k],
                                  ppl[k], 1, pet[k], pst[k], 1);
        if (var < 0)
            return 0;
        func_param_var[f][k] = var;
    }
    /* main という名前の関数は特別扱い (v12): プログラムの
     * 入口になる。入口そのものの生成は gen_all が行う
     * (main を呼び出す jal を置く)。v22 からは main も
     * 引数を持てる: KosabaOS ターゲットでは入口が決め打ち
     * の argv を a0 / a1 に渡す (gen_all の説明参照)。
     * ベアメタルでは引数は用意されない (値は不定) */
    if (name_len == 4 && name_buf[0] == 'm' && name_buf[1] == 'a' &&
        name_buf[2] == 'i' && name_buf[3] == 'n') {
        prog_main = f;
    }
    int save_in_func = parse_in_func;
    parse_in_func = 1;
    int body = parse_stmt_list(1);
    parse_in_func = save_in_func;
    parse_cur_func = save_cur_func;
    if (body == -2)
        return 0;
    func_body[f] = body;
    func_has_body[f] = 1;
    return 1;
}


/* for の更新部を読む (v20): ')' の手前まで。
 * 無ければ -1、エラーは -2、読めたら文のノードを返す。
 * 形は「代入」か「++/--」だけ (カンマ区切りは対象外) */
static int parse_for_step(void) {
    skip_spaces();
    if (*cur == ')')
        return -1;  /* 更新なし */
    if ((cur[0] == '+' && cur[1] == '+') ||
        (cur[0] == '-' && cur[1] == '-')) {
        int e = parse_unary_expr();
        if (e < 0)
            return -2;
        return e;  /* 前置の増減 */
    }
    if (!is_ident_start(*cur)) {
        errmsg = "for の更新は 代入 か ++/-- で書きます";
        return -2;
    }
    if (!parse_ident())
        return -2;
    char name_buf[32];
    int name_len = ident_len;
    for (int i = 0; i < name_len; i++)
        name_buf[i] = ident_buf[i];
    skip_spaces();
    if ((cur[0] == '+' && cur[1] == '+') ||
        (cur[0] == '-' && cur[1] == '-')) {
        int var = find_var(name_buf, name_len);
        if (var < 0) {
            errmsg = "代入されていない変数が使われました";
            return -2;
        }
        int kind = cur[0] == '+' ? NODE_POSTINC : NODE_POSTDEC;
        cur += 2;
        int n = new_node(kind, var, 0, 0);
        if (n < 0)
            return -2;
        return n;
    }
    if (*cur == '=' && cur[1] != '=') {
        /* 代入: 右辺を読んでから名簿に載せる (代入文と同じ) */
        cur++;
        int expr = parse_expr();
        if (expr < 0)
            return -2;
        int var = define_var(name_buf, name_len);
        if (var < 0)
            return -2;
        int n = new_node(NODE_ASSIGN, var, expr, 0);
        if (n < 0)
            return -2;
        return n;
    }
    errmsg = "for の更新は 代入 か ++/-- で書きます";
    return -2;
}

/* 文を1つ読み、その AST ノードを返す。失敗したら -1。
 * 呼び出し時点でカーソルは文の頭にあること */
/* 制御文の本体を読む (v25): { 文の並び } でも文1つでも
 * よい (C の本来の形)。失敗したら -2、空の本体なら -1、
 * それ以外は本体の先頭ノードを返す (parse_stmt_list と
 * 同じ約束)。v24 までは { } が必須だった */
static int parse_body(void) {
    skip_spaces();
    if (*cur == '{') {
        cur++;
        return parse_stmt_list(1);
    }
    int s = parse_stmt();
    if (s < 0)
        return -2;
    return s;
}

static int parse_stmt(void) {
    skip_spaces();
    if (*cur == '{') {
        /* 裸のブロック (v25): { 文の並び } 自体が文になる。
         * 中身の並びをそのまま返す */
        cur++;
        int body = parse_stmt_list(1);
        if (body == -2)
            return -1;
        return body;
    }
    if (is_ident_start(*cur)) {
        /* 型で始まる文は宣言 (関数定義は頭専用で、
         * parse_stmt_list がそちらへ振り分ける) */
        char *tsave = cur;
        if (parse_ident() &&
            (is_type_word() || ident_is("const") || ident_is("static") ||
             is_typedef_name() || ident_is("struct"))) {
            cur = tsave;
            return parse_decl_stmt();
        }
        cur = tsave;
    }
    if (*cur == '*') {
        /* メモリ書き込み文: * 先 = 式 ;
         * 先は変数 (ポインタ) か数 (絶対アドレス)。
         * デバイスへ書くのはこの形: *0x10000000 = c; は
         * 「UART の送信窓口へ c を置く = 送信する」になる */
        cur++;
        skip_spaces();
        int addr = parse_primary_node();
        if (addr < 0)
            return -1;
        int akind = node_kind[addr];
        if (akind != NODE_VAR && akind != NODE_NUM) {
            errmsg = "書き込む先は変数 (ポインタ) かアドレス (数) です";
            return -1;
        }
        if (!expect_char('=', "メモリ書き込みは *先 = 式 ; の形です"))
            return -1;
        int val = parse_expr();
        if (val < 0)
            return -1;
        if (!expect_char(';', "書き込み文の終わりには ; が必要です"))
            return -1;
        return new_node(NODE_STORE, 0, addr, val);
    }
    if ((cur[0] == '+' && cur[1] == '+') ||
        (cur[0] == '-' && cur[1] == '-')) {
        /* 前置の増減文: ++ 名前 ; / -- 名前 ; (v19) */
        int e = parse_unary_expr();
        if (e < 0)
            return -1;
        if (!expect_char(';', "文の終わりには ; が必要です"))
            return -1;
        return e;
    }
    char *stmt_name_start = cur;  /* 要素の増減文の読み直し用 */
    if (!parse_ident())
        return -1;

    if (ident_is("while")) {
        /* while ( 条件 ) 本体。本体は { } か文1つ (v25) */
        if (!expect_char('(', "while の後には ( が必要です"))
            return -1;
        int cond = parse_expr();
        if (cond < 0)
            return -1;
        if (!expect_char(')', "while の条件の後には ) が必要です"))
            return -1;
        parse_loop_depth++;
        int body = parse_body();
        parse_loop_depth--;
        if (body == -2)
            return -1;
        return new_node(NODE_WHILE, 0, cond, body);
    }

    if (ident_is("putc")) {
        /* putc ( 式 ); -- 組み込みの出力文 */
        if (!expect_char('(', "putc の後には ( が必要です"))
            return -1;
        int expr = parse_expr();
        if (expr < 0)
            return -1;
        if (!expect_char(')', "putc の式の後には ) が必要です"))
            return -1;
        if (!expect_char(';', "putc 文の終わりには ; が必要です"))
            return -1;
        return new_node(NODE_PUTC, 0, expr, 0);
    }

    if (ident_is("if")) {
        /* if ( 条件 ) 本体 [ else 本体 ] (v25)。
         * 本体は { } でも文1つでもよい。else if は
         * 「else の本体が if 文1つ」として自然に読める */
        if (!expect_char('(', "if の後には ( が必要です"))
            return -1;
        int cond = parse_expr();
        if (cond < 0)
            return -1;
        if (!expect_char(')', "if の条件の後には ) が必要です"))
            return -1;
        int then_body = parse_body();
        if (then_body == -2)
            return -1;
        /* else があるかを先読みする (無ければ巻き戻す) */
        int else_body = -1;
        char *save = cur;
        skip_spaces();
        if (is_ident_start(*cur) && parse_ident() &&
            ident_is("else")) {
            else_body = parse_body();
            if (else_body == -2)
                return -1;
        } else {
            cur = save;  /* else ではなかった */
        }
        return new_node(NODE_IF, else_body, cond, then_body);
    }

    if (ident_is("for")) {
        /* for ( 初期化 ; 条件 ; 更新 ) { 本体 } (v20)。
         * while への読み替えで作る: 初期化は for の前に1回、
         * 更新は本体の最後に (NODE_FORSTEP で包んで) 繋ぐ。
         * 条件が空なら「数の 1」(= 常に成り立つ) にする */
        if (!expect_char('(', "for の後には ( が必要です"))
            return -1;
        int init = -1;
        skip_spaces();
        if (*cur == ';') {
            cur++;  /* 初期化なし */
        } else {
            /* 型で始まれば宣言つきの初期化 (for (int i = 0; …)、
             * v21 で対応)。それ以外は普通の文 (代入文など) */
            char *save = cur;
            int is_decl = is_ident_start(*cur) && parse_ident() &&
                          (is_type_word() || ident_is("const") || ident_is("static"));
            cur = save;
            if (is_decl) {
                init = parse_decl_stmt();  /* ';' まで読む */
            } else {
                init = parse_stmt();  /* ';' まで読む (代入文など) */
            }
            if (init < 0)
                return -1;
        }
        int cond;
        skip_spaces();
        if (*cur == ';') {
            cond = new_node(NODE_NUM, 1, 0, 0);
            if (cond < 0)
                return -1;
            cur++;  /* 条件なし = 常に成り立つ */
        } else {
            cond = parse_expr();
            if (cond < 0)
                return -1;
            if (!expect_char(';', "for の条件の後には ; が必要です"))
                return -1;
        }
        int step = parse_for_step();
        if (step == -2)
            return -1;
        if (!expect_char(')', "for の後には ) が必要です"))
            return -1;
        parse_loop_depth++;
        int body = parse_body();
        parse_loop_depth--;
        if (body == -2)
            return -1;
        if (step >= 0) {
            /* 更新は本体の最後に繋ぐ。continue の飛び先が
             * この文の頭になる (生成側の約束) */
            int wrap = new_node(NODE_FORSTEP, step, 0, 0);
            if (wrap < 0)
                return -1;
            if (body < 0)
                body = wrap;
            else {
                int tail = body;
                while (node_next[tail] >= 0)
                    tail = node_next[tail];
                node_next[tail] = wrap;
            }
        }
        return new_node(NODE_FOR, init, cond, body);
    }

    if (ident_is("switch")) {
        /* switch ( 制御式 ) { case 群... } (v43、設計は
         * doc/switch.md)。本体は { } 必須。case / default の
         * ラベルで区切られた文の並びを NODE_CASE の連鎖にし、
         * NODE_SWITCH の rhs に置く。ラベルの定数は
         * eval_const で確定し、重複はエラーにする */
        if (!expect_char('(', "switch の後には ( が必要です"))
            return -1;
        int cond = parse_expr();
        if (cond < 0)
            return -1;
        if (!expect_char(')', "switch の制御式の後には ) が必要です"))
            return -1;
        skip_spaces();
        if (*cur != '{') {
            errmsg = "switch の本体は { } で書きます";
            return -1;
        }
        cur++;
        parse_switch_depth++;
        int groups = -1;
        int gtail = -1;
        int group = -1;   /* 今の NODE_CASE */
        int stail = -1;   /* 今の case の文の末尾 */
        int seen_default = 0;
        int failed = 0;
        for (;;) {
            skip_spaces();
            if (*cur == '}') {
                cur++;
                break;
            }
            if (*cur == '\0') {
                errmsg = "switch の本体が } で閉じていません";
                failed = 1;
                break;
            }
            /* ラベルかどうかを先読みする */
            char *save = cur;
            int is_label = 0;
            int is_default = 0;
            int label_val = 0;
            if (is_ident_start(*cur) && parse_ident()) {
                if (ident_is("case")) {
                    int e = parse_expr();
                    if (e < 0) {
                        failed = 1;
                        break;
                    }
                    long v;
                    if (eval_const(e, &v) != 1) {
                        errmsg = "case の後には定数が必要です";
                        failed = 1;
                        break;
                    }
                    if (v > 2147483647L || v < -2147483647L - 1) {
                        errmsg = "case の値が大きすぎます";
                        failed = 1;
                        break;
                    }
                    label_val = (int)v;
                    is_label = 1;
                } else if (ident_is("default")) {
                    is_default = 1;
                    is_label = 1;
                }
            }
            if (is_label) {
                if (!expect_char(':',
                                 "case / default の後には : が必要です")) {
                    failed = 1;
                    break;
                }
                if (is_default) {
                    if (seen_default) {
                        errmsg = "default は1つだけです";
                        failed = 1;
                        break;
                    }
                    seen_default = 1;
                } else {
                    int dup = 0;
                    for (int g = groups; g != -1; g = node_next[g]) {
                        if (node_rhs[g] == 0 &&
                            node_val[g] == label_val)
                            dup = 1;
                    }
                    if (dup) {
                        errmsg = "case の値が重なっています";
                        failed = 1;
                        break;
                    }
                }
                int gnode = new_node(NODE_CASE, label_val, -1,
                                     is_default);
                if (gnode < 0) {
                    failed = 1;
                    break;
                }
                if (groups < 0)
                    groups = gnode;
                else
                    node_next[gtail] = gnode;
                gtail = gnode;
                group = gnode;
                stail = -1;
                continue;
            }
            cur = save;  /* ラベルではなかった。文として読む */
            if (group < 0) {
                errmsg = "switch の本体は case か default から書きます";
                failed = 1;
                break;
            }
            int s = parse_stmt();
            if (s < 0) {
                failed = 1;
                break;
            }
            if (node_lhs[group] < 0)
                node_lhs[group] = s;
            else
                node_next[stail] = s;
            stail = s;
            while (node_next[stail] >= 0)
                stail = node_next[stail];
        }
        parse_switch_depth--;
        if (failed)
            return -1;
        return new_node(NODE_SWITCH, 0, cond, groups);
    }

    if (ident_is("case") || ident_is("default")) {
        errmsg = "case と default は switch の中でだけ使えます";
        return -1;
    }

    if (ident_is("break")) {
        if (parse_loop_depth == 0 && parse_switch_depth == 0) {
            errmsg = "break はループか switch の中でだけ使えます";
            return -1;
        }
        if (!expect_char(';', "break の後には ; が必要です"))
            return -1;
        return new_node(NODE_BREAK, 0, 0, 0);
    }

    if (ident_is("continue")) {
        if (parse_loop_depth == 0) {
            errmsg = "continue はループの中でだけ使えます";
            return -1;
        }
        if (!expect_char(';', "continue の後には ; が必要です"))
            return -1;
        return new_node(NODE_CONTINUE, 0, 0, 0);
    }

    if (ident_is("return")) {
        /* return (v20 で値なし、v22 で戻り値つきも)。
         * 値があれば NODE_RETURN の node_lhs に式を置く */
        if (!parse_in_func) {
            errmsg = "return は関数の中でだけ使えます";
            return -1;
        }
        skip_spaces();
        if (*cur == ';') {
            cur++;
            return new_node(NODE_RETURN, 0, -1, 0);
        }
        int val = parse_expr();
        if (val < 0)
            return -1;
        if (!expect_char(';', "return の後には ; が必要です"))
            return -1;
        return new_node(NODE_RETURN, 0, val, 0);
    }

    char name_buf[32];
    int name_len = ident_len;
    for (int i = 0; i < name_len; i++)
        name_buf[i] = ident_buf[i];

    /* メンバへの書き込み・増減文 (v37): 名前から始まる
     * designator に . か -> が含まれていたら (先読みで
     * 確かめる)、頭に戻して後置の段で読み直し、= なら
     * 要素への書き込み (ELEM_STORE) にする */
    {
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
        }
        if (ok && has &&
            ((*p == '=' && p[1] != '=') ||
             (p[0] == '+' && p[1] == '+') ||
             (p[0] == '-' && p[1] == '-'))) {
            cur = stmt_name_start;
            int d = parse_postfix_expr();
            if (d < 0)
                return -1;
            skip_spaces();
            if (*cur == '=' && cur[1] != '=') {
                cur++;
                int val = parse_expr();
                if (val < 0)
                    return -1;
                if (!expect_char(';', "書き込み文の終わりには ; が必要です"))
                    return -1;
                if (node_kind[d] != NODE_MEMBER &&
                    node_kind[d] != NODE_MELEM) {
                    errmsg = "メンバへの書き込みの形が違います";
                    return -1;
                }
                return new_node(NODE_ELEM_STORE, 0, d, val);
            }
            if (!expect_char(';', "文の終わりには ; が必要です"))
                return -1;
            return d;  /* ++ / -- は後置の段が包んだ */
        }
    }

    /* 名前の次が '[' なら、要素への書き込み文:
     * 配列なら 名前 [ 添字 ] = 式 ; (2次元は [ 行 ] [ 列 ])、
     * ポインタ変数なら 名前 [ 添字 ] = 式 ; (v23) */
    skip_spaces();
    if (*cur == '[') {
        /* 要素への増減文 (v25): 名前 [ 添字 ] ++ ; の形は、
         * 添字列の後ろを先読みして見分ける。当たりなら
         * 巻き戻して式として読み直す (後置の道具が
         * 要素への増減ノードを作る) */
        {
            char *p = cur;
            int depth = 0;
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
            if (depth == 0 &&
                ((p[0] == '+' && p[1] == '+') ||
                 (p[0] == '-' && p[1] == '-'))) {
                /* 名前の頭に戻して式として読み直す */
                cur = stmt_name_start;
                int e = parse_unary_expr();
                if (e < 0)
                    return -1;
                if (!expect_char(';', "文の終わりには ; が必要です"))
                    return -1;
                return e;
            }
        }
        int a = find_array(name_buf, name_len);
        if (a >= 0) {
            cur++;
            int idx = parse_expr();
            if (idx < 0)
                return -1;
            if (!expect_char(']', "添字の後には ] が必要です"))
                return -1;
            skip_spaces();
            if (*cur == '[') {
                /* 2次元の要素への書き込み (v23)。
                 * 要素を表す式ノードを包んで文にする */
                if (arr_cols[a] == 0) {
                    errmsg = "1次元の配列に付けられる添字は1つだけです";
                    return -1;
                }
                cur++;
                int idx2 = parse_expr();
                if (idx2 < 0)
                    return -1;
                if (!expect_char(']', "添字の後には ] が必要です"))
                    return -1;
                if (!expect_char('=', "要素への書き込みは 名前[行][列] = 式 ; の形です"))
                    return -1;
                int val = parse_expr();
                if (val < 0)
                    return -1;
                if (!expect_char(';', "書き込み文の終わりには ; が必要です"))
                    return -1;
                int elem = new_node(NODE_INDEX2, a, idx, idx2);
                return new_node(NODE_ELEM_STORE, 0, elem, val);
            }
            if (!expect_char('=', "配列への書き込みは 名前[添字] = 式 ; の形です"))
                return -1;
            int val = parse_expr();
            if (val < 0)
                return -1;
            if (!expect_char(';', "書き込み文の終わりには ; が必要です"))
                return -1;
            return new_node(NODE_INDEX_STORE, a, idx, val);
        }
        /* ポインタの添字書き込み (v23) */
        int var = find_var(name_buf, name_len);
        if (var < 0 || var_plevel[var] < 1) {
            errmsg = "添字 [ ] を付けられるのは配列かポインタだけです";
            return -1;
        }
        cur++;
        int idx = parse_expr();
        if (idx < 0)
            return -1;
        if (!expect_char(']', "添字の後には ] が必要です"))
            return -1;
        if (!expect_char('=', "要素への書き込みは 名前[添字] = 式 ; の形です"))
            return -1;
        int val = parse_expr();
        if (val < 0)
            return -1;
        if (!expect_char(';', "書き込み文の終わりには ; が必要です"))
            return -1;
        int elem = new_node(NODE_PINDEX, var, 0, idx);
        return new_node(NODE_ELEM_STORE, 0, elem, val);
    }

    /* 名前の次が '(' なら、関数の呼び出し文:
     * 名前 ( [式の並び] ) ; 。作るノードは式としての
     * 呼び出し (parse_primary_node) と同じ NODE_CALL で、
     * 文の位置では戻り値を捨てる。関数は定義済みのもの
     * だけ呼べる (「使う前に定義」の約束は関数も同じ) */
    if (*cur == '(') {
        if (ident_is("syscall3")) {
            /* 組み込みの呼び出し文 (v27): 式として
             * 読み直して、戻り値は捨てる */
            cur = stmt_name_start;
            int e = parse_primary_node();
            if (e < 0)
                return -1;
            if (!expect_char(';', "呼び出し文の終わりには ; が必要です"))
                return -1;
            return e;
        }
        int f = find_func(name_buf, name_len);
        if (f < 0) {
            errmsg = "定義されていない関数が呼ばれました";
            return -1;
        }
        cur++;
        int args;
        if (!parse_call_list(f, &args))
            return -1;
        if (!expect_char(';', "呼び出し文の終わりには ; が必要です"))
            return -1;
        return new_node(NODE_CALL, f, args, 0);
    }

    /* 名前の次が ++ / -- なら、後置の増減文 (v19):
     * 名前 ++ ; / 名前 -- ; */
    skip_spaces();
    if ((cur[0] == '+' && cur[1] == '+') ||
        (cur[0] == '-' && cur[1] == '-')) {
        int var = find_var(name_buf, name_len);
        if (var < 0) {
            errmsg = "代入されていない変数が使われました";
            return -1;
        }
        int kind = cur[0] == '+' ? NODE_POSTINC : NODE_POSTDEC;
        cur += 2;
        if (!expect_char(';', "文の終わりには ; が必要です"))
            return -1;
        return new_node(kind, var, 0, 0);
    }

    /* 複合代入 (v25): 名前 op= 式 ; は
     * 名前 = 名前 op 式 ; と同じ意味として読む。
     * 対象は変数だけ (要素への複合代入は扱わない) */
    {
        int cop = -1;
        int clen = 0;
        if (cur[0] == '+' && cur[1] == '=') { cop = NODE_ADD; clen = 2; }
        else if (cur[0] == '-' && cur[1] == '=') { cop = NODE_SUB; clen = 2; }
        else if (cur[0] == '*' && cur[1] == '=') { cop = NODE_MUL; clen = 2; }
        else if (cur[0] == '/' && cur[1] == '=') { cop = NODE_DIV; clen = 2; }
        else if (cur[0] == '%' && cur[1] == '=') { cop = NODE_MOD; clen = 2; }
        else if (cur[0] == '<' && cur[1] == '<' && cur[2] == '=') { cop = NODE_SHL; clen = 3; }
        else if (cur[0] == '>' && cur[1] == '>' && cur[2] == '=') { cop = NODE_SHR; clen = 3; }
        else if (cur[0] == '&' && cur[1] == '=') { cop = NODE_AND; clen = 2; }
        else if (cur[0] == '|' && cur[1] == '=') { cop = NODE_OR; clen = 2; }
        else if (cur[0] == '^' && cur[1] == '=') { cop = NODE_XOR; clen = 2; }
        if (cop >= 0) {
            cur += clen;
            int cvar = find_var(name_buf, name_len);
            if (cvar < 0) {
                errmsg = "代入されていない変数が使われました";
                return -1;
            }
            int cexpr = parse_expr();
            if (cexpr < 0)
                return -1;
            int self = new_node(NODE_VAR, cvar, 0, 0);
            if (self < 0)
                return -1;
            int comb = new_node(cop, 0, self, cexpr);
            if (comb < 0)
                return -1;
            if (!expect_char(';', "代入文の終わりには ; が必要です"))
                return -1;
            return new_node(NODE_ASSIGN, cvar, comb, 0);
        }
    }

    /* 残る形は代入文: 名前 = 式 ;
     * 右辺を先に読んでから名簿に載せる (右辺の中で代入先
     * 自身を使うのは、初回代入ではまだ値が無いのでエラーに
     * なるのが正しい)。再代入では名簿に既にあるので読める */
    if (!expect_char('=', "文は 代入 (名前 = 式 ;) か while か putc か呼び出しです"))
        return -1;
    int expr = parse_expr();
    if (expr < 0)
        return -1;
    int var = define_var(name_buf, name_len);
    if (var < 0)
        return -1;
    if (!expect_char(';', "代入文の終わりには ; が必要です"))
        return -1;
    return new_node(NODE_ASSIGN, var, expr, 0);
}

/* プログラム全体を読む。成功したら 1、失敗したら 0。
 * 結果は prog_stmts / prog_final / prog_has_final に置く */
static int parse_program(char *program) {
    cur = program;
    nnode = 0;
    nvar = 0;
    ntdef = 0;
    nstag = 0;  /* 構造体もファイルごとに数え直す (v37) */
    nsmem = 0;
    var_nglobal = 0;
    parse_cur_func = -1;
    nstr = 0;
    str_used = 0;
    nfunc = 0;
    narr = 0;
    arr_used = 0;
    arr_init_used = 0;
    arr_init_end = 0;
    prog_stmts = -1;
    prog_final = -1;
    prog_has_final = 0;
    prog_main = -1;
    has_var_inits = 0;
    lex_failed = 0;
    skip_spaces();

    /* v0 互換: プログラムが丸ごと1文字 (数字以外) なら、
     * その文字コードを数のノードとして扱う。
     * (この互換のため、1文字の変数名だけのプログラムは
     * 「その変数の値」ではなく「その文字コード」になる) */
    if (cur[0] != '\0' && cur[1] == '\0' && !is_digit(cur[0])) {
        prog_final = new_node(NODE_NUM, (unsigned char)cur[0], 0, 0);
        if (prog_final < 0)
            return 0;
        prog_has_final = 1;
        return 1;
    }

    int head = parse_stmt_list(0);
    if (head == -2)
        return 0;
    if (lex_failed)
        return 0;  /* 字句の失敗 (コメントの閉じ忘れ等) */
    if (head == -1 && !prog_has_final && nfunc == 0) {
        /* 文も式も無い = 空のプログラム。文だけのプログラムは
         * よいが、空は書き間違いとしてエラーにしておく */
        errmsg = "プログラムが空です";
        return 0;
    }
    if (prog_main >= 0 && (head != -1 || prog_has_final)) {
        /* main の形と、外に文を書く形の混ぜ書きは禁止。
         * どちらかに統一する (C の作法に合わせる) */
        errmsg = "main があるプログラムでは、文は main の中に書きます";
        return 0;
    }
    prog_stmts = head;
    return 1;
}

