#ifndef PYTHONX_NATIVE_H
#define PYTHONX_NATIVE_H

#include "Python.h"
#include "pycore_ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PythonX native backend.
 *
 * Python source is parsed by Python's existing frontend. This backend consumes
 * that AST and emits target machine-code bytes directly. It does not generate
 * C, C++, Rust, assembly source, Python bytecode, or another source language.
 */

PyAPI_FUNC(PyObject *) _PyX_NativeCompile(mod_ty module, PyObject *filename);
PyAPI_FUNC(PyObject *) _PyX_NativeExecute(PyObject *native_code);

#ifdef __cplusplus
}
#endif

#endif /* PYTHONX_NATIVE_H */
