#include "Python.h"
#include "pythonx_ir.h"
#include "pythonx_native_ir.h"

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} XIRCode;

typedef struct {
    unsigned char *code;
    size_t size;
} XIRNativeCode;

typedef PyObject *(*XIRNativeFunction)(void);

static int code_reserve(XIRCode *code, size_t extra)
{
    if (extra <= code->capacity - code->size) {
        return 0;
    }

    size_t needed = code->size + extra;
    size_t capacity = code->capacity ? code->capacity : 128;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            PyErr_NoMemory();
            return -1;
        }
        capacity *= 2;
    }

    unsigned char *data = PyMem_RawRealloc(code->data, capacity);
    if (data == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    code->data = data;
    code->capacity = capacity;
    return 0;
}

static int code_bytes(XIRCode *code, const unsigned char *data, size_t size)
{
    if (code_reserve(code, size) < 0) return -1;
    memcpy(code->data + code->size, data, size);
    code->size += size;
    return 0;
}

static int code_byte(XIRCode *code, unsigned char value)
{
    return code_bytes(code, &value, 1);
}

static int code_u64(XIRCode *code, uint64_t value)
{
    unsigned char bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (unsigned char)(value >> (i * 8));
    return code_bytes(code, bytes, sizeof(bytes));
}

static int emit_mov_rax_imm64(XIRCode *code, uint64_t value)
{
    static const unsigned char op[] = {0x48, 0xB8};
    return code_bytes(code, op, sizeof(op)) || code_u64(code, value);
}

static int emit_mov_rcx_imm64(XIRCode *code, uint64_t value)
{
    static const unsigned char op[] = {0x48, 0xB9};
    return code_bytes(code, op, sizeof(op)) || code_u64(code, value);
}

static int emit_add(XIRCode *code)
{
    static const unsigned char op[] = {0x48, 0x01, 0xC8};
    return code_bytes(code, op, sizeof(op));
}

static int emit_sub(XIRCode *code)
{
    static const unsigned char op[] = {0x48, 0x29, 0xC8};
    return code_bytes(code, op, sizeof(op));
}

static int emit_mul(XIRCode *code)
{
    static const unsigned char op[] = {0x48, 0x0F, 0xAF, 0xC1};
    return code_bytes(code, op, sizeof(op));
}

static int emit_result_to_pyobject(XIRCode *code)
{
#if defined(_WIN32)
    static const unsigned char move_arg[] = {0x48, 0x89, 0xC1};
    static const unsigned char frame[] = {0x48, 0x83, 0xEC, 0x28};
    static const unsigned char unframe[] = {0x48, 0x83, 0xC4, 0x28};
#else
    static const unsigned char move_arg[] = {0x48, 0x89, 0xC7};
    static const unsigned char frame[] = {0x48, 0x83, 0xEC, 0x08};
    static const unsigned char unframe[] = {0x48, 0x83, 0xC4, 0x08};
#endif
    static const unsigned char call_rax[] = {0xFF, 0xD0};

    if (code_bytes(code, move_arg, sizeof(move_arg)) < 0 ||
        code_bytes(code, frame, sizeof(frame)) < 0 ||
        emit_mov_rax_imm64(code, (uint64_t)(uintptr_t)&PyLong_FromLong) < 0 ||
        code_bytes(code, call_rax, sizeof(call_rax)) < 0 ||
        code_bytes(code, unframe, sizeof(unframe)) < 0 ||
        code_byte(code, 0xC3) < 0) {
        return -1;
    }
    return 0;
}

/*
 * The IR emitter evaluates the IR tree into RAX.  It deliberately does not
 * inspect Python AST nodes: the native backend now has a clean IR boundary.
 *
 * This is still a bootstrap integer backend. Python's full object semantics
 * will be represented by later IR operations rather than by changing the
 * Python frontend or translating Python into another source language.
 */
