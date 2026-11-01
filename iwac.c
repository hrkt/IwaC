/* iwac -- Cコンパイラのサブセット (v13)
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
 * === v13 の言語 (文法) ===
 *
 *   program = 1文字                 (v0 との互換。その文字コードが値)
 *           | { funcdef | stmt } [ expr ]
 *   funcdef = type ident "(" [ param ] ")" "{" { stmt } "}"
 *   type    = [ "const" ] ("void" | "char" | "int") { "*" }
 *             (型は今は読み捨てる。値は全部「64ビットの数」
 *              として扱い、型による区別はまだしない)
 *   param   = type ident          (引数は今は1つまで)
 *   stmt    = ident "=" expr ";"               (代入文)
 *           | type ident "[" number "]" ";"    (配列の宣言)
 *           | ident "[" expr "]" "=" expr ";"  (配列の要素へ書く)
 *           | "*" (ident | number) "=" expr ";" (メモリへ書く文)
 *           | "putc" "(" expr ")" ";"          (出力文。組み込み)
 *           | "while" "(" expr ")" "{" { stmt } "}"  (繰り返し)
 *           | "if" "(" expr ")" "{" { stmt } "}"
 *             [ "else" ( "{" { stmt } "}" | if文 ) ]  (条件分岐)
 *           | ident "(" [ expr ] ")" ";"       (関数の呼び出し)
 *   expr    = eq { "&" eq }         (優先順位は C と同じ順。
 *   eq      = rel { ("==" | "!=") rel }   固い順に * / 、+ -、
 *   rel     = add { ("<" | ">" | "<=" | ">=") add }   < >、
 *   add     = term { ("+" | "-") term }   == !=、& の段を
 *   term    = primary { ("*" | "/") primary }  重ねてある)
 *   primary = number | ident        (数、または変数)
 *           | ident "[" expr "]"    (配列の要素を読む)
 *           | string                (文字列。値はその住所)
 *           | "*" primary           (ポインタの先を読む。
 *                                  相手が数なら絶対アドレス読み)
 *           | "(" expr ")"          (かっこ)
 *   number  = 10進数 (0..2047。理由は addi の項で)
 *           | "0x" 16進数 (アドレス用。32ビットまで)
 *   ident   = 英字か _ で始まる名前 (while / putc と型の単語は予約語)
 *   string  = '"' 文字の並び '"'    (エスケープは \n \t \\ \" )
 *
 *   例: "x = 65; x + 1"        → 66 になり 'B' が出る (v2)
 *       "s = \"Hi\"; *s"       → 'H' が出る (v3)
 *       "void uart_puts(char *s) { while (*s != 0) { putc(*s); s = s + 1; } }"
 *       "msg = \"hello\n\"; uart_puts(msg);"
 *                              → hello と出る (v5。目標の
 *                                KosabaOS の uart_puts その形)
 *       "void uart_putc(char c) {"
 *       "  while ((*0x10000005 & 32) == 0) { }"
 *       "  *0x10000000 = c; }"
 *                              → 本物の uart_putc 本体 (v6 で
 *                                部品が揃い、v7 で C と同じ
 *                                かっこ付きの形になった)
 *
 * v5 で育ったのは「関数」。呼び出しの約束 (呼び出し規約) は:
 *   - 引数は a0 レジスタで渡す。関数は入ってすぐ、それを
 *     引数の変数のレジスタへ写す
 *   - 呼び出しは jal ra (戻り先を ra に残して飛ぶ)、
 *     戻りは jalr (ra の指す場所へ飛ぶ)
 *   - 関数は入るときに戻り先 ra をスタックへ退避し、
 *     出るときに戻す。これで「関数が関数を呼ぶ」入れ子の
 *     呼び出しでも、戻り先が上書きされずに済む
 *   - スタックは、バイナリの後ろの空き地にコンパイラが
 *     自分で用意する (起動直後は sp が保証されないため)
 * 関数も変数と同じく「使う前に定義」が今の約束。スコープは
 * まだ無く、引数も含め変数はプログラム全体で名前が一意。
 * 戻り値のある関数 (return) はまだ無い (void のみ)。
 * (計画は doc/roadmap.md の「print loop」目標の節)
 *
 * 制御の流れは、分岐 (beq: 条件が 0 なら飛ぶ) とジャンプ
 * (jal: 無条件に戻る) で作る。命令列 (第2層) の段階では
 * 飛び先を「命令の番号」で持ち、ビット列への変換 (第3層) で
 * 「今の命令からの距離 (相対オフセット)」に直す。
 * while の条件は「0 でない間くり返す」という意味。
 * 比較 (== / !=) は v5 で入った。
 *
 * v6 で育ったのは「デバイスに触る」部品一式で、これで
 * 本物の uart_putc の本体が書けるようになった:
 *   - *0x10000005 のように、数 (絶対アドレス) を相手にした
 *     ポインタ読み。デバイスのレジスタは「決まった住所の
 *     メモリ」として読む (MMIO)。住所の材料は auipc 相対
 *     なので、0x1000xxxx のような住所でも壊れない
 *   - ビット演算 & (状態レジスタから目的のビットだけを
 *     取り出すため。LSR の bit5 = 0x20 が送信窓口の合図)
 *   - *住所 = 値; のメモリ書き込み文 (THR へ書く = 送信)
 *   - 16進数リテラル 0x... (アドレスを書くための数)
 * 組み込みの putc は互換のため残してあるが、もう必須では
 * ない。iwac のプログラムは毎回メモリを読む (コンパイラが
 * 読みを消すことはない) ので、C の volatile に相当する
 * 指定は今のところ要らない。
 *
 * v7 では演算子の優先順位を C に合わせ直した。v6 では
 * かっこが無い代わりに & を比較より固くしていたが、
 * かっこ (式) が入ったので、C の順 (固い順に: * 読みと
 * かっこ、掛け算・割り算、足し引き、比較、&) に戻し、
 * 掛け算 * と割り算 / も入れた (M 拡張。ターゲットの
 * RV64G 前提を使う)。ポーリングの形は C と同じく
 * ((*0x10000005 & 32) == 0) とかっこで書く。
 * あわせて式の評価も一般化した: 右の子が葉でない式
 * ( (50+10)*(1+1) のような形) は、左の結果をスタックに
 * 退避してから右を計算する。スタックは、関数がある
 * プログラムと同じ仕組みでコンパイラが用意する
 * (要るプログラムのときだけ出すので、昔のプログラムの
 * 出力は変わらない)。
 *
 * v8 では条件分岐 if / else を入れた。本体は while と
 * 同じく { } で囲む形だけ (ぶら下がり文は扱わない)。
 * else の中身は { } の塊か、else if の連なりのための
 * if 文1つ。生成は while と同じ道具立て: 条件が 0 なら
 * beq で else (または末尾) へ飛び、else がある場合は
 * 本体の末尾に jal を置いて else を飛び越える。
 *
 * v9 では配列を入れた。宣言は char buf[16]; の形で、
 * 要素は1バイト (型の単語は読み捨てるので、どの型で
 * 書いても1バイト要素の配列になる)。置き場所は変数の
 * レジスタではなくメモリで、バイナリの「コード＋文字列
 * の後ろ」の静的な領域にまとめて確保し、住所は文字列と
 * 同じ2パス生成で確定させる。配列の名前そのものの値は
 * 先頭アドレスなので、関数へ渡したりポインタ変数に
 * 入れたりできる。要素の読み書きは「先頭＋添字」の
 * 住所計算で、読みは lbu、書きは sb。配列はプログラム
 * 全体で共有の1つだけ (スコープも関数の局所配列も無い)。
 *
 * v10 では入力がファイルから読めるようになった
 * (iwac hello.c > hello.bin)。引数が .c で終わる名前
 * ならファイルとして開き、それ以外でも同名ファイルが
 * 開ければその中身を、無ければ引数そのものをプログラム
 * として読む (従来の使い方も残る)。ファイルの中身の
 * ために、空白として改行も飛ばせるようにし、行コメント
 * // (行末まで) も入れた。ホストに頼る外部関数は
 * write に open/read/close が加わって4つになった。
 * ここが将来 KosabaOS 上で動かすときの差し替え場所。
 *
 * v11 では関係演算子 < > <= >= を入れた。優先順位の段を
 * C の順に1つ差し込み (add と eq の間)、結果は == と
 * 同じく「成り立てば 1、だめなら 0」。機械語は slt
 * (符号付き比較) が中心で、> は両辺を入れ替えた <、
 * <= と >= は反対の比較を xori 1 で反転して作る。これで
 * ループの条件が while (i < 10) と自然に書ける。
 *
 * v12 では main の形を入れた。main という名前の関数が
 * あると、それがプログラムの入口になる: 生成は「入口で
 * main を jal で呼び、戻ってきたら wfi で止まる」形で、
 * main 自体は通常の関数とまったく同じ作り (戻り先の
 * 退避も含む)。main がある場合は、外側に文や最後の式を
 * 書く形との混ぜ書きを禁止する (C の作法に合わせる)。
 * main が無ければ従来の形のままで、出力も変わらない。
 * 戻り値 (int main の int) は今までどおり読み捨てで、
 * 終了コードのような仕組みはまだ無い。
 *
 * v13 では単純な #define を入れた (前処理)。行頭の
 * "#define 名前 置き換え" を読み、以降の行の名前を
 * 単語単位で置き換えてから parse する。文字列と
 * コメントの中は置き換えず、置き換えた結果は再走査
 * しない (入れ子は展開しない)。再定義はエラー。
 * #include や引数付きマクロは対象外のまま。なお
 * iwac 自身のソースは今もプリプロセッサ無しで書く
 * (これはコンパイルされる側のプログラムの機能)。
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
 * write: fd へ buf の先頭 len バイトを書くホストの関数
 * open / read / close: ファイル入力 (v10) のための3つ。
 * flags は読み専用の 0 を渡す。iwac がホストに頼るのは
 * この4つだけで、ここが KosabaOS 上で動かすときに
 * システムコールへ差し替える場所になる */
