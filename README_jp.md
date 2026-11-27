[English](README.md) | 日本語

# Python(する のライブラリ)

日本語のプログラミング言語「[する](https://github.com/suru-lang/suru)」から Python を動かすライブラリです。libpython を同じプロセスに埋め込み、Python のコードやスクリプトを実行したり、式を評価して値を受け取ったりします。

作っているところです(計画は [#1](https://github.com/suru-lang/python/issues/1))。

## 入れ方

[kumu](https://github.com/suru-lang/suru/blob/main/README_jp.md#プロジェクトを組むkumu) のプロジェクトで:

```sh
kumu 追加 github.com/suru-lang/python
```

Python 3.8 以上と、その libpython(共有ライブラリ)が要ります。作るときに Python のヘッダーは要りません。使い始めるときに、次の順で libpython を探します。

1. インタプリタの `suru` のように、プロセスでもう Python が動いていれば、それを使います。
2. 環境変数 `SURU_LIBPYTHON` があれば、その libpython のファイルを使います。
3. 環境変数 `SURU_PYTHON` の Python(なければ `python3`、`python`)に、自分の libpython の場所を聞きます。

する 本体は 0.9.1 以上が要ります。

## 使い方

```
「JSON」 を 読み込んで 使う
「Python」 を 読み込んで 使う

主処理する は 数 の 入出力動作
主処理する とは 以下の手順です
  「import math」 を Python実行する
  「def 円の面積(r):」 + 改行 + 「    return math.pi * r * r」 を Python実行する
  面積 とは 「round(円の面積(2), 3)」 を Python評価した 結果
  「半径 2 の円の面積は 」 + （面積 を JSON文字列にする） + 「 です」 を 表示する
  「集計.py」 を Pythonスクリプト実行する
  0 を 結果にする
  以上です

主処理する
```

| 書き方 | 意味 |
|---|---|
| `コード を Python実行する` | 文の並びを実行する。定義した名前や import したものは、後の実行・評価で使える |
| `式 を Python評価する` | 式を評価して、値を「JSON」の `JSON` にする(None・真偽・数・文字列・リスト・タプル・辞書) |
| `道 を Pythonスクリプト実行する` | Python のファイルを `__main__` として実行する(`python3 道` と同じ) |
| `Python開始する` | 前もって Python を始める(呼ばなくても、最初に使うときに始まります) |

どれも動作です。失敗すると「Python で『1 / 0』を評価するのに失敗しました(1行目: ZeroDivisionError: division by zero)」のような実行時エラーになり、`試行する` で受け止められます。する のどのスレッドから呼んでもかまいません(呼ぶたびに GIL を取ります)。

## ライセンス

MIT
