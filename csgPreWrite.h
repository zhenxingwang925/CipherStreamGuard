#ifndef __CSG_PRE_WRITE_H__
#define __CSG_PRE_WRITE_H__

#include "csgGlobal.h"

FLT_PREOP_CALLBACK_STATUS
csgPreWriteBuffers(
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __deref_out_opt PVOID *CompletionContext
    );

FLT_POSTOP_CALLBACK_STATUS
csgPostWriteBuffers(
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in PVOID CompletionContext,
    __in FLT_POST_OPERATION_FLAGS Flags
    );




#endif // __CSG_PRE_WRITE_H__