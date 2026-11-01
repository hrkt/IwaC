# IwaC 環境の作り方メモ

ツール本体はリポジトリに入れないが、環境を再現できるように
作り方（種類・バージョン・入手方法・手順）はこのメモに残す。
環境のセットアップは自動化せず、このメモを見て手で行う方針
（KosabaOS と同じ）。

## ワークスペース構成

```text
~/workspace/iwac/          -- プロジェクトの置き場 (リポジトリではない)
  iwac/                    -- 唯一の Git リポジトリ (hrkt/IwaC)
                            自分で書いたものだけ: iwac.c, test.sh,
                            README.md, doc/
  tools/                   -- ツール置き場 (リポジトリ管理対象外)
  reference/               -- 参考コード置き場 (同上、丸ごと入れない)
```

## v0 の検証に使ったツール（2026-11-01 時点の実測）

すべて Muse のマシンにあったシステム側ツールで検証した:

- ホスト gcc: Ubuntu gcc 13.3.0 (`gcc --version`)
  - `iwac` 本体のビルド用。`gcc -std=c11 -Wall -Wextra -O2`
- QEMU: 8.2.2 (`qemu-system-riscv64 --version`)
  - 出力バイナリのベアメタル実行用 (`-machine virt -bios default`)
- riscv64 向け objdump: GNU Binutils 2.42
  (`riscv64-linux-gnu-objdump`)、逆アセンブル確認用（任意）

マシンが入れ替わるとシステム側ツールは消えることがある
（KosabaOS で実際に起きた）。その場合は apt で入れ直す:

```sh
sudo apt-get install -y gcc qemu-system-misc binutils-riscv64-linux-gnu
```

KosabaOS 側の `~/workspace/kosaba-os/tools/` にも、ポータブルな
QEMU 8.2.2 と xPack riscv-none-elf-gcc 15.2.0 が常駐化してあるので、
必要ならそちらも使える（置き場所と再現手順は KosabaOS 側の
`tools/README.md`）。

## GitHub への push

- リモート: `git@github.com:hrkt/IwaC.git`
- GitHub のデプロイキーは1リポジトリ専用で使い回せない。
  KosabaOS 用の鍵はそちらに登録済みなので、IwaC には専用の鍵を
  1つ作った: `~/.ssh/id_ed25519_iwac` (秘密鍵はマシン外に出さない)。
  公開鍵 (`id_ed25519_iwac.pub`) を IwaC リポジトリの
  Settings → Deploy keys に登録し、Allow write access を付ける。
- このリポジトリには `core.sshCommand` で上記鍵を明示する設定を
  入れてある（実行環境が root で動き、素の `ssh` は鍵の場所を
  取り違えるための対策も兼ねる）。ので、push はそのまま:

```sh
git push origin main
```

- 2026-11-01: KosabaOS 用の鍵で push を試みて
  "Permission to hrkt/IwaC.git denied to deploy key" と拒否されたのが、
  専用鍵を作った理由。専用鍵の登録が済めば、ローカルにある
  コミットをそのまま push できる。

## ビルドと検証のコマンド

```sh
cd ~/workspace/iwac/iwac
gcc -std=c11 -Wall -Wextra -O2 -o iwac iwac.c
./iwac A > a.bin
./test.sh   # ビルド、16バイト確認、QEMU実行まで一式
```
