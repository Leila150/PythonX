#include "Python.h"
#include "pythonx_ir.h"
#include "pythonx_ir_builder.h"

static PyXIRNode *builder_new(PyXIROp op)
{
    PyXIRNode *node = PyMem_Calloc(1, sizeof(*node));
    if (node == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    node->op = op;
    return node;
}

static void builder_free(PyXIRNode *node)
{
    if (node == NULL) {
        return;
    }
    Py_XDECREF(node->constant);
    builder_free(node->left);
    builder_free(node->right);
    if (node->children != NULL) {
        for (Py_ssize_t i = 0; i < node->child_count; i++) {
            builder_free(node->children[i]);
        }
        PyMem_Free(node->children);
    }
    PyMem_Free(node);
}

PyXIRNode *_PyX_IR_NewNode(PyXIROp op)
{
    return builder_new(op);
}

int _PyX_IR_NodeChildren(PyXIRNode *node, Py_ssize_t count)
{
    if (node == NULL || count < 0) {
        PyErr_SetString(PyExc_ValueError, "invalid PythonX IR child count");
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    node->children = PyMem_Calloc((size_t)count, sizeof(*node->children));
    if (node->children == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    node->child_count = count;
    return 0;
}

int _PyX_IR_NodeConstant(PyXIRNode *node, PyObject *value)
{
    if (node == NULL) {
        PyErr_SetString(PyExc_ValueError, "NULL PythonX IR node");
        return -1;
    }
    Py_XSETREF(node->constant, Py_XNewRef(value));
    return 0;
}

PyXIRNode *_PyX_IR_UnaryNode(PyXIROp op, PyXIRNode *operand)
{
    PyXIRNode *node = builder_new(op);
    if (node == NULL) {
        builder_free(operand);
        return NULL;
    }
    node->left = operand;
    return node;
}

PyXIRNode *_PyX_IR_BinaryNode(PyXIROp op, PyXIRNode *left, PyXIRNode *right)
{
    PyXIRNode *node = builder_new(op);
    if (node == NULL) {
        builder_free(left);
        builder_free(right);
        return NULL;
    }
    node->left = left;
    node->right = right;
    return node;
}

PyXIRNode *_PyX_IR_SequenceNode(PyXIROp op, PyXIRNode **items, Py_ssize_t count)
{
    PyXIRNode *node = builder_new(op);
    if (node == NULL) {
        if (items != NULL) {
            for (Py_ssize_t i = 0; i < count; i++) builder_free(items[i]);
            PyMem_Free(items);
        }
        return NULL;
    }
    if (count > 0) {
        node->children = items;
        node->child_count = count;
    }
    return node;
}

static PyXIRNode *structured3(PyXIROp op, PyXIRNode *a, PyXIRNode *b, PyXIRNode *c)
{
    PyXIRNode *node = builder_new(op);
    if (node == NULL) {
        builder_free(a); builder_free(b); builder_free(c);
        return NULL;
    }
    if (_PyX_IR_NodeChildren(node, 3) < 0) {
        builder_free(node); builder_free(a); builder_free(b); builder_free(c);
        return NULL;
    }
    node->children[0] = a;
    node->children[1] = b;
    node->children[2] = c;
    return node;
}

PyXIRNode *_PyX_IR_IfNode(PyXIRNode *condition, PyXIRNode *then_body, PyXIRNode *else_body)
{
    return structured3(PYX_IR_IF, condition, then_body, else_body);
}

PyXIRNode *_PyX_IR_WhileNode(PyXIRNode *condition, PyXIRNode *body, PyXIRNode *else_body)
{
    return structured3(PYX_IR_WHILE, condition, body, else_body);
}

static PyXIRNode *loop_node(PyXIROp op, PyXIRNode *target, PyXIRNode *iterable,
                            PyXIRNode *body, PyXIRNode *else_body)
{
    PyXIRNode *node = builder_new(op);
    if (node == NULL) {
        builder_free(target); builder_free(iterable);
        builder_free(body); builder_free(else_body);
        return NULL;
    }
    if (_PyX_IR_NodeChildren(node, 4) < 0) {
        builder_free(node); builder_free(target); builder_free(iterable);
        builder_free(body); builder_free(else_body);
        return NULL;
    }
    node->children[0] = target;
    node->children[1] = iterable;
    node->children[2] = body;
    node->children[3] = else_body;
    return node;
}

PyXIRNode *_PyX_IR_ForNode(PyXIRNode *target, PyXIRNode *iterable,
                           PyXIRNode *body, PyXIRNode *else_body)
{
    return loop_node(PYX_IR_FOR, target, iterable, body, else_body);
}

PyXIRNode *_PyX_IR_AsyncForNode(PyXIRNode *target, PyXIRNode *iterable,
                                PyXIRNode *body, PyXIRNode *else_body)
{
    return loop_node(PYX_IR_ASYNC_FOR, target, iterable, body, else_body);
}

PyXIRNode *_PyX_IR_BreakNode(void) { return builder_new(PYX_IR_BREAK); }
PyXIRNode *_PyX_IR_ContinueNode(void) { return builder_new(PYX_IR_CONTINUE); }

PyXIRNode *_PyX_IR_ReturnNode(PyXIRNode *value)
{
    return _PyX_IR_UnaryNode(PYX_IR_RETURN, value);
}

static PyXIRNode *named_node(PyXIROp op, PyObject *name, PyXIRNode *body)
{
    PyXIRNode *node = builder_new(op);
    if (node == NULL) {
        builder_free(body);
        return NULL;
    }
    node->constant = Py_NewRef(name);
    node->left = body;
    if (node->constant == NULL && name != Py_None) {
        builder_free(node);
        return NULL;
    }
    return node;
}

PyXIRNode *_PyX_IR_FunctionNode(PyObject *name, PyXIRNode *body)
{
    return named_node(PYX_IR_FUNCTION, name, body);
}

PyXIRNode *_PyX_IR_LambdaNode(PyXIRNode *body)
{
    return _PyX_IR_UnaryNode(PYX_IR_LAMBDA, body);
}

PyXIRNode *_PyX_IR_YieldNode(PyXIRNode *value)
{
    return _PyX_IR_UnaryNode(PYX_IR_YIELD, value);
}

PyXIRNode *_PyX_IR_YieldFromNode(PyXIRNode *value)
{
    return _PyX_IR_UnaryNode(PYX_IR_YIELD_FROM, value);
}

PyXIRNode *_PyX_IR_AwaitNode(PyXIRNode *value)
{
    return _PyX_IR_UnaryNode(PYX_IR_AWAIT, value);
}

PyXIRNode *_PyX_IR_ClassNode(PyObject *name, PyXIRNode *body)
{
    return named_node(PYX_IR_CLASS, name, body);
}

PyXIRNode *_PyX_IR_WithNode(PyXIRNode *items, PyXIRNode *body, int is_async)
{
    return _PyX_IR_BinaryNode(is_async ? PYX_IR_ASYNC_WITH : PYX_IR_WITH, items, body);
}

PyXIRNode *_PyX_IR_ComprehensionNode(PyXIROp op, PyXIRNode **parts, Py_ssize_t count)
{
    return _PyX_IR_SequenceNode(op, parts, count);
}

PyXIRNode *_PyX_IR_ImportNode(PyXIROp op, PyObject *name, PyXIRNode *payload)
{
    return named_node(op, name, payload);
}

PyXIRNode *_PyX_IR_MatchNode(PyXIRNode *subject, PyXIRNode *cases)
{
    return _PyX_IR_BinaryNode(PYX_IR_MATCH, subject, cases);
}
