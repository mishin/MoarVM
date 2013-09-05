BEGIN_OPS_PREAMBLE

#include "parrot/parrot.h"
#include "parrot/extend.h"
#include "parrot/dynext.h"
#include "../6model/sixmodelobject.h"
#include "../6model/reprs/NativeCall.h"
#include "../6model/reprs/CStruct.h"
#include "../6model/reprs/CPointer.h"
#include "../6model/reprs/CArray.h"
#include "../6model/reprs/CStr.h"
#include "../6model/reprs/dyncall_reprs.h"

/* This library contains just three operations: one to initialize it,
 * one to look up a native function and build a handle to it, and
 * another to actually make the call.
 *
 * It uses hashes to describe arguments and return types. The following
 * keys and values are allowable.
 *
 * type
 *   Any of the following strings:
 *     void
 *     char
 *     short
 *     int
 *     long
 *     longlong
 *     float
 *     double
 *     asciistr
 *     utf8str
 *     utf16str
 *     cpointer
 *     cstruct
 *     carray
 *     callback
 *
 * free_str
 *   Controls whether strings that are passed get freed or not. Zero to not
 *   free, non-zero to free. The default is to free.
 *
 * callback_args
 *   nqp::list(...) of nqp::hash(...) describing the arguments for the callback.
 * 
 * XXX Probably more callback stuff to figure out here...
 */

/* Our various argument types. */
#define DYNCALL_ARG_VOID            0
#define DYNCALL_ARG_CHAR            2
#define DYNCALL_ARG_SHORT           4
#define DYNCALL_ARG_INT             6
#define DYNCALL_ARG_LONG            8
#define DYNCALL_ARG_LONGLONG        10
#define DYNCALL_ARG_FLOAT           12
#define DYNCALL_ARG_DOUBLE          14
#define DYNCALL_ARG_ASCIISTR        16
#define DYNCALL_ARG_UTF8STR         18
#define DYNCALL_ARG_UTF16STR        20
#define DYNCALL_ARG_CSTRUCT         22
#define DYNCALL_ARG_CARRAY          24
#define DYNCALL_ARG_CALLBACK        26
#define DYNCALL_ARG_CPOINTER        28
#define DYNCALL_ARG_TYPE_MASK       30

/* Flag for whether we should free a string after passing it or not. */
#define DYNCALL_ARG_NO_FREE_STR     0
#define DYNCALL_ARG_FREE_STR        1
#define DYNCALL_ARG_FREE_STR_MASK   1

typedef struct {
    PMC **types;
    INTVAL *typeinfos;
    INTVAL length;
    PARROT_INTERP;
    PMC *sub;
    DCCallback *cb;
} CallbackData;

/* Predeclare some mutually recursive functions. */
static void dyncall_wb_ca(PARROT_INTERP, PMC *);
static void dyncall_wb_cs(PARROT_INTERP, PMC *);

static char callback_handler(DCCallback *cb, DCArgs *args, DCValue *result, CallbackData *data);

/* The ID of the NativeCall, CPointer and CStruct REPRs. */
static INTVAL nc_repr_id = 0;
static INTVAL cs_repr_id = 0;
static INTVAL cp_repr_id = 0;
static INTVAL ca_repr_id = 0;
static INTVAL cstr_repr_id = 0;
static INTVAL smo_id = 0;

PMC *callback_cache = NULL;

INTVAL get_nc_repr_id(void) { return nc_repr_id; }
INTVAL get_cs_repr_id(void) { return cs_repr_id; }
INTVAL get_cp_repr_id(void) { return cp_repr_id; }
INTVAL get_ca_repr_id(void) { return ca_repr_id; }

/* Grabs a NativeCall body. */
static NativeCallBody * get_nc_body(PARROT_INTERP, PMC *obj) {
    struct SixModel_REPROps *r = REPR(obj);
    if (r->ID == nc_repr_id)
        return &((NativeCallInstance *)PMC_data(obj))->body;
    else
        return (NativeCallBody *)r->box_funcs->get_boxed_ref(interp, STABLE(obj),
            OBJECT_BODY(obj), nc_repr_id);
}

/* Gets the flag for whether to free a string after a call or not. */
static INTVAL
get_str_free_flag(PARROT_INTERP, PMC *info) {
    STRING *flag = Parrot_str_new_constant(interp, "free_str");
    if (VTABLE_exists_keyed_str(interp, info, flag))
        if (!VTABLE_get_integer_keyed_str(interp, info, flag))
            return DYNCALL_ARG_NO_FREE_STR;
    return DYNCALL_ARG_FREE_STR;
}

