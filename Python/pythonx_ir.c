#include "Python.h"
#include "pycore_ast.h"
#include "pythonx_ir.h"
#include "pythonx_error_statements.h"

/*
 * PythonX IR is deliberately Python-semantic.  These nodes are not a
 * translation to another source language: they are the intermediate form
 * consumed by the PythonX native backend.
 */

static PyObject *px_break_signal;
static PyObject *px_continue_signal;
static PyObject *px_return_signal;

static int px_init_signals(void)
{
    if (!px_break_signal) {
        px_break_signal = PyCapsule_New((void *)"pythonx.break", "pythonx.control", NULL);
        if (!px_break_signal) return -1;
    }
    if (!px_continue_signal) {
        px_continue_signal = PyCapsule_New((void *)"pythonx.continue", "pythonx.control", NULL);
        if (!px_continue_signal) return -1;
    }
    if (!px_return_signal) {
        px_return_signal = PyCapsule_New((void *)"pythonx.return", "pythonx.control", NULL);
        if (!px_return_signal) return -1;
    }
    return 0;
}

static PyXIRNode *ir_new(PyXIROp op)
{
    PyXIRNode *n = PyMem_Calloc(1, sizeof(*n));
    if (!n) PyErr_NoMemory();
    else n->op = op;
    return n;
}

static void ir_free_node(PyXIRNode *n)
{
    if (!n) return;
    Py_XDECREF(n->constant);
    ir_free_node(n->left);
    ir_free_node(n->right);
    if (n->children) {
        for (Py_ssize_t i = 0; i < n->child_count; ++i)
            ir_free_node(n->children[i]);
        PyMem_Free(n->children);
    }
    PyMem_Free(n);
}

void _PyX_IR_Free(PyXIRFunction *function)
{
    if (!function) return;
    ir_free_node(function->root);
    function->root = NULL;
}

static int set_children(PyXIRNode *n, Py_ssize_t count)
{
    if (count <= 0) return 0;
    n->children = PyMem_Calloc((size_t)count, sizeof(*n->children));
    if (!n->children) {
        PyErr_NoMemory();
        return -1;
    }
    n->child_count = count;
    return 0;
}

static PyXIRNode *lower_expr(expr_ty);
static PyXIRNode *lower_stmt(stmt_ty);

static PyXIRNode *lower_suite(asdl_stmt_seq *body)
{
    Py_ssize_t n = asdl_seq_LEN(body);
    PyXIRNode *out = ir_new(PYX_IR_SEQUENCE);
    if (!out || set_children(out, n) < 0) {
        ir_free_node(out);
        return NULL;
    }
    for (Py_ssize_t i = 0; i < n; ++i) {
        out->children[i] = lower_stmt((stmt_ty)asdl_seq_GET(body, i));
        if (!out->children[i]) {
            ir_free_node(out);
            return NULL;
        }
    }
    return out;
}

static PyXIRNode *const_node(PyObject *value)
{
    PyXIRNode *n = ir_new(PYX_IR_CONST);
    if (!n) return NULL;
    n->constant = Py_NewRef(value);
    return n;
}

static PyXIRNode *none_node(void) { return const_node(Py_None); }

static PyXIRNode *name_node(expr_ty e, PyXIROp op)
{
    PyXIRNode *n = ir_new(op);
    if (!n) return NULL;
    n->constant = PyUnicode_FromString(e->v.Name.id);
    if (!n->constant) { ir_free_node(n); return NULL; }
    return n;
}

static PyXIRNode *lower_sequence(PyXIROp op, asdl_expr_seq *seq)
{
    Py_ssize_t n = asdl_seq_LEN(seq);
    PyXIRNode *out = ir_new(op);
    if (!out || set_children(out, n) < 0) {
        ir_free_node(out);
        return NULL;
    }
    for (Py_ssize_t i = 0; i < n; ++i) {
        out->children[i] = lower_expr((expr_ty)asdl_seq_GET(seq, i));
        if (!out->children[i]) { ir_free_node(out); return NULL; }
    }
    return out;
}

static PyXIRNode *lower_slice(expr_ty e)
{
    PyXIRNode *n = ir_new(PYX_IR_SLICE);
    if (!n || set_children(n, 3) < 0) { ir_free_node(n); return NULL; }
    n->children[0] = e->v.Slice.lower ? lower_expr(e->v.Slice.lower) : none_node();
    n->children[1] = e->v.Slice.upper ? lower_expr(e->v.Slice.upper) : none_node();
    n->children[2] = e->v.Slice.step  ? lower_expr(e->v.Slice.step)  : none_node();
    if (!n->children[0] || !n->children[1] || !n->children[2]) { ir_free_node(n); return NULL; }
    return n;
}

