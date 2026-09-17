#include "Python.h"
#include "pythonx_errors.h"

/* PythonX keeps the interpreter's native exception machinery intact. */

int
_PyX_ErrorSet(PyObject *exception, PyObject *value)
{
    if (!exception || !PyExceptionClass_Check(exception)) {
        PyErr_SetString(PyExc_TypeError,
                        "PythonX: exception must be an exception class");
        return -1;
    }

    PyErr_SetObject(exception, value ? value : Py_None);
    return -1;
}

int
_PyX_ErrorSetString(PyObject *exception, const char *message)
{
    if (!exception || !PyExceptionClass_Check(exception)) {
        PyErr_SetString(PyExc_TypeError,
                        "PythonX: exception must be an exception class");
        return -1;
    }

    PyErr_SetString(exception, message ? message : "");
    return -1;
}

int
_PyX_ErrorFormat(PyObject *exception, const char *format, ...)
{
    if (!exception || !PyExceptionClass_Check(exception)) {
        PyErr_SetString(PyExc_TypeError,
                        "PythonX: exception must be an exception class");
        return -1;
    }

    va_list args;
    va_start(args, format);
    PyObject *message = PyUnicode_FromFormatV(format, args);
    va_end(args);

    if (!message) {
        return -1;
    }

    PyErr_SetObject(exception, message);
    Py_DECREF(message);
    return -1;
}

void
_PyX_ErrorFetch(PyObject **type, PyObject **value, PyObject **traceback)
{
    PyErr_Fetch(type, value, traceback);
}

void
_PyX_ErrorRestore(PyObject *type, PyObject *value, PyObject *traceback)
{
    PyErr_Restore(type, value, traceback);
}

int
_PyX_ErrorMatches(PyObject *type, PyObject *exc)
{
    if (!type || !exc) {
        return 0;
    }
    return PyErr_GivenExceptionMatches(type, exc);
}

int
_PyX_ErrorMatchesCurrent(PyObject *exc)
{
    if (!exc) {
        return 0;
    }
    return PyErr_ExceptionMatches(exc);
}

int
_PyX_ErrorNormalize(PyObject **type, PyObject **value, PyObject **traceback)
{
    if (!type || !value || !traceback) {
        PyErr_SetString(PyExc_SystemError,
                        "PythonX: invalid exception normalization state");
        return -1;
    }

    PyErr_NormalizeException(type, value, traceback);
    return 0;
}

int
_PyX_ErrorAddTraceback(const char *funcname, const char *filename, int lineno)
{
    /*
     * PyTraceBack_Here() works with the current frame.  PythonX's native
     * backend can use the CPython internal traceback injector when it has a
     * native source location that needs to be represented as a Python frame.
     *
     * Keep this helper conservative for now: if there is no active exception,
     * there is no traceback to extend.
     */
    (void)funcname;
    (void)filename;
    (void)lineno;

    if (!PyErr_Occurred()) {
        return 0;
    }

    return 0;
}

void
_PyX_ErrorPrint(void)
{
    PyErr_Print();
}

int
_PyX_ErrorRaiseWithContext(PyObject *exception, PyObject *value)
{
    PyObject *old_type = NULL;
    PyObject *old_value = NULL;
    PyObject *old_traceback = NULL;

    if (PyErr_Occurred()) {
        PyErr_Fetch(&old_type, &old_value, &old_traceback);

        /* Restore first so CPython can establish normal implicit context. */
        PyErr_Restore(old_type, old_value, old_traceback);
    }

    PyErr_SetObject(exception, value ? value : Py_None);
    return -1;
}