/* Takes a hash describing a type hands back an argument type code. */
static INTVAL
get_arg_type(PARROT_INTERP, PMC *info, INTVAL is_return) {
    STRING *type_name = VTABLE_get_string_keyed_str(interp, info,
        Parrot_str_new_constant(interp, "type"));
    if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "void"))) {
        if (is_return)
            return DYNCALL_ARG_VOID;
        Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
            "Cannot use 'void' type except for on native call return values");
    }
    else if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "char"))) {
        return DYNCALL_ARG_CHAR;
    }
    else if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "short"))) {
        return DYNCALL_ARG_SHORT;
    }
    else if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "int"))) {
        return DYNCALL_ARG_INT;
    }
    else if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "long"))) {
        return DYNCALL_ARG_LONG;
    }
    else if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "longlong"))) {
        return DYNCALL_ARG_LONGLONG;
    }
    else if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "float"))) {
        return DYNCALL_ARG_FLOAT;
    }
    else if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "double"))) {
        return DYNCALL_ARG_DOUBLE;
    }
    else if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "asciistr"))) {
        return DYNCALL_ARG_ASCIISTR | get_str_free_flag(interp, info);
    }
    else if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "utf8str"))) {
        return DYNCALL_ARG_UTF8STR | get_str_free_flag(interp, info);
    }
    else if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "utf16str"))) {
        return DYNCALL_ARG_UTF16STR | get_str_free_flag(interp, info);
    }
    else if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "cstruct"))) {
        return DYNCALL_ARG_CSTRUCT;
    }
    else if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "cpointer"))) {
        return DYNCALL_ARG_CPOINTER;
    }
    else if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "carray"))) {
        return DYNCALL_ARG_CARRAY;
    }
    else if (Parrot_str_equal(interp, type_name, Parrot_str_new_constant(interp, "callback"))) {
        return DYNCALL_ARG_CALLBACK;
    }
    else {
        Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
            "Unknown type '%Ss' used for native call", type_name);
    }
}

/* Maps a calling convention name to an ID. */
static INTVAL
get_calling_convention(PARROT_INTERP, STRING *name) {
    if (STRING_IS_NULL(name)) {
        return DC_CALL_C_DEFAULT;
    }
    else if (Parrot_str_equal(interp, name, Parrot_str_new_constant(interp, ""))) {
        return DC_CALL_C_DEFAULT;
    }
    else if (Parrot_str_equal(interp, name, Parrot_str_new_constant(interp, "cdecl"))) {
        return DC_CALL_C_X86_CDECL;
    }
    else if (Parrot_str_equal(interp, name, Parrot_str_new_constant(interp, "stdcall"))) {
        return DC_CALL_C_X86_WIN32_STD;
    }
    else if (Parrot_str_equal(interp, name, Parrot_str_new_constant(interp, "win64"))) {
        return DC_CALL_C_X64_WIN64;
    }
    else {
        Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
            "Unknown calling convention '%Ss' used for native call", name);
    }
}

/* Map argument type id to dyncall character id. */
static char
get_signature_char(INTVAL type_id) {
    switch (type_id & DYNCALL_ARG_TYPE_MASK) {
        case DYNCALL_ARG_VOID:
            return 'v';
        case DYNCALL_ARG_CHAR:
            return 'c';
        case DYNCALL_ARG_SHORT:
            return 's';
        case DYNCALL_ARG_INT:
            return 'i';
        case DYNCALL_ARG_LONG:
            return 'j';
        case DYNCALL_ARG_LONGLONG:
            return 'l';
        case DYNCALL_ARG_FLOAT:
            return 'f';
        case DYNCALL_ARG_DOUBLE:
            return 'd';
        case DYNCALL_ARG_ASCIISTR:
        case DYNCALL_ARG_UTF8STR:
        case DYNCALL_ARG_UTF16STR:
        case DYNCALL_ARG_CSTRUCT:
        case DYNCALL_ARG_CPOINTER:
        case DYNCALL_ARG_CARRAY:
        case DYNCALL_ARG_CALLBACK:
            return 'p';
        default:
            return '\0';
    }
}

/* Constructs a boxed result from a native integer return. */
PMC *
make_int_result(PARROT_INTERP, PMC *type, INTVAL value) {
    PMC *result = PMCNULL;
    if (!PMC_IS_NULL(type)) {
        result = REPR(type)->allocate(interp, STABLE(type));
        REPR(result)->initialize(interp, STABLE(result), OBJECT_BODY(result));
        REPR(result)->box_funcs->set_int(interp, STABLE(result), OBJECT_BODY(result), value);
    }
    return result;
}

/* Constructs a boxed result from a native number return. */
PMC *
make_num_result(PARROT_INTERP, PMC *type, FLOATVAL value) {
    PMC *result = PMCNULL;
    if (!PMC_IS_NULL(type)) {
        result = REPR(type)->allocate(interp, STABLE(type));
        REPR(result)->initialize(interp, STABLE(result), OBJECT_BODY(result));
        REPR(result)->box_funcs->set_num(interp, STABLE(result), OBJECT_BODY(result), value);
    }
    return result;
}

/* Constructs a boxed result from a string return. */
PMC *
make_str_result(PARROT_INTERP, PMC *type, INTVAL ret_type, char *cstring) {
    PMC *result = type;
    if (cstring != NULL && !PMC_IS_NULL(type)) {
        STRING *value = STRINGNULL;
        switch (ret_type & DYNCALL_ARG_TYPE_MASK) {
            case DYNCALL_ARG_ASCIISTR:
                value = Parrot_str_new_init(interp, cstring, strlen(cstring), Parrot_ascii_encoding_ptr, 0);
                break;
            case DYNCALL_ARG_UTF8STR:
                value = Parrot_str_new_init(interp, cstring, strlen(cstring), Parrot_utf8_encoding_ptr, 0);
                break;
            case DYNCALL_ARG_UTF16STR:
                value = Parrot_str_new_init(interp, cstring, strlen(cstring), Parrot_utf16_encoding_ptr, 0);
                break;
            default:
                Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION, "Internal error: unhandled encoding");
        }
        result = REPR(type)->allocate(interp, STABLE(type));
        REPR(result)->initialize(interp, STABLE(result), OBJECT_BODY(result));
        REPR(result)->box_funcs->set_str(interp, STABLE(result), OBJECT_BODY(result), value);
        PARROT_GC_WRITE_BARRIER(interp, result);
        if (ret_type & DYNCALL_ARG_FREE_STR)
            free(cstring);
    }
    return result;
}