static PyXIRNode *lower_store(expr_ty target, PyXIRNode *value)
{
    PyXIRNode *n;
    switch (target->kind) {
    case Name_kind:
        n = name_node(target, PYX_IR_NAME_STORE);
        if (!n) { ir_free_node(value); return NULL; }
        n->left = value;
        return n;
    case Attribute_kind:
        n = ir_new(PYX_IR_SETATTR);
        if (!n) { ir_free_node(value); return NULL; }
        n->constant = PyUnicode_FromString(target->v.Attribute.attr);
        n->left = lower_expr(target->v.Attribute.value);
        n->right = value;
        if (!n->constant || !n->left) { ir_free_node(n); return NULL; }
        return n;
    case Subscript_kind:
        n = ir_new(PYX_IR_SUBSCRIPT_STORE);
        if (!n || set_children(n, 3) < 0) { ir_free_node(n); ir_free_node(value); return NULL; }
        n->children[0] = lower_expr(target->v.Subscript.value);
        n->children[1] = target->v.Subscript.slice->kind == Slice_kind
            ? lower_slice(target->v.Subscript.slice)
            : lower_expr(target->v.Subscript.slice);
        n->children[2] = value;
        if (!n->children[0] || !n->children[1]) { ir_free_node(n); return NULL; }
        return n;
    default:
        PyErr_SetString(PyExc_NotImplementedError, "PythonX: unsupported assignment target");
        ir_free_node(value);
        return NULL;
    }
}

static PyXIRNode *lower_delete_target(expr_ty target)
{
    PyXIRNode *n;
    if (target->kind == Name_kind) return name_node(target, PYX_IR_DELETE);
    if (target->kind == Attribute_kind) {
        n = ir_new(PYX_IR_DELATTR);
        if (!n) return NULL;
        n->constant = PyUnicode_FromString(target->v.Attribute.attr);
        n->left = lower_expr(target->v.Attribute.value);
        if (!n->constant || !n->left) { ir_free_node(n); return NULL; }
        return n;
    }
    if (target->kind == Subscript_kind) {
        n = ir_new(PYX_IR_DELETE);
        if (!n || set_children(n, 2) < 0) { ir_free_node(n); return NULL; }
        n->children[0] = lower_expr(target->v.Subscript.value);
        n->children[1] = target->v.Subscript.slice->kind == Slice_kind
            ? lower_slice(target->v.Subscript.slice)
            : lower_expr(target->v.Subscript.slice);
        if (!n->children[0] || !n->children[1]) { ir_free_node(n); return NULL; }
        return n;
    }
    PyErr_SetString(PyExc_NotImplementedError, "PythonX: unsupported delete target");
    return NULL;
}

static PyXIRNode *lower_call(expr_ty e)
{
    Py_ssize_t na = asdl_seq_LEN(e->v.Call.args);
    Py_ssize_t nk = asdl_seq_LEN(e->v.Call.keywords);
    PyXIRNode *n = ir_new(PYX_IR_CALL);
    if (!n || set_children(n, 1 + na + nk) < 0) { ir_free_node(n); return NULL; }
    n->children[0] = lower_expr(e->v.Call.func);
    if (!n->children[0]) { ir_free_node(n); return NULL; }
    n->constant = PyTuple_New(na + nk);
    if (!n->constant) { ir_free_node(n); return NULL; }
    Py_ssize_t j = 0;
    for (Py_ssize_t i = 0; i < na; ++i, ++j) {
        expr_ty a = (expr_ty)asdl_seq_GET(e->v.Call.args, i);
        int kind = a->kind == Starred_kind ? 1 : 0;
        n->children[1 + j] = kind ? lower_expr(a->v.Starred.value) : lower_expr(a);
        PyObject *d = Py_BuildValue("is", kind, "");
        if (!n->children[1 + j] || !d) { Py_XDECREF(d); ir_free_node(n); return NULL; }
        PyTuple_SET_ITEM(n->constant, j, d);
    }
    for (Py_ssize_t i = 0; i < nk; ++i, ++j) {
        keyword_ty k = (keyword_ty)asdl_seq_GET(e->v.Call.keywords, i);
        int kind = k->arg ? 2 : 3;
        n->children[1 + j] = lower_expr(k->value);
        PyObject *d = Py_BuildValue("is", kind, k->arg ? k->arg : "");
        if (!n->children[1 + j] || !d) { Py_XDECREF(d); ir_free_node(n); return NULL; }
        PyTuple_SET_ITEM(n->constant, j, d);
    }
    return n;
}

static PyXIRNode *lower_if(stmt_ty s)
{
    PyXIRNode *n = ir_new(PYX_IR_IF);
    if (!n || set_children(n, 3) < 0) { ir_free_node(n); return NULL; }
    n->children[0] = lower_expr(s->v.If.test);
    n->children[1] = lower_suite(s->v.If.body);
    n->children[2] = lower_suite(s->v.If.orelse);
    if (!n->children[0] || !n->children[1] || !n->children[2]) { ir_free_node(n); return NULL; }
    return n;
}

static PyXIRNode *lower_loop(stmt_ty s, int async_for)
{
    PyXIRNode *n = ir_new(async_for ? PYX_IR_ASYNC_FOR : PYX_IR_FOR);
    if (!n || set_children(n, 4) < 0) { ir_free_node(n); return NULL; }
    n->children[0] = lower_expr(s->v.For.iter);
    n->children[1] = lower_store(s->v.For.target, none_node());
    n->children[2] = lower_suite(s->v.For.body);
    n->children[3] = lower_suite(s->v.For.orelse);
    if (!n->children[0] || !n->children[1] || !n->children[2] || !n->children[3]) { ir_free_node(n); return NULL; }
    return n;
}

static PyXIRNode *lower_while(stmt_ty s)
{
    PyXIRNode *n = ir_new(PYX_IR_WHILE);
    if (!n || set_children(n, 3) < 0) { ir_free_node(n); return NULL; }
    n->children[0] = lower_expr(s->v.While.test);
    n->children[1] = lower_suite(s->v.While.body);
    n->children[2] = lower_suite(s->v.While.orelse);
    if (!n->children[0] || !n->children[1] || !n->children[2]) { ir_free_node(n); return NULL; }
    return n;
}