static int emit_node(XIRCode *code, const PyXIRNode *node)
{
    if (node == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "PythonX native IR contains a null node");
        return -1;
    }

    if (node->op == PYX_IR_CONST_INT) {
        return emit_mov_rax_imm64(code, (uint64_t)node->value);
    }

    if (node->left == NULL || node->right == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "PythonX native IR operation has missing operands");
        return -1;
    }

    int64_t left;
    int64_t right;
    if (node->left->op != PYX_IR_CONST_INT || node->right->op != PYX_IR_CONST_INT) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "PythonX native IR bootstrap requires constant operands");
        return -1;
    }
    left = node->left->value;
    right = node->right->value;

    if (emit_mov_rax_imm64(code, (uint64_t)left) < 0 ||
        emit_mov_rcx_imm64(code, (uint64_t)right) < 0) {
        return -1;
    }

    switch (node->op) {
        case PYX_IR_ADD: return emit_add(code);
        case PYX_IR_SUB: return emit_sub(code);
        case PYX_IR_MUL: return emit_mul(code);
        default:
            PyErr_Format(PyExc_NotImplementedError,
                         "PythonX native IR operation %d is not implemented",
                         (int)node->op);
            return -1;
    }
}

static void *alloc_executable(size_t size)
{
#if defined(_WIN32)
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? NULL : memory;
#endif
}

static void free_executable(void *memory, size_t size)
{
#if defined(_WIN32)
    (void)size;
    if (memory != NULL) VirtualFree(memory, 0, MEM_RELEASE);
#else
    if (memory != NULL) munmap(memory, size);
#endif
}

static void native_ir_capsule_destructor(PyObject *capsule)
{
    XIRNativeCode *native = PyCapsule_GetPointer(capsule, "PythonX.native_ir_code");
    if (native == NULL) {
        PyErr_Clear();
        return;
    }
    free_executable(native->code, native->size);
    PyMem_RawFree(native);
}

PyObject *_PyX_NativeCompileIR(const PyXIRFunction *function)
{
    if (function == NULL || function->root == NULL) {
        PyErr_SetString(PyExc_TypeError, "PythonX native IR compiler requires a function");
        return NULL;
    }

    XIRCode code = {0};
    if (emit_node(&code, function->root) < 0 || emit_result_to_pyobject(&code) < 0) {
        PyMem_RawFree(code.data);
        return NULL;
    }

    void *memory = alloc_executable(code.size);
    if (memory == NULL) {
        PyMem_RawFree(code.data);
        PyErr_SetString(PyExc_MemoryError, "PythonX could not allocate executable memory");
        return NULL;
    }
    memcpy(memory, code.data, code.size);
    size_t size = code.size;
    PyMem_RawFree(code.data);

    XIRNativeCode *native = PyMem_RawMalloc(sizeof(*native));
    if (native == NULL) {
        free_executable(memory, size);
        PyErr_NoMemory();
        return NULL;
    }
    native->code = memory;
    native->size = size;

    PyObject *capsule = PyCapsule_New(native, "PythonX.native_ir_code",
                                      native_ir_capsule_destructor);
    if (capsule == NULL) {
        free_executable(memory, size);
        PyMem_RawFree(native);
        return NULL;
    }
    return capsule;
}

PyObject *_PyX_NativeExecuteIR(PyObject *native_code)
{
    if (!PyCapsule_IsValid(native_code, "PythonX.native_ir_code")) {
        PyErr_SetString(PyExc_TypeError, "invalid PythonX native IR code");
        return NULL;
    }
    XIRNativeCode *native = PyCapsule_GetPointer(native_code, "PythonX.native_ir_code");
    if (native == NULL) return NULL;
    if (native->code == NULL || native->size == 0) {
        PyErr_SetString(PyExc_RuntimeError, "PythonX native IR code is empty");
        return NULL;
    }
    return ((XIRNativeFunction)native->code)();
}

#else

PyObject *_PyX_NativeCompileIR(const PyXIRFunction *function)
{
    (void)function;
    PyErr_SetString(PyExc_NotImplementedError,
                    "PythonX native IR bootstrap currently targets x86-64");
    return NULL;
}

PyObject *_PyX_NativeExecuteIR(PyObject *native_code)
{
    (void)native_code;
    PyErr_SetString(PyExc_NotImplementedError,
                    "PythonX native IR bootstrap execution currently targets x86-64");
    return NULL;
}

#endif