/* Constructs a boxed result using a CStruct REPR type. */
PMC *
make_cstruct_result(PARROT_INTERP, PMC *type, void *cstruct) {
    PMC *result = type;
    if (cstruct != NULL && !PMC_IS_NULL(type)) {
        if (REPR(type)->ID != cs_repr_id)
            Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
                "Native call expected return type with CStruct representation, but got something else");
        result = REPR(type)->allocate(interp, STABLE(type));
        ((CStructInstance *)PMC_data(result))->body.cstruct = cstruct;
    }
    return result;
}

/* Constructs a boxed result using a CPointer REPR type. */
PMC *
make_cpointer_result(PARROT_INTERP, PMC *type, void *ptr) {
    PMC *result = type;
    if (ptr != NULL && !PMC_IS_NULL(type)) {
        if (REPR(type)->ID != cp_repr_id)
            Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
                "Native call expected return type with CPointer representation, but got something else");
        result = REPR(type)->allocate(interp, STABLE(type));
        ((CPointerInstance *)PMC_data(result))->body.ptr = ptr;
    }
    return result;
}

/* Constructs a boxed result using a CArray REPR type. */
PMC *
make_carray_result(PARROT_INTERP, PMC *type, void *carray) {
    PMC *result = type;
    if (carray != NULL && !PMC_IS_NULL(type)) {
        if (REPR(type)->ID != ca_repr_id)
            Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
                "Native call expected return type with CArray representation, but got something else");
        result = REPR(type)->allocate(interp, STABLE(type));
        ((CArrayInstance *)PMC_data(result))->body.storage = carray;
    }
    return result;
}

static DCchar
unmarshal_char(PARROT_INTERP, PMC *value) {
    if (value->vtable->base_type != smo_id)
        return (DCchar) VTABLE_get_integer(interp, value);
    return (DCchar)REPR(value)->box_funcs->get_int(interp, STABLE(value), OBJECT_BODY(value));
}

static DCshort
unmarshal_short(PARROT_INTERP, PMC *value) {
    if (value->vtable->base_type != smo_id)
        return (DCshort) VTABLE_get_integer(interp, value);
    return (DCshort)REPR(value)->box_funcs->get_int(interp, STABLE(value), OBJECT_BODY(value));
}

static DCint
unmarshal_int(PARROT_INTERP, PMC *value) {
    if (value->vtable->base_type != smo_id)
        return (DCint) VTABLE_get_integer(interp, value);
    return (DCint)REPR(value)->box_funcs->get_int(interp, STABLE(value), OBJECT_BODY(value));
}

static DClong
unmarshal_long(PARROT_INTERP, PMC *value) {
    if (value->vtable->base_type != smo_id)
        return (DClong) VTABLE_get_integer(interp, value);
    return (DClong)REPR(value)->box_funcs->get_int(interp, STABLE(value), OBJECT_BODY(value));
}

static DClonglong
unmarshal_longlong(PARROT_INTERP, PMC *value) {
    if (value->vtable->base_type != smo_id)
        return (DClonglong) VTABLE_get_integer(interp, value);
    return (DClonglong)REPR(value)->box_funcs->get_int(interp, STABLE(value), OBJECT_BODY(value));
}

static DCfloat
unmarshal_float(PARROT_INTERP, PMC *value) {
    if (value->vtable->base_type != smo_id)
        return (DCfloat) VTABLE_get_number(interp, value);
    return (DCfloat)REPR(value)->box_funcs->get_num(interp, STABLE(value), OBJECT_BODY(value));
}

static DCdouble
unmarshal_double(PARROT_INTERP, PMC *value) {
    if (value->vtable->base_type != smo_id)
        return (DCdouble) VTABLE_get_number(interp, value);
    return (DCdouble)REPR(value)->box_funcs->get_num(interp, STABLE(value), OBJECT_BODY(value));
}

static char *
unmarshal_string(PARROT_INTERP, PMC *value, INTVAL type, INTVAL *free) {
    if (value->vtable->base_type != smo_id)
        return Parrot_str_to_encoded_cstring(interp, VTABLE_get_string(interp, value), Parrot_utf8_encoding_ptr);

    if (IS_CONCRETE(value)) {
        char *str;
        PMC *meth = VTABLE_find_method(interp, STABLE(value)->WHAT,
            Parrot_str_new_constant(interp, "cstr"));

        /* Initial assumption: string shouldn't be freed. */
        if(free)
            *free = 0;

        if (!PMC_IS_NULL(meth)) {
            PMC *obj;
            PMC *old_ctx = Parrot_pcc_get_signature(interp, CURRENT_CONTEXT(interp));
            PMC *cappy = Parrot_pmc_new(interp, enum_class_CallContext);

            VTABLE_push_pmc(interp, cappy, value);
            Parrot_pcc_invoke_from_sig_object(interp, meth, cappy);
            cappy = Parrot_pcc_get_signature(interp, CURRENT_CONTEXT(interp));
            Parrot_pcc_set_signature(interp, CURRENT_CONTEXT(interp), old_ctx);
            obj = decontainerize(interp, VTABLE_get_pmc_keyed_int(interp, cappy, 0));

            str = ((CStrInstance *)PMC_data(obj))->body.cstr;
        }
        else {
            str = Parrot_str_to_encoded_cstring(interp,
                REPR(value)->box_funcs->get_str(interp, STABLE(value), OBJECT_BODY(value)),
                type & DYNCALL_ARG_TYPE_MASK == DYNCALL_ARG_ASCIISTR ? Parrot_ascii_encoding_ptr :
                type & DYNCALL_ARG_TYPE_MASK == DYNCALL_ARG_UTF16STR ? Parrot_utf16_encoding_ptr :
                                                            Parrot_utf8_encoding_ptr);

            if (free && type & DYNCALL_ARG_FREE_STR_MASK) {
                *free = 1;
            }
        }
        return str;
    }
    else {
        return NULL;
    }
    return NULL;
}

