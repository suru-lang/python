English | [日本語](README_jp.md)

# Python (a suru library)

A library for running Python from the Japanese programming language [suru](https://github.com/suru-lang/suru). It embeds libpython in the same process to run Python code and scripts, and to evaluate expressions and get their values back.

Work in progress (plan: [#1](https://github.com/suru-lang/python/issues/1)).

## Installation

In a [kumu](https://github.com/suru-lang/suru/blob/main/README.md#building-projects-kumu) project:

```sh
kumu 追加 github.com/suru-lang/python
```

You need Python 3.10 or later and its libpython (shared library). No Python headers are needed at build time. When first used, the library looks for libpython in this order:

1. If Python is already running in the process (like the `suru` interpreter), it uses that.
2. If the environment variable `SURU_LIBPYTHON` is set, it loads that libpython file.
3. It asks the Python in `SURU_PYTHON` (otherwise the Python of `VIRTUAL_ENV` inside a venv, otherwise `python3`, then `python`) where its libpython is. With a venv's Python, packages installed in that venv can be used too.

On Windows, use the MSYS2 UCRT64 Python (`pacman -S mingw-w64-ucrt-x86_64-python`).

suru 0.9.1 or later is required.

## Usage

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

| Form | Meaning |
|---|---|
| `コード を Python実行する` | Runs a sequence of statements. Names defined and modules imported stay available to later runs and evaluations |
| `式 を Python評価する` | Evaluates an expression and returns its value as a `JSON` of 「JSON」 (None, booleans, numbers, strings, lists, tuples, dicts) |
| `道 を Pythonスクリプト実行する` | Runs a Python file as `__main__` (same as `python3 道`) |
| `Python開始する` | Starts Python ahead of time (optional; it starts on first use) |

All of them are actions. On failure they raise a run-time error such as 「Python で『1 / 0』を評価するのに失敗しました(1行目: ZeroDivisionError: division by zero)」, which `試行する` can catch. They may be called from any suru thread (each call takes the GIL).

Objects that cannot become JSON (modules, functions, class instances, …) are held as `Python物` handles. Release them with `Python解放する` when done (otherwise they stay until the program ends).

```
統計 とは 「statistics」 を Python取込した 結果
平均 とは 統計 から 「mean」 を Python属性取得した 結果
r とは 平均 を （（（80 の JSON数） と （92 の JSON数）） の JSON配列 を 空 に 挿入する） で Python呼出した 結果
（r を Python値にした 結果） を 表示する
```

| Form | Meaning |
|---|---|
| `名前 を Python取込する` | The imported module (dotted names such as `「os.path」` too) |
| `式 を Python物評価する` | Evaluates an expression and returns the value as a `Python物` |
| `物 から 名前 を Python属性取得する` | `物.名前` |
| `物 を 引数群 で Python呼出する` | `物(*引数群)`. 引数群 is a list of `JSON`; the result is a `Python物` |
| `物 を 引数群 で 名前つき に Python名前つき呼出する` | `物(*引数群, **名前つき)`. 名前つき is a `JSON物` |
| `物 を Python値にする` | Converts the object to `JSON` (fails if it cannot) |
| `値 を Python物にする` | Makes a Python value from `JSON` |
| `物 を Python文字列にする` | `str(物)` |
| `名前 に 物 を Python代入する` | Gives the object a name usable from `Python実行する` / `Python評価する` code |
| `物 を Python解放する` | Releases the object |

Numbers, strings, byte strings and so on can also be passed directly without going through JSON text (faster for large byte strings and repeated calls).

```
f とは 「max」 を Python物評価した 結果
r とは f を （（3 を Python整数物にした 結果） と （2.5 を Python小数物にした 結果）） で Python物呼出した 結果
（r を Python整数値にした 結果） を 表示する
```

| Form | Meaning |
|---|---|
| `n を Python整数物にする`, `x を Python小数物にする`, `b を Python真偽物にする`, `s を Python文字列物にする`, `bs を Pythonバイト列物にする` | Make a Python object from a suru value |
| `物 を Python整数値にする` (`数`, up to ±2^62), `Python小数値にする` (`小数`), `Python真偽値にする` (`bool(物)`) | Turn an object into a suru value |
| `物 を Python文字列値にする` (str objects only), `Pythonバイト列値にする` (bytes, bytearray, …) | Likewise |
| `物 を Python長さ取得する` | `len(物)` |
| `物 から 鍵 を Python要素取得する` | `物[鍵]` (鍵 is a `Python物` too) |
| `物 で 鍵 に 値 を Python要素設定する` | `物[鍵] = 値` |
| `物 を Python一覧にする` | The items of `list(物)` as a list of `Python物` |
| `物 を 物群 で Python物呼出する` | `物(*物群)`, passing arguments as a list of `Python物` |
| `物 を 物群 で 名前つき に Python物名前つき呼出する` | `物(*物群, **名前つき)`; 名前つき is a dict `Python物` |

## License

MIT
