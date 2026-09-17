#!/usr/bin/env python3
"""Build the PythonX 3.14 branch without GitHub Actions.

This is a local build driver. It keeps CPython's parser/AST frontend and
patches only the generated build graph and top-level execution path so Python
source is lowered into PythonX IR and executed through PythonX native code.
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAKEFILE = ROOT / "Makefile"
RUNNER = ROOT / "Python" / "pythonrun.c"

PYX_OBJECTS = [
    "pythonx_ir",
    "pythonx_backend",
    "pythonx_native_ir",
    "pythonx_compile",
    "pythonx_errors",
    "pythonx_traceback",
    "pythonx_error_runtime",
    "pythonx_error_statements",
]


def run(*args: str) -> None:
    print("+", " ".join(args), flush=True)
    subprocess.run(args, cwd=ROOT, check=True)


def patch_makefile() -> None:
    text = MAKEFILE.read_text(encoding="utf-8")
    marker = "\t\tPython/asdl.o \\\n"
    if "Python/pythonx_ir.o" in text:
        return
    if marker not in text:
        raise RuntimeError("Could not find Python object insertion point in Makefile")
    extra = "".join(f"\t\tPython/{name}.o \\\n" for name in PYX_OBJECTS)
    text = text.replace(marker, marker + extra, 1)
    MAKEFILE.write_text(text, encoding="utf-8")


def patch_runner() -> None:
    text = RUNNER.read_text(encoding="utf-8")
    include = '#include "pythonx_compile.h"\n#include "pythonx_native_ir.h"\n'
    marker = '#include "pycore_ast.h"           // PyAST_mod2obj()\n'
    if "pythonx_compile.h" not in text:
        if marker not in text:
            raise RuntimeError("Could not find pythonrun.c include insertion point")
        text = text.replace(marker, marker + include, 1)

    start = text.find("static PyObject *\nrun_mod(")
    if start < 0:
        raise RuntimeError("Could not find run_mod() in Python/pythonrun.c")
    end = text.find("\nstatic PyObject *\nrun_pyc_file(", start)
    if end < 0:
        raise RuntimeError("Could not find run_pyc_file() after run_mod()")

    replacement = '''static PyObject *
run_mod(mod_ty mod, PyObject *filename, PyObject *globals, PyObject *locals,
        PyCompilerFlags *flags, PyArena *arena, PyObject *interactive_src,
        int generate_new_source)
{
    (void)filename;
    (void)globals;
    (void)locals;
    (void)flags;
    (void)arena;
    (void)interactive_src;
    (void)generate_new_source;

    /* Python frontend: parser -> AST.  PythonX owns everything after AST. */
    PyObject *native_code = _PyX_CompileASTNative(mod);
    if (native_code == NULL) {
        return NULL;
    }

    PyObject *result = _PyX_NativeExecuteIR(native_code);
    Py_DECREF(native_code);
    return result;
}
'''
    text = text[:start] + replacement + text[end:]
    RUNNER.write_text(text, encoding="utf-8")


def main() -> int:
    if not MAKEFILE.exists():
        run("./configure", "--with-pydebug")
    patch_makefile()
    patch_runner()
    run("make", "-j2")
    run("./python", "-c", "print(1 + 1)")
    run("./python", "-c", "x = 10; print(x * 2)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        print(f"PythonX build failed with exit code {exc.returncode}", file=sys.stderr)
        raise SystemExit(exc.returncode)
