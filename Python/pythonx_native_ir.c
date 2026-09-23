#include "Python.h"
#include "pythonx_ir.h"
#include "pythonx_native_ir.h"
#include "pythonx_error_statements.h"
#include "pycore_pythonxobject.h"
#include <structmember.h>
#include <stdint.h>
#include <string.h>
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

typedef enum { PX_NORMAL=0, PX_BREAK, PX_CONTINUE, PX_RETURN } PXFlow;
typedef struct { PXFlow flow; int loop_depth; } PXState;

static PyObject *px_eval(const PyXIRNode *, PyObject *, PXState *);
static PyObject *px_name_load(PyObject *globals, PyObject *name)
{
    PyObject *value = PyDict_GetItemWithError(globals, name);
    if (value) return Py_NewRef(value);
    if (PyErr_Occurred()) return NULL;

    /* Function-local name resolution: locals -> defining scopes -> globals. */
    PyObject *parent = PyDict_GetItemString(globals, "__pythonx_parent__");
    if (parent && PyDict_Check(parent)) {
        value = PyDict_GetItemWithError(parent, name);
        if (value) return Py_NewRef(value);
        if (PyErr_Occurred()) return NULL;
        PyObject *result = px_name_load(parent, name);
        if (result) return result;
        if (!PyErr_ExceptionMatches(PyExc_NameError)) return NULL;
        PyErr_Clear();
    }

    PyObject *real_globals = PyDict_GetItemString(globals, "__pythonx_globals__");
    if (real_globals && PyDict_Check(real_globals) && real_globals != globals) {
        value = PyDict_GetItemWithError(real_globals, name);
        if (value) return Py_NewRef(value);
        if (PyErr_Occurred()) return NULL;
        globals = real_globals;
    }

    /*
     * Resolve built-ins from the execution globals first.  A Python module
     * normally carries __builtins__ as either the builtins module or its
     * dictionary.
     */
    PyObject *builtins = PyDict_GetItemString(globals, "__builtins__");
    if (builtins && PyModule_Check(builtins)) {
        builtins = PyModule_GetDict(builtins);
    }
    if (builtins && PyDict_Check(builtins)) {
        value = PyDict_GetItemWithError(builtins, name);
        if (value) return Py_NewRef(value);
        if (PyErr_Occurred()) return NULL;
    }

    /* Fall back to the interpreter's active built-in namespace for callers
       that construct a globals dictionary without __builtins__. */
    builtins = PyEval_GetBuiltins();
    if (builtins && PyDict_Check(builtins)) {
        value = PyDict_GetItemWithError(builtins, name);
        if (value) return Py_NewRef(value);
        if (PyErr_Occurred()) return NULL;
    }

    PyErr_Format(PyExc_NameError, "name '%U' is not defined", name);
    return NULL;
}

typedef struct {
    PyObject_HEAD
    const PyXIRNode *node;
    PyObject *globals;
    PyObject *defaults;
    PyObject *kwdefaults;
    PyObject *annotations;
    PyObject *name;
    PyObject *qualname;
    PyObject *module;
    PyObject *doc;
    PyObject *parent;
    PyObject *owner;
    int is_async;
} PyXFunctionObject;

static PyTypeObject PyXFunction_Type;

static PyObject *px_name_load(PyObject *locals, PyObject *name);

static void px_function_dealloc(PyXFunctionObject *fn)
{
    Py_XDECREF(fn->globals);
    Py_XDECREF(fn->defaults);
    Py_XDECREF(fn->kwdefaults);
    Py_XDECREF(fn->annotations);
    Py_XDECREF(fn->name);
    Py_XDECREF(fn->qualname);
    Py_XDECREF(fn->module);
    Py_XDECREF(fn->doc);
    Py_XDECREF(fn->parent);
    Py_XDECREF(fn->owner);
    Py_TYPE(fn)->tp_free((PyObject *)fn);
}

static PyObject *px_function_descr_get(PyObject *self, PyObject *obj, PyObject *type)
{
    Py_UNUSED(type);
    if (!obj || obj == Py_None)
        return Py_NewRef(self);
    return PyMethod_New(self, obj);
}


/*
 * PythonX special-method bridge.
 *
 * Python's implicit special-method lookup is performed on the type rather than
 * through ordinary instance attribute lookup.  PythonX functions are native
 * callables, so CPython's type slot machinery cannot treat them exactly like
 * PyFunctionObject instances.  We therefore install real CPython function
 * wrappers for PythonX dunder methods.  The wrapper is what CPython sees in
 * number/sequence/mapping/async slots; it dispatches to the original PythonX
 * function stored under a private name.
 *
 * This covers the Python data-model surface without changing PythonX's
 * function implementation or replacing the user's method bodies.
 */
static PyObject *px_dunder_wrappers;

static PyObject *px_dunder_dispatch(PyObject *ignored, PyObject *args, PyObject *kwargs)
{
    Py_UNUSED(ignored);
    if (!PyTuple_Check(args) || PyTuple_GET_SIZE(args) != 4) {
        PyErr_SetString(PyExc_TypeError, "PythonX dunder dispatcher received invalid arguments");
        return NULL;
    }

    PyObject *self = PyTuple_GET_ITEM(args, 0);
    PyObject *name = PyTuple_GET_ITEM(args, 1);
    PyObject *forwarded = PyTuple_GET_ITEM(args, 2);
    PyObject *forwarded_kwargs = PyTuple_GET_ITEM(args, 3);
    if (!PyUnicode_Check(name) || !PyTuple_Check(forwarded) ||
        forwarded_kwargs != Py_None && !PyDict_Check(forwarded_kwargs)) {
        PyErr_SetString(PyExc_TypeError, "PythonX dunder dispatcher received malformed arguments");
        return NULL;
    }

    PyObject *owner = NULL;

    /*
     * Instance special methods live on type(self).  Class-level hooks such as
     * __class_getitem__ and __init_subclass__ live on the class object itself.
     */
    if (PyType_Check(self))
        owner = Py_NewRef(self);
    else
        owner = Py_NewRef((PyObject *)Py_TYPE(self));

    PyObject *hidden = PyUnicode_FromFormat("__pythonx_dunder_%U", name);
    if (!hidden) {
        Py_DECREF(owner);
        return NULL;
    }

    PyObject *target = PyObject_GetAttr(owner, hidden);
    Py_DECREF(hidden);
    Py_DECREF(owner);
    if (!target)
        return NULL;

    /*
     * The original PythonX function is a descriptor.  Looking it up on the
     * class above returns the unbound PythonX callable, so prepend self/cls.
     */
    Py_ssize_t argc = PyTuple_GET_SIZE(forwarded);
    PyObject *call_args = PyTuple_New(argc + 1);
    if (!call_args) {
        Py_DECREF(target);
        return NULL;
    }
    PyTuple_SET_ITEM(call_args, 0, Py_NewRef(self));
    for (Py_ssize_t i = 0; i < argc; ++i)
        PyTuple_SET_ITEM(call_args, i + 1, Py_NewRef(PyTuple_GET_ITEM(forwarded, i)));

    PyObject *result = PyObject_Call(target, call_args,
                                     forwarded_kwargs == Py_None ? NULL : forwarded_kwargs);
    Py_DECREF(call_args);
    Py_DECREF(target);
    return result;
}

static PyObject *px_make_dunder_wrapper(const char *name)
{
    if (!px_dunder_wrappers) {
        px_dunder_wrappers = PyDict_New();
        if (!px_dunder_wrappers)
            return NULL;
    }

    PyObject *key = PyUnicode_FromString(name);
    if (!key)
        return NULL;

    PyObject *cached = PyDict_GetItemWithError(px_dunder_wrappers, key);
    if (cached) {
        Py_INCREF(cached);
        Py_DECREF(key);
        return cached;
    }
    if (PyErr_Occurred()) {
        Py_DECREF(key);
        return NULL;
    }

    /*
     * Every generated wrapper is an actual Python function.  CPython therefore
     * installs the appropriate implicit type slots when the class is created.
     */
    PyObject *globals = PyDict_New();
    if (!globals) {
        Py_DECREF(key);
        return NULL;
    }
    PyObject *builtins = PyEval_GetBuiltins();
    if (builtins && PyDict_SetItemString(globals, "__builtins__", builtins) < 0) {
        Py_DECREF(globals);
        Py_DECREF(key);
        return NULL;
    }

    static PyMethodDef dispatch_def = {
        "__pythonx_dispatch__",
        (PyCFunction)px_dunder_dispatch,
        METH_VARARGS | METH_KEYWORDS,
        NULL
    };
    PyObject *dispatch = PyCFunction_New(&dispatch_def, NULL);
    if (!dispatch) {
        Py_DECREF(globals);
        Py_DECREF(key);
        return NULL;
    }
    if (PyDict_SetItemString(globals, "__pythonx_dispatch__", dispatch) < 0) {
        Py_DECREF(dispatch);
        Py_DECREF(globals);
        Py_DECREF(key);
        return NULL;
    }
    Py_DECREF(dispatch);

    PyObject *source = PyUnicode_FromFormat(
        "def %s(self, *args, **kwargs):\n"
        "    return __pythonx_dispatch__(self, \"%s\", args, kwargs)\n",
        name, name);
    if (!source) {
        Py_DECREF(globals);
        Py_DECREF(key);
        return NULL;
    }

    PyObject *filename = PyUnicode_FromString("<pythonx-dunder>");
    PyObject *code = Py_CompileStringObject(
        PyUnicode_AsUTF8(source),
        filename,
        Py_file_input,
        NULL,
        -1);
    Py_DECREF(filename);
    Py_DECREF(source);
    if (!code) {
        Py_DECREF(globals);
        Py_DECREF(key);
        return NULL;
    }

    PyObject *result = PyEval_EvalCode(code, globals, globals);
    Py_DECREF(code);
    Py_DECREF(globals);
    if (!result) {
        Py_DECREF(key);
        return NULL;
    }

    PyObject *wrapper = PyDict_GetItemWithError(result, key);
    if (!wrapper) {
        Py_DECREF(result);
        Py_DECREF(key);
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_RuntimeError, "PythonX failed to create dunder wrapper");
        return NULL;
    }

    Py_INCREF(wrapper);
    if (PyDict_SetItem(px_dunder_wrappers, key, wrapper) < 0) {
        Py_DECREF(wrapper);
        Py_DECREF(result);
        Py_DECREF(key);
        return NULL;
    }
    Py_DECREF(result);
    Py_DECREF(key);
    return wrapper;
}

