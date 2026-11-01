/* iwac -- Cコンパイラのサブセット (v3)
 *
 * === このコンパイラの内部構成 (3つの層) ===
 *
 * 普通のコンパイラは「ソース → アセンブリ → (アセンブラ) →
 * バイナリ」と進むが、iwac はアセンブラを持たず、直接
 * バイナリを出す。その代わり、iwac の内部を次の3層に
 * はっきり分けてある (設計書 doc/design.md 参照):
 *
 *   第1層 parse:   ソース文字列 → AST (抽象構文木)
 *   第2層 gen:     AST → 命令列 (機械語の一歩手前の形)
 *   第3層 encode:  命令列 → 32ビットの機械語 → ファイル出力
 *
 * 例: プログラム "65+1" は、層を進むごとにこう姿を変える:
 *
 *   第1層 AST:        +(65, 1) という木
 *                      ┌─ 65
 *                  (+)─┤
 *                      └─ 1
 *   第2層 命令列:     lui  t0, 0x10000
 *                     addi t1, zero, 65
 *                     addi t2, zero, 1
 *                     add  t1, t1, t2
 *                     sb   t1, 0(t0)
 *                     wfi
 *                     (これは「データとしての命令の並び」で、
 *                      アセンブリ文字列として存在するわけではない)
 *   第3層 バイナリ:   24バイトの機械語 (上の命令が各4バイト)
 *
 * 層を分ける意味:
 *   - AST があると、次のステップで演算子の優先順位や
 *     かっこが扱える (言語は木で考えるのが自然)
 *   - 命令列が分かれていると、「どんな命令を出すか」と
 *     「命令をどういうビット列にするか」という別の問題を
 *     別々に考え、確かめられる
 *   - 各層は前の層の成果物しか見ない。層同士が混ざらない
 *     ことが、このファイルの構成そのもので分かるようにする
 *
 * ファイルの並びも層の順: 外部宣言と道具 → 第1層 → 第2層 →
 * 第3層 → main (層を順に呼ぶだけ)。
 *
 * === v3 の言語 (文法) ===
 *
 *   program = 1文字                 (v0 との互換。その文字コードが値)
 *           | { stmt } expr         (文の並び + 最後の式)
 *   stmt    = ident "=" expr ";"    (代入文)
 *   expr    = primary { ("+" | "-" ) primary }
 *   primary = number | ident        (数、または変数)
 *           | string                (文字列。値はその住所)
 *           | "*" ident             (ポインタの先を読む)
 *   number  = 数字の並び (10進数、0..2047。理由は addi の項で)
 *   ident   = 英字か _ で始まる名前 (変数名)
 *   string  = '"' 文字の並び '"'    (エスケープは \n \t \\ \" )
 *
 *   例: "x = 65; x + 1"        → 66 になり 'B' が出る (v2)
 *       "s = \"Hi\"; *s"       → 'H' が出る (v3)
 *       "s = \"Hi\"; s = s + 1; *s" → 'i' が出る (v3)
 *
 * v3 で育ったのは「ポインタと文字列」。目標である KosabaOS の
 * print loop (uart_puts) に必要な部品の2つ目
 * (計画は doc/roadmap.md の「print loop」目標の節)。
 *
 * 文字列の置き場所: バイナリの「コードの後ろ」に文字列の
 * バイト列を並べる (最後に 0 を付けた C の文字列の形)。
 * 文字列リテラルの「値」は、その先頭の住所である。住所は
 * コードの長さが決まらないと分からないので、第2層 (gen) は
 * 2回生成する: 1回目で命令数を数え、文字列の住所を確定
 * させて、2回目で本物の住所を入れる (2パス生成)。
 *
 * ポインタは特別な型ではなく「住所が入った変数」にすぎない。
 * s = s + 1 は住所の足し算であり、*s は、その住所から
 * 1バイト読む (lbu) ことである。* の相手は今は変数だけ。
 * 変数は最大4個までで、実体はレジスタ (t3〜t6) に割り当てる。
 * 代入されていない変数を使うとエラーになる。
 *
 * 計算はコンパイラが代行せず、出力バイナリの中の add / sub が
 * 実行時に行う。最後の式の値の下1バイトが UART に出る。
 * シェルで実行するときは './iwac 'x=65; x+1'' のように引用符で囲む。
 *
 * === なぜこのファイルに #include が無いのか ===
 *
 * iwac の言語にはプリプロセッサが無いと決めた。なので iwac
 * 自身のソースもプリプロセッサ (#include も #define も)
 * 一切使わない。「まだ作っていないものを使って作らない」
 * ためで、将来のセルフホストのためでもある。
 * 標準ライブラリから使うのは write だけで、その宣言だけを
 * 下に自分で書く。宣言はプリプロセッサではなく、「こういう
 * 形の関数が外にある」というコンパイラへの伝言であり、
 * 本体はリンク時にホストのライブラリと繋がる。将来 iwac で
 * 書くプログラムも、#include の代わりに使う関数の宣言を
 * 自分で書く流儀にする想定。
 *
 * === 混同しやすい「2つの出力」 ===
 *
 *   - iwac 自身の出力先は「ホストの標準出力」。out_byte が
 *     命令のバイト列を標準出力に書く (> a.bin でファイルへ)。
 *   - 作られたバイナリの出力先は「ベアメタル機の UART」。
 *     バイナリの中の sb 命令が UART のレジスタに書く。
 */

