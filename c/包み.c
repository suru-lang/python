// SPDX-License-Identifier: MIT
// Copyright (c) 2026 sugarsweetrobotics(LICENSE)
//
// 「Python」の C の包み。kumu の Cソース として一緒にコンパイルされます。
//
// libpython のファイル名には版が入る(libpython3.12.so など)ので、する の 外部の「…」 には固定で
// 書けません。そこで、実行時に libpython を dlopen し、使う関数を dlsym で取ります。作るときに
// Python のヘッダーは要らず、3.10 以上の Python で動きます(CI で 3.10 と 3.13 を確かめています)。
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
static PyObject *(*p_PyImport_ImportModule)(const char *);
static PyObject *(*p_PyObject_GetAttrString)(PyObject *, const char *);
static PyObject *(*p_PyObject_Str)(PyObject *);

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
    "def from_json(s):\n"
    "    return _json.loads(s)\n"
    "def call(f, args, kwargs):\n"
    "    a = _json.loads(args) if args else []\n"
    "    k = _json.loads(kwargs) if kwargs else {}\n"
    "    if not isinstance(a, list):\n"
    "        raise TypeError('位置の引数は JSON の配列で渡してください')\n"
    "    if not isinstance(k, dict):\n"
    "        raise TypeError('名前つきの引数は JSON の物で渡してください')\n"
    "    return f(*a, **k)\n"
    "def to_bytes(v):\n"
    "    if isinstance(v, (str, int)):\n"
    "        raise TypeError('バイト列にできるのは bytes・bytearray・memoryview などです(' + type(v).__name__ + ' は できません)')\n"
    "    return bytes(v)\n"
    "def adopt(info):\n"
    "    i = _json.loads(info)\n"
    "    _sys.path[:] = i['path']\n"
    "    _sys.prefix = i['prefix']\n"
    "    _sys.exec_prefix = i['exec_prefix']\n"
    "    _sys.executable = i['executable']\n"
    "def bind(name, v):\n"
    "    import sys\n"
    "    setattr(sys.modules['__suru__'], name, v)\n"
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
    "    # C の側は、無いものの代わりに型を渡してくる\n"
    "    if not isinstance(v, BaseException):\n"
    "        v = None\n"
    "    if not hasattr(tb, 'tb_frame'):\n"
    "        tb = None\n"
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
static PyObject *fn_from_json;
static PyObject *fn_call;
static PyObject *fn_bind;
static PyObject *fn_to_bytes;
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

#ifdef _WIN32
// UTF-8 の文字列を Windows の幅の広い文字列にする(日本語の道のため)
static wchar_t *widen(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = n > 0 ? (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n) : NULL;
  if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}
#endif

static int open_lib(const char *path) {
  if (!path || !*path) return 0;
#ifdef _WIN32
  wchar_t *w = widen(path);
  // 同じフォルダの DLL(vcruntime など)も探せるように、道の付いた読み方にする
  lib = w ? (void *)LoadLibraryExW(w, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) : NULL;
  if (!lib && w) lib = (void *)LoadLibraryW(w);
  free(w);
#else
  lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
#endif
  have_lib = lib != NULL;
  return have_lib;
}