static int px_bridge_dunder_namespace(PyObject *namespace)
{
    /*
     * This is the broad Python data-model set.  The wrappers are only installed
     * when the class actually defines that dunder, so ordinary Python classes
     * retain their normal behavior.
     */
    static const char *const dunders[] = {
        "__new__", "__init__", "__del__",
        "__repr__", "__str__", "__bytes__", "__format__", "__bool__",
        "__hash__", "__sizeof__", "__dir__", "__getattribute__",
        "__getattr__", "__setattr__", "__delattr__",
        "__lt__", "__le__", "__eq__", "__ne__", "__gt__", "__ge__",
        "__cmp__",
        "__add__", "__sub__", "__mul__", "__matmul__", "__truediv__",
        "__floordiv__", "__mod__", "__divmod__", "__pow__",
        "__lshift__", "__rshift__", "__and__", "__xor__", "__or__",
        "__neg__", "__pos__", "__abs__", "__invert__", "__index__",
        "__round__", "__trunc__", "__floor__", "__ceil__",
        "__complex__", "__int__", "__float__",
        "__iadd__", "__isub__", "__imul__", "__imatmul__", "__itruediv__",
        "__ifloordiv__", "__imod__", "__ipow__", "__ilshift__",
        "__irshift__", "__iand__", "__ixor__", "__ior__",
        "__radd__", "__rsub__", "__rmul__", "__rmatmul__", "__rtruediv__",
        "__rfloordiv__", "__rmod__", "__rdivmod__", "__rpow__",
        "__rlshift__", "__rrshift__", "__rand__", "__rxor__", "__ror__",
        "__len__", "__length_hint__", "__getitem__", "__setitem__",
        "__delitem__", "__missing__", "__iter__", "__next__", "__reversed__",
        "__contains__", "__call__",
        "__enter__", "__exit__", "__aenter__", "__aexit__",
        "__await__", "__aiter__", "__anext__",
        "__get__", "__set__", "__delete__", "__set_name__",
        "__instancecheck__", "__subclasscheck__", "__subclasses__",
        "__init_subclass__", "__class_getitem__", "__mro_entries__",
        "__prepare__", "__instancecheck__", "__subclasscheck__",
        "__reduce__", "__reduce_ex__", "__getnewargs__", "__getnewargs_ex__",
        "__getstate__", "__setstate__", "__copy__", "__deepcopy__",
        "__replace__", "__class__", "__dict__", "__weakref__",
        "__slots__", "__annotations__", "__match_args__",
        "__fspath__", "__enter__", "__exit__",
        "__getformat__", "__setformat__"
    };

    const Py_ssize_t count = (Py_ssize_t)(sizeof(dunders) / sizeof(dunders[0]));
    for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject *name = PyUnicode_FromString(dunders[i]);
        if (!name)
            return -1;

        PyObject *value = PyDict_GetItemWithError(namespace, name);
        if (!value) {
            Py_DECREF(name);
            if (PyErr_Occurred())
                return -1;
            continue;
        }

        /*
         * Only PythonX functions need bridging.  Decorators such as
         * @staticmethod, @classmethod, @property and native descriptors are
         * already understood by CPython and must be left untouched.
         */
        if (!PyObject_TypeCheck(value, &PyXFunction_Type)) {
            Py_DECREF(name);
            continue;
        }

        PyObject *hidden = PyUnicode_FromFormat("__pythonx_dunder_%s", dunders[i]);
        if (!hidden) {
            Py_DECREF(name);
            return -1;
        }

        if (PyDict_SetItem(namespace, hidden, value) < 0) {
            Py_DECREF(hidden);
            Py_DECREF(name);
            return -1;
        }

        PyObject *wrapper = px_make_dunder_wrapper(dunders[i]);
        if (!wrapper) {
            Py_DECREF(hidden);
            Py_DECREF(name);
            return -1;
        }

        /*
         * Keep introspection useful: wrapper.__wrapped__ points at the actual
         * PythonX function that contains the user's implementation.
         */
        if (PyObject_SetAttrString(wrapper, "__wrapped__", value) < 0) {
            PyErr_Clear();
        }

        if (PyDict_SetItem(namespace, name, wrapper) < 0) {
            Py_DECREF(wrapper);
            Py_DECREF(hidden);
            Py_DECREF(name);
            return -1;
        }

        Py_DECREF(wrapper);
        Py_DECREF(hidden);
        Py_DECREF(name);
    }
    return 0;
}

static PyObject *px_class(const PyXIRNode *n, PyObject *g, PXState *s)
{
    PyObject *meta = n->constant;
    PyObject *name = PyTuple_GET_ITEM(meta, 0);
    Py_ssize_t nb = PyLong_AsSsize_t(PyTuple_GET_ITEM(meta, 1));
    PyObject *kw_names = PyTuple_GET_ITEM(meta, 2);
    Py_ssize_t nd = PyLong_AsSsize_t(PyTuple_GET_ITEM(meta, 3));
    if (nb < 0 || nd < 0) {
        PyErr_SetString(PyExc_SystemError, "invalid PythonX class metadata");
        return NULL;
    }

    /*
     * PythonX classes have a native PythonX object root when no explicit
     * bases are supplied.  This makes instances PythonX objects rather than
     * merely ordinary object subclasses, while explicit Python bases retain
     * normal Python multiple-inheritance semantics.
     */
    if (PyType_Ready(&PyXObject_Type) < 0)
        return NULL;

    Py_ssize_t effective_bases = nb == 0 ? 1 : nb;
    PyObject *bases = PyTuple_New(effective_bases);
    PyObject *namespace = PyDict_New();
    if (!bases || !namespace) {
        Py_XDECREF(bases); Py_XDECREF(namespace); return NULL;
    }

    if (nb == 0)
        PyTuple_SET_ITEM(bases, 0, Py_NewRef((PyObject *)&PyXObject_Type));

    for (Py_ssize_t i = 0; i < nb; ++i) {
        PyObject *base = px_eval(n->children[i], g, s);
        if (!base) { Py_DECREF(bases); Py_DECREF(namespace); return NULL; }
        PyTuple_SET_ITEM(bases, i, base);
    }

    /*
     * Execute the class suite in its own namespace.  FunctionDef stores
     * methods in this namespace, so a method named __init__ becomes a real
     * descriptor on the resulting Python type.  Python's type machinery then
     * automatically calls __init__(self, ...) when an instance is created.
     */
    if (PyDict_SetItemString(namespace, "__module__",
                             PyDict_GetItemString(g, "__name__") ?
                             PyDict_GetItemString(g, "__name__") : Py_None) < 0) {
        Py_DECREF(bases); Py_DECREF(namespace); return NULL;
    }
    /*
     * Hidden execution metadata is deliberately kept out of the normal
     * Python-facing class API.  __pythonx_globals__ provides module-global
     * lookup for names used while executing the class body; the class marker
     * prevents methods from incorrectly capturing class attributes.
     */
    if (PyDict_SetItemString(namespace, "__pythonx_globals__", g) < 0 ||
        PyDict_SetItemString(namespace, "__pythonx_class_namespace__", Py_True) < 0) {
        Py_DECREF(bases); Py_DECREF(namespace); return NULL;
    }
    PyObject *body_result = px_eval(n->left, namespace, s);
    if (!body_result) { Py_DECREF(bases); Py_DECREF(namespace); return NULL; }
    Py_DECREF(body_result);

    /*
     * Bridge PythonX-defined dunder methods to real CPython function objects
     * before type() inspects the namespace.  This is what makes implicit
     * operators, len(), iteration, item access, context management, hashing,
     * async protocols, and the other data-model operations reach PythonX code.
     */
    if (px_bridge_dunder_namespace(namespace) < 0) {
        Py_DECREF(bases);
        Py_DECREF(namespace);
        return NULL;
    }

    PyObject *kwargs = PyDict_New();
    if (!kwargs) { Py_DECREF(bases); Py_DECREF(namespace); return NULL; }
    Py_ssize_t nk = PyTuple_GET_SIZE(kw_names);
    for (Py_ssize_t i = 0; i < nk; ++i) {
        PyObject *key = PyTuple_GET_ITEM(kw_names, i);
        PyObject *value = px_eval(n->children[nb + i], g, s);
        if (!value) { Py_DECREF(kwargs); Py_DECREF(bases); Py_DECREF(namespace); return NULL; }
        if (key == Py_None) {
            /* **class_kwargs */
            if (PyDict_Update(kwargs, value) < 0) {
                Py_DECREF(value); Py_DECREF(kwargs); Py_DECREF(bases); Py_DECREF(namespace); return NULL;
            }
        } else if (PyDict_SetItem(kwargs, key, value) < 0) {
            Py_DECREF(value); Py_DECREF(kwargs); Py_DECREF(bases); Py_DECREF(namespace); return NULL;
        }
        Py_DECREF(value);
    }

    PyObject *type_obj = (PyObject *)&PyType_Type;
    PyObject *class_args = PyTuple_Pack(3, name, bases, namespace);
    if (!class_args) {
        Py_DECREF(kwargs); Py_DECREF(bases); Py_DECREF(namespace); return NULL;
    }
    /*
     * Always pass class-definition keywords through the type constructor.
     * This is required for metaclasses and for __init_subclass__(**kwargs).
     */
    PyObject *class_obj = PyObject_Call(type_obj, class_args, kwargs);
    Py_DECREF(class_args);
    Py_DECREF(kwargs);
    Py_DECREF(bases);
    Py_DECREF(namespace);
    if (!class_obj) return NULL;

    /*
     * Record the class that defined each PythonX method.  We do this after
     * type() creates the class because the class object did not exist while
     * the class suite was executing.
     */
    PyObject *dict_keys = PyDict_Keys(namespace);
    if (!dict_keys) { Py_DECREF(class_obj); return NULL; }
    Py_ssize_t method_count = PyList_GET_SIZE(dict_keys);
    for (Py_ssize_t i = 0; i < method_count; ++i) {
        PyObject *key = PyList_GET_ITEM(dict_keys, i);
        PyObject *value = PyDict_GetItemWithError(namespace, key);
        if (!value) continue;
        if (PyObject_TypeCheck(value, &PyXFunction_Type)) {
            PyXFunctionObject *fn = (PyXFunctionObject *)value;
            Py_XSETREF(fn->owner, Py_NewRef(class_obj));
        }
        /* Dunder bridge keeps the original PythonX function under this name. */
        if (PyUnicode_Check(key)) {
            PyObject *hidden = PyUnicode_FromFormat("__pythonx_dunder_%U", key);
            if (!hidden) { Py_DECREF(dict_keys); Py_DECREF(class_obj); return NULL; }
            PyObject *original = PyDict_GetItemWithError(namespace, hidden);
            if (original && PyObject_TypeCheck(original, &PyXFunction_Type)) {
                PyXFunctionObject *fn = (PyXFunctionObject *)original;
                Py_XSETREF(fn->owner, Py_NewRef(class_obj));
            }
            Py_DECREF(hidden);
        }
    }
    Py_DECREF(dict_keys);

    /*
     * Class decorators are applied from the innermost decorator to the
     * outermost decorator, matching Python:
     *
     *     @outer
     *     @inner
     *     class C: ...
     *
     * becomes outer(inner(C)).
     */
    for (Py_ssize_t i = nd - 1; i >= 0; --i) {
        PyObject *decorator = px_eval(n->children[nb + nk + i], g, s);
        if (!decorator) { Py_DECREF(class_obj); return NULL; }
        PyObject *next = PyObject_CallOneArg(decorator, class_obj);
        Py_DECREF(decorator);
        Py_DECREF(class_obj);
        if (!next) return NULL;
        class_obj = next;
    }
    return class_obj;
}