/* 外部関数の宣言 (ヘッダの代わりに自分で書く)
 * write: fd へ buf の先頭 len バイトを書くホストの関数 */
long write(int fd, char *buf, long len);

/* ----------------------------------------------------------------
 * 道具: ホストへの出力と、エラー処理
 * ---------------------------------------------------------------- */

/* 文字列を fd に書く (長さは自分で数える) */
static void print_str(int fd, char *s) {
    long len = 0;
    while (s[len] != '\0')
        len++;
    write(fd, s, len);
}

/* 1バイトを標準出力 (fd = 1) に書く。第3層だけが使う */
static void out_byte(int byte) {
    char b = (char)byte;
    write(1, &b, 1);
}

/* エラーの置き場所。失敗した関数は理由をここに置き、
 * 「失敗」を戻り値 (-1 や 0) で伝える。main が表示する。
 * 解析や生成の段階では何も出力されていないので、失敗しても
 * 半端なバイナリが残ることはない */
static char *errmsg;

/* レジスタ番号の約束 (設計書にも記載。ABI 名と x 番号)
 * レジスタは番号で扱う: zero = 0、t0 = 5、t1 = 6、t2 = 7 */
static int REG_ZERO = 0;
static int REG_T0 = 5;  /* UART ベースアドレス専用 */
static int REG_T1 = 6;  /* 今の値 */
static int REG_T2 = 7;  /* 次の項 */
/* 変数は宣言された順に t3 (x28) から割り当てる。変数 i 番の
 * レジスタ番号は REG_VAR_BASE + i (最大4個なので t3〜t6) */
static int REG_VAR_BASE = 28;

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
                            * node_lhs = ポインタの変数ノード */

static int node_kind[512];
static int node_val[512];
static int node_lhs[512];
static int node_rhs[512];
static int nnode;  /* 作ったノードの数 */

/* --- 変数の名簿 (記号表) ---
 * 変数は「名前 → 番号」の対応表で管理する。番号が決まれば、
 * 第2層でレジスタ (t3 + 番号) に割り当てられる。
 * 名前は最大31文字、変数は最大4個まで (レジスタの数の都合) */
static char var_names[4][32];
static int var_name_len[4];
static int nvar;  /* 登録された変数の数 */

/* 名前 (場所と長さで渡す) が名簿にあればその番号、無ければ -1 */
static int find_var(char *name, int len) {
    for (int i = 0; i < nvar; i++) {
        if (var_name_len[i] != len)
            continue;
        int j = 0;
        while (j < len && var_names[i][j] == name[j])
            j++;
        if (j == len)
            return i;
    }
    return -1;
}

/* 名前を名簿に登録し、番号を返す。既にある名前なら同じ番号を
 * 返す (再代入は同じ変数への代入になる)。失敗したら -1 */
