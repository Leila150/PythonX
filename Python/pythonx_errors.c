#include "Python.h"
#include "pythonx_errors.h"

/* PythonX keeps the interpreter's native exception machinery intact. */

int
_PyX_ErrorSet(PyObject *exception, PyObject *value)
{
    if (!exception || !PyExceptionClass_Check(exception)) {
        PyErr_SetString(PyExc_TypeError, "exception must be an exception class");
        return -1;
    }
    PyErr_SetObject(exception, value ? value : Py_None);
    return -1;
}

int
_PyX_ErrorSetString(PyObject *exception, const char *message)
{
    if (!exception || !PyExceptionClass_Check(exception)) {
        PyErr_SetString(PyExc_TypeError, "exception must be an exception class");
        return -1;
    }
    PyErr_SetString(exception, message ? message : "");
    return -1;
}

int
_PyX_ErrorFormat(PyObject *exception, const char *format, ...)
{
    if (!exception || !PyExceptionClass_Check(exception)) {
        PyErr_SetString(PyExc_TypeError, "exception must be an exception class");
        return -1;
    }
    va_list args;
    va_start(args, format);
    PyObject *message = PyUnicode_FromFormatV(format, args);
    va_end(args);
    if (!message) {
        return -1;
    }
    PyErr_SetObject(exception, message);
    Py_DECREF(message);
    return -1;
}

void
_PyX_ErrorFetch(PyObject **type, PyObject **value, PyObject **traceback)
{
    PyErr_Fetch(type, value, traceback);
}

void
_PyX_ErrorRestore(PyObject *type, PyObject *value, PyObject *traceback)
{
    PyErr_Restore(type, value, traceback);
}

int
_PyX_ErrorMatches(PyObject *type, PyObject *exc)
{
    if (!type || !exc) {
        return 0;
    }
    return PyErr_GivenExceptionMatches(type, exc);
}

int
_PyX_ErrorMatchesCurrent(PyObject *exc)
{
    return exc != NULL && PyErr_ExceptionMatches(exc);
}

int
_PyX_ErrorNormalize(PyObject **type, PyObject **value, PyObject **traceback)
{
    if (!type || !value || !traceback) {
        PyErr_SetString(PyExc_SystemError, "invalid exception normalization state");
        return -1;
    }
    PyErr_NormalizeException(type, value, traceback);
    return 0;
}

int
_PyX_ErrorAddTraceback(const char *funcname, const char *filename, int lineno)
{
    /* Native traceback frame injection will be connected to PythonX native
     * source locations. There is deliberately no custom traceback type. */
    (void)funcname;
    (void)filename;
    (void)lineno;
    return PyErr_Occurred() ? 0 : 0;
}

void
_PyX_ErrorPrint(void)
{
    PyErr_Print();
}

int
_PyX_ErrorRaiseWithContext(PyObject *exception, PyObject *value)
{
    if (!exception || !PyExceptionClass_Check(exception)) {
        PyErr_SetString(PyExc_TypeError, "exception must be an exception class");
        return -1;
    }
    PyErr_SetObject(exception, value ? value : Py_None);
    return -1;
}

/*
 * Return Python's existing exception/warning object for a catalog ID.
 * No exception classes are defined or allocated here.
 */