static PyObject *px_function_call(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyXFunctionObject *fn = (PyXFunctionObject *)self;
    const PyXIRNode *n = fn->node;
    PyObject *meta = n->constant;
    Py_ssize_t posonly = PyLong_AsSsize_t(PyTuple_GET_ITEM(meta, 0));
    Py_ssize_t positional = PyLong_AsSsize_t(PyTuple_GET_ITEM(meta, 1));
    Py_ssize_t kwonly = PyLong_AsSsize_t(PyTuple_GET_ITEM(meta, 2));
    PyObject *vararg_name = PyTuple_GET_ITEM(meta, 3);
    PyObject *kwarg_name = PyTuple_GET_ITEM(meta, 4);
    PyObject *names = PyTuple_GET_ITEM(meta, 5);
    PyObject *callable_name = PyTuple_GET_SIZE(meta) > 6
        ? PyTuple_GET_ITEM(meta, 6) : PyUnicode_FromString("<lambda>");
    int owns_callable_name = PyTuple_GET_SIZE(meta) <= 6;
    const char *kind_name = PyUnicode_Check(callable_name)
        ? PyUnicode_AsUTF8(callable_name) : "<function>";
    if (!kind_name) {
        if (owns_callable_name) Py_DECREF(callable_name);
        return NULL;
    }

    Py_ssize_t total_pos = posonly + positional;
    Py_ssize_t arg_count = PyTuple_GET_SIZE(args);
    if (posonly < 0 || positional < 0 || kwonly < 0 ||
        PyTuple_GET_SIZE(names) != total_pos + kwonly) {
        PyErr_SetString(PyExc_SystemError, "invalid PythonX function signature");
        goto error_no_locals;
    }

    if (arg_count > total_pos && vararg_name == Py_None) {
        PyErr_Format(PyExc_TypeError,
                     "%s() takes %zd positional arguments but %zd were given",
                     kind_name, total_pos, arg_count);
        goto error_no_locals;
    }

    /*
     * A PythonX function gets a real local namespace.  Globals are resolved
     * lazily through the hidden globals/parent links rather than by copying
     * the entire globals dictionary into locals.  This is what makes
     * assignments inside a function local by default.
     */
    PyObject *locals = PyDict_New();
    if (!locals) goto error_no_locals;
    if (PyDict_SetItemString(locals, "__pythonx_globals__", fn->globals) < 0) goto error;
    /*
     * Keep the bound receiver and defining class available to the PythonX
     * evaluator.  This is used for class-aware operations such as super().
     */
    if (PyDict_SetItemString(locals, "__pythonx_self__", Py_None) < 0)
        goto error;
    if (fn->owner &&
        PyDict_SetItemString(locals, "__pythonx_class__", fn->owner) < 0)
        goto error;
    
    /* Nested PythonX functions can read names from their defining scope. */
    if (fn->parent) {
        if (PyDict_SetItemString(locals, "__pythonx_parent__", fn->parent) < 0)
            goto error;
    }

    /* Positional-only and normal positional parameters. */
    if (total_pos > 0 && arg_count > 0 &&
        PyDict_SetItemString(locals, "__pythonx_self__", PyTuple_GET_ITEM(args, 0)) < 0)
        goto error;
    for (Py_ssize_t i = 0; i < total_pos; ++i) {
        PyObject *name = PyTuple_GET_ITEM(names, i);
        PyObject *value = NULL;

        if (i < arg_count) {
            value = PyTuple_GET_ITEM(args, i);
            if (kwargs && PyDict_GetItemWithError(kwargs, name)) {
                if (i < posonly) {
                    PyErr_Format(PyExc_TypeError,
                                 "%s() got some positional-only arguments passed as keyword arguments: '%U'",
                                 kind_name, name);
                } else {
                    PyErr_Format(PyExc_TypeError,
                                 "%s() got multiple values for argument '%U'",
                                 kind_name, name);
                }
                goto error;
            }
        } else if (i >= posonly && kwargs) {
            value = PyDict_GetItemWithError(kwargs, name);
            if (!value && PyErr_Occurred()) goto error;
        } else if (i < posonly && kwargs && PyDict_GetItemWithError(kwargs, name)) {
            PyErr_Format(PyExc_TypeError,
                         "%s() got some positional-only arguments passed as keyword arguments: '%U'",
                         kind_name, name);
            goto error;
        }

        if (!value) {
            Py_ssize_t default_count = PyTuple_GET_SIZE(fn->defaults);
            Py_ssize_t default_index = i - (total_pos - default_count);
            if (default_index >= 0) value = PyTuple_GET_ITEM(fn->defaults, default_index);
        }

        if (!value) {
            PyErr_Format(PyExc_TypeError,
                         "%s() missing required argument: '%U'", kind_name, name);
            goto error;
        }
        if (PyDict_SetItem(locals, name, value) < 0) goto error;
    }

    if (vararg_name != Py_None) {
        PyObject *extra = PyTuple_GetSlice(args, total_pos, arg_count);
        if (!extra) goto error;
        if (PyDict_SetItem(locals, vararg_name, extra) < 0) {
            Py_DECREF(extra);
            goto error;
        }
        Py_DECREF(extra);
    }

    /* Keyword-only parameters, including defaults. */
    for (Py_ssize_t i = total_pos; i < total_pos + kwonly; ++i) {
        PyObject *name = PyTuple_GET_ITEM(names, i);
        PyObject *value = kwargs ? PyDict_GetItemWithError(kwargs, name) : NULL;
        if (!value && kwargs && PyErr_Occurred()) goto error;

        if (!value && fn->kwdefaults)
            value = PyDict_GetItemWithError(fn->kwdefaults, name);

        if (!value) {
            PyErr_Format(PyExc_TypeError,
                         "%s() missing required keyword-only argument: '%U'",
                         kind_name, name);
            goto error;
        }
        if (PyDict_SetItem(locals, name, value) < 0) goto error;
    }

    /* Collect unknown keywords into **kwargs, or reject them if absent. */
    PyObject *extra_kwargs = PyDict_New();
    if (!extra_kwargs) goto error;

    if (kwargs) {
        PyObject *key, *value;
        Py_ssize_t p = 0;
        while (PyDict_Next(kwargs, &p, &key, &value)) {
            int formal = PySequence_Contains(names, key);
            if (formal < 0) {
                Py_DECREF(extra_kwargs);
                goto error;
            }
            if (!formal) {
                if (PyDict_SetItem(extra_kwargs, key, value) < 0) {
                    Py_DECREF(extra_kwargs);
                    goto error;
                }
            }
        }
    }

    if (kwarg_name != Py_None) {
        if (PyDict_SetItem(locals, kwarg_name, extra_kwargs) < 0) {
            Py_DECREF(extra_kwargs);
            goto error;
        }
    } else if (PyDict_GET_SIZE(extra_kwargs) != 0) {
        PyObject *key = NULL, *value = NULL;
        Py_ssize_t pos = 0;
        (void)PyDict_Next(extra_kwargs, &pos, &key, &value);
        PyErr_Format(PyExc_TypeError,
                     "%s() got an unexpected keyword argument '%U'",
                     kind_name, key);
        Py_DECREF(extra_kwargs);
        goto error;
    }
    Py_DECREF(extra_kwargs);

    PXState state = {PX_NORMAL, 0};
    PyObject *result = px_eval(n->left, locals, &state);
    Py_DECREF(locals);
    if (!result) {
        if (owns_callable_name) Py_DECREF(callable_name);
        return NULL;
    }
    if (state.flow == PX_RETURN) {
        state.flow = PX_NORMAL;
        if (owns_callable_name) Py_DECREF(callable_name);
        return result;
    }
    if (state.flow == PX_BREAK || state.flow == PX_CONTINUE) {
        Py_DECREF(result);
        PyErr_Format(PyExc_SyntaxError, "'%s' outside loop", state.flow == PX_BREAK ? "break" : "continue");
        if (owns_callable_name) Py_DECREF(callable_name);
        return NULL;
    }
    Py_DECREF(result);
    if (owns_callable_name) Py_DECREF(callable_name);
    return Py_NewRef(Py_None);

error:
    Py_DECREF(locals);
error_no_locals:
    if (owns_callable_name) Py_DECREF(callable_name);
    return NULL;
}

static PyObject *px_function_get_parent(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyXFunctionObject *fn = (PyXFunctionObject *)self;
    return PyDict_New(); /* Parent is attached by px_function when nested. */
}

static PyMemberDef px_function_members[] = {
    {"__name__", T_OBJECT_EX, offsetof(PyXFunctionObject, name), READONLY, NULL},
    {"__qualname__", T_OBJECT_EX, offsetof(PyXFunctionObject, qualname), READONLY, NULL},
    {"__module__", T_OBJECT_EX, offsetof(PyXFunctionObject, module), READONLY, NULL},
    {"__doc__", T_OBJECT_EX, offsetof(PyXFunctionObject, doc), 0, NULL},
    {"__defaults__", T_OBJECT_EX, offsetof(PyXFunctionObject, defaults), 0, NULL},
    {"__kwdefaults__", T_OBJECT_EX, offsetof(PyXFunctionObject, kwdefaults), 0, NULL},
    {"__annotations__", T_OBJECT_EX, offsetof(PyXFunctionObject, annotations), 0, NULL},
    {"__pythonx_parent__", T_OBJECT_EX, offsetof(PyXFunctionObject, parent), READONLY, NULL},
    {NULL}
};

static PyTypeObject PyXFunction_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "pythonx.function",
    .tp_basicsize = sizeof(PyXFunctionObject),
    .tp_dealloc = (destructor)px_function_dealloc,
    .tp_call = px_function_call,
    .tp_descr_get = px_function_descr_get,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_members = px_function_members,
    .tp_doc = "PythonX native function",
    .tp_new = PyType_GenericNew,
};

