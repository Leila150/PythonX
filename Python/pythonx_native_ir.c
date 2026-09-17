#include "Python.h"
#include "pythonx_ir.h"
#include "pythonx_native_ir.h"
#include "pythonx_error_statements.h"
#include <stdint.h>
#include <string.h>
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

/*
 * The native entrypoint calls this evaluator for now.  Control flow is real
 * IR control flow: break and continue are propagated as compiler state, not
 * Python exceptions.  The next native-codegen stage can replace individual
 * eval_* operations without changing the IR semantics.
 */
typedef enum { PX_NORMAL=0, PX_BREAK, PX_CONTINUE, PX_RETURN } PXFlow;
typedef struct { PXFlow flow; PyObject *value; } PXState;

static PyObject *px_eval(const PyXIRNode*,PyObject*,PXState*);

static PyObject *px_suite(const PyXIRNode*n,PyObject*g,PXState*s)
{
    PyObject*r=Py_NewRef(Py_None);
    for(Py_ssize_t i=0;i<n->child_count;i++){
        PyObject*x=px_eval(n->children[i],g,s);
        if(!x){Py_DECREF(r);return NULL;}
        Py_SETREF(r,x);
        if(s->flow!=PX_NORMAL)break;
    }
    return r;
}

static PyObject *px_call(const PyXIRNode*n,PyObject*g,PXState*s)
{
    PyObject*c=px_eval(n->children[0],g,s),*args=PyList_New(0),*kw=PyDict_New();
    if(!c)return NULL;
    if(!args||!kw){Py_XDECREF(args);Py_XDECREF(kw);Py_DECREF(c);return NULL;}
    Py_ssize_t count=n->child_count-1;
    for(Py_ssize_t i=0;i<count;i++){
        PyObject*d=PyTuple_GET_ITEM(n->constant,i);
        int kind=(int)PyLong_AsLong(PyTuple_GET_ITEM(d,0));
        PyObject*v=px_eval(n->children[i+1],g,s);
        if(!v)goto error;
        if(kind==0){if(PyList_Append(args,v)<0){Py_DECREF(v);goto error;}}
        else if(kind==1){PyObject*seq=PySequence_Fast(v,"* argument must be an iterable");if(!seq){Py_DECREF(v);goto error;}for(Py_ssize_t j=0;j<PySequence_Fast_GET_SIZE(seq);j++)if(PyList_Append(args,PySequence_Fast_GET_ITEM(seq,j))<0){Py_DECREF(seq);Py_DECREF(v);goto error;}Py_DECREF(seq);}
        else if(kind==2){if(PyDict_SetItem(kw,PyTuple_GET_ITEM(d,1),v)<0){Py_DECREF(v);goto error;}}
        else if(kind==3){if(PyDict_Update(kw,v)<0){Py_DECREF(v);goto error;}}
        Py_DECREF(v);
    }
    {PyObject*a=PyList_AsTuple(args),*r;if(!a)goto error;r=PyObject_Call(c,a,kw);Py_DECREF(a);Py_DECREF(args);Py_DECREF(kw);Py_DECREF(c);return r;}
error:Py_DECREF(args);Py_DECREF(kw);Py_DECREF(c);return NULL;
}

static PyObject *px_await(PyObject*awaitable)
{
    PyObject*it=PyObject_CallMethod(awaitable,"__await__",NULL);
    if(!it)return NULL;
    for(;;){
        PyObject*x=PyIter_Next(it);
        if(!x){
            if(PyErr_Occurred()&&!PyErr_ExceptionMatches(PyExc_StopIteration)){Py_DECREF(it);return NULL;}
            PyObject*t=NULL,*v=NULL,*tb=NULL;PyErr_Fetch(&t,&v,&tb);Py_XDECREF(t);Py_XDECREF(tb);Py_DECREF(it);return v?v:Py_NewRef(Py_None);
        }
        /* A yielded object means this await actually suspended.  A native
           PythonX execution frame will later turn this into a real suspend
           point; do not silently discard it. */
        Py_DECREF(x);Py_DECREF(it);PyErr_SetString(PyExc_RuntimeError,"PythonX: await suspended before native async frame support");return NULL;
    }
}

