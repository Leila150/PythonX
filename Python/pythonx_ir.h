#ifndef PYTHONX_IR_H
#define PYTHONX_IR_H

#include "Python.h"
#include "pycore_ast.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PYX_IR_CONST_INT = 1,
    PYX_IR_ADD,
    PYX_IR_SUB,
    PYX_IR_MUL
} PyXIROp;

typedef struct PyXIRNode PyXIRNode;

struct PyXIRNode {
    PyXIROp op;
    int64_t value;
    PyXIRNode *left;
    PyXIRNode *right;
};

typedef struct {
    PyXIRNode *root;
} PyXIRFunction;

PyAPI_FUNC(void) _PyX_IR_Free(PyXIRFunction *function);
PyAPI_FUNC(int) _PyX_IR_FromAST(mod_ty module, PyXIRFunction *function);
PyAPI_FUNC(const char *) _PyX_IR_OpName(PyXIROp op);

#ifdef __cplusplus
}
#endif

#endif /* PYTHONX_IR_H */