static PyObject *px_function(const PyXIRNode *n, PyObject *g, PXState *s)
{
    if (PyType_Ready(&PyXFunction_Type) < 0) return NULL;

    PyObject *meta = n->constant;
    Py_ssize_t positional = PyLong_AsSsize_t(PyTuple_GET_ITEM(meta, 1));
    Py_ssize_t total_pos = PyLong_AsSsize_t(PyTuple_GET_ITEM(meta, 0)) + positional;
    Py_ssize_t kwonly = PyLong_AsSsize_t(PyTuple_GET_ITEM(meta, 2));
    Py_ssize_t meta_size = PyTuple_GET_SIZE(meta);
    Py_ssize_t annotation_count = meta_size > 11 ? PyLong_AsSsize_t(PyTuple_GET_ITEM(meta, 11)) : 0;
    Py_ssize_t decorator_count = meta_size > 12 ? PyLong_AsSsize_t(PyTuple_GET_ITEM(meta, 12)) : 0;
    Py_ssize_t nd_total = n->child_count;
    Py_ssize_t nd = nd_total - kwonly - annotation_count - decorator_count;
    if (nd < 0) {
        PyErr_SetString(PyExc_SystemError, "invalid PythonX function metadata");
        return NULL;
    }

    PyObject *defaults = PyTuple_New(nd);
    PyObject *kwdefaults = PyDict_New();
    PyObject *annotations = PyDict_New();
    if (!defaults || !kwdefaults || !annotations) {
        Py_XDECREF(defaults); Py_XDECREF(kwdefaults); Py_XDECREF(annotations);
        return NULL;
    }

    Py_ssize_t nd_pos = nd;
    for (Py_ssize_t i = 0; i < nd_pos; ++i) {
        PyObject *v = px_eval(n->children[i], g, s);
        if (!v) goto fail;
        PyTuple_SET_ITEM(defaults, i, v);
    }

    for (Py_ssize_t i = 0; i < kwonly; ++i) {
        PyXIRNode *d = n->children[nd + i];
        if (!d) continue;
        PyObject *v = px_eval(d, g, s);
        if (!v) goto fail;
        PyObject *name = PyTuple_GET_ITEM(PyTuple_GET_ITEM(meta, 5), total_pos + i);
        if (PyDict_SetItem(kwdefaults, name, v) < 0) { Py_DECREF(v); goto fail; }
        Py_DECREF(v);
    }

    if (annotation_count) {
        PyObject *names = PyTuple_GET_ITEM(meta, 5);
        for (Py_ssize_t i = 0; i < annotation_count; ++i) {
            PyXIRNode *a = n->children[nd + kwonly + i];
            if (!a) continue;
            PyObject *v = px_eval(a, g, s);
            if (!v) goto fail;
            PyObject *key;
            if (i < PyTuple_GET_SIZE(names))
                key = PyTuple_GET_ITEM(names, i);
            else
                key = PyUnicode_FromString("return");
            if (PyDict_SetItem(annotations, key, v) < 0) {
                Py_DECREF(v);
                if (i >= PyTuple_GET_SIZE(names)) Py_DECREF(key);
                goto fail;
            }
            Py_DECREF(v);
            if (i >= PyTuple_GET_SIZE(names)) Py_DECREF(key);
        }
    }

    PyObject *name = meta_size > 6 ? PyTuple_GET_ITEM(meta, 6) : PyUnicode_FromString("<lambda>");
    PyObject *qualname = meta_size > 7 ? PyTuple_GET_ITEM(meta, 7) : name;
    PyObject *module = meta_size > 8 ? PyTuple_GET_ITEM(meta, 8) : Py_None;
    PyObject *doc = meta_size > 9 ? PyTuple_GET_ITEM(meta, 9) : Py_None;
    int is_async = meta_size > 10 && PyObject_IsTrue(PyTuple_GET_ITEM(meta, 10));
    if (PyErr_Occurred()) goto fail;

    /*
     * A class suite needs its own namespace for class attributes and methods,
     * but methods do not close over the class namespace.  Marking the class
     * namespace lets us evaluate defaults/decorators there while making the
     * resulting method resolve globals from the defining module.
     */
    int class_namespace = g && PyDict_Check(g) &&
        PyDict_GetItemString(g, "__pythonx_class_namespace__") != NULL;
    PyObject *function_globals = g;
    if (class_namespace) {
        PyObject *real_globals = PyDict_GetItemString(g, "__pythonx_globals__");
        if (real_globals && PyDict_Check(real_globals))
            function_globals = real_globals;
    }

    PyXFunctionObject *fn = (PyXFunctionObject *)PyObject_New(PyXFunctionObject, &PyXFunction_Type);
    if (!fn) goto fail;
    fn->node = n;
    fn->globals = Py_NewRef(function_globals);
    fn->defaults = defaults; defaults = NULL;
    fn->kwdefaults = kwdefaults; kwdefaults = NULL;
    fn->annotations = annotations; annotations = NULL;
    fn->name = Py_NewRef(name);
    fn->qualname = Py_NewRef(qualname);
    if (module == Py_None) {
        PyObject *m = PyDict_GetItemString(g, "__name__");
        fn->module = m ? Py_NewRef(m) : Py_NewRef(Py_None);
    } else fn->module = Py_NewRef(module);
    fn->doc = Py_NewRef(doc);
    fn->parent = (class_namespace || !function_globals) ? NULL :
        ((g && PyDict_Check(g) && PyDict_GetItemString(g, "__pythonx_globals__"))
            ? Py_NewRef(g) : NULL);
    fn->owner = NULL;
    fn->is_async = is_async;

    /*
     * Apply decorators from bottom to top, exactly as Python does:
     * @outer
     * @inner
     * def f:  => outer(inner(f))
     */
    if (decorator_count) {
        PyObject *decorated = (PyObject *)fn;
        Py_INCREF(decorated);
        Py_ssize_t base = nd + kwonly + annotation_count;
        for (Py_ssize_t i = decorator_count - 1; i >= 0; --i) {
            PyObject *decorator = px_eval(n->children[base + i], g, s);
            if (!decorator) { Py_DECREF(decorated); return NULL; }
            PyObject *next = PyObject_CallOneArg(decorator, decorated);
            Py_DECREF(decorator);
            Py_DECREF(decorated);
            if (!next) return NULL;
            decorated = next;
        }
        Py_DECREF(fn);
        return decorated;
    }

    return (PyObject *)fn;

fail:
    Py_XDECREF(defaults);
    Py_XDECREF(kwdefaults);
    Py_XDECREF(annotations);
    return NULL;
}

static PyObject *px_lambda(const PyXIRNode *n, PyObject *g, PXState *s)
{
    return px_function(n, g, s);
}


static PyObject *px_suite(const PyXIRNode *n, PyObject *g, PXState *s)
{
    PyObject *result = Py_NewRef(Py_None);
    for (Py_ssize_t i = 0; i < n->child_count; ++i) {
        PyObject *value = px_eval(n->children[i], g, s);
        if (!value) { Py_DECREF(result); return NULL; }
        Py_SETREF(result, value);
        if (s->flow != PX_NORMAL) break;
    }
    return result;
}

static PyObject *px_call(const PyXIRNode *n, PyObject *g, PXState *s)
{
    PyObject *callable = px_eval(n->children[0], g, s);
    if (!callable) return NULL;

    /*
     * Python's zero-argument super() normally gets __class__ and self from
     * the executing frame. PythonX uses an explicit evaluator namespace, so
     * provide the same information here when super() has no arguments.
     */
    if (callable == (PyObject *)&PySuper_Type && n->child_count == 1) {
        PyObject *self_obj = PyDict_GetItemString(g, "__pythonx_self__");
        PyObject *class_obj = PyDict_GetItemString(g, "__pythonx_class__");
        if (!self_obj || self_obj == Py_None || !class_obj) {
            Py_DECREF(callable);
            PyErr_SetString(PyExc_RuntimeError,
                            "super(): no current PythonX class context");
            return NULL;
        }
        PyObject *result = PyObject_CallFunctionObjArgs(
            callable, class_obj, self_obj, NULL);
        Py_DECREF(callable);
        return result;
    }
    PyObject *args = PyList_New(0), *kwargs = PyDict_New();
    if (!args || !kwargs) { Py_XDECREF(args); Py_XDECREF(kwargs); Py_DECREF(callable); return NULL; }
    for (Py_ssize_t i = 0; i < n->child_count - 1; ++i) {
        PyObject *descriptor = PyTuple_GET_ITEM(n->constant, i);
        int kind = (int)PyLong_AsLong(PyTuple_GET_ITEM(descriptor, 0));
        PyObject *value = px_eval(n->children[i + 1], g, s);
        if (!value) goto error;
        if (kind == 0) {
            if (PyList_Append(args, value) < 0) { Py_DECREF(value); goto error; }
        } else if (kind == 1) {
            PyObject *seq = PySequence_Fast(value, "* argument must be an iterable");
            if (!seq) { Py_DECREF(value); goto error; }
            for (Py_ssize_t j = 0; j < PySequence_Fast_GET_SIZE(seq); ++j) {
                if (PyList_Append(args, PySequence_Fast_GET_ITEM(seq, j)) < 0) {
                    Py_DECREF(seq); Py_DECREF(value); goto error;
                }
            }
            Py_DECREF(seq);
        } else if (kind == 2) {
            if (PyDict_SetItem(kwargs, PyTuple_GET_ITEM(descriptor, 1), value) < 0) { Py_DECREF(value); goto error; }
        } else if (kind == 3) {
            if (PyDict_Update(kwargs, value) < 0) { Py_DECREF(value); goto error; }
        }
        Py_DECREF(value);
    }
    {
        PyObject *tuple = PyList_AsTuple(args);
        PyObject *result;
        if (!tuple) goto error;
        result = PyObject_Call(callable, tuple, kwargs);
        Py_DECREF(tuple); Py_DECREF(args); Py_DECREF(kwargs); Py_DECREF(callable);
        return result;
    }
error:
    Py_DECREF(args); Py_DECREF(kwargs); Py_DECREF(callable); return NULL;
}

static PyObject *px_await(PyObject *awaitable)
{
    PyObject *iterator = PyObject_GetIter(awaitable);
    if (!iterator) {
        PyErr_Clear();
        iterator = PyObject_CallMethod(awaitable, "__await__", NULL);
    }
    if (!iterator) return NULL;

    PyObject *send_value = Py_NewRef(Py_None);
    for (;;) {
        PyObject *value = PyObject_CallMethod(iterator, "send", "O", send_value);
        Py_DECREF(send_value);
        send_value = NULL;

        if (!value) {
            if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                Py_DECREF(iterator);
                return NULL;
            }
            PyObject *stop = NULL, *result = NULL, *tb = NULL;
            PyErr_Fetch(&stop, &result, &tb);
            Py_XDECREF(tb);
            PyErr_NormalizeException(&stop, &result, NULL);
            Py_XDECREF(stop);
            if (result) {
                PyObject *resolved = PyObject_GetAttrString(result, "value");
                Py_DECREF(result);
                if (resolved) result = resolved;
                else { PyErr_Clear(); result = Py_NewRef(Py_None); }
            } else {
                result = Py_NewRef(Py_None);
            }
            Py_DECREF(iterator);
            return result;
        }

        if (value == Py_None) {
            send_value = Py_NewRef(Py_None);
            Py_DECREF(value);
            continue;
        }

        PyObject *nested = PyObject_CallMethod(value, "__await__", NULL);
        if (!nested) {
            PyErr_SetString(PyExc_RuntimeError,
                            "PythonX: native await received a non-awaitable suspension value");
            Py_DECREF(value);
            Py_DECREF(iterator);
            return NULL;
        }
        Py_DECREF(nested);

        PyObject *resolved = px_await(value);
        Py_DECREF(value);
        if (!resolved) {
            Py_DECREF(iterator);
            return NULL;
        }
        send_value = resolved;
    }
}

