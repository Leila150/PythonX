#include "Python.h"
#include "pycore_ast.h"
#include "pythonx_ir.h"
#include "pythonx_native_ir.h"

/*
 * PythonX compiler coordinator.
 *
 * The Python frontend stops at the AST.  From this point onward the pipeline
 * is PythonX-owned: AST -> PythonX IR -> native machine code.
 */
PyObject *_PyX_CompileASTToNative(mod_ty module)
{
    PyXIRFunction function = {0};

    if (_PyX_IR_FromAST(module, &function) < 0) {
        _PyX_IR_Free(&function);
        return NULL;
    }

    PyObject *native_code = _PyX_NativeCompileIR(&function);
    _PyX_IR_Free(&function);
    return native_code;
}