static void *
unmarshal_cstruct(PARROT_INTERP, PMC *value) {
    if (value->vtable->base_type != smo_id)
        Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
            "Can't unmarshal non-sixmodel PMC to struct");

    if (!IS_CONCRETE(value))
        return NULL;
    else if (REPR(value)->ID == cs_repr_id)
        return ((CStructInstance *)PMC_data(value))->body.cstruct;
    else
        Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
            "Native call expected object with CStruct representation, but got something else");
}

static void *
unmarshal_carray(PARROT_INTERP, PMC *value) {
    if (value->vtable->base_type != smo_id)
        Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
            "Can't unmarshal non-sixmodel PMC to array");

    if (!IS_CONCRETE(value))
        return NULL;
    else if (REPR(value)->ID == ca_repr_id)
        return ((CArrayInstance *)PMC_data(value))->body.storage;
    else
        Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
            "Native call expected object with CArray representation, but got something else");
}

static void *
unmarshal_cpointer(PARROT_INTERP, PMC *value) {
    if (value->vtable->base_type != smo_id)
        Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
            "Can't unmarshal non-sixmodel PMC to opaque pointer");

    if (!IS_CONCRETE(value))
        return NULL;
    else if (REPR(value)->ID == cp_repr_id)
        return ((CPointerInstance *)PMC_data(value))->body.ptr;
    else
        Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
            "Native call expected object with CPointer representation, but got something else");
}

static void *
unmarshal_callback(PARROT_INTERP, PMC *value, PMC *info) {
    PMC *callback_data;

    if (value->vtable->base_type != smo_id)
        Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
            "Can't unmarshal non-sixmodel PMC to callback");


    if (!IS_CONCRETE(value)) {
        return NULL;
    }

    if(!callback_cache) {
        callback_cache = Parrot_pmc_new(interp, enum_class_Hash);
        Parrot_pmc_gc_register(interp, callback_cache);
    }

    callback_data = VTABLE_get_pmc_keyed(interp, callback_cache, value);

    if(!PMC_IS_NULL(callback_data)) {
        CallbackData *data = (CallbackData *) VTABLE_get_pointer(interp, callback_data);
        return data->cb;
    }
    else {
        /* TODO: Make sure it's a Callable? */
        char *signature;
        CallbackData *data;
        PMC *typehash;
        PMC *ptrpmc;
        INTVAL num_info = VTABLE_elements(interp, info);
        INTVAL i;

        data = (CallbackData *) mem_sys_allocate(sizeof(CallbackData));
        data->typeinfos = (INTVAL *) mem_sys_allocate(num_info);
        data->types = (PMC **) mem_sys_allocate(num_info*sizeof(PMC *));
        /* A dyncall signature looks like this: xxx)x
        * Argument types before the ) and return type after it. Thus,
        * num_info+1 must be NULL (zero-terminated string) and num_info-1
        * must be the ).
        */
        signature = (char *) mem_sys_allocate(num_info + 2);
        signature[num_info+1] = '\0';
        signature[num_info-1] = ')';
        typehash = VTABLE_get_pmc_keyed_int(interp, info, 0);
        data->types[0] = VTABLE_get_pmc_keyed_str(interp, typehash,
                Parrot_str_new_constant(interp, "typeobj"));
        data->typeinfos[0] = get_arg_type(interp, typehash, 1);
        signature[num_info] = get_signature_char(data->typeinfos[0]);
        for (i = 1; i < num_info; i++) {
            typehash = VTABLE_get_pmc_keyed_int(interp, info, i);
            data->types[i] = VTABLE_get_pmc_keyed_str(interp, typehash,
                    Parrot_str_new_constant(interp, "typeobj"));
            data->typeinfos[i] = get_arg_type(interp, typehash, 0);
            signature[i-1] = get_signature_char(data->typeinfos[i]);
        }

        data->length = num_info;
        data->interp = interp;
        data->sub = value;

        data->cb = dcbNewCallback(signature, &callback_handler, data);

        mem_sys_free(signature); /* XXX: Not entirely sure if I can do this... */

        /* Stash data in a Pointer PMC and chuck that in callback_cache. */
        ptrpmc = Parrot_pmc_new(interp, enum_class_Pointer);
        VTABLE_set_pointer(interp, ptrpmc, data);
        VTABLE_set_pmc_keyed(interp, callback_cache, value, ptrpmc);

        return data->cb;
    }
}

