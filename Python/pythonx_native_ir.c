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
    PyXIRNode *root;
} XIRNativeCode;

typedef PyObject *(*XIRNativeFunction)(void);

static int code_reserve(XIRCode *code, size_t extra)
{
    if (extra <= code->capacity - code->size) return 0;
    size_t needed = code->size + extra;
    size_t capacity = code->capacity ? code->capacity : 64;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) { PyErr_NoMemory(); return -1; }
        capacity *= 2;
    }
    unsigned char *data = PyMem_RawRealloc(code->data, capacity);
    if (!data) { PyErr_NoMemory(); return -1; }
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

static int code_u64(XIRCode *code, uint64_t value)
{
    unsigned char bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (unsigned char)(value >> (i * 8));
    return code_bytes(code, bytes, sizeof(bytes));
}

static int emit_mov_arg_imm64(XIRCode *code, uint64_t value)
{
#if defined(_WIN32)
    static const unsigned char op[] = {0x48, 0xB9}; /* mov rcx, imm64 */
#else
    static const unsigned char op[] = {0x48, 0xBF}; /* mov rdi, imm64 */
#endif
    return code_bytes(code, op, sizeof(op)) || code_u64(code, value);
}

static int emit_call_imm64(XIRCode *code, uint64_t address)
{
    static const unsigned char mov_rax[] = {0x48, 0xB8};
    static const unsigned char call_rax[] = {0xFF, 0xD0};
    return code_bytes(code, mov_rax, sizeof(mov_rax)) ||
           code_u64(code, address) ||
           code_bytes(code, call_rax, sizeof(call_rax)) ||
           code_bytes(code, (const unsigned char[]) {0xC3}, 1);
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
    if (memory) VirtualFree(memory, 0, MEM_RELEASE);
#else
    if (memory) munmap(memory, size);
#endif
}

static void native_ir_capsule_destructor(PyObject *capsule)
{
    XIRNativeCode *native = PyCapsule_GetPointer(capsule, "PythonX.native_ir_code");
    if (!native) { PyErr_Clear(); return; }
    free_executable(native->code, native->size);
    PyXIRFunction function = {native->root};
    _PyX_IR_Free(&function);
    PyMem_RawFree(native);
}

PyObject *_PyX_NativeCompileIR(const PyXIRFunction *function)
{
    if (!function || !function->root) {
        PyErr_SetString(PyExc_TypeError, "PythonX native IR compiler requires a function");
        return NULL;
    }

    XIRCode code = {0};
    if (emit_mov_arg_imm64(&code, (uint64_t)(uintptr_t)function->root) < 0 ||
        emit_call_imm64(&code, (uint64_t)(uintptr_t)&_PyX_IR_Evaluate) < 0) {
        PyMem_RawFree(code.data);
        return NULL;
    }

    void *memory = alloc_executable(code.size);
    if (!memory) {
        PyMem_RawFree(code.data);
        PyErr_SetString(PyExc_MemoryError, "PythonX could not allocate executable memory");
        return NULL;
    }
    memcpy(memory, code.data, code.size);
    PyMem_RawFree(code.data);

    XIRNativeCode *native = PyMem_RawMalloc(sizeof(*native));
    if (!native) {
        free_executable(memory, code.size);
        PyErr_NoMemory();
        return NULL;
    }
    native->code = memory;
    native->size = code.size;
    native->root = function->root;

    /* Ownership transfers from the caller's IR function to the native capsule. */
    ((PyXIRFunction *)function)->root = NULL;

    PyObject *capsule = PyCapsule_New(native, "PythonX.native_ir_code",
                                      native_ir_capsule_destructor);
    if (!capsule) {
        PyXIRFunction owned = {native->root};
        _PyX_IR_Free(&owned);
        free_executable(memory, code.size);
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
    if (!native || !native->code || !native->size) {
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
                    "PythonX native backend currently targets x86-64");
    return NULL;
}

PyObject *_PyX_NativeExecuteIR(PyObject *native_code)
{
    (void)native_code;
    PyErr_SetString(PyExc_NotImplementedError,
                    "PythonX native backend currently targets x86-64");
    return NULL;
}

#endif