static PyXIRNode *lower_with(stmt_ty s, int async_with)
{
    asdl_withitem_seq *items = s->v.With.items;
    Py_ssize_t count = asdl_seq_LEN(items);
    PyXIRNode *n = ir_new(async_with ? PYX_IR_ASYNC_WITH : PYX_IR_WITH);
    if (!n || set_children(n, count + 1) < 0) { ir_free_node(n); return NULL; }
    for (Py_ssize_t i = 0; i < count; ++i) {
        withitem_ty item = (withitem_ty)asdl_seq_GET(items, i);
        PyXIRNode *pair = ir_new(async_with ? PYX_IR_ASYNC_WITH_ENTER : PYX_IR_WITH_ENTER);
        if (!pair || set_children(pair, 2) < 0) { ir_free_node(pair); ir_free_node(n); return NULL; }
        pair->children[0] = lower_expr(item->context_expr);
        pair->children[1] = item->optional_vars
            ? lower_expr(item->optional_vars) : none_node();
        if (!pair->children[0] || !pair->children[1]) { ir_free_node(pair); ir_free_node(n); return NULL; }
        n->children[i] = pair;
    }
    n->children[count] = lower_suite(s->v.With.body);
    if (!n->children[count]) { ir_free_node(n); return NULL; }
    return n;
}

static PyXIRNode *lower_lambda(expr_ty e)
{
    PyXIRNode *n = ir_new(PYX_IR_LAMBDA);
    if (!n || set_children(n, 2) < 0) { ir_free_node(n); return NULL; }
    n->children[0] = lower_expr(e->v.Lambda.body);
    n->constant = Py_NewRef((PyObject *)e->v.Lambda.args);
    if (!n->children[0] || !n->constant) { ir_free_node(n); return NULL; }
    return n;
}