// python に、自分の libpython の場所などを聞く
//   1行目: libpython の候補を「;」区切り(Windows の道に ; は使えない)
//   2行目: PYTHONHOME(base_prefix と base_exec_prefix)
//   3行目: その Python の sys.path などの JSON(venv の site-packages を使うため)
static const char *ask_script =
    "import sys, sysconfig, os, json\n"
    "v = sysconfig.get_config_var\n"
    "c = []\n"
    "fw = v('PYTHONFRAMEWORKPREFIX')\n"
    "if fw and v('LDLIBRARY'): c.append(os.path.join(fw, v('LDLIBRARY')))\n"
    "ver = sys.version_info[:2]\n"
    "for d in (v('LIBDIR'), v('LIBPL'), os.path.join(sys.base_prefix, 'lib'), os.path.join(sys.base_prefix, 'bin'),\n"
    "          sys.base_prefix, os.path.dirname(getattr(sys, '_base_executable', '') or sys.executable)):\n"
    "    for n in (v('LDLIBRARY'), v('INSTSONAME'), 'libpython%d.%d.so' % ver, 'libpython%d.%d.dylib' % ver,\n"
    "              'libpython%d.%d.dll' % ver, 'python%d%d.dll' % ver):\n"
    "        if d and n: c.append(os.path.join(d, n))\n"
    "print(';'.join(p for p in c if os.path.isfile(p)))\n"
    "print(sys.base_prefix + os.pathsep + sys.base_exec_prefix)\n"
    "print(json.dumps({'path': sys.path[1:], 'prefix': sys.prefix, 'exec_prefix': sys.exec_prefix,\n"
    "                  'executable': sys.executable}, ensure_ascii=True))\n";

static char asked_info[65536];  // 3行目(聞いた Python の sys.path など)。空なら聞いていない

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
  // -X utf8: 出力を UTF-8 にする(日本語の道のため)
#ifdef _WIN32
  // cmd.exe は前後の " を1組外すので、全体をもう1組で囲む
  snprintf(cmd, sizeof cmd, "\"\"%s\" -X utf8 \"%s\"\"", exe, script_path);
  FILE *p = _popen(cmd, "r");
#else
  snprintf(cmd, sizeof cmd, "\"%s\" -X utf8 \"%s\" 2>/dev/null", exe, script_path);
  FILE *p = popen(cmd, "r");