static int px_store(const PyXIRNode*n,PyObject*g,PyObject*v,PXState*s)
{
    if(n->op==PYX_IR_NAME_STORE){int rc=PyDict_SetItem(g,n->constant,v);return rc;}
    if(n->op==PYX_IR_SETATTR){PyObject*o=px_eval(n->left,g,s);if(!o)return-1;int rc=PyObject_SetAttr(o,n->constant,v);Py_DECREF(o);return rc;}
    if(n->op==PYX_IR_SUBSCRIPT_STORE){PyObject*o=px_eval(n->children[0],g,s),*k=px_eval(n->children[1],g,s);if(!o||!k){Py_XDECREF(o);Py_XDECREF(k);return-1;}int rc=PyObject_SetItem(o,k,v);Py_DECREF(o);Py_DECREF(k);return rc;}
    PyErr_SetString(PyExc_SystemError,"invalid PythonX loop target");return-1;
}

static PyObject *px_for(const PyXIRNode*n,PyObject*g,PXState*s,int async_for)
{
    PyObject*source=px_eval(n->children[0],g,s);if(!source)return NULL;
    PyObject*it=async_for?PyObject_CallMethod(source,"__aiter__",NULL):PyObject_GetIter(source);Py_DECREF(source);if(!it)return NULL;
    PyObject*r=Py_NewRef(Py_None);int broke=0;
    for(;;){
        PyObject*item=NULL;
        if(async_for){
            PyObject*a=PyObject_CallMethod(it,"__anext__",NULL);if(!a){if(PyErr_ExceptionMatches(PyExc_StopAsyncIteration)){PyErr_Clear();break;}Py_DECREF(it);Py_DECREF(r);return NULL;}
            item=px_await(a);Py_DECREF(a);if(!item){Py_DECREF(it);Py_DECREF(r);return NULL;}
        }else{
            item=PyIter_Next(it);
            if(!item){if(PyErr_Occurred()){Py_DECREF(it);Py_DECREF(r);return NULL;}break;}
        }
        PyXIRNode*t=n->children[1];PyObject*old=NULL;
        if(t->op==PYX_IR_NAME_STORE){old=t->left;t->left=cn(item);if(!t->left){Py_DECREF(item);break;}}
        else if(t->op==PYX_IR_SETATTR){old=t->right;t->right=cn(item);if(!t->right){Py_DECREF(item);break;}}
        else if(t->op==PYX_IR_SUBSCRIPT_STORE){old=t->children[2];t->children[2]=cn(item);if(!t->children[2]){Py_DECREF(item);break;}}
        else {Py_DECREF(item);PyErr_SetString(PyExc_SystemError,"invalid PythonX loop target");break;}
        Py_DECREF(item);
        PyObject*x=px_eval(t,g,s);
        if(t->op==PYX_IR_NAME_STORE){Py_XDECREF(old);t->left=NULL;}
        else if(t->op==PYX_IR_SETATTR){Py_XDECREF(old);t->right=NULL;}
        else {Py_XDECREF(old);t->children[2]=NULL;}
        if(!x){Py_DECREF(it);Py_DECREF(r);return NULL;}Py_DECREF(x);
        x=px_eval(n->children[2],g,s);if(!x){Py_DECREF(it);Py_DECREF(r);return NULL;}Py_SETREF(r,x);
        if(s->flow==PX_BREAK){s->flow=PX_NORMAL;broke=1;break;}
        if(s->flow==PX_CONTINUE){s->flow=PX_NORMAL;continue;}
        if(s->flow!=PX_NORMAL){Py_DECREF(it);Py_DECREF(r);return NULL;}
    }
    Py_DECREF(it);
    if(!broke){PyObject*x=px_eval(n->children[3],g,s);if(!x){Py_DECREF(r);return NULL;}Py_SETREF(r,x);}
    return r;
}

