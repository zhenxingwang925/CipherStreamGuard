/*++

Copyright (c) 1999 - 2002  Microsoft Corporation

Module Name:

    csg.c

Abstract:

    This is a sample filter which demonstrates proper access of data buffer
    and a general guideline of how to swap buffers.
    For now it only swaps buffers for:

    IRP_MJ_READ
    IRP_MJ_WRITE
    IRP_MJ_DIRECTORY_CONTROL

    By default this filter attaches to all volumes it is notified about.  It
    does support having multiple instances on a given volume.

Environment:

    Kernel mode

--*/

#include "csgGlobal.h"
#include "csgDirCtrl.h"
#include "csgRead.h"
#include "csgWrite.h"

#pragma prefast(disable:__WARNING_ENCODE_MEMBER_FUNCTION_POINTER, "Not valid for kernel mode drivers")

PFLT_FILTER gFilterHandle;

ULONG LoggingFlags = 0;             // all disabled by default

#define MIN_SECTOR_SIZE 0x200

//
//  This is a lookAside list used to allocate our pre-2-post structure.
//

NPAGED_LOOKASIDE_LIST Pre2PostContextList;

CSG_GLOBAL_DATA g_Global;

/*************************************************************************
    Prototypes
*************************************************************************/

NTSTATUS
InstanceSetup (
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in FLT_INSTANCE_SETUP_FLAGS Flags,
    __in DEVICE_TYPE VolumeDeviceType,
    __in FLT_FILESYSTEM_TYPE VolumeFilesystemType
    );

VOID
CleanupVolumeContext(
    __in PFLT_CONTEXT Context,
    __in FLT_CONTEXT_TYPE ContextType
    );

NTSTATUS
InstanceQueryTeardown (
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
    );

DRIVER_INITIALIZE DriverEntry;
NTSTATUS
DriverEntry (
    __in PDRIVER_OBJECT DriverObject,
    __in PUNICODE_STRING RegistryPath
    );

NTSTATUS
FilterUnload (
    __in FLT_FILTER_UNLOAD_FLAGS Flags
    );

VOID
ReadDriverParameters (
    __in PUNICODE_STRING RegistryPath
    );

//
//  Assign text sections for each routine.
//

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, InstanceSetup)
#pragma alloc_text(PAGE, CleanupVolumeContext)
#pragma alloc_text(PAGE, InstanceQueryTeardown)
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(INIT, ReadDriverParameters)
#pragma alloc_text(PAGE, FilterUnload)
#endif

//
//  Operation we currently care about.
//

CONST FLT_OPERATION_REGISTRATION Callbacks[] = {
    { IRP_MJ_READ,
      0,
      csgPreReadBuffers,
      csgPostReadBuffers },

    { IRP_MJ_WRITE,
      0,
      csgPreWriteBuffers,
      csgPostWriteBuffers },

    { IRP_MJ_DIRECTORY_CONTROL,
      0,
      csgPreDirCtrlBuffers,
      csgPostDirCtrlBuffers },

    { IRP_MJ_OPERATION_END }
};

//
//  Context definitions we currently care about.  Note that the system will
//  create a lookAside list for the volume context because an explicit size
//  of the context is specified.
//

CONST FLT_CONTEXT_REGISTRATION ContextNotifications[] = {

     { FLT_VOLUME_CONTEXT,
       0,
       CleanupVolumeContext,
       sizeof(VOLUME_CONTEXT),
       CONTEXT_TAG },

     { FLT_CONTEXT_END }
};

//
//  This defines what we want to filter with FltMgr
//

CONST FLT_REGISTRATION FilterRegistration = {

    sizeof( FLT_REGISTRATION ),         //  Size
    FLT_REGISTRATION_VERSION,           //  Version
    0,                                  //  Flags

    ContextNotifications,               //  Context
    Callbacks,                          //  Operation callbacks

    FilterUnload,                       //  MiniFilterUnload

    InstanceSetup,                      //  InstanceSetup
    InstanceQueryTeardown,              //  InstanceQueryTeardown
    NULL,                               //  InstanceTeardownStart
    NULL,                               //  InstanceTeardownComplete

    NULL,                               //  GenerateFileName
    NULL,                               //  GenerateDestinationFileName
    NULL                                //  NormalizeNameComponent

};



//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
//
//                      Routines
//
//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////


NTSTATUS
InstanceSetup (
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in FLT_INSTANCE_SETUP_FLAGS Flags,
    __in DEVICE_TYPE VolumeDeviceType,
    __in FLT_FILESYSTEM_TYPE VolumeFilesystemType
    )
