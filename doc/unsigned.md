# 古典スペルの unsigned の修正 (v54)

状態: 実装済み (2026-11-23、branch `v54`)。
不具合の修正で、最適化トラックの段3 (式評価、
doc/expr-eval.md) は v55 にずれた。

## 不具合

`unsigned long` の変数の比較が符号つきに
なっていた。C の規格では符号なし同士の比較・
除算・剰余・右シフトは符号なしでなければ
ならないので、規格と比べてまずい状態だった。

## 仕組み (なぜそうなっていたか)

iwac の古典スペル (char / short / int / long と
unsigned / signed の組み合わせ) は、v16 以来
「型の言葉は読み捨てる」モデルで、char だけ
1バイト、それ以外は全部 8バイトの符号つき
整数に潰していた。幅と符号を本当に持つのは
v35 の幅つきの名前 (int32 / uint64 等) だけ
だった。そのため:

- `unsigned long` と `long` と `int` が全部
  同じ型タグ (8) になり、生成側の符号なし
  判定 (型タグ == 8+16+32、または & 48) に
  決して当たらなかった
- 比較だけでなく、除算 (div / divu)、剰余、
  右シフト (sra / srl) も符号つきだった。
  double への変換も符号つき扱いだった
- 気づかなかった理由: iwac 自身と nami は
  符号なしが要る所で uint64 等の幅つきの名前を
  使っており、正の範囲に収まる値では符号つき
  でも答えが一致する

## 直し方

parse_type が古典スペルの並びの中の unsigned を
拾い、型タグに符号なしの印 (16) と幅確定の印
(32) を立てる。大きさは読み捨てモデルのまま
変えない (char = 1バイト、それ以外 = 8バイト):

- `unsigned long` / `unsigned int` /
  `unsigned short` / `unsigned` /
  `unsigned long long` → 型タグ 8+16+32
  (= uint64 と同一)。比較・除算・剰余・右
  シフト・double 変換が符号なしになる
- `unsigned char` → 1+16+32。代入時の正規化が
  幅つき型と同じくゼロ拡張になる
- typedef 経由 (`typedef unsigned long usz;`)
  も parse_type を通るので自動で効く

## 変えていないこと (割り切り)

- 大きさ: int / long は 8バイトのまま、
  short も 8バイトのまま (幅が要る所は
  幅つきの名前を使う、という従来モデル)
- char の符号: plain char は符号なしのまま
  (既存の決定)。signed char も区別しない
- 符号つきと符号なしの混在演算は「どちらかが
  幅確定の符号なしなら符号なし」という既存の
  伝播規則のまま (型タグの & 32 で決まる)

## 検証

- test.sh に v54 の検査2件: unsigned long の
  比較・除算・右シフト・剰余と、typedef /
  unsigned char / unsigned int の組み合わせ
- 既存の検査で unsigned を使うもの (v16 の
  unsigned char 配列、v25 のキャスト、v36 の
  typedef unsigned long) は出力が変わらない
  ことを個別に確かめた