long write(int fd, char *buf, long len);
int open(char *path, int flags);
long read(int fd, char *buf, long len);
int close(int fd);

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
static int REG_RA = 1;  /* 戻り先の住所 (関数の呼び出し用) */
static int REG_SP = 2;  /* スタックポインタ (コンパイラが用意) */
static int REG_A0 = 10; /* 引数を渡すレジスタ (呼び出し規約) */
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
                            * node_lhs = 住所を表すノード
                            * (変数ノード、または数のノード =
                            * 絶対アドレス読み) */
static int NODE_ASSIGN = 6; /* 代入文。node_val = 変数番号、
                             * node_lhs = 右辺の式 */
static int NODE_PUTC = 7;   /* 出力文。node_lhs = 出す式 */
static int NODE_WHILE = 8;  /* 繰り返し文。node_lhs = 条件の式、
                             * node_rhs = 本体の文の並びの先頭 */
static int NODE_CALL = 9;   /* 呼び出し文。node_val = 関数番号、
                             * node_lhs = 引数の式 (無ければ -1) */
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

static int node_kind[512];
static int node_val[512];
static int node_lhs[512];
static int node_rhs[512];
static int node_next[512]; /* 文の並び用: 次の文ノード (-1 で終わり)。
                            * 式のノードでは使わない */
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

/* 配列の名簿は下の方にある (名前の重なりを確かめるため
 * 先に宣言だけ置く) */
static int find_array(char *name, int len);

/* 名前を名簿に登録し、番号を返す。既にある名前なら同じ番号を
 * 返す (再代入は同じ変数への代入になる)。配列と同じ名前は
 * 使えない。失敗したら -1 */
static int define_var(char *name, int len) {
    if (find_array(name, len) >= 0) {
        errmsg = "その名前は配列に使われています";
        return -1;
    }
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
 *   func_has_param[i] : 引数があるか
 *   func_param_var[i] : 引数の変数番号 (引数があれば)
 *   func_body[i]      : 本体の文の並びの先頭ノード
 *   func_entry[i]     : 命令列の中での入口の命令番号
 *                       (第2層が生成のたびに決める)
 * 関数は最大16個まで */
static char func_names[16][32];
static int func_name_len[16];
static int func_has_param[16];
static int func_param_var[16];
static int func_body[16];
static int func_entry[16];
static int nfunc;

/* --- 配列の名簿 ---
 * 配列は変数 (レジスタ) と違い、メモリに置き場所が要る。
 * 置き場所はバイナリの「コード＋文字列の後ろ」にまとめて
 * 確保し (静的な領域)、住所は文字列と同じく2パス生成で
 * 確定させる。配列 i 番について:
 *   arr_size[i] : 要素数 (= バイト数。要は1バイト配列)
 *   arr_off[i]  : 配列領域の中での位置
 *   arr_addr[i] : 確定した先頭アドレス (第2層が決める)
 * 配列は最大8個、合計 4096 バイトまで */
static char arr_names[8][32];
static int arr_name_len[8];
static int arr_size[8];
static int arr_off[8];
static unsigned int arr_addr[8];
static int narr;      /* 登録された配列の数 */
static int arr_used;  /* 配列領域の使用済みバイト数 */

/* 名前 (場所と長さで渡す) の配列があればその番号、無ければ -1 */
static int find_array(char *name, int len) {
    for (int i = 0; i < narr; i++) {
        if (arr_name_len[i] != len)
            continue;
        int j = 0;
        while (j < len && arr_names[i][j] == name[j])
            j++;
        if (j == len)
            return i;
    }
    return -1;
}

/* 配列を名簿に登録し、番号を返す。名前は変数とも配列とも
 * 重なってはいけない。失敗したら -1 */
static int define_array(char *name, int len, int size) {
    if (find_array(name, len) >= 0 || find_var(name, len) >= 0) {
        errmsg = "その名前は既に使われています";
        return -1;
    }
    if (narr >= 8) {
        errmsg = "配列は8個までです";
        return -1;
    }
    if (arr_used + size > 4096) {
        errmsg = "配列の合計が大きすぎます (4096バイトまで)";
        return -1;
    }
    int i = narr++;
    for (int j = 0; j < len; j++)
        arr_names[i][j] = name[j];
    arr_name_len[i] = len;
    arr_size[i] = size;
    arr_off[i] = arr_used;
    arr_used += size;
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
        return new_node(NODE_NUM, (int)(unsigned int)value, 0, 0);
    }
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

/* 式全体を読む関数は、この下で定義する (かっこの中身が
 * 式全体なので、互いに呼び合う)。先に宣言だけ置く。
 * expect_char も下で定義する道具の先行宣言 */
static int parse_expr(void);
static int parse_rel_expr(void);  /* eq の段から呼ばれる (v11) */
static int expect_char(char c, char *msg);

/* 式の材料 (primary) を1つ読む: 数、変数、文字列、
 * かっこで囲んだ式、または *材料 (ポインタの先の1バイト)。
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
    if (*cur == '*') {
        /* ポインタ読み: * の次は「住所を表すもの」。
         * 変数なら、その変数が持つ住所 (ポインタ)。
         * 数なら、その数そのものが住所になる (絶対アドレス
         * 読み)。デバイスのレジスタは決まった住所にあるので、
         * *0x10000005 と書けば LSR が読める (MMIO)。
         * かっこで囲んだ式 (住所の計算結果) も相手にできる。
         * * を重ねる (**) 形はまだ扱わない */
        cur++;
        skip_spaces();
        int operand = parse_primary_node();
        if (operand < 0)
            return -1;
        if (node_kind[operand] == NODE_DEREF) {
            errmsg = "* を重ねる形 (**) はまだ扱えません";
            return -1;
        }
        return new_node(NODE_DEREF, 0, operand, 0);
    }
    if (is_ident_start(*cur)) {
        if (!parse_ident())
            return -1;
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
        return new_node(NODE_VAR, var, 0, 0);
    }
    errmsg = "数か変数が必要です";
    return -1;
}