static int px_assign_unpack(const PyXIRNode *n, PyObject *g, PyObject *value, PXState *s)
{
    PyObject *seq = PySequence_Fast(value, "cannot unpack non-iterable object");
    if (!seq) return -1;
    Py_ssize_t count = n->child_count;
    Py_ssize_t star = -1;
    for (Py_ssize_t i = 0; i < count; ++i) {
        if (n->children[i]->op == PYX_IR_STAR_UNPACK) {
            if (star >= 0) {
                Py_DECREF(seq);
                PyErr_SetString(PyExc_SyntaxError, "multiple starred expressions in assignment");
                return -1;
            }
            star = i;
        }
    }
    Py_ssize_t size = PySequence_Fast_GET_SIZE(seq);
    if (star < 0) {
        if (size != count) {
            PyErr_Format(PyExc_ValueError, "not enough values to unpack (expected %zd, got %zd)", count, size);
            if (size > count) PyErr_Format(PyExc_ValueError, "too many values to unpack (expected %zd)", count);
            Py_DECREF(seq);
            return -1;
        }
        for (Py_ssize_t i = 0; i < count; ++i) {
            if (px_assign_target(n->children[i], g, PySequence_Fast_GET_ITEM(seq, i), s) < 0) {
                Py_DECREF(seq); return -1;
            }
        }
    } else {
        Py_ssize_t fixed = count - 1;
        if (size < fixed) {
            PyErr_Format(PyExc_ValueError, "not enough values to unpack (expected at least %zd, got %zd)", fixed, size);
            Py_DECREF(seq);
            return -1;
        }
        for (Py_ssize_t i = 0; i < star; ++i) {
            if (px_assign_target(n->children[i], g, PySequence_Fast_GET_ITEM(seq, i), s) < 0) {
                Py_DECREF(seq); return -1;
            }
        }
        PyObject *middle = PyList_New(size - fixed);
        if (!middle) { Py_DECREF(seq); return -1; }
        for (Py_ssize_t i = star; i < size - (count - star - 1); ++i) {
            Py_INCREF(PySequence_Fast_GET_ITEM(seq, i));
            PyList_SET_ITEM(middle, i - star, PySequence_Fast_GET_ITEM(seq, i));
        }
        if (px_assign_target(n->children[star]->left, g, middle, s) < 0) {
            Py_DECREF(middle); Py_DECREF(seq); return -1;
        }
        Py_DECREF(middle);
        for (Py_ssize_t i = star + 1; i < count; ++i) {
            Py_ssize_t source = size - (count - i);
            if (px_assign_target(n->children[i], g, PySequence_Fast_GET_ITEM(seq, source), s) < 0) {
                Py_DECREF(seq); return -1;
            }
        }
    }
    Py_DECREF(seq);
    return 0;
}

static int px_assign_target(const PyXIRNode *target, PyObject *g, PyObject *value, PXState *s)
{
    if (target->op == PYX_IR_UNPACK) return px_assign_unpack(target, g, value, s);
    if (target->op == PYX_IR_STAR_UNPACK) return px_assign_target(target->left, g, value, s);
    if (target->op == PYX_IR_NAME_STORE) return PyDict_SetItem(g, target->constant, value);
    if (target->op == PYX_IR_SETATTR) {
        PyObject *object = px_eval(target->left, g, s);
        if (!object) return -1;
        int rc = PyObject_SetAttr(object, target->constant, value);
        Py_DECREF(object); return rc;
    }
    if (target->op == PYX_IR_SUBSCRIPT_STORE) {
        PyObject *object = px_eval(target->children[0], g, s);
        PyObject *key = px_eval(target->children[1], g, s);
        if (!object || !key) { Py_XDECREF(object); Py_XDECREF(key); return -1; }
        int rc = PyObject_SetItem(object, key, value);
        Py_DECREF(object); Py_DECREF(key); return rc;
    }
    if (target->op == PYX_IR_SUBSCRIPT) {
        PyObject *object = px_eval(target->children[0], g, s);
        PyObject *key = px_eval(target->children[1], g, s);
        if (!object || !key) { Py_XDECREF(object); Py_XDECREF(key); return -1; }
        int rc = PyObject_SetItem(object, key, value);
        Py_DECREF(object); Py_DECREF(key); return rc;
    }
    PyErr_SetString(PyExc_SystemError, "invalid PythonX assignment target");
    return -1;
}

static PyObject *px_for(const PyXIRNode *n, PyObject *g, PXState *s, int async_for)
{
    s->loop_depth++;
    PyObject *source = px_eval(n->children[0], g, s);
    if (!source) { s->loop_depth--; return NULL; }
    PyObject *iterator = async_for ? PyObject_CallMethod(source, "__aiter__", NULL) : PyObject_GetIter(source);
    Py_DECREF(source);
    if (!iterator) { s->loop_depth--; return NULL; }

    PyObject *result = Py_NewRef(Py_None);
    int broke = 0;
    for (;;) {
        PyObject *item = NULL;
        if (async_for) {
            PyObject *awaitable = PyObject_CallMethod(iterator, "__anext__", NULL);
            if (!awaitable) {
                if (PyErr_ExceptionMatches(PyExc_StopAsyncIteration)) { PyErr_Clear(); break; }
                Py_DECREF(iterator); Py_DECREF(result); s->loop_depth--; return NULL;
            }
            item = px_await(awaitable);
            Py_DECREF(awaitable);
            if (!item) { Py_DECREF(iterator); Py_DECREF(result); s->loop_depth--; return NULL; }
        } else {
            item = PyIter_Next(iterator);
            if (!item) {
                if (PyErr_Occurred()) { Py_DECREF(iterator); Py_DECREF(result); s->loop_depth--; return NULL; }
                break;
            }
        }

        if (px_assign_target(n->children[1], g, item, s) < 0) {
            Py_DECREF(item); Py_DECREF(iterator); Py_DECREF(result); s->loop_depth--; return NULL;
        }
        Py_DECREF(item);

        PyObject *body = px_eval(n->children[2], g, s);
        if (!body) { Py_DECREF(iterator); Py_DECREF(result); s->loop_depth--; return NULL; }
        Py_DECREF(body);
        if (s->flow == PX_BREAK) { s->flow = PX_NORMAL; broke = 1; break; }
        if (s->flow == PX_CONTINUE) { s->flow = PX_NORMAL; continue; }
        if (s->flow != PX_NORMAL) { Py_DECREF(iterator); Py_DECREF(result); s->loop_depth--; return NULL; }
    }
    Py_DECREF(iterator);
    s->loop_depth--;

    if (!broke) {
        PyObject *else_result = px_eval(n->children[3], g, s);
        if (!else_result) { Py_DECREF(result); return NULL; }
        Py_SETREF(result, else_result);
    }
    return result;
}

static PyObject *px_with(const PyXIRNode *n, PyObject *g, PXState *s, int async_with)
{
    Py_ssize_t count = n->child_count - 1;
    PyObject **managers = PyMem_Calloc((size_t)count, sizeof(*managers));
    if (!managers) { PyErr_NoMemory(); return NULL; }
    Py_ssize_t entered = 0;
    PyObject *result = NULL;

    for (Py_ssize_t i = 0; i < count; ++i) {
        PyXIRNode *item = n->children[i];
        PyObject *manager = px_eval(item->children[0], g, s);
        if (!manager) goto fail;
        PyObject *value = async_with
            ? PyObject_CallMethod(manager, "__aenter__", NULL)
            : PyObject_CallMethod(manager, "__enter__", NULL);
        if (!value) { Py_DECREF(manager); goto fail; }
        if (async_with) {
            PyObject *resolved = px_await(value);
            Py_DECREF(value);
            value = resolved;
            if (!value) { Py_DECREF(manager); goto fail; }
        }
        managers[entered++] = manager;
        if (item->children[1]->op != PYX_IR_CONST ||
            item->children[1]->constant != Py_None) {
            if (px_assign_target(item->children[1], g, value, s) < 0) {
                Py_DECREF(value); goto fail;
            }
        }
        Py_DECREF(value);
    }

    result = px_eval(n->children[count], g, s);

    if (result) {
        for (Py_ssize_t i = entered; i > 0; --i) {
            PyObject *exit_result = async_with
                ? PyObject_CallMethod(managers[i - 1], "__aexit__", Py_None, Py_None, Py_None)
                : PyObject_CallMethod(managers[i - 1], "__exit__", Py_None, Py_None, Py_None);
            if (async_with && exit_result) {
                PyObject *resolved = px_await(exit_result);
                Py_DECREF(exit_result); exit_result = resolved;
            }
            if (!exit_result) {
                Py_DECREF(result); result = NULL;
                for (Py_ssize_t j = i - 1; j > 0; --j) {
                    PyObject *r = async_with
                        ? PyObject_CallMethod(managers[j - 1], "__aexit__", Py_None, Py_None, Py_None)
                        : PyObject_CallMethod(managers[j - 1], "__exit__", Py_None, Py_None, Py_None);
                    Py_XDECREF(r);
                }
                break;
            }
            Py_DECREF(exit_result);
            Py_DECREF(managers[i - 1]);
        }
    } else {
        PyObject *exc_type = NULL, *exc_value = NULL, *exc_tb = NULL;
        PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
        PyErr_NormalizeException(&exc_type, &exc_value, &exc_tb);

        int suppressed = 0;
        for (Py_ssize_t i = entered; i > 0; --i) {
            PyObject *exit_result = async_with
                ? PyObject_CallMethod(managers[i - 1], "__aexit__", "OOO",
                                      exc_type ? exc_type : Py_None,
                                      exc_value ? exc_value : Py_None,
                                      exc_tb ? exc_tb : Py_None)
                : PyObject_CallMethod(managers[i - 1], "__exit__", "OOO",
                                      exc_type ? exc_type : Py_None,
                                      exc_value ? exc_value : Py_None,
                                      exc_tb ? exc_tb : Py_None);
            if (async_with && exit_result) {
                PyObject *resolved = px_await(exit_result);
                Py_DECREF(exit_result); exit_result = resolved;
            }
            if (!exit_result) {
                Py_XDECREF(exc_type); Py_XDECREF(exc_value); Py_XDECREF(exc_tb);
                for (Py_ssize_t j = i - 1; j > 0; --j) {
                    PyObject *r = async_with
                        ? PyObject_CallMethod(managers[j - 1], "__aexit__", Py_None, Py_None, Py_None)
                        : PyObject_CallMethod(managers[j - 1], "__exit__", Py_None, Py_None, Py_None);
                    Py_XDECREF(r);
                }
                for (Py_ssize_t j = entered; j > 0; --j) Py_DECREF(managers[j - 1]);
                PyMem_Free(managers);
                return NULL;
            }
            int truth = PyObject_IsTrue(exit_result);
            Py_DECREF(exit_result);
            if (truth < 0) {
                Py_XDECREF(exc_type); Py_XDECREF(exc_value); Py_XDECREF(exc_tb);
                for (Py_ssize_t j = entered; j > 0; --j) Py_DECREF(managers[j - 1]);
                PyMem_Free(managers);
                return NULL;
            }
            if (truth) {
                suppressed = 1;
                Py_XDECREF(exc_type); Py_XDECREF(exc_value); Py_XDECREF(exc_tb);
                exc_type = exc_value = exc_tb = NULL;
                PyErr_Clear();
                break;
            }
        }

        for (Py_ssize_t i = entered; i > 0; --i) Py_DECREF(managers[i - 1]);
        if (!suppressed) PyErr_Restore(exc_type, exc_value, exc_tb);
        else { Py_XDECREF(exc_type); Py_XDECREF(exc_value); Py_XDECREF(exc_tb); result = Py_NewRef(Py_None); }
    }

    PyMem_Free(managers);
    return result;

fail:
    Py_XDECREF(result);
    for (Py_ssize_t i = entered; i > 0; --i) {
        PyObject *exit_result = async_with
            ? PyObject_CallMethod(managers[i - 1], "__aexit__", Py_None, Py_None, Py_None)
            : PyObject_CallMethod(managers[i - 1], "__exit__", Py_None, Py_None, Py_None);
        if (async_with && exit_result) {
            PyObject *resolved = px_await(exit_result);
            Py_DECREF(exit_result); exit_result = resolved;
        }
        Py_XDECREF(exit_result);
        Py_DECREF(managers[i - 1]);
    }
    PyMem_Free(managers);
    return NULL;
}

