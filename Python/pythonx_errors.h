#ifndef PYTHONX_ERRORS_H
#define PYTHONX_ERRORS_H

#include "Python.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PythonX error runtime.
 *
 * PythonX deliberately uses Python's native exception objects and exception
 * state.  This is not a second error system: it exposes the machinery the
 * native backend needs while preserving Python's exception semantics.
 */

PyAPI_FUNC(int) _PyX_ErrorSet(PyObject *exception, PyObject *value);
PyAPI_FUNC(int) _PyX_ErrorSetString(PyObject *exception, const char *message);
PyAPI_FUNC(int) _PyX_ErrorFormat(PyObject *exception, const char *format, ...);

PyAPI_FUNC(void) _PyX_ErrorFetch(PyObject **type, PyObject **value, PyObject **traceback);
PyAPI_FUNC(void) _PyX_ErrorRestore(PyObject *type, PyObject *value, PyObject *traceback);
PyAPI_FUNC(int) _PyX_ErrorMatches(PyObject *type, PyObject *exc);
PyAPI_FUNC(int) _PyX_ErrorMatchesCurrent(PyObject *exc);

PyAPI_FUNC(int) _PyX_ErrorNormalize(PyObject **type, PyObject **value, PyObject **traceback);
PyAPI_FUNC(int) _PyX_ErrorAddTraceback(const char *funcname, const char *filename, int lineno);
PyAPI_FUNC(void) _PyX_ErrorPrint(void);

/* Raise an exception while preserving an existing active exception as its
 * context, matching Python's exception-chaining model. */
PyAPI_FUNC(int) _PyX_ErrorRaiseWithContext(PyObject *exception, PyObject *value);

#ifdef __cplusplus
}
#endif

#endif /* PYTHONX_ERRORS_H */
