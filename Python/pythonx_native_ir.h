#ifndef PYTHONX_NATIVE_IR_H
#define PYTHONX_NATIVE_IR_H

#include "Python.h"
#include "pythonx_ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compile PythonX IR into executable target machine code. */
PyAPI_FUNC(PyObject *) _PyX_NativeCompileIR(const PyXIRFunction *function);
PyAPI_FUNC(PyObject *) _PyX_NativeExecuteIR(PyObject *native_code);

/* Execute the semantics represented by PythonX IR from the native entrypoint. */
PyAPI_FUNC(PyObject *) _PyX_NativeEvaluateIR(const PyXIRNode *node, PyObject *globals);

#ifdef __cplusplus
}
#endif

#endif /* PYTHONX_NATIVE_IR_H */
