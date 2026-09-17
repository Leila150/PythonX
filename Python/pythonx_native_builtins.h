#ifndef PYTHONX_NATIVE_BUILTINS_H
#define PYTHONX_NATIVE_BUILTINS_H

#include "Python.h"

/*
 * PythonX native builtin operations.
 *
 * These are implementation primitives, not a translation target. Python
 * source remains Python source; the PythonX compiler may select these native
 * operations when lowering calls to the corresponding Python builtins.
 */

PyAPI_FUNC(PyObject *) _PyX_Print(
    PyObject *const *args,
    Py_ssize_t nargs,
    PyObject *sep,
    PyObject *end,
    PyObject *file,
    int flush);

PyAPI_FUNC(PyObject *) _PyX_Input(PyObject *prompt);

PyAPI_FUNC(PyObject *) _PyX_Range(
    PyTypeObject *type,
    PyObject *const *args,
    Py_ssize_t nargs);

#endif /* PYTHONX_NATIVE_BUILTINS_H */
