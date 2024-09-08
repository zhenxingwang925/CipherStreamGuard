#include "csgWrite.h"

extern NPAGED_LOOKASIDE_LIST Pre2PostContextList;

FLT_PREOP_CALLBACK_STATUS
csgPreWriteBuffers(
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __deref_out_opt PVOID *CompletionContext
    )
/*++

Routine Description:

    This routine demonstrates how to swap buffers for the WRITE operation.

    Note that it handles all errors by simply not doing the buffer swap.

Arguments:

    Data - Pointer to the filter callbackData that is passed to us.

    FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
        opaque handles to this filter, instance, its associated volume and
        file object.

    CompletionContext - Receives the context that will be passed to the
        post-operation callback.

Return Value:

    FLT_PREOP_SUCCESS_WITH_CALLBACK - we want a postOpeation callback
    FLT_PREOP_SUCCESS_NO_CALLBACK - we don't want a postOperation callback
    FLT_PREOP_COMPLETE -
--*/
{
    PFLT_IO_PARAMETER_BLOCK iopb = Data->Iopb;
    FLT_PREOP_CALLBACK_STATUS retValue = FLT_PREOP_SUCCESS_NO_CALLBACK;
    PVOID newBuf = NULL;
    PMDL newMdl = NULL;
    PVOLUME_CONTEXT volCtx = NULL;
    PPRE_2_POST_CONTEXT p2pCtx;
    PVOID origBuf;
    NTSTATUS status;
    ULONG writeLen = iopb->Parameters.Write.Length;

    try {

        //
        //  If they are trying to write ZERO bytes, then don't do anything and
        //  we don't need a post-operation callback.
        //

        if (writeLen == 0) {

            leave;
        }

        //
        //  Get our volume context so we can display our volume name in the
        //  debug output.
        //

        status = FltGetVolumeContext( FltObjects->Filter,
                                      FltObjects->Volume,
                                      &volCtx );

        if (!NT_SUCCESS(status)) {

            LOG_PRINT( LOGFL_ERRORS,
                       ("csg!csgPreWriteBuffers:            Error getting volume context, status=%x\n",
                        status) );

            leave;
        }

        //
        //  If this is a non-cached I/O we need to round the length up to the
        //  sector size for this device.  We must do this because the file
        //  systems do this and we need to make sure our buffer is as big
        //  as they are expecting.
        //

        if (FlagOn(IRP_NOCACHE,iopb->IrpFlags)) {

            writeLen = (ULONG)ROUND_TO_SIZE(writeLen,volCtx->SectorSize);
        }

        //
        //  Allocate nonPaged memory for the buffer we are swapping to.
        //  If we fail to get the memory, just don't swap buffers on this
        //  operation.
        //

        newBuf = ExAllocatePoolWithTag( NonPagedPool,
                                        writeLen,
                                        BUFFER_SWAP_TAG );

        if (newBuf == NULL) {

            LOG_PRINT( LOGFL_ERRORS,
                       ("csg!csgPreWriteBuffers:            %wZ Failed to allocate %d bytes of memory.\n",
                        &volCtx->Name,
                        writeLen) );

            leave;
        }

        //
        //  We only need to build a MDL for IRP operations.  We don't need to
        //  do this for a FASTIO operation because it is a waste of time since
        //  the FASTIO interface has no parameter for passing the MDL to the
        //  file system.
        //

        if (FlagOn(Data->Flags,FLTFL_CALLBACK_DATA_IRP_OPERATION)) {

            //
            //  Allocate a MDL for the new allocated memory.  If we fail
            //  the MDL allocation then we won't swap buffer for this operation
            //

            newMdl = IoAllocateMdl( newBuf,
                                    writeLen,
                                    FALSE,
                                    FALSE,
                                    NULL );

            if (newMdl == NULL) {

                LOG_PRINT( LOGFL_ERRORS,
                           ("csg!csgPreWriteBuffers:            %wZ Failed to allocate MDL.\n",
                            &volCtx->Name) );

                leave;
            }

            //
            //  setup the MDL for the non-paged pool we just allocated
            //

            MmBuildMdlForNonPagedPool( newMdl );
        }

        //
        //  If the users original buffer had a MDL, get a system address.
        //

        if (iopb->Parameters.Write.MdlAddress != NULL) {

            origBuf = MmGetSystemAddressForMdlSafe( iopb->Parameters.Write.MdlAddress,
                                                    NormalPagePriority );

            if (origBuf == NULL) {

                LOG_PRINT( LOGFL_ERRORS,
                           ("csg!csgPreWriteBuffers:            %wZ Failed to get system address for MDL: %p\n",
                            &volCtx->Name,
                            iopb->Parameters.Write.MdlAddress) );

                //
                //  If we could not get a system address for the users buffer,
                //  then we are going to fail this operation.
                //

                Data->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                Data->IoStatus.Information = 0;
                retValue = FLT_PREOP_COMPLETE;
                leave;
            }

        } else {

            //
            //  There was no MDL defined, use the given buffer address.
            //

            origBuf = iopb->Parameters.Write.WriteBuffer;
        }

        //
        //  Copy the memory, we must do this inside the try/except because we
        //  may be using a users buffer address
        //

        try {

            RtlCopyMemory( newBuf,
                           origBuf,
                           writeLen );

        } except (EXCEPTION_EXECUTE_HANDLER) {

            //
            //  The copy failed, return an error, failing the operation.
            //

            Data->IoStatus.Status = GetExceptionCode();
            Data->IoStatus.Information = 0;
            retValue = FLT_PREOP_COMPLETE;

            LOG_PRINT( LOGFL_ERRORS,
                       ("csg!csgPreWriteBuffers:            %wZ Invalid user buffer, oldB=%p, status=%x\n",
                        &volCtx->Name,
                        origBuf,
                        Data->IoStatus.Status) );

            leave;
        }

        //
        //  We are ready to swap buffers, get a pre2Post context structure.
        //  We need it to pass the volume context and the allocate memory
        //  buffer to the post operation callback.
        //

        p2pCtx = ExAllocateFromNPagedLookasideList( &Pre2PostContextList );

        if (p2pCtx == NULL) {

            LOG_PRINT( LOGFL_ERRORS,
                       ("csg!csgPreWriteBuffers:            %wZ Failed to allocate pre2Post context structure\n",
                        &volCtx->Name) );

            leave;
        }

        //
        //  Set new buffers
        //

        LOG_PRINT( LOGFL_WRITE,
                   ("csg!csgPreWriteBuffers:            %wZ newB=%p newMdl=%p oldB=%p oldMdl=%p len=%d\n",
                    &volCtx->Name,
                    newBuf,
                    newMdl,
                    iopb->Parameters.Write.WriteBuffer,
                    iopb->Parameters.Write.MdlAddress,
                    writeLen) );

        iopb->Parameters.Write.WriteBuffer = newBuf;
        iopb->Parameters.Write.MdlAddress = newMdl;
        FltSetCallbackDataDirty( Data );

        //
        //  Pass state to our post-operation callback.
        //

        p2pCtx->SwappedBuffer = newBuf;
        p2pCtx->VolCtx = volCtx;

        *CompletionContext = p2pCtx;

        //
        //  Return we want a post-operation callback
        //

        retValue = FLT_PREOP_SUCCESS_WITH_CALLBACK;

    } finally {

        //
        //  If we don't want a post-operation callback, then free the buffer
        //  or MDL if it was allocated.
        //

        if (retValue != FLT_PREOP_SUCCESS_WITH_CALLBACK) {

            if (newBuf != NULL) {

                ExFreePool( newBuf );
            }

            if (newMdl != NULL) {

                IoFreeMdl( newMdl );
            }

            if (volCtx != NULL) {

                FltReleaseContext( volCtx );
            }
        }
    }

    return retValue;
}


FLT_POSTOP_CALLBACK_STATUS
csgPostWriteBuffers(
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in PVOID CompletionContext,
    __in FLT_POST_OPERATION_FLAGS Flags
    )
/*++

Routine Description:


Arguments:


Return Value:

--*/
{
    PPRE_2_POST_CONTEXT p2pCtx = CompletionContext;

    UNREFERENCED_PARAMETER( FltObjects );
    UNREFERENCED_PARAMETER( Flags );

    LOG_PRINT( LOGFL_WRITE,
               ("csg!csgPostWriteBuffers:           %wZ newB=%p info=%d Freeing\n",
                &p2pCtx->VolCtx->Name,
                p2pCtx->SwappedBuffer,
                Data->IoStatus.Information) );

    //
    //  Free allocate POOL and volume context
    //

    ExFreePool( p2pCtx->SwappedBuffer );
    FltReleaseContext( p2pCtx->VolCtx );

    ExFreeToNPagedLookasideList( &Pre2PostContextList,
                                 p2pCtx );

    return FLT_POSTOP_FINISHED_PROCESSING;
}