#include "Python.h"
#include "pycore_ast.h"
#include "pythonx_native.h"

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} XCode;

typedef struct {
    unsigned char *code;
    size_t size;
} XNativeCode;

typedef PyObject *(*XNativeFunction)(void);

static void
xcode_free(XCode *x)
{
    PyMem_RawFree(x->data);
    x->data = NULL;
    x->size = 0;
    x->capacity = 0;
}

static int
xcode_reserve(XCode *x, size_t extra)
{
    if (extra <= x->capacity - x->size) {
        return 0;
    }

    size_t needed = x->size + extra;
    size_t capacity = x->capacity ? x->capacity : 64;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            PyErr_NoMemory();
            return -1;
        }
        capacity *= 2;
    }

    unsigned char *data = PyMem_RawRealloc(x->data, capacity);
    if (data == NULL) {
        PyErr_NoMemory();
        return -1;
    }

    x->data = data;
    x->capacity = capacity;
    return 0;
}

static int
xcode_byte(XCode *x, unsigned char value)
{
    if (xcode_reserve(x, 1) < 0) {
        return -1;
    }
    x->data[x->size++] = value;
    return 0;
}

static int
xcode_bytes(XCode *x, const unsigned char *data, size_t size)
{
    if (xcode_reserve(x, size) < 0) {
        return -1;
    }
    memcpy(x->data + x->size, data, size);
    x->size += size;
    return 0;
}

static int
xcode_u64(XCode *x, uint64_t value)
{
    unsigned char b[8] = {
        (unsigned char)value,
        (unsigned char)(value >> 8),
        (unsigned char)(value >> 16),
        (unsigned char)(value >> 24),
        (unsigned char)(value >> 32),
        (unsigned char)(value >> 40),
        (unsigned char)(value >> 48),
        (unsigned char)(value >> 56)
    };
    return xcode_bytes(x, b, sizeof(b));
}

#if defined(__x86_64__) || defined(_M_X64)

/*
 * Bootstrap machine-code emitter.
 *
 * The generated function has this ABI:
 *     PyObject *fn(void)
 *
 * The arithmetic is deliberately limited to signed machine-word integer
 * constants for now. Python's real numeric semantics will be implemented by
 * the PythonX native IR/runtime instead of pretending that a CPU integer is a
 * Python integer.
 */

static int
emit_mov_rax_imm64(XCode *x, uint64_t value)
{
    if (xcode_byte(x, 0x48) < 0 || xcode_byte(x, 0xB8) < 0) {
        return -1;
    }
    return xcode_u64(x, value);
}

static int
emit_mov_rcx_imm64(XCode *x, uint64_t value)
{
    if (xcode_byte(x, 0x48) < 0 || xcode_byte(x, 0xB9) < 0) {
        return -1;
    }
    return xcode_u64(x, value);
}

static int
emit_add_rax_rcx(XCode *x)
{
    static const unsigned char op[] = {0x48, 0x01, 0xC8};
    return xcode_bytes(x, op, sizeof(op));
}

static int
emit_sub_rax_rcx(XCode *x)
{
    static const unsigned char op[] = {0x48, 0x29, 0xC8};
    return xcode_bytes(x, op, sizeof(op));
}

static int
emit_imul_rax_rcx(XCode *x)
{
    static const unsigned char op[] = {0x48, 0x0F, 0xAF, 0xC1};
    return xcode_bytes(x, op, sizeof(op));
}

static int
emit_mov_arg_from_rax(XCode *x)
{
#if defined(_WIN32)
    /* Windows x64: first integer/pointer argument is RCX. */
    static const unsigned char op[] = {0x48, 0x89, 0xC1};
#else
    /* System V AMD64: first integer/pointer argument is RDI. */
    static const unsigned char op[] = {0x48, 0x89, 0xC7};
#endif
    return xcode_bytes(x, op, sizeof(op));
}

static int
emit_call_rax(XCode *x)
{
    static const unsigned char op[] = {0xFF, 0xD0};
    return xcode_bytes(x, op, sizeof(op));
}

static int
emit_ret(XCode *x)
{
    return xcode_byte(x, 0xC3);
}

static int
emit_windows_call_frame(XCode *x)
{
#if defined(_WIN32)
    /* Reserve the mandatory 32-byte Windows x64 shadow space. */
    static const unsigned char op[] = {0x48, 0x83, 0xEC, 0x20};
    return xcode_bytes(x, op, sizeof(op));
#else
    (void)x;
    return 0;
#endif
}

static int
emit_windows_call_frame_free(XCode *x)
{
#if defined(_WIN32)
    static const unsigned char op[] = {0x48, 0x83, 0xC4, 0x20};
    return xcode_bytes(x, op, sizeof(op));
#else
    (void)x;
    return 0;
#endif
}

static int
compile_constant_long(expr_ty node, long *value)
{
    if (node->kind != Constant_kind) {
        return 0;
    }

    PyObject *v = node->v.Constant.value;
    if (!PyLong_Check(v)) {
        return 0;
    }

    long n = PyLong_AsLong(v);
    if (n == -1 && PyErr_Occurred()) {
        return -1;
    }

    *value = n;
    return 1;
}

