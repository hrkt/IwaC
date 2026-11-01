# IwaC

Cコンパイラのサブセットを自作するプロジェクト。

最終的なゴールは KosabaOS（自作OS）のプロジェクトと接続している:
OS上のシェルからコンパイラを実行して Hello World をコンパイルし、
実行できるようにすること。そのコンパイラが、この `iwac`。

欲張らない方針で進める。プリプロセッサなどは対象外
（計画は `doc/roadmap.md`、設計は `doc/design.md`、
できたことは `doc/progress.md`、決めたことは `doc/notes.md`）。

## 現在地: v0

一番小さいところから始める。v0 の言語は「1バイトだけ」:

- プログラムは、1文字（例: `A`）または 10進数 0..255（例: `65`）
- `iwac` は、その1バイトを UART に出して止まる RISC-V (rv64i) の
  生バイナリ（16バイト、命令4つ）を標準出力に書き出す
- 出力バイナリは QEMU virt で `-kernel` に渡すと、OpenSBI が
  `0x80200000` に置いて実行する（KosabaOS と同じ土俵）

## ビルドと実行

ビルド（最初はホストの gcc で。将来のセルフホストは夢として置く）:

```sh
gcc -std=c11 -Wall -Wextra -O2 -o iwac iwac.c
```

コンパイルと実行:

```sh
./iwac A > a.bin        # 16バイトのバイナリができる
qemu-system-riscv64 -machine virt -bios default -kernel a.bin \
    -m 128M -nographic  # シリアルに A と出て止まる (終了は Ctrl-A x)
```

まとめて確認する検証スクリプトもある:

```sh
./test.sh
```

環境の作り方（ツールの種類・バージョン・入手方法）は
`doc/environment.md` にメモしてある。ツール本体はこのリポジトリには
入れず、手元ディスクの別ディレクトリに置く。
