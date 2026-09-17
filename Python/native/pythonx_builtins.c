/* PythonX native backend for selected builtins. */

#include "Python.h"
#include "pycore_fileutils.h"
#include "pycore_sysmodule.h"
#include "pythonx_builtins.h"

/*
 * These functions deliberately operate on Python objects and Python's
 * existing runtime protocols. That keeps the Python frontend/API identical
 * while giving the future native compiler stable backend entry points.
 */

PyObject *
PyXNative_Print(PyObject *const *args, Py_ssize_t nargs,
                PyObject *sep, PyObject *end,
                PyObject *file, int flush)
{
    if (file == Py_None) {
        file = _PySys_GetRequiredAttr(&_Py_ID(stdout));
        if (file == NULL) {
            return NULL;
        }
        if (file == Py_None) {
            Py_DECREF(file);
            Py_RETURN_NONE;
        }
    }
    else {
        Py_INCREF(file);
    }

    if (sep == Py_None) {
        sep = NULL;
    }
    else if (sep != NULL && !PyUnicode_Check(sep)) {
        PyErr_Format(PyExc_TypeError,
                     "sep must be None or a string, not %.200s",
                     Py_TYPE(sep)->tp_name);
        Py_DECREF(file);
        return NULL;
    }

    if (end == Py_None) {
        end = NULL;
    }
    else if (end != NULL && !PyUnicode_Check(end)) {
        PyErr_Format(PyExc_TypeError,
                     "end must be None or a string, not %.200s",
                     Py_TYPE(end)->tp_name);
        Py_DECREF(file);
        return NULL;
    }

    for (Py_ssize_t i = 0; i < nargs; i++) {
        int err;
        if (i > 0) {
            err = sep == NULL
                ? PyFile_WriteString(" ", file)
                : PyFile_WriteObject(sep, file, Py_PRINT_RAW);
            if (err) {
                Py_DECREF(file);
                return NULL;
            }
        }

        err = PyFile_WriteObject(args[i], file, Py_PRINT_RAW);
        if (err) {
            Py_DECREF(file);
            return NULL;
        }
    }

    if (end == NULL) {
        if (PyFile_WriteString("\n", file)) {
            Py_DECREF(file);
            return NULL;
        }
    }
    else if (PyFile_WriteObject(end, file, Py_PRINT_RAW)) {
        Py_DECREF(file);
        return NULL;
    }

    if (flush && _PyFile_Flush(file) < 0) {
        Py_DECREF(file);
        return NULL;
    }

    Py_DECREF(file);
    Py_RETURN_NONE;
}

PyObject *
PyXNative_Input(PyObject *prompt)
{
    PyObject *fin = NULL;
    PyObject *fout = NULL;
    PyObject *result = NULL;

    fin = _PySys_GetRequiredAttr(&_Py_ID(stdin));
    if (fin == NULL) {
        return NULL;
    }
    fout = _PySys_GetRequiredAttr(&_Py_ID(stdout));
    if (fout == NULL) {
        Py_DECREF(fin);
        return NULL;
    }

    if (fin == Py_None) {
        PyErr_SetString(PyExc_RuntimeError, "lost sys.stdin");
        goto error;
    }
    if (fout == Py_None) {
        PyErr_SetString(PyExc_RuntimeError, "lost sys.stdout");
        goto error;
    }

    if (PySys_Audit("builtins.input", "O", prompt ? prompt : Py_None) < 0) {
        goto error;
    }

    if (prompt != NULL) {
        if (PyFile_WriteObject(prompt, fout, Py_PRINT_RAW) != 0) {
            goto error;
        }
    }

    if (_PyFile_Flush(fout) < 0) {
        PyErr_Clear();
    }

    result = PyFile_GetLine(fin, -1);
    if (result == NULL) {
        goto error;
    }

    if (PyUnicode_Check(result)) {
        Py_ssize_t size = PyUnicode_GET_LENGTH(result);
        if (size > 0 && PyUnicode_READ_CHAR(result, size - 1) == '\n') {
            PyObject *trimmed = PyUnicode_Substring(result, 0, size - 1);
            if (trimmed == NULL) {
                goto error;
            }
            Py_SETREF(result, trimmed);
        }
    }

    if (PySys_Audit("builtins.input/result", "O", result) < 0) {
        goto error;
    }

    Py_DECREF(fin);
    Py_DECREF(fout);
    return result;

error:
    Py_XDECREF(result);
    Py_DECREF(fin);
    Py_DECREF(fout);
    return NULL;
}

PyObject *
PyXNative_Range(PyTypeObject *type, PyObject *const *args, Py_ssize_t nargs)
{
    /* Range construction is already a native C object operation. The PythonX
       compiler can lower range(...) directly to this entry point. */
    PyObject *tuple = PyTuple_New(nargs);
    if (tuple == NULL) {
        return NULL;
    }

    for (Py_ssize_t i = 0; i < nargs; i++) {
        Py_INCREF(args[i]);
        PyTuple_SET_ITEM(tuple, i, args[i]);
    }

    PyObject *result = type->tp_new(type, tuple, NULL);
    Py_DECREF(tuple);
    return result;
}