static int define_var(char *name, int len) {
    int found = find_var(name, len);
    if (found >= 0)
        return found;
    if (nvar >= 4) {
        errmsg = "変数は4個までです (レジスタ t3〜t6 の都合)";
        return -1;
    }
    int i = nvar++;
    for (int j = 0; j < len; j++)
        var_names[i][j] = name[j];
    var_name_len[i] = len;
    return i;
}

/* --- 代入文の並び ---
 * プログラムは「代入文の並び + 最後の式」でできている。
 * 代入文1つは (代入先の変数番号, 右辺の式の根ノード) の組。
 * 最後の式の根は parse_program の戻り値で返す */
static int stmt_var[128];
static int stmt_expr[128];
static int nstmt;  /* 代入文の数 */

/* --- 文字列の置き場 (文字列テーブル) ---
 * プログラム中に出てきた文字列リテラルを全部溜める。
 * str_pool には各文字列のバイト列が、末尾の 0 (文字列の
 * 終わり印) 付きで連続して並ぶ。文字列 i 番は:
 *   str_off[i] : str_pool の中の開始位置
 *   str_len[i] : 長さ (末尾の 0 は含まない)
 * 実行時の住所 (str_addr) は第2層が決める (コードの後ろに
 * このプールをそのまま置くので、住所 = 読み込み先頭 +
 * コードのバイト数 + str_off[i]) */
static char str_pool[4096];
static int str_off[64];
static int str_len[64];
static int nstr;       /* 文字列の数 */
static int str_used;   /* str_pool の使用済みバイト数 */
static unsigned int str_addr[64];  /* 第2層が確定させる住所 */

