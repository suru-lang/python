// SPDX-License-Identifier: MIT
// Copyright (c) 2026 sugarsweetrobotics(LICENSE)
//
// 「Python」の C の包み。kumu の Cソース として一緒にコンパイルされます。
//
// libpython のファイル名には版が入る(libpython3.12.so など)ので、する の 外部の「…」 には固定で
// 書けません。そこで、実行時に libpython を dlopen し、使う関数を dlsym で取ります。作るときに
// Python のヘッダーは要らず、3.8 以上のどの Python でも動きます。
//
// 包みが受け持つこと:
//   - 最初に使うときに一度だけ Python を始める(すでに動いている Python があればそれを使う)
//   - 呼ぶたびに GIL を取る(する のスレッドから呼んでも安全)
//   - Python の例外を「3行目: ZeroDivisionError: division by zero」のような文にする
//
// 結果の文字列(評価した値の JSON・エラーの文)は、呼んだスレッドごとの入れ物に置きます。
// 次に同じスレッドで包みを呼ぶまで使えます(する の側はすぐに写します)。

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#endif

typedef void PyObject;
typedef int PyGILState_STATE;

// Python の C API の、包みが使う関数(dlsym で取る)
static int (*p_Py_IsInitialized)(void);
static void (*p_Py_InitializeEx)(int);
static void *(*p_PyEval_SaveThread)(void);
static PyGILState_STATE (*p_PyGILState_Ensure)(void);
static void (*p_PyGILState_Release)(PyGILState_STATE);
static PyObject *(*p_PyRun_StringFlags)(const char *, int, PyObject *, PyObject *, void *);
static PyObject *(*p_PyImport_AddModule)(const char *);
static PyObject *(*p_PyModule_GetDict)(PyObject *);
static PyObject *(*p_PyDict_New)(void);
static PyObject *(*p_PyDict_GetItemString)(PyObject *, const char *);
static int (*p_PyDict_SetItemString)(PyObject *, const char *, PyObject *);
static PyObject *(*p_PyErr_Occurred)(void);
static void (*p_PyErr_Fetch)(PyObject **, PyObject **, PyObject **);
static void (*p_PyErr_NormalizeException)(PyObject **, PyObject **, PyObject **);
static void (*p_PyErr_Clear)(void);
static PyObject *(*p_PyObject_CallFunctionObjArgs)(PyObject *, ...);
static PyObject *(*p_PyUnicode_FromString)(const char *);
static const char *(*p_PyUnicode_AsUTF8)(PyObject *);
static void (*p_Py_IncRef)(PyObject *);
static void (*p_Py_DecRef)(PyObject *);

enum { PY_FILE_INPUT = 257, PY_EVAL_INPUT = 258 };

// Python で書いた手伝いの関数(始めたときに一度だけ定義する)
static const char *helper_source =
    "import json as _json, sys as _sys, traceback as _tb, os as _os\n"
    "# する の文字は UTF-8 なので、ロケールが C のままでも標準入出力は UTF-8 にする\n"
    "for _f in (_sys.stdin, _sys.stdout, _sys.stderr):\n"
    "    try:\n"
    "        if _f is not None and _f.encoding.lower().replace('-', '') != 'utf8':\n"
    "            _f.reconfigure(encoding='utf-8')\n"
    "    except Exception:\n"
    "        pass\n"
    "def flush():\n"
    "    for f in (_sys.stdout, _sys.stderr):\n"
    "        try:\n"
    "            f.flush()\n"
    "        except Exception:\n"
    "            pass\n"
    "def to_json(v):\n"
    "    return _json.dumps(v, ensure_ascii=False, allow_nan=False)\n"
    "def run_file(path):\n"
    "    import runpy\n"
    "    old = list(_sys.argv)\n"
    "    _sys.argv[:] = [path]\n"
    "    try:\n"
    "        runpy.run_path(path, run_name='__main__')\n"
    "    finally:\n"
    "        _sys.argv[:] = old\n"
    "import sysconfig as _sc\n"
    "_lib = tuple(p for p in {_sc.get_paths().get('stdlib'), _sc.get_paths().get('purelib'), _sc.get_paths().get('platlib')} if p)\n"
    "def _is_user(f):\n"
    "    return bool(f) and not f.startswith('<suru-python>') and not f.startswith('<frozen') and not f.startswith(_lib)\n"
    "def format_error(t, v, tb):\n"
    "    if t is None:\n"
    "        return '原因の分からない失敗です'\n"
    "    if isinstance(v, SystemExit):\n"
    "        return 'SystemExit: ' + str(v.code)\n"
    "    s = ''.join(_tb.format_exception_only(t, v)).strip().splitlines()\n"
    "    msg = s[-1] if s else t.__name__\n"
    "    where = None\n"
    "    if isinstance(v, SyntaxError) and v.lineno:\n"
    "        where = (v.filename, v.lineno)\n"
    "    else:\n"
    "        for fr in _tb.extract_tb(tb):\n"
    "            if _is_user(fr.filename):\n"
    "                where = (fr.filename, fr.lineno)\n"
    "    if where and where[1]:\n"
    "        f, n = where\n"
    "        if f in ('<string>', None):\n"
    "            msg = str(n) + '行目: ' + msg\n"
    "        else:\n"
    "            msg = '「' + str(f) + '」の' + str(n) + '行目: ' + msg\n"
    "    return msg\n";