static int
emit_expr(XCode *x, expr_ty node)
{
    long value;
    int constant = compile_constant_long(node, &value);
    if (constant < 0) {
        return -1;
    }
    if (constant) {
        return emit_mov_rax_imm64(x, (uint64_t)(int64_t)value);
    }

    if (node->kind != BinOp_kind) {
        PyErr_Format(PyExc_NotImplementedError,
                     "PythonX native bootstrap: unsupported AST node kind %d",
                     (int)node->kind);
        return -1;
    }

    long left_value;
    long right_value;
    if (compile_constant_long(node->v.BinOp.left, &left_value) <= 0 ||
        compile_constant_long(node->v.BinOp.right, &right_value) <= 0) {
        PyErr_SetString(
            PyExc_NotImplementedError,
            "PythonX native bootstrap currently requires integer constant operands");
        return -1;
    }

    if (emit_mov_rax_imm64(x, (uint64_t)(int64_t)left_value) < 0 ||
        emit_mov_rcx_imm64(x, (uint64_t)(int64_t)right_value) < 0) {
        return -1;
    }

    switch (node->v.BinOp.op) {
        case Add:
            return emit_add_rax_rcx(x);
        case Sub:
            return emit_sub_rax_rcx(x);
        case Mult:
            return emit_imul_rax_rcx(x);
        default:
            /* Do not emit incorrect semantics for Python '/' or '//'. */
            PyErr_SetString(
                PyExc_NotImplementedError,
                "PythonX native bootstrap does not yet implement this binary operator");
            return -1;
    }
}

static void *
alloc_executable(size_t size)
{
#if defined(_WIN32)
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                        PAGE_EXECUTE_READWRITE);
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static void
free_executable(void *p, size_t size)
{
    if (p == NULL) {
        return;
    }
#if defined(_WIN32)
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

static void
native_capsule_destructor(PyObject *capsule)
{
    XNativeCode *native = PyCapsule_GetPointer(capsule, "PythonX.native_code");
    if (native == NULL) {
        PyErr_Clear();
        return;
    }

    free_executable(native->code, native->size);
    PyMem_RawFree(native);
}

PyObject *
_PyX_NativeCompile(mod_ty module, PyObject *filename)
{
    (void)filename;

    if (module == NULL || module->kind != Module_kind) {
        PyErr_SetString(PyExc_TypeError,
                        "PythonX native compiler requires a module AST");
        return NULL;
    }

    Py_ssize_t n = asdl_seq_LEN(module->v.Module.body);
    if (n != 1) {
        PyErr_SetString(
            PyExc_NotImplementedError,
            "PythonX native bootstrap currently requires exactly one statement");
        return NULL;
    }

    stmt_ty statement = (stmt_ty)asdl_seq_GET(module->v.Module.body, 0);
    if (statement->kind != Expr_kind) {
        PyErr_SetString(
            PyExc_NotImplementedError,
            "PythonX native bootstrap currently requires an expression statement");
        return NULL;
    }

    XCode x = {0};
    if (emit_expr(&x, statement->v.Expr.value) < 0) {
        xcode_free(&x);
        return NULL;
    }

    /* Convert the native integer result into a Python object. */
    if (emit_mov_arg_from_rax(&x) < 0 ||
        emit_windows_call_frame(&x) < 0 ||
        emit_mov_rax_imm64(&x, (uint64_t)(uintptr_t)&PyLong_FromLong) < 0 ||
        emit_call_rax(&x) < 0 ||
        emit_windows_call_frame_free(&x) < 0 ||
        emit_ret(&x) < 0) {
        xcode_free(&x);
        return NULL;
    }

    void *memory = alloc_executable(x.size);
    if (memory == NULL) {
        xcode_free(&x);
        PyErr_SetString(PyExc_MemoryError,
                        "PythonX could not allocate executable memory");
        return NULL;
    }

    memcpy(memory, x.data, x.size);
    size_t code_size = x.size;
    xcode_free(&x);

    XNativeCode *native = PyMem_RawMalloc(sizeof(*native));
    if (native == NULL) {
        free_executable(memory, code_size);
        PyErr_NoMemory();
        return NULL;
    }

    native->code = memory;
    native->size = code_size;

    PyObject *capsule = PyCapsule_New(native, "PythonX.native_code",
                                      native_capsule_destructor);
    if (capsule == NULL) {
        free_executable(native->code, native->size);
        PyMem_RawFree(native);
        return NULL;
    }

    return capsule;
}

PyObject *
_PyX_NativeExecute(PyObject *native_code)
{
    if (!PyCapsule_IsValid(native_code, "PythonX.native_code")) {
        PyErr_SetString(PyExc_TypeError,
                        "PythonX native execution requires a valid native-code capsule");
        return NULL;
    }

    XNativeCode *native = PyCapsule_GetPointer(native_code,
                                               "PythonX.native_code");
    if (native == NULL) {
        return NULL;
    }

    if (native->code == NULL || native->size == 0) {
        PyErr_SetString(PyExc_RuntimeError,
                        "PythonX native-code capsule contains no executable code");
        return NULL;
    }

    XNativeFunction function = (XNativeFunction)native->code;
    return function();
}

#else

PyObject *
_PyX_NativeCompile(mod_ty module, PyObject *filename)
{
    (void)module;
    (void)filename;
    PyErr_SetString(PyExc_NotImplementedError,
                    "PythonX native bootstrap currently targets x86-64");
    return NULL;
}

PyObject *
_PyX_NativeExecute(PyObject *native_code)
{
    (void)native_code;
    PyErr_SetString(PyExc_NotImplementedError,
                    "PythonX native bootstrap execution currently targets x86-64");
    return NULL;
}

#endif
