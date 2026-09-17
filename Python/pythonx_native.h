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
 * This backend consumes Python's AST directly. It does not emit C, C++,
 * Rust, assembly source, Python bytecode, or another source language.
 * Its final output is target machine-code bytes held by a PyCapsule.
 */

PyAPI_FUNC(PyObject *) _PyX_NativeCompile(mod_ty module, PyObject *filename);

#ifdef __cplusplus
}
#endif

#endif /* PYTHONX_NATIVE_H */