PMC * decontainerize(PARROT_INTERP, PMC *var) {
    if (var->vtable->base_type == smo_id)
        var = DECONT(interp, var);
    return var;
}

static void dyncall_wb_ca(PARROT_INTERP, PMC *obj) {
    CArrayBody      *body      = (CArrayBody *) OBJECT_BODY(obj);
    CArrayREPRData  *repr_data = (CArrayREPRData *) STABLE(obj)->REPR_data;
    void           **storage   = (void **) body->storage;
    INTVAL           i;

    /* No need to check for numbers. They're stored directly in the array. */
    if (repr_data->elem_kind == CARRAY_ELEM_KIND_NUMERIC)
        return;

    for (i = 0; i < body->elems; i++) {
        void *cptr;   /* The pointer in the C storage. */
        void *objptr; /* The pointer in the object representing the C object. */

        /* Ignore elements where we haven't generated an object. */
        if (!body->child_objs[i])
            continue;

        cptr = storage[i];
        if (IS_CONCRETE(body->child_objs[i])) {
            switch (repr_data->elem_kind) {
                case CARRAY_ELEM_KIND_CARRAY:
                    objptr = ((CArrayBody *) OBJECT_BODY(body->child_objs[i]))->storage;
                    break;
                case CARRAY_ELEM_KIND_CPOINTER:
                    objptr = ((CPointerBody *) OBJECT_BODY(body->child_objs[i]))->ptr;
                    break;
                case CARRAY_ELEM_KIND_CSTRUCT:
                    objptr = (CStructBody *) OBJECT_BODY(body->child_objs[i]);
                    break;
                case CARRAY_ELEM_KIND_STRING:
                    objptr = NULL; /* TODO */
                    break;
                default:
                    Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
                        "Fatal error: bad elem_kind (%d) in dyncall_wb_ca", repr_data->elem_kind);
            }
        }
        else {
            objptr = NULL;
        }

        if (objptr != cptr) {
            body->child_objs[i] = NULL;
        }
        else if (repr_data->elem_kind == CARRAY_ELEM_KIND_CARRAY) {
            dyncall_wb_ca(interp, body->child_objs[i]);
        }
        else if (repr_data->elem_kind == CARRAY_ELEM_KIND_CSTRUCT) {
            dyncall_wb_cs(interp, body->child_objs[i]);
        }
    }
}

static void dyncall_wb_cs(PARROT_INTERP, PMC *obj) {
    CStructBody     *body      = (CStructBody *) OBJECT_BODY(obj);
    CStructREPRData *repr_data = (CStructREPRData *) STABLE(obj)->REPR_data;
    char            *storage   = (char *) body->cstruct;
    INTVAL           i;

    for (i = 0; i < repr_data->num_attributes; i++) {
        INTVAL kind = repr_data->attribute_locations[i] & CSTRUCT_ATTR_MASK;
        INTVAL slot = repr_data->attribute_locations[i] >> CSTRUCT_ATTR_SHIFT;
        void *cptr;   /* The pointer in the C storage. */
        void *objptr; /* The pointer in the object representing the C object. */

        if (kind == CSTRUCT_ATTR_IN_STRUCT || !body->child_objs[slot])
            continue;

        cptr = *((void **) (storage + repr_data->struct_offsets[i]));
        if (IS_CONCRETE(body->child_objs[slot])) {
            switch (kind) {
                case CSTRUCT_ATTR_CARRAY:
                    objptr = ((CArrayBody *) OBJECT_BODY(body->child_objs[slot]))->storage;
                    break;
                case CSTRUCT_ATTR_CPTR:
                    objptr = ((CPointerBody *) OBJECT_BODY(body->child_objs[slot]))->ptr;
                    break;
                case CSTRUCT_ATTR_CSTRUCT:
                    objptr = (CStructBody *) OBJECT_BODY(body->child_objs[slot]);
                    break;
                case CSTRUCT_ATTR_STRING:
                    objptr = NULL;
                    break;
                default:
                    Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
                        "Fatal error: bad kind (%d) in dyncall_wb_cs", kind);
            }
        }
        else {
            objptr = NULL;
        }

        if (objptr != cptr) {
            body->child_objs[slot] = NULL;
        }
        else if (kind == CSTRUCT_ATTR_CARRAY) {
            dyncall_wb_ca(interp, body->child_objs[slot]);
        }
        else if (kind == CSTRUCT_ATTR_CSTRUCT) {
            dyncall_wb_cs(interp, body->child_objs[slot]);
        }
    }
}

