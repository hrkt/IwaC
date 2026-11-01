/* iwac -- Cコンパイラのサブセット (v1、内部構造を分離した版)
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
 * === v1 の言語 (文法) ===
 *
 *   program = 1文字               (v0 との互換。その文字コードが値)
 *           | expr
 *   expr    = number { ("+" | "-" ) number }
 *   number  = 数字の並び (10進数、0..2047。理由は addi の項で)
 *
 * 計算はコンパイラが代行せず、出力バイナリの中の add / sub が
 * 実行時に行う。結果の下1バイトが UART に出る。
 * シェルで実行するときは './iwac '65+1'' のように引用符で囲む。
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

static int NODE_NUM = 0;  /* 数 (葉) */
static int NODE_ADD = 1;  /* 足し算 */
static int NODE_SUB = 2;  /* 引き算 */

static int node_kind[512];
static int node_val[512];
static int node_lhs[512];
static int node_rhs[512];
static int nnode;  /* 作ったノードの数 */

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

/* 式を読み、AST の根のノード番号を返す。失敗したら -1。
 *
 * 読み方 (左結合): まず項を読む。演算子が来たら、次の項を
 * 読んで「今の木全体を左の子、新しい項を右の子とする新しい
 * 根」を作る。"50+10+5" は ((50+10)+5) という木になる。
 * 左から順に計算する v1 の意味は、この木の形そのものである */
static int parse_expr(void) {
    int lhs = parse_number_node();
    if (lhs < 0)
        return -1;
    for (;;) {
        skip_spaces();
        if (*cur == '\0')
            return lhs;  /* 式の終わり */
        int kind;
        if (*cur == '+')
            kind = NODE_ADD;
        else if (*cur == '-')
            kind = NODE_SUB;
        else {
            errmsg = "演算子は + か - だけです (他の記号はまだ未対応)";
            return -1;
        }
        cur++;
        skip_spaces();
        int rhs = parse_number_node();
        if (rhs < 0)
            return -1;
        lhs = new_node(kind, 0, lhs, rhs);
        if (lhs < 0)
            return -1;
    }
}

/* プログラム全体を読み、AST の根を返す。失敗したら -1 */
static int parse_program(char *program) {
    cur = program;
    nnode = 0;
    skip_spaces();

    /* v0 互換: プログラムが丸ごと1文字 (数字以外) なら、
     * その文字コードを数のノードとして扱う */
    if (cur[0] != '\0' && cur[1] == '\0' && !is_digit(cur[0]))
        return new_node(NODE_NUM, (unsigned char)cur[0], 0, 0);

    return parse_expr();
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
 * ノード数 x2 + 3 個 (演算ノード1つが addi + add/sub の
 * 2命令、数ノードは子の側で数えられる)。2048 あれば
 * 解析が成功した後は生成があふれることはない。
 * ================================================================ */

static int OP_LUI = 0;
static int OP_ADDI = 1;
static int OP_ADD = 2;
static int OP_SUB = 3;
static int OP_SB = 4;
static int OP_WFI = 5;

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

/* 式の AST から命令列を作る。結果は t1 に残る約束。
 * 失敗したら 0 を返す (理由は errmsg)。
 *
 * 作り方: 左の子を先に生成する (結果は t1)。右の子は、
 * 今の文法では必ず数なので、t2 に入れて演算する。
 * 「右の子も式になる」一般形 (かっこや優先順位) では
 * t1 を退避する仕組み (スタック) が必要になる。それは
 * v2 で扱う題材で、ここでは文法の範囲だけを相手にする */
static int gen_expr(int node) {
    if (node_kind[node] == NODE_NUM) {
        /* 数: t1 にその値を入れる */
        asm_push(OP_ADDI, REG_T1, REG_ZERO, 0, node_val[node]);
        return 1;
    }
    /* 演算: まず左を t1 に */
    if (!gen_expr(node_lhs[node]))
        return 0;
    int rhs = node_rhs[node];
    if (node_kind[rhs] != NODE_NUM) {
        errmsg = "内部エラー: 右の子が数でない式はまだ扱えません";
        return 0;
    }
    /* 次の項を t2 に入れて、t1 と演算する */
    asm_push(OP_ADDI, REG_T2, REG_ZERO, 0, node_val[rhs]);
    if (node_kind[node] == NODE_ADD)
        asm_push(OP_ADD, REG_T1, REG_T1, REG_T2, 0);
    else
        asm_push(OP_SUB, REG_T1, REG_T1, REG_T2, 0);
    return 1;
}

/* プログラム全体の命令列を作る: 土台 (UART の住所) →
 * 式の計算 → UART へ書く → 止まる。失敗したら 0 */
static int gen_program(int root) {
    nasm = 0;
    /* lui t0, 0x10000 -- UART ベース 0x10000000 を t0 に作る */
    asm_push(OP_LUI, REG_T0, 0, 0, 0x10000);
    if (!gen_expr(root))
        return 0;
    /* sb t1, 0(t0) -- 結果の下1バイトを UART 送信レジスタへ */
    asm_push(OP_SB, 0, REG_T0, REG_T1, 0);
    /* wfi -- 止まる */
    asm_push(OP_WFI, 0, 0, 0, 0);
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

/* 命令列を全部変換して書き出す。これが最終成果物の生成 */
static void write_binary(void) {
    for (int i = 0; i < nasm; i++)
        write32(encode_inst(i));
}

/* ================================================================
 * main: 層を順に呼ぶだけ
 *   parse (失敗したら何も出さない) → gen (同) → write
 * ================================================================ */

int main(int argc, char **argv) {
    if (argc != 2) {
        print_str(2, "usage: iwac '<式>'   例: iwac '65+1'\n");
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