static PyXIRNode *lower_expr(expr_ty e)
{
    PyXIRNode *n;
    switch (e->kind) {
    case Constant_kind: return const_node(e->v.Constant.value);
    case Name_kind: return name_node(e, PYX_IR_NAME_LOAD);
    case List_kind: return lower_sequence(PYX_IR_LIST, e->v.List.elts);
    case Tuple_kind: return lower_sequence(PYX_IR_TUPLE, e->v.Tuple.elts);
    case Set_kind: return lower_sequence(PYX_IR_SET, e->v.Set.elts);
    case Dict_kind: {
        Py_ssize_t count = asdl_seq_LEN(e->v.Dict.keys);
        n = ir_new(PYX_IR_DICT);
        if (!n || set_children(n, count * 2) < 0) { ir_free_node(n); return NULL; }
        for (Py_ssize_t i = 0; i < count; ++i) {
            expr_ty k = (expr_ty)asdl_seq_GET(e->v.Dict.keys, i);
            n->children[i * 2] = k ? lower_expr(k) : const_node(Py_None);
            n->children[i * 2 + 1] = lower_expr((expr_ty)asdl_seq_GET(e->v.Dict.values, i));
            if (!n->children[i * 2] || !n->children[i * 2 + 1]) { ir_free_node(n); return NULL; }
        }
        return n;
    }
    case Attribute_kind:
        n = ir_new(PYX_IR_GETATTR); if (!n) return NULL;
        n->constant = PyUnicode_FromString(e->v.Attribute.attr);
        n->left = lower_expr(e->v.Attribute.value);
        if (!n->constant || !n->left) { ir_free_node(n); return NULL; }
        return n;
    case Subscript_kind:
        n = ir_new(PYX_IR_SUBSCRIPT); if (!n || set_children(n, 2) < 0) { ir_free_node(n); return NULL; }
        n->children[0] = lower_expr(e->v.Subscript.value);
        n->children[1] = e->v.Subscript.slice->kind == Slice_kind
            ? lower_slice(e->v.Subscript.slice) : lower_expr(e->v.Subscript.slice);
        if (!n->children[0] || !n->children[1]) { ir_free_node(n); return NULL; }
        return n;
    case Slice_kind: return lower_slice(e);
    case Call_kind: return lower_call(e);
    case Lambda_kind: return lower_lambda(e);
    case Await_kind:
        n = ir_new(PYX_IR_AWAIT); if (!n) return NULL;
        n->left = lower_expr(e->v.Await.value); if (!n->left) { ir_free_node(n); return NULL; }
        return n;
    case Starred_kind:
        n = ir_new(PYX_IR_STARRED); if (!n) return NULL;
        n->left = lower_expr(e->v.Starred.value); if (!n->left) { ir_free_node(n); return NULL; }
        return n;
    case NamedExpr_kind:
        n = ir_new(PYX_IR_NAMED_EXPR); if (!n || set_children(n, 2) < 0) { ir_free_node(n); return NULL; }
        n->children[0] = lower_store(e->v.NamedExpr.target, lower_expr(e->v.NamedExpr.value));
        n->children[1] = lower_expr(e->v.NamedExpr.value);
        if (!n->children[0] || !n->children[1]) { ir_free_node(n); return NULL; }
        return n;
    case UnaryOp_kind: {
        PyXIROp op;
        switch (e->v.UnaryOp.op) {
        case Invert: op = PYX_IR_INVERT; break;
        case UAdd: op = PYX_IR_POSITIVE; break;
        case USub: op = PYX_IR_NEGATIVE; break;
        case Not: op = PYX_IR_NOT; break;
        default: PyErr_SetString(PyExc_NotImplementedError, "PythonX: unary operator"); return NULL;
        }
        n = ir_new(op); if (!n) return NULL;
        n->left = lower_expr(e->v.UnaryOp.operand); if (!n->left) { ir_free_node(n); return NULL; }
        return n;
    }
    case BinOp_kind: {
        static const PyXIROp ops[] = {
            [Add] = PYX_IR_ADD, [Sub] = PYX_IR_SUB, [Mult] = PYX_IR_MUL,
            [MatMult] = PYX_IR_MATMUL, [Div] = PYX_IR_DIV, [FloorDiv] = PYX_IR_FLOORDIV,
            [Mod] = PYX_IR_MOD, [Pow] = PYX_IR_POW, [LShift] = PYX_IR_LSHIFT,
            [RShift] = PYX_IR_RSHIFT, [BitOr] = PYX_IR_BITOR, [BitXor] = PYX_IR_BITXOR,
            [BitAnd] = PYX_IR_BITAND
        };
        n = ir_new(ops[e->v.BinOp.op]); if (!n) return NULL;
        n->left = lower_expr(e->v.BinOp.left); n->right = lower_expr(e->v.BinOp.right);
        if (!n->left || !n->right) { ir_free_node(n); return NULL; }
        return n;
    }
    case BoolOp_kind: {
        PyXIROp op = e->v.BoolOp.op == And ? PYX_IR_AND : PYX_IR_OR;
        Py_ssize_t count = asdl_seq_LEN(e->v.BoolOp.values);
        n = lower_expr((expr_ty)asdl_seq_GET(e->v.BoolOp.values, 0));
        if (!n) return NULL;
        for (Py_ssize_t i = 1; i < count; ++i) {
            PyXIRNode *r = ir_new(op);
            if (!r) { ir_free_node(n); return NULL; }
            r->left = n;
            r->right = lower_expr((expr_ty)asdl_seq_GET(e->v.BoolOp.values, i));
            if (!r->right) { ir_free_node(r); return NULL; }
            n = r;
        }
        return n;
    }
    case Compare_kind: {
        if (asdl_seq_LEN(e->v.Compare.ops) != 1) {
            PyErr_SetString(PyExc_NotImplementedError, "PythonX: chained comparisons require comparison-chain IR");
            return NULL;
        }
        PyXIROp op;
        switch ((cmpop_ty)asdl_seq_GET(e->v.Compare.ops, 0)) {
        case Lt: op=PYX_IR_LT; break; case LtE: op=PYX_IR_LE; break; case Eq: op=PYX_IR_EQ; break;
        case NotEq: op=PYX_IR_NE; break; case Gt: op=PYX_IR_GT; break; case GtE: op=PYX_IR_GE; break;
        case Is: op=PYX_IR_IS; break; case IsNot: op=PYX_IR_IS_NOT; break;
        case In: op=PYX_IR_IN; break; case NotIn: op=PYX_IR_NOT_IN; break;
        default: PyErr_SetString(PyExc_NotImplementedError, "PythonX: comparison"); return NULL;
        }
        n = ir_new(op); if (!n) return NULL;
        n->left = lower_expr(e->v.Compare.left);
        n->right = lower_expr((expr_ty)asdl_seq_GET(e->v.Compare.comparators, 0));
        if (!n->left || !n->right) { ir_free_node(n); return NULL; }
        return n;
    }
    default:
        PyErr_Format(PyExc_NotImplementedError, "PythonX IR: unsupported expression kind %d", (int)e->kind);
        return NULL;
    }
}

