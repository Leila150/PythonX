#ifndef PYTHONX_COMPILE_H
#define PYTHONX_COMPILE_H

#include "Python.h"
#include "pycore_ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PythonX native compilation entry point.
 *
 * The Python frontend has already produced the AST. PythonX owns everything
 * after that point: AST -> PythonX IR -> native machine code.
 */
PyAPI_FUNC(PyObject *) _PyX_CompileASTNative(mod_ty module);

#ifdef __cplusplus
}
#endif

#endif /* PYTHONX_COMPILE_H */
