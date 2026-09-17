#include "Python.h"
#include "pycore_traceback.h"
#include "pythonx_traceback.h"

int
_PyX_TracebackAdd(const char *funcname, const char *filename, int lineno)
{
    if (!PyErr_Occurred()) {
        return 0;
    }

    _PyTraceback_Add(funcname ? funcname : "<pythonx>",
                     filename ? filename : "<pythonx>",
                     lineno);
    return 0;
}

int
_PyX_TracebackAddIfError(const char *funcname,
                         const char *filename,
                         int lineno)
{
    if (!PyErr_Occurred()) {
        return 0;
    }
    return _PyX_TracebackAdd(funcname, filename, lineno);
}