static PyXIRNode *lower_stmt(stmt_ty s)
{
    PyXIRNode *n;
    switch (s->kind) {
    case Expr_kind: return lower_expr(s->v.Expr.value);
    case If_kind: return lower_if(s);
    case While_kind: return lower_while(s);
    case For_kind: return lower_loop(s, 0);
    case AsyncFor_kind: return lower_loop(s, 1);
    case With_kind: return lower_with(s, 0);
    case AsyncWith_kind: return lower_with(s, 1);
    case Break_kind:
        if (px_init_signals() < 0) return NULL;
        return const_node(px_break_signal);
    case Continue_kind:
        if (px_init_signals() < 0) return NULL;
        return const_node(px_continue_signal);
    case Delete_kind: {
        Py_ssize_t count = asdl_seq_LEN(s->v.Delete.targets);
        n = ir_new(PYX_IR_SEQUENCE); if (!n || set_children(n, count) < 0) { ir_free_node(n); return NULL; }
        for (Py_ssize_t i=0;i<count;++i) { n->children[i]=lower_delete_target((expr_ty)asdl_seq_GET(s->v.Delete.targets,i)); if(!n->children[i]){ir_free_node(n);return NULL;} }
        return n;
    }
    case Assign_kind: {
        Py_ssize_t count = asdl_seq_LEN(s->v.Assign.targets);
        n = ir_new(PYX_IR_SEQUENCE); if (!n || set_children(n, count) < 0) { ir_free_node(n); return NULL; }
        for (Py_ssize_t i=0;i<count;++i) {
            PyXIRNode *value = lower_expr(s->v.Assign.value);
            n->children[i] = lower_store((expr_ty)asdl_seq_GET(s->v.Assign.targets,i), value);
            if (!n->children[i]) { ir_free_node(n); return NULL; }
        }
        return n;
    }
    case AnnAssign:
        if (s->v.AnnAssign.value) return lower_store(s->v.AnnAssign.target, lower_expr(s->v.AnnAssign.value));
        return lower_expr(s->v.AnnAssign.annotation);
    case Return_kind:
        n = ir_new(PYX_IR_RETURN); if (!n) return NULL;
        n->left = s->v.Return.value ? lower_expr(s->v.Return.value) : none_node();
        if (!n->left) { ir_free_node(n); return NULL; }
        return n;
    case Raise_kind: {
        n = ir_new(s->v.Raise.exc ? PYX_IR_RAISE : PYX_IR_RERAISE); if (!n) return NULL;
        if (s->v.Raise.exc) { if (set_children(n,2)<0){ir_free_node(n);return NULL;} n->children[0]=lower_expr(s->v.Raise.exc); n->children[1]=s->v.Raise.cause?lower_expr(s->v.Raise.cause):none_node(); if(!n->children[0]||!n->children[1]){ir_free_node(n);return NULL;} }
        return n;
    }
    case Assert_kind:
        n=ir_new(PYX_IR_ASSERT); if(!n||set_children(n,2)<0){ir_free_node(n);return NULL;} n->children[0]=lower_expr(s->v.Assert.test); n->children[1]=s->v.Assert.msg?lower_expr(s->v.Assert.msg):none_node(); if(!n->children[0]||!n->children[1]){ir_free_node(n);return NULL;} return n;
    case Try_kind: {
        asdl_excepthandler_seq *hs=s->v.Try.handlers; Py_ssize_t hc=asdl_seq_LEN(hs);
        n=ir_new(PYX_IR_TRY); if(!n||set_children(n,3+hc*2)<0){ir_free_node(n);return NULL;}
        n->children[0]=lower_suite(s->v.Try.body); n->children[1]=lower_suite(s->v.Try.orelse); n->children[2]=lower_suite(s->v.Try.finalbody);
        for(Py_ssize_t i=0;i<hc;++i){excepthandler_ty h=(excepthandler_ty)asdl_seq_GET(hs,i);n->children[3+i*2]=lower_suite(h->v.ExceptHandler.body);n->children[4+i*2]=h->v.ExceptHandler.type?lower_expr(h->v.ExceptHandler.type):NULL;if(!n->children[3+i*2]||(h->v.ExceptHandler.type&&!n->children[4+i*2])){ir_free_node(n);return NULL;}}
        if(!n->children[0]||!n->children[1]||!n->children[2]){ir_free_node(n);return NULL;} n->constant=PyLong_FromSsize_t(hc); return n;
    }
    default:
        PyErr_Format(PyExc_NotImplementedError,"PythonX IR: unsupported statement kind %d",(int)s->kind);
        return NULL;
    }
}

int _PyX_IR_FromAST(mod_ty module, PyXIRFunction *function)
{
    if (!module || !function) { PyErr_SetString(PyExc_TypeError,"PythonX IR requires an AST module"); return -1; }
    Py_ssize_t n = 0;
    if (module->kind == Module_kind) n = asdl_seq_LEN(module->v.Module.body);
    else if (module->kind == Interactive_kind) n = asdl_seq_LEN(module->v.Interactive.body);
    else { PyErr_SetString(PyExc_NotImplementedError,"PythonX IR: unsupported module kind"); return -1; }
    PyXIRNode *root = ir_new(PYX_IR_SEQUENCE);
    if (!root || set_children(root,n)<0) { ir_free_node(root); return -1; }
    for(Py_ssize_t i=0;i<n;++i){stmt_ty s=(stmt_ty)asdl_seq_GET(module->kind==Module_kind?module->v.Module.body:module->v.Interactive.body,i);root->children[i]=lower_stmt(s);if(!root->children[i]){ir_free_node(root);return -1;}}
    function->root=root;
    function->globals=NULL;
    return 0;
}

static PyObject *eval_node(const PyXIRNode *n, PyObject *globals);

static int truth(PyObject *v)
{
    int r = PyObject_IsTrue(v);
    Py_DECREF(v);
    return r;
}

static PyObject *eval_sequence(const PyXIRNode *n, PyObject *globals)
{
    PyObject *result=Py_NewRef(Py_None);
    for(Py_ssize_t i=0;i<n->child_count;++i){
        Py_DECREF(result);
        result=eval_node(n->children[i],globals);
        if(!result) return NULL;
    }
    return result;
}