/* ノードを1つ作って、その番号を返す。失敗したら -1 */
static int new_node(int kind, int val, int lhs, int rhs) {
    if (nnode >= 512) {
        errmsg = "プログラムが大きすぎます (ノード数上限)";
        return -1;
    }
    int i = nnode++;
    node_kind[i] = kind;
    node_val[i] = val;
    node_lhs[i] = lhs;
    node_rhs[i] = rhs;
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
static void skip_spaces(void) {
    while (*cur == ' ' || *cur == '\t')
        cur++;
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
    int value = 0;
    while (is_digit(*cur)) {
        value = value * 10 + (*cur - '0');
        if (value > 2047) {
            errmsg = "項が大きすぎます (2047 まで。addi の即値が12ビットのため)";
            return -1;
        }
        cur++;
    }
    return value;
}

/* 数字を読み、その NODE_NUM ノードを作って番号を返す */
static int parse_number_node(void) {
    int value = parse_number_value();
    if (value < 0)
        return -1;
    return new_node(NODE_NUM, value, 0, 0);
}

/* 文字列リテラルを読み、文字列テーブルに登録して、その
 * NODE_STR ノードを作る。カーソルは開きの '"' の位置にある
 * 前提。エスケープは \n (改行) \t (タブ) \\ \" の4つだけ。
 * 閉じの '"' が無い、知らないエスケープ、長すぎる文字列は
 * エラー。成功したらノード番号、失敗したら -1 を返す */
static int parse_string_node(void) {
    if (nstr >= 64) {
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
            else if (*cur == '\\')
                c = '\\';
            else if (*cur == '"')
                c = '"';
            else {
                errmsg = "使えるエスケープは \\n \\t \\\\ \\\" だけです";
                return -1;
            }
        }
        if (str_used >= 4095) {
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

/* 式の材料 (primary) を1つ読む: 数、変数、文字列、
 * または *変数 (ポインタの先の1バイト)。
 * 変数は名簿にあるものだけが使える。代入されていない変数を
 * 使うのはエラー (「いつか入るはず」の値で計算しないため。
 * この小さい言語では、使う前に必ず代入文が先にある書き方)。
 * 成功したらノード番号、失敗したら -1 を返す */
static int parse_primary_node(void) {
    skip_spaces();
    if (is_digit(*cur))
        return parse_number_node();
    if (*cur == '"')
        return parse_string_node();
    if (*cur == '*') {
        /* ポインタ読み: * の次に変数が来る形だけを扱う */
        cur++;
        skip_spaces();
        int operand = parse_primary_node();
        if (operand < 0)
            return -1;
        if (node_kind[operand] != NODE_VAR) {
            errmsg = "* の相手は変数だけです (今の文法)";
            return -1;
        }
        return new_node(NODE_DEREF, 0, operand, 0);
    }
    if (is_ident_start(*cur)) {
        if (!parse_ident())
            return -1;
        int var = find_var(ident_buf, ident_len);
        if (var < 0) {
            errmsg = "代入されていない変数が使われました";
            return -1;
        }
        return new_node(NODE_VAR, var, 0, 0);
    }
    errmsg = "数か変数が必要です";
    return -1;
}

/* 式を読み、AST の根のノード番号を返す。失敗したら -1。
 *
 * 読み方 (左結合): まず項を読む。演算子が来たら、次の項を
 * 読んで「今の木全体を左の子、新しい項を右の子とする新しい
 * 根」を作る。"50+10+5" は ((50+10)+5) という木になる。
 * 左から順に計算する v1 の意味は、この木の形そのものである */
static int parse_expr(void) {
    int lhs = parse_primary_node();
    if (lhs < 0)
        return -1;
    for (;;) {
        skip_spaces();
        if (*cur == '\0' || *cur == ';')
            return lhs;  /* 式の終わり (文の区切りか、全体の終わり) */
        int kind;
        if (*cur == '+')
            kind = NODE_ADD;
        else if (*cur == '-')
            kind = NODE_SUB;
        else if (is_ident_start(*cur)) {
            errmsg = "式の途中に名前があります (文の区切り ; の不足かもしれません)";
            return -1;
        } else {
            errmsg = "演算子は + か - だけです (他の記号はまだ未対応)";
            return -1;
        }
        cur++;
        skip_spaces();
        int rhs = parse_primary_node();
        if (rhs < 0)
            return -1;
        lhs = new_node(kind, 0, lhs, rhs);
        if (lhs < 0)
            return -1;
    }
}

/* プログラム全体を読み、最後の式の AST の根を返す。
 * 代入文は stmt_var[] / stmt_expr[] に溜まる。失敗したら -1。
 *
 * 代入文か最後の式かは「先読み」で見分ける: 識別子で始まって、
 * その次 (空白の後) が '=' なら代入文。そうでなければ読み
 * 直して式として扱う (カーソルを巻き戻すのが先読みの実体) */
static int parse_program(char *program) {
    cur = program;
    nnode = 0;
    nvar = 0;
    nstmt = 0;
    nstr = 0;
    str_used = 0;
    skip_spaces();

    /* v0 互換: プログラムが丸ごと1文字 (数字以外) なら、
     * その文字コードを数のノードとして扱う。
     * (この互換のため、1文字の変数名だけのプログラムは
     * 「その変数の値」ではなく「その文字コード」になる) */
    if (cur[0] != '\0' && cur[1] == '\0' && !is_digit(cur[0]))
        return new_node(NODE_NUM, (unsigned char)cur[0], 0, 0);

    for (;;) {
        skip_spaces();
        char *save = cur;
        if (is_ident_start(*cur)) {
            /* 先読み: 名前の後に '=' があるか */
            if (!parse_ident())
                return -1;
            /* 名前は巻き戻しても使うことがあるので控える */
            char name_buf[32];
            int name_len = ident_len;
            for (int i = 0; i < name_len; i++)
                name_buf[i] = ident_buf[i];
            skip_spaces();
            if (*cur == '=' && cur[1] != '=') {
                /* 代入文だった。右辺を先に読んでから名簿に載せる
                 * (右辺の中で代入先自身を使うのは、初回代入では
                 * まだ値が無いのでエラーになるのが正しい) */
                cur++;
                int expr = parse_expr();
                if (expr < 0)
                    return -1;
                int var = define_var(name_buf, name_len);
                if (var < 0)
                    return -1;
                skip_spaces();
                if (*cur != ';') {
                    errmsg = "代入文の終わりには ; が必要です";
                    return -1;
                }
                cur++;
                if (nstmt >= 128) {
                    errmsg = "文が多すぎます";
                    return -1;
                }
                stmt_var[nstmt] = var;
                stmt_expr[nstmt] = expr;
                nstmt++;
                continue;
            }
            /* 代入文ではなかった。カーソルを巻き戻して式へ */
            cur = save;
        }
        /* 最後の式。これでプログラムは終わる */
        int expr = parse_expr();
        if (expr < 0)
            return -1;
        skip_spaces();
        if (*cur != '\0') {
            errmsg = "最後の式の後に、余分な文字があります";
            return -1;
        }
        return expr;
    }
}

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
 * 容量の約束: ノード上限 512 に対し命令は最大でも
 * ノード数 x2 + 文の数 + 3 個程度 (演算ノード1つが
 * addi + add/sub の2命令、代入文1つが余分に移し替え1命令)。
 * 2048 あれば解析が成功した後は生成があふれることはない。
 * ================================================================ */

static int OP_LUI = 0;
static int OP_ADDI = 1;
static int OP_ADD = 2;
static int OP_SUB = 3;
static int OP_SB = 4;
static int OP_WFI = 5;
static int OP_LBU = 6;  /* メモリから1バイト読む (符号なし) */
static int OP_AUIPC = 7; /* 自分の住所 + 即値 (PC 相対) */

static int asm_op[2048];
static int asm_rd[2048];
static int asm_rs1[2048];
static int asm_rs2[2048];
static int asm_imm[2048];
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

/* 変数番号から、その変数の住むレジスタ番号を出す */
static int var_reg(int var) {
    return REG_VAR_BASE + var;
}

/* 葉かどうか: 式の中で「そのまま値として使える材料」は
 * 数・変数・文字列 (住所)・ポインタ読みの4種 */
static int is_leaf(int node) {
    int k = node_kind[node];
    return k == NODE_NUM || k == NODE_VAR || k == NODE_STR ||
           k == NODE_DEREF;
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

static void gen_load_addr(int reg, unsigned int addr) {
    /* この auipc 命令自身が置かれる住所 (命令列の今の末尾) */
    unsigned int pc = LOAD_BASE + (unsigned int)nasm * 4u;
    long diff = (long)addr - (long)pc;
    long hi = (diff + 0x800) >> 12;
    long lo = diff - (hi << 12);
    asm_push(OP_AUIPC, reg, 0, 0, (int)hi);
    asm_push(OP_ADDI, reg, reg, 0, (int)lo);
}

/* 葉を、指定レジスタへ読み出す命令を積む。
 *   数:       即値で入れる
 *   変数:     そのレジスタからの写し (addi rd, 相手, 0 = mv)
 *   文字列:   その住所を作る (auipc + addi の2命令)
 *   ポインタ読み: 変数の持つ住所から1バイト読む (lbu) */
static void gen_load_leaf(int reg, int leaf) {
    int k = node_kind[leaf];
    if (k == NODE_NUM)
        asm_push(OP_ADDI, reg, REG_ZERO, 0, node_val[leaf]);
    else if (k == NODE_VAR)
        asm_push(OP_ADDI, reg, var_reg(node_val[leaf]), 0, 0);
    else if (k == NODE_STR)
        gen_load_addr(reg, str_addr[node_val[leaf]]);
    else /* NODE_DEREF: lhs がポインタの変数ノード */
        asm_push(OP_LBU, reg, var_reg(node_val[node_lhs[leaf]]), 0, 0);
}

/* 式の AST から命令列を作る。結果は t1 に残る約束。
 * 失敗したら 0 を返す (理由は errmsg)。
 *
 * 作り方: 左の子を先に生成する (結果は t1)。右の子は葉
 * (数か変数) なので、t2 に入れて演算する。
 * 「右の子も式になる」一般形 (かっこや優先順位) では
 * t1 を退避する仕組み (スタック) が必要になる。それは
 * 後回しリストの題材で、ここでは文法の範囲だけを相手にする */
static int gen_expr(int node) {
    if (is_leaf(node)) {
        /* 葉: t1 にその値 (変数なら今の値、文字列なら住所、
         * ポインタ読みなら指された先の1バイト) が入る */
        gen_load_leaf(REG_T1, node);
        return 1;
    }
    /* 演算: まず左を t1 に */
    if (!gen_expr(node_lhs[node]))
        return 0;
    int rhs = node_rhs[node];
    if (!is_leaf(rhs)) {
        errmsg = "内部エラー: 右の子が葉でない式はまだ扱えません";
        return 0;
    }
    /* 次の項を t2 に入れて、t1 と演算する */
    gen_load_leaf(REG_T2, rhs);
    if (node_kind[node] == NODE_ADD)
        asm_push(OP_ADD, REG_T1, REG_T1, REG_T2, 0);
    else
        asm_push(OP_SUB, REG_T1, REG_T1, REG_T2, 0);
    return 1;
}

/* 命令列を全部作る (1回分): 土台 (UART の住所) → 代入文を
 * 順に実行 → 最後の式 → UART へ書く → 止まる。
 * 代入文の実行は「右辺を t1 で計算し、変数のレジスタへ
 * 写す」ことで、変数の値が次の文へ持ち越される。
 * 失敗したら 0 */
static int gen_all(int root) {
    nasm = 0;
    /* lui t0, 0x10000 -- UART ベース 0x10000000 を t0 に作る */
    asm_push(OP_LUI, REG_T0, 0, 0, 0x10000);
    for (int i = 0; i < nstmt; i++) {
        if (!gen_expr(stmt_expr[i]))
            return 0;
        /* addi 変数レジスタ, t1, 0 -- 結果を変数へ写す (代入) */
        asm_push(OP_ADDI, var_reg(stmt_var[i]), REG_T1, 0, 0);
    }
    if (!gen_expr(root))
        return 0;
    /* sb t1, 0(t0) -- 結果の下1バイトを UART 送信レジスタへ */
    asm_push(OP_SB, 0, REG_T0, REG_T1, 0);
    /* wfi -- 止まる */
    asm_push(OP_WFI, 0, 0, 0, 0);
    return 1;
}

/* プログラム全体の生成。文字列の住所を確定させるため、
 * 生成を2回行う (2パス生成):
 *   1回目: 命令の個数を数えることが目的。この時点では
 *          文字列の住所は仮の値だが、住所を作る命令の
 *          個数 (lui + addi) は本物と同じになる
 *   住所の確定: 文字列はコードの後ろに並ぶので、
 *          住所 = LOAD_BASE + 命令数 x 4 + プール内の位置
 *   2回目: 確定した住所で作り直す。これが最終成果になる */
static int gen_program(int root) {
    code_insts = 0;
    if (!gen_all(root))
        return 0;
    code_insts = nasm;
    for (int i = 0; i < nstr; i++)
        str_addr[i] = LOAD_BASE + (unsigned int)nasm * 4u +
                      (unsigned int)str_off[i];
    if (!gen_all(root))
        return 0;
    return 1;
}

/* ================================================================
 * 第3層 encode: 命令列 → 機械語 → ファイル出力
 *
 * ここだけが「命令のビット列の形」を知っている層。
 * RISC-V の命令は4バイトで、フィールド (区画) の並びは
 * 命令の種類ごとに決まっている。各命令の式のコメントに、
 * どの区画に何が入るかを書いておく。
 * ================================================================ */

/* 命令列の i 番目の命令を、32ビットの機械語に変換する */
static unsigned int encode_inst(int i) {
    int op = asm_op[i];
    int rd = asm_rd[i];
    int rs1 = asm_rs1[i];
    int rs2 = asm_rs2[i];
    int imm = asm_imm[i];

    if (op == OP_LUI) {
        /* lui rd, imm -- 上の20ビットに imm を置く U 形式
         * [ imm << 12 | rd << 7 | opcode 0x37 ]
         * lui t0, 0x10000 なら 0x10000000 が t0 に入る */
        return ((unsigned int)imm << 12) | ((unsigned int)rd << 7) | 0x37u;
    }
    if (op == OP_ADDI) {
        /* addi rd, rs1, imm -- I 形式
         * [ imm << 20 | rs1 << 15 | funct3(0) << 12
         *   | rd << 7 | opcode 0x13 ] */
        return ((unsigned int)imm << 20) | ((unsigned int)rs1 << 15) |
               ((unsigned int)rd << 7) | 0x13u;
    }
    if (op == OP_ADD) {
        /* add rd, rs1, rs2 -- R 形式 (レジスタ同士)
         * [ funct7(0) << 25 | rs2 << 20 | rs1 << 15
         *   | funct3(0) << 12 | rd << 7 | opcode 0x33 ] */
        return ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_SUB) {
        /* sub rd, rs1, rs2 -- add との違いは funct7 が 0x20 */
        return (0x20u << 25) | ((unsigned int)rs2 << 20) |
               ((unsigned int)rs1 << 15) | ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_SB) {
        /* sb rs2, imm(rs1) -- S 形式 (メモリへ書く)
         * 即値は上下に分かれて入るのが S 形式の特徴:
         * [ imm[11:5] << 25 | rs2 << 20 | rs1 << 15
         *   | funct3(0) << 12 | imm[4:0] << 7 | opcode 0x23 ]
         * 今の使い方では imm は常に 0 なので、即値の区画は
         * 全部 0 になる */
        return ((unsigned int)(imm >> 5) << 25) |
               ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               ((unsigned int)(imm & 31) << 7) | 0x23u;
    }
    if (op == OP_AUIPC) {
        /* auipc rd, imm -- U 形式 (lui と同じ並び、opcode だけ
         * 違う: 0x17)。rd = 実行中の命令の住所 + (imm << 12) */
        return ((unsigned int)imm << 12) | ((unsigned int)rd << 7) | 0x17u;
    }
    if (op == OP_LBU) {
        /* lbu rd, imm(rs1) -- I 形式 (addi と同じ並び)。
         * addi との違いは opcode が 0x03 (読み込み系) で、
         * funct3 が 4 (符号なしバイト読み) なこと */
        return ((unsigned int)imm << 20) | ((unsigned int)rs1 << 15) |
               (4u << 12) | ((unsigned int)rd << 7) | 0x03u;
    }
    /* OP_WFI: 引数を取らない固定パターン */
    return 0x10500073u;
}

/* 32ビットの機械語を、リトルエンディアンで標準出力に書く
 * (小さい方のバイトから順に書くのがリトルエンディアン) */
static void write32(unsigned int inst) {
    out_byte(inst & 0xff);
    out_byte((inst >> 8) & 0xff);
    out_byte((inst >> 16) & 0xff);
    out_byte((inst >> 24) & 0xff);
}

/* 命令列を全部変換して書き出し、続けて文字列のデータ
 * (str_pool の中身そのもの) を書き出す。これが最終成果物。
 * バイナリの姿: [コード (命令 x 4バイト)] [文字列の並び]
 * 実行が wfi で止まるため、後ろの文字列が命令として
 * 実行されることはない。文字列の住所は第2層がこの並びに
 * 合わせて決めてある */
static void write_binary(void) {
    for (int i = 0; i < nasm; i++)
        write32(encode_inst(i));
    for (int i = 0; i < str_used; i++)
        out_byte((unsigned char)str_pool[i]);
}

/* ================================================================
 * main: 層を順に呼ぶだけ
 *   parse (失敗したら何も出さない) → gen (同) → write
 * ================================================================ */

int main(int argc, char **argv) {
    if (argc != 2) {
        print_str(2, "usage: iwac '<プログラム>'   例: iwac 's=\"Hi\"; *s'\n");
        return 1;
    }

    int root = parse_program(argv[1]);
    if (root < 0) {
        print_str(2, "iwac: ");
        print_str(2, errmsg);
        print_str(2, "\n");
        return 1;
    }

    if (!gen_program(root)) {
        print_str(2, "iwac: ");
        print_str(2, errmsg);
        print_str(2, "\n");
        return 1;
    }

    write_binary();
    return 0;
}
