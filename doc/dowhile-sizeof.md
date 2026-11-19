# v48 設計: do-while と sizeof(式)

doc/missing-features.md の「小物」のうち2つ:
do-while と sizeof(式)。どちらも parse が
中心で、生成は do-while の分だけ。

## do-while

文法: `do 本体 while ( 式 );`
本体は while と同じ parse_body ({ } でも
文1つでもよい)。終わりの `;` は必須
(C と同じ)。

ノードは新種 NODE_DOWHILE = 62:
node_lhs = 条件の式、node_rhs = 本体の文の
並び (NODE_WHILE と同じ欄の使い方)。

生成 (gen_stmt): while の裏返しで、
分岐の道具は既存の emit_beq_t1z をそのまま
使う (長い分岐への伸ばし relax の仕組みに
新しい分岐種を足さないため、「真なら戻る」を
「偽なら落ちる + jal で戻る」に分解する。
while が末尾で jal 戻りするのと同じ形):

    L_top:  本体
    L_cond: 条件 → t1        ← continue の飛び先
            beq t1, zero → L_end
            jal zero, L_top
    L_end:                   ← break の飛び先

break / continue は既存の loop_* の積み重ねを
そのまま使う (parse 側も parse_loop_depth を
本体の間だけ上げる)。type_node と
stmt_needs_stack には NODE_WHILE と同じ
扱いで NODE_DOWHILE を足す。

## sizeof(式)

現行 (v37) は sizeof ( 型 ) だけ。C のもう
半分、sizeof 式 (評価しない) を足す。

### 振り分け

parse_primary_node の sizeof で:

1. 次が `(` なら、まず型として読んでみる
   (キャストの判定と同じ巻き戻し式:
   位置を保存 → parse_type → `)` が続けば
   型の形として確定し、現行どおりの大きさ)。
2. そうでなければ単項式 (parse_unary_expr)
   を読み、式の木から大きさだけを求めて
   NODE_NUM にする。木は捨てる (生成に
   渡らない = 評価されない)。ただし読みの
   途中のエラー (未定義の名前など) は
   そのままエラーになる。

`sizeof x` (かっこ無し) も C どおり受ける。

### 大きさの求め方

parse 側に再帰関数を2つ置く
(node_maybe_fp と同じ流儀で、生成の
type_node の規則に合わせる):

- size_type_of(node): 式の型を
  (型タグ etype, ポインタ段数, 構造体番号)
  の3つ組で返す。葉は変数・配列・メンバ・
  関数・署名の各表から、二項演算は
  type_node の規則 (double があれば double、
  幅つきがあればその型 (両方なら左)、
  それ以外は 8。ポインタの + / - は
  ポインタのまま) で組み立てる。
- expr_size(node): 大きさそのもの。
  配列名 (全体のバイト数)、2次元の行
  (行のバイト数)、文字列 (長さ + 1) は
  特別扱いし、それ以外は3つ組から
  (ポインタなら 8、構造体なら
  stag_size、それ以外は etype の幅)。

分かる範囲は式の全種:
数・文字・浮動小数点リテラル、変数、配列と
要素、ポインタ添字、メンバ (配列メンバの
全体も)、* と &、呼び出し (直接・間接)、
関数名、二項・単項・比較・論理・三項、
キャスト (ノードになるもの)、代入、
++/--、syscall3。分からない形はエラーに
する (黙って 8 を返さない)。

### 割り切り

- sizeof の中身は iwac の型付け規則で
  測る。C と違う点が2つある: (1) 幅つきの
  演算は C では int に昇格するが、iwac の
  type_node は「幅つきがあればその型
  (両方なら左)」なので、sizeof(int16 +
  int16) も sizeof(int16 + int) も 2。
  iwac の中では値の型がそうなっている
  ので、こちらが一貫する。(2) 読み捨て
  られるキャスト (v25 の割り切り: 従来の
  型やポインタへの変換) は木に残らない
  ので効かない。sizeof((char)x) は 8
  (キャスト自体が iwac では無変換)。
- 構造体を返す関数の呼び出しは、値の
  大きさ (stag_size) を返す (v45 の
  値セマンティクスに合わせる)。
- 絶対アドレス読み (*0x...) の大きさは
  1 (生成の deref_etype の約束と同じ)。

## 容量への影響

増える関数は size_type_of と expr_size の
2つ (実測で確認)。表は増えない。
NODE_DOWHILE は decls.c にも宣言を足す
(持ち主は 02-parse.c、並びは NODE_NUMFP
の次 = 最初に現れる位置)。
