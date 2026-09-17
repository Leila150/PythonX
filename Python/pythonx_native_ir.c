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
typedef struct { unsigned char *data; size_t size; size_t capacity; } XIRCode;
typedef struct { unsigned char *code; size_t size; PyXIRNode *root; PyObject *globals; } XIRNativeCode;
typedef PyObject *(*XIRNativeFunction)(void);
static int code_reserve(XIRCode *c,size_t extra){if(extra<=c->capacity-c->size)return 0;size_t need=c->size+extra,cap=c->capacity?c->capacity:64;while(cap<need){if(cap>SIZE_MAX/2){PyErr_NoMemory();return -1;}cap*=2;}unsigned char *p=PyMem_RawRealloc(c->data,cap);if(!p){PyErr_NoMemory();return -1;}c->data=p;c->capacity=cap;return 0;}
static int code_bytes(XIRCode *c,const unsigned char *d,size_t n){if(code_reserve(c,n)<0)return -1;memcpy(c->data+c->size,d,n);c->size+=n;return 0;}
static int code_u64(XIRCode *c,uint64_t v){unsigned char b[8];for(int i=0;i<8;i++)b[i]=(unsigned char)(v>>(i*8));return code_bytes(c,b,8);}
static int emit_mov_arg_imm64(XIRCode *c,uint64_t v,int arg){
#if defined(_WIN32)
 static const unsigned char rcx[]={0x48,0xB9},rdx[]={0x48,0xBA}; const unsigned char *op=arg==1?rcx:rdx;
#else
 static const unsigned char rdi[]={0x48,0xBF},rsi[]={0x48,0xBE}; const unsigned char *op=arg==1?rdi:rsi;
#endif
 return code_bytes(c,op,2)||code_u64(c,v);
}
static int emit_call_imm64(XIRCode *c,uint64_t a){static const unsigned char mov[]={0x48,0xB8},call[]={0xFF,0xD0},ret[]={0xC3};return code_bytes(c,mov,2)||code_u64(c,a)||code_bytes(c,call,2)||code_bytes(c,ret,1);}
static void *alloc_executable(size_t n){
#if defined(_WIN32)
 return VirtualAlloc(NULL,n,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
#else
 void *p=mmap(NULL,n,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);return p==MAP_FAILED?NULL:p;
#endif
}
static void free_executable(void *p,size_t n){
#if defined(_WIN32)
 (void)n;if(p)VirtualFree(p,0,MEM_RELEASE);
#else
 if(p)munmap(p,n);
#endif
}
static void native_ir_capsule_destructor(PyObject *capsule){XIRNativeCode *n=PyCapsule_GetPointer(capsule,"PythonX.native_ir_code");if(!n){PyErr_Clear();return;}free_executable(n->code,n->size);PyXIRFunction f={n->root,n->globals};_PyX_IR_Free(&f);PyMem_RawFree(n);}
PyObject *_PyX_NativeCompileIR(const PyXIRFunction *function){
 if(!function||!function->root){PyErr_SetString(PyExc_TypeError,"PythonX native IR compiler requires a function");return NULL;}
 XIRCode code={0};PyObject *globals=function->globals?Py_NewRef(function->globals):PyDict_New();if(!globals)return NULL;
 if(emit_mov_arg_imm64(&code,(uint64_t)(uintptr_t)function->root,1)<0||emit_mov_arg_imm64(&code,(uint64_t)(uintptr_t)globals,2)<0||emit_call_imm64(&code,(uint64_t)(uintptr_t)&_PyX_IR_Evaluate)<0){Py_DECREF(globals);PyMem_RawFree(code.data);return NULL;}
 void *memory=alloc_executable(code.size);if(!memory){Py_DECREF(globals);PyMem_RawFree(code.data);PyErr_SetString(PyExc_MemoryError,"PythonX could not allocate executable memory");return NULL;}memcpy(memory,code.data,code.size);PyMem_RawFree(code.data);
 XIRNativeCode *native=PyMem_RawMalloc(sizeof(*native));if(!native){free_executable(memory,code.size);Py_DECREF(globals);PyErr_NoMemory();return NULL;}
 native->code=memory;native->size=code.size;native->root=function->root;native->globals=globals;((PyXIRFunction *)function)->root=NULL;((PyXIRFunction *)function)->globals=NULL;
 PyObject *capsule=PyCapsule_New(native,"PythonX.native_ir_code",native_ir_capsule_destructor);if(!capsule){PyXIRFunction owned={native->root,native->globals};_PyX_IR_Free(&owned);free_executable(memory,code.size);PyMem_RawFree(native);return NULL;}return capsule;
}
PyObject *_PyX_NativeExecuteIR(PyObject *native_code){if(!PyCapsule_IsValid(native_code,"PythonX.native_ir_code")){PyErr_SetString(PyExc_TypeError,"invalid PythonX native IR code");return NULL;}XIRNativeCode *n=PyCapsule_GetPointer(native_code,"PythonX.native_ir_code");if(!n||!n->code||!n->size||!n->globals){PyErr_SetString(PyExc_RuntimeError,"PythonX native IR code is empty");return NULL;}return ((XIRNativeFunction)n->code)();}
#else
PyObject *_PyX_NativeCompileIR(const PyXIRFunction *function){(void)function;PyErr_SetString(PyExc_NotImplementedError,"PythonX native backend currently targets x86-64");return NULL;}
PyObject *_PyX_NativeExecuteIR(PyObject *native_code){(void)native_code;PyErr_SetString(PyExc_NotImplementedError,"PythonX native backend currently targets x86-64");return NULL;}
#endif
