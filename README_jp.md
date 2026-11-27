[English](README.md) | 日本語

# Python(する のライブラリ)

日本語のプログラミング言語「[する](https://github.com/suru-lang/suru)」から Python を動かすライブラリです。libpython を同じプロセスに埋め込み、Python のコードやスクリプトを実行したり、式を評価して値を受け取ったりします。

作っているところです(計画は [#1](https://github.com/suru-lang/python/issues/1))。

## 入れ方

[kumu](https://github.com/suru-lang/suru/blob/main/README_jp.md#プロジェクトを組むkumu) のプロジェクトで:

```sh
kumu 追加 github.com/suru-lang/python
```

Python 3.10 以上と、その libpython(共有ライブラリ)が要ります。作るときに Python のヘッダーは要りません。使い始めるときに、次の順で libpython を探します。

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

JSON にできない物(モジュール・関数・クラスの物など)は、手札の `Python物` で持ちます。使い終わったら `Python解放する` で放します(放さなければ、プログラムが終わるまで残ります)。

```
統計 とは 「statistics」 を Python取込した 結果
平均 とは 統計 から 「mean」 を Python属性取得した 結果
r とは 平均 を （（（80 の JSON数） と （92 の JSON数）） の JSON配列 を 空 に 挿入する） で Python呼出した 結果
（r を Python値にした 結果） を 表示する
```

| 書き方 | 意味 |
|---|---|
| `名前 を Python取込する` | import したモジュール(`「os.path」` のような点の入った名前も) |
| `式 を Python物評価する` | 式を評価して、値を `Python物` のまま返す |
| `物 から 名前 を Python属性取得する` | `物.名前` |
| `物 を 引数群 で Python呼出する` | `物(*引数群)`。引数群は `JSON` のリスト、結果は `Python物` |
| `物 を 引数群 で 名前つき に Python名前つき呼出する` | `物(*引数群, **名前つき)`。名前つきは `JSON物` |
| `物 を Python値にする` | 物を `JSON` にする(できなければ失敗) |
| `値 を Python物にする` | `JSON` から Python の値を作る |
| `物 を Python文字列にする` | `str(物)` |
| `名前 に 物 を Python代入する` | 物に名前を付け、`Python実行する`・`Python評価する` のコードから使えるようにする |
| `物 を Python解放する` | 物を放す |

数・文字列・バイト列などは、JSON の文字を通さずに直接やり取りすることもできます(大きなバイト列や、何度も呼ぶときに速い)。

```
f とは 「max」 を Python物評価した 結果
r とは f を （（3 を Python整数物にした 結果） と （2.5 を Python小数物にした 結果）） で Python物呼出した 結果
（r を Python整数値にした 結果） を 表示する
```

| 書き方 | 意味 |
|---|---|
| `n を Python整数物にする`・`x を Python小数物にする`・`b を Python真偽物にする`・`s を Python文字列物にする`・`bs を Pythonバイト列物にする` | する の値から Python の物を作る |
| `物 を Python整数値にする`(`数`。±2^62 まで)・`Python小数値にする`(`小数`)・`Python真偽値にする`(`bool(物)`) | 物を する の値にする |
| `物 を Python文字列値にする`(str の物だけ)・`Pythonバイト列値にする`(bytes・bytearray など) | 同じく |
| `物 を Python長さ取得する` | `len(物)` |
| `物 から 鍵 を Python要素取得する` | `物[鍵]`(鍵も `Python物`) |
| `物 で 鍵 に 値 を Python要素設定する` | `物[鍵] = 値` |
| `物 を Python一覧にする` | `list(物)` の要素を `Python物` のリストにする |
| `物 を 物群 で Python物呼出する` | `物(*物群)`。引数を `Python物` のリストで渡す |
| `物 を 物群 で 名前つき に Python物名前つき呼出する` | `物(*物群, **名前つき)`。名前つきは辞書の `Python物` |

## ライセンス

MIT
