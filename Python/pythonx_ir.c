#include "Python.h"
#include "pycore_ast.h"
#include "pythonx_ir.h"

static PyXIRNode *ir_new(PyXIROp op)
{
    PyXIRNode *node = PyMem_Calloc(1, sizeof(*node));
    if (!node) PyErr_NoMemory();
    else node->op = op;
    return node;
}

static void ir_free_node(PyXIRNode *node)
{
    if (!node) return;
    Py_XDECREF(node->constant);
    ir_free_node(node->left);
    ir_free_node(node->right);
    if (node->children) {
        for (Py_ssize_t i = 0; i < node->child_count; i++) ir_free_node(node->children[i]);
        PyMem_Free(node->children);
    }
    PyMem_Free(node);
}

void _PyX_IR_Free(PyXIRFunction *function)
{
    if (!function) return;
    ir_free_node(function->root);
    function->root = NULL;
}

static int ir_set_children(PyXIRNode *node, Py_ssize_t count)
{
    if (count == 0) return 0;
    node->children = PyMem_Calloc((size_t)count, sizeof(*node->children));
    if (!node->children) { PyErr_NoMemory(); return -1; }
    node->child_count = count;
    return 0;
}

static PyXIRNode *lower_expr(expr_ty expr);

static PyXIRNode *lower_sequence(PyXIROp op, asdl_expr_seq *values)
{
    Py_ssize_t count = asdl_seq_LEN(values);
    PyXIRNode *node = ir_new(op);
    if (!node) return NULL;
    if (ir_set_children(node, count) < 0) { ir_free_node(node); return NULL; }
    for (Py_ssize_t i = 0; i < count; i++) {
        node->children[i] = lower_expr((expr_ty)asdl_seq_GET(values, i));
        if (!node->children[i]) { ir_free_node(node); return NULL; }
    }
    return node;
}

static PyXIRNode *lower_dict(expr_ty expr)
{
    asdl_expr_seq *keys = expr->v.Dict.keys;
    asdl_expr_seq *values = expr->v.Dict.values;
    Py_ssize_t count = asdl_seq_LEN(keys);
    PyXIRNode *node = ir_new(PYX_IR_DICT);
    if (!node) return NULL;
    if (ir_set_children(node, count * 2) < 0) { ir_free_node(node); return NULL; }
    for (Py_ssize_t i = 0; i < count; i++) {
        expr_ty key = (expr_ty)asdl_seq_GET(keys, i);
        if (!key) {
            PyErr_SetString(PyExc_NotImplementedError, "PythonX: dictionary unpacking is not yet lowered");
            ir_free_node(node); return NULL;
        }
        node->children[i * 2] = lower_expr(key);
        node->children[i * 2 + 1] = lower_expr((expr_ty)asdl_seq_GET(values, i));
        if (!node->children[i * 2] || !node->children[i * 2 + 1]) { ir_free_node(node); return NULL; }
    }
    return node;
}

static PyXIRNode *lower_name(expr_ty expr, PyXIROp op)
{
    PyXIRNode *node = ir_new(op);
    if (!node) return NULL;
    node->constant = PyUnicode_FromString(expr->v.Name.id);
    if (!node->constant) { ir_free_node(node); return NULL; }
    return node;
}

static PyXIRNode *lower_none_constant(void)
{
    PyXIRNode *node = ir_new(PYX_IR_CONST);
    if (!node) return NULL;
    node->constant = Py_NewRef(Py_None);
    return node;
}

static PyXIRNode *lower_slice(expr_ty expr)
{
    PyXIRNode *node = ir_new(PYX_IR_SEQUENCE);
    if (!node) return NULL;
    if (ir_set_children(node, 4) < 0) { ir_free_node(node); return NULL; }
    node->children[0] = ir_new(PYX_IR_CONST);
    if (!node->children[0]) { ir_free_node(node); return NULL; }
    node->children[0]->constant = PyUnicode_FromString("__pythonx_slice__");
    node->children[1] = expr->v.Slice.lower ? lower_expr(expr->v.Slice.lower) : lower_none_constant();
    node->children[2] = expr->v.Slice.upper ? lower_expr(expr->v.Slice.upper) : lower_none_constant();
    node->children[3] = expr->v.Slice.step ? lower_expr(expr->v.Slice.step) : lower_none_constant();
    if (!node->children[0]->constant || !node->children[1] || !node->children[2] || !node->children[3]) {
        ir_free_node(node); return NULL;
    }
    return node;
}