static PyObject *px_with(const PyXIRNode*n,PyObject*g,PXState*s,int async_with)
{
    Py_ssize_t count=n->child_count-1;PyObject**mgr=PyMem_Calloc((size_t)count,sizeof(*mgr));if(!mgr){PyErr_NoMemory();return NULL;}
    Py_ssize_t entered=0;PyObject*r=NULL;
    for(Py_ssize_t i=0;i<count;i++){
        PyXIRNode*x=n->children[i];PyObject*m=px_eval(x->children[0],g,s);if(!m)goto fail;
        PyObject*v=async_with?PyObject_CallMethod(m,"__aenter__",NULL):PyObject_CallMethod(m,"__enter__",NULL);if(!v){Py_DECREF(m);goto fail;}
        if(async_with){PyObject*z=px_await(v);Py_DECREF(v);v=z;if(!v){Py_DECREF(m);goto fail;}}
        mgr[entered++]=m;
        if(x->children[1]->op!=PYX_IR_CONST||x->children[1]->constant!=Py_None){if(px_store(x->children[1],g,v,s)<0){Py_DECREF(v);goto fail;}}
        Py_DECREF(v);
    }
    r=px_eval(n->children[count],g,s);
    for(Py_ssize_t i=entered;i-->0;){PyObject*v;if(async_with)v=PyObject_CallMethod(mgr[i],"__aexit__",Py_None,Py_None,Py_None);else v=PyObject_CallMethod(mgr[i],"__exit__",Py_None,Py_None,Py_None);if(async_with&&v){PyObject*z=px_await(v);Py_DECREF(v);v=z;}Py_XDECREF(v);Py_DECREF(mgr[i]);}
    PyMem_Free(mgr);return r;
fail:
    for(Py_ssize_t i=entered;i-->0;){PyObject*v=async_with?PyObject_CallMethod(mgr[i],"__aexit__",Py_None,Py_None,Py_None):PyObject_CallMethod(mgr[i],"__exit__",Py_None,Py_None,Py_None);Py_XDECREF(v);Py_DECREF(mgr[i]);}
    PyMem_Free(mgr);return NULL;
}