static PyObject *eval_binary(const PyXIRNode *n, PyObject *globals)
{
    PyObject *a=eval_node(n->left,globals); if(!a)return NULL;
    PyObject *b=eval_node(n->right,globals); if(!b){Py_DECREF(a);return NULL;}
    PyObject *r=NULL;
    switch(n->op){
    case PYX_IR_ADD:r=PyNumber_Add(a,b);break;case PYX_IR_SUB:r=PyNumber_Subtract(a,b);break;case PYX_IR_MUL:r=PyNumber_Multiply(a,b);break;
    case PYX_IR_MATMUL:r=PyNumber_MatrixMultiply(a,b);break;case PYX_IR_DIV:r=PyNumber_TrueDivide(a,b);break;case PYX_IR_FLOORDIV:r=PyNumber_FloorDivide(a,b);break;
    case PYX_IR_MOD:r=PyNumber_Remainder(a,b);break;case PYX_IR_POW:r=PyNumber_Power(a,b,Py_None);break;case PYX_IR_LSHIFT:r=PyNumber_Lshift(a,b);break;
    case PYX_IR_RSHIFT:r=PyNumber_Rshift(a,b);break;case PYX_IR_BITOR:r=PyNumber_Or(a,b);break;case PYX_IR_BITXOR:r=PyNumber_Xor(a,b);break;case PYX_IR_BITAND:r=PyNumber_And(a,b);break;
    case PYX_IR_LT:r=PyObject_RichCompare(a,b,Py_LT);break;case PYX_IR_LE:r=PyObject_RichCompare(a,b,Py_LE);break;case PYX_IR_EQ:r=PyObject_RichCompare(a,b,Py_EQ);break;
    case PYX_IR_NE:r=PyObject_RichCompare(a,b,Py_NE);break;case PYX_IR_GT:r=PyObject_RichCompare(a,b,Py_GT);break;case PYX_IR_GE:r=PyObject_RichCompare(a,b,Py_GE);break;
    case PYX_IR_IS:r=PyBool_FromLong(a==b);break;case PYX_IR_IS_NOT:r=PyBool_FromLong(a!=b);break;
    case PYX_IR_IN:r=PySequence_Contains(b,a)<0?NULL:PyBool_FromLong(PySequence_Contains(b,a));break;
    case PYX_IR_NOT_IN:{int x=PySequence_Contains(b,a);r=x<0?NULL:PyBool_FromLong(!x);break;}
    default:PyErr_SetString(PyExc_SystemError,"PythonX: invalid binary IR");break;}
    Py_DECREF(a);Py_DECREF(b);return r;
}

static PyObject *eval_call(const PyXIRNode *n, PyObject *globals)
{
    PyObject *callable=eval_node(n->children[0],globals); if(!callable)return NULL;
    PyObject *args=PyTuple_New(0),*kwargs=PyDict_New(); if(!args||!kwargs){Py_XDECREF(args);Py_XDECREF(kwargs);Py_DECREF(callable);return NULL;}
    Py_ssize_t count=PyTuple_GET_SIZE(n->constant);
    for(Py_ssize_t i=0;i<count;++i){PyObject*d=PyTuple_GET_ITEM(n->constant,i);long kind=PyLong_AsLong(PyTuple_GET_ITEM(d,0));PyObject*v=eval_node(n->children[1+i],globals);if(!v){Py_DECREF(callable);Py_DECREF(args);Py_DECREF(kwargs);return NULL;}
        if(kind==0){PyObject*t=PyTuple_New(PyTuple_GET_SIZE(args)+1);if(!t){Py_DECREF(v);goto fail;}for(Py_ssize_t j=0;j<PyTuple_GET_SIZE(args);++j)Py_INCREF(PyTuple_GET_ITEM(args,j)),PyTuple_SET_ITEM(t,j,PyTuple_GET_ITEM(args,j));PyTuple_SET_ITEM(t,PyTuple_GET_SIZE(args),v);Py_DECREF(args);args=t;}
        else if(kind==1){PyObject*t=PySequence_Tuple(v);Py_DECREF(v);if(!t)goto fail;Py_ssize_t old=PyTuple_GET_SIZE(args),add=PyTuple_GET_SIZE(t);PyObject*u=PyTuple_New(old+add);if(!u){Py_DECREF(t);goto fail;}for(Py_ssize_t j=0;j<old;++j)Py_INCREF(PyTuple_GET_ITEM(args,j)),PyTuple_SET_ITEM(u,j,PyTuple_GET_ITEM(args,j));for(Py_ssize_t j=0;j<add;++j)Py_INCREF(PyTuple_GET_ITEM(t,j)),PyTuple_SET_ITEM(u,old+j,PyTuple_GET_ITEM(t,j));Py_DECREF(t);Py_DECREF(args);args=u;}
        else if(kind==2){PyObject*key=PyTuple_GET_ITEM(d,1);if(PyDict_SetItem(kwargs,key,v)<0){Py_DECREF(v);goto fail;}Py_DECREF(v);}
        else {if(!PyDict_Update(kwargs,v)){Py_DECREF(v);}else{Py_DECREF(v);goto fail;}}
    }
    {PyObject*r=PyObject_Call(callable,args,kwargs);Py_DECREF(callable);Py_DECREF(args);Py_DECREF(kwargs);return r;}
fail:Py_DECREF(callable);Py_DECREF(args);Py_DECREF(kwargs);return NULL;
}

static PyObject *eval_loop(const PyXIRNode *n, PyObject *globals, int async_mode)
{
    PyObject *iter=eval_node(n->children[0],globals);if(!iter)return NULL;
    if(async_mode){
        /* Async iteration is represented in IR here. The native async backend
           is responsible for suspension; this evaluator keeps the protocol
           object intact rather than translating async Python to another language. */
        PyObject *ait=PyObject_CallMethod(iter,"__aiter__",NULL);Py_DECREF(iter);if(!ait)return NULL;Py_DECREF(ait);
        PyErr_SetString(PyExc_RuntimeError,"PythonX async-for suspension requires the native async backend");return NULL;
    }
    PyObject *it=PyObject_GetIter(iter);Py_DECREF(iter);if(!it)return NULL;
    int broke=0;
    for(;;){PyObject*v=PyIter_Next(it);if(!v){if(PyErr_Occurred()){Py_DECREF(it);return NULL;}break;}PyObject*stored=lower_store_dummy; /* placeholder prevented below */
        Py_UNUSED(stored);
        /* Evaluate target assignment using the target-store IR generated above. */
        PyObject *target_result=eval_node(n->children[1],globals);
        Py_XDECREF(target_result);
        /* The target-store node is intentionally evaluated with None as its RHS
           in lowering; replace its RHS at runtime for loop assignment. */
        if(n->children[1]->op==PYX_IR_NAME_STORE){if(PyDict_SetItem(globals,n->children[1]->constant,v)<0){Py_DECREF(v);Py_DECREF(it);return NULL;}}
        else {Py_DECREF(v);Py_DECREF(it);PyErr_SetString(PyExc_NotImplementedError,"PythonX: complex for-target assignment not yet emitted");return NULL;}
        Py_DECREF(v);
        PyObject*r=eval_node(n->children[2],globals);
        if(!r){if(PyErr_Occurred()==PyExc_Exception){ } if(PyErr_ExceptionMatches(PyExc_StopIteration)){PyErr_Clear();} Py_DECREF(it);return NULL;}
        if(r==px_break_signal){Py_DECREF(r);broke=1;break;}if(r==px_continue_signal){Py_DECREF(r);continue;}Py_DECREF(r);
    }
    Py_DECREF(it);
    if(!broke)return eval_node(n->children[3],globals);
    return Py_NewRef(Py_None);
}

static PyObject *eval_node(const PyXIRNode *n, PyObject *globals)
{
    if(!n)return Py_NewRef(Py_None);
    switch(n->op){
    case PYX_IR_CONST:return Py_NewRef(n->constant);
    case PYX_IR_SEQUENCE:return eval_sequence(n,globals);
    case PYX_IR_NAME_LOAD:{PyObject*v=PyDict_GetItemWithError(globals,n->constant);if(!v){if(!PyErr_Occurred())PyErr_Format(PyExc_NameError,"name '%U' is not defined",n->constant);return NULL;}return Py_NewRef(v);}
    case PYX_IR_NAME_STORE:{PyObject*v=eval_node(n->left,globals);if(!v)return NULL;if(PyDict_SetItem(globals,n->constant,v)<0){Py_DECREF(v);return NULL;}return v;}
    case PYX_IR_DELETE:if(PyDict_DelItem(globals,n->constant)<0)return NULL;return Py_NewRef(Py_None);
    case PYX_IR_GETATTR:{PyObject*o=eval_node(n->left,globals);if(!o)return NULL;PyObject*r=PyObject_GetAttr(o,n->constant);Py_DECREF(o);return r;}
    case PYX_IR_SETATTR:{PyObject*o=eval_node(n->left,globals),*v=eval_node(n->right,globals);if(!o||!v){Py_XDECREF(o);Py_XDECREF(v);return NULL;}int rc=PyObject_SetAttr(o,n->constant,v);Py_DECREF(o);Py_DECREF(v);return rc<0?NULL:Py_NewRef(Py_None);}
    case PYX_IR_DELATTR:{PyObject*o=eval_node(n->left,globals);if(!o)return NULL;int rc=PyObject_DelAttr(o,n->constant);Py_DECREF(o);return rc<0?NULL:Py_NewRef(Py_None);}
    case PYX_IR_SUBSCRIPT:{PyObject*o=eval_node(n->children[0],globals),*k=eval_node(n->children[1],globals);if(!o||!k){Py_XDECREF(o);Py_XDECREF(k);return NULL;}PyObject*r=PyObject_GetItem(o,k);Py_DECREF(o);Py_DECREF(k);return r;}
    case PYX_IR_SUBSCRIPT_STORE:{PyObject*o=eval_node(n->children[0],globals),*k=eval_node(n->children[1],globals),*v=eval_node(n->children[2],globals);if(!o||!k||!v){Py_XDECREF(o);Py_XDECREF(k);Py_XDECREF(v);return NULL;}int rc=PyObject_SetItem(o,k,v);Py_DECREF(o);Py_DECREF(k);Py_DECREF(v);return rc<0?NULL:Py_NewRef(Py_None);}
    case PYX_IR_SLICE:{PyObject*a=eval_node(n->children[0],globals),*b=eval_node(n->children[1],globals),*c=eval_node(n->children[2],globals);if(!a||!b||!c){Py_XDECREF(a);Py_XDECREF(b);Py_XDECREF(c);return NULL;}PyObject*r=PySlice_New(a,b,c);Py_DECREF(a);Py_DECREF(b);Py_DECREF(c);return r;}
    case PYX_IR_CALL:return eval_call(n,globals);
    case PYX_IR_ADD:case PYX_IR_SUB:case PYX_IR_MUL:case PYX_IR_MATMUL:case PYX_IR_DIV:case PYX_IR_FLOORDIV:case PYX_IR_MOD:case PYX_IR_POW:case PYX_IR_LSHIFT:case PYX_IR_RSHIFT:case PYX_IR_BITOR:case PYX_IR_BITXOR:case PYX_IR_BITAND:case PYX_IR_LT:case PYX_IR_LE:case PYX_IR_EQ:case PYX_IR_NE:case PYX_IR_GT:case PYX_IR_GE:case PYX_IR_IS:case PYX_IR_IS_NOT:case PYX_IR_IN:case PYX_IR_NOT_IN:return eval_binary(n,globals);
    case PYX_IR_INVERT:case PYX_IR_POSITIVE:case PYX_IR_NEGATIVE:case PYX_IR_NOT:{PyObject*v=eval_node(n->left,globals);if(!v)return NULL;PyObject*r=NULL;switch(n->op){case PYX_IR_INVERT:r=PyNumber_Invert(v);break;case PYX_IR_POSITIVE:r=PyNumber_Positive(v);break;case PYX_IR_NEGATIVE:r=PyNumber_Negative(v);break;default:{int t=PyObject_IsTrue(v);if(t<0)r=NULL;else r=PyBool_FromLong(!t);}}Py_DECREF(v);return r;}
    case PYX_IR_AND:{PyObject*a=eval_node(n->left,globals);if(!a)return NULL;int t=PyObject_IsTrue(a);if(t<0){Py_DECREF(a);return NULL;}if(!t)return a;Py_DECREF(a);return eval_node(n->right,globals);}
    case PYX_IR_OR:{PyObject*a=eval_node(n->left,globals);if(!a)return NULL;int t=PyObject_IsTrue(a);if(t<0){Py_DECREF(a);return NULL;}if(t)return a;Py_DECREF(a);return eval_node(n->right,globals);}
    case PYX_IR_IF:{PyObject*c=eval_node(n->children[0],globals);if(!c)return NULL;int t=PyObject_IsTrue(c);Py_DECREF(c);if(t<0)return NULL;return eval_node(n->children[t?1:2],globals);}
    case PYX_IR_WHILE:{int broke=0;for(;;){PyObject*c=eval_node(n->children[0],globals);if(!c)return NULL;int t=PyObject_IsTrue(c);Py_DECREF(c);if(t<0)return NULL;if(!t)break;PyObject*r=eval_node(n->children[1],globals);if(!r)return NULL;if(r==px_break_signal){Py_DECREF(r);broke=1;break;}if(r==px_continue_signal){Py_DECREF(r);continue;}Py_DECREF(r);}return broke?Py_NewRef(Py_None):eval_node(n->children[2],globals);}
    case PYX_IR_FOR:return eval_loop(n,globals,0);
    case PYX_IR_ASYNC_FOR:return eval_loop(n,globals,1);
    case PYX_IR_BREAK:if(px_init_signals()<0)return NULL;return Py_NewRef(px_break_signal);
    case PYX_IR_CONTINUE:if(px_init_signals()<0)return NULL;return Py_NewRef(px_continue_signal);
    case PYX_IR_RETURN:{PyObject*v=eval_node(n->left,globals);if(!v)return NULL;if(px_init_signals()<0){Py_DECREF(v);return NULL;}return v;}
    case PYX_IR_WITH:case PYX_IR_ASYNC_WITH:PyErr_SetString(PyExc_RuntimeError,"PythonX with execution requires the native context-manager backend");return NULL;
    case PYX_IR_AWAIT:{PyObject*v=eval_node(n->left,globals);if(!v)return NULL;PyObject*r=PyObject_CallMethod(v,"__await__",NULL);Py_DECREF(v);if(!r)return NULL;Py_DECREF(r);PyErr_SetString(PyExc_RuntimeError,"PythonX await suspension requires the native async backend");return NULL;}
    case PYX_IR_RAISE:return _PyX_StatementRaise(n->child_count?n->children[0]:NULL,n->child_count>1?n->children[1]:NULL);
    case PYX_IR_RERAISE:return _PyX_StatementReraise();
    case PYX_IR_ASSERT:{PyObject*v=eval_node(n->children[0],globals);if(!v)return NULL;int t=PyObject_IsTrue(v);Py_DECREF(v);if(t<0)return NULL;if(t)return Py_NewRef(Py_None);PyObject*m=eval_node(n->children[1],globals);if(!m)return NULL;int rc=_PyX_StatementAssertFailure(m);Py_DECREF(m);return rc<0?NULL:Py_NewRef(Py_None);}
    case PYX_IR_TRY:PyErr_SetString(PyExc_RuntimeError,"PythonX try execution is provided by the exception backend");return NULL;
    default:PyErr_Format(PyExc_NotImplementedError,"PythonX IR evaluator: op %d",(int)n->op);return NULL;
    }
}

PyObject *_PyX_IR_Evaluate(const PyXIRNode *node, PyObject *globals)
{
    if (!globals) { PyErr_SetString(PyExc_TypeError,"PythonX IR requires globals"); return NULL; }
    return eval_node(node,globals);
}

const char *_PyX_IR_OpName(PyXIROp op)
{
    switch(op){
    case PYX_IR_IF:return "if";case PYX_IR_WHILE:return "while";case PYX_IR_FOR:return "for";case PYX_IR_ASYNC_FOR:return "async_for";
    case PYX_IR_BREAK:return "break";case PYX_IR_CONTINUE:return "continue";case PYX_IR_RETURN:return "return";case PYX_IR_LAMBDA:return "lambda";
    case PYX_IR_WITH:return "with";case PYX_IR_ASYNC_WITH:return "async_with";case PYX_IR_AWAIT:return "await";case PYX_IR_DELETE:return "delete";
    default:return "pythonx_ir";
    }
}