/* --- 式の優先順位 (v7 で C に合わせた) ---
 *
 * 演算子の結びの固さは C と同じ順にした。固い順に:
 *   * (ポインタ読み) と かっこ   … parse_primary_node
 *   * / (掛け算・割り算)        … parse_term_expr
 *   + -                         … parse_add_expr
 *   < > <= >=                   … parse_rel_expr (v11)
 *   == !=                       … parse_eq_expr
 *   &                           … parse_expr (一番外側)
 * 「固い」ほど先に (深く) 計算される。読み方は各段とも
 * 左結合: 演算子が来たら「今の木全体を左の子、新しい材料を
 * 右の子とする新しい根」を作る。"50+10+5" は ((50+10)+5)。
 * 段を分けることで、"60+2*3" は +(60, *(2,3)) = 66 に、
 * "a & b == c" は &(a, ==(b, c)) になる (C と同じ)。
 * ポーリングの形は C と同じくかっこで書く:
 *   (*0x10000005 & 32) == 0
 * (v6 では、かっこが無い代わりに & を固くしていた。
 *  かっこが入ったので C の順に戻した) */

/* 掛け算・割り算の段 (term) を読む。相手は材料 (primary)。
 * 演算子の * は、材料の頭に来る * (ポインタ読み) とは
 * 場所で見分ける: 材料を読んだ直後の * は掛け算 */
