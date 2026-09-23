#include "Python.h"
#include "pycore_pythonxobject.h"


static int
pythonx_object_traverse(PyXObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->dict);
    return 0;
}

static int
pythonx_object_clear(PyXObject *self)
{
    Py_CLEAR(self->dict);
    return 0;
}

static void
pythonx_object_dealloc(PyXObject *self)
{
    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    pythonx_object_clear(self);
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static PyObject *
pythonx_object_repr(PyObject *self)
{
    PyTypeObject *type = Py_TYPE(self);
    return PyUnicode_FromFormat("<PythonX %s object at %p>",
                               type->tp_name, self);
}

PyTypeObject PyXObject_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "pythonx.object",
    .tp_basicsize = sizeof(PyXObject),
    .tp_dealloc = (destructor)pythonx_object_dealloc,
    /*
     * Keep print(obj) fully native for PythonX objects.  CPython's normal
     * object string fallback is equivalent to repr(), but providing the slot
     * explicitly makes the PythonX root object own that behavior and ensures
     * print() has a direct string representation.
     *
     * A user-defined __str__ on a PythonX class still overrides this slot
     * through the normal type machinery and the PythonX dunder bridge.
     */
    .tp_repr = pythonx_object_repr,
    .tp_str = pythonx_object_repr,
    .tp_as_async = NULL,
    .tp_flags = Py_TPFLAGS_DEFAULT |
                Py_TPFLAGS_BASETYPE |
                Py_TPFLAGS_HAVE_GC,
    .tp_doc = "PythonX root object type.",
    .tp_traverse = (traverseproc)pythonx_object_traverse,
    .tp_clear = (inquiry)pythonx_object_clear,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_setattro = PyObject_GenericSetAttr,
    .tp_dictoffset = offsetof(PyXObject, dict),
    .tp_alloc = PyType_GenericAlloc,
    .tp_free = PyObject_GC_Del,
    .tp_new = PyType_GenericNew,
};

PyObject *
_PyX_NewObject(PyTypeObject *type)
{
    return type->tp_new(type, NULL, NULL);
}
