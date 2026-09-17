#include "Python.h"
#include "pythonx_error_statements.h"

static PyObject *
instantiate_exception(PyObject *exception)
{
    if (!exception) {
        return NULL;
    }

    if (PyExceptionInstance_Check(exception)) {
        return Py_NewRef(exception);
    }

    if (!PyExceptionClass_Check(exception)) {
        PyErr_SetString(PyExc_TypeError,
                        "exceptions must derive from BaseException");
        return NULL;
    }

    return PyObject_CallNoArgs(exception);
}

int
_PyX_StatementRaise(PyObject *exception, PyObject *cause)
{
    if (exception == NULL) {
        return _PyX_StatementReraise();
    }

    PyObject *raised = instantiate_exception(exception);
    if (!raised) {
        return -1;
    }

    if (cause != NULL) {
        if (cause != Py_None && !PyExceptionInstance_Check(cause)) {
            Py_DECREF(raised);
            PyErr_SetString(PyExc_TypeError,
                            "exception causes must derive from BaseException");
            return -1;
        }

        /* PyException_SetCause() steals its argument and marks the explicit
           cause as suppressing implicit context.  Passing NULL represents
           the Python spelling `raise ... from None`. */
        PyObject *owned_cause = cause == Py_None ? NULL : Py_NewRef(cause);
        PyException_SetCause(raised, owned_cause);
    }

    PyErr_SetRaisedException(raised);
    return -1;
}

int
_PyX_StatementReraise(void)
{
    PyObject *handled = PyErr_GetHandledException();
    if (!handled) {
        PyErr_SetString(PyExc_RuntimeError,
                        "No active exception to reraise");
        return -1;
    }

    PyErr_SetRaisedException(handled);
    return -1;
}

int
_PyX_StatementAssert(PyObject *test, PyObject *message)
{
    if (!test) {
        return -1;
    }

    int truth = PyObject_IsTrue(test);
    if (truth < 0) {
        return -1;
    }
    if (truth) {
        return 0;
    }

    if (message && message != Py_None) {
        PyErr_SetObject(PyExc_AssertionError, message);
    }
    else {
        PyErr_SetNone(PyExc_AssertionError);
    }
    return -1;
}

int
_PyX_ExceptionMatchesValue(PyObject *exception, PyObject *expected)
{
    if (!exception || !expected) {
        return 0;
    }
    return PyErr_GivenExceptionMatches(exception, expected);
}

int
_PyX_BeginExceptionHandler(PyObject *exception)
{
    if (!exception || !PyExceptionInstance_Check(exception)) {
        PyErr_SetString(PyExc_TypeError,
                        "exception handler requires an exception instance");
        return -1;
    }

    PyObject *owned = Py_NewRef(exception);
    PyErr_SetHandledException(owned);
    return 0;
}

void
_PyX_EndExceptionHandler(PyObject *previous)
{
    PyErr_SetHandledException(previous);
}

int
_PyX_BindException(PyObject *globals, PyObject *name, PyObject *exception)
{
    if (!globals || !PyDict_Check(globals) || !name || !PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError,
                        "invalid exception binding target");
        return -1;
    }
    if (!exception || !PyExceptionInstance_Check(exception)) {
        PyErr_SetString(PyExc_TypeError,
                        "exception binding requires an exception instance");
        return -1;
    }

    return PyDict_SetItem(globals, name, exception);
}

int
_PyX_ClearExceptionBinding(PyObject *globals, PyObject *name)
{
    if (!globals || !PyDict_Check(globals) || !name || !PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError,
                        "invalid exception binding target");
        return -1;
    }

    if (PyDict_DelItem(globals, name) < 0) {
        if (PyErr_ExceptionMatches(PyExc_KeyError)) {
            PyErr_Clear();
            return 0;
        }
        return -1;
    }
    return 0;
}

int
_PyX_IsExceptionGroup(PyObject *exception)
{
    if (!exception || !PyExceptionInstance_Check(exception)) {
        return 0;
    }

    if (PyExc_ExceptionGroup &&
        PyObject_IsInstance(exception, PyExc_ExceptionGroup) == 1) {
        return 1;
    }
    if (PyExc_BaseExceptionGroup &&
        PyObject_IsInstance(exception, PyExc_BaseExceptionGroup) == 1) {
        return 1;
    }
    return 0;
}

PyObject *
_PyX_WrapExceptionGroup(PyObject *exception)
{
    if (!exception || !PyExceptionInstance_Check(exception)) {
        PyErr_SetString(PyExc_TypeError,
                        "expected an exception instance");
        return NULL;
    }

    PyObject *items = PyTuple_Pack(1, exception);
    if (!items) {
        return NULL;
    }

    PyObject *group;
    if (PyExc_ExceptionGroup &&
        PyObject_IsInstance(exception, PyExc_Exception) == 1) {
        group = PyObject_CallFunctionObjArgs(
            PyExc_ExceptionGroup, PyUnicode_FromString(""), items, NULL);
    }
    else {
        group = PyObject_CallFunctionObjArgs(
            PyExc_BaseExceptionGroup, PyUnicode_FromString(""), items, NULL);
    }

    Py_DECREF(items);
    return group;
}

PyObject *
_PyX_SplitExceptionGroup(PyObject *exception, PyObject *match)
{
    if (!exception || !PyExceptionInstance_Check(exception)) {
        PyErr_SetString(PyExc_TypeError,
                        "expected an exception instance");
        return NULL;
    }
    if (!match) {
        PyErr_SetString(PyExc_TypeError,
                        "exception group match cannot be NULL");
        return NULL;
    }

    PyObject *group = Py_NewRef(exception);
    if (!_PyX_IsExceptionGroup(group)) {
        PyObject *wrapped = _PyX_WrapExceptionGroup(group);
        Py_DECREF(group);
        if (!wrapped) {
            return NULL;
        }
        group = wrapped;
    }

    PyObject *result = PyObject_CallMethod(group, "split", "O", match);
    Py_DECREF(group);
    return result;
}
