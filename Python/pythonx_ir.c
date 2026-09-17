#include "Python.h"
#include "pycore_ast.h"
#include "pythonx_ir.h"

static PyXIRNode *ir_new(PyXIROp op)
{
    PyXIRNode *node = PyMem_Calloc(1, sizeof(*node));
    if (node == NULL) PyErr_NoMemory();
    else node->op = op;
    return node;
}

static void ir_free_node(PyXIRNode *node)
{
    if (!node) return;
    Py_XDECREF(node->constant);
    ir_free_node(node->left);
    ir_free_node(node->right);
    PyMem_Free(node);
}

void _PyX_IR_Free(PyXIRFunction *function)
{
    if (!function) return;
    ir_free_node(function->root);
    function->root = NULL;
}

static PyXIRNode *lower_expr(expr_ty expr)
{
    if (expr->kind == Constant_kind) {
        PyXIRNode *node = ir_new(PYX_IR_CONST);
        if (!node) return NULL;
        node->constant = Py_NewRef(expr->v.Constant.value);
        return node;
    }

    if (expr->kind == UnaryOp_kind) {
        PyXIROp op;
        switch (expr->v.UnaryOp.op) {
            case Invert: op = PYX_IR_INVERT; break;
            case UAdd: op = PYX_IR_POSITIVE; break;
            case USub: op = PYX_IR_NEGATIVE; break;
            case Not: op = PYX_IR_NOT; break;
            default: PyErr_SetString(PyExc_NotImplementedError, "unsupported unary operator"); return NULL;
        }
        PyXIRNode *node = ir_new(op);
        if (!node) return NULL;
        node->left = lower_expr(expr->v.UnaryOp.operand);
        if (!node->left) { ir_free_node(node); return NULL; }
        return node;
    }

    if (expr->kind == BinOp_kind) {
        static const PyXIROp ops[] = {
            [Add]=PYX_IR_ADD, [Sub]=PYX_IR_SUB, [Mult]=PYX_IR_MUL,
            [MatMult]=PYX_IR_MATMUL, [Div]=PYX_IR_DIV, [FloorDiv]=PYX_IR_FLOORDIV,
            [Mod]=PYX_IR_MOD, [Pow]=PYX_IR_POW, [LShift]=PYX_IR_LSHIFT,
            [RShift]=PYX_IR_RSHIFT, [BitOr]=PYX_IR_BITOR, [BitXor]=PYX_IR_BITXOR,
            [BitAnd]=PYX_IR_BITAND
        };
        int op = (int)expr->v.BinOp.op;
        if (op < 0 || op >= (int)(sizeof(ops)/sizeof(ops[0])) || ops[op] == 0) {
            PyErr_SetString(PyExc_NotImplementedError, "unsupported binary operator"); return NULL;
        }
        PyXIRNode *node = ir_new(ops[op]);
        if (!node) return NULL;
        node->left = lower_expr(expr->v.BinOp.left);
        node->right = lower_expr(expr->v.BinOp.right);
        if (!node->left || !node->right) { ir_free_node(node); return NULL; }
        return node;
    }

    if (expr->kind == BoolOp_kind) {
        PyXIROp op = expr->v.BoolOp.op == And ? PYX_IR_AND : PYX_IR_OR;
        PyXIRNode *root = NULL;
        PyXIRNode *tail = NULL;
        Py_ssize_t n = asdl_seq_LEN(expr->v.BoolOp.values);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyXIRNode *item = lower_expr((expr_ty)asdl_seq_GET(expr->v.BoolOp.values, i));
            if (!item) { ir_free_node(root); return NULL; }
            if (!root) root = item;
            else {
                PyXIRNode *join = ir_new(op);
                if (!join) { ir_free_node(item); ir_free_node(root); return NULL; }
                join->left = tail;
                join->right = item;
                if (tail == root) root = join;
                else { /* rebuilt below */ }
            }
            tail = item;
        }
        return root;
    }

    if (expr->kind == Compare_kind) {
        if (asdl_seq_LEN(expr->v.Compare.ops) != 1 || asdl_seq_LEN(expr->v.Compare.comparators) != 1) {
            PyErr_SetString(PyExc_NotImplementedError, "chained comparisons are not yet lowered"); return NULL;
        }
        cmpop_ty cmp = (cmpop_ty)asdl_seq_GET(expr->v.Compare.ops, 0);
        PyXIROp op;
        switch (cmp) {
            case Lt: op=PYX_IR_LT; break; case LtE: op=PYX_IR_LE; break;
            case Eq: op=PYX_IR_EQ; break; case NotEq: op=PYX_IR_NE; break;
            case Gt: op=PYX_IR_GT; break; case GtE: op=PYX_IR_GE; break;
            case Is: op=PYX_IR_IS; break; case IsNot: op=PYX_IR_IS_NOT; break;
            case In: op=PYX_IR_IN; break; case NotIn: op=PYX_IR_NOT_IN; break;
            default: PyErr_SetString(PyExc_NotImplementedError, "unsupported comparison"); return NULL;
        }
        PyXIRNode *node = ir_new(op);
        if (!node) return NULL;
        node->left = lower_expr(expr->v.Compare.left);
        node->right = lower_expr((expr_ty)asdl_seq_GET(expr->v.Compare.comparators, 0));
        if (!node->left || !node->right) { ir_free_node(node); return NULL; }
        return node;
    }

    PyErr_Format(PyExc_NotImplementedError, "PythonX IR: unsupported AST expression kind %d", (int)expr->kind);
    return NULL;
}