/*++

Routine Description:

    This routine is called whenever a new instance is created on a volume.

    By default we want to attach to all volumes.  This routine will try and
    get a "DOS" name for the given volume.  If it can't, it will try and
    get the "NT" name for the volume (which is what happens on network
    volumes).  If a name is retrieved a volume context will be created with
    that name.

Arguments:

    FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
        opaque handles to this filter, instance and its associated volume.

    Flags - Flags describing the reason for this attach request.

Return Value:

    STATUS_SUCCESS - attach
    STATUS_FLT_DO_NOT_ATTACH - do not attach

--*/
{
    PDEVICE_OBJECT devObj = NULL;
    PVOLUME_CONTEXT ctx = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG retLen;
    PUNICODE_STRING workingName;
    USHORT size;
    UCHAR volPropBuffer[sizeof(FLT_VOLUME_PROPERTIES)+512];
    PFLT_VOLUME_PROPERTIES volProp = (PFLT_VOLUME_PROPERTIES)volPropBuffer;

    PAGED_CODE();

    UNREFERENCED_PARAMETER( Flags );
    UNREFERENCED_PARAMETER( VolumeDeviceType );
    UNREFERENCED_PARAMETER( VolumeFilesystemType );

    try {

        //
        //  Allocate a volume context structure.
        //

        status = FltAllocateContext( FltObjects->Filter,
                                     FLT_VOLUME_CONTEXT,
                                     sizeof(VOLUME_CONTEXT),
                                     NonPagedPool,
                                     &ctx );

        if (!NT_SUCCESS(status)) {

            //
            //  We could not allocate a context, quit now
            //

            leave;
        }

        //
        //  Always get the volume properties, so I can get a sector size
        //

        status = FltGetVolumeProperties( FltObjects->Volume,
                                         volProp,
                                         sizeof(volPropBuffer),
                                         &retLen );

        if (!NT_SUCCESS(status)) {

            leave;
        }

        //
        //  Save the sector size in the context for later use.  Note that
        //  we will pick a minimum sector size if a sector size is not
        //  specified.
        //

        ASSERT((volProp->SectorSize == 0) || (volProp->SectorSize >= MIN_SECTOR_SIZE));

        ctx->SectorSize = max(volProp->SectorSize,MIN_SECTOR_SIZE);

        //
        //  Init the buffer field (which may be allocated later).
        //

        ctx->Name.Buffer = NULL;

        //
        //  Get the storage device object we want a name for.
        //

        status = FltGetDiskDeviceObject( FltObjects->Volume, &devObj );

        if (NT_SUCCESS(status)) {

            //
            //  Try and get the DOS name.  If it succeeds we will have
            //  an allocated name buffer.  If not, it will be NULL
            //

#pragma prefast(suppress:__WARNING_USE_OTHER_FUNCTION, "Used to maintain compatability with Win 2k")
            status = RtlVolumeDeviceToDosName( devObj, &ctx->Name );
        }

        //
        //  If we could not get a DOS name, get the NT name.
        //

        if (!NT_SUCCESS(status)) {

            ASSERT(ctx->Name.Buffer == NULL);

            //
            //  Figure out which name to use from the properties
            //

            if (volProp->RealDeviceName.Length > 0) {

                workingName = &volProp->RealDeviceName;

            } else if (volProp->FileSystemDeviceName.Length > 0) {

                workingName = &volProp->FileSystemDeviceName;

            } else {

                //
                //  No name, don't save the context
                //

                status = STATUS_FLT_DO_NOT_ATTACH;
                leave;
            }

            //
            //  Get size of buffer to allocate.  This is the length of the
            //  string plus room for a trailing colon.
            //

            size = workingName->Length + sizeof(WCHAR);

            //
            //  Now allocate a buffer to hold this name
            //

#pragma prefast(suppress:__WARNING_MEMORY_LEAK, "ctx->Name.Buffer will not be leaked because it is freed in CleanupVolumeContext")
            ctx->Name.Buffer = ExAllocatePoolWithTag( NonPagedPool,
                                                      size,
                                                      NAME_TAG );
            if (ctx->Name.Buffer == NULL) {

                status = STATUS_INSUFFICIENT_RESOURCES;
                leave;
            }

            //
            //  Init the rest of the fields
            //

            ctx->Name.Length = 0;
            ctx->Name.MaximumLength = size;

            //
            //  Copy the name in
            //

            RtlCopyUnicodeString( &ctx->Name,
                                  workingName );

            //
            //  Put a trailing colon to make the display look good
            //

            RtlAppendUnicodeToString( &ctx->Name,
                                      L":" );
        }

        //
        //  Set the context
        //

        status = FltSetVolumeContext( FltObjects->Volume,
                                      FLT_SET_CONTEXT_KEEP_IF_EXISTS,
                                      ctx,
                                      NULL );

        //
        //  Log debug info
        //

        LOG_PRINT( LOGFL_VOLCTX,
                   ("csg!InstanceSetup:                  Real SectSize=0x%04x, Used SectSize=0x%04x, Name=\"%wZ\"\n",
                    volProp->SectorSize,
                    ctx->SectorSize,
                    &ctx->Name) );

        //
        //  It is OK for the context to already be defined.
        //

        if (status == STATUS_FLT_CONTEXT_ALREADY_DEFINED) {

            status = STATUS_SUCCESS;
        }

    } finally {

        //
        //  Always release the context.  If the set failed, it will free the
        //  context.  If not, it will remove the reference added by the set.
        //  Note that the name buffer in the ctx will get freed by the context
        //  cleanup routine.
        //

        if (ctx) {

            FltReleaseContext( ctx );
        }

        //
        //  Remove the reference added to the device object by
        //  FltGetDiskDeviceObject.
        //

        if (devObj) {

            ObDereferenceObject( devObj );
        }
    }

    return status;
}


