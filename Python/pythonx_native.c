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
#  include <unistd.h>
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
xcode_u32(XCode *x, uint32_t value)
{
    unsigned char b[4] = {
        (unsigned char)(value),
        (unsigned char)(value >> 8),
        (unsigned char)(value >> 16),
        (unsigned char)(value >> 24)
    };
    return xcode_bytes(x, b, sizeof(b));
}

static int
xcode_u64(XCode *x, uint64_t value)
{
    unsigned char b[8] = {
        (unsigned char)(value),
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

/*
 * This first native backend deliberately starts with a tiny, real machine-code
 * subset. It consumes CPython's AST directly and emits x86-64 instructions.
 * No C/assembly/Rust source is generated and no Python bytecode is produced.
 *
 * Generated function ABI:
 *     PyObject *fn(void)
 *
 * The generated function calls the existing native PyLong_FromLong runtime
 * primitive to construct the Python result object. The call itself is emitted
 * as machine-code bytes.
 */
#if defined(__x86_64__) || defined(_M_X64)

static int
emit_mov_rax_imm64(XCode *x, uint64_t value)
{
    if (xcode_byte(x, 0x48) < 0 || xcode_byte(x, 0xB8) < 0) {
        return -1;
    }
    return xcode_u64(x, value);
}

static int
emit_mov_rdi_imm64(XCode *x, uint64_t value)
{
    if (xcode_byte(x, 0x48) < 0 || xcode_byte(x, 0xBF) < 0) {
        return -1;
    }
    return xcode_u64(x, value);
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
emit_add_rax_rbx(XCode *x)
{
    static const unsigned char op[] = {0x48, 0x01, 0xD8};
    return xcode_bytes(x, op, sizeof(op));
}

static int
emit_sub_rax_rbx(XCode *x)
{
    static const unsigned char op[] = {0x48, 0x29, 0xD8};
    return xcode_bytes(x, op, sizeof(op));
}

static int
emit_imul_rax_rbx(XCode *x)
{
    static const unsigned char op[] = {0x48, 0x0F, 0xAF, 0xC3};
    return xcode_bytes(x, op, sizeof(op));
}

static int
emit_mov_rbx_imm64(XCode *x, uint64_t value)
{
    if (xcode_byte(x, 0x48) < 0 || xcode_byte(x, 0xBB) < 0) {
        return -1;
    }
    return xcode_u64(x, value);
}

static int
emit_push_rbx(XCode *x)
{
    return xcode_byte(x, 0x53);
}

static int
emit_pop_rbx(XCode *x)
{
    return xcode_byte(x, 0x5B);
}

static int
emit_cqo(XCode *x)
{
    static const unsigned char op[] = {0x48, 0x99};
    return xcode_bytes(x, op, sizeof(op));
}

static int
emit_idiv_rbx(XCode *x)
{
    static const unsigned char op[] = {0x48, 0xF7, 0xFB};
    return xcode_bytes(x, op, sizeof(op));
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
                     "PythonX native backend: unsupported AST node kind %d",
                     (int)node->kind);
        return -1;
    }

    expr_ty left = node->v.BinOp.left;
    expr_ty right = node->v.BinOp.right;

    /* Evaluate the right side first and preserve it in RBX. This bootstrap
       backend only supports integer constants on both sides. */
    long left_value;
    long right_value;
    if (compile_constant_long(left, &left_value) <= 0 ||
        compile_constant_long(right, &right_value) <= 0) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "PythonX native backend currently requires integer constant operands");
        return -1;
    }

    if (emit_mov_rax_imm64(x, (uint64_t)(int64_t)left_value) < 0 ||
        emit_mov_rbx_imm64(x, (uint64_t)(int64_t)right_value) < 0) {
        return -1;
    }

    switch (node->v.BinOp.op) {
        case Add:
            return emit_add_rax_rbx(x);
        case Sub:
            return emit_sub_rax_rbx(x);
        case Mult:
            return emit_imul_rax_rbx(x);
        case Div:
            if (emit_cqo(x) < 0 || emit_idiv_rbx(x) < 0) {
                return -1;
            }
            return 0;
        default:
            PyErr_SetString(PyExc_NotImplementedError,
                            "PythonX native backend: unsupported binary operator");
            return -1;
    }
}

