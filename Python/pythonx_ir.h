#ifndef PYTHONX_IR_H
#define PYTHONX_IR_H

#include "Python.h"
#include "pycore_ast.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PYX_IR_CONST = 1,

    /* Python container/literal types. */
    PYX_IR_LIST,
    PYX_IR_TUPLE,
    PYX_IR_SET,
    PYX_IR_DICT,

    /* Operators. */
    PYX_IR_ADD, PYX_IR_SUB, PYX_IR_MUL, PYX_IR_MATMUL,
    PYX_IR_DIV, PYX_IR_FLOORDIV, PYX_IR_MOD, PYX_IR_POW,
    PYX_IR_LSHIFT, PYX_IR_RSHIFT, PYX_IR_BITOR, PYX_IR_BITXOR, PYX_IR_BITAND,
    PYX_IR_INVERT, PYX_IR_POSITIVE, PYX_IR_NEGATIVE,
    PYX_IR_NOT,
    PYX_IR_LT, PYX_IR_LE, PYX_IR_EQ, PYX_IR_NE, PYX_IR_GT, PYX_IR_GE,
    PYX_IR_IS, PYX_IR_IS_NOT, PYX_IR_IN, PYX_IR_NOT_IN,
    PYX_IR_AND, PYX_IR_OR
} PyXIROp;

typedef struct PyXIRNode PyXIRNode;

struct PyXIRNode {
    PyXIROp op;
    PyObject *constant;
    PyXIRNode *left;
    PyXIRNode *right;

    /* Used by variable-sized literals such as list/tuple/set/dict. */
    PyXIRNode **children;
    Py_ssize_t child_count;
};

typedef struct {
    PyXIRNode *root;
} PyXIRFunction;

PyAPI_FUNC(void) _PyX_IR_Free(PyXIRFunction *function);
PyAPI_FUNC(int) _PyX_IR_FromAST(mod_ty module, PyXIRFunction *function);
PyAPI_FUNC(const char *) _PyX_IR_OpName(PyXIROp op);
PyAPI_FUNC(PyObject *) _PyX_IR_Evaluate(const PyXIRNode *node);

#ifdef __cplusplus
}
#endif

#endif /* PYTHONX_IR_H */