static PyObject *px_aug_apply(PyObject *left, PyObject *right, long op)
{
    switch ((operator_ty)op) {
        case Add: return PyNumber_InPlaceAdd(left, right);
        case Sub: return PyNumber_InPlaceSubtract(left, right);
        case Mult: return PyNumber_InPlaceMultiply(left, right);
        case MatMult: return PyNumber_InPlaceMatrixMultiply(left, right);
        case Div: return PyNumber_InPlaceTrueDivide(left, right);
        case FloorDiv: return PyNumber_InPlaceFloorDivide(left, right);
        case Mod: return PyNumber_InPlaceRemainder(left, right);
        case Pow: return PyNumber_InPlacePower(left, right, Py_None);
        case LShift: return PyNumber_InPlaceLshift(left, right);
        case RShift: return PyNumber_InPlaceRshift(left, right);
        case BitOr: return PyNumber_InPlaceOr(left, right);
        case BitXor: return PyNumber_InPlaceXor(left, right);
        case BitAnd: return PyNumber_InPlaceAnd(left, right);
        default:
            PyErr_SetString(PyExc_SystemError, "invalid PythonX augmented-assignment operator");
            return NULL;
    }
}

static PyObject *px_aug_assign(const PyXIRNode *n, PyObject *g, PXState *s)
{
    long op = PyLong_AsLong(n->constant);
    if (op == -1 && PyErr_Occurred()) return NULL;
    const PyXIRNode *target = n->children[0];
    PyObject *old = NULL, *value = NULL, *result = NULL;

    if (target->op == PYX_IR_NAME_LOAD) {
        old = px_eval(target, g, s);
        if (!old) return NULL;
        value = px_eval(n->children[1], g, s);
        if (!value) { Py_DECREF(old); return NULL; }
        result = px_aug_apply(old, value, op);
        Py_DECREF(value);
        if (!result) { Py_DECREF(old); return NULL; }
        if (PyDict_SetItem(g, target->constant, result) < 0) {
            Py_DECREF(old); Py_DECREF(result); return NULL;
        }
        Py_DECREF(old);
        Py_DECREF(result);
        return Py_NewRef(Py_None);
    }

    if (target->op == PYX_IR_GETATTR) {
        PyObject *object = px_eval(target->left, g, s);
        if (!object) return NULL;
        old = PyObject_GetAttr(object, target->constant);
        if (!old) { Py_DECREF(object); return NULL; }
        value = px_eval(n->children[1], g, s);
        if (!value) { Py_DECREF(old); Py_DECREF(object); return NULL; }
        result = px_aug_apply(old, value, op);
        Py_DECREF(value);
        if (!result) { Py_DECREF(old); Py_DECREF(object); return NULL; }
        int rc = PyObject_SetAttr(object, target->constant, result);
        Py_DECREF(old);
        Py_DECREF(result);
        Py_DECREF(object);
        return rc < 0 ? NULL : Py_NewRef(Py_None);
    }

    if (target->op == PYX_IR_SUBSCRIPT) {
        PyObject *object = px_eval(target->children[0], g, s);
        if (!object) return NULL;
        PyObject *key = px_eval(target->children[1], g, s);
        if (!key) { Py_DECREF(object); return NULL; }
        old = PyObject_GetItem(object, key);
        if (!old) { Py_DECREF(key); Py_DECREF(object); return NULL; }
        value = px_eval(n->children[1], g, s);
        if (!value) { Py_DECREF(old); Py_DECREF(key); Py_DECREF(object); return NULL; }
        result = px_aug_apply(old, value, op);
        Py_DECREF(value);
        if (!result) { Py_DECREF(old); Py_DECREF(key); Py_DECREF(object); return NULL; }
        int rc = PyObject_SetItem(object, key, result);
        Py_DECREF(old);
        Py_DECREF(result);
        Py_DECREF(key);
        Py_DECREF(object);
        return rc < 0 ? NULL : Py_NewRef(Py_None);
    }

    PyErr_SetString(PyExc_SystemError, "invalid PythonX augmented-assignment target");
    return NULL;
}

