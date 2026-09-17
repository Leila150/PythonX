#include "Python.h"
#include "pythonx_error_runtime.h"
#include "pythonx_traceback.h"

int
_PyX_Raise(PyObject *exc, PyObject *value, PyObject *cause, int suppress_context)
{
    if (!exc || !PyExceptionClass_Check(exc)) {
        PyErr_SetString(PyExc_TypeError, "exceptions must derive from BaseException");
        return -1;
    }

    if (cause != NULL) {
        if (!PyExceptionInstance_Check(cause)) {
            PyErr_SetString(PyExc_TypeError, "exception causes must derive from BaseException");
            return -1;
        }
    }

    PyErr_SetObject(exc, value ? value : Py_None);

    if (cause != NULL) {
        PyObject *type = NULL;
        PyObject *raised = NULL;
        PyObject *tb = NULL;
        PyErr_Fetch(&type, &raised, &tb);
        if (raised && PyExceptionInstance_Check(raised)) {
            if (PyException_SetCause(raised, Py_NewRef(cause)) < 0) {
                Py_XDECREF(type);
                Py_XDECREF(raised);
                Py_XDECREF(tb);
                return -1;
            }
            if (suppress_context) {
                PyException_SetTraceback(raised, tb);
            }
        }
        PyErr_Restore(type, raised, tb);
    }

    return -1;
}

int
_PyX_Reraise(void)
{
    if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_RuntimeError, "No active exception to reraise");
    }
    return -1;
}

int
_PyX_ExceptionMatches(PyObject *type, PyObject *expected)
{
    if (!type || !expected) {
        return 0;
    }
    return PyErr_GivenExceptionMatches(type, expected);
}

int
_PyX_FetchNormalized(PyObject **type, PyObject **value, PyObject **traceback)
{
    if (!type || !value || !traceback) {
        PyErr_SetString(PyExc_SystemError, "invalid exception state pointers");
        return -1;
    }
    PyErr_Fetch(type, value, traceback);
    PyErr_NormalizeException(type, value, traceback);
    return 0;
}

void
_PyX_ClearError(void)
{
    PyErr_Clear();
}

int
_PyX_AttachTraceback(const char *funcname, const char *filename, int lineno)
{
    return _PyX_TracebackAddIfError(funcname, filename, lineno);
}

int
_PyX_SetSyntaxError(const char *message, const char *filename, int lineno, int column)
{
    PyErr_SetString(PyExc_SyntaxError, message ? message : "invalid syntax");
    if (filename != NULL) {
        PyErr_SyntaxLocationEx(filename, lineno, column);
    }
    return -1;
}

int
_PyX_Warn(PyObject *category, const char *message, int stacklevel)
{
    if (!category) {
        category = PyExc_Warning;
    }
    if (!PyExceptionClass_Check(category) || !PyErr_GivenExceptionMatches(category, PyExc_Warning)) {
        PyErr_SetString(PyExc_TypeError, "warning category must derive from Warning");
        return -1;
    }
    return PyErr_WarnEx(category, message ? message : "", stacklevel > 0 ? stacklevel : 1);
}

PyObject *
_PyX_NewExceptionGroup(PyObject *message, PyObject *exceptions)
{
    if (!PyExc_ExceptionGroup) {
        PyErr_SetString(PyExc_RuntimeError, "ExceptionGroup is unavailable");
        return NULL;
    }
    if (!message || !exceptions) {
        PyErr_SetString(PyExc_TypeError, "ExceptionGroup requires message and exceptions");
        return NULL;
    }
    return PyObject_CallFunctionObjArgs(PyExc_ExceptionGroup, message, exceptions, NULL);
}

PyObject *
_PyX_NewBaseExceptionGroup(PyObject *message, PyObject *exceptions)
{
    if (!PyExc_BaseExceptionGroup) {
        PyErr_SetString(PyExc_RuntimeError, "BaseExceptionGroup is unavailable");
        return NULL;
    }
    if (!message || !exceptions) {
        PyErr_SetString(PyExc_TypeError, "BaseExceptionGroup requires message and exceptions");
        return NULL;
    }
    return PyObject_CallFunctionObjArgs(PyExc_BaseExceptionGroup, message, exceptions, NULL);
}