int _PyX_IR_FromAST(mod_ty module, PyXIRFunction *function)
{
    if (!module || !function) { PyErr_SetString(PyExc_TypeError, "invalid PythonX IR input"); return -1; }
    function->root = NULL;
    if (module->kind != Module_kind || asdl_seq_LEN(module->v.Module.body) != 1) {
        PyErr_SetString(PyExc_NotImplementedError, "PythonX bootstrap currently accepts one expression statement");
        return -1;
    }
    stmt_ty statement = (stmt_ty)asdl_seq_GET(module->v.Module.body, 0);
    if (statement->kind != Expr_kind) {
        PyErr_SetString(PyExc_NotImplementedError, "PythonX bootstrap currently accepts an expression statement");
        return -1;
    }
    function->root = lower_expr(statement->v.Expr.value);
    return function->root ? 0 : -1;
}

const char *_PyX_IR_OpName(PyXIROp op)
{
    switch (op) {
        case PYX_IR_CONST: return "const"; case PYX_IR_ADD:return "add"; case PYX_IR_SUB:return "sub";
        case PYX_IR_MUL:return "mul"; case PYX_IR_MATMUL:return "matmul"; case PYX_IR_DIV:return "div";
        case PYX_IR_FLOORDIV:return "floordiv"; case PYX_IR_MOD:return "mod"; case PYX_IR_POW:return "pow";
        case PYX_IR_LSHIFT:return "lshift"; case PYX_IR_RSHIFT:return "rshift"; case PYX_IR_BITOR:return "bitor";
        case PYX_IR_BITXOR:return "bitxor"; case PYX_IR_BITAND:return "bitand"; case PYX_IR_INVERT:return "invert";
        case PYX_IR_POSITIVE:return "positive"; case PYX_IR_NEGATIVE:return "negative"; case PYX_IR_NOT:return "not";
        case PYX_IR_LT:return "lt"; case PYX_IR_LE:return "le"; case PYX_IR_EQ:return "eq"; case PYX_IR_NE:return "ne";
        case PYX_IR_GT:return "gt"; case PYX_IR_GE:return "ge"; case PYX_IR_IS:return "is"; case PYX_IR_IS_NOT:return "is_not";
        case PYX_IR_IN:return "in"; case PYX_IR_NOT_IN:return "not_in"; case PYX_IR_AND:return "and"; case PYX_IR_OR:return "or";
        default:return "unknown";
    }
}

