#include "Python.h"
#include "pythonx_compile.h"
#include "pythonx_backend.h"

/*
 * Public internal coordinator for the PythonX native compilation path.
 *
 * This intentionally accepts an AST produced by Python's existing frontend.
 * No Python source is translated into C, Rust, assembly, or another language.
 */
PyObject *_PyX_CompileASTNative(mod_ty module)
{
    if (module == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "PythonX native compilation requires an AST module");
        return NULL;
    }
    return _PyX_CompileASTToNative(module);
}