static PyObject *main_dict;    // __suru__ の名前(実行・評価はここで)
static PyObject *fn_to_json;   // 手伝いの関数
static PyObject *fn_run_file;
static PyObject *fn_format_error;
static PyObject *fn_flush;
static char init_error[1024];  // 始めるのに失敗したときの文(空なら成功)

#ifdef _WIN32
static __declspec(thread) char *last_text;
#else
static __thread char *last_text;
#endif

static void set_text(const char *s) {
  free(last_text);
  last_text = strdup(s ? s : "");
}

// ---- libpython を探す ----

static void *lib;   // dlopen したもの(RTLD_DEFAULT は NULL なので、見つけたかは have_lib で見る)
static int have_lib;

static void *sym(const char *name) {
#ifdef _WIN32
  return (void *)GetProcAddress((HMODULE)lib, name);
#else
  return dlsym(lib, name);
#endif
}

static int open_lib(const char *path) {
  if (!path || !*path) return 0;
#ifdef _WIN32
  lib = (void *)LoadLibraryA(path);
#else
  lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
#endif
  have_lib = lib != NULL;
  return have_lib;
}

// python3 に、自分の libpython の場所と prefix を聞く(1行目: libpython の候補を「;」区切り、2行目: PYTHONHOME)
static const char *ask_script =
    "import sys, sysconfig, os\n"
    "v = sysconfig.get_config_var\n"
    "c = []\n"
    "fw = v('PYTHONFRAMEWORKPREFIX')\n"
    "if fw and v('LDLIBRARY'): c.append(os.path.join(fw, v('LDLIBRARY')))\n"
    "for d in (v('LIBDIR'), v('LIBPL'), os.path.join(sys.base_prefix, 'lib'), sys.base_prefix):\n"
    "    for n in (v('LDLIBRARY'), v('INSTSONAME'), 'libpython%d.%d.so' % sys.version_info[:2],\n"
    "              'libpython%d.%d.dylib' % sys.version_info[:2], 'libpython%d.%d.dll' % sys.version_info[:2],\n"
    "              'python%d%d.dll' % sys.version_info[:2]):\n"
    "        if d and n: c.append(os.path.join(d, n))\n"
    "print(';'.join(p for p in c if os.path.isfile(p)))\n"
    "print(sys.base_prefix + os.pathsep + sys.base_exec_prefix)\n";

static int ask_python(const char *exe, char *home, size_t home_size) {
  char cmd[4096];
  char script_path[512];
#ifdef _WIN32
  char tmp[MAX_PATH];
  GetTempPathA(sizeof tmp, tmp);
  snprintf(script_path, sizeof script_path, "%ssuru_python_ask_%lu.py", tmp, (unsigned long)GetCurrentProcessId());
#else
  const char *tmp = getenv("TMPDIR");
  snprintf(script_path, sizeof script_path, "%s/suru_python_ask_%ld.py", tmp && *tmp ? tmp : "/tmp", (long)getpid());
#endif
  FILE *f = fopen(script_path, "w");
  if (!f) return 0;
  fputs(ask_script, f);
  fclose(f);
  snprintf(cmd, sizeof cmd, "\"%s\" \"%s\"", exe, script_path);
#ifdef _WIN32
  FILE *p = _popen(cmd, "r");
#else
  FILE *p = popen(cmd, "r");
#endif
  int ok = 0;
  if (p) {
    static char line1[8192];
    static char line2[4096];
    if (fgets(line1, sizeof line1, p) && fgets(line2, sizeof line2, p)) {
      line1[strcspn(line1, "\r\n")] = 0;
      line2[strcspn(line2, "\r\n")] = 0;
      for (char *s = line1; s && *s && !ok;) {
        char *semi = strchr(s, ';');
        if (semi) *semi = 0;
        ok = open_lib(s);
        s = semi ? semi + 1 : NULL;
      }
      if (ok) snprintf(home, home_size, "%s", line2);
    }
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
  }
  remove(script_path);
  return ok;
}

// 使う関数を全部取る。足りなければ 0
static int load_symbols(void) {
#define GET(n)                                                                          \
  do {                                                                                  \
    *(void **)(&p_##n) = sym(#n);                                                       \
    if (!p_##n) {                                                                       \
      snprintf(init_error, sizeof init_error, "libpython に %s がありません", #n);      \
      return 0;                                                                         \
    }                                                                                   \
  } while (0)
  GET(Py_IsInitialized);
  GET(Py_InitializeEx);
  GET(PyEval_SaveThread);
  GET(PyGILState_Ensure);
  GET(PyGILState_Release);
  GET(PyRun_StringFlags);
  GET(PyImport_AddModule);
  GET(PyModule_GetDict);
  GET(PyDict_New);
  GET(PyDict_GetItemString);
  GET(PyDict_SetItemString);
  GET(PyErr_Occurred);
  GET(PyErr_Fetch);
  GET(PyErr_NormalizeException);
  GET(PyErr_Clear);
  GET(PyObject_CallFunctionObjArgs);
  GET(PyUnicode_FromString);
  GET(PyUnicode_AsUTF8);
  GET(Py_IncRef);
  GET(Py_DecRef);
#undef GET
  return 1;
}

// 今の例外を文にして last_text に置き、例外を消す
static void take_error(void) {
  PyObject *t = NULL, *v = NULL, *tb = NULL;
  p_PyErr_Fetch(&t, &v, &tb);
  p_PyErr_NormalizeException(&t, &v, &tb);
  PyObject *s = NULL;
  if (fn_format_error && t) {
    // NULL は引数の終わりの印なので、無いものは型だけで呼ぶ(v と tb は正規化でふつうはそろう)
    s = p_PyObject_CallFunctionObjArgs(fn_format_error, t, v ? v : t, tb ? tb : t, NULL);
  }
  const char *u = s ? p_PyUnicode_AsUTF8(s) : NULL;
  set_text(u ? u : "Python の例外の文を作れませんでした");
  if (s) p_Py_DecRef(s);
  if (p_PyErr_Occurred()) p_PyErr_Clear();
  if (t) p_Py_DecRef(t);
  if (v) p_Py_DecRef(v);
  if (tb) p_Py_DecRef(tb);
}

// Python の print が する の表示と前後しないように、呼ぶたびに出力を書き出す
static void flush_output(void) {
  if (!fn_flush) return;
  PyObject *r = p_PyObject_CallFunctionObjArgs(fn_flush, NULL);
  if (r) p_Py_DecRef(r); else p_PyErr_Clear();
}

static void start(void) {
  init_error[0] = 0;
  char home[4096] = "";
  int we_set_home = 0;
  const char *env_lib = getenv("SURU_LIBPYTHON");
#ifndef _WIN32
  // すでにプロセスに Python がいる(インタプリタの suru、または Python から読み込まれた)ならそれを使う
  {
    int (*is_init)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "Py_IsInitialized");
    if (is_init && is_init()) {
      lib = RTLD_DEFAULT;
      have_lib = 1;
    }
  }
#endif
  if (!have_lib && env_lib && *env_lib) {
    if (!open_lib(env_lib)) {
      snprintf(init_error, sizeof init_error, "SURU_LIBPYTHON の「%s」を開けません", env_lib);
      return;
    }
  }
  if (!have_lib) {
    const char *exe = getenv("SURU_PYTHON");
    if (exe && *exe) {
      if (!ask_python(exe, home, sizeof home)) {
        snprintf(init_error, sizeof init_error, "SURU_PYTHON の「%s」から libpython を見つけられません", exe);
        return;
      }
    } else if (!ask_python("python3", home, sizeof home) && !ask_python("python", home, sizeof home)) {
      snprintf(init_error, sizeof init_error,
               "libpython を見つけられません(python3 を入れるか、SURU_LIBPYTHON に libpython のファイルの道を書いてください)");
      return;
    }
  }
  if (!load_symbols()) return;

  if (!p_Py_IsInitialized()) {
    // 使う側の実行ファイルの場所からは標準ライブラリを探せないので、聞いてきた prefix を教える
    if (home[0] && !getenv("PYTHONHOME")) {
#ifdef _WIN32
      _putenv_s("PYTHONHOME", home);
#else
      setenv("PYTHONHOME", home, 1);
#endif
      we_set_home = 1;
    }
    // する の文字は UTF-8 なので、ロケールが C のままでも UTF-8 モードにする(PEP 540。ファイルの道の文字のため)
    int *utf8_mode = (int *)sym("Py_UTF8Mode");
    if (utf8_mode && !getenv("PYTHONUTF8")) *utf8_mode = 1;
    p_Py_InitializeEx(0);  // 0: シグナルの処理を上書きしない
    if (we_set_home) {
#ifdef _WIN32
      _putenv_s("PYTHONHOME", "");
#else
      unsetenv("PYTHONHOME");
#endif
    }
    if (!p_Py_IsInitialized()) {
      snprintf(init_error, sizeof init_error, "Python を始められませんでした");
      return;
    }
    p_PyEval_SaveThread();  // GIL を放す(これからは呼ぶたびに取る)
  }

  PyGILState_STATE g = p_PyGILState_Ensure();
  // 実行・評価の名前は、モジュール __suru__ に置く(インタプリタの suru では __main__ が suru.py 自身なので、混ぜない)
  main_dict = p_PyModule_GetDict(p_PyImport_AddModule("__suru__"));
  PyObject *hd = p_PyDict_New();
  // 手伝いの関数は「<suru-python>」の名前でコンパイルし、例外の位置から除けるようにする
  PyObject *src = p_PyUnicode_FromString(helper_source);
  PyObject *r = NULL;
  if (src && p_PyDict_SetItemString(hd, "SRC", src) == 0)
    r = p_PyRun_StringFlags("exec(compile(SRC, '<suru-python>', 'exec'))\n", PY_FILE_INPUT, hd, hd, NULL);
  if (src) p_Py_DecRef(src);
  if (!r) {
    p_PyErr_Clear();
    snprintf(init_error, sizeof init_error, "Python の手伝いの関数を定義できませんでした");
  } else {
    p_Py_DecRef(r);
    fn_to_json = p_PyDict_GetItemString(hd, "to_json");
    fn_run_file = p_PyDict_GetItemString(hd, "run_file");
    fn_format_error = p_PyDict_GetItemString(hd, "format_error");
    fn_flush = p_PyDict_GetItemString(hd, "flush");
    // 始めるためだけに足した環境変数は、Python から動かす子のプロセスに渡さない
    if (we_set_home) {
      PyObject *r2 = p_PyRun_StringFlags("_os.environ.pop('PYTHONHOME', None)\n", PY_FILE_INPUT, hd, hd, NULL);
      if (r2) p_Py_DecRef(r2); else p_PyErr_Clear();
    }
  }
  p_PyGILState_Release(g);
}

#ifdef _WIN32
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK start_once(PINIT_ONCE o, PVOID p, PVOID *c) {
  (void)o; (void)p; (void)c;
  start();
  return TRUE;
}
static void ensure_started(void) { InitOnceExecuteOnce(&once, start_once, NULL, NULL); }
#else
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void ensure_started(void) { pthread_once(&once, start); }
#endif

// ---- する から呼ぶ関数 ----
// どれも 0 で成功、-1 で失敗(文は supy_text で取る)

// 始める。失敗したら -1
int64_t supy_start(void) {
  ensure_started();
  if (init_error[0]) {
    set_text(init_error);
    return -1;
  }
  return 0;
}

// 最後の結果の文字列(評価した値の JSON か、エラーの文)
const char *supy_text(void) { return last_text ? last_text : ""; }

// コードを __suru__ で実行する(文の並び)
int64_t supy_exec(const char *code) {
  if (supy_start() < 0) return -1;
  PyGILState_STATE g = p_PyGILState_Ensure();
  PyObject *r = p_PyRun_StringFlags(code, PY_FILE_INPUT, main_dict, main_dict, NULL);
  int64_t rc = 0;
  if (r) {
    p_Py_DecRef(r);
    set_text("");
  } else {
    take_error();
    rc = -1;
  }
  flush_output();
  p_PyGILState_Release(g);
  return rc;
}

// 式を __suru__ で評価し、値を JSON の文字列にして置く
int64_t supy_eval_json(const char *code) {
  if (supy_start() < 0) return -1;
  PyGILState_STATE g = p_PyGILState_Ensure();
  int64_t rc = -1;
  PyObject *v = p_PyRun_StringFlags(code, PY_EVAL_INPUT, main_dict, main_dict, NULL);
  if (v) {
    PyObject *s = p_PyObject_CallFunctionObjArgs(fn_to_json, v, NULL);
    p_Py_DecRef(v);
    if (s) {
      const char *u = p_PyUnicode_AsUTF8(s);
      if (u) {
        set_text(u);
        rc = 0;
      }
      p_Py_DecRef(s);
    }
  }
  if (rc < 0) take_error();
  flush_output();
  p_PyGILState_Release(g);
  return rc;
}

// スクリプトのファイルを __main__ として実行する(runpy.run_path)
int64_t supy_run_file(const char *path) {
  if (supy_start() < 0) return -1;
  PyGILState_STATE g = p_PyGILState_Ensure();
  int64_t rc = -1;
  PyObject *p = p_PyUnicode_FromString(path);
  if (p) {
    PyObject *r = p_PyObject_CallFunctionObjArgs(fn_run_file, p, NULL);
    p_Py_DecRef(p);
    if (r) {
      p_Py_DecRef(r);
      set_text("");
      rc = 0;
    }
  }
  if (rc < 0) take_error();
  flush_output();
  p_PyGILState_Release(g);
  return rc;
}