static PyXIRNode *lower_subscript(expr_ty expr)
{
    PyXIRNode *node = ir_new(PYX_IR_SUBSCRIPT);
    if (!node) return NULL;
    if (ir_set_children(node, 2) < 0) { ir_free_node(node); return NULL; }
    node->children[0] = lower_expr(expr->v.Subscript.value);
    node->children[1] = expr->v.Subscript.slice->kind == Slice_kind
        ? lower_slice(expr->v.Subscript.slice)
        : lower_expr(expr->v.Subscript.slice);
    if (!node->children[0] || !node->children[1]) { ir_free_node(node); return NULL; }
    return node;
}

static PyXIRNode *lower_attribute(expr_ty expr)
{
    PyXIRNode *node = ir_new(PYX_IR_GETATTR);
    if (!node) return NULL;
    node->constant = PyUnicode_FromString(expr->v.Attribute.attr);
    if (!node->constant) { ir_free_node(node); return NULL; }
    node->left = lower_expr(expr->v.Attribute.value);
    if (!node->left) { ir_free_node(node); return NULL; }
    return node;
}

static PyXIRNode *lower_expr(expr_ty expr)
{
    switch (expr->kind) {
        case Constant_kind: {
            PyXIRNode *node = ir_new(PYX_IR_CONST);
            if (!node) return NULL;
            node->constant = Py_NewRef(expr->v.Constant.value);
            return node;
        }
        case Name_kind: return lower_name(expr, PYX_IR_NAME_LOAD);
        case List_kind: return lower_sequence(PYX_IR_LIST, expr->v.List.elts);
        case Tuple_kind: return lower_sequence(PYX_IR_TUPLE, expr->v.Tuple.elts);
        case Set_kind: return lower_sequence(PYX_IR_SET, expr->v.Set.elts);
        case Dict_kind: return lower_dict(expr);
        case Attribute_kind: return lower_attribute(expr);
        case Subscript_kind: return lower_subscript(expr);
        case Slice_kind: return lower_slice(expr);
        case UnaryOp_kind: {
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
        case BinOp_kind: {
            PyXIROp op;
            switch (expr->v.BinOp.op) {
                case Add: op = PYX_IR_ADD; break;
                case Sub: op = PYX_IR_SUB; break;
                case Mult: op = PYX_IR_MUL; break;
                case MatMult: op = PYX_IR_MATMUL; break;
                case Div: op = PYX_IR_DIV; break;
                case FloorDiv: op = PYX_IR_FLOORDIV; break;
                case Mod: op = PYX_IR_MOD; break;
                case Pow: op = PYX_IR_POW; break;
                case LShift: op = PYX_IR_LSHIFT; break;
                case RShift: op = PYX_IR_RSHIFT; break;
                case BitOr: op = PYX_IR_BITOR; break;
                case BitXor: op = PYX_IR_BITXOR; break;
                case BitAnd: op = PYX_IR_BITAND; break;
                default: PyErr_SetString(PyExc_NotImplementedError, "unsupported binary operator"); return NULL;
            }
            PyXIRNode *node = ir_new(op);
            if (!node) return NULL;
            node->left = lower_expr(expr->v.BinOp.left);
            node->right = lower_expr(expr->v.BinOp.right);
            if (!node->left || !node->right) { ir_free_node(node); return NULL; }
            return node;
        }
        case BoolOp_kind: {
            PyXIROp op = expr->v.BoolOp.op == And ? PYX_IR_AND : PYX_IR_OR;
            Py_ssize_t n = asdl_seq_LEN(expr->v.BoolOp.values);
            if (n < 1) { PyErr_SetString(PyExc_SyntaxError, "empty boolean expression"); return NULL; }
            PyXIRNode *root = lower_expr((expr_ty)asdl_seq_GET(expr->v.BoolOp.values, 0));
            if (!root) return NULL;
            for (Py_ssize_t i = 1; i < n; i++) {
                PyXIRNode *item = lower_expr((expr_ty)asdl_seq_GET(expr->v.BoolOp.values, i));
                if (!item) { ir_free_node(root); return NULL; }
                PyXIRNode *join = ir_new(op);
                if (!join) { ir_free_node(item); ir_free_node(root); return NULL; }
                join->left = root;
                join->right = item;
                root = join;
            }
            return root;
        }
        case Compare_kind: {
            if (asdl_seq_LEN(expr->v.Compare.ops) != 1 || asdl_seq_LEN(expr->v.Compare.comparators) != 1) {
                PyErr_SetString(PyExc_NotImplementedError, "chained comparisons are not yet lowered"); return NULL;
            }
            PyXIROp op;
            switch ((cmpop_ty)asdl_seq_GET(expr->v.Compare.ops, 0)) {
                case Lt: op = PYX_IR_LT; break;
                case LtE: op = PYX_IR_LE; break;
                case Eq: op = PYX_IR_EQ; break;
                case NotEq: op = PYX_IR_NE; break;
                case Gt: op = PYX_IR_GT; break;
                case GtE: op = PYX_IR_GE; break;
                case Is: op = PYX_IR_IS; break;
                case IsNot: op = PYX_IR_IS_NOT; break;
                case In: op = PYX_IR_IN; break;
                case NotIn: op = PYX_IR_NOT_IN; break;
                default: PyErr_SetString(PyExc_NotImplementedError, "unsupported comparison"); return NULL;
            }
            PyXIRNode *node = ir_new(op);
            if (!node) return NULL;
            node->left = lower_expr(expr->v.Compare.left);
            node->right = lower_expr((expr_ty)asdl_seq_GET(expr->v.Compare.comparators, 0));
            if (!node->left || !node->right) { ir_free_node(node); return NULL; }
            return node;
        }
        default:
            PyErr_Format(PyExc_NotImplementedError, "PythonX IR: unsupported AST expression kind %d", (int)expr->kind);
            return NULL;
    }
}

static PyXIRNode *lower_store(expr_ty target, expr_ty value)
{
    if (target->kind == Name_kind) {
        PyXIRNode *node = lower_name(target, PYX_IR_NAME_STORE);
        if (!node) return NULL;
        node->left = lower_expr(value);
        if (!node->left) { ir_free_node(node); return NULL; }
        return node;
    }
    if (target->kind == Subscript_kind) {
        PyXIRNode *node = ir_new(PYX_IR_SUBSCRIPT_STORE);
        if (!node) return NULL;
        if (ir_set_children(node, 3) < 0) { ir_free_node(node); return NULL; }
        node->children[0] = lower_expr(target->v.Subscript.value);
        node->children[1] = target->v.Subscript.slice->kind == Slice_kind
            ? lower_slice(target->v.Subscript.slice)
            : lower_expr(target->v.Subscript.slice);
        node->children[2] = lower_expr(value);
        if (!node->children[0] || !node->children[1] || !node->children[2]) { ir_free_node(node); return NULL; }
        return node;
    }
    if (target->kind == Attribute_kind) {
        PyXIRNode *node = ir_new(PYX_IR_SETATTR);
        if (!node) return NULL;
        node->constant = PyUnicode_FromString(target->v.Attribute.attr);
        if (!node->constant) { ir_free_node(node); return NULL; }
        node->left = lower_expr(target->v.Attribute.value);
        node->right = lower_expr(value);
        if (!node->left || !node->right) { ir_free_node(node); return NULL; }
        return node;
    }
    PyErr_SetString(PyExc_NotImplementedError, "PythonX: unsupported assignment target");
    return NULL;
}

static PyXIRNode *lower_statement(stmt_ty statement)
{
    if (statement->kind == Expr_kind) return lower_expr(statement->v.Expr.value);
    if (statement->kind == Assign_kind) {
        Py_ssize_t count = asdl_seq_LEN(statement->v.Assign.targets);
        PyXIRNode *sequence = ir_new(PYX_IR_SEQUENCE);
        if (!sequence) return NULL;
        if (ir_set_children(sequence, count) < 0) { ir_free_node(sequence); return NULL; }
        for (Py_ssize_t i = 0; i < count; i++) {
            sequence->children[i] = lower_store((expr_ty)asdl_seq_GET(statement->v.Assign.targets, i), statement->v.Assign.value);
            if (!sequence->children[i]) { ir_free_node(sequence); return NULL; }
        }
        return sequence;
    }
    if (statement->kind == AnnAssign_kind) {
        if (!statement->v.AnnAssign.value) {
            PyErr_SetString(PyExc_NotImplementedError, "PythonX: annotated declarations without a value are not yet supported");
            return NULL;
        }
        return lower_store(statement->v.AnnAssign.target, statement->v.AnnAssign.value);
    }
    PyErr_Format(PyExc_NotImplementedError, "PythonX IR: unsupported statement kind %d", (int)statement->kind);
    return NULL;
}

int _PyX_IR_FromAST(mod_ty module, PyXIRFunction *function)
{
    if (!module || !function) { PyErr_SetString(PyExc_TypeError, "invalid PythonX IR input"); return -1; }
    function->root = NULL;
    if (module->kind != Module_kind || asdl_seq_LEN(module->v.Module.body) < 1) {
        PyErr_SetString(PyExc_NotImplementedError, "PythonX requires a non-empty module during bootstrap");
        return -1;
    }
    Py_ssize_t count = asdl_seq_LEN(module->v.Module.body);
    PyXIRNode *root = ir_new(PYX_IR_SEQUENCE);
    if (!root) return -1;
    if (ir_set_children(root, count) < 0) { ir_free_node(root); return -1; }
    for (Py_ssize_t i = 0; i < count; i++) {
        root->children[i] = lower_statement((stmt_ty)asdl_seq_GET(module->v.Module.body, i));
        if (!root->children[i]) { ir_free_node(root); return -1; }
    }
    function->root = root;
    return 0;
}

const char *_PyX_IR_OpName(PyXIROp op)
{
    switch (op) {
        case PYX_IR_CONST: return "const";
        case PYX_IR_LIST: return "list";
        case PYX_IR_TUPLE: return "tuple";
        case PYX_IR_SET: return "set";
        case PYX_IR_DICT: return "dict";
        case PYX_IR_NAME_LOAD: return "name_load";
        case PYX_IR_NAME_STORE: return "name_store";
        case PYX_IR_SEQUENCE: return "sequence";
        case PYX_IR_SUBSCRIPT: return "subscript";
        case PYX_IR_SUBSCRIPT_STORE: return "subscript_store";
        case PYX_IR_GETATTR: return "getattr";
        case PYX_IR_SETATTR: return "setattr";
        case PYX_IR_ADD: return "add";
        case PYX_IR_SUB: return "sub";
        case PYX_IR_MUL: return "mul";
        case PYX_IR_MATMUL: return "matmul";
        case PYX_IR_DIV: return "div";
        case PYX_IR_FLOORDIV: return "floordiv";
        case PYX_IR_MOD: return "mod";
        case PYX_IR_POW: return "pow";
        case PYX_IR_LSHIFT: return "lshift";
        case PYX_IR_RSHIFT: return "rshift";
        case PYX_IR_BITOR: return "bitor";
        case PYX_IR_BITXOR: return "bitxor";
        case PYX_IR_BITAND: return "bitand";
        case PYX_IR_INVERT: return "invert";
        case PYX_IR_POSITIVE: return "positive";
        case PYX_IR_NEGATIVE: return "negative";
        case PYX_IR_NOT: return "not";
        case PYX_IR_LT: return "lt";
        case PYX_IR_LE: return "le";
        case PYX_IR_EQ: return "eq";
        case PYX_IR_NE: return "ne";
        case PYX_IR_GT: return "gt";
        case PYX_IR_GE: return "ge";
        case PYX_IR_IS: return "is";
        case PYX_IR_IS_NOT: return "is_not";
        case PYX_IR_IN: return "in";
        case PYX_IR_NOT_IN: return "not_in";
        case PYX_IR_AND: return "and";
        case PYX_IR_OR: return "or";
        default: return "unknown";
    }
}

static PyObject *eval_node(const PyXIRNode *node, PyObject *globals);

static int is_slice_node(const PyXIRNode *node)
{
    return node && node->op == PYX_IR_SEQUENCE && node->child_count == 4 &&
           node->children[0]->op == PYX_IR_CONST &&
           PyUnicode_Check(node->children[0]->constant) &&
           PyUnicode_CompareWithASCIIString(node->children[0]->constant, "__pythonx_slice__") == 0;
}

static PyObject *eval_slice(const PyXIRNode *node, PyObject *globals)
{
    PyObject *lower = eval_node(node->children[1], globals);
    PyObject *upper = eval_node(node->children[2], globals);
    PyObject *step = eval_node(node->children[3], globals);
    if (!lower || !upper || !step) { Py_XDECREF(lower); Py_XDECREF(upper); Py_XDECREF(step); return NULL; }
    PyObject *result = PySlice_New(lower, upper, step);
    Py_DECREF(lower); Py_DECREF(upper); Py_DECREF(step);
    return result;
}

static PyObject *eval_node(const PyXIRNode *node, PyObject *globals)
{
    if (node->op == PYX_IR_CONST) return Py_NewRef(node->constant);

    if (node->op == PYX_IR_NAME_LOAD) {
        PyObject *value = PyDict_GetItemWithError(globals, node->constant);
        if (!value) {
            if (PyErr_Occurred()) return NULL;
            PyErr_Format(PyExc_NameError, "name '%U' is not defined", node->constant);
            return NULL;
        }
        return Py_NewRef(value);
    }

    if (node->op == PYX_IR_NAME_STORE) {
        PyObject *value = eval_node(node->left, globals);
        if (!value) return NULL;
        int rc = PyDict_SetItem(globals, node->constant, value);
        Py_DECREF(value);
        if (rc < 0) return NULL;
        Py_RETURN_NONE;
    }

    if (node->op == PYX_IR_GETATTR) {
        PyObject *object = eval_node(node->left, globals);
        if (!object) return NULL;
        PyObject *result = PyObject_GetAttr(object, node->constant);
        Py_DECREF(object);
        return result;
    }

    if (node->op == PYX_IR_SETATTR) {
        PyObject *object = eval_node(node->left, globals);
        PyObject *value = eval_node(node->right, globals);
        if (!object || !value) { Py_XDECREF(object); Py_XDECREF(value); return NULL; }
        int rc = PyObject_SetAttr(object, node->constant, value);
        Py_DECREF(object); Py_DECREF(value);
        if (rc < 0) return NULL;
        Py_RETURN_NONE;
    }

    if (node->op == PYX_IR_SUBSCRIPT || node->op == PYX_IR_SUBSCRIPT_STORE) {
        PyObject *object = eval_node(node->children[0], globals);
        if (!object) return NULL;
        PyObject *key = is_slice_node(node->children[1])
            ? eval_slice(node->children[1], globals)
            : eval_node(node->children[1], globals);
        if (!key) { Py_DECREF(object); return NULL; }
        if (node->op == PYX_IR_SUBSCRIPT) {
            PyObject *result = PyObject_GetItem(object, key);
            Py_DECREF(object); Py_DECREF(key);
            return result;
        }
        PyObject *value = eval_node(node->children[2], globals);
        if (!value) { Py_DECREF(object); Py_DECREF(key); return NULL; }
        int rc = PyObject_SetItem(object, key, value);
        Py_DECREF(object); Py_DECREF(key); Py_DECREF(value);
        if (rc < 0) return NULL;
        Py_RETURN_NONE;
    }

    if (node->op == PYX_IR_SEQUENCE) {
        PyObject *result = Py_NewRef(Py_None);
        for (Py_ssize_t i = 0; i < node->child_count; i++) {
            PyObject *item = eval_node(node->children[i], globals);
            if (!item) { Py_DECREF(result); return NULL; }
            Py_SETREF(result, item);
        }
        return result;
    }

    if (node->op == PYX_IR_LIST || node->op == PYX_IR_TUPLE || node->op == PYX_IR_SET) {
        PyObject *result = node->op == PYX_IR_LIST ? PyList_New(node->child_count) :
                           node->op == PYX_IR_TUPLE ? PyTuple_New(node->child_count) : PySet_New(NULL);
        if (!result) return NULL;
        for (Py_ssize_t i = 0; i < node->child_count; i++) {
            PyObject *item = eval_node(node->children[i], globals);
            if (!item) { Py_DECREF(result); return NULL; }
            if (node->op == PYX_IR_LIST) PyList_SET_ITEM(result, i, item);
            else if (node->op == PYX_IR_TUPLE) PyTuple_SET_ITEM(result, i, item);
            else {
                int rc = PySet_Add(result, item);
                Py_DECREF(item);
                if (rc < 0) { Py_DECREF(result); return NULL; }
            }
        }
        return result;
    }

    if (node->op == PYX_IR_DICT) {
        PyObject *result = PyDict_New();
        if (!result) return NULL;
        for (Py_ssize_t i = 0; i < node->child_count; i += 2) {
            PyObject *key = eval_node(node->children[i], globals);
            PyObject *value = eval_node(node->children[i + 1], globals);
            if (!key || !value) { Py_XDECREF(key); Py_XDECREF(value); Py_DECREF(result); return NULL; }
            int rc = PyDict_SetItem(result, key, value);
            Py_DECREF(key); Py_DECREF(value);
            if (rc < 0) { Py_DECREF(result); return NULL; }
        }
        return result;
    }

    if (node->op == PYX_IR_NOT || node->op == PYX_IR_INVERT || node->op == PYX_IR_POSITIVE || node->op == PYX_IR_NEGATIVE) {
        PyObject *value = eval_node(node->left, globals);
        if (!value) return NULL;
        PyObject *result = NULL;
        if (node->op == PYX_IR_NOT) {
            int truth = PyObject_IsTrue(value);
            if (truth < 0) { Py_DECREF(value); return NULL; }
            result = PyBool_FromLong(!truth);
        } else if (node->op == PYX_IR_INVERT) result = PyNumber_Invert(value);
        else if (node->op == PYX_IR_POSITIVE) result = PyNumber_Positive(value);
        else result = PyNumber_Negative(value);
        Py_DECREF(value);
        return result;
    }

    if (node->op >= PYX_IR_ADD && node->op <= PYX_IR_OR) {
        PyObject *left = eval_node(node->left, globals);
        if (!left) return NULL;
        if (node->op == PYX_IR_AND || node->op == PYX_IR_OR) {
            int truth = PyObject_IsTrue(left);
            if (truth < 0) { Py_DECREF(left); return NULL; }
            if ((node->op == PYX_IR_AND && !truth) || (node->op == PYX_IR_OR && truth)) return left;
        }
        PyObject *right = eval_node(node->right, globals);
        if (!right) { Py_DECREF(left); return NULL; }
        PyObject *result = NULL;
        switch (node->op) {
            case PYX_IR_ADD: result = PyNumber_Add(left, right); break;
            case PYX_IR_SUB: result = PyNumber_Subtract(left, right); break;
            case PYX_IR_MUL: result = PyNumber_Multiply(left, right); break;
            case PYX_IR_MATMUL: result = PyNumber_MatrixMultiply(left, right); break;
            case PYX_IR_DIV: result = PyNumber_TrueDivide(left, right); break;
            case PYX_IR_FLOORDIV: result = PyNumber_FloorDivide(left, right); break;
            case PYX_IR_MOD: result = PyNumber_Remainder(left, right); break;
            case PYX_IR_POW: result = PyNumber_Power(left, right, Py_None); break;
            case PYX_IR_LSHIFT: result = PyNumber_Lshift(left, right); break;
            case PYX_IR_RSHIFT: result = PyNumber_Rshift(left, right); break;
            case PYX_IR_BITOR: result = PyNumber_Or(left, right); break;
            case PYX_IR_BITXOR: result = PyNumber_Xor(left, right); break;
            case PYX_IR_BITAND: result = PyNumber_And(left, right); break;
            case PYX_IR_LT: result = PyObject_RichCompare(left, right, Py_LT); break;
            case PYX_IR_LE: result = PyObject_RichCompare(left, right, Py_LE); break;
            case PYX_IR_EQ: result = PyObject_RichCompare(left, right, Py_EQ); break;
            case PYX_IR_NE: result = PyObject_RichCompare(left, right, Py_NE); break;
            case PYX_IR_GT: result = PyObject_RichCompare(left, right, Py_GT); break;
            case PYX_IR_GE: result = PyObject_RichCompare(left, right, Py_GE); break;
            case PYX_IR_IS: result = PyBool_FromLong(left == right); break;
            case PYX_IR_IS_NOT: result = PyBool_FromLong(left != right); break;
            case PYX_IR_IN: { int rc = PySequence_Contains(right, left); result = rc < 0 ? NULL : PyBool_FromLong(rc); break; }
            case PYX_IR_NOT_IN: { int rc = PySequence_Contains(right, left); result = rc < 0 ? NULL : PyBool_FromLong(!rc); break; }
            case PYX_IR_AND: result = right; Py_INCREF(result); break;
            case PYX_IR_OR: result = right; Py_INCREF(result); break;
            default: PyErr_SetString(PyExc_SystemError, "unsupported PythonX binary operation"); break;
        }
        Py_DECREF(left); Py_DECREF(right);
        return result;
    }

    PyErr_Format(PyExc_NotImplementedError, "PythonX IR evaluator: unsupported operation %d", (int)node->op);
    return NULL;
}

PyObject *_PyX_IR_Evaluate(const PyXIRNode *node, PyObject *globals)
{
    if (!node || !globals || !PyDict_Check(globals)) {
        PyErr_SetString(PyExc_TypeError, "invalid PythonX IR evaluation state");
        return NULL;
    }
    return eval_node(node, globals);
}