VOID
CleanupVolumeContext(
    __in PFLT_CONTEXT Context,
    __in FLT_CONTEXT_TYPE ContextType
    )
/*++

Routine Description:

    The given context is being freed.
    Free the allocated name buffer if there one.

Arguments:

    Context - The context being freed

    ContextType - The type of context this is

Return Value:

    None

--*/
{
    PVOLUME_CONTEXT ctx = Context;

    PAGED_CODE();

    UNREFERENCED_PARAMETER( ContextType );

    ASSERT(ContextType == FLT_VOLUME_CONTEXT);

    if (ctx->Name.Buffer != NULL) {

        ExFreePool(ctx->Name.Buffer);
        ctx->Name.Buffer = NULL;
    }
}


NTSTATUS
InstanceQueryTeardown (
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
    )
/*++

Routine Description:

    This is called when an instance is being manually deleted by a
    call to FltDetachVolume or FilterDetach.  We always return it is OK to
    detach.

Arguments:

    FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
        opaque handles to this filter, instance and its associated volume.

    Flags - Indicating where this detach request came from.

Return Value:

    Always succeed.

--*/
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER( FltObjects );
    UNREFERENCED_PARAMETER( Flags );

    return STATUS_SUCCESS;
}


/*************************************************************************
    Initialization and unload routines.
*************************************************************************/

NTSTATUS
DriverEntry (
    __in PDRIVER_OBJECT DriverObject,
    __in PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;

    ReadDriverParameters( RegistryPath );

    ExInitializeNPagedLookasideList( &Pre2PostContextList,
                                     NULL,
                                     NULL,
                                     0,
                                     sizeof(PRE_2_POST_CONTEXT),
                                     PRE_2_POST_TAG,
                                     0 );

    status = FltRegisterFilter( DriverObject,
                                &FilterRegistration,
                                &gFilterHandle );

    if (! NT_SUCCESS( status )) {

        goto SwapDriverEntryExit;
    }

    status = FltStartFiltering( gFilterHandle );

    if (! NT_SUCCESS( status )) {

        FltUnregisterFilter( gFilterHandle );
        goto SwapDriverEntryExit;
    }

    LOG_PRINT(LOGFL_ERRORS, ("DriverEntry start ok!\n"));

SwapDriverEntryExit:

    if(! NT_SUCCESS( status )) {

        ExDeleteNPagedLookasideList( &Pre2PostContextList );
    }

    return status;
}


NTSTATUS
FilterUnload (
    __in FLT_FILTER_UNLOAD_FLAGS Flags
    )
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER( Flags );

    FltUnregisterFilter( gFilterHandle );

    ExDeleteNPagedLookasideList( &Pre2PostContextList );

    LOG_PRINT(LOGFL_ERRORS, ("DriverEntry unload ok!\n"));

    return STATUS_SUCCESS;
}

// TODO: Abstract registry functions, All operations require a corresponding func
VOID
ReadDriverParameters (
      __in PUNICODE_STRING RegistryPath
      )
{
    OBJECT_ATTRIBUTES attributes;
    HANDLE driverRegKey;
    NTSTATUS status;
    ULONG resultLength;
    UNICODE_STRING valueName;
    UCHAR buffer[sizeof( KEY_VALUE_PARTIAL_INFORMATION ) + sizeof( LONG )];

    g_Global.DebugFlags = LOGFL_ERRORS | LOGFL_READ | LOGFL_WRITE | LOGFL_DIRCTRL | LOGFL_VOLCTX;    // open all

    InitializeObjectAttributes( &attributes,
                RegistryPath,
                OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                NULL,
                NULL );

    status = ZwOpenKey( &driverRegKey,
            KEY_READ,
            &attributes );

    if (!NT_SUCCESS( status )) {

        LOG_PRINT(LOGFL_ERRORS, ("ZwOpenKey Error Code: 0x%x\n", status));

        goto ERROR;
    }

    RtlInitUnicodeString( &valueName, L"DebugFlags" );

    status = ZwQueryValueKey( driverRegKey,
                &valueName,
                KeyValuePartialInformation,
                buffer,
                sizeof(buffer),
                &resultLength );

    if (NT_SUCCESS( status )) {

        g_Global.DebugFlags = *((PULONG) &(((PKEY_VALUE_PARTIAL_INFORMATION)buffer)->Data));
    }

ERROR:
    if (driverRegKey)
        ZwClose(driverRegKey);
    
    LOG_PRINT(LOGFL_ERRORS, ("Current DebugFlags : 0x%x\n", g_Global.DebugFlags));
}
