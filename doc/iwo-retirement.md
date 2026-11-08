# .iwo 引退の設計 (v42)

## 決定

2026-11-08、ユーザーが .iwo (自作テキスト
オブジェクト形式) の引退を決めた。仕様の記録
(doc/object-format.md) は歴史的資産として
残し、現役の形式は ELF オブジェクト (v39、
doc/elf-object.md) に一本化する。

.iwo は段1〜段6 の階段を支えた形式で、役割は
終わった: ELF 経由のリンクが .iwo 経由と
バイト一致することが v39 以来の物差しで
確認できており、残る利用者は移行できる。

## 現役の利用者 (引退で動くもの)

1. **Asari**: 出力が .iwo。チェーン
   (iwac → .ias → Asari → オブジェクト →
   nori) の要なので、ELF 書き手を Asari に
   足すのが一番大きい仕事
2. **nori**: .iwo と ELF の両読み。 .iwo の
   読み口 (テキスト解析) を撤去する
3. **iwac**: `--object` 出口 (write_object)
   を撤去する。再配置の収集 (obj_* の表) は
   --elf / --asari と共有なので残す
4. **Misoni**: マニフェストが --object を
   使っている。 --elf への切替が、そのまま
   残件だった「Misoni の標準経路の ELF 化」
   になる
5. **test.sh の mini_nori**: .iwo を読む
   参照リンカ。分割チェックは v39 以来の形
   (nori で ELF オブジェクトをリンクして
   一括版と比べる) に寄せて引退させる

## 手順 (各段で物差しを通してから次へ)

1. **Asari の ELF 出力**: asari.c に ELF
   書き手を足す (iwac の write_elf_object と
   同じ意味論。シンボルの数え直し方式も同じ)。
   物差し: Asari の .eo が iwac の .eo と
   バイト一致 (現行の .iwo 一致の置き換え)
2. **Misoni の ELF 切替**: マニフェストと
   駆動を --elf に。物差し: OS 上の固定点が
   従来どおり一致
3. **nori の .iwo 撤去**: テキスト読み口を
   抜く。.iwo を渡したら形式エラーにする
4. **iwac の --object 撤去**: write_object と
   obj_hex 系の .iwo 専用書き手を抜く
5. **test.sh の追随と全体 PASS**: mini_nori
   引退、.iwo 前提のチェックを ELF 前提へ
6. **文書**: doc/object-format.md の冒頭に
   「歴史的記録 (v42 で引退)」と明記して
   残置。elf-object.md / README / roadmap /
   selfhost / progress / phases.html を更新

## 触らないもの

- .ias (Asari への入力テキスト) は現役のまま。
  引退するのは .iwo だけ
- 一括コンパイルとベアメタル出力は不変
- doc/object-format.md は消さない
