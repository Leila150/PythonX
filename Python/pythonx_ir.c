#include "Python.h"
#include "pycore_ast.h"
#include "pythonx_ir.h"

#include <stdint.h>

static PyXIRNode *ir_new(PyXIROp op)
{
    PyXIRNode *node = PyMem_Calloc(1, sizeof(*node));
    if (node == NULL) { PyErr_NoMemory(); return NULL; }
    node->op = op;
    return node;
}

static void ir_free_node(PyXIRNode *node)
{
    if (node == NULL) return;
    ir_free_node(node->left);
    ir_free_node(node->right);
    PyMem_Free(node);
}

void _PyX_IR_Free(PyXIRFunction *function)
{
    if (function == NULL) return;
    ir_free_node(function->root);
    function->root = NULL;
}

static int constant_int(expr_ty expr, int64_t *value)
{
    if (expr->kind != Constant_kind || !PyLong_Check(expr->v.Constant.value)) return 0;
    int overflow = 0;
    long long converted = PyLong_AsLongLongAndOverflow(expr->v.Constant.value, &overflow);
    if (converted == -1 && PyErr_Occurred()) return -1;
    if (overflow != 0) return 0;
    *value = (int64_t)converted;
    return 1;
}

static PyXIRNode *lower_expr(expr_ty expr)
{
    int64_t value;
    int result = constant_int(expr, &value);
    if (result < 0) return NULL;
    if (result) {
        PyXIRNode *node = ir_new(PYX_IR_CONST_INT);
        if (node != NULL) node->value = value;
        return node;
    }

    if (expr->kind != BinOp_kind) {
        PyErr_Format(PyExc_NotImplementedError,
                     "PythonX IR: unsupported AST expression kind %d", (int)expr->kind);
        return NULL;
    }

    PyXIROp op;
    switch (expr->v.BinOp.op) {
        case Add: op = PYX_IR_ADD; break;
        case Sub: op = PYX_IR_SUB; break;
        case Mult: op = PYX_IR_MUL; break;
        default:
            PyErr_Format(PyExc_NotImplementedError,
                         "PythonX IR: unsupported binary operator %d",
                         (int)expr->v.BinOp.op);
            return NULL;
    }

    PyXIRNode *node = ir_new(op);
    if (node == NULL) return NULL;
    node->left = lower_expr(expr->v.BinOp.left);
    if (node->left == NULL) { ir_free_node(node); return NULL; }
    node->right = lower_expr(expr->v.BinOp.right);
    if (node->right == NULL) { ir_free_node(node); return NULL; }
    return node;
}

int _PyX_IR_FromAST(mod_ty module, PyXIRFunction *function)
{
    if (module == NULL || function == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "PythonX IR lowering requires an AST module and output function");
        return -1;
    }
    function->root = NULL;
    if (module->kind != Module_kind || asdl_seq_LEN(module->v.Module.body) != 1) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "PythonX IR bootstrap requires exactly one module statement");
        return -1;
    }
    stmt_ty statement = (stmt_ty)asdl_seq_GET(module->v.Module.body, 0);
    if (statement->kind != Expr_kind) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "PythonX IR bootstrap requires an expression statement");
        return -1;
    }
    function->root = lower_expr(statement->v.Expr.value);
    return function->root == NULL ? -1 : 0;
}

const char *_PyX_IR_OpName(PyXIROp op)
{
    switch (op) {
        case PYX_IR_CONST_INT: return "const_int";
        case PYX_IR_ADD: return "add";
        case PYX_IR_SUB: return "sub";
        case PYX_IR_MUL: return "mul";
        default: return "unknown";
    }
}
