#include "csgRead.h"
#include "csgGlobal.h"
#include "csgStruct.h"

extern NPAGED_LOOKASIDE_LIST Pre2PostContextList;

FLT_PREOP_CALLBACK_STATUS
csgPreReadBuffers(
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __deref_out_opt PVOID *CompletionContext
    )
/*++

Routine Description:

    This routine demonstrates how to swap buffers for the READ operation.

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

--*/
{
    PFLT_IO_PARAMETER_BLOCK iopb = Data->Iopb;
    FLT_PREOP_CALLBACK_STATUS retValue = FLT_PREOP_SUCCESS_NO_CALLBACK;
    PVOID newBuf = NULL;
    PMDL newMdl = NULL;
    PVOLUME_CONTEXT volCtx = NULL;
    PPRE_2_POST_CONTEXT p2pCtx;
    NTSTATUS status;
    ULONG readLen = iopb->Parameters.Read.Length;

    try {

        //
        //  If they are trying to read ZERO bytes, then don't do anything and
        //  we don't need a post-operation callback.
        //

        if (readLen == 0) {

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
                       ("csg!csgPreReadBuffers:             Error getting volume context, status=%x\n",
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

            readLen = (ULONG)ROUND_TO_SIZE(readLen,volCtx->SectorSize);
        }

        //
        //  Allocate nonPaged memory for the buffer we are swapping to.
        //  If we fail to get the memory, just don't swap buffers on this
        //  operation.
        //

        newBuf = ExAllocatePoolWithTag( NonPagedPool,
                                        readLen,
                                        BUFFER_SWAP_TAG );

        if (newBuf == NULL) {

            LOG_PRINT( LOGFL_ERRORS,
                       ("csg!csgPreReadBuffers:             %wZ Failed to allocate %d bytes of memory\n",
                        &volCtx->Name,
                        readLen) );

            leave;
        }

        //
        //  We only need to build a MDL for IRP operations.  We don't need to
        //  do this for a FASTIO operation since the FASTIO interface has no
        //  parameter for passing the MDL to the file system.
        //

        if (FlagOn(Data->Flags,FLTFL_CALLBACK_DATA_IRP_OPERATION)) {

            //
            //  Allocate a MDL for the new allocated memory.  If we fail
            //  the MDL allocation then we won't swap buffer for this operation
            //

            newMdl = IoAllocateMdl( newBuf,
                                    readLen,
                                    FALSE,
                                    FALSE,
                                    NULL );

            if (newMdl == NULL) {

                LOG_PRINT( LOGFL_ERRORS,
                           ("csg!csgPreReadBuffers:             %wZ Failed to allocate MDL\n",
                            &volCtx->Name) );

                leave;
            }

            //
            //  setup the MDL for the non-paged pool we just allocated
            //

            MmBuildMdlForNonPagedPool( newMdl );
        }

        //
        //  We are ready to swap buffers, get a pre2Post context structure.
        //  We need it to pass the volume context and the allocate memory
        //  buffer to the post operation callback.
        //

        p2pCtx = ExAllocateFromNPagedLookasideList( &Pre2PostContextList );

        if (p2pCtx == NULL) {

            LOG_PRINT( LOGFL_ERRORS,
                       ("csg!csgPreReadBuffers:             %wZ Failed to allocate pre2Post context structure\n",
                        &volCtx->Name) );

            leave;
        }

        //
        //  Log that we are swapping
        //

        LOG_PRINT( LOGFL_READ,
                   ("csg!csgPreReadBuffers:             %wZ newB=%p newMdl=%p oldB=%p oldMdl=%p len=%d\n",
                    &volCtx->Name,
                    newBuf,
                    newMdl,
                    iopb->Parameters.Read.ReadBuffer,
                    iopb->Parameters.Read.MdlAddress,
                    readLen) );

        //
        //  Update the buffer pointers and MDL address, mark we have changed
        //  something.
        //

        iopb->Parameters.Read.ReadBuffer = newBuf;
        iopb->Parameters.Read.MdlAddress = newMdl;
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
        //  If we don't want a post-operation callback, then cleanup state.
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
csgPostReadBuffers(
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in PVOID CompletionContext,
    __in FLT_POST_OPERATION_FLAGS Flags
    )
/*++

Routine Description:

    This routine does postRead buffer swap handling

Arguments:

    Data - Pointer to the filter callbackData that is passed to us.

    FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
        opaque handles to this filter, instance, its associated volume and
        file object.

    CompletionContext - The completion context set in the pre-operation routine.

    Flags - Denotes whether the completion is successful or is being drained.

Return Value:

    FLT_POSTOP_FINISHED_PROCESSING
    FLT_POSTOP_MORE_PROCESSING_REQUIRED

--*/
{
    PVOID origBuf;
    PFLT_IO_PARAMETER_BLOCK iopb = Data->Iopb;
    FLT_POSTOP_CALLBACK_STATUS retValue = FLT_POSTOP_FINISHED_PROCESSING;
    PPRE_2_POST_CONTEXT p2pCtx = CompletionContext;
    BOOLEAN cleanupAllocatedBuffer = TRUE;

    //
    //  This system won't draining an operation with swapped buffers, verify
    //  the draining flag is not set.
    //

    ASSERT(!FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING));

    try {

        //
        //  If the operation failed or the count is zero, there is no data to
        //  copy so just return now.
        //

        if (!NT_SUCCESS(Data->IoStatus.Status) ||
            (Data->IoStatus.Information == 0)) {

            LOG_PRINT( LOGFL_READ,
                       ("csg!csgPostReadBuffers:            %wZ newB=%p No data read, status=%x, info=%x\n",
                        &p2pCtx->VolCtx->Name,
                        p2pCtx->SwappedBuffer,
                        Data->IoStatus.Status,
                        Data->IoStatus.Information) );

            leave;
        }

        //
        //  We need to copy the read data back into the users buffer.  Note
        //  that the parameters passed in are for the users original buffers
        //  not our swapped buffers.
        //

        if (iopb->Parameters.Read.MdlAddress != NULL) {

            //
            //  There is a MDL defined for the original buffer, get a
            //  system address for it so we can copy the data back to it.
            //  We must do this because we don't know what thread context
            //  we are in.
            //

            origBuf = MmGetSystemAddressForMdlSafe( iopb->Parameters.Read.MdlAddress,
                                                    NormalPagePriority );

            if (origBuf == NULL) {

                LOG_PRINT( LOGFL_ERRORS,
                           ("csg!csgPostReadBuffers:            %wZ Failed to get system address for MDL: %p\n",
                            &p2pCtx->VolCtx->Name,
                            iopb->Parameters.Read.MdlAddress) );

                //
                //  If we failed to get a SYSTEM address, mark that the read
                //  failed and return.
                //

                Data->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                Data->IoStatus.Information = 0;
                leave;
            }

        } else if (FlagOn(Data->Flags,FLTFL_CALLBACK_DATA_SYSTEM_BUFFER) ||
                   FlagOn(Data->Flags,FLTFL_CALLBACK_DATA_FAST_IO_OPERATION)) {

            //
            //  If this is a system buffer, just use the given address because
            //      it is valid in all thread contexts.
            //  If this is a FASTIO operation, we can just use the
            //      buffer (inside a try/except) since we know we are in
            //      the correct thread context (you can't pend FASTIO's).
            //

            origBuf = iopb->Parameters.Read.ReadBuffer;

        } else {

            //
            //  They don't have a MDL and this is not a system buffer
            //  or a fastio so this is probably some arbitrary user
            //  buffer.  We can not do the processing at DPC level so
            //  try and get to a safe IRQL so we can do the processing.
            //

            if (FltDoCompletionProcessingWhenSafe( Data,
                                                   FltObjects,
                                                   CompletionContext,
                                                   Flags,
                                                   SwapPostReadBuffersWhenSafe,
                                                   &retValue )) {

                //
                //  This operation has been moved to a safe IRQL, the called
                //  routine will do (or has done) the freeing so don't do it
                //  in our routine.
                //

                cleanupAllocatedBuffer = FALSE;

            } else {

                //
                //  We are in a state where we can not get to a safe IRQL and
                //  we do not have a MDL.  There is nothing we can do to safely
                //  copy the data back to the users buffer, fail the operation
                //  and return.  This shouldn't ever happen because in those
                //  situations where it is not safe to post, we should have
                //  a MDL.
                //

                LOG_PRINT( LOGFL_ERRORS,
                           ("csg!csgPostReadBuffers:            %wZ Unable to post to a safe IRQL\n",
                            &p2pCtx->VolCtx->Name) );

                Data->IoStatus.Status = STATUS_UNSUCCESSFUL;
                Data->IoStatus.Information = 0;
            }

            leave;
        }

        //
        //  We either have a system buffer or this is a fastio operation
        //  so we are in the proper context.  Copy the data handling an
        //  exception.
        //

        try {

            RtlCopyMemory( origBuf,
                           p2pCtx->SwappedBuffer,
                           Data->IoStatus.Information );

        } except (EXCEPTION_EXECUTE_HANDLER) {

            //
            //  The copy failed, return an error, failing the operation.
            //

            Data->IoStatus.Status = GetExceptionCode();
            Data->IoStatus.Information = 0;

            LOG_PRINT( LOGFL_ERRORS,
                       ("csg!csgPostReadBuffers:            %wZ Invalid user buffer, oldB=%p, status=%x\n",
                        &p2pCtx->VolCtx->Name,
                        origBuf,
                        Data->IoStatus.Status) );
        }

    } finally {

        //
        //  If we are supposed to, cleanup the allocated memory and release
        //  the volume context.  The freeing of the MDL (if there is one) is
        //  handled by FltMgr.
        //

        if (cleanupAllocatedBuffer) {

            LOG_PRINT( LOGFL_READ,
                       ("csg!csgPostReadBuffers:            %wZ newB=%p info=%d Freeing\n",
                        &p2pCtx->VolCtx->Name,
                        p2pCtx->SwappedBuffer,
                        Data->IoStatus.Information) );

            ExFreePool( p2pCtx->SwappedBuffer );
            FltReleaseContext( p2pCtx->VolCtx );

            ExFreeToNPagedLookasideList( &Pre2PostContextList,
                                         p2pCtx );
        }
    }

    return retValue;
}


FLT_POSTOP_CALLBACK_STATUS
SwapPostReadBuffersWhenSafe (
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in PVOID CompletionContext,
    __in FLT_POST_OPERATION_FLAGS Flags
    )
/*++

Routine Description:

    We had an arbitrary users buffer without a MDL so we needed to get
    to a safe IRQL so we could lock it and then copy the data.

Arguments:

    Data - Pointer to the filter callbackData that is passed to us.

    FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
        opaque handles to this filter, instance, its associated volume and
        file object.

    CompletionContext - Contains state from our PreOperation callback

    Flags - Denotes whether the completion is successful or is being drained.

Return Value:

    FLT_POSTOP_FINISHED_PROCESSING - This is always returned.

--*/
{
    PFLT_IO_PARAMETER_BLOCK iopb = Data->Iopb;
    PPRE_2_POST_CONTEXT p2pCtx = CompletionContext;
    PVOID origBuf;
    NTSTATUS status;

    UNREFERENCED_PARAMETER( FltObjects );
    UNREFERENCED_PARAMETER( Flags );
    ASSERT(Data->IoStatus.Information != 0);

    //
    //  This is some sort of user buffer without a MDL, lock the user buffer
    //  so we can access it.  This will create a MDL for it.
    //

    status = FltLockUserBuffer( Data );

    if (!NT_SUCCESS(status)) {

        LOG_PRINT( LOGFL_ERRORS,
                   ("csg!SwapPostReadBuffersWhenSafe:    %wZ Could not lock user buffer, oldB=%p, status=%x\n",
                    &p2pCtx->VolCtx->Name,
                    iopb->Parameters.Read.ReadBuffer,
                    status) );

        //
        //  If we can't lock the buffer, fail the operation
        //

        Data->IoStatus.Status = status;
        Data->IoStatus.Information = 0;

    } else {

        //
        //  Get a system address for this buffer.
        //

        origBuf = MmGetSystemAddressForMdlSafe( iopb->Parameters.Read.MdlAddress,
                                                NormalPagePriority );

        if (origBuf == NULL) {

            LOG_PRINT( LOGFL_ERRORS,
                       ("csg!SwapPostReadBuffersWhenSafe:    %wZ Failed to get system address for MDL: %p\n",
                        &p2pCtx->VolCtx->Name,
                        iopb->Parameters.Read.MdlAddress) );

            //
            //  If we couldn't get a SYSTEM buffer address, fail the operation
            //

            Data->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            Data->IoStatus.Information = 0;

        } else {

            //
            //  Copy the data back to the original buffer.  Note that we
            //  don't need a try/except because we will always have a system
            //  buffer address.
            //

            RtlCopyMemory( origBuf,
                           p2pCtx->SwappedBuffer,
                           Data->IoStatus.Information );
        }
    }

    //
    //  Free allocated memory and release the volume context
    //

    LOG_PRINT( LOGFL_READ,
               ("csg!SwapPostReadBuffersWhenSafe:    %wZ newB=%p info=%d Freeing\n",
                &p2pCtx->VolCtx->Name,
                p2pCtx->SwappedBuffer,
                Data->IoStatus.Information) );

    ExFreePool( p2pCtx->SwappedBuffer );
    FltReleaseContext( p2pCtx->VolCtx );

    ExFreeToNPagedLookasideList( &Pre2PostContextList,
                                 p2pCtx );

    return FLT_POSTOP_FINISHED_PROCESSING;
}