PyObject *
_PyX_ErrorType(PyXErrorId id)
{
    switch (id) {
        case PYX_ERROR_BASEEXCEPTION: return PyExc_BaseException;
        case PYX_ERROR_EXCEPTION: return PyExc_Exception;
        case PYX_ERROR_ARITHMETICERROR: return PyExc_ArithmeticError;
        case PYX_ERROR_BUFFERERROR: return PyExc_BufferError;
        case PYX_ERROR_LOOKUPERROR: return PyExc_LookupError;
        case PYX_ERROR_ASSERTIONERROR: return PyExc_AssertionError;
        case PYX_ERROR_ATTRIBUTEERROR: return PyExc_AttributeError;
        case PYX_ERROR_EOFERROR: return PyExc_EOFError;
        case PYX_ERROR_FLOATINGPOINTERROR: return PyExc_FloatingPointError;
        case PYX_ERROR_GENERATOREXIT: return PyExc_GeneratorExit;
        case PYX_ERROR_IMPORTERROR: return PyExc_ImportError;
        case PYX_ERROR_MODULENOTFOUNDERROR: return PyExc_ModuleNotFoundError;
        case PYX_ERROR_INDEXERROR: return PyExc_IndexError;
        case PYX_ERROR_KEYERROR: return PyExc_KeyError;
        case PYX_ERROR_KEYBOARDINTERRUPT: return PyExc_KeyboardInterrupt;
        case PYX_ERROR_MEMORYERROR: return PyExc_MemoryError;
        case PYX_ERROR_NAMEERROR: return PyExc_NameError;
        case PYX_ERROR_NOTIMPLEMENTEDERROR: return PyExc_NotImplementedError;
        case PYX_ERROR_OSERROR: return PyExc_OSError;
        case PYX_ERROR_CONNECTIONERROR: return PyExc_ConnectionError;
        case PYX_ERROR_BROKENPIPEERROR: return PyExc_BrokenPipeError;
        case PYX_ERROR_CONNECTIONABORTEDERROR: return PyExc_ConnectionAbortedError;
        case PYX_ERROR_CONNECTIONREFUSEDERROR: return PyExc_ConnectionRefusedError;
        case PYX_ERROR_CONNECTIONRESETERROR: return PyExc_ConnectionResetError;
        case PYX_ERROR_FILEEXISTSERROR: return PyExc_FileExistsError;
        case PYX_ERROR_FILENOTFOUNDERROR: return PyExc_FileNotFoundError;
        case PYX_ERROR_INTERRUPTEDERROR: return PyExc_InterruptedError;
        case PYX_ERROR_ISADIRECTORYERROR: return PyExc_IsADirectoryError;
        case PYX_ERROR_NOTADIRECTORYERROR: return PyExc_NotADirectoryError;
        case PYX_ERROR_PERMISSIONERROR: return PyExc_PermissionError;
        case PYX_ERROR_PROCESSLOOKUPERROR: return PyExc_ProcessLookupError;
        case PYX_ERROR_TIMEOUTERROR: return PyExc_TimeoutError;
        case PYX_ERROR_OVERFLOWERROR: return PyExc_OverflowError;
        case PYX_ERROR_REFERENCEERROR: return PyExc_ReferenceError;
        case PYX_ERROR_RUNTIMEERROR: return PyExc_RuntimeError;
        case PYX_ERROR_STOPITERATION: return PyExc_StopIteration;
        case PYX_ERROR_STOPASYNCIERATION: return PyExc_StopAsyncIteration;
        case PYX_ERROR_SYNTAXERROR: return PyExc_SyntaxError;
        case PYX_ERROR_INDENTATIONERROR: return PyExc_IndentationError;
        case PYX_ERROR_TABERROR: return PyExc_TabError;
        case PYX_ERROR_SYSTEMERROR: return PyExc_SystemError;
        case PYX_ERROR_SYSTEMEXIT: return PyExc_SystemExit;
        case PYX_ERROR_TYPEERROR: return PyExc_TypeError;
        case PYX_ERROR_UNBOUNDLOCALERROR: return PyExc_UnboundLocalError;
        case PYX_ERROR_UNICODEERROR: return PyExc_UnicodeError;
        case PYX_ERROR_UNICODEENCODEERROR: return PyExc_UnicodeEncodeError;
        case PYX_ERROR_UNICODEDECODEERROR: return PyExc_UnicodeDecodeError;
        case PYX_ERROR_UNICODETRANSLATEERROR: return PyExc_UnicodeTranslateError;
        case PYX_ERROR_VALUEERROR: return PyExc_ValueError;
        case PYX_ERROR_ZERODIVISIONERROR: return PyExc_ZeroDivisionError;
        case PYX_ERROR_WARNING: return PyExc_Warning;
        case PYX_ERROR_USERWARNING: return PyExc_UserWarning;
        case PYX_ERROR_DEPRECATIONWARNING: return PyExc_DeprecationWarning;
        case PYX_ERROR_PENDINGDEPRECATIONWARNING: return PyExc_PendingDeprecationWarning;
        case PYX_ERROR_SYNTAXWARNING: return PyExc_SyntaxWarning;
        case PYX_ERROR_RUNTIMEWARNING: return PyExc_RuntimeWarning;
        case PYX_ERROR_FUTUREWARNING: return PyExc_FutureWarning;
        case PYX_ERROR_IMPORTWARNING: return PyExc_ImportWarning;
        case PYX_ERROR_UNICODEWARNING: return PyExc_UnicodeWarning;
        case PYX_ERROR_BYTESWARNING: return PyExc_BytesWarning;
        case PYX_ERROR_ENCODINGWARNING: return PyExc_EncodingWarning;
        case PYX_ERROR_RESOURCEWARNING: return PyExc_ResourceWarning;
        case PYX_ERROR_EXCEPTIONGROUP: return PyExc_ExceptionGroup;
        case PYX_ERROR_BASEEXCEPTIONGROUP: return PyExc_BaseExceptionGroup;

        /* Compatibility aliases: exactly the same OSError object. */
        case PYX_ERROR_ENVIRONMENTERROR: return PyExc_OSError;
        case PYX_ERROR_IOERROR: return PyExc_OSError;
#ifdef MS_WINDOWS
        case PYX_ERROR_WINDOWSError: return PyExc_OSError;
#else
        case PYX_ERROR_WINDOWSError: return NULL;
#endif
        default: return NULL;
    }
}