static PyObject *px_eval(const PyXIRNode*n,PyObject*g,PXState*s)
{
    if(!n)return Py_NewRef(Py_None);
    switch(n->op){
    case PYX_IR_CONST:return Py_NewRef(n->constant);
    case PYX_IR_SEQUENCE:return px_suite(n,g,s);
    case PYX_IR_NAME_LOAD:{PyObject*v=PyDict_GetItemWithError(g,n->constant);if(!v){if(!PyErr_Occurred())PyErr_Format(PyExc_NameError,"name '%U' is not defined",n->constant);return NULL;}return Py_NewRef(v);}
    case PYX_IR_NAME_STORE:{PyObject*v=px_eval(n->left,g,s);if(!v)return NULL;int rc=PyDict_SetItem(g,n->constant,v);Py_DECREF(v);return rc<0?NULL:Py_NewRef(Py_None);}
    case PYX_IR_DELETE:{if(n->constant){if(PyDict_DelItem(g,n->constant)<0)return NULL;return Py_NewRef(Py_None);}PyObject*o=px_eval(n->children[0],g,s),*k=px_eval(n->children[1],g,s);if(!o||!k){Py_XDECREF(o);Py_XDECREF(k);return NULL;}int rc=PyObject_DelItem(o,k);Py_DECREF(o);Py_DECREF(k);return rc<0?NULL:Py_NewRef(Py_None);}
    case PYX_IR_GETATTR:{PyObject*o=px_eval(n->left,g,s);if(!o)return NULL;PyObject*r=PyObject_GetAttr(o,n->constant);Py_DECREF(o);return r;}
    case PYX_IR_SETATTR:{PyObject*o=px_eval(n->left,g,s),*v=px_eval(n->right,g,s);if(!o||!v){Py_XDECREF(o);Py_XDECREF(v);return NULL;}int rc=PyObject_SetAttr(o,n->constant,v);Py_DECREF(o);Py_DECREF(v);return rc<0?NULL:Py_NewRef(Py_None);}
    case PYX_IR_SUBSCRIPT:{PyObject*o=px_eval(n->children[0],g,s),*k=px_eval(n->children[1],g,s);if(!o||!k){Py_XDECREF(o);Py_XDECREF(k);return NULL;}PyObject*r=PyObject_GetItem(o,k);Py_DECREF(o);Py_DECREF(k);return r;}
    case PYX_IR_SUBSCRIPT_STORE:{PyObject*o=px_eval(n->children[0],g,s),*k=px_eval(n->children[1],g,s),*v=px_eval(n->children[2],g,s);if(!o||!k||!v){Py_XDECREF(o);Py_XDECREF(k);Py_XDECREF(v);return NULL;}int rc=PyObject_SetItem(o,k,v);Py_DECREF(o);Py_DECREF(k);Py_DECREF(v);return rc<0?NULL:Py_NewRef(Py_None);}
    case PYX_IR_LIST:case PYX_IR_TUPLE:case PYX_IR_SET:{PyObject*r=n->op==PYX_IR_LIST?PyList_New(n->child_count):n->op==PYX_IR_TUPLE?PyTuple_New(n->child_count):PySet_New(NULL);if(!r)return NULL;for(Py_ssize_t i=0;i<n->child_count;i++){PyObject*v=px_eval(n->children[i],g,s);if(!v){Py_DECREF(r);return NULL;}if(n->op==PYX_IR_LIST)PyList_SET_ITEM(r,i,v);else if(n->op==PYX_IR_TUPLE)PyTuple_SET_ITEM(r,i,v);else{int rc=PySet_Add(r,v);Py_DECREF(v);if(rc<0){Py_DECREF(r);return NULL;}}}return r;}
    case PYX_IR_DICT:{PyObject*r=PyDict_New();if(!r)return NULL;for(Py_ssize_t i=0;i<n->child_count;i+=2){PyObject*k=px_eval(n->children[i],g,s),*v=px_eval(n->children[i+1],g,s);if(!k||!v){Py_XDECREF(k);Py_XDECREF(v);Py_DECREF(r);return NULL;}int rc=PyDict_SetItem(r,k,v);Py_DECREF(k);Py_DECREF(v);if(rc<0){Py_DECREF(r);return NULL;}}return r;}
    case PYX_IR_CALL:return px_call(n,g,s);
    case PYX_IR_IF:{PyObject*t=px_eval(n->children[0],g,s);if(!t)return NULL;int truth=PyObject_IsTrue(t);Py_DECREF(t);if(truth<0)return NULL;return px_eval(n->children[truth?1:2],g,s);}
    case PYX_IR_WHILE:{PyObject*r=Py_NewRef(Py_None);int broke=0;for(;;){PyObject*t=px_eval(n->children[0],g,s);if(!t){Py_DECREF(r);return NULL;}int truth=PyObject_IsTrue(t);Py_DECREF(t);if(truth<0){Py_DECREF(r);return NULL;}if(!truth)break;PyObject*x=px_eval(n->children[1],g,s);if(!x){Py_DECREF(r);return NULL;}Py_DECREF(x);if(s->flow==PX_BREAK){s->flow=PX_NORMAL;broke=1;break;}if(s->flow==PX_CONTINUE){s->flow=PX_NORMAL;continue;}if(s->flow!=PX_NORMAL){Py_DECREF(r);return NULL;}}if(!broke){PyObject*x=px_eval(n->children[2],g,s);if(!x){Py_DECREF(r);return NULL;}Py_SETREF(r,x);}return r;}
    case PYX_IR_FOR:return px_for(n,g,s,0);
    case PYX_IR_ASYNC_FOR:return px_for(n,g,s,1);
    case PYX_IR_BREAK:s->flow=PX_BREAK;return Py_NewRef(Py_None);
    case PYX_IR_CONTINUE:s->flow=PX_CONTINUE;return Py_NewRef(Py_None);
    case PYX_IR_RETURN:s->flow=PX_RETURN;return n->left?px_eval(n->left,g,s):Py_NewRef(Py_None);
    case PYX_IR_AWAIT:{PyObject*a=px_eval(n->left,g,s);if(!a)return NULL;PyObject*r=px_await(a);Py_DECREF(a);return r;}
    case PYX_IR_WITH:return px_with(n,g,s,0);
    case PYX_IR_ASYNC_WITH:return px_with(n,g,s,1);
    case PYX_IR_RAISE:{PyObject*e=px_eval(n->children[0],g,s),*c=n->child_count>1?px_eval(n->children[1],g,s):NULL;if(!e){Py_XDECREF(c);return NULL;}int rc=_PyX_StatementRaise(e,c);Py_DECREF(e);Py_XDECREF(c);return rc<0?NULL:Py_NewRef(Py_None);}
    case PYX_IR_RERAISE:return _PyX_StatementReraise()<0?NULL:Py_NewRef(Py_None);
    case PYX_IR_ASSERT:{PyObject*t=px_eval(n->children[0],g,s),*m=px_eval(n->children[1],g,s);if(!t||!m){Py_XDECREF(t);Py_XDECREF(m);return NULL;}int rc=_PyX_StatementAssert(t,m);Py_DECREF(t);Py_DECREF(m);return rc<0?NULL:Py_NewRef(Py_None);}
    case PYX_IR_NOT:case PYX_IR_INVERT:case PYX_IR_POSITIVE:case PYX_IR_NEGATIVE:{PyObject*v=px_eval(n->left,g,s);if(!v)return NULL;PyObject*r;if(n->op==PYX_IR_NOT){int t=PyObject_IsTrue(v);r=t<0?NULL:PyBool_FromLong(!t);}else if(n->op==PYX_IR_INVERT)r=PyNumber_Invert(v);else if(n->op==PYX_IR_POSITIVE)r=PyNumber_Positive(v);else r=PyNumber_Negative(v);Py_DECREF(v);return r;}
    default:break;
    }
    if(n->op>=PYX_IR_ADD&&n->op<=PYX_IR_OR){PyObject*l=px_eval(n->left,g,s);if(!l)return NULL;if(n->op==PYX_IR_AND||n->op==PYX_IR_OR){int t=PyObject_IsTrue(l);if(t<0){Py_DECREF(l);return NULL;}if((n->op==PYX_IR_AND&&!t)||(n->op==PYX_IR_OR&&t))return l;}PyObject*r=px_eval(n->right,g,s);if(!r){Py_DECREF(l);return NULL;}PyObject*z=NULL;switch(n->op){case PYX_IR_ADD:z=PyNumber_Add(l,r);break;case PYX_IR_SUB:z=PyNumber_Subtract(l,r);break;case PYX_IR_MUL:z=PyNumber_Multiply(l,r);break;case PYX_IR_MATMUL:z=PyNumber_MatrixMultiply(l,r);break;case PYX_IR_DIV:z=PyNumber_TrueDivide(l,r);break;case PYX_IR_FLOORDIV:z=PyNumber_FloorDivide(l,r);break;case PYX_IR_MOD:z=PyNumber_Remainder(l,r);break;case PYX_IR_POW:z=PyNumber_Power(l,r,Py_None);break;case PYX_IR_LSHIFT:z=PyNumber_Lshift(l,r);break;case PYX_IR_RSHIFT:z=PyNumber_Rshift(l,r);break;case PYX_IR_BITOR:z=PyNumber_Or(l,r);break;case PYX_IR_BITXOR:z=PyNumber_Xor(l,r);break;case PYX_IR_BITAND:z=PyNumber_And(l,r);break;case PYX_IR_LT:z=PyObject_RichCompare(l,r,Py_LT);break;case PYX_IR_LE:z=PyObject_RichCompare(l,r,Py_LE);break;case PYX_IR_EQ:z=PyObject_RichCompare(l,r,Py_EQ);break;case PYX_IR_NE:z=PyObject_RichCompare(l,r,Py_NE);break;case PYX_IR_GT:z=PyObject_RichCompare(l,r,Py_GT);break;case PYX_IR_GE:z=PyObject_RichCompare(l,r,Py_GE);break;case PYX_IR_IS:z=PyBool_FromLong(l==r);break;case PYX_IR_IS_NOT:z=PyBool_FromLong(l!=r);break;case PYX_IR_IN:{int x=PySequence_Contains(r,l);z=x<0?NULL:PyBool_FromLong(x);break;}case PYX_IR_NOT_IN:{int x=PySequence_Contains(r,l);z=x<0?NULL:PyBool_FromLong(!x);break;}default:z=Py_NewRef(r);break;}Py_DECREF(l);Py_DECREF(r);return z;}
    PyErr_Format(PyExc_NotImplementedError,"PythonX native IR: operation %d is not executable yet",(int)n->op);return NULL;
}

