/* PythonX native implementations of Python's print(), input(), and range(). */

#include "Python.h"
#include "pycore_fileutils.h"
#include "pycore_sysmodule.h"
#include "pythonx_native_builtins.h"

/*
 * These functions implement Python operations directly inside PythonX's
 * runtime. They are NOT generated C from user Python code and are NOT a
 * translation layer. The compiler remains responsible for turning Python's
 * AST into PythonX's own native representation and ultimately machine code.
 */

PyObject *
_PyX_Print(PyObject *const *args, Py_ssize_t nargs,
           PyObject *sep, PyObject *end, PyObject *file, int flush)
{
    if (file == Py_None) {
        file = _PySys_GetRequiredAttr(&_Py_ID(stdout));
        if (file == NULL) {
            return NULL;
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
        if (i != 0) {
            if (sep == NULL) {
                if (PyFile_WriteString(" ", file) < 0) {
                    Py_DECREF(file);
                    return NULL;
                }
            }
            else if (PyFile_WriteObject(sep, file, Py_PRINT_RAW) < 0) {
                Py_DECREF(file);
                return NULL;
            }
        }

        if (PyFile_WriteObject(args[i], file, Py_PRINT_RAW) < 0) {
            Py_DECREF(file);
            return NULL;
        }
    }

    if (end == NULL) {
        if (PyFile_WriteString("\n", file) < 0) {
            Py_DECREF(file);
            return NULL;
        }
    }
    else if (PyFile_WriteObject(end, file, Py_PRINT_RAW) < 0) {
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
_PyX_Input(PyObject *prompt)
{
    PyObject *fin = _PySys_GetRequiredAttr(&_Py_ID(stdin));
    if (fin == NULL) {
        return NULL;
    }

    PyObject *fout = _PySys_GetRequiredAttr(&_Py_ID(stdout));
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

    if (PySys_Audit("builtins.input", "O", prompt) < 0) {
        goto error;
    }

    if (prompt != NULL && PyFile_WriteObject(prompt, fout, Py_PRINT_RAW) < 0) {
        goto error;
    }

    if (_PyFile_Flush(fout) < 0) {
        PyErr_Clear();
    }

    PyObject *result = PyFile_GetLine(fin, -1);
    if (result == NULL) {
        goto error;
    }

    if (PyUnicode_Check(result)) {
        Py_ssize_t length = PyUnicode_GET_LENGTH(result);
        if (length > 0 && PyUnicode_READ_CHAR(result, length - 1) == '\n') {
            PyObject *trimmed = PyUnicode_Substring(result, 0, length - 1);
            if (trimmed == NULL) {
                Py_DECREF(result);
                goto error;
            }
            Py_SETREF(result, trimmed);
        }
    }

    if (PySys_Audit("builtins.input/result", "O", result) < 0) {
        Py_DECREF(result);
        goto error;
    }

    Py_DECREF(fin);
    Py_DECREF(fout);
    return result;

error:
    Py_DECREF(fin);
    Py_DECREF(fout);
    return NULL;
}

PyObject *
_PyX_Range(PyTypeObject *type, PyObject *const *args, Py_ssize_t nargs)
{
    /* range is a native Python object; preserve its existing constructor and
       object semantics rather than translating range() into another language. */
    PyObject *packed = PyTuple_New(nargs);
    if (packed == NULL) {
        return NULL;
    }

    for (Py_ssize_t i = 0; i < nargs; i++) {
        PyTuple_SET_ITEM(packed, i, Py_NewRef(args[i]));
    }

    PyObject *result = type->tp_new(type, packed, NULL);
    Py_DECREF(packed);
    return result;
}
