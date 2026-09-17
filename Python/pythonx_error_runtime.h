#ifndef PYTHONX_ERROR_RUNTIME_H
#define PYTHONX_ERROR_RUNTIME_H

#include "Python.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Core Python exception-state operations used by the native backend. */
PyAPI_FUNC(int) _PyX_Raise(PyObject *exc, PyObject *value, PyObject *cause, int suppress_context);
PyAPI_FUNC(int) _PyX_Reraise(void);
PyAPI_FUNC(int) _PyX_ExceptionMatches(PyObject *type, PyObject *expected);
PyAPI_FUNC(int) _PyX_FetchNormalized(PyObject **type, PyObject **value, PyObject **traceback);
PyAPI_FUNC(void) _PyX_ClearError(void);

/* Traceback and source-location support. */
PyAPI_FUNC(int) _PyX_AttachTraceback(const char *funcname, const char *filename, int lineno);
PyAPI_FUNC(int) _PyX_SetSyntaxError(const char *message, const char *filename, int lineno, int column);

/* Python warning machinery; warnings remain Python Warning subclasses. */
PyAPI_FUNC(int) _PyX_Warn(PyObject *category, const char *message, int stacklevel);

/* Exception-group operations. */
PyAPI_FUNC(PyObject *) _PyX_NewExceptionGroup(PyObject *message, PyObject *exceptions);
PyAPI_FUNC(PyObject *) _PyX_NewBaseExceptionGroup(PyObject *message, PyObject *exceptions);
PyAPI_FUNC(PyObject *) _PyX_ExceptionGroupSplit(PyObject *group, PyObject *match, int *is_match);
PyAPI_FUNC(PyObject *) _PyX_ExceptionGroupSubgroup(PyObject *group, PyObject *match, int *is_match);

/* Exception metadata. */
PyAPI_FUNC(PyObject *) _PyX_GetCause(PyObject *exception);
PyAPI_FUNC(int) _PyX_SetCause(PyObject *exception, PyObject *cause, int suppress_context);
PyAPI_FUNC(PyObject *) _PyX_GetContext(PyObject *exception);
PyAPI_FUNC(int) _PyX_SetContext(PyObject *exception, PyObject *context);
PyAPI_FUNC(PyObject *) _PyX_GetTraceback(PyObject *exception);
PyAPI_FUNC(int) _PyX_SetTraceback(PyObject *exception, PyObject *traceback);

#ifdef __cplusplus
}
#endif

#endif /* PYTHONX_ERROR_RUNTIME_H */
