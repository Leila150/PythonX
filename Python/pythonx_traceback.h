#ifndef PYTHONX_TRACEBACK_H
#define PYTHONX_TRACEBACK_H

#include "Python.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Add a real Python traceback frame to the active exception. */
PyAPI_FUNC(int) _PyX_TracebackAdd(const char *funcname,
                                 const char *filename,
                                 int lineno);

/* Add a traceback frame only when an exception is currently active. */
PyAPI_FUNC(int) _PyX_TracebackAddIfError(const char *funcname,
                                         const char *filename,
                                         int lineno);

#ifdef __cplusplus
}
#endif

#endif /* PYTHONX_TRACEBACK_H */
