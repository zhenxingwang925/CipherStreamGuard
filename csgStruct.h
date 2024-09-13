#ifndef __CSG_STRUCT_H__
#define __CSG_STRUCT_H__

#include <fltKernel.h>
#include <dontuse.h>
#include <suppress.h>

/*************************************************************************
    Local structures
*************************************************************************/

//
//  This is a volume context, one of these are attached to each volume
//  we monitor.  This is used to get a "DOS" name for debug display.
//

typedef struct _VOLUME_CONTEXT {

    //
    //  Holds the name to display
    //

    UNICODE_STRING Name;

    //
    //  Holds the sector size for this volume.
    //

    ULONG SectorSize;

} VOLUME_CONTEXT, *PVOLUME_CONTEXT;

//
//  This is a context structure that is used to pass state from our
//  pre-operation callback to our post-operation callback.
//

typedef struct _PRE_2_POST_CONTEXT {

    //
    //  Pointer to our volume context structure.  We always get the context
    //  in the preOperation path because you can not safely get it at DPC
    //  level.  We then release it in the postOperation path.  It is safe
    //  to release contexts at DPC level.
    //

    PVOLUME_CONTEXT VolCtx;

    //
    //  Since the post-operation parameters always receive the "original"
    //  parameters passed to the operation, we need to pass our new destination
    //  buffer to our post operation routine so we can free it.
    //

    PVOID SwappedBuffer;

} PRE_2_POST_CONTEXT, *PPRE_2_POST_CONTEXT;

typedef struct _CSG_GLOBAL_DATA {

    //
    //  logging flags
    //

    ULONG DebugFlags;
    
} CSG_GLOBAL_DATA, *PCSG_GLOBAL_DATA;

extern CSG_GLOBAL_DATA g_Global;

/*************************************************************************
    Debug tracing information
*************************************************************************/

//
//  Definitions to display log messages.  The registry DWORD entry:
//  "hklm\system\CurrentControlSet\Services\Swapbuffers\DebugFlags" defines
//  the default state of these logging flags
//

#define LOGFL_ERRORS    0x00000001  // if set, display error messages
#define LOGFL_READ      0x00000002  // if set, display READ operation info
#define LOGFL_WRITE     0x00000004  // if set, display WRITE operation info
#define LOGFL_DIRCTRL   0x00000008  // if set, display DIRCTRL operation info
#define LOGFL_VOLCTX    0x00000010  // if set, display VOLCTX operation info

#define csg_print_form "[csg] [%d:%d] [%s:%u]: ", PsGetCurrentProcessId(), PsGetCurrentThreadId(), __FUNCTION__, __LINE__

#define LOG_PRINT( _logFlag, _msg )                            \
    do {                                                       \
        if (FlagOn(g_Global.DebugFlags, (_logFlag))) {         \
            DbgPrint(csg_print_form);                          \
            DbgPrint _msg;                                     \
        }                                                      \
    } while(0)

#endif // __CSG_STRUCT_H__