/* Handle callback from C code. */
static char
callback_handler(DCCallback *cb, DCArgs *args, DCValue *result, CallbackData *data) {
    PMC *old_ctx = Parrot_pcc_get_signature(data->interp, CURRENT_CONTEXT(data->interp));
    PMC *cappy = Parrot_pmc_new(data->interp, enum_class_CallContext);
    PMC *retval;
    INTVAL i;

    /* Unpack args into Parrot data structures. */
    for (i = 1; i < data->length; i++) {
        PMC *value;
        /*PMC *type = decontainerize(data->interp, data->types[i]);*/
        PMC *type = data->types[i];
        INTVAL typeinfo = data->typeinfos[i];

        switch (typeinfo & DYNCALL_ARG_TYPE_MASK) {
            case DYNCALL_ARG_CHAR:
                value = make_int_result(data->interp, type, dcbArgChar(args));
                break;
            case DYNCALL_ARG_SHORT:
                value = make_int_result(data->interp, type, dcbArgShort(args));
                break;
            case DYNCALL_ARG_INT:
                value = make_int_result(data->interp, type, dcbArgInt(args));
                break;
            case DYNCALL_ARG_LONG:
                value = make_int_result(data->interp, type, dcbArgLong(args));
                break;
            case DYNCALL_ARG_LONGLONG:
                value = make_int_result(data->interp, type, dcbArgLongLong(args));
                break;
            case DYNCALL_ARG_FLOAT:
                value = make_num_result(data->interp, type, dcbArgFloat(args));
                break;
            case DYNCALL_ARG_DOUBLE:
                value = make_num_result(data->interp, type, dcbArgDouble(args));
                break;
            case DYNCALL_ARG_ASCIISTR:
            case DYNCALL_ARG_UTF8STR:
            case DYNCALL_ARG_UTF16STR:
                value = make_str_result(data->interp, type, typeinfo,
                    (char *)dcbArgPointer(args));
                break;
            case DYNCALL_ARG_CSTRUCT:
                value = make_cstruct_result(data->interp, type, dcbArgPointer(args));
                break;
            case DYNCALL_ARG_CPOINTER:
                value = make_cpointer_result(data->interp, type, dcbArgPointer(args));
                break;
            case DYNCALL_ARG_CARRAY:
                value = make_carray_result(data->interp, type, dcbArgPointer(args));
                break;
            case DYNCALL_ARG_CALLBACK:
                /* TODO: A callback -return- value means that we have a C method
                * that needs to be wrapped similarly to a is native(...) Perl 6
                * sub. */
                dcbArgPointer(args);
                value = type;
        default:
            Parrot_ex_throw_from_c_args(data->interp, NULL, EXCEPTION_INVALID_OPERATION, "Internal error: unhandled dyncall callback argument type");
        }

        VTABLE_push_pmc(data->interp, cappy, value);
    }

    /* Set up the Parrot call and invoke the sub. */
    Parrot_pcc_invoke_from_sig_object(data->interp, data->sub, cappy);
    cappy = Parrot_pcc_get_signature(data->interp, CURRENT_CONTEXT(data->interp));
    Parrot_pcc_set_signature(data->interp, CURRENT_CONTEXT(data->interp), old_ctx);
    retval = decontainerize(data->interp, VTABLE_get_pmc_keyed_int(data->interp, cappy, 0));

    /* Unpack the return value and set the appropriate field in result and
     * return signature char. */
    switch (data->typeinfos[0] & DYNCALL_ARG_TYPE_MASK) {
        case DYNCALL_ARG_VOID:
            break;
        case DYNCALL_ARG_CHAR:
            result->c = unmarshal_char(data->interp, retval);
            break;
        case DYNCALL_ARG_SHORT:
            result->s = unmarshal_short(data->interp, retval);
            break;
        case DYNCALL_ARG_INT:
            result->i = unmarshal_int(data->interp, retval);
            break;
        case DYNCALL_ARG_LONG:
            result->j = unmarshal_long(data->interp, retval);
            break;
        case DYNCALL_ARG_LONGLONG:
            result->l = unmarshal_longlong(data->interp, retval);
            break;
        case DYNCALL_ARG_FLOAT:
            result->f = unmarshal_float(data->interp, retval);
            break;
        case DYNCALL_ARG_DOUBLE:
            result->d = unmarshal_double(data->interp, retval);
            break;
        case DYNCALL_ARG_ASCIISTR:
        case DYNCALL_ARG_UTF8STR:
        case DYNCALL_ARG_UTF16STR:
            result->Z = unmarshal_string(data->interp, retval, data->typeinfos[0], NULL);
            break;
        case DYNCALL_ARG_CSTRUCT:
            result->p = unmarshal_cstruct(data->interp, retval);
            break;
        case DYNCALL_ARG_CPOINTER:
            result->p = unmarshal_cpointer(data->interp, retval);
            break;
        case DYNCALL_ARG_CARRAY:
            result->p = unmarshal_carray(data->interp, retval);
            break;
        case DYNCALL_ARG_CALLBACK:
            result->p = unmarshal_callback(data->interp, retval, data->types[0]);
            break;
        default:
            Parrot_ex_throw_from_c_args(data->interp, NULL, EXCEPTION_INVALID_OPERATION, "Internal error: unhandled dyncall callback return type");
    }

    return get_signature_char(data->typeinfos[0]);
}

END_OPS_PREAMBLE

/* Initialize the native call library. */
inline op nqp_native_call_setup() :base_core {
    if (!nc_repr_id)
        nc_repr_id = REGISTER_DYNAMIC_REPR(interp,
            Parrot_str_new_constant(interp, "NativeCall"),
            NativeCall_initialize);
    if (!cs_repr_id)
        cs_repr_id = REGISTER_DYNAMIC_REPR(interp,
            Parrot_str_new_constant(interp, "CStruct"),
            CStruct_initialize);
    if (!cp_repr_id)
        cp_repr_id = REGISTER_DYNAMIC_REPR(interp,
            Parrot_str_new_constant(interp, "CPointer"),
            CPointer_initialize);
    if (!ca_repr_id)
        ca_repr_id = REGISTER_DYNAMIC_REPR(interp,
            Parrot_str_new_constant(interp, "CArray"),
            CArray_initialize);
    if (!cstr_repr_id)
        cstr_repr_id = REGISTER_DYNAMIC_REPR(interp,
            Parrot_str_new_constant(interp, "CStr"),
            CStr_initialize);
    if (!smo_id)
        smo_id = Parrot_pmc_get_type_str(interp, Parrot_str_new(interp, "SixModelObject", 0));
}


