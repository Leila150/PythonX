#ifndef PYTHONX_NATIVE_BUILTINS_H
#define PYTHONX_NATIVE_BUILTINS_H

#include "Python.h"

/*
 * PythonX native builtin backend.
 *
 * The Python frontend keeps Python's exact builtin signatures. These entry
 * points are the backend boundary: the compiler may emit direct native calls
 * to them instead of routing builtin calls through Python bytecode.
 */

PyObject *PyXNative_Print(
    PyObject *const *args,
    Py_ssize_t nargs,
    PyObject *sep,
    PyObject *end,
    PyObject *file,
    int flush);

PyObject *PyXNative_Input(PyObject *prompt);

PyObject *PyXNative_Range(
    PyTypeObject *type,
    PyObject *const *args,
    Py_ssize_t nargs);

#endif /* PYTHONX_NATIVE_BUILTINS_H */