#endif
  int ok = 0;
  if (p) {
    static char line1[16384];
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
      if (ok) {
        snprintf(home, home_size, "%s", line2);
        if (fgets(asked_info, sizeof asked_info, p)) asked_info[strcspn(asked_info, "\r\n")] = 0;
        else asked_info[0] = 0;
      }
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

// すでにプロセスにいる Python(インタプリタの suru など)を探す
static int find_running_python(void) {
#ifdef _WIN32
  static const char *names[] = {"python3%d.dll", "libpython3.%d.dll"};
  for (int k = 0; k < 2; k++) {
    for (int minor = 8; minor <= 30; minor++) {
      char name[64];
      snprintf(name, sizeof name, names[k], minor);
      HMODULE h = GetModuleHandleA(name);
      if (!h) continue;
      int (*is_init)(void) = (int (*)(void))GetProcAddress(h, "Py_IsInitialized");
      if (is_init && is_init()) {
        lib = (void *)h;
        have_lib = 1;
        return 1;
      }
    }
  }
  return 0;
#else
  int (*is_init)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "Py_IsInitialized");
  if (is_init && is_init()) {
    lib = RTLD_DEFAULT;
    have_lib = 1;
    return 1;
  }
  return 0;
#endif
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
  GET(PyImport_ImportModule);
  GET(PyObject_GetAttrString);
  GET(PyObject_Str);
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
  int we_started = 0;  // この包みが Python を始めた(すでに動いていたのではない)
  const char *env_lib = getenv("SURU_LIBPYTHON");
  // すでにプロセスに Python がいる(インタプリタの suru、または Python から読み込まれた)ならそれを使う
  find_running_python();
  if (!have_lib && env_lib && *env_lib) {
    if (!open_lib(env_lib)) {
      snprintf(init_error, sizeof init_error, "SURU_LIBPYTHON の「%s」を開けません", env_lib);
      return;
    }
  }
  if (!have_lib) {
    const char *exe = getenv("SURU_PYTHON");
    char venv_exe[4096];
    const char *venv = getenv("VIRTUAL_ENV");
    if ((!exe || !*exe) && venv && *venv) {
      // venv の中で動かしたら、その venv の Python を使う
#ifdef _WIN32
      snprintf(venv_exe, sizeof venv_exe, "%s\\Scripts\\python.exe", venv);
#else
      snprintf(venv_exe, sizeof venv_exe, "%s/bin/python", venv);
#endif
      exe = venv_exe;
    }
    if (exe && *exe) {
      if (!ask_python(exe, home, sizeof home)) {
        snprintf(init_error, sizeof init_error, "Python の「%s」から libpython を見つけられません", exe);
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
    we_started = 1;
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
    fn_from_json = p_PyDict_GetItemString(hd, "from_json");
    fn_call = p_PyDict_GetItemString(hd, "call");
    fn_bind = p_PyDict_GetItemString(hd, "bind");
    fn_to_bytes = p_PyDict_GetItemString(hd, "to_bytes");
    // 聞いた Python(venv など)の sys.path・prefix・executable に合わせる
    if (we_started && asked_info[0]) {
      PyObject *fn_adopt = p_PyDict_GetItemString(hd, "adopt");
      PyObject *info = p_PyUnicode_FromString(asked_info);
      PyObject *r3 = (fn_adopt && info) ? p_PyObject_CallFunctionObjArgs(fn_adopt, info, NULL) : NULL;
      if (info) p_Py_DecRef(info);
      if (r3) p_Py_DecRef(r3); else p_PyErr_Clear();
    }
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
// どれも 0 で成功、-1 で失敗(文は impl_text で取る)

// 始める。失敗したら -1
static int64_t impl_start(void) {
  ensure_started();
  if (init_error[0]) {
    set_text(init_error);
    return -1;
  }
  return 0;
}

// 最後の結果の文字列(評価した値の JSON か、エラーの文)

// コードを __suru__ で実行する(文の並び)
static int64_t impl_exec(const char *code) {
  if (impl_start() < 0) return -1;
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
static int64_t impl_eval_json(const char *code) {
  if (impl_start() < 0) return -1;
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
static int64_t impl_run_file(const char *path) {
  if (impl_start() < 0) return -1;
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

// ---- 手札(Python の物) ----
// 物を返す関数は、新しい参照の番地を返します(失敗は 0 で、文は impl_text)。
// 使い終わったら impl_release で放します。

// 物を返した後の始末: 失敗なら文を置く
static PyObject *done_obj(PyObject *o) {
  if (o) set_text("");
  else take_error();
  flush_output();
  return o;
}

// 文字列の物(新しい参照)を置いて放す
static int64_t done_text(PyObject *s) {
  int64_t rc = -1;
  if (s) {
    const char *u = p_PyUnicode_AsUTF8(s);
    if (u) {
      set_text(u);
      rc = 0;
    }
    p_Py_DecRef(s);
  }
  if (rc < 0) take_error();
  flush_output();
  return rc;
}

// モジュールを取り込む(import 名前。「os.path」のような点の入った名前も)
static void *impl_import(const char *name) {
  if (impl_start() < 0) return NULL;
  PyGILState_STATE g = p_PyGILState_Ensure();
  // PyImport_ImportModule は「os.path」なら一番内側のモジュールを返す(import 文と違う)
  PyObject *o = p_PyImport_ImportModule(name);
  o = done_obj(o);
  p_PyGILState_Release(g);
  return o;
}

// 式を評価して、物のまま返す
static void *impl_eval_obj(const char *code) {
  if (impl_start() < 0) return NULL;
  PyGILState_STATE g = p_PyGILState_Ensure();
  PyObject *o = done_obj(p_PyRun_StringFlags(code, PY_EVAL_INPUT, main_dict, main_dict, NULL));
  p_PyGILState_Release(g);
  return o;
}

// 属性を取る
static void *impl_getattr(void *obj, const char *name) {
  if (impl_start() < 0) return NULL;
  PyGILState_STATE g = p_PyGILState_Ensure();
  PyObject *o = done_obj(p_PyObject_GetAttrString(obj, name));
  p_PyGILState_Release(g);
  return o;
}

// 呼ぶ。args は JSON の配列、kwargs は JSON の物(空の文字列なら無し)
static void *impl_call(void *f, const char *args, const char *kwargs) {
  if (impl_start() < 0) return NULL;
  PyGILState_STATE g = p_PyGILState_Ensure();
  PyObject *a = p_PyUnicode_FromString(args);
  PyObject *k = p_PyUnicode_FromString(kwargs);
  PyObject *o = NULL;
  if (a && k) o = p_PyObject_CallFunctionObjArgs(fn_call, f, a, k, NULL);
  if (a) p_Py_DecRef(a);
  if (k) p_Py_DecRef(k);
  o = done_obj(o);
  p_PyGILState_Release(g);
  return o;
}

// JSON の文字列から Python の値を作る
static void *impl_from_json(const char *json) {
  if (impl_start() < 0) return NULL;
  PyGILState_STATE g = p_PyGILState_Ensure();
  PyObject *s = p_PyUnicode_FromString(json);
  PyObject *o = s ? p_PyObject_CallFunctionObjArgs(fn_from_json, s, NULL) : NULL;
  if (s) p_Py_DecRef(s);
  o = done_obj(o);
  p_PyGILState_Release(g);
  return o;
}

// 物を JSON の文字列にして置く
static int64_t impl_obj_json(void *obj) {
  if (impl_start() < 0) return -1;
  PyGILState_STATE g = p_PyGILState_Ensure();
  int64_t rc = done_text(p_PyObject_CallFunctionObjArgs(fn_to_json, obj, NULL));
  p_PyGILState_Release(g);
  return rc;
}

// 物を str() の文字列にして置く
static int64_t impl_obj_str(void *obj) {
  if (impl_start() < 0) return -1;
  PyGILState_STATE g = p_PyGILState_Ensure();
  int64_t rc = done_text(p_PyObject_Str(obj));
  p_PyGILState_Release(g);
  return rc;
}

// 物に名前を付けて、Python実行する・Python評価する のコードから使えるようにする
static int64_t impl_bind(const char *name, void *obj) {
  if (impl_start() < 0) return -1;
  PyGILState_STATE g = p_PyGILState_Ensure();
  int64_t rc = -1;
  PyObject *n = p_PyUnicode_FromString(name);
  if (n) {
    PyObject *r = p_PyObject_CallFunctionObjArgs(fn_bind, n, obj, NULL);
    p_Py_DecRef(n);
    if (r) {
      p_Py_DecRef(r);
      rc = 0;
    }
  }
  if (rc < 0) take_error();
  else set_text("");
  p_PyGILState_Release(g);
  return rc;
}

// 物を放す(参照を1つ減らす)
static int64_t impl_release(void *obj) {
  if (!obj || impl_start() < 0) return 0;
  PyGILState_STATE g = p_PyGILState_Ensure();
  p_Py_DecRef(obj);
  p_PyGILState_Release(g);
  return 0;
}

// ---- 速い受け渡し(JSON の文字を通さない) ----

#ifdef _WIN32
static __declspec(thread) int64_t last_int;
static __declspec(thread) double last_float;
static __declspec(thread) void *last_buf;
#else
static __thread int64_t last_int;
static __thread double last_float;
static __thread void *last_buf;
#endif

static PyObject *(*p_PyLong_FromLongLong)(long long);
static long long (*p_PyLong_AsLongLong)(PyObject *);
static PyObject *(*p_PyFloat_FromDouble)(double);
static double (*p_PyFloat_AsDouble)(PyObject *);
static PyObject *(*p_PyBool_FromLong)(long);
static int (*p_PyObject_IsTrue)(PyObject *);
static PyObject *(*p_PyBytes_FromStringAndSize)(const char *, intptr_t);
static PyObject *(*p_PyTuple_New)(intptr_t);
static int (*p_PyTuple_SetItem)(PyObject *, intptr_t, PyObject *);
static PyObject *(*p_PyObject_Call)(PyObject *, PyObject *, PyObject *);
static PyObject *(*p_PyObject_GetItem)(PyObject *, PyObject *);
static intptr_t (*p_PyObject_Size)(PyObject *);
static int (*p_PyBytes_AsStringAndSize)(PyObject *, char **, intptr_t *);
static int (*p_PyObject_SetItem)(PyObject *, PyObject *, PyObject *);
static PyObject *(*p_PySequence_List)(PyObject *);
static PyObject *(*p_PySequence_GetItem)(PyObject *, intptr_t);

static int more_loaded;
static int load_more(void) {
  if (more_loaded) return 1;
#define GET2(n)                                                                    \
  do {                                                                             \
    *(void **)(&p_##n) = sym(#n);                                                  \
    if (!p_##n) {                                                                  \
      set_text("libpython に " #n " がありません");                                \
      return 0;                                                                    \
    }                                                                              \
  } while (0)
  GET2(PyLong_FromLongLong);
  GET2(PyLong_AsLongLong);
  GET2(PyFloat_FromDouble);
  GET2(PyFloat_AsDouble);
  GET2(PyBool_FromLong);
  GET2(PyObject_IsTrue);
  GET2(PyBytes_FromStringAndSize);
  GET2(PyTuple_New);
  GET2(PyTuple_SetItem);
  GET2(PyObject_Call);
  GET2(PyObject_GetItem);
  GET2(PyObject_Size);
  GET2(PyBytes_AsStringAndSize);
  GET2(PyObject_SetItem);
  GET2(PySequence_List);
  GET2(PySequence_GetItem);
#undef GET2
  more_loaded = 1;
  return 1;
}

// 始めて、追加の関数も取る。GIL を取った状態を g に入れて 1、だめなら 0
static int enter(PyGILState_STATE *g) {
  if (impl_start() < 0) return 0;
  *g = p_PyGILState_Ensure();
  if (!load_more()) {
    p_PyGILState_Release(*g);
    return 0;
  }
  return 1;
}

#define WITH_GIL(expr)                     \
  PyGILState_STATE g;                      \
  if (!enter(&g)) return NULL;             \
  PyObject *o = done_obj(expr);            \
  p_PyGILState_Release(g);                 \
  return o

static void *impl_from_int(int64_t n) { WITH_GIL(p_PyLong_FromLongLong(n)); }
static void *impl_from_float(double x) { WITH_GIL(p_PyFloat_FromDouble(x)); }
static void *impl_from_bool(int b) { WITH_GIL(p_PyBool_FromLong(b)); }
static void *impl_from_str(const char *s) { WITH_GIL(p_PyUnicode_FromString(s)); }
static void *impl_from_bytes(const uint8_t *b, int64_t n) { WITH_GIL(p_PyBytes_FromStringAndSize((const char *)b, (intptr_t)n)); }

// obj[key]
static void *impl_getitem(void *obj, void *key) { WITH_GIL(p_PyObject_GetItem(obj, key)); }

// 長さの決まった物の並びを作る(呼出の引数のため)。要素は impl_tuple_set で入れる
static void *impl_tuple_new(int64_t n) { WITH_GIL(p_PyTuple_New((intptr_t)n)); }

// 並びの i 番目に物を入れる(物の参照は並びが別に持つので、物は放してかまわない)
static int64_t impl_tuple_set(void *t, int64_t i, void *obj) {
  PyGILState_STATE g;
  if (!enter(&g)) return -1;
  p_Py_IncRef(obj);  // SetItem は参照を盗む
  int64_t rc = p_PyTuple_SetItem(t, (intptr_t)i, obj) == 0 ? 0 : -1;
  if (rc < 0) take_error();
  p_PyGILState_Release(g);
  return rc;
}

// f(*並び)
static void *impl_call_tuple(void *f, void *t) { WITH_GIL(p_PyObject_Call(f, t, NULL)); }

// f(*並び, **辞書)
static void *impl_call_tuple_kw(void *f, void *t, void *kw) { WITH_GIL(p_PyObject_Call(f, t, kw)); }

// 数にする(int にできるもの。結果は impl_int)
static int64_t impl_to_int(void *obj) {
  PyGILState_STATE g;
  if (!enter(&g)) return -1;
  long long v = p_PyLong_AsLongLong(obj);
  int64_t rc = 0;
  if (v == -1 && p_PyErr_Occurred()) {
    take_error();
    rc = -1;
  } else if (v > ((long long)1 << 62) || v < -((long long)1 << 62)) {
    set_text("OverflowError: する の数の範囲(±2^62)を超えています");
    rc = -1;
  } else {
    last_int = v;
  }
  p_PyGILState_Release(g);
  return rc;
}

// 小数にする(float にできるもの。結果は impl_float)
static int64_t impl_to_float(void *obj) {
  PyGILState_STATE g;
  if (!enter(&g)) return -1;
  double v = p_PyFloat_AsDouble(obj);
  int64_t rc = 0;
  if (v == -1.0 && p_PyErr_Occurred()) {
    take_error();
    rc = -1;
  } else {
    last_float = v;
  }
  p_PyGILState_Release(g);
  return rc;
}

// 真偽にする(bool(物))。1・0、失敗は -1
static int64_t impl_to_bool(void *obj) {
  PyGILState_STATE g;
  if (!enter(&g)) return -1;
  int r = p_PyObject_IsTrue(obj);
  if (r < 0) take_error();
  p_PyGILState_Release(g);
  return r;
}

// 文字列にする(str の物だけ。結果は impl_text)
static int64_t impl_to_str(void *obj) {
  PyGILState_STATE g;
  if (!enter(&g)) return -1;
  const char *u = p_PyUnicode_AsUTF8(obj);
  int64_t rc = 0;
  if (u) set_text(u);
  else {
    take_error();
    rc = -1;
  }
  p_PyGILState_Release(g);
  return rc;
}

// バイト列にする(bytes・bytearray・memoryview など。長さを返し、中身は impl_buf の番地に)
static int64_t impl_to_bytes(void *obj) {
  PyGILState_STATE g;
  if (!enter(&g)) return -1;
  int64_t rc = -1;
  PyObject *b = p_PyObject_CallFunctionObjArgs(fn_to_bytes, obj, NULL);  // bytes(物)
  if (b) {
    char *data = NULL;
    intptr_t n = 0;
    if (p_PyBytes_AsStringAndSize(b, &data, &n) == 0) {
      free(last_buf);
      last_buf = malloc(n > 0 ? (size_t)n : 1);
      if (n > 0) memcpy(last_buf, data, (size_t)n);
      rc = n;
    }
    p_Py_DecRef(b);
  }
  if (rc < 0) take_error();
  p_PyGILState_Release(g);
  return rc;
}

// len(物)
static int64_t impl_len(void *obj) {
  PyGILState_STATE g;
  if (!enter(&g)) return -1;
  intptr_t n = p_PyObject_Size(obj);
  if (n < 0) take_error();
  p_PyGILState_Release(g);
  return n;
}

// obj[key] = val
static int64_t impl_setitem(void *obj, void *key, void *val) {
  PyGILState_STATE g;
  if (!enter(&g)) return -1;
  int64_t rc = p_PyObject_SetItem(obj, key, val) == 0 ? 0 : -1;
  if (rc < 0) take_error();
  p_PyGILState_Release(g);
  return rc;
}

// list(物)
static void *impl_to_list(void *obj) { WITH_GIL(p_PySequence_List(obj)); }

// 並び(list・tuple など)の i 番目(0 から)
static void *impl_index(void *seq, int64_t i) { WITH_GIL(p_PySequence_GetItem(seq, (intptr_t)i)); }

// ---- する から呼ぶ関数 ----
// どれも結果の箱(Res)を返します。中身は supy_res_* で読み、supy_res_free で放します。
// 結果をスレッドごとの入れ物に置いて、後でもう一度呼んで取ってもらう形にはしません。
// 2回の呼び出しの間に別の呼び出しが入っても、結果を取り違えないようにするためです。

typedef struct {
  int64_t rc;  // 0 以上で成功(長さなど)、負で失敗
  void *obj;   // 物を返す関数の物(失敗は NULL)
  char *text;  // 文字列の結果か、失敗の文
  int64_t n;   // 数の結果
  double f;    // 小数の結果
  void *buf;   // バイト列の結果(rc バイト)
} Res;

// 同じ C の呼び出しの中で、スレッドごとの入れ物を箱に移す
static Res *fill(Res *r) {
  if (!r) return r;
  r->text = strdup(last_text ? last_text : "");
  r->n = last_int;
  r->f = last_float;
  return r;
}

int64_t supy_res_rc(Res *r) { return r ? r->rc : -1; }
void *supy_res_obj(Res *r) { return r ? r->obj : NULL; }
const char *supy_res_text(Res *r) { return r && r->text ? r->text : "結果の箱を作れませんでした"; }
int64_t supy_res_int(Res *r) { return r ? r->n : 0; }
double supy_res_float(Res *r) { return r ? r->f : 0.0; }
void *supy_res_buf(Res *r) { return r ? r->buf : NULL; }
int64_t supy_res_free(Res *r) {
  if (!r) return 0;
  free(r->text);
  free(r->buf);
  free(r);
  return 0;
}

Res *supy_start(void) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_start();
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_exec(const char *code) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_exec(code);
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_eval_json(const char *code) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_eval_json(code);
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_run_file(const char *path) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_run_file(path);
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_obj_json(void *obj) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_obj_json(obj);
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_obj_str(void *obj) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_obj_str(obj);
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_bind(const char *name, void *obj) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_bind(name, obj);
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_release(void *obj) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_release(obj);
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_tuple_set(void *t, int64_t i, void *obj) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_tuple_set(t, i, obj);
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_to_int(void *obj) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_to_int(obj);
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_to_float(void *obj) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_to_float(obj);
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_to_bool(void *obj) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_to_bool(obj);
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_to_str(void *obj) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_to_str(obj);
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_to_bytes(void *obj) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_to_bytes(obj);
  if (r) r->rc = rc;
  if (r && r->rc >= 0) {
    r->buf = last_buf;  // 箱へ渡す(入れ物は空にする)
    last_buf = NULL;
  }
  return fill(r);
}

Res *supy_len(void *obj) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_len(obj);
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_setitem(void *obj, void *key, void *val) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  int64_t rc = impl_setitem(obj, key, val);
  if (r) r->rc = rc;
  return fill(r);
}

Res *supy_import(const char *name) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_import(name);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_eval_obj(const char *code) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_eval_obj(code);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_getattr(void *obj, const char *name) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_getattr(obj, name);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_call(void *f, const char *args, const char *kwargs) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_call(f, args, kwargs);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_from_json(const char *json) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_from_json(json);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_from_int(int64_t n) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_from_int(n);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_from_float(double x) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_from_float(x);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_from_bool(int b) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_from_bool(b);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_from_str(const char *s) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_from_str(s);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_from_bytes(const uint8_t *b, int64_t n) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_from_bytes(b, n);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_getitem(void *obj, void *key) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_getitem(obj, key);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_tuple_new(int64_t n) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_tuple_new(n);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_call_tuple(void *f, void *t) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_call_tuple(f, t);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_call_tuple_kw(void *f, void *t, void *kw) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_call_tuple_kw(f, t, kw);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_to_list(void *obj) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_to_list(obj);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}

Res *supy_index(void *seq, int64_t i) {
  if (last_text) last_text[0] = 0;
  Res *r = (Res *)calloc(1, sizeof(Res));
  void *o = impl_index(seq, i);
  if (r) {
    r->obj = o;
    r->rc = o ? 0 : -1;
  } else if (o) {
    impl_release(o);
  }
  return fill(r);
}