/* Build a native call object.
 *
 * $1 is the object to store the call in. It should be of a type that is
 *    based on or boxes the NativeCall REPR, and should be an instance.
 * $2 is the name of the library to load the function from.
 * $3 is the name of the function to load.
 * $4 is a string name specifying the calling convention to use.
 * $5 is an nqp::list(...) of nqp::hash(...), one hash per argument.
      The entries in the hash describe the type of argument being passed.
 * $6 is an nqp::hash(...) that describes the expected return type
 *
 * There's no need to manually release the handle; when it is no longer
 * referenced, it will be automatically garbage collected.
 */
inline op nqp_native_call_build(invar PMC, in STR, in STR, in STR, invar PMC, invar PMC) :base_core {
    char *lib_name    = Parrot_str_to_cstring(interp, $2);
    char *sym_name    = Parrot_str_to_cstring(interp, $3);
    PMC  *arg_info    = $5;
    PMC  *ret_info    = $6;
    int   i;
    
    /* Initialize the object; grab native call part of its body. */
    NativeCallBody *body = get_nc_body(interp, $1);
    
    /* Try to load the library. */
    body->lib_name = lib_name;
    body->lib_handle = dlLoadLibrary(strlen(lib_name) ? lib_name : NULL);
    if (!body->lib_handle) {
        Parrot_str_free_cstring(sym_name);
        Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
            "Cannot locate native library '%Ss'", $2);
    }
    
    /* Try to locate the symbol. */
    body->entry_point = dlFindSymbol(body->lib_handle, sym_name);
    Parrot_str_free_cstring(sym_name);
    if (!body->entry_point) {
        Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION,
            "Cannot locate symbol '%Ss' in native library '%Ss'", $3, $2);
    }

    /* Set calling convention, if any. */
    body->convention = get_calling_convention(interp, $4);

    /* Transform each of the args info structures into a flag. */
    body->num_args  = VTABLE_elements(interp, arg_info);
    body->arg_types = (INTVAL *) mem_sys_allocate(sizeof(INTVAL) * (body->num_args ? body->num_args : 1));
    body->arg_info  = (PMC **) mem_sys_allocate(sizeof(PMC *) * (body->num_args ? body->num_args : 1));
    for (i = 0; i < body->num_args; i++) {
        PMC *info = VTABLE_get_pmc_keyed_int(interp, arg_info, i);
        body->arg_types[i] = get_arg_type(interp, info, 0);
        body->arg_info[i]  = NULL;

        if(body->arg_types[i] == DYNCALL_ARG_CALLBACK)
            body->arg_info[i] = VTABLE_get_pmc_keyed_str(interp, info, Parrot_str_new_constant(interp, "callback_args"));
    }

    /* If the function has any callback parameters we've added markables to
     * it, so we have to write barrier. */
    PARROT_GC_WRITE_BARRIER(interp, $1);

    /* Transform return argument type info a flag. */
    body->ret_type = get_arg_type(interp, ret_info, 1);
}

/* Makes a native call.
 *
 * $2 is the type of result to build. It can be a null if the return value
 *    is void or can simply be discarded. If the return value is a native
 *    type, then this type should be capable of boxing it.
 * $3 is an object representing the call, obtained from nqp_native_call_build.
 * $4 is an nqp::list(...), which contains the arguments to pass; note this
 *    means they are in boxed form
 *
 * $1 will be populated with an instance of $2 that contains the result
 * of the call. If $2 was null PMC, then $1 also will be. If the call
 * was to return a struct, array or some other pointer type and the
 * result comes back as NULL, then $1 will simply be $2 (which is
 * presumably a type object).
 */
