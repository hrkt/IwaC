# IwaC

Cコンパイラのサブセットを自作するプロジェクト。

最終的なゴールは KosabaOS（自作OS）のプロジェクトと接続している:
OS上のシェルからコンパイラを実行して Hello World をコンパイルし、
実行できるようにすること。そのコンパイラが、この `iwac`。

欲張らない方針で進める。プリプロセッサなどは対象外
（計画は `doc/roadmap.md`、設計は `doc/design.md`、
できたことは `doc/progress.md`、決めたことは `doc/notes.md`）。

ここまでの歩み（v0〜v4）を図で説明したページもある:
[`doc/phases.html`](doc/phases.html)（単体で開ける HTML、
初心者向けに各フェーズで何をやったかを解説）。

## 現在地: v4

現在の言語には `while` と出力文があり、目標の print loop の
本体が書けるようになった:

- プログラムは、1文字（例: `A`、v0互換）、式（例: `65+1`）、
  または文の並びに任意で最後の式を続けたもの。文は代入文、
  `putc(式);` の出力文、`while (式) { 文の並び }`。
  項に使える数字は 0..2047、変数は最大4個（レジスタに割り当て）
- 文字列リテラルが書け、変数に入れて `*s` で先の1バイトを
  読める。文字列はバイナリのコードの後ろに置かれ、値はその住所
- KosabaOS の print loop（`uart_puts`）の本体に当たる
  `s = "hello"; while (*s) { putc(*s); s = s + 1; }` が
  コンパイルでき、実行すると `hello` と出る
- 目下の目標は、KosabaOS の print loop（`uart_puts`）を
  コンパイルできるように育てること（計画は `doc/roadmap.md`）
- `iwac` は、実行時にプログラムを計算して最後の式の値の下1バイトを
  UART に出し、止まる RISC-V (rv64i) の生バイナリを標準出力に
  書き出す（`65` だけなら16バイト、文や項が増えると少しずつ育つ）
- 出力バイナリは QEMU virt で `-kernel` に渡すと、OpenSBI が
  `0x80200000` に置いて実行する（KosabaOS と同じ土俵）

## ビルドと実行

ビルド（最初はホストの gcc で。将来のセルフホストは夢として置く）:

```sh
gcc -std=c11 -Wall -Wextra -O2 -o iwac iwac.c
```

コンパイルと実行:

```sh
./iwac '65+1' > a.bin   # 24バイトのバイナリができる
qemu-system-riscv64 -machine virt -bios default -kernel a.bin \
    -m 128M -nographic  # シリアルに B と出て止まる (終了は Ctrl-A x)
```

まとめて確認する検証スクリプトもある:

```sh
./test.sh
```

環境の作り方（ツールの種類・バージョン・入手方法）は
`doc/environment.md` にメモしてある。ツール本体はこのリポジトリには
入れず、手元ディスクの別ディレクトリに置く。