static PyObject *px_eval(const PyXIRNode *n, PyObject *g, PXState *s)
{
    if (!n) return Py_NewRef(Py_None);
    switch (n->op) {
    case PYX_IR_CONST: return Py_NewRef(n->constant);
    case PYX_IR_SEQUENCE: return px_suite(n, g, s);
    case PYX_IR_NAME_LOAD:
        return px_name_load(g, n->constant);
    case PYX_IR_AUG_ASSIGN:return px_aug_assign(n,g,s);
    case PYX_IR_ASSIGN_CHAIN: {
        PyObject *value=px_eval(n->left,g,s);
        if(!value)return NULL;
        for(Py_ssize_t i=0;i<n->child_count;++i){
            if(px_assign_target(n->children[i],g,value,s)<0){Py_DECREF(value);return NULL;}
        }
        Py_DECREF(value);
        return Py_NewRef(Py_None);
    }
    case PYX_IR_NAME_STORE: {
        PyObject *value = px_eval(n->left, g, s);
        if (!value) return NULL;
        int rc = PyDict_SetItem(g, n->constant, value);
        Py_DECREF(value);
        return rc < 0 ? NULL : Py_NewRef(Py_None);
    }
    case PYX_IR_DELETE:
        if (n->constant) {
            if (PyDict_DelItem(g, n->constant) < 0) return NULL;
            return Py_NewRef(Py_None);
        }
        {
            PyObject *object = px_eval(n->children[0], g, s);
            PyObject *key = px_eval(n->children[1], g, s);
            if (!object || !key) { Py_XDECREF(object); Py_XDECREF(key); return NULL; }
            int rc = PyObject_DelItem(object, key);
            Py_DECREF(object); Py_DECREF(key);
            return rc < 0 ? NULL : Py_NewRef(Py_None);
        }
    case PYX_IR_DELATTR: {
        PyObject *object = px_eval(n->left, g, s);
        if (!object) return NULL;
        int rc = PyObject_DelAttr(object, n->constant);
        Py_DECREF(object);
        return rc < 0 ? NULL : Py_NewRef(Py_None);
    }
    case PYX_IR_GETATTR: {
        PyObject *object = px_eval(n->left, g, s); if (!object) return NULL;
        PyObject *result = PyObject_GetAttr(object, n->constant); Py_DECREF(object); return result;
    }
    case PYX_IR_SETATTR: {
        PyObject *object = px_eval(n->left, g, s), *value = px_eval(n->right, g, s);
        if (!object || !value) { Py_XDECREF(object); Py_XDECREF(value); return NULL; }
        int rc = PyObject_SetAttr(object, n->constant, value); Py_DECREF(object); Py_DECREF(value);
        return rc < 0 ? NULL : Py_NewRef(Py_None);
    }
    case PYX_IR_SUBSCRIPT: {
        PyObject *object = px_eval(n->children[0], g, s), *key = px_eval(n->children[1], g, s);
        if (!object || !key) { Py_XDECREF(object); Py_XDECREF(key); return NULL; }
        PyObject *result = PyObject_GetItem(object, key); Py_DECREF(object); Py_DECREF(key); return result;
    }
    case PYX_IR_SUBSCRIPT_STORE: {
        PyObject *object=px_eval(n->children[0],g,s),*key=px_eval(n->children[1],g,s),*value=px_eval(n->children[2],g,s);
        if(!object||!key||!value){Py_XDECREF(object);Py_XDECREF(key);Py_XDECREF(value);return NULL;}
        int rc=PyObject_SetItem(object,key,value);Py_DECREF(object);Py_DECREF(key);Py_DECREF(value);return rc<0?NULL:Py_NewRef(Py_None);
    }
    case PYX_IR_LIST: case PYX_IR_TUPLE: case PYX_IR_SET: {
        PyObject *result=n->op==PYX_IR_LIST?PyList_New(n->child_count):n->op==PYX_IR_TUPLE?PyTuple_New(n->child_count):PySet_New(NULL);
        if(!result)return NULL;
        for(Py_ssize_t i=0;i<n->child_count;i++){PyObject*v=px_eval(n->children[i],g,s);if(!v){Py_DECREF(result);return NULL;}if(n->op==PYX_IR_LIST)PyList_SET_ITEM(result,i,v);else if(n->op==PYX_IR_TUPLE)PyTuple_SET_ITEM(result,i,v);else{int rc=PySet_Add(result,v);Py_DECREF(v);if(rc<0){Py_DECREF(result);return NULL;}}}
        return result;
    }
    case PYX_IR_NAMED_EXPR: {
        PyObject *value=px_eval(n->children[1],g,s); if(!value)return NULL;
        if(px_assign_target(n->children[0],g,value,s)<0){Py_DECREF(value);return NULL;}
        return value;
    }
    case PYX_IR_DICT: {
        PyObject *result=PyDict_New();if(!result)return NULL;
        for(Py_ssize_t i=0;i<n->child_count;i+=2){
            PyObject *v=px_eval(n->children[i+1],g,s); if(!v){Py_DECREF(result);return NULL;}
            if(n->children[i]->op==PYX_IR_DICT_UNPACK){
                if(PyDict_Update(result,v)<0){Py_DECREF(v);Py_DECREF(result);return NULL;}
                Py_DECREF(v); continue;
            }
            PyObject*k=px_eval(n->children[i],g,s); if(!k){Py_DECREF(v);Py_DECREF(result);return NULL;}
            int rc=PyDict_SetItem(result,k,v);Py_DECREF(k);Py_DECREF(v);if(rc<0){Py_DECREF(result);return NULL;}
        }
        return result;
    }
    case PYX_IR_CALL:return px_call(n,g,s);
    case PYX_IR_COMPARE_CHAIN: {
        PyObject *left=px_eval(n->children[0],g,s);
        if(!left)return NULL;
        for(Py_ssize_t i=0;i<n->child_count-1;++i){
            PyObject *right=px_eval(n->children[i+1],g,s);
            if(!right){Py_DECREF(left);return NULL;}
            int op=(int)PyLong_AsLong(PyTuple_GET_ITEM(n->constant,i));
            PyObject *z=NULL;
            switch((cmpop_ty)op){
                case Lt:z=PyObject_RichCompare(left,right,Py_LT);break;
                case LtE:z=PyObject_RichCompare(left,right,Py_LE);break;
                case Eq:z=PyObject_RichCompare(left,right,Py_EQ);break;
                case NotEq:z=PyObject_RichCompare(left,right,Py_NE);break;
                case Gt:z=PyObject_RichCompare(left,right,Py_GT);break;
                case GtE:z=PyObject_RichCompare(left,right,Py_GE);break;
                case Is:z=PyBool_FromLong(left==right);break;
                case IsNot:z=PyBool_FromLong(left!=right);break;
                case In:{int q=PySequence_Contains(right,left);z=q<0?NULL:PyBool_FromLong(q);break;}
                case NotIn:{int q=PySequence_Contains(right,left);z=q<0?NULL:PyBool_FromLong(!q);break;}
                default:PyErr_SetString(PyExc_NotImplementedError,"PythonX native comparison");z=NULL;
            }
            if(!z){Py_DECREF(left);Py_DECREF(right);return NULL;}
            int truth=PyObject_IsTrue(z);
            Py_DECREF(z);
            if(truth<0){Py_DECREF(left);Py_DECREF(right);return NULL;}
            if(!truth){Py_DECREF(left);Py_DECREF(right);return Py_NewRef(Py_False);}
            Py_SETREF(left,right);
        }
        Py_DECREF(left);
        return Py_NewRef(Py_True);
    }
    case PYX_IR_STARRED:
        return px_eval(n->left, g, s);
    case PYX_IR_DICT_UNPACK:
        return px_eval(n->left ? n->left : (n->child_count ? n->children[0] : NULL), g, s);
    case PYX_IR_ATTRIBUTE: {
        PyObject *object = px_eval(n->left, g, s);
        if (!object) return NULL;
        PyObject *result = PyObject_GetAttr(object, n->constant);
        Py_DECREF(object);
        return result;
    }
    case PYX_IR_TYPE_OF: {
        PyObject *object = px_eval(n->left ? n->left : (n->child_count ? n->children[0] : NULL), g, s);
        if (!object) return NULL;
        PyObject *result = (PyObject *)Py_TYPE(object);
        Py_INCREF(result);
        Py_DECREF(object);
        return result;
    }
    case PYX_IR_INSTANCE_OF: {
        PyObject *object = px_eval(n->children[0], g, s);
        PyObject *type = px_eval(n->children[1], g, s);
        if (!object || !type) { Py_XDECREF(object); Py_XDECREF(type); return NULL; }
        int ok = PyObject_IsInstance(object, type);
        Py_DECREF(object); Py_DECREF(type);
        if (ok < 0) return NULL;
        return PyBool_FromLong(ok);
    }
    case PYX_IR_SUBCLASS_OF: {
        PyObject *derived = px_eval(n->children[0], g, s);
        PyObject *base = px_eval(n->children[1], g, s);
        if (!derived || !base) { Py_XDECREF(derived); Py_XDECREF(base); return NULL; }
        int ok = PyObject_IsSubclass(derived, base);
        Py_DECREF(derived); Py_DECREF(base);
        if (ok < 0) return NULL;
        return PyBool_FromLong(ok);
    }
    case PYX_IR_HASH: {
        PyObject *object = px_eval(n->left ? n->left : (n->child_count ? n->children[0] : NULL), g, s);
        if (!object) return NULL;
        Py_hash_t hash = PyObject_Hash(object);
        Py_DECREF(object);
        if (hash == -1 && PyErr_Occurred()) return NULL;
        return PyLong_FromSsize_t(hash);
    }
    case PYX_IR_ITER: {
        PyObject *object = px_eval(n->left ? n->left : (n->child_count ? n->children[0] : NULL), g, s);
        if (!object) return NULL;
        PyObject *result = PyObject_GetIter(object);
        Py_DECREF(object);
        return result;
    }
    case PYX_IR_NEXT: {
        PyObject *iterator = px_eval(n->children[0], g, s);
        if (!iterator) return NULL;
        PyObject *result = PyIter_Next(iterator);
        if (result) { Py_DECREF(iterator); return result; }
        if (PyErr_Occurred()) { Py_DECREF(iterator); return NULL; }
        if (n->child_count > 1) {
            result = px_eval(n->children[1], g, s);
            Py_DECREF(iterator);
            return result;
        }
        Py_DECREF(iterator);
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
    }
    case PYX_IR_CLASS: {
        PyObject *klass = px_class(n, g, s);
        if (!klass) return NULL;
        PyObject *name = PyTuple_GET_ITEM(n->constant, 0);
        int rc = PyDict_SetItem(g, name, klass);
        if (rc < 0) { Py_DECREF(klass); return NULL; }
        return klass;
    }
    case PYX_IR_FUNCTION: {
        PyObject *fn = px_function(n, g, s);
        if (!fn) return NULL;
        PyObject *name = PyTuple_GET_ITEM(n->constant, 6);
        int rc = PyDict_SetItem(g, name, fn);
        Py_DECREF(fn);
        return rc < 0 ? NULL : Py_NewRef(Py_None);
    }
    case PYX_IR_LAMBDA:return px_lambda(n,g,s);

    case PYX_IR_IF: {
        PyObject*t=px_eval(n->children[0],g,s);if(!t)return NULL;int truth=PyObject_IsTrue(t);Py_DECREF(t);if(truth<0)return NULL;return px_eval(n->children[truth?1:2],g,s);
    }
    case PYX_IR_WHILE: {
        s->loop_depth++;
        PyObject*result=Py_NewRef(Py_None);int broke=0;
        for(;;){PyObject*t=px_eval(n->children[0],g,s);if(!t){Py_DECREF(result);s->loop_depth--;return NULL;}int truth=PyObject_IsTrue(t);Py_DECREF(t);if(truth<0){Py_DECREF(result);s->loop_depth--;return NULL;}if(!truth)break;PyObject*x=px_eval(n->children[1],g,s);if(!x){Py_DECREF(result);s->loop_depth--;return NULL;}Py_DECREF(x);if(s->flow==PX_BREAK){s->flow=PX_NORMAL;broke=1;break;}if(s->flow==PX_CONTINUE){s->flow=PX_NORMAL;continue;}if(s->flow!=PX_NORMAL){Py_DECREF(result);s->loop_depth--;return NULL;}}
        if(!broke){PyObject*x=px_eval(n->children[2],g,s);if(!x){Py_DECREF(result);return NULL;}Py_SETREF(result,x);}return result;
    }
    case PYX_IR_FOR:return px_for(n,g,s,0);
    case PYX_IR_ASYNC_FOR:return px_for(n,g,s,1);
    case PYX_IR_BREAK:
        if (s->loop_depth <= 0) { PyErr_SetString(PyExc_SyntaxError, "'break' outside loop"); return NULL; }
        s->flow=PX_BREAK; return Py_NewRef(Py_None);
    case PYX_IR_CONTINUE:
        if (s->loop_depth <= 0) { PyErr_SetString(PyExc_SyntaxError, "'continue' not properly in loop"); return NULL; }
        s->flow=PX_CONTINUE; return Py_NewRef(Py_None);
    case PYX_IR_RETURN:s->flow=PX_RETURN;return n->left?px_eval(n->left,g,s):Py_NewRef(Py_None);
    case PYX_IR_AWAIT:{PyObject*a=px_eval(n->left,g,s);if(!a)return NULL;PyObject*r=px_await(a);Py_DECREF(a);return r;}
    case PYX_IR_WITH:return px_with(n,g,s,0);
    case PYX_IR_ASYNC_WITH:return px_with(n,g,s,1);
    case PYX_IR_RAISE:{PyObject*e=px_eval(n->children[0],g,s),*c=n->child_count>1?px_eval(n->children[1],g,s):NULL;if(!e){Py_XDECREF(c);return NULL;}int rc=_PyX_StatementRaise(e,c);Py_DECREF(e);Py_XDECREF(c);return rc<0?NULL:Py_NewRef(Py_None);}
    case PYX_IR_RERAISE:return _PyX_StatementReraise()<0?NULL:Py_NewRef(Py_None);
    case PYX_IR_IMPORT: {
        PyObject *items = n->constant;
        Py_ssize_t count = PyTuple_GET_SIZE(items);
        for (Py_ssize_t i = 0; i < count; ++i) {
            PyObject *item = PyTuple_GET_ITEM(items, i);
            PyObject *module_name = PyTuple_GET_ITEM(item, 0);
            PyObject *asname = PyTuple_GET_ITEM(item, 1);
            PyObject *module = PyImport_Import(module_name);
            if (!module) return NULL;
            PyObject *bind = asname != Py_None ? asname : module_name;
            PyObject *dot = PyUnicode_FindChar(bind, '.', 0, PyUnicode_GET_LENGTH(bind), 1);
            PyObject *name = NULL;
            if (asname == Py_None && dot != Py_None) {
                name = PyUnicode_Substring(bind, 0, PyUnicode_GET_LENGTH(bind));
            }
            if (asname == Py_None) {
                Py_ssize_t pos = PyUnicode_FindChar(bind, '.', 0, PyUnicode_GET_LENGTH(bind), 1);
                if (pos >= 0) {
                    Py_DECREF(module);
                    module = PyImport_Import(PyUnicode_Substring(bind, 0, pos));
                    if (!module) return NULL;
                    bind = PyTuple_GET_ITEM(item, 0);
                    Py_INCREF(module);
                    Py_DECREF(module);
                    module = PyImport_Import(PyUnicode_Substring(bind, 0, pos));
                    if (!module) return NULL;
                }
            }
            int rc = PyDict_SetItem(g, bind, module);
            Py_XDECREF(name);
            Py_DECREF(module);
            if (rc < 0) return NULL;
        }
        return Py_NewRef(Py_None);
    }
    case PYX_IR_IMPORT_FROM: {
        PyObject *meta = n->constant;
        PyObject *module_name = PyTuple_GET_ITEM(meta, 0);
        long level = PyLong_AsLong(PyTuple_GET_ITEM(meta, 1));
        if (level == -1 && PyErr_Occurred()) return NULL;
        PyObject *module = module_name == Py_None ? PyUnicode_FromString("") : Py_NewRef(module_name);
        if (!module) return NULL;
        PyObject *pkg = PyImport_Import(module);
        Py_DECREF(module);
        if (!pkg) return NULL;
        Py_ssize_t count = PyTuple_GET_SIZE(meta) - 2;
        for (Py_ssize_t i = 0; i < count; ++i) {
            PyObject *item = PyTuple_GET_ITEM(meta, i + 2);
            PyObject *name = PyTuple_GET_ITEM(item, 0);
            PyObject *asname = PyTuple_GET_ITEM(item, 1);
            if (PyUnicode_CompareWithASCIIString(name, "*") == 0) {
                PyObject *dict = PyModule_GetDict(pkg);
                if (!dict) { Py_DECREF(pkg); return NULL; }
                if (PyDict_Update(g, dict) < 0) { Py_DECREF(pkg); return NULL; }
                continue;
            }
            PyObject *value = PyObject_GetAttr(pkg, name);
            if (!value) { Py_DECREF(pkg); return NULL; }
            PyObject *bind = asname != Py_None ? asname : name;
            int rc = PyDict_SetItem(g, bind, value);
            Py_DECREF(value);
            if (rc < 0) { Py_DECREF(pkg); return NULL; }
        }
        Py_DECREF(pkg);
        return Py_NewRef(Py_None);
    }
    case PYX_IR_ASSERT:{PyObject*t=px_eval(n->children[0],g,s),*m=px_eval(n->children[1],g,s);if(!t||!m){Py_XDECREF(t);Py_XDECREF(m);return NULL;}int rc=_PyX_StatementAssert(t,m);Py_DECREF(t);Py_DECREF(m);return rc<0?NULL:Py_NewRef(Py_None);}
    case PYX_IR_NOT:case PYX_IR_INVERT:case PYX_IR_POSITIVE:case PYX_IR_NEGATIVE:{PyObject*v=px_eval(n->left,g,s);if(!v)return NULL;PyObject*r;if(n->op==PYX_IR_NOT){int t=PyObject_IsTrue(v);r=t<0?NULL:PyBool_FromLong(!t);}else if(n->op==PYX_IR_INVERT)r=PyNumber_Invert(v);else if(n->op==PYX_IR_POSITIVE)r=PyNumber_Positive(v);else r=PyNumber_Negative(v);Py_DECREF(v);return r;}
    case PYX_IR_SLICE:{PyObject*a=px_eval(n->children[0],g,s),*b=px_eval(n->children[1],g,s),*c=px_eval(n->children[2],g,s);if(!a||!b||!c){Py_XDECREF(a);Py_XDECREF(b);Py_XDECREF(c);return NULL;}PyObject*r=PySlice_New(a,b,c);Py_DECREF(a);Py_DECREF(b);Py_DECREF(c);return r;}
    case PYX_IR_FSTRING:{
        PyObject *result=PyUnicode_New(0,127);
        if(!result)return NULL;
        for(Py_ssize_t i=0;i<n->child_count;++i){
            PyObject *part=px_eval(n->children[i],g,s);
            if(!part){Py_DECREF(result);return NULL;}
            if(!PyUnicode_Check(part)){
                PyObject *text=PyObject_Str(part);
                Py_DECREF(part);
                part=text;
                if(!part){Py_DECREF(result);return NULL;}
            }
            PyUnicode_AppendAndDel(&result,part);
            if(!result)return NULL;
        }
        return result;
    }
    case PYX_IR_FORMAT_VALUE:{
        PyObject *value=px_eval(n->children[0],g,s);
        if(!value)return NULL;
        long conversion=PyLong_AsLong(n->constant);
        if(conversion==-1 && PyErr_Occurred()){Py_DECREF(value);return NULL;}
        PyObject *converted=value;
        if(conversion=='s') converted=PyObject_Str(value);
        else if(conversion=='r') converted=PyObject_Repr(value);
        else if(conversion=='a') converted=PyObject_ASCII(value);
        else Py_INCREF(converted);
        Py_DECREF(value);
        if(!converted)return NULL;
        PyObject *spec=px_eval(n->children[1],g,s);
        if(!spec){Py_DECREF(converted);return NULL;}
        if(spec==Py_None){Py_DECREF(spec);return converted;}
        if(!PyUnicode_Check(spec)){
            PyObject *tmp=PyObject_Str(spec);
            Py_DECREF(spec);
            spec=tmp;
            if(!spec){Py_DECREF(converted);return NULL;}
        }
        PyObject *out=PyObject_Format(converted,spec);
        Py_DECREF(converted);
        Py_DECREF(spec);
        return out;
    }
    default:break;
    }
    if(n->op>=PYX_IR_ADD&&n->op<=PYX_IR_OR){PyObject*l=px_eval(n->left,g,s);if(!l)return NULL;if(n->op==PYX_IR_AND||n->op==PYX_IR_OR){int t=PyObject_IsTrue(l);if(t<0){Py_DECREF(l);return NULL;}if((n->op==PYX_IR_AND&&!t)||(n->op==PYX_IR_OR&&t))return l;}PyObject*r=px_eval(n->right,g,s);if(!r){Py_DECREF(l);return NULL;}PyObject*z=NULL;switch(n->op){case PYX_IR_ADD:z=PyNumber_Add(l,r);break;case PYX_IR_SUB:z=PyNumber_Subtract(l,r);break;case PYX_IR_MUL:z=PyNumber_Multiply(l,r);break;case PYX_IR_MATMUL:z=PyNumber_MatrixMultiply(l,r);break;case PYX_IR_DIV:z=PyNumber_TrueDivide(l,r);break;case PYX_IR_FLOORDIV:z=PyNumber_FloorDivide(l,r);break;case PYX_IR_MOD:z=PyNumber_Remainder(l,r);break;case PYX_IR_POW:z=PyNumber_Power(l,r,Py_None);break;case PYX_IR_LSHIFT:z=PyNumber_Lshift(l,r);break;case PYX_IR_RSHIFT:z=PyNumber_Rshift(l,r);break;case PYX_IR_BITOR:z=PyNumber_Or(l,r);break;case PYX_IR_BITXOR:z=PyNumber_Xor(l,r);break;case PYX_IR_BITAND:z=PyNumber_And(l,r);break;case PYX_IR_LT:z=PyObject_RichCompare(l,r,Py_LT);break;case PYX_IR_LE:z=PyObject_RichCompare(l,r,Py_LE);break;case PYX_IR_EQ:z=PyObject_RichCompare(l,r,Py_EQ);break;case PYX_IR_NE:z=PyObject_RichCompare(l,r,Py_NE);break;case PYX_IR_GT:z=PyObject_RichCompare(l,r,Py_GT);break;case PYX_IR_GE:z=PyObject_RichCompare(l,r,Py_GE);break;case PYX_IR_IS:z=PyBool_FromLong(l==r);break;case PYX_IR_IS_NOT:z=PyBool_FromLong(l!=r);break;case PYX_IR_IN:{int x=PySequence_Contains(r,l);z=x<0?NULL:PyBool_FromLong(x);break;}case PYX_IR_NOT_IN:{int x=PySequence_Contains(r,l);z=x<0?NULL:PyBool_FromLong(!x);break;}default:z=Py_NewRef(r);break;}Py_DECREF(l);Py_DECREF(r);return z;}
    PyErr_Format(PyExc_NotImplementedError,"PythonX native IR: operation %d is not executable yet",(int)n->op);return NULL;
}

