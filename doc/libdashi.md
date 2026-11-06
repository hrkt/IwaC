# libdashi — 共通関数の小さいライブラリ

「Libc とまでは言わない」小さい共通関数の束。
名前は **libdashi**（出汁＝料理の土台、と
「lib da C」の二重掛け。2026-11-06 命名）。

## なぜ作るか（重複の棚卸し、2026-11-06）

iwac の言語で書いたプログラムが増え、同じ
小物が何度も書かれている:

- 文字列比較: iwac 本体 (07-main の str_eq)、
  nori (str_eq)、Misoni (seq と改名して再実装)
  で3つ
- 数値の出力: rt (rt_putdec / rt_puthex)、
  Misoni (say_n)、nori の中にも同等品
- 長さ・連結: Misoni の slen / append_str は
  その場で書かれたもの
- fd へ文字列を出す: nori の puts_fd、
  Misoni の say、rt の rt_puts

## 形

- 本体は1ファイル: `libdashi/libdashi.c`
  （iwac の言語で書く。外部関数は write だけを
  手で宣言する）
- 消費者は、使う関数のプロトタイプを自分の
  ソースの頭に書く（nori / Misoni が open 等を
  宣言しているのと同じ流儀）
- 一括コンパイルでは、プログラムの入力に
  libdashi.c を足す。分割コンパイルでは
  libdashi.c をオブジェクト化して nori で
  つなぐ（Misoni のマニフェストなら
  `obj libdashi.iwo libdashi/libdashi.c` の
  1行が足せる）
- 関数名は `ds_` 接頭辞で揃える（rt の
  `rt_` と同じ流儀。ホスト側の libc と名前が
  衝突しないための実利もある）

## 線引き

- **libdashi に入るもの**: 純粋な関数（文字列・
  数値変換）と、write の上に乗る最小の出力
  ヘルパ
- **入らないもの**: 入出力の舞台そのもの
  （ファイルの開閉、fd 1 のファイル化）は
  kosaba_rt.c の領分のまま。rt は自己完結を
  保ち、libdashi には依存しない
- **iwac 本体は対象外**: 本体は decls.c の
  内部共有で足りており、自分の道具に依存
  しない今の形を保つ。libdashi の消費者は
  「iwac の言語で書くプログラム」たち
- アーカイブ（必要なものだけ引く仕組み）は
  作らない。nori は渡されたオブジェクトを
  全部つなぐ今の形で、プログラムが小さい
  間は十分。実需が出たら nori の次の段で

## 初版の範囲（v1）

文字列:

- `int ds_strlen(char *s)`
- `int ds_streq(char *a, char *b)`
- `int ds_strcpy(char *dst, char *src)`
  （写して、写した長さを返す）
- `int ds_append(char *dst, int pos, char *src)`
  （pos の位置から足して、新しい pos を返す）

数値と文字列の変換（バッファへ書く純粋関数）:

- `int ds_dec(char *buf, long v)`
  （10進で書いて長さを返す。負数も扱う）
- `int ds_hex(char *buf, long v)`
  （16進・小文字・ゼロ詰めなしで書き、長さを返す）
- `long ds_atoi(char *s)`
  （数字が続く間だけ読む。符号も見る）
- `int ds_hexval(int c)`（16進1文字 → 値。
  違えば -1）

出力（write の上）:

- `void ds_puts(int fd, char *s)`
- `void ds_putdec(int fd, long v)`
- `void ds_puthex(int fd, long v)`

## 移行の記録

- Misoni: slen / seq / say / say_n / append_str
  を libdashi へ（移行第1号）
- nori: puts_fd / str_eq / hex_val を libdashi へ
- rt と iwac 本体は移行しない（線引きのとおり）