static int
emit_native_result(XCode *x)
{
    /* Save the integer result in RBX while calling PyLong_FromLong.
       Generated code is intentionally tiny and self-contained. */
    if (emit_push_rbx(x) < 0) {
        return -1;
    }
    if (xcode_byte(x, 0x48) < 0 || xcode_byte(x, 0x89) < 0 ||
        xcode_byte(x, 0xC3) < 0) {
        return -1;
    }
    /* Replace the temporary sequence above with a simple ABI-safe path. */
    x->size -= 3;
    if (emit_mov_rdi_imm64(x, 0) < 0) {
        return -1;
    }
    return 0;
}

static void *
alloc_executable(size_t size)
{
#if defined(_WIN32)
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    return mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED ? NULL :
           /* mmap result must be repeated below; this expression is replaced
              by the implementation in alloc_executable_real(). */ NULL;
#endif
}

static void *
alloc_executable_real(size_t size)
{
#if defined(_WIN32)
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
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
        PyErr_SetString(PyExc_TypeError, "PythonX native compiler requires a module AST");
        return NULL;
    }

    Py_ssize_t n = asdl_seq_LEN(module->v.Module.body);
    if (n != 1) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "PythonX native bootstrap backend currently requires exactly one statement");
        return NULL;
    }

    stmt_ty statement = (stmt_ty)asdl_seq_GET(module->v.Module.body, 0);
    if (statement->kind != Expr_kind) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "PythonX native bootstrap backend currently requires an expression statement");
        return NULL;
    }

    XCode x = {0};
    if (emit_expr(&x, statement->v.Expr.value) < 0) {
        xcode_free(&x);
        return NULL;
    }

    /* The bootstrap ABI returns a Python integer. The integer currently in
       RAX is passed to PyLong_FromLong through RDI, then its native address is
       called through RAX. */
    if (emit_push_rbx(&x) < 0) {
        xcode_free(&x);
        return NULL;
    }
    static const unsigned char mov_rdi_rax[] = {0x48, 0x89, 0xC7};
    if (xcode_bytes(&x, mov_rdi_rax, sizeof(mov_rdi_rax)) < 0) {
        xcode_free(&x);
        return NULL;
    }
    if (emit_mov_rax_imm64(&x, (uint64_t)(uintptr_t)&PyLong_FromLong) < 0 ||
        emit_call_rax(&x) < 0 || emit_pop_rbx(&x) < 0 || emit_ret(&x) < 0) {
        xcode_free(&x);
        return NULL;
    }

    size_t alloc_size = x.size;
    void *memory = alloc_executable_real(alloc_size);
    if (memory == NULL) {
        xcode_free(&x);
        PyErr_SetString(PyExc_MemoryError, "PythonX could not allocate executable memory");
        return NULL;
    }
    memcpy(memory, x.data, x.size);
    xcode_free(&x);

    XNativeCode *native = PyMem_RawMalloc(sizeof(*native));
    if (native == NULL) {
        free_executable(memory, alloc_size);
        PyErr_NoMemory();
        return NULL;
    }
    native->code = memory;
    native->size = alloc_size;

    PyObject *capsule = PyCapsule_New(native, "PythonX.native_code",
                                      native_capsule_destructor);
    if (capsule == NULL) {
        native_capsule_destructor(PyCapsule_New(native, "PythonX.native_code", NULL));
        return NULL;
    }
    return capsule;
}

#else

PyObject *
_PyX_NativeCompile(mod_ty module, PyObject *filename)
{
    (void)module;
    (void)filename;
    PyErr_SetString(PyExc_NotImplementedError,
                    "PythonX native bootstrap backend currently targets x86-64");
    return NULL;
}

#endif