PyObject *_PyX_NativeEvaluateIR(const PyXIRNode*n,PyObject*g){if(!n||!g||!PyDict_Check(g)){PyErr_SetString(PyExc_TypeError,"invalid PythonX native IR state");return NULL;}PXState s={PX_NORMAL,NULL};PyObject*r=px_eval(n,g,&s);if(!r)return NULL;if(s.flow==PX_BREAK||s.flow==PX_CONTINUE){Py_DECREF(r);PyErr_SetString(PyExc_SyntaxError,"break/continue outside loop");return NULL;}return r;}

#if defined(__x86_64__) || defined(_M_X64)
typedef struct {unsigned char*code;size_t size;} XIRCode;
typedef struct {unsigned char*code;size_t size;PyXIRNode*root;PyObject*globals;} XIRNativeCode;
typedef PyObject*(*XIRNativeFunction)(void);
static int reserve_code(XIRCode*c,size_t n){if(n<=c->size)return 0;size_t cap=c->size?c->size:64;while(cap<n)cap*=2;unsigned char*p=PyMem_RawRealloc(c->code,cap);if(!p){PyErr_NoMemory();return-1;}c->code=p;c->size=cap;return 0;}
static int bytes_code(XIRCode*c,const unsigned char*p,size_t n){size_t old=c->size?c->size:0; if(reserve_code(c,old+n)<0)return-1;memcpy(c->code+old,p,n);c->size=old+n;return 0;}
static int imm64(XIRCode*c,uint64_t v){unsigned char b[8];for(int i=0;i<8;i++)b[i]=(unsigned char)(v>>(i*8));return bytes_code(c,b,8);}
static int emit_arg(XIRCode*c,uint64_t v,int arg){#if defined(_WIN32) const unsigned char op1[]={0x48,0xB9},op2[]={0x48,0xBA};#else const unsigned char op1[]={0x48,0xBF},op2[]={0x48,0xBE};#endif const unsigned char*op=arg==1?op1:op2;return bytes_code(c,op,2)||imm64(c,v);}
static int emit_call(XIRCode*c,uint64_t fn){const unsigned char mov[]={0x48,0xB8},call[]={0xFF,0xD0},ret[]={0xC3};return bytes_code(c,mov,2)||imm64(c,fn)||bytes_code(c,call,2)||bytes_code(c,ret,1);}
static void*alloc_exec(size_t n){#if defined(_WIN32)return VirtualAlloc(NULL,n,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);#else void*p=mmap(NULL,n,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);return p==MAP_FAILED?NULL:p;#endif}
static void free_exec(void*p,size_t n){#if defined(_WIN32)(void)n;if(p)VirtualFree(p,0,MEM_RELEASE);#else if(p)munmap(p,n);#endif}
static void capsule_free(PyObject*c){XIRNativeCode*n=PyCapsule_GetPointer(c,"PythonX.native_ir_code");if(!n){PyErr_Clear();return;}free_exec(n->code,n->size);PyXIRFunction f={n->root,n->globals};_PyX_IR_Free(&f);PyMem_RawFree(n);}
PyObject*_PyX_NativeCompileIR(const PyXIRFunction*f){if(!f||!f->root){PyErr_SetString(PyExc_TypeError,"PythonX native IR compiler requires a function");return NULL;}XIRCode c={0};PyObject*g=f->globals?Py_NewRef(f->globals):PyDict_New();if(!g)return NULL;if(emit_arg(&c,(uint64_t)(uintptr_t)f->root,1)<0||emit_arg(&c,(uint64_t)(uintptr_t)g,2)<0||emit_call(&c,(uint64_t)(uintptr_t)&_PyX_NativeEvaluateIR)<0){Py_DECREF(g);PyMem_RawFree(c.code);return NULL;}void*m=alloc_exec(c.size);if(!m){Py_DECREF(g);PyMem_RawFree(c.code);PyErr_SetString(PyExc_MemoryError,"PythonX could not allocate executable memory");return NULL;}memcpy(m,c.code,c.size);PyMem_RawFree(c.code);XIRNativeCode*n=PyMem_RawMalloc(sizeof(*n));if(!n){free_exec(m,c.size);Py_DECREF(g);PyErr_NoMemory();return NULL;}n->code=m;n->size=c.size;n->root=f->root;n->globals=g;((PyXIRFunction*)f)->root=NULL;((PyXIRFunction*)f)->globals=NULL;PyObject*cap=PyCapsule_New(n,"PythonX.native_ir_code",capsule_free);if(!cap){free_exec(m,c.size);Py_DECREF(g);PyMem_RawFree(n);return NULL;}return cap;}
PyObject*_PyX_NativeExecuteIR(PyObject*code){if(!PyCapsule_IsValid(code,"PythonX.native_ir_code")){PyErr_SetString(PyExc_TypeError,"invalid PythonX native IR code");return NULL;}XIRNativeCode*n=PyCapsule_GetPointer(code,"PythonX.native_ir_code");if(!n||!n->code||!n->globals){PyErr_SetString(PyExc_RuntimeError,"empty PythonX native code");return NULL;}return((XIRNativeFunction)n->code)();}
#else
PyObject*_PyX_NativeCompileIR(const PyXIRFunction*f){Py_UNUSED(f);PyErr_SetString(PyExc_NotImplementedError,"PythonX native backend currently targets x86-64");return NULL;}
PyObject*_PyX_NativeExecuteIR(PyObject*c){Py_UNUSED(c);PyErr_SetString(PyExc_NotImplementedError,"PythonX native backend currently targets x86-64");return NULL;}
#endif