static PyObject *
exception_group_method(PyObject *group, const char *method, PyObject *match, int *is_match)
{
    if (is_match) {
        *is_match = 0;
    }
    if (!group || !PyExceptionInstance_Check(group)) {
        PyErr_SetString(PyExc_TypeError, "expected an exception instance");
        return NULL;
    }

    PyObject *result = PyObject_CallMethod(group, method, "O", match);
    if (!result) {
        return NULL;
    }
    if (is_match && result != Py_None) {
        *is_match = 1;
    }
    return result;
}

PyObject *
_PyX_ExceptionGroupSplit(PyObject *group, PyObject *match, int *is_match)
{
    return exception_group_method(group, "split", match, is_match);
}

PyObject *
_PyX_ExceptionGroupSubgroup(PyObject *group, PyObject *match, int *is_match)
{
    return exception_group_method(group, "subgroup", match, is_match);
}

PyObject *
_PyX_GetCause(PyObject *exception)
{
    if (!exception || !PyExceptionInstance_Check(exception)) {
        PyErr_SetString(PyExc_TypeError, "expected an exception instance");
        return NULL;
    }
    PyObject *cause = PyException_GetCause(exception);
    return cause ? cause : Py_NewRef(Py_None);
}

int
_PyX_SetCause(PyObject *exception, PyObject *cause, int suppress_context)
{
    if (!exception || !PyExceptionInstance_Check(exception)) {
        PyErr_SetString(PyExc_TypeError, "expected an exception instance");
        return -1;
    }
    if (cause != Py_None && (!cause || !PyExceptionInstance_Check(cause))) {
        PyErr_SetString(PyExc_TypeError, "exception cause must be an exception instance or None");
        return -1;
    }
    PyObject *owned = cause == Py_None ? NULL : Py_NewRef(cause);
    PyException_SetCause(exception, owned);
    if (suppress_context) {
        PyException_SetTraceback(exception, PyException_GetTraceback(exception));
    }
    return 0;
}

PyObject *
_PyX_GetContext(PyObject *exception)
{
    if (!exception || !PyExceptionInstance_Check(exception)) {
        PyErr_SetString(PyExc_TypeError, "expected an exception instance");
        return NULL;
    }
    PyObject *context = PyException_GetContext(exception);
    return context ? context : Py_NewRef(Py_None);
}

int
_PyX_SetContext(PyObject *exception, PyObject *context)
{
    if (!exception || !PyExceptionInstance_Check(exception)) {
        PyErr_SetString(PyExc_TypeError, "expected an exception instance");
        return -1;
    }
    if (context != Py_None && (!context || !PyExceptionInstance_Check(context))) {
        PyErr_SetString(PyExc_TypeError, "exception context must be an exception instance or None");
        return -1;
    }
    PyException_SetContext(exception, context == Py_None ? NULL : Py_NewRef(context));
    return 0;
}

PyObject *
_PyX_GetTraceback(PyObject *exception)
{
    if (!exception || !PyExceptionInstance_Check(exception)) {
        PyErr_SetString(PyExc_TypeError, "expected an exception instance");
        return NULL;
    }
    PyObject *tb = PyException_GetTraceback(exception);
    return tb ? Py_NewRef(tb) : Py_NewRef(Py_None);
}

int
_PyX_SetTraceback(PyObject *exception, PyObject *traceback)
{
    if (!exception || !PyExceptionInstance_Check(exception)) {
        PyErr_SetString(PyExc_TypeError, "expected an exception instance");
        return -1;
    }
    if (traceback != Py_None && traceback != NULL && !PyTraceBack_Check(traceback)) {
        PyErr_SetString(PyExc_TypeError, "traceback must be a traceback object or None");
        return -1;
    }
    PyException_SetTraceback(exception, traceback == Py_None ? NULL : Py_XNewRef(traceback));
    return 0;
}