PyObject *_PyX_NativeEvaluateIR(const PyXIRNode*n,PyObject*g)
{
    if(!n||!g||!PyDict_Check(g)){PyErr_SetString(PyExc_TypeError,"invalid PythonX native IR state");return NULL;}
    PXState s={PX_NORMAL};PyObject*r=px_eval(n,g,&s);if(!r)return NULL;
    if(s.flow==PX_BREAK||s.flow==PX_CONTINUE){Py_DECREF(r);PyErr_SetString(PyExc_SyntaxError,"break/continue outside loop");return NULL;}
    if(s.flow==PX_RETURN){Py_DECREF(r);PyErr_SetString(PyExc_SyntaxError,"'return' outside function");return NULL;}
    return r;
}

#if defined(__x86_64__) || defined(_M_X64)
typedef struct { unsigned char *code; size_t size; PyXIRNode *root; PyObject *globals; } XIRNativeCode;
typedef PyObject *(*XIRNativeFunction)(void);
static void *alloc_exec(size_t size){
#if defined(_WIN32)
    return VirtualAlloc(NULL,size,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
#else
    void *p=mmap(NULL,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);return p==MAP_FAILED?NULL:p;
#endif
}
static void free_exec(void*p,size_t size){
#if defined(_WIN32)
    (void)size;if(p)VirtualFree(p,0,MEM_RELEASE);
#else
    if(p)munmap(p,size);
#endif
}
static void capsule_free(PyObject*c){XIRNativeCode*n=PyCapsule_GetPointer(c,"PythonX.native_ir_code");if(!n){PyErr_Clear();return;}free_exec(n->code,n->size);PyXIRFunction f={n->root,n->globals};_PyX_IR_Free(&f);PyMem_RawFree(n);}
PyObject *_PyX_NativeCompileIR(const PyXIRFunction*f)
{
    if(!f||!f->root){PyErr_SetString(PyExc_TypeError,"PythonX native IR compiler requires a function");return NULL;}
    unsigned char code[64];size_t p=0;
#if defined(_WIN32)
    code[p++]=0x48;code[p++]=0xB9;
#else
    code[p++]=0x48;code[p++]=0xBF;
#endif
    uint64_t root=(uint64_t)(uintptr_t)f->root;memcpy(code+p,&root,8);p+=8;
#if defined(_WIN32)
    code[p++]=0x48;code[p++]=0xBA;
#else
    code[p++]=0x48;code[p++]=0xBE;
#endif
    PyObject*g=f->globals?Py_NewRef(f->globals):PyDict_New();if(!g)return NULL;uint64_t gp=(uint64_t)(uintptr_t)g;memcpy(code+p,&gp,8);p+=8;
    code[p++]=0x48;code[p++]=0xB8;uint64_t fn=(uint64_t)(uintptr_t)&_PyX_NativeEvaluateIR;memcpy(code+p,&fn,8);p+=8;code[p++]=0xFF;code[p++]=0xD0;code[p++]=0xC3;
    void*m=alloc_exec(p);if(!m){Py_DECREF(g);PyErr_SetString(PyExc_MemoryError,"PythonX could not allocate executable memory");return NULL;}memcpy(m,code,p);
    XIRNativeCode*n=PyMem_RawMalloc(sizeof(*n));if(!n){free_exec(m,p);Py_DECREF(g);PyErr_NoMemory();return NULL;}n->code=m;n->size=p;n->root=f->root;n->globals=g;((PyXIRFunction*)f)->root=NULL;((PyXIRFunction*)f)->globals=NULL;
    PyObject*c=PyCapsule_New(n,"PythonX.native_ir_code",capsule_free);if(!c){free_exec(m,p);Py_DECREF(g);PyMem_RawFree(n);return NULL;}return c;
}
PyObject *_PyX_NativeExecuteIR(PyObject*code){if(!PyCapsule_IsValid(code,"PythonX.native_ir_code")){PyErr_SetString(PyExc_TypeError,"invalid PythonX native IR code");return NULL;}XIRNativeCode*n=PyCapsule_GetPointer(code,"PythonX.native_ir_code");if(!n||!n->code){PyErr_SetString(PyExc_RuntimeError,"empty PythonX native code");return NULL;}return((XIRNativeFunction)n->code)();}
#else
PyObject *_PyX_NativeCompileIR(const PyXIRFunction*f){Py_UNUSED(f);PyErr_SetString(PyExc_NotImplementedError,"PythonX native backend currently targets x86-64");return NULL;}
PyObject *_PyX_NativeExecuteIR(PyObject*c){Py_UNUSED(c);PyErr_SetString(PyExc_NotImplementedError,"PythonX native backend currently targets x86-64");return NULL;}
#endif