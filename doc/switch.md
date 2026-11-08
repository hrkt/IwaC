# switch の設計 (v43)

位置づけ: doc/missing-features.md の実装予定の
第一候補。Ikesu 側の不足リストの筆頭で、nami でも
「無いので if-else で書く」となった (2026-11-08)。
C どおりの switch を入れる。

## 文法

```
switch ( 式 ) { 本体 }
本体 = { case 定数式 : 文... } { default : 文... }
```

- 制御式は整数型の式。1 回だけ評価する
- case のラベルは整数の定数式 (parse の eval_const
  で評価する。負数も受ける)
- default は最大 1 つ、置く位置はどこでもよい
  (C どおり)
- case / default は switch の本体ブロックの中で
  だけ現れる
- 本体は { } 必須に割り切る (C は文 1 つでもよい)

## 意味 (C どおり)

- 一致した case へ飛ぶ。なければ default、どちらも
  なければ何もしない
- 飛んだ先から下を順に実行する。fallthrough あり
  (暗黙の break はしない)
- break は最も内側のループまたは switch を抜ける
- continue は switch の中でも外側のループに効く
  (switch は continue の対象にならない)

## AST

- NODE_SWITCH = 57: lhs = 制御式、
  rhs = case 群の連鎖
- NODE_CASE = 58: val = case の定数値 (default は
  特別値)、lhs = その case の文の連鎖、
  next = 次の case 群
- case 定数は parse 時に eval_const で確定する。
  重複した case、重複した default は parse エラー

## parse の置き場

- 文の解析は src/02b-decl.c の parse_body。
  switch 文の追加と、本体ブロックの中で
  case / default ラベルを受ける処理が要る
- ラベルは「文の前置き」として読む: case 定数 :
  または default : を読んだら NODE_CASE を
  切り替え、次のラベルか } まで、文をその
  case の連鎖に集める
- キーワード switch / case / default の追加は
  既存の流儀 (文字列比較) で

## gen の置き場

- 制御式を評価して t1 に置き、各 case と比べる
  (定数を t2 に li、beq で case 群の先頭へ)。
  全部外れたら default 群 (なければ末尾) へ跳ぶ。
  その後に case 群を順番に並べるので、fallthrough
  は自然に起きる
- 比較は直線並び (ケース数に比例する)。ジャンプ
  テーブルは最適化の話として将来に回す
  (doc/optimization.md)
- 制御式と case 定数は、64 ビット正規形同士で
  比べる (幅つき型は v35 の正規形の流儀)
- break の解決が gen 側の要点: 現状は loop_depth で
  loop_break_head[32] / loop_cont_head[32] を
  管理している。switch 用に sw_break_head[32] を
  足し、「最も内側の制御構造がループか switch か」
  の種別スタックを 1 つ持つ。break は種別スタックの
  先頭を見て正しいパッチリストへ出し、continue は
  今までどおりループ側だけを見る

## 割り切り

- 本体は { } 必須
- case ラベルは定数式まで (変数を書いたらエラー)
- case は本体ブロックの直下だけ。内側のブロックの
  中にある case (Duff's device のような形) は
  対象外
- 本体の中の変数宣言は、iwac の局所変数が
  関数範囲なので、飛び越し初期化のような C の
  細部は自然に消える (宣言は parse 時に登録され、
  初期化の代入文だけが実行順に従う)
- iwac 自身のソースでは v43 の switch を使わない
  (新機能は同版の自己ソースで使わない、従来の
  割り切りどおり)

## 検証の物差し (test.sh の v43 節)

- 基本の振り分け、fallthrough、default
  (あり・なし・真ん中に置く)、break で抜ける
- ループの中の switch で continue がループに
  効くこと、switch の中のループの break が
  ループを抜けること (内側優先の確認)
- switch の入れ子
- エラー系: case の重複、default の重複、
  switch の外の case、定数でない case ラベル
- 振る舞いの一致は check_tail で確認する。
  自己ホストの固定点は従来の節が担保する
