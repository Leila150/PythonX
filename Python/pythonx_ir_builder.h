#ifndef PYTHONX_IR_BUILDER_H
#define PYTHONX_IR_BUILDER_H

#include "Python.h"
#include "pythonx_ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generic IR construction. */
PyAPI_FUNC(PyXIRNode *) _PyX_IR_NewNode(PyXIROp op);
PyAPI_FUNC(int) _PyX_IR_NodeChildren(PyXIRNode *node, Py_ssize_t count);
PyAPI_FUNC(int) _PyX_IR_NodeConstant(PyXIRNode *node, PyObject *value);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_UnaryNode(PyXIROp op, PyXIRNode *operand);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_BinaryNode(PyXIROp op, PyXIRNode *left, PyXIRNode *right);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_SequenceNode(PyXIROp op, PyXIRNode **items, Py_ssize_t count);

/* Structured control flow. */
PyAPI_FUNC(PyXIRNode *) _PyX_IR_IfNode(PyXIRNode *condition, PyXIRNode *then_body, PyXIRNode *else_body);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_WhileNode(PyXIRNode *condition, PyXIRNode *body, PyXIRNode *else_body);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_ForNode(PyXIRNode *target, PyXIRNode *iterable, PyXIRNode *body, PyXIRNode *else_body);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_AsyncForNode(PyXIRNode *target, PyXIRNode *iterable, PyXIRNode *body, PyXIRNode *else_body);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_BreakNode(void);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_ContinueNode(void);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_ReturnNode(PyXIRNode *value);

/* Functions, generators, classes and context managers. */
PyAPI_FUNC(PyXIRNode *) _PyX_IR_FunctionNode(PyObject *name, PyXIRNode *body);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_LambdaNode(PyXIRNode *body);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_YieldNode(PyXIRNode *value);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_YieldFromNode(PyXIRNode *value);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_AwaitNode(PyXIRNode *value);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_ClassNode(PyObject *name, PyXIRNode *body);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_WithNode(PyXIRNode *items, PyXIRNode *body, int is_async);

/* Comprehensions, imports and structural pattern matching. */
PyAPI_FUNC(PyXIRNode *) _PyX_IR_ComprehensionNode(PyXIROp op, PyXIRNode **parts, Py_ssize_t count);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_ImportNode(PyXIROp op, PyObject *name, PyXIRNode *payload);
PyAPI_FUNC(PyXIRNode *) _PyX_IR_MatchNode(PyXIRNode *subject, PyXIRNode *cases);

#ifdef __cplusplus
}
#endif

#endif /* PYTHONX_IR_BUILDER_H */