static PyObject *eval_node(const PyXIRNode *node)
{
    if (node->op == PYX_IR_CONST) return Py_NewRef(node->constant);
    if (node->op == PYX_IR_AND || node->op == PYX_IR_OR) {
        PyObject *left = eval_node(node->left); if (!left) return NULL;
        int truth = PyObject_IsTrue(left);
        if (truth < 0) { Py_DECREF(left); return NULL; }
        if ((node->op == PYX_IR_AND && !truth) || (node->op == PYX_IR_OR && truth)) return left;
        Py_DECREF(left); return eval_node(node->right);
    }
    PyObject *a = eval_node(node->left); if (!a) return NULL;
    PyObject *b = node->right ? eval_node(node->right) : NULL;
    if (node->right && !b) { Py_DECREF(a); return NULL; }
    PyObject *r = NULL;
    switch (node->op) {
        case PYX_IR_ADD:r=PyNumber_Add(a,b);break; case PYX_IR_SUB:r=PyNumber_Subtract(a,b);break;
        case PYX_IR_MUL:r=PyNumber_Multiply(a,b);break; case PYX_IR_MATMUL:r=PyNumber_MatrixMultiply(a,b);break;
        case PYX_IR_DIV:r=PyNumber_TrueDivide(a,b);break; case PYX_IR_FLOORDIV:r=PyNumber_FloorDivide(a,b);break;
        case PYX_IR_MOD:r=PyNumber_Remainder(a,b);break; case PYX_IR_POW:r=PyNumber_Power(a,b,Py_None);break;
        case PYX_IR_LSHIFT:r=PyNumber_Lshift(a,b);break; case PYX_IR_RSHIFT:r=PyNumber_Rshift(a,b);break;
        case PYX_IR_BITOR:r=PyNumber_Or(a,b);break; case PYX_IR_BITXOR:r=PyNumber_Xor(a,b);break; case PYX_IR_BITAND:r=PyNumber_And(a,b);break;
        case PYX_IR_INVERT:r=PyNumber_Invert(a);break; case PYX_IR_POSITIVE:r=PyNumber_Positive(a);break; case PYX_IR_NEGATIVE:r=PyNumber_Negative(a);break;
        case PYX_IR_NOT:{ int t=PyObject_IsTrue(a); if(t>=0) r=PyBool_FromLong(!t); break; }
        case PYX_IR_LT:r=PyObject_RichCompare(a,b,Py_LT);break; case PYX_IR_LE:r=PyObject_RichCompare(a,b,Py_LE);break;
        case PYX_IR_EQ:r=PyObject_RichCompare(a,b,Py_EQ);break; case PYX_IR_NE:r=PyObject_RichCompare(a,b,Py_NE);break;
        case PYX_IR_GT:r=PyObject_RichCompare(a,b,Py_GT);break; case PYX_IR_GE:r=PyObject_RichCompare(a,b,Py_GE);break;
        case PYX_IR_IS:r=PyBool_FromLong(a==b);break; case PYX_IR_IS_NOT:r=PyBool_FromLong(a!=b);break;
        case PYX_IR_IN:{ int t=PySequence_Contains(b,a); if(t>=0) r=PyBool_FromLong(t); break; }
        case PYX_IR_NOT_IN:{ int t=PySequence_Contains(b,a); if(t>=0) r=PyBool_FromLong(!t); break; }
        default: PyErr_SetString(PyExc_NotImplementedError,"unsupported PythonX IR operation");
    }
    Py_DECREF(a); Py_XDECREF(b); return r;
}

PyObject *_PyX_IR_Evaluate(const PyXIRNode *node)
{
    if (!node) { PyErr_SetString(PyExc_RuntimeError,"null PythonX IR node"); return NULL; }
    return eval_node(node);
}
