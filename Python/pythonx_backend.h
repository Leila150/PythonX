#ifndef PYTHONX_BACKEND_H
#define PYTHONX_BACKEND_H

#include "Python.h"
#include "pycore_ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compile an already-parsed Python AST through the PythonX native pipeline. */
PyAPI_FUNC(PyObject *) _PyX_CompileASTToNative(mod_ty module);

#ifdef __cplusplus
}
#endif

#endif /* PYTHONX_BACKEND_H */
