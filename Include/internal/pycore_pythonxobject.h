#ifndef Py_INTERNAL_PYTHONXOBJECT_H
#define Py_INTERNAL_PYTHONXOBJECT_H

#include "Python.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PythonX root object.
 *
 * PythonX-defined classes use this type as their implicit root base.  It keeps
 * ordinary Python object semantics while giving PythonX a native object type
 * that can grow its own allocation, attribute, GC, and data-model machinery
 * without changing the public PyObject ABI.
 */
typedef struct {
    PyObject_HEAD
    PyObject *dict;
} PyXObject;

PyAPI_DATA(PyTypeObject) PyXObject_Type;

#define PyXObject_Check(op) PyObject_TypeCheck((op), &PyXObject_Type)

#ifdef __cplusplus
}
#endif

#endif /* Py_INTERNAL_PYTHONXOBJECT_H */
