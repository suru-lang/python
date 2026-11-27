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
3. It asks the Python in `SURU_PYTHON` (otherwise `python3`, then `python`) where its libpython is.

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

## License

MIT