const char *
_PyX_ErrorTypeName(PyXErrorId id)
{
    switch (id) {
        case PYX_ERROR_BASEEXCEPTION: return "BaseException";
        case PYX_ERROR_EXCEPTION: return "Exception";
        case PYX_ERROR_ARITHMETICERROR: return "ArithmeticError";
        case PYX_ERROR_BUFFERERROR: return "BufferError";
        case PYX_ERROR_LOOKUPERROR: return "LookupError";
        case PYX_ERROR_ASSERTIONERROR: return "AssertionError";
        case PYX_ERROR_ATTRIBUTEERROR: return "AttributeError";
        case PYX_ERROR_EOFERROR: return "EOFError";
        case PYX_ERROR_FLOATINGPOINTERROR: return "FloatingPointError";
        case PYX_ERROR_GENERATOREXIT: return "GeneratorExit";
        case PYX_ERROR_IMPORTERROR: return "ImportError";
        case PYX_ERROR_MODULENOTFOUNDERROR: return "ModuleNotFoundError";
        case PYX_ERROR_INDEXERROR: return "IndexError";
        case PYX_ERROR_KEYERROR: return "KeyError";
        case PYX_ERROR_KEYBOARDINTERRUPT: return "KeyboardInterrupt";
        case PYX_ERROR_MEMORYERROR: return "MemoryError";
        case PYX_ERROR_NAMEERROR: return "NameError";
        case PYX_ERROR_NOTIMPLEMENTEDERROR: return "NotImplementedError";
        case PYX_ERROR_OSERROR: return "OSError";
        case PYX_ERROR_CONNECTIONERROR: return "ConnectionError";
        case PYX_ERROR_BROKENPIPEERROR: return "BrokenPipeError";
        case PYX_ERROR_CONNECTIONABORTEDERROR: return "ConnectionAbortedError";
        case PYX_ERROR_CONNECTIONREFUSEDERROR: return "ConnectionRefusedError";
        case PYX_ERROR_CONNECTIONRESETERROR: return "ConnectionResetError";
        case PYX_ERROR_FILEEXISTSERROR: return "FileExistsError";
        case PYX_ERROR_FILENOTFOUNDERROR: return "FileNotFoundError";
        case PYX_ERROR_INTERRUPTEDERROR: return "InterruptedError";
        case PYX_ERROR_ISADIRECTORYERROR: return "IsADirectoryError";
        case PYX_ERROR_NOTADIRECTORYERROR: return "NotADirectoryError";
        case PYX_ERROR_PERMISSIONERROR: return "PermissionError";
        case PYX_ERROR_PROCESSLOOKUPERROR: return "ProcessLookupError";
        case PYX_ERROR_TIMEOUTERROR: return "TimeoutError";
        case PYX_ERROR_OVERFLOWERROR: return "OverflowError";
        case PYX_ERROR_REFERENCEERROR: return "ReferenceError";
        case PYX_ERROR_RUNTIMEERROR: return "RuntimeError";
        case PYX_ERROR_STOPITERATION: return "StopIteration";
        case PYX_ERROR_STOPASYNCIERATION: return "StopAsyncIteration";
        case PYX_ERROR_SYNTAXERROR: return "SyntaxError";
        case PYX_ERROR_INDENTATIONERROR: return "IndentationError";
        case PYX_ERROR_TABERROR: return "TabError";
        case PYX_ERROR_SYSTEMERROR: return "SystemError";
        case PYX_ERROR_SYSTEMEXIT: return "SystemExit";
        case PYX_ERROR_TYPEERROR: return "TypeError";
        case PYX_ERROR_UNBOUNDLOCALERROR: return "UnboundLocalError";
        case PYX_ERROR_UNICODEERROR: return "UnicodeError";
        case PYX_ERROR_UNICODEENCODEERROR: return "UnicodeEncodeError";
        case PYX_ERROR_UNICODEDECODEERROR: return "UnicodeDecodeError";
        case PYX_ERROR_UNICODETRANSLATEERROR: return "UnicodeTranslateError";
        case PYX_ERROR_VALUEERROR: return "ValueError";
        case PYX_ERROR_ZERODIVISIONERROR: return "ZeroDivisionError";
        case PYX_ERROR_WARNING: return "Warning";
        case PYX_ERROR_USERWARNING: return "UserWarning";
        case PYX_ERROR_DEPRECATIONWARNING: return "DeprecationWarning";
        case PYX_ERROR_PENDINGDEPRECATIONWARNING: return "PendingDeprecationWarning";
        case PYX_ERROR_SYNTAXWARNING: return "SyntaxWarning";
        case PYX_ERROR_RUNTIMEWARNING: return "RuntimeWarning";
        case PYX_ERROR_FUTUREWARNING: return "FutureWarning";
        case PYX_ERROR_IMPORTWARNING: return "ImportWarning";
        case PYX_ERROR_UNICODEWARNING: return "UnicodeWarning";
        case PYX_ERROR_BYTESWARNING: return "BytesWarning";
        case PYX_ERROR_ENCODINGWARNING: return "EncodingWarning";
        case PYX_ERROR_RESOURCEWARNING: return "ResourceWarning";
        case PYX_ERROR_EXCEPTIONGROUP: return "ExceptionGroup";
        case PYX_ERROR_BASEEXCEPTIONGROUP: return "BaseExceptionGroup";
        case PYX_ERROR_ENVIRONMENTERROR: return "EnvironmentError";
        case PYX_ERROR_IOERROR: return "IOError";
        case PYX_ERROR_WINDOWSError: return "WindowsError";
        default: return NULL;
    }
}

int
_PyX_ErrorTypeIsAlias(PyXErrorId id)
{
    return id == PYX_ERROR_ENVIRONMENTERROR ||
           id == PYX_ERROR_IOERROR ||
           id == PYX_ERROR_WINDOWSError;
}