static int parse_term_expr(void) {
    int lhs = parse_primary_node();
    if (lhs < 0)
        return -1;
    for (;;) {
        skip_spaces();
        int kind;
        if (*cur == '*')
            kind = NODE_MUL;
        else if (*cur == '/')
            kind = NODE_DIV;
        else
            return lhs;
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

/* 関係演算子の段 (v11): < > <= >= を読む。
 * C の優先順位では、足し算よりゆるく、== != より固い。
 * 例: "i + 1 < 10" は <(+(i, 1), 10)、"a < b == c" は
 * ==(<(a, b), c) という木になる (C と同じ読み)。
 * <= と >= は2文字で読む (< の次が = でも、代入や
 * == と取り違えないよう、ここでまとめて処理する) */
static int parse_rel_expr(void) {
    int lhs = parse_add_expr();
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
        int rhs = parse_add_expr();
        if (rhs < 0)
            return -1;
        lhs = new_node(kind, 0, lhs, rhs);
        if (lhs < 0)
            return -1;
    }
}

/* 式全体を読む。一番外側はビット演算 & の段 (C の
 * 優先順位で、iwac にある演算子の中では一番ゆるい)。
 * 例: "*s != 0" は !=(*s, 0)、"*p & 32 == 0" は
 * &(*p, ==(32, 0)) という木になる (C と同じ読み) */
static int parse_expr(void) {
    int lhs = parse_eq_expr();
    if (lhs < 0)
        return -1;
    for (;;) {
        skip_spaces();
        if (*cur != '&')
            return lhs;
        if (cur[1] == '&') {
            errmsg = "&& はまだありません (& は1つで書きます)";
            return -1;
        }
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
static int parse_stmt_list(int in_brace);

/* 型を1つ読む: [ const ] (void|char|int) { * }
 * 型は今は読み捨てるので、形だけ確かめて進む。
 * 単語が型でなければ 0 を返す */
static int parse_type(void) {
    if (!is_ident_start(*cur))
        return 0;
    if (!parse_ident())
        return 0;
    if (ident_is("const")) {
        skip_spaces();
        if (!parse_ident())
            return 0;
    }
    if (!ident_is("void") && !ident_is("char") && !ident_is("int"))
        return 0;
    skip_spaces();
    while (*cur == '*') {  /* ポインタの印は読み飛ばす */
        cur++;
        skip_spaces();
    }
    return 1;
}

/* 関数定義を読む: 型 名前 ( [引数] ) { 文の並び }
 * 成功したら 1、失敗したら 0。関数は本体を読む前に名簿へ
 * 載せるので、自分自身を呼ぶ (再帰の) 形も読める。
 * 引数は今は1つまで。引数の名前は変数として名簿に載る */
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
    if (find_func(name_buf, name_len) >= 0) {
        errmsg = "同じ名前の関数が二つあります";
        return 0;
    }
    if (nfunc >= 16) {
        errmsg = "関数は16個までです";
        return 0;
    }
    int f = nfunc++;
    for (int i = 0; i < name_len; i++)
        func_names[f][i] = name_buf[i];
    func_name_len[f] = name_len;
    func_has_param[f] = 0;
    func_param_var[f] = -1;
    func_body[f] = -1;

    if (!expect_char('(', "関数名の後には ( が必要です"))
        return 0;
    skip_spaces();
    if (*cur == ')') {
        cur++;  /* 引数なし */
    } else {
        /* 引数: 型 名前 (名前が変数として登録される) */
        if (!parse_type()) {
            errmsg = "引数は 型 名前 の形で書きます";
            return 0;
        }
        skip_spaces();
        if (!parse_ident()) {
            errmsg = "引数の名前が必要です";
            return 0;
        }
        char pname[32];
        int plen = ident_len;
        for (int i = 0; i < plen; i++)
            pname[i] = ident_buf[i];
        int var = define_var(pname, plen);
        if (var < 0)
            return 0;
        func_has_param[f] = 1;
        func_param_var[f] = var;
        skip_spaces();
        if (*cur == ',') {
            errmsg = "引数は今は1つまでです";
            return 0;
        }
        if (!expect_char(')', "引数の後には ) が必要です"))
            return 0;
    }
    /* main という名前の関数は特別扱い (v12): プログラムの
     * 入口になる。引数は持てない。入口そのものの生成は
     * gen_all が行う (main を呼び出す jal を置く) */
    if (name_len == 4 && name_buf[0] == 'm' && name_buf[1] == 'a' &&
        name_buf[2] == 'i' && name_buf[3] == 'n') {
        if (func_has_param[f]) {
            errmsg = "main に引数は書けません";
            return 0;
        }
        prog_main = f;
    }
    if (!expect_char('{', "関数の本体は { で始めます"))
        return 0;
    int body = parse_stmt_list(1);
    if (body == -2)
        return 0;
    func_body[f] = body;
    return 1;
}

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
        /* 文の始まりかどうかを先読みする: 識別子で始まる場合、
         * 名前とその次第で「代入文・while・putc」のどれか、
         * それ以外 (式の始まり) もありうる */
        char *save = cur;
        if (is_ident_start(*cur)) {
            if (!parse_ident())
                return -2;
            int is_keyword = ident_is("while") || ident_is("putc") ||
                             ident_is("if");
            int is_type = ident_is("void") || ident_is("char") ||
                          ident_is("int") || ident_is("const");
            if (is_type) {
                /* 型で始まるのは、関数定義か配列の宣言。
                 * 名前の後ろが '[' なら配列の宣言で、こちらは
                 * 文としてどこにでも置ける。'(' なら関数定義で、
                 * 置けるのはプログラムの頭だけ */
                cur = save;
                int is_array_decl = 0;
                if (parse_type()) {
                    skip_spaces();
                    if (parse_ident()) {
                        skip_spaces();
                        is_array_decl = (*cur == '[');
                    }
                }
                cur = save;
                if (is_array_decl) {
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
                if (in_brace) {
                    errmsg = "関数定義はプログラムの頭にだけ書けます";
                    return -2;
                }
                if (!parse_funcdef())
                    return -2;
                continue;
            }
            skip_spaces();
            int is_assign = (*cur == '=' && cur[1] != '=');
            int is_call = (*cur == '(');
            int is_index_store = 0;
            if (*cur == '[') {
                /* 名前 [ ... ] の後ろに '=' があれば、配列の
                 * 要素への書き込み文 (式の要素読みと見分ける)。
                 * ']' までを数えて先読みするだけ */
                int depth = 0;
                char *p = cur;
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
                while (*p == ' ' || *p == '\t')
                    p++;
                is_index_store = (depth == 0 && *p == '=' &&
                                  p[1] != '=');
            }
            if (is_keyword || is_assign || is_call || is_index_store) {
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

/* 配列の宣言を読む: 型 名前 [ 大きさ ] ;
 * 要素は1バイト (char の配列と同じ扱い。型の単語自体は
 * 今までどおり読み捨てる)。置き場所は名簿に予約するだけで、
 * 実行時のコードは出ない。成功したらノード番号、失敗は -1 */
static int parse_array_decl(void) {
    if (!parse_type()) {
        errmsg = "配列の宣言は 型 名前 [ 大きさ ] ; の形です";
        return -1;
    }
    skip_spaces();
    if (!parse_ident()) {
        errmsg = "配列の名前が必要です";
        return -1;
    }
    char name_buf[32];
    int name_len = ident_len;
    for (int i = 0; i < name_len; i++)
        name_buf[i] = ident_buf[i];
    if (!expect_char('[', "配列は 名前 [ 大きさ ] の形で書きます"))
        return -1;
    skip_spaces();
    int size = parse_number_value();
    if (size < 0)
        return -1;
    if (size < 1 || size > 1024) {
        errmsg = "配列の大きさは 1 から 1024 までです";
        return -1;
    }
    if (!expect_char(']', "配列の大きさの後には ] が必要です"))
        return -1;
    if (!expect_char(';', "配列の宣言の終わりには ; が必要です"))
        return -1;
    int a = define_array(name_buf, name_len, size);
    if (a < 0)
        return -1;
    return new_node(NODE_DECL, a, 0, 0);
}

/* 文を1つ読み、その AST ノードを返す。失敗したら -1。
 * 呼び出し時点でカーソルは文の頭にあること */
static int parse_stmt(void) {
    skip_spaces();
    if (is_ident_start(*cur)) {
        /* 型で始まる文は配列の宣言 (関数定義は頭専用で、
         * parse_stmt_list がそちらへ振り分ける) */
        char *tsave = cur;
        if (parse_ident() &&
            (ident_is("void") || ident_is("char") ||
             ident_is("int") || ident_is("const"))) {
            cur = tsave;
            return parse_array_decl();
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
    if (!parse_ident())
        return -1;

    if (ident_is("while")) {
        /* while ( 条件 ) { 文の並び } */
        if (!expect_char('(', "while の後には ( が必要です"))
            return -1;
        int cond = parse_expr();
        if (cond < 0)
            return -1;
        if (!expect_char(')', "while の条件の後には ) が必要です"))
            return -1;
        if (!expect_char('{', "while の後には { が必要です"))
            return -1;
        int body = parse_stmt_list(1);
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
        /* if ( 条件 ) { 文の並び } [ else { 文の並び }
         *                          | else if ... ]
         * 本体は while と同じく { } で囲む形だけにする */
        if (!expect_char('(', "if の後には ( が必要です"))
            return -1;
        int cond = parse_expr();
        if (cond < 0)
            return -1;
        if (!expect_char(')', "if の条件の後には ) が必要です"))
            return -1;
        if (!expect_char('{', "if の後には { が必要です"))
            return -1;
        int then_body = parse_stmt_list(1);
        if (then_body == -2)
            return -1;
        /* else があるかを先読みする (無ければ巻き戻す) */
        int else_body = -1;
        char *save = cur;
        skip_spaces();
        if (is_ident_start(*cur) && parse_ident() &&
            ident_is("else")) {
            skip_spaces();
            if (*cur == '{') {
                cur++;
                else_body = parse_stmt_list(1);
                if (else_body == -2)
                    return -1;
            } else if (is_ident_start(*cur)) {
                /* else if の形: else の中身は if 文を1つ */
                int sub = parse_stmt();
                if (sub < 0)
                    return -1;
                else_body = sub;
            } else {
                errmsg = "else の後には { か if が必要です";
                return -1;
            }
        } else {
            cur = save;  /* else ではなかった */
        }
        return new_node(NODE_IF, else_body, cond, then_body);
    }

    char name_buf[32];
    int name_len = ident_len;
    for (int i = 0; i < name_len; i++)
        name_buf[i] = ident_buf[i];

    /* 名前の次が '[' なら、配列の要素への書き込み文:
     * 名前 [ 添字の式 ] = 式 ; */
    skip_spaces();
    if (*cur == '[') {
        int a = find_array(name_buf, name_len);
        if (a < 0) {
            errmsg = "添字 [ ] を付けられるのは配列だけです";
            return -1;
        }
        cur++;
        int idx = parse_expr();
        if (idx < 0)
            return -1;
        if (!expect_char(']', "添字の後には ] が必要です"))
            return -1;
        if (!expect_char('=', "配列への書き込みは 名前[添字] = 式 ; の形です"))
            return -1;
        int val = parse_expr();
        if (val < 0)
            return -1;
        if (!expect_char(';', "書き込み文の終わりには ; が必要です"))
            return -1;
        return new_node(NODE_INDEX_STORE, a, idx, val);
    }

    /* 名前の次が '(' なら、関数の呼び出し文:
     * 名前 ( [式] ) ; 。関数は定義済みのものだけ呼べる
     * (「使う前に定義」の約束は関数も同じ) */
    if (*cur == '(') {
        int f = find_func(name_buf, name_len);
        if (f < 0) {
            errmsg = "定義されていない関数が呼ばれました";
            return -1;
        }
        cur++;
        skip_spaces();
        int arg = -1;
        if (*cur != ')') {
            arg = parse_expr();
            if (arg < 0)
                return -1;
        }
        if (!expect_char(')', "呼び出しの後には ) が必要です"))
            return -1;
        if (!expect_char(';', "呼び出し文の終わりには ; が必要です"))
            return -1;
        if ((arg >= 0) != func_has_param[f]) {
            errmsg = "引数の数が関数の定義と合いません";
            return -1;
        }
        return new_node(NODE_CALL, f, arg, 0);
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
    nstr = 0;
    str_used = 0;
    nfunc = 0;
    narr = 0;
    arr_used = 0;
    prog_stmts = -1;
    prog_final = -1;
    prog_has_final = 0;
    prog_main = -1;
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
static int OP_BEQ = 8;   /* 2つのレジスタが等しければ飛ぶ。
                          * asm_imm には「飛び先の命令番号」を
                          * 入れる (バイト数ではない)。
                          * バイト単位の距離への変換は第3層 */
static int OP_JAL = 9;   /* 無条件に飛ぶ。asm_imm は同上、
                          * asm_rd は 0 (戻り先は保存しない)。
                          * 関数呼び出しでは asm_rd を ra (1) にし、
                          * asm_imm には関数番号を負数で入れる
                          * (-(関数番号+1))。生成の最後に、
                          * 関数の入口番号へ書き直す */
static int OP_SD = 10;   /* メモリへ8バイト書く (ra の退避用) */
static int OP_LD = 11;   /* メモリから8バイト読む (ra の復帰用) */
static int OP_JALR = 12; /* レジスタの指す住所へ飛ぶ (関数からの
                          * 戻り。rd=0, rs1=ra, imm=0) */
static int OP_SLTU = 13; /* 符号なし比較: rs1 < rs2 なら 1
                          * (0 と比べれば「0 でないか」になる) */
static int OP_SLTIU = 14;/* 即値との符号なし比較
                          * (1 と比べれば「0 かどうか」になる) */
static int OP_AND = 15;  /* ビットごとの AND (両方 1 の桁だけ 1) */
static int OP_MUL = 16;  /* 掛け算 (M 拡張。RV64G 前提) */
static int OP_DIV = 17;  /* 割り算 (符号付き、M 拡張) */
static int OP_SLT = 18;  /* 符号付き比較: rs1 < rs2 なら 1
                          * (関係演算子用。SLTU と funct3 だけ違う) */
static int OP_XORI = 19; /* 即値との XOR。1 との XOR は
                          * 「1か0の反転」に使える */

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
 * 数・変数・文字列 (住所)・ポインタ読みの4種。ただし
 * ポインタ読みは、相手 (住所) が材料そのもののときだけ
 * 葉扱いする。相手が計算式の場合は、住所の計算に t1 を
 * 使うので、葉としては扱えない (gen_expr で別に処理する) */
static int is_leaf(int node) {
    int k = node_kind[node];
    if (k == NODE_NUM || k == NODE_VAR || k == NODE_STR ||
        k == NODE_ARRAY)
        return 1;
    if (k == NODE_DEREF) {
        int ok = node_kind[node_lhs[node]];
        return ok == NODE_NUM || ok == NODE_VAR || ok == NODE_STR ||
               ok == NODE_ARRAY;
    }
    return 0;
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
 *   数:       小さければ即値で入れる。大きければ (16進の
 *             アドレスなど) 住所の材料と同じ作り方で作る
 *   変数:     そのレジスタからの写し (addi rd, 相手, 0 = mv)
 *   文字列:   その住所を作る (auipc + addi の2命令)
 *   ポインタ読み: 住所から1バイト読む (lbu)。住所は、
 *             変数ならその値、数なら絶対アドレスそのもの */
static void gen_load_leaf(int reg, int leaf) {
    int k = node_kind[leaf];
    if (k == NODE_NUM) {
        if ((unsigned int)node_val[leaf] <= 2047u)
            asm_push(OP_ADDI, reg, REG_ZERO, 0, node_val[leaf]);
        else
            gen_load_addr(reg, (unsigned int)node_val[leaf]);
    } else if (k == NODE_VAR)
        asm_push(OP_ADDI, reg, var_reg(node_val[leaf]), 0, 0);
    else if (k == NODE_STR)
        gen_load_addr(reg, str_addr[node_val[leaf]]);
    else if (k == NODE_ARRAY)
        /* 配列の名前の「値」は先頭アドレス */
        gen_load_addr(reg, arr_addr[node_val[leaf]]);
    else { /* NODE_DEREF: lhs が住所を表すノード */
        int operand = node_lhs[leaf];
        if (node_kind[operand] == NODE_VAR) {
            /* 変数の持つ住所から読む (従来の形) */
            asm_push(OP_LBU, reg, var_reg(node_val[operand]), 0, 0);
        } else {
            /* 絶対アドレス読み: 住所を reg に作ってから読む。
             * 住所の材料は auipc 相対なので、0x10000005 の
             * ようなデバイスの住所でも壊れない */
            if (node_kind[operand] == NODE_NUM)
                gen_load_addr(reg, (unsigned int)node_val[operand]);
            else if (node_kind[operand] == NODE_ARRAY)
                gen_load_addr(reg, arr_addr[node_val[operand]]);
            else /* NODE_STR: 文字列の住所から読む */
                gen_load_addr(reg, str_addr[node_val[operand]]);
            asm_push(OP_LBU, reg, reg, 0, 0);
        }
    }
}

/* 式の AST から命令列を作る。結果は t1 に残る約束。
 * 失敗したら 0 を返す (理由は errmsg)。
 *
 * 作り方: 左の子を先に生成する (結果は t1)。右の子が葉
 * なら t2 に入れて演算する。右の子も式 (木) の場合は、
 * 左の結果をスタックに退避してから右を計算し、t2 へ
 * 写して左を戻す (v7 で入れた一般形の評価)。
 * スタックが要るプログラムでは、gen_all が先に sp を
 * 用意する (関数のあるプログラムと同じ仕組み) */
static int gen_expr(int node) {
    if (is_leaf(node)) {
        /* 葉: t1 にその値 (変数なら今の値、文字列なら住所、
         * ポインタ読みなら指された先の1バイト) が入る */
        gen_load_leaf(REG_T1, node);
        return 1;
    }
    if (node_kind[node] == NODE_DEREF) {
        /* 相手が計算式のポインタ読み: 住所を t1 で計算し、
         * その住所から1バイト読む */
        if (!gen_expr(node_lhs[node]))
            return 0;
        asm_push(OP_LBU, REG_T1, REG_T1, 0, 0);
        return 1;
    }
    if (node_kind[node] == NODE_INDEX) {
        /* 配列の要素読み: 添字を t1 で計算し、先頭アドレス
         * (t2) と足して、その場所から1バイト読む。
         * 要素は1バイトなので、足し算はそのまま使える */
        if (!gen_expr(node_rhs[node]))
            return 0;
        gen_load_addr(REG_T2, arr_addr[node_val[node]]);
        asm_push(OP_ADD, REG_T1, REG_T2, REG_T1, 0);
        asm_push(OP_LBU, REG_T1, REG_T1, 0, 0);
        return 1;
    }
    /* 二項演算: まず左を t1 に */
    if (!gen_expr(node_lhs[node]))
        return 0;
    int rhs = node_rhs[node];
    if (is_leaf(rhs)) {
        /* 右が葉: そのまま t2 に入れる */
        gen_load_leaf(REG_T2, rhs);
    } else {
        /* 右も式: 左の結果 (t1) をスタックへ退避し、右を
         * 計算して t2 へ写し、左を t1 へ戻す */
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
        asm_push(OP_SD, 0, REG_SP, REG_T1, 0);
        if (!gen_expr(rhs))
            return 0;
        asm_push(OP_ADDI, REG_T2, REG_T1, 0, 0);
        asm_push(OP_LD, REG_T1, REG_SP, 0, 0);
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
    }
    if (node_kind[node] == NODE_ADD)
        asm_push(OP_ADD, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_SUB)
        asm_push(OP_SUB, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_AND)
        asm_push(OP_AND, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_MUL)
        asm_push(OP_MUL, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_DIV)
        asm_push(OP_DIV, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_LT)
        /* slt: 左 (t1) < 右 (t2) なら 1 (符号付き比較) */
        asm_push(OP_SLT, REG_T1, REG_T1, REG_T2, 0);
    else if (node_kind[node] == NODE_GT)
        /* > は両辺を入れ替えた < 。slt の両辺を逆に置く */
        asm_push(OP_SLT, REG_T1, REG_T2, REG_T1, 0);
    else if (node_kind[node] == NODE_LE ||
             node_kind[node] == NODE_GE) {
        /* <= は「> でない」、>= は「< でない」。
         * 反対の比較をしてから 1 と XOR して反転する
         * (値は 1 か 0 なので、XOR 1 がちょうど反転になる) */
        if (node_kind[node] == NODE_LE)
            asm_push(OP_SLT, REG_T1, REG_T2, REG_T1, 0);
        else
            asm_push(OP_SLT, REG_T1, REG_T1, REG_T2, 0);
        asm_push(OP_XORI, REG_T1, REG_T1, 0, 1);
    }
    else {
        /* 比較 (== / !=): まず引き算して「差」にする。
         * 差が 0 なら等しい。あとは差を見て 1 か 0 を作る:
         *   != : sltu t1, zero, t1  (差が 0 でない = 1)
         *   == : sltiu t1, t1, 1    (差が 1 未満 = 差 0 = 1) */
        asm_push(OP_SUB, REG_T1, REG_T1, REG_T2, 0);
        if (node_kind[node] == NODE_NE)
            asm_push(OP_SLTU, REG_T1, REG_ZERO, REG_T1, 0);
        else
            asm_push(OP_SLTIU, REG_T1, REG_T1, 0, 1);
    }
    return 1;
}

/* 文の AST から命令列を作る。失敗したら 0。
 *
 * while の作り方 (命令列の並びがループの形そのもの):
 *   L_cond: 条件を計算 (結果は t1)
 *           beq t1, zero, L_end   (0 なら繰り返しを抜ける)
 *           本体の文...
 *           jal zero, L_cond      (条件の判定に戻る)
 *   L_end:
 * 飛び先 (L_cond / L_end) は「命令の番号」で表す。L_end は
 * 本体を作り終えた時点の末尾の番号なので、beq は飛び先を
 * 後から書き直す (asm_imm の書換え)。アセンブラでラベルを
 * 使うのと同じことを、番号と書換えでやっている */
static int gen_stmt(int node) {
    int kind = node_kind[node];
    if (kind == NODE_DECL) {
        /* 配列の宣言: 置き場所はコンパイラが静的に確保する
         * だけなので、実行時のコードは何も出ない */
        return 1;
    }
    if (kind == NODE_INDEX_STORE) {
        /* 配列の要素への書き込み: 先頭アドレス + 添字 の
         * 場所へ、値の下1バイトを書く (sb)。
         * 書く値が葉なら「住所を t2 → 値を t1 → sb」。
         * 値も式なら、住所をスタックへ退避してから値を
         * 計算する (値の計算が t2 を使うため) */
        int a = node_val[node];
        if (!gen_expr(node_lhs[node]))
            return 0;
        gen_load_addr(REG_T2, arr_addr[a]);
        asm_push(OP_ADD, REG_T2, REG_T2, REG_T1, 0);
        if (is_leaf(node_rhs[node])) {
            gen_load_leaf(REG_T1, node_rhs[node]);
            asm_push(OP_SB, 0, REG_T2, REG_T1, 0);
            return 1;
        }
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
        asm_push(OP_SD, 0, REG_SP, REG_T2, 0);
        if (!gen_expr(node_rhs[node]))
            return 0;
        asm_push(OP_LD, REG_T2, REG_SP, 0, 0);
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
        asm_push(OP_SB, 0, REG_T2, REG_T1, 0);
        return 1;
    }
    if (kind == NODE_ASSIGN) {
        if (!gen_expr(node_lhs[node]))
            return 0;
        /* addi 変数レジスタ, t1, 0 -- 結果を変数へ写す (代入) */
        asm_push(OP_ADDI, var_reg(node_val[node]), REG_T1, 0, 0);
        return 1;
    }
    if (kind == NODE_PUTC) {
        if (!gen_expr(node_lhs[node]))
            return 0;
        /* sb t1, 0(t0) -- 式の値の下1バイトを UART へ */
        asm_push(OP_SB, 0, REG_T0, REG_T1, 0);
        return 1;
    }
    if (kind == NODE_STORE) {
        /* メモリ書き込み: 先に「書く値」を t1 で計算し、
         * 次に「書き込み先の住所」を t2 に作って sb する。
         * この順番なのは、住所作りが t1 を壊さないため
         * (値の計算は t2 を下書きに使うことがある) */
        if (!gen_expr(node_rhs[node]))
            return 0;
        int addr = node_lhs[node];
        if (node_kind[addr] == NODE_NUM)
            gen_load_addr(REG_T2, (unsigned int)node_val[addr]);
        else /* NODE_VAR: 変数の値そのものが住所 (ポインタ) */
            asm_push(OP_ADDI, REG_T2, var_reg(node_val[addr]), 0, 0);
        /* sb t1, 0(t2) -- 値の下1バイトを住所へ書く */
        asm_push(OP_SB, 0, REG_T2, REG_T1, 0);
        return 1;
    }
    if (kind == NODE_CALL) {
        /* 呼び出し: 引数があれば式を t1 で計算して a0 へ写し、
         * jal ra で関数へ飛ぶ。飛び先は生成の最後に直すので、
         * ここでは関数番号を負数の印で入れておく */
        if (node_lhs[node] >= 0) {
            if (!gen_expr(node_lhs[node]))
                return 0;
            asm_push(OP_ADDI, REG_A0, REG_T1, 0, 0);
        }
        asm_push(OP_JAL, REG_RA, 0, 0, -(node_val[node] + 1));
        return 1;
    }
    if (kind == NODE_IF) {
        /* if の作り方:
         *   条件を計算 (結果は t1)
         *   beq t1, zero, L_else (または L_end)  (0 なら飛ぶ)
         *   成り立つときの文...
         *   (else がある場合) jal zero, L_end
         * L_else:
         *   else の文...
         * L_end:
         * 飛び先の書き直しは while と同じやり方 */
        if (!gen_expr(node_lhs[node]))
            return 0;
        int beq_index = nasm;
        asm_push(OP_BEQ, 0, REG_T1, REG_ZERO, 0);
        int then_body = node_rhs[node];
        for (int s = then_body; s != -1; s = node_next[s]) {
            if (!gen_stmt(s))
                return 0;
        }
        int else_body = node_val[node];
        if (else_body < 0) {
            /* else 無し: ここが L_end */
            asm_imm[beq_index] = nasm;
            return 1;
        }
        /* else あり: 本体を飛び越えるジャンプを置き、
         * beq の飛び先 (else の頭) を確定させる */
        int jal_index = nasm;
        asm_push(OP_JAL, 0, 0, 0, 0);
        asm_imm[beq_index] = nasm;
        for (int s = else_body; s != -1; s = node_next[s]) {
            if (!gen_stmt(s))
                return 0;
        }
        asm_imm[jal_index] = nasm;
        return 1;
    }
    if (kind == NODE_WHILE) {
        int l_cond = nasm;  /* 条件判定の命令番号 */
        if (!gen_expr(node_lhs[node]))
            return 0;
        /* beq t1, zero, L_end -- 飛び先はまだ分からないので
         * 仮に 0 を入れておき、下で書き直す */
        int beq_index = nasm;
        asm_push(OP_BEQ, 0, REG_T1, REG_ZERO, 0);
        int body = node_rhs[node];
        for (int s = body; s != -1; s = node_next[s]) {
            if (!gen_stmt(s))
                return 0;
        }
        /* jal zero, L_cond -- 条件の判定に戻る */
        asm_push(OP_JAL, 0, 0, 0, l_cond);
        /* ここ (末尾) が L_end。beq の飛び先を書き直す */
        asm_imm[beq_index] = nasm;
        return 1;
    }
    errmsg = "内部エラー: 知らない種類の文です";
    return 0;
}

/* スタックの天井の住所 (第2層が2パス間で確定させる)。
 * スタックはバイナリの後ろの空き地に置き、下へ伸ばす */
static unsigned int stack_top_addr;

/* 関数1つ分の命令列を作る: 入口の決まり文句 (プロローグ)
 * → 本体 → 出口の決まり文句 (エピローグ)。
 *   プロローグ: スタックに場所を取り、戻り先 ra を退避。
 *               引数があれば a0 から引数の変数へ写す
 *   エピローグ: ra をスタックから戻し、場所を返して、
 *               jalr で呼び出し側へ戻る
 * ra を退避するのが肝心で、これがあるから関数の中で
 * さらに別の関数を呼んでも (ra が上書きされても)
 * 自分の戻り先を失わない。失敗したら 0 */
static int gen_func(int f) {
    func_entry[f] = nasm;
    /* addi sp, sp, -16 / sd ra, 8(sp) */
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
    asm_push(OP_SD, 0, REG_SP, REG_RA, 8);
    if (func_has_param[f]) {
        /* addi 引数の変数, a0, 0 -- 引数を受け取る */
        asm_push(OP_ADDI, var_reg(func_param_var[f]), REG_A0, 0, 0);
    }
    for (int s = func_body[f]; s != -1; s = node_next[s]) {
        if (!gen_stmt(s))
            return 0;
    }
    /* ld ra, 8(sp) / addi sp, sp, 16 / jalr zero, ra, 0 */
    asm_push(OP_LD, REG_RA, REG_SP, 0, 8);
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
    asm_push(OP_JALR, 0, REG_RA, 0, 0);
    return 1;
}

/* スタックが要るかどうかを、AST を歩いて先に調べる。
 * 要るのは「関数がある」か「右の子が葉でない二項演算が
 * ある (評価に退避が要る)」場合。その場合だけ gen_all が
 * sp の用意を出すので、要らないプログラムの出力は
 * 従来と1バイトも変わらない */
static int stack_needed;

static int expr_needs_stack(int node) {
    int k = node_kind[node];
    if (k == NODE_ADD || k == NODE_SUB || k == NODE_AND ||
        k == NODE_EQ || k == NODE_NE || k == NODE_MUL ||
        k == NODE_DIV || k == NODE_LT || k == NODE_GT ||
        k == NODE_LE || k == NODE_GE) {
        if (!is_leaf(node_rhs[node]))
            return 1;
        return expr_needs_stack(node_lhs[node]) ||
               expr_needs_stack(node_rhs[node]);
    }
    if (k == NODE_DEREF)
        return expr_needs_stack(node_lhs[node]);
    if (k == NODE_INDEX)
        return expr_needs_stack(node_rhs[node]);
    return 0;
}

static int list_needs_stack(int head);  /* 相互に使うので先行宣言 */

static int stmt_needs_stack(int node) {
    int k = node_kind[node];
    if (k == NODE_ASSIGN || k == NODE_PUTC)
        return expr_needs_stack(node_lhs[node]);
    if (k == NODE_WHILE)
        return expr_needs_stack(node_lhs[node]) ||
               list_needs_stack(node_rhs[node]);
    if (k == NODE_IF)
        return expr_needs_stack(node_lhs[node]) ||
               list_needs_stack(node_rhs[node]) ||
               (node_val[node] >= 0 &&
                list_needs_stack(node_val[node]));
    if (k == NODE_CALL)
        return node_lhs[node] >= 0 &&
               expr_needs_stack(node_lhs[node]);
    if (k == NODE_STORE)
        return expr_needs_stack(node_rhs[node]);
    if (k == NODE_INDEX_STORE)
        /* 値が葉でない書き込みは、住所の退避にスタックを使う */
        return expr_needs_stack(node_lhs[node]) ||
               !is_leaf(node_rhs[node]) ||
               expr_needs_stack(node_rhs[node]);
    return 0;
}

static int list_needs_stack(int head) {
    for (int s = head; s != -1; s = node_next[s]) {
        if (stmt_needs_stack(s))
            return 1;
    }
    return 0;
}

/* 命令列を全部作る (1回分): 土台 (UART の住所) → (関数が
 * あれば) スタックの用意 → 文の並び → 最後の式 (あれば) →
 * UART へ書く → 止まる → その後に関数の本体を並べる。
 * 関数の本体は「止まる」の後ろに置くので、上から実行して
 * 関数の中に落ちてくることはない (関数は呼び出しでのみ入る)。
 * 最後に、呼び出しの飛び先 (負数の印) を関数の入口番号へ
 * 書き直す。失敗したら 0 */
static int gen_all(void) {
    nasm = 0;
    /* lui t0, 0x10000 -- UART ベース 0x10000000 を t0 に作る */
    asm_push(OP_LUI, REG_T0, 0, 0, 0x10000);
    if (stack_needed) {
        /* スタックの用意: 天井の住所を sp に作る
         * (要らないプログラムでは出さない。昔のプログラムの
         * 出力が変わらないようにするため) */
        gen_load_addr(REG_SP, stack_top_addr);
    }
    for (int s = prog_stmts; s != -1; s = node_next[s]) {
        if (!gen_stmt(s))
            return 0;
    }
    if (prog_has_final) {
        if (!gen_expr(prog_final))
            return 0;
        /* sb t1, 0(t0) -- 最後の式の下1バイトを UART へ */
        asm_push(OP_SB, 0, REG_T0, REG_T1, 0);
    }
    if (prog_main >= 0) {
        /* main の形 (v12): 入口として main を呼ぶ。
         * 飛び先は通常の呼び出しと同じ印 (-(番号+1)) で置き、
         * この後の一括書き直しで確定する。main から戻ると
         * ra の指す次の命令 = wfi に落ちて止まる。
         * main の入口では、戻り先 (wfi の住所) が ra に
         * 入っているのを通常の関数と同じく退避するので、
         * 呼び出しの作法そのままで動く */
        asm_push(OP_JAL, REG_RA, 0, 0, -(prog_main + 1));
    }
    /* wfi -- 止まる */
    asm_push(OP_WFI, 0, 0, 0, 0);
    for (int f = 0; f < nfunc; f++) {
        if (!gen_func(f))
            return 0;
    }
    /* 呼び出しの飛び先を書き直す (印: -(関数番号+1)) */
    for (int i = 0; i < nasm; i++) {
        if (asm_op[i] == OP_JAL && asm_imm[i] < 0)
            asm_imm[i] = func_entry[-asm_imm[i] - 1];
    }
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
static int gen_program(void) {
    code_insts = 0;
    /* スタックが要るかは生成の前に AST から決める */
    stack_needed = nfunc > 0 || list_needs_stack(prog_stmts) ||
                   (prog_has_final && expr_needs_stack(prog_final));
    for (int f = 0; f < nfunc && !stack_needed; f++)
        stack_needed = list_needs_stack(func_body[f]);
    stack_top_addr = LOAD_BASE;  /* 1回目は仮の値 (個数は同じ) */
    for (int i = 0; i < narr; i++)
        arr_addr[i] = LOAD_BASE;  /* 同上 (配列の住所も仮置き) */
    if (!gen_all())
        return 0;
    code_insts = nasm;
    for (int i = 0; i < nstr; i++)
        str_addr[i] = LOAD_BASE + (unsigned int)nasm * 4u +
                      (unsigned int)str_off[i];
    /* 配列の領域は文字列の後ろに置く */
    for (int i = 0; i < narr; i++)
        arr_addr[i] = LOAD_BASE + (unsigned int)nasm * 4u +
                      (unsigned int)str_used +
                      (unsigned int)arr_off[i];
    /* スタックは「コード + 文字列 + 配列」の後ろ 4KB 先から
     * 下へ伸ばす。16 バイト単位に切り上げておく
     * (置き場所の約束) */
    stack_top_addr = LOAD_BASE + (unsigned int)nasm * 4u +
                     (unsigned int)str_used +
                     (unsigned int)arr_used + 4096u;
    stack_top_addr = (stack_top_addr + 15u) & ~15u;
    if (!gen_all())
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
    if (op == OP_SD) {
        /* sd rs2, imm(rs1) -- S 形式 (sb と同じ並び)。
         * sb との違いは funct3 が 3 (8バイト幅) なこと */
        return ((unsigned int)(imm >> 5) << 25) |
               ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               (3u << 12) | ((unsigned int)(imm & 31) << 7) | 0x23u;
    }
    if (op == OP_LD) {
        /* ld rd, imm(rs1) -- I 形式。lbu との違いは funct3 が 3 */
        return ((unsigned int)imm << 20) | ((unsigned int)rs1 << 15) |
               (3u << 12) | ((unsigned int)rd << 7) | 0x03u;
    }
    if (op == OP_JALR) {
        /* jalr rd, rs1, imm -- I 形式 (opcode 0x67)。
         * rd=0, rs1=ra, imm=0 で「呼び出し側へ戻る」になる */
        return ((unsigned int)imm << 20) | ((unsigned int)rs1 << 15) |
               ((unsigned int)rd << 7) | 0x67u;
    }
    if (op == OP_MUL) {
        /* mul rd, rs1, rs2 -- R 形式。M 拡張の命令は
         * funct7 が 1。funct3 は 0 */
        return (1u << 25) | ((unsigned int)rs2 << 20) |
               ((unsigned int)rs1 << 15) |
               ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_DIV) {
        /* div rd, rs1, rs2 -- R 形式、funct7 が 1 で
         * funct3 が 4。除数が 0 のときの結果は -1
         * (RISC-V の決まり。例外にはならない) */
        return (1u << 25) | ((unsigned int)rs2 << 20) |
               ((unsigned int)rs1 << 15) | (4u << 12) |
               ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_AND) {
        /* and rd, rs1, rs2 -- R 形式。add との違いは funct3 が 7 */
        return ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               (7u << 12) | ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_SLT) {
        /* slt rd, rs1, rs2 -- R 形式。sltu との違いは
         * funct3 が 2 (符号付きで比べる) */
        return ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               (2u << 12) | ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_XORI) {
        /* xori rd, rs1, imm -- I 形式。addi との違いは funct3 が 4 */
        return ((unsigned int)imm << 20) | ((unsigned int)rs1 << 15) |
               (4u << 12) | ((unsigned int)rd << 7) | 0x13u;
    }
    if (op == OP_SLTU) {
        /* sltu rd, rs1, rs2 -- R 形式。add との違いは funct3 が 3 */
        return ((unsigned int)rs2 << 20) | ((unsigned int)rs1 << 15) |
               (3u << 12) | ((unsigned int)rd << 7) | 0x33u;
    }
    if (op == OP_SLTIU) {
        /* sltiu rd, rs1, imm -- I 形式。addi との違いは funct3 が 3 */
        return ((unsigned int)imm << 20) | ((unsigned int)rs1 << 15) |
               (3u << 12) | ((unsigned int)rd << 7) | 0x13u;
    }
    if (op == OP_BEQ) {
        /* beq rs1, rs2, offset -- B 形式 (分岐)。
         * 命令列での飛び先は「命令番号」なので、まず
         * バイト単位の相対オフセットに直す:
         *   offset = (飛び先番号 - 自分の番号) x 4
         * 即値 (オフセット) のビットは形式の都合で
         * バラバラの場所に散らばっているのが B 形式の特徴:
         * [ imm[12] << 31 | imm[10:5] << 25 | rs2 << 20
         *   | rs1 << 15 | funct3(0) << 12 | imm[4:1] << 8
         *   | imm[11] << 7 | opcode 0x63 ]
         * オフセットの下1ビットは常に 0 (命令は4バイト単位)
         * なので格納されない。届く範囲は 前後 約4KB */
        int off = (imm - i) * 4;
        return ((unsigned int)((off >> 12) & 1) << 31) |
               ((unsigned int)((off >> 5) & 0x3f) << 25) |
               ((unsigned int)rs2 << 20) |
               ((unsigned int)rs1 << 15) |
               ((unsigned int)((off >> 1) & 0xf) << 8) |
               ((unsigned int)((off >> 11) & 1) << 7) | 0x63u;
    }
    if (op == OP_JAL) {
        /* jal rd, offset -- J 形式 (ジャンプ)。
         * 相対オフセットへの直し方は beq と同じ。ビットの
         * 散らばり方は J 形式の流儀:
         * [ imm[20] << 31 | imm[10:1] << 21 | imm[11] << 20
         *   | imm[19:12] << 12 | rd << 7 | opcode 0x6f ]
         * 届く範囲は 前後 約1MB。rd が 0 のときは「戻り先を
         * 保存しない単なるジャンプ」(アセンブリの j) になる */
        int off = (imm - i) * 4;
        return ((unsigned int)((off >> 20) & 1) << 31) |
               ((unsigned int)((off >> 1) & 0x3ff) << 21) |
               ((unsigned int)((off >> 11) & 1) << 20) |
               ((unsigned int)((off >> 12) & 0xff) << 12) |
               ((unsigned int)rd << 7) | 0x6fu;
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
 * 入力 (v10): ファイルからプログラムを読む
 *
 * 引数が「.c で終わる名前」ならファイルとして開く
 * (開けなければエラー)。それ以外でも、同名のファイルが
 * 開けるならその中身を、開けないなら引数そのものを
 * プログラムとして読む (従来の使い方もそのまま残す)。
 * 読み込んだソースは src_buf に置く。上限は約64KB。
 * ================================================================ */

static char src_buf[65536];

/* path が ".c" で終わる名前かどうか */
static int ends_with_c(char *path) {
    int len = 0;
    while (path[len] != '\0')
        len++;
    return len >= 2 && path[len - 2] == '.' && path[len - 1] == 'c';
}

/* ファイルを src_buf へ読み込む。
 * 戻り値: 1 = 読めた、0 = ファイルとして開けなかった
 * (引数そのものとして扱ってよい)、-1 = エラー
 * (errmsg に理由。開けたのに読めなかった、大きすぎる等) */
static int load_file(char *path) {
    int fd = open(path, 0);
    if (fd < 0)
        return 0;
    int used = 0;
    for (;;) {
        if (used >= 65535) {
            close(fd);
            errmsg = "ファイルが大きすぎます (約64KBまで)";
            return -1;
        }
        long n = read(fd, src_buf + used, 65535 - used);
        if (n < 0) {
            close(fd);
            /* ディレクトリなど、開けても読めないものは
             * 「ファイルではなかった」として扱う */
            return 0;
        }
        if (n == 0)
            break;
        used += (int)n;
    }
    close(fd);
    src_buf[used] = '\0';
    return 1;
}

/* ================================================================
 * 前処理 (v13): 単純な #define だけ
 *
 * 行頭 (空白の後ろでもよい) の「#define 名前 置き換え」の行を
 * 読み取り、以降の文に出てくる「名前」を単語単位で置き換えた
 * 新しいソースを作る。parse は展開後のソースだけを見る。
 * 決め事 (計画で「実装するときに決める」と置いた点):
 *   - 置き換えは単語 (識別子) 単位。文字列の中と // コメントの
 *     中は置き換えない (C と同じ)
 *   - 置き換えた結果はもう一度走査しない (入れ子は展開しない)
 *   - 再定義はエラー。define は最大16個、置き換えは127文字まで
 *   - # で始まる行は #define 以外エラー (#include 等は対象外)
 * なお iwac 自身のソースは今もプリプロセッサ無しのままで、
 * これは「コンパイルされる側のプログラム」のための機能。
 * ================================================================ */

static char def_names[16][32];
static int def_name_len[16];
static char def_text[16][128];
static int def_text_len[16];
static int ndef;

static char exp_buf[131072];
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

/* 展開先へ1文字書く (あふれたら印だけ付ける) */
static void exp_ch(char c) {
    if (exp_len >= 131071) {
        exp_failed = 1;
        return;
    }
    exp_buf[exp_len++] = c;
}

static void exp_str(char *s, int len) {
    for (int i = 0; i < len; i++)
        exp_ch(s[i]);
}

/* #define を展開し、結果を exp_buf に置く。成功したら 1 */
static int expand_defines(char *src) {
    ndef = 0;
    exp_len = 0;
    exp_failed = 0;
    int i = 0;
    int sol = 1;  /* 行頭 (その行にまだ空白しか無い) か */
    while (src[i] != '\0') {
        if (sol) {
            /* 行頭の空白の先が # なら指令行として処理する */
            int j = i;
            while (src[j] == ' ' || src[j] == '\t')
                j++;
            if (src[j] == '#') {
                j++;
                while (src[j] == ' ' || src[j] == '\t')
                    j++;
                char *kw = "define";
                int ok = 1;
                for (int k = 0; k < 6; k++)
                    if (src[j + k] != kw[k])
                        ok = 0;
                if (ok && is_ident_char(src[j + 6]))
                    ok = 0;
                if (!ok) {
                    errmsg = "# で始まる行は #define だけが使えます";
                    return 0;
                }
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
                if (nlen > 31) {
                    errmsg = "#define の名前が長すぎます";
                    return 0;
                }
                if (find_define(src + ns, nlen) >= 0) {
                    errmsg = "#define の名前が重なっています";
                    return 0;
                }
                if (ndef >= 16) {
                    errmsg = "#define は16個までです";
                    return 0;
                }
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
                int tlen = te - ts;
                if (tlen > 127) {
                    errmsg = "#define の置き換えが長すぎます";
                    return 0;
                }
                int d = ndef++;
                for (int k = 0; k < nlen; k++)
                    def_names[d][k] = src[ns + k];
                def_name_len[d] = nlen;
                for (int k = 0; k < tlen; k++)
                    def_text[d][k] = src[ts + k];
                def_text_len[d] = tlen;
                i = j;  /* 行末 (改行か終端) の位置へ */
                if (src[i] == '\n') {
                    exp_ch('\n');
                    i++;
                }
                sol = 1;
                continue;
            }
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
        if (c == '/' && src[i + 1] == '/') {
            /* コメントの中も置き換えない */
            while (src[i] != '\0' && src[i] != '\n') {
                exp_ch(src[i]);
                i++;
            }
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
    if (exp_failed) {
        errmsg = "展開後のソースが大きすぎます";
        return 0;
    }
    exp_buf[exp_len] = '\0';
    return 1;
}

/* ================================================================
 * main: 層を順に呼ぶだけ
 *   parse (失敗したら何も出さない) → gen (同) → write
 * ================================================================ */

int main(int argc, char **argv) {
    if (argc != 2) {
        print_str(2, "usage: iwac <ファイル.c>   (例: iwac hello.c > hello.bin)\n       iwac '<プログラム>'   (従来の形: 引数そのものをコンパイル)\n");
        return 1;
    }

    /* 入力の決定: .c ファイルは必ずファイルとして読む。
     * それ以外は、ファイルが開ければファイル、無ければ
     * 引数そのものがプログラム */
    char *src = argv[1];
    int must_file = ends_with_c(argv[1]);
    int loaded = load_file(argv[1]);
    if (loaded < 0 || (must_file && loaded == 0)) {
        print_str(2, "iwac: ");
        if (loaded < 0)
            print_str(2, errmsg);
        else {
            print_str(2, "ファイルが開けません: ");
            print_str(2, argv[1]);
        }
        print_str(2, "\n");
        return 1;
    }
    if (loaded == 1)
        src = src_buf;

    /* 前処理: #define を展開してから parse に渡す (v13) */
    if (!expand_defines(src)) {
        print_str(2, "iwac: ");
        print_str(2, errmsg);
        print_str(2, "\n");
        return 1;
    }
    src = exp_buf;

    if (!parse_program(src)) {
        print_str(2, "iwac: ");
        print_str(2, errmsg);
        print_str(2, "\n");
        return 1;
    }

    if (!gen_program()) {
        print_str(2, "iwac: ");
        print_str(2, errmsg);
        print_str(2, "\n");
        return 1;
    }

    write_binary();
    return 0;
}