inline op nqp_native_call(out PMC, invar PMC, invar PMC, invar PMC) :base_core {
    PMC *args        = $4;
    PMC *result      = PMCNULL;
    char **free_strs = NULL;
    INTVAL num_strs  = 0;
    INTVAL i;

    /* Get native call body, so we can locate the call info. */
    NativeCallBody *body = get_nc_body(interp, $3);
    
    /* Create and set up call VM. */
    DCCallVM *vm = dcNewCallVM(8192);
    dcMode(vm, body->convention);
    
    /* Process arguments. */
    for (i = 0; i < body->num_args; i++) {
        PMC *value = decontainerize(interp, VTABLE_get_pmc_keyed_int(interp, args, i));
        switch (body->arg_types[i] & DYNCALL_ARG_TYPE_MASK) {
            case DYNCALL_ARG_CHAR:
                dcArgChar(vm, unmarshal_char(interp, value));
                break;
            case DYNCALL_ARG_SHORT:
                dcArgShort(vm, unmarshal_short(interp, value));
                break;
            case DYNCALL_ARG_INT:
                dcArgInt(vm, unmarshal_int(interp, value));
                break;
            case DYNCALL_ARG_LONG:
                dcArgLong(vm, unmarshal_long(interp, value));
                break;
            case DYNCALL_ARG_LONGLONG:
                dcArgLongLong(vm, unmarshal_longlong(interp, value));
                break;
            case DYNCALL_ARG_FLOAT:
                dcArgFloat(vm, unmarshal_float(interp, value));
                break;
            case DYNCALL_ARG_DOUBLE:
                dcArgDouble(vm, unmarshal_double(interp, value));
                break;
            case DYNCALL_ARG_ASCIISTR:
            case DYNCALL_ARG_UTF8STR:
            case DYNCALL_ARG_UTF16STR:
                {
                    INTVAL free;
                    char *str = unmarshal_string(interp, value, body->arg_types[i], &free);
                    if(free) {
                        if (!free_strs)
                            free_strs = (char**) mem_sys_allocate(body->num_args * sizeof(char *));
                        free_strs[num_strs] = str;
                        num_strs++;
                    }
                    dcArgPointer(vm, str);
                }
                break;
            case DYNCALL_ARG_CSTRUCT:
                dcArgPointer(vm, unmarshal_cstruct(interp, value));
                break;
            case DYNCALL_ARG_CPOINTER:
                dcArgPointer(vm, unmarshal_cpointer(interp, value));
                break;
            case DYNCALL_ARG_CARRAY:
                dcArgPointer(vm, unmarshal_carray(interp, value));
                break;
            case DYNCALL_ARG_CALLBACK:
                dcArgPointer(vm, unmarshal_callback(interp, value, body->arg_info[i]));
                break;
            default:
                Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION, "Internal error: unhandled dyncall argument type");
        }
    }

    /* Call and process return values. */
    switch (body->ret_type & DYNCALL_ARG_TYPE_MASK) {
        case DYNCALL_ARG_VOID:
            dcCallVoid(vm, body->entry_point);
            result = $2;
            break;
        case DYNCALL_ARG_CHAR:
            result = make_int_result(interp, $2, dcCallChar(vm, body->entry_point));
            break;
        case DYNCALL_ARG_SHORT:
            result = make_int_result(interp, $2, dcCallShort(vm, body->entry_point));
            break;
        case DYNCALL_ARG_INT:
            result = make_int_result(interp, $2, dcCallInt(vm, body->entry_point));
            break;
        case DYNCALL_ARG_LONG:
            result = make_int_result(interp, $2, dcCallLong(vm, body->entry_point));
            break;
        case DYNCALL_ARG_LONGLONG:
            result = make_int_result(interp, $2, dcCallLongLong(vm, body->entry_point));
            break;
        case DYNCALL_ARG_FLOAT:
            result = make_num_result(interp, $2, dcCallFloat(vm, body->entry_point));
            break;
        case DYNCALL_ARG_DOUBLE:
            result = make_num_result(interp, $2, dcCallDouble(vm, body->entry_point));
            break;
        case DYNCALL_ARG_ASCIISTR:
        case DYNCALL_ARG_UTF8STR:
        case DYNCALL_ARG_UTF16STR:
            result = make_str_result(interp, $2, body->ret_type,
                (char *)dcCallPointer(vm, body->entry_point));
            break;
        case DYNCALL_ARG_CSTRUCT:
            result = make_cstruct_result(interp, $2, dcCallPointer(vm, body->entry_point));
            break;
        case DYNCALL_ARG_CPOINTER:
            result = make_cpointer_result(interp, $2, dcCallPointer(vm, body->entry_point));
            break;
        case DYNCALL_ARG_CARRAY:
            result = make_carray_result(interp, $2, dcCallPointer(vm, body->entry_point));
            break;
        case DYNCALL_ARG_CALLBACK:
            /* XXX Above are all still todo. */
            /* TODO: A callback -return- value means that we have a C method
             * that needs to be wrapped similarly to a is native(...) Perl 6
             * sub. */
            dcCallPointer(vm, body->entry_point);
            result = $2;
        default:
            Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_INVALID_OPERATION, "Internal error: unhandled dyncall return type");
    }

    for (i = 0; i < body->num_args; i++) {
        PMC *value = decontainerize(interp, VTABLE_get_pmc_keyed_int(interp, args, i));
        if(!IS_CONCRETE(value))
            continue;

        switch (body->arg_types[i]) {
            case DYNCALL_ARG_CARRAY:
                dyncall_wb_ca(interp, value);
                break;
            case DYNCALL_ARG_CSTRUCT:
                dyncall_wb_cs(interp, value);
                break;
            default: /* Noop to eliminate warning. */
                continue;
        }
    }
    
    /* Free any memory that we need to. */
    if (free_strs) {
        for (i = 0; i < num_strs; i++)
            Parrot_str_free_cstring(free_strs[i]);
        mem_sys_free(free_strs);
    }
    
    /* Finally, free call VM. */
    dcFree(vm);
    
    $1 = result;
}

/* Write-barrier a dyncall object so that delayed changes to the C-side of
 * objects are propagated to the HLL side. All CArray and CStruct arguments to
 * C functions are write-barriered automatically, so this should be necessary
 * only in the rarest of cases.
 *
 * $1 is the object to write barrier.
 */
inline op nqp_native_call_wb(invar PMC) :base_core {
    PMC *obj = decontainerize(interp, $1);

    if(REPR(obj)->ID == ca_repr_id) {
        dyncall_wb_ca(interp, obj);
    }
    else if(REPR(obj)->ID == cs_repr_id) {
        dyncall_wb_cs(interp, obj);
    }
}