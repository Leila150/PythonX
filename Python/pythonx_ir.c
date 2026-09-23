#include "Python.h"
#include "pycore_ast.h"
#include "pythonx_ir.h"
#include "pythonx_error_statements.h"

static PyObject *px_break_signal;
static PyObject *px_continue_signal;

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
    if (!n->children) { PyErr_NoMemory(); return -1; }
    n->child_count = count;
    return 0;
}

static PyXIRNode *lower_expr(expr_ty);
static PyXIRNode *lower_stmt(stmt_ty);

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

static PyXIRNode *lower_suite(asdl_stmt_seq *body)
{
    Py_ssize_t count = asdl_seq_LEN(body);
    PyXIRNode *n = ir_new(PYX_IR_SEQUENCE);
    if (!n || set_children(n, count) < 0) { ir_free_node(n); return NULL; }
    for (Py_ssize_t i = 0; i < count; ++i) {
        n->children[i] = lower_stmt((stmt_ty)asdl_seq_GET(body, i));
        if (!n->children[i]) { ir_free_node(n); return NULL; }
    }
    return n;
}

static PyXIRNode *lower_sequence(PyXIROp op, asdl_expr_seq *values)
{
    Py_ssize_t count = asdl_seq_LEN(values);
    PyXIRNode *n = ir_new(op);
    if (!n || set_children(n, count) < 0) { ir_free_node(n); return NULL; }
    for (Py_ssize_t i = 0; i < count; ++i) {
        n->children[i] = lower_expr((expr_ty)asdl_seq_GET(values, i));
        if (!n->children[i]) { ir_free_node(n); return NULL; }
    }
    return n;
}

static PyXIRNode *lower_slice(expr_ty e)
{
    PyXIRNode *n = ir_new(PYX_IR_SLICE);
    if (!n || set_children(n, 3) < 0) { ir_free_node(n); return NULL; }
    n->children[0] = e->v.Slice.lower ? lower_expr(e->v.Slice.lower) : none_node();
    n->children[1] = e->v.Slice.upper ? lower_expr(e->v.Slice.upper) : none_node();
    n->children[2] = e->v.Slice.step ? lower_expr(e->v.Slice.step) : none_node();
    if (!n->children[0] || !n->children[1] || !n->children[2]) { ir_free_node(n); return NULL; }
    return n;
}

static PyXIRNode *lower_store(expr_ty target, PyXIRNode *value)
{
    PyXIRNode *n;
    if (target->kind == Name_kind) {
        n = name_node(target, PYX_IR_NAME_STORE);
        if (!n) { ir_free_node(value); return NULL; }
        n->left = value;
        return n;
    }
    if (target->kind == Attribute_kind) {
        n = ir_new(PYX_IR_SETATTR);
        if (!n) { ir_free_node(value); return NULL; }
        n->constant = PyUnicode_FromString(target->v.Attribute.attr);
        n->left = lower_expr(target->v.Attribute.value);
        n->right = value;
        if (!n->constant || !n->left) { ir_free_node(n); return NULL; }
        return n;
    }
    if (target->kind == Tuple_kind || target->kind == List_kind) {
        Py_ssize_t count = target->kind == Tuple_kind
            ? asdl_seq_LEN(target->v.Tuple.elts)
            : asdl_seq_LEN(target->v.List.elts);
        n = ir_new(PYX_IR_UNPACK);
        if (!n || set_children(n, count) < 0) { ir_free_node(n); ir_free_node(value); return NULL; }
        n->left = value;
        for (Py_ssize_t i = 0; i < count; ++i) {
            expr_ty item = target->kind == Tuple_kind
                ? (expr_ty)asdl_seq_GET(target->v.Tuple.elts, i)
                : (expr_ty)asdl_seq_GET(target->v.List.elts, i);
            if (item->kind == Starred_kind) {
                PyXIRNode *star = ir_new(PYX_IR_STAR_UNPACK);
                if (!star) { ir_free_node(n); return NULL; }
                star->left = lower_store(item->v.Starred.value, none_node());
                if (!star->left) { ir_free_node(star); ir_free_node(n); return NULL; }
                n->children[i] = star;
            } else {
                n->children[i] = lower_store(item, none_node());
            }
            if (!n->children[i]) { ir_free_node(n); return NULL; }
        }
        return n;
    }
    if (target->kind == Starred_kind) {
        n = ir_new(PYX_IR_STAR_UNPACK);
        if (!n) { ir_free_node(value); return NULL; }
        n->left = lower_store(target->v.Starred.value, value);
        if (!n->left) { ir_free_node(n); return NULL; }
        return n;
    }
    if (target->kind == Subscript_kind) {
        n = ir_new(PYX_IR_SUBSCRIPT_STORE);
        if (!n || set_children(n, 3) < 0) { ir_free_node(n); ir_free_node(value); return NULL; }
        n->children[0] = lower_expr(target->v.Subscript.value);
        n->children[1] = target->v.Subscript.slice->kind == Slice_kind
            ? lower_slice(target->v.Subscript.slice) : lower_expr(target->v.Subscript.slice);
        n->children[2] = value;
        if (!n->children[0] || !n->children[1]) { ir_free_node(n); return NULL; }
        return n;
    }
    PyErr_SetString(PyExc_NotImplementedError, "PythonX: unsupported assignment target");
    ir_free_node(value);
    return NULL;
}

static PyXIRNode *lower_aug_assign(stmt_ty s)
{
    PyXIRNode *n = ir_new(PYX_IR_AUG_ASSIGN);
    if (!n || set_children(n, 3) < 0) { ir_free_node(n); return NULL; }
    n->constant = PyLong_FromLong((long)s->v.AugAssign.op);
    if (!n->constant) { ir_free_node(n); return NULL; }
    n->children[0] = lower_expr(s->v.AugAssign.target);
    n->children[1] = lower_expr(s->v.AugAssign.value);
    n->children[2] = none_node();
    if (!n->children[0] || !n->children[1] || !n->children[2]) { ir_free_node(n); return NULL; }
    return n;
}

static PyXIRNode *lower_delete(expr_ty target)
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
            ? lower_slice(target->v.Subscript.slice) : lower_expr(target->v.Subscript.slice);
        if (!n->children[0] || !n->children[1]) { ir_free_node(n); return NULL; }
        return n;
    }
    if (target->kind == Tuple_kind || target->kind == List_kind) {
        Py_ssize_t count = target->kind == Tuple_kind
            ? asdl_seq_LEN(target->v.Tuple.elts)
            : asdl_seq_LEN(target->v.List.elts);
        n = ir_new(PYX_IR_SEQUENCE);
        if (!n || set_children(n, count) < 0) { ir_free_node(n); return NULL; }
        for (Py_ssize_t i = 0; i < count; ++i) {
            expr_ty item = target->kind == Tuple_kind
                ? (expr_ty)asdl_seq_GET(target->v.Tuple.elts, i)
                : (expr_ty)asdl_seq_GET(target->v.List.elts, i);
            n->children[i] = lower_delete(item);
            if (!n->children[i]) { ir_free_node(n); return NULL; }
        }
        return n;
    }
    PyErr_SetString(PyExc_NotImplementedError, "PythonX: unsupported delete target");
    return NULL;
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

static PyXIRNode *lower_for(stmt_ty s, int async_for)
{
    PyXIRNode *n = ir_new(async_for ? PYX_IR_ASYNC_FOR : PYX_IR_FOR);
    if (!n || set_children(n, 4) < 0) { ir_free_node(n); return NULL; }
    n->children[0] = lower_expr(s->v.For.iter);
    /* child 1 is the target descriptor; its value is filled by the loop backend. */
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
        PyXIRNode *enter = ir_new(async_with ? PYX_IR_ASYNC_WITH_ENTER : PYX_IR_WITH_ENTER);
        if (!enter || set_children(enter, 2) < 0) { ir_free_node(enter); ir_free_node(n); return NULL; }
        enter->children[0] = lower_expr(item->context_expr);
        enter->children[1] = item->optional_vars ? lower_expr(item->optional_vars) : none_node();
        if (!enter->children[0] || !enter->children[1]) { ir_free_node(enter); ir_free_node(n); return NULL; }
        n->children[i] = enter;
    }
    n->children[count] = lower_suite(s->v.With.body);
    if (!n->children[count]) { ir_free_node(n); return NULL; }
    return n;
}

static PyXIRNode *lower_lambda(expr_ty e)
{
    PyXIRNode *n = ir_new(PYX_IR_LAMBDA);
    arguments_ty a = e->v.Lambda.args;
    Py_ssize_t np = asdl_seq_LEN(a->posonlyargs) + asdl_seq_LEN(a->args);
    Py_ssize_t nk = asdl_seq_LEN(a->kwonlyargs);
    Py_ssize_t nd = asdl_seq_LEN(a->defaults);
    PyObject *meta = PyTuple_New(6);
    PyObject *names = PyTuple_New(np + nk);
    if (!n || !meta || !names) {
        Py_XDECREF(meta); Py_XDECREF(names); ir_free_node(n); return NULL;
    }

    PyTuple_SET_ITEM(meta, 0, PyLong_FromSsize_t(asdl_seq_LEN(a->posonlyargs)));
    PyTuple_SET_ITEM(meta, 1, PyLong_FromSsize_t(asdl_seq_LEN(a->args)));
    PyTuple_SET_ITEM(meta, 2, PyLong_FromSsize_t(nk));
    PyTuple_SET_ITEM(meta, 3, a->vararg ? PyUnicode_FromString(a->vararg->arg) : Py_NewRef(Py_None));
    PyTuple_SET_ITEM(meta, 4, a->kwarg ? PyUnicode_FromString(a->kwarg->arg) : Py_NewRef(Py_None));
    PyTuple_SET_ITEM(meta, 5, names);

    Py_ssize_t j = 0, i;
    for (i = 0; i < asdl_seq_LEN(a->posonlyargs); ++i)
        PyTuple_SET_ITEM(names, j++, PyUnicode_FromString(((arg_ty)asdl_seq_GET(a->posonlyargs, i))->arg));
    for (i = 0; i < asdl_seq_LEN(a->args); ++i)
        PyTuple_SET_ITEM(names, j++, PyUnicode_FromString(((arg_ty)asdl_seq_GET(a->args, i))->arg));
    for (i = 0; i < nk; ++i)
        PyTuple_SET_ITEM(names, j++, PyUnicode_FromString(((arg_ty)asdl_seq_GET(a->kwonlyargs, i))->arg));

    n->constant = meta;
    n->left = lower_expr(e->v.Lambda.body);
    if (!n->left) { ir_free_node(n); return NULL; }

    if (set_children(n, nd + nk) < 0) { ir_free_node(n); return NULL; }
    for (i = 0; i < nd; ++i) {
        n->children[i] = lower_expr((expr_ty)asdl_seq_GET(a->defaults, i));
        if (!n->children[i]) { ir_free_node(n); return NULL; }
    }
    for (i = 0; i < nk; ++i) {
        expr_ty d = (expr_ty)asdl_seq_GET(a->kw_defaults, i);
        n->children[nd + i] = d ? lower_expr(d) : NULL;
        if (d && !n->children[nd + i]) { ir_free_node(n); return NULL; }
    }
    return n;
}

static PyXIRNode *lower_call(expr_ty e)
{
    Py_ssize_t na=asdl_seq_LEN(e->v.Call.args), nk=asdl_seq_LEN(e->v.Call.keywords);
    PyXIRNode *n=ir_new(PYX_IR_CALL);
    if(!n||set_children(n,1+na+nk)<0){ir_free_node(n);return NULL;}
    n->children[0]=lower_expr(e->v.Call.func);
    n->constant=PyTuple_New(na+nk);
    if(!n->children[0]||!n->constant){ir_free_node(n);return NULL;}
    Py_ssize_t j=0;
    for(Py_ssize_t i=0;i<na;++i,++j){expr_ty a=(expr_ty)asdl_seq_GET(e->v.Call.args,i);int kind=a->kind==Starred_kind?1:0;n->children[1+j]=kind?lower_expr(a->v.Starred.value):lower_expr(a);PyObject*d=Py_BuildValue("iO",kind,Py_None);if(!n->children[1+j]||!d){Py_XDECREF(d);ir_free_node(n);return NULL;}PyTuple_SET_ITEM(n->constant,j,d);}
    for(Py_ssize_t i=0;i<nk;++i,++j){keyword_ty k=(keyword_ty)asdl_seq_GET(e->v.Call.keywords,i);int kind=k->arg?2:3;n->children[1+j]=lower_expr(k->value);PyObject*key=k->arg?PyUnicode_FromString(k->arg):Py_None;Py_INCREF(key);PyObject*d=Py_BuildValue("iO",kind,key);Py_DECREF(key);if(!n->children[1+j]||!d){Py_XDECREF(d);ir_free_node(n);return NULL;}PyTuple_SET_ITEM(n->constant,j,d);}
    return n;
}

static PyXIRNode *lower_expr(expr_ty e)
{
    PyXIRNode *n;
    switch(e->kind){
    case Constant_kind:return const_node(e->v.Constant.value);
    case Name_kind:return name_node(e,PYX_IR_NAME_LOAD);
    case List_kind:return lower_sequence(PYX_IR_LIST,e->v.List.elts);
    case Tuple_kind:return lower_sequence(PYX_IR_TUPLE,e->v.Tuple.elts);
    case Set_kind:return lower_sequence(PYX_IR_SET,e->v.Set.elts);
    case Dict_kind:{Py_ssize_t c=asdl_seq_LEN(e->v.Dict.keys);n=ir_new(PYX_IR_DICT);if(!n||set_children(n,c*2)<0){ir_free_node(n);return NULL;}for(Py_ssize_t i=0;i<c;++i){expr_ty k=(expr_ty)asdl_seq_GET(e->v.Dict.keys,i);n->children[i*2]=k?lower_expr(k):ir_new(PYX_IR_DICT_UNPACK);n->children[i*2+1]=lower_expr((expr_ty)asdl_seq_GET(e->v.Dict.values,i));if(!n->children[i*2]||!n->children[i*2+1]){ir_free_node(n);return NULL;}}return n;}
    case Attribute_kind:n=ir_new(PYX_IR_GETATTR);if(!n)return NULL;n->constant=PyUnicode_FromString(e->v.Attribute.attr);n->left=lower_expr(e->v.Attribute.value);if(!n->constant||!n->left){ir_free_node(n);return NULL;}return n;
    case Subscript_kind:n=ir_new(PYX_IR_SUBSCRIPT);if(!n||set_children(n,2)<0){ir_free_node(n);return NULL;}n->children[0]=lower_expr(e->v.Subscript.value);n->children[1]=e->v.Subscript.slice->kind==Slice_kind?lower_slice(e->v.Subscript.slice):lower_expr(e->v.Subscript.slice);if(!n->children[0]||!n->children[1]){ir_free_node(n);return NULL;}return n;
    case Slice_kind:return lower_slice(e);
    case JoinedStr_kind:{
        Py_ssize_t c=asdl_seq_LEN(e->v.JoinedStr.values);
        n=ir_new(PYX_IR_FSTRING);
        if(!n||set_children(n,c)<0){ir_free_node(n);return NULL;}
        for(Py_ssize_t i=0;i<c;++i){
            n->children[i]=lower_expr((expr_ty)asdl_seq_GET(e->v.JoinedStr.values,i));
            if(!n->children[i]){ir_free_node(n);return NULL;}
        }
        return n;
    }
    case FormattedValue_kind:{
        n=ir_new(PYX_IR_FORMAT_VALUE);
        if(!n||set_children(n,2)<0){ir_free_node(n);return NULL;}
        n->constant=PyLong_FromLong((long)e->v.FormattedValue.conversion);
        n->children[0]=lower_expr(e->v.FormattedValue.value);
        n->children[1]=e->v.FormattedValue.format_spec?lower_expr(e->v.FormattedValue.format_spec):none_node();
        if(!n->constant||!n->children[0]||!n->children[1]){ir_free_node(n);return NULL;}
        return n;
    }
    case Call_kind:return lower_call(e);
    case Lambda_kind:return lower_lambda(e);
    case Await_kind:n=ir_new(PYX_IR_AWAIT);if(!n)return NULL;n->left=lower_expr(e->v.Await.value);if(!n->left){ir_free_node(n);return NULL;}return n;
    case Starred_kind:n=ir_new(PYX_IR_STARRED);if(!n)return NULL;n->left=lower_expr(e->v.Starred.value);if(!n->left){ir_free_node(n);return NULL;}return n;
    case NamedExpr_kind:n=ir_new(PYX_IR_NAMED_EXPR);if(!n||set_children(n,2)<0){ir_free_node(n);return NULL;}n->children[0]=lower_expr(e->v.NamedExpr.target);n->children[1]=lower_expr(e->v.NamedExpr.value);if(!n->children[0]||!n->children[1]){ir_free_node(n);return NULL;}return n;
    case UnaryOp_kind:{PyXIROp op;switch(e->v.UnaryOp.op){case Invert:op=PYX_IR_INVERT;break;case UAdd:op=PYX_IR_POSITIVE;break;case USub:op=PYX_IR_NEGATIVE;break;case Not:op=PYX_IR_NOT;break;default:PyErr_SetString(PyExc_NotImplementedError,"PythonX: unary operator");return NULL;}n=ir_new(op);if(!n)return NULL;n->left=lower_expr(e->v.UnaryOp.operand);if(!n->left){ir_free_node(n);return NULL;}return n;}
    case BinOp_kind:{PyXIROp op;switch(e->v.BinOp.op){case Add:op=PYX_IR_ADD;break;case Sub:op=PYX_IR_SUB;break;case Mult:op=PYX_IR_MUL;break;case MatMult:op=PYX_IR_MATMUL;break;case Div:op=PYX_IR_DIV;break;case FloorDiv:op=PYX_IR_FLOORDIV;break;case Mod:op=PYX_IR_MOD;break;case Pow:op=PYX_IR_POW;break;case LShift:op=PYX_IR_LSHIFT;break;case RShift:op=PYX_IR_RSHIFT;break;case BitOr:op=PYX_IR_BITOR;break;case BitXor:op=PYX_IR_BITXOR;break;case BitAnd:op=PYX_IR_BITAND;break;default:PyErr_SetString(PyExc_NotImplementedError,"PythonX: binary operator");return NULL;}n=ir_new(op);if(!n)return NULL;n->left=lower_expr(e->v.BinOp.left);n->right=lower_expr(e->v.BinOp.right);if(!n->left||!n->right){ir_free_node(n);return NULL;}return n;}
    case BoolOp_kind:{Py_ssize_t c=asdl_seq_LEN(e->v.BoolOp.values);PyXIROp op=e->v.BoolOp.op==And?PYX_IR_AND:PYX_IR_OR;n=lower_expr((expr_ty)asdl_seq_GET(e->v.BoolOp.values,0));if(!n)return NULL;for(Py_ssize_t i=1;i<c;++i){PyXIRNode*r=ir_new(op);if(!r){ir_free_node(n);return NULL;}r->left=n;r->right=lower_expr((expr_ty)asdl_seq_GET(e->v.BoolOp.values,i));if(!r->right){ir_free_node(r);return NULL;}n=r;}return n;}
    case Compare_kind:{
        Py_ssize_t c=asdl_seq_LEN(e->v.Compare.ops);
        n=ir_new(c==1?PYX_IR_EQ:PYX_IR_COMPARE_CHAIN);
        if(!n)return NULL;
        if(c==1){
            PyXIROp op;
            switch((cmpop_ty)asdl_seq_GET(e->v.Compare.ops,0)){case Lt:op=PYX_IR_LT;break;case LtE:op=PYX_IR_LE;break;case Eq:op=PYX_IR_EQ;break;case NotEq:op=PYX_IR_NE;break;case Gt:op=PYX_IR_GT;break;case GtE:op=PYX_IR_GE;break;case Is:op=PYX_IR_IS;break;case IsNot:op=PYX_IR_IS_NOT;break;case In:op=PYX_IR_IN;break;case NotIn:op=PYX_IR_NOT_IN;break;default:PyErr_SetString(PyExc_NotImplementedError,"PythonX: comparison");ir_free_node(n);return NULL;}
            n->op=op;n->left=lower_expr(e->v.Compare.left);n->right=lower_expr((expr_ty)asdl_seq_GET(e->v.Compare.comparators,0));
            if(!n->left||!n->right){ir_free_node(n);return NULL;}return n;
        }
        if(set_children(n,c+1)<0){ir_free_node(n);return NULL;}
        n->children[0]=lower_expr(e->v.Compare.left);
        for(Py_ssize_t i=0;i<c;++i)n->children[i+1]=lower_expr((expr_ty)asdl_seq_GET(e->v.Compare.comparators,i));
        n->constant=PyTuple_New(c);
        if(!n->constant){ir_free_node(n);return NULL;}
        for(Py_ssize_t i=0;i<c;++i)PyTuple_SET_ITEM(n->constant,i,PyLong_FromLong((long)((cmpop_ty)asdl_seq_GET(e->v.Compare.ops,i))));
        for(Py_ssize_t i=0;i<c+1;++i)if(!n->children[i]){ir_free_node(n);return NULL;}
        return n;
    }
    default:PyErr_Format(PyExc_NotImplementedError,"PythonX IR: unsupported expression kind %d",(int)e->kind);return NULL;
    }
}

static PyXIRNode *lower_function(stmt_ty s)
{
    PyXIRNode *n = ir_new(PYX_IR_FUNCTION);
    if (!n) return NULL;

    arguments_ty a = s->v.FunctionDef.args;
    Py_ssize_t np = asdl_seq_LEN(a->posonlyargs) + asdl_seq_LEN(a->args);
    Py_ssize_t nk = asdl_seq_LEN(a->kwonlyargs);
    Py_ssize_t nd = asdl_seq_LEN(a->defaults);
    PyObject *meta = PyTuple_New(7);
    PyObject *names = PyTuple_New(np + nk);
    if (!meta || !names) {
        Py_XDECREF(meta);
        Py_XDECREF(names);
        ir_free_node(n);
        return NULL;
    }

    PyTuple_SET_ITEM(meta, 0, PyLong_FromSsize_t(asdl_seq_LEN(a->posonlyargs)));
    PyTuple_SET_ITEM(meta, 1, PyLong_FromSsize_t(asdl_seq_LEN(a->args)));
    PyTuple_SET_ITEM(meta, 2, PyLong_FromSsize_t(nk));
    PyTuple_SET_ITEM(meta, 3, a->vararg ? PyUnicode_FromString(a->vararg->arg) : Py_NewRef(Py_None));
    PyTuple_SET_ITEM(meta, 4, a->kwarg ? PyUnicode_FromString(a->kwarg->arg) : Py_NewRef(Py_None));
    PyTuple_SET_ITEM(meta, 5, names);
    PyTuple_SET_ITEM(meta, 6, PyUnicode_FromString(s->v.FunctionDef.name));

    Py_ssize_t j = 0;
    for (Py_ssize_t i = 0; i < asdl_seq_LEN(a->posonlyargs); ++i)
        PyTuple_SET_ITEM(names, j++, PyUnicode_FromString(((arg_ty)asdl_seq_GET(a->posonlyargs, i))->arg));
    for (Py_ssize_t i = 0; i < asdl_seq_LEN(a->args); ++i)
        PyTuple_SET_ITEM(names, j++, PyUnicode_FromString(((arg_ty)asdl_seq_GET(a->args, i))->arg));
    for (Py_ssize_t i = 0; i < nk; ++i)
        PyTuple_SET_ITEM(names, j++, PyUnicode_FromString(((arg_ty)asdl_seq_GET(a->kwonlyargs, i))->arg));

    n->constant = meta;
    n->left = lower_suite(s->v.FunctionDef.body);
    if (!n->left) {
        ir_free_node(n);
        return NULL;
    }

    if (set_children(n, nd + nk) < 0) {
        ir_free_node(n);
        return NULL;
    }
    for (Py_ssize_t i = 0; i < nd; ++i) {
        n->children[i] = lower_expr((expr_ty)asdl_seq_GET(a->defaults, i));
        if (!n->children[i]) {
            ir_free_node(n);
            return NULL;
        }
    }
    for (Py_ssize_t i = 0; i < nk; ++i) {
        expr_ty d = (expr_ty)asdl_seq_GET(a->kw_defaults, i);
        n->children[nd + i] = d ? lower_expr(d) : NULL;
        if (d && !n->children[nd + i]) {
            ir_free_node(n);
            return NULL;
        }
    }
    return n;
}

static PyXIRNode *lower_stmt(stmt_ty s)
{
    PyXIRNode *n;
    switch(s->kind){
    case Expr_kind:return lower_expr(s->v.Expr.value);
    case FunctionDef_kind:return lower_function(s);
    case If_kind:return lower_if(s);
    case While_kind:return lower_while(s);
    case For_kind:return lower_for(s,0);
    case AsyncFor_kind:return lower_for(s,1);
    case With_kind:return lower_with(s,0);
    case AsyncWith_kind:return lower_with(s,1);
    case Break_kind:return ir_new(PYX_IR_BREAK);
    case Continue_kind:return ir_new(PYX_IR_CONTINUE);
    case Delete_kind:{Py_ssize_t c=asdl_seq_LEN(s->v.Delete.targets);n=ir_new(PYX_IR_SEQUENCE);if(!n||set_children(n,c)<0){ir_free_node(n);return NULL;}for(Py_ssize_t i=0;i<c;++i){n->children[i]=lower_delete((expr_ty)asdl_seq_GET(s->v.Delete.targets,i));if(!n->children[i]){ir_free_node(n);return NULL;}}return n;}
    case AugAssign_kind:return lower_aug_assign(s);
    case Assign_kind:{
        Py_ssize_t c=asdl_seq_LEN(s->v.Assign.targets);
        if(c==1)return lower_store((expr_ty)asdl_seq_GET(s->v.Assign.targets,0),lower_expr(s->v.Assign.value));
        n=ir_new(PYX_IR_ASSIGN_CHAIN);
        if(!n||set_children(n,c)<0){ir_free_node(n);return NULL;}
        n->left=lower_expr(s->v.Assign.value);
        if(!n->left){ir_free_node(n);return NULL;}
        for(Py_ssize_t i=0;i<c;++i){
            expr_ty target=(expr_ty)asdl_seq_GET(s->v.Assign.targets,i);
            n->children[i]=lower_store(target,none_node());
            if(!n->children[i]){ir_free_node(n);return NULL;}
        }
        return n;
    }
    case AnnAssign_kind:return s->v.AnnAssign.value?lower_store(s->v.AnnAssign.target,lower_expr(s->v.AnnAssign.value)):lower_expr(s->v.AnnAssign.annotation);
    case Return_kind:n=ir_new(PYX_IR_RETURN);if(!n)return NULL;n->left=s->v.Return.value?lower_expr(s->v.Return.value):none_node();if(!n->left){ir_free_node(n);return NULL;}return n;
    case Raise_kind:n=ir_new(s->v.Raise.exc?PYX_IR_RAISE:PYX_IR_RERAISE);if(!n)return NULL;if(s->v.Raise.exc){if(set_children(n,2)<0){ir_free_node(n);return NULL;}n->children[0]=lower_expr(s->v.Raise.exc);n->children[1]=s->v.Raise.cause?lower_expr(s->v.Raise.cause):none_node();if(!n->children[0]||!n->children[1]){ir_free_node(n);return NULL;}}return n;
    case Assert_kind:n=ir_new(PYX_IR_ASSERT);if(!n||set_children(n,2)<0){ir_free_node(n);return NULL;}n->children[0]=lower_expr(s->v.Assert.test);n->children[1]=s->v.Assert.msg?lower_expr(s->v.Assert.msg):none_node();if(!n->children[0]||!n->children[1]){ir_free_node(n);return NULL;}return n;
    default:PyErr_Format(PyExc_NotImplementedError,"PythonX IR: unsupported statement kind %d",(int)s->kind);return NULL;
    }
}

int _PyX_IR_FromAST(mod_ty module, PyXIRFunction *function)
{
    if(!module||!function){PyErr_SetString(PyExc_TypeError,"PythonX IR requires an AST module");return -1;}
    asdl_stmt_seq *body=NULL;
    if(module->kind==Module_kind) body=module->v.Module.body;
    else if(module->kind==Interactive_kind) body=module->v.Interactive.body;
    else {PyErr_SetString(PyExc_NotImplementedError,"PythonX IR: unsupported module kind");return -1;}
    PyXIRNode *root=lower_suite(body);if(!root)return -1;function->root=root;function->globals=NULL;return 0;
}

PyObject *_PyX_IR_Evaluate(const PyXIRNode *node, PyObject *globals)
{
    Py_UNUSED(node);Py_UNUSED(globals);
    PyErr_SetString(PyExc_RuntimeError,"PythonX IR evaluator is being replaced by the native backend; this function must not be used for execution");
    return NULL;
}

const char *_PyX_IR_OpName(PyXIROp op)
{
    switch(op){case PYX_IR_IF:return "if";case PYX_IR_WHILE:return "while";case PYX_IR_FOR:return "for";case PYX_IR_ASYNC_FOR:return "async_for";case PYX_IR_BREAK:return "break";case PYX_IR_CONTINUE:return "continue";case PYX_IR_WITH:return "with";case PYX_IR_ASYNC_WITH:return "async_with";case PYX_IR_AWAIT:return "await";case PYX_IR_LAMBDA:return "lambda";case PYX_IR_DELETE:return "delete";default:return "pythonx_ir";}
}
