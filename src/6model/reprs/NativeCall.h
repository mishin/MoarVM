#include "dyncall.h"
#include "dynload.h"
#include "dyncall_callback.h"

/* Body of a NativeCall. */
struct MVMNativeCallBody {
    char       *lib_name;
    DLLib      *lib_handle;
    void       *entry_point;
    MVMuint16   convention;
    MVMuint16   num_args;
    MVMuint16  *arg_types;
    MVMuint16   ret_type;
    MVMObject **arg_info;
};

/* This is how an instance with the NativeCall representation looks. */
struct MVMNativeCall {
    MVMObject common;
    MVMNativeCallBody body;
};

/* Initializes the NativeCall REPR. */
MVMREPROps * MVMNativeCall_initialize(MVMThreadContext *tc,
        MVMObject * (* wrap_object_func_ptr) (MVMThreadContext *tc, void *obj),
        MVMObject * (* create_stable_func_ptr) (MVMThreadContext *tc, MVMREPROps *REPR, MVMObject *HOW));