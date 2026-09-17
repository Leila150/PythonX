#ifndef PYTHONX_ERROR_STATEMENTS_H
#define PYTHONX_ERROR_STATEMENTS_H

#include "Python.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime operations corresponding to Python's error-related statements. */
PyAPI_FUNC(int) _PyX_StatementRaise(PyObject *exception, PyObject *cause);
PyAPI_FUNC(int) _PyX_StatementReraise(void);
PyAPI_FUNC(int) _PyX_StatementAssert(PyObject *test, PyObject *message);

/* Operations used by except/except* handlers. */
PyAPI_FUNC(int) _PyX_ExceptionMatchesValue(PyObject *exception, PyObject *expected);
PyAPI_FUNC(int) _PyX_BeginExceptionHandler(PyObject *exception);
PyAPI_FUNC(void) _PyX_EndExceptionHandler(PyObject *previous);
PyAPI_FUNC(int) _PyX_BindException(PyObject *globals, PyObject *name, PyObject *exception);
PyAPI_FUNC(int) _PyX_ClearExceptionBinding(PyObject *globals, PyObject *name);

/* Exception-group matching used by except*. */
PyAPI_FUNC(PyObject *) _PyX_SplitExceptionGroup(PyObject *exception, PyObject *match);
PyAPI_FUNC(PyObject *) _PyX_WrapExceptionGroup(PyObject *exception);
PyAPI_FUNC(int) _PyX_IsExceptionGroup(PyObject *exception);

#ifdef __cplusplus
}
#endif

#endif /* PYTHONX_ERROR_STATEMENTS_H */
