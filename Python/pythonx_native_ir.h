#ifndef PYTHONX_NATIVE_IR_H
#define PYTHONX_NATIVE_IR_H

#include "Python.h"
#include "pythonx_ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Emit PythonX IR directly as target machine code. */
PyAPI_FUNC(PyObject *) _PyX_NativeCompileIR(const PyXIRFunction *function);
PyAPI_FUNC(PyObject *) _PyX_NativeExecuteIR(PyObject *native_code);

#ifdef __cplusplus
}
#endif

#endif /* PYTHONX_NATIVE_IR_H */
