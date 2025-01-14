/* $Id$ */
/** @file
 * DrvVD - Generic VBox disk media driver.
 */

/*
 * Copyright (C) 2006-2015 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*********************************************************************************************************************************
*   Header files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_DRV_VD
#include <VBox/vd.h>
#include <VBox/vmm/pdmdrv.h>
#include <VBox/vmm/pdmstorageifs.h>
#include <VBox/vmm/pdmasynccompletion.h>
#include <VBox/vmm/pdmblkcache.h>
#include <iprt/asm.h>
#include <iprt/alloc.h>
#include <iprt/assert.h>
#include <iprt/uuid.h>
#include <iprt/file.h>
#include <iprt/string.h>
#include <iprt/tcp.h>
#include <iprt/semaphore.h>
#include <iprt/sg.h>
#include <iprt/poll.h>
#include <iprt/pipe.h>
#include <iprt/system.h>
#include <iprt/memsafer.h>
#include <iprt/memcache.h>
#include <iprt/list.h>

#ifdef VBOX_WITH_INIP
/* All lwip header files are not C++ safe. So hack around this. */
RT_C_DECLS_BEGIN
#include <lwip/opt.h>
#include <lwip/inet.h>
#include <lwip/tcp.h>
#include <lwip/sockets.h>
# if LWIP_IPV6
#  include <lwip/inet6.h>
# endif
RT_C_DECLS_END
#endif /* VBOX_WITH_INIP */

#include "HBDMgmt.h"

#include "VBoxDD.h"

#ifdef VBOX_WITH_INIP
/* Small hack to get at lwIP initialized status */
extern bool DevINIPConfigured(void);
#endif /* VBOX_WITH_INIP */


/** @def VBOX_PERIODIC_FLUSH
 * Enable support for periodically flushing the VDI to disk. This may prove
 * useful for those nasty problems with the ultra-slow host filesystems.
 * If this is enabled, it can be configured via the CFGM key
 * "VBoxInternal/Devices/piix3ide/0/LUN#<x>/Config/FlushInterval". <x>
 * must be replaced with the correct LUN number of the disk that should
 * do the periodic flushes. The value of the key is the number of bytes
 * written between flushes. A value of 0 (the default) denotes no flushes. */
#define VBOX_PERIODIC_FLUSH

/** @def VBOX_IGNORE_FLUSH
 * Enable support for ignoring VDI flush requests. This can be useful for
 * filesystems that show bad guest IDE write performance (especially with
 * Windows guests). NOTE that this does not disable the flushes caused by
 * the periodic flush cache feature above.
 * If this feature is enabled, it can be configured via the CFGM key
 * "VBoxInternal/Devices/piix3ide/0/LUN#<x>/Config/IgnoreFlush". <x>
 * must be replaced with the correct LUN number of the disk that should
 * ignore flush requests. The value of the key is a boolean. The default
 * is to ignore flushes, i.e. true. */
#define VBOX_IGNORE_FLUSH

/*********************************************************************************************************************************
*   Defined types, constants and macros                                                                                          *
*********************************************************************************************************************************/

/** Converts a pointer to VBOXDISK::IMedia to a PVBOXDISK. */
#define PDMIMEDIA_2_VBOXDISK(pInterface) \
    ( (PVBOXDISK)((uintptr_t)pInterface - RT_OFFSETOF(VBOXDISK, IMedia)) )

/** Converts a pointer to VBOXDISK::IMediaAsync to a PVBOXDISK. */
#define PDMIMEDIAASYNC_2_VBOXDISK(pInterface) \
    ( (PVBOXDISK)((uintptr_t)pInterface - RT_OFFSETOF(VBOXDISK, IMediaAsync)) )

/** Forward declaration for the dis kcontainer. */
typedef struct VBOXDISK *PVBOXDISK;

/**
 * VBox disk container, image information, private part.
 */

typedef struct VBOXIMAGE
{
    /** Pointer to next image. */
    struct VBOXIMAGE    *pNext;
    /** Pointer to list of VD interfaces. Per-image. */
    PVDINTERFACE       pVDIfsImage;
    /** Configuration information interface. */
    VDINTERFACECONFIG  VDIfConfig;
    /** TCP network stack interface. */
    VDINTERFACETCPNET  VDIfTcpNet;
    /** I/O interface. */
    VDINTERFACEIO      VDIfIo;
} VBOXIMAGE, *PVBOXIMAGE;

/**
 * Storage backend data.
 */
typedef struct DRVVDSTORAGEBACKEND
{
    /** PDM async completion end point. */
    PPDMASYNCCOMPLETIONENDPOINT pEndpoint;
    /** The template. */
    PPDMASYNCCOMPLETIONTEMPLATE pTemplate;
    /** Event semaphore for synchronous operations. */
    RTSEMEVENT                  EventSem;
    /** Flag whether a synchronous operation is currently pending. */
    volatile bool               fSyncIoPending;
    /** Return code of the last completed request. */
    int                         rcReqLast;
    /** Callback routine */
    PFNVDCOMPLETED              pfnCompleted;
} DRVVDSTORAGEBACKEND, *PDRVVDSTORAGEBACKEND;

/**
 * VD I/O request state.
 */
typedef enum VDIOREQSTATE
{
    /** Invalid. */
    VDIOREQSTATE_INVALID = 0,
    /** The request is not in use and resides on the free list. */
    VDIOREQSTATE_FREE,
    /** The request was just allocated and is not active. */
    VDIOREQSTATE_ALLOCATED,
    /** The request was allocated and is in use. */
    VDIOREQSTATE_ACTIVE,
    /** The request is in the last step of completion and syncs memory. */
    VDIOREQSTATE_COMPLETING,
    /** The request completed. */
    VDIOREQSTATE_COMPLETED,
    /** The request was aborted but wasn't returned as complete from the storage
     * layer below us. */
    VDIOREQSTATE_CANCELED,
    /** 32bit hack. */
    VDIOREQSTATE_32BIT_HACK = 0x7fffffff
} VDIOREQSTATE;

/**
 * VD I/O Request.
 */
typedef struct PDMMEDIAEXIOREQINT
{
    /** List node for the list of allocated requests. */
    RTLISTNODE                    NdAllocatedList;
    /** I/O request type. */
    PDMMEDIAEXIOREQTYPE           enmType;
    /** Request state. */
    volatile VDIOREQSTATE         enmState;
    /** I/O request ID. */
    PDMMEDIAEXIOREQID             uIoReqId;
    /** Flags. */
    uint32_t                      fFlags;
    /** Pointer to the disk container. */
    PVBOXDISK                     pDisk;
    /** I/O memory buffer. */
    RTSGSEG                       DataSeg;
    /** S/G buffer for this request. */
    RTSGBUF                       SgBuf;
    /** Allocator specific memory - variable size. */
    uint8_t                       abAlloc[1];
} PDMMEDIAEXIOREQINT;
/** Pointer to a VD I/O request. */
typedef PDMMEDIAEXIOREQINT *PPDMMEDIAEXIOREQINT;

/**
 * Structure for holding a list of allocated requests.
 */
typedef struct VDLSTIOREQALLOC
{
    /** Mutex protecting the table of allocated requests. */
    RTSEMFASTMUTEX           hMtxLstIoReqAlloc;
    /** List anchor. */
    RTLISTANCHOR             LstIoReqAlloc;
} VDLSTIOREQALLOC;
typedef VDLSTIOREQALLOC *PVDLSTIOREQALLOC;

/** Number of bins for allocated requests. */
#define DRVVD_VDIOREQ_ALLOC_BINS    8

/**
 * VBox disk container media main structure, private part.
 *
 * @implements  PDMIMEDIA
 * @implements  PDMIMEDIAASYNC
 * @implements  PDMIMEDIAEX
 * @implements  PDMIMOUNT
 * @implements  VDINTERFACEERROR
 * @implements  VDINTERFACETCPNET
 * @implements  VDINTERFACEASYNCIO
 * @implements  VDINTERFACECONFIG
 */
typedef struct VBOXDISK
{
    /** The VBox disk container. */
    PVBOXHDD                 pDisk;
    /** The media interface. */
    PDMIMEDIA                IMedia;
    /** Media port. */
    PPDMIMEDIAPORT           pDrvMediaPort;
    /** Pointer to the driver instance. */
    PPDMDRVINS               pDrvIns;
    /** Flag whether suspend has changed image open mode to read only. */
    bool                     fTempReadOnly;
    /** Flag whether to use the runtime (true) or startup error facility. */
    bool                     fErrorUseRuntime;
    /** Pointer to list of VD interfaces. Per-disk. */
    PVDINTERFACE             pVDIfsDisk;
    /** Error interface. */
    VDINTERFACEERROR         VDIfError;
    /** Thread synchronization interface. */
    VDINTERFACETHREADSYNC    VDIfThreadSync;

    /** Flag whether opened disk supports async I/O operations. */
    bool                     fAsyncIOSupported;
    /** The async media interface. */
    PDMIMEDIAASYNC           IMediaAsync;
    /** The async media port interface above. */
    PPDMIMEDIAASYNCPORT      pDrvMediaAsyncPort;
    /** Pointer to the list of data we need to keep per image. */
    PVBOXIMAGE               pImages;
    /** Flag whether the media should allow concurrent open for writing. */
    bool                     fShareable;
    /** Flag whether a merge operation has been set up. */
    bool                     fMergePending;
    /** Synchronization to prevent destruction before merge finishes. */
    RTSEMFASTMUTEX           MergeCompleteMutex;
    /** Synchronization between merge and other image accesses. */
    RTSEMRW                  MergeLock;
    /** Source image index for merging. */
    unsigned                 uMergeSource;
    /** Target image index for merging. */
    unsigned                 uMergeTarget;

    /** Flag whether boot acceleration is enabled. */
    bool                     fBootAccelEnabled;
    /** Flag whether boot acceleration is currently active. */
    bool                     fBootAccelActive;
    /** Size of the disk, used for read truncation. */
    uint64_t                 cbDisk;
    /** Size of the configured buffer. */
    size_t                   cbBootAccelBuffer;
    /** Start offset for which the buffer holds data. */
    uint64_t                 offDisk;
    /** Number of valid bytes in the buffer. */
    size_t                   cbDataValid;
    /** The disk buffer. */
    uint8_t                 *pbData;
    /** Bandwidth group the disk is assigned to. */
    char                    *pszBwGroup;
    /** Flag whether async I/O using the host cache is enabled. */
    bool                     fAsyncIoWithHostCache;

    /** I/O interface for a cache image. */
    VDINTERFACEIO            VDIfIoCache;
    /** Interface list for the cache image. */
    PVDINTERFACE             pVDIfsCache;

    /** The block cache handle if configured. */
    PPDMBLKCACHE             pBlkCache;
    /** Host block device manager. */
    HBDMGR                   hHbdMgr;

    /** Drive type. */
    PDMMEDIATYPE            enmType;
    /** Locked indicator. */
    bool                    fLocked;
    /** Mountable indicator. */
    bool                    fMountable;
    /** Visible to the BIOS. */
    bool                    fBiosVisible;
#ifdef VBOX_PERIODIC_FLUSH
    /** HACK: Configuration value for number of bytes written after which to flush. */
    uint32_t                cbFlushInterval;
    /** HACK: Current count for the number of bytes written since the last flush. */
    uint32_t                cbDataWritten;
#endif /* VBOX_PERIODIC_FLUSH */
#ifdef VBOX_IGNORE_FLUSH
    /** HACK: Disable flushes for this drive. */
    bool                    fIgnoreFlush;
    /** Disable async flushes for this drive. */
    bool                    fIgnoreFlushAsync;
#endif /* VBOX_IGNORE_FLUSH */
    /** Our mountable interface. */
    PDMIMOUNT               IMount;
    /** Pointer to the mount notify interface above us. */
    PPDMIMOUNTNOTIFY        pDrvMountNotify;
    /** Uuid of the drive. */
    RTUUID                  Uuid;
    /** BIOS PCHS Geometry. */
    PDMMEDIAGEOMETRY        PCHSGeometry;
    /** BIOS LCHS Geometry. */
    PDMMEDIAGEOMETRY        LCHSGeometry;

    /** Cryptographic support
     * @{ */
    /** Pointer to the CFGM node containing the config of the crypto filter
     * if enable. */
    PCFGMNODE                pCfgCrypto;
    /** Config interface for the encryption filter. */
    VDINTERFACECONFIG        VDIfCfg;
    /** Crypto interface for the encryption filter. */
    VDINTERFACECRYPTO        VDIfCrypto;
    /** The secret key interface used to retrieve keys. */
    PPDMISECKEY              pIfSecKey;
    /** The secret key helper interface used to notify about missing keys. */
    PPDMISECKEYHLP           pIfSecKeyHlp;
    /** @} */

    /** @name IMEDIAEX interface support specific members.
     * @{ */
    /** Pointer to the IMEDIAEXPORT interface above us. */
    PPDMIMEDIAEXPORT         pDrvMediaExPort;
    /** Our extended media interface. */
    PDMIMEDIAEX              IMediaEx;
    /** Memory cache for the I/O requests. */
    RTMEMCACHE               hIoReqCache;
    /** Active request counter. */
    volatile uint32_t        cIoReqsActive;
    /** Bins for allocated requests. */
    VDLSTIOREQALLOC          aIoReqAllocBins[DRVVD_VDIOREQ_ALLOC_BINS];
    /** @} */
} VBOXDISK;


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/

/**
 * Internal: allocate new image descriptor and put it in the list
 */
static PVBOXIMAGE drvvdNewImage(PVBOXDISK pThis)
{
    AssertPtr(pThis);
    PVBOXIMAGE pImage = (PVBOXIMAGE)RTMemAllocZ(sizeof(VBOXIMAGE));
    if (pImage)
    {
        pImage->pVDIfsImage = NULL;
        PVBOXIMAGE *pp = &pThis->pImages;
        while (*pp != NULL)
            pp = &(*pp)->pNext;
        *pp = pImage;
        pImage->pNext = NULL;
    }

    return pImage;
}

/**
 * Internal: free the list of images descriptors.
 */
static void drvvdFreeImages(PVBOXDISK pThis)
{
    while (pThis->pImages != NULL)
    {
        PVBOXIMAGE p = pThis->pImages;
        pThis->pImages = pThis->pImages->pNext;
        RTMemFree(p);
    }
}


/**
 * Make the image temporarily read-only.
 *
 * @returns VBox status code.
 * @param   pThis               The driver instance data.
 */
static int drvvdSetReadonly(PVBOXDISK pThis)
{
    int rc = VINF_SUCCESS;
    if (   pThis->pDisk
        && !VDIsReadOnly(pThis->pDisk))
    {
        unsigned uOpenFlags;
        rc = VDGetOpenFlags(pThis->pDisk, VD_LAST_IMAGE, &uOpenFlags);
        AssertRC(rc);
        uOpenFlags |= VD_OPEN_FLAGS_READONLY;
        rc = VDSetOpenFlags(pThis->pDisk, VD_LAST_IMAGE, uOpenFlags);
        AssertRC(rc);
        pThis->fTempReadOnly = true;
    }
    return rc;
}


/**
 * Undo the temporary read-only status of the image.
 *
 * @returns VBox status code.
 * @param   pThis               The driver instance data.
 */
static int drvvdSetWritable(PVBOXDISK pThis)
{
    int rc = VINF_SUCCESS;
    if (pThis->fTempReadOnly)
    {
        unsigned uOpenFlags;
        rc = VDGetOpenFlags(pThis->pDisk, VD_LAST_IMAGE, &uOpenFlags);
        AssertRC(rc);
        uOpenFlags &= ~VD_OPEN_FLAGS_READONLY;
        rc = VDSetOpenFlags(pThis->pDisk, VD_LAST_IMAGE, uOpenFlags);
        if (RT_SUCCESS(rc))
            pThis->fTempReadOnly = false;
        else
            AssertRC(rc);
    }
    return rc;
}


/*********************************************************************************************************************************
*   Error reporting callback                                                                                                     *
*********************************************************************************************************************************/

static DECLCALLBACK(void) drvvdErrorCallback(void *pvUser, int rc, RT_SRC_POS_DECL,
                               const char *pszFormat, va_list va)
{
    PPDMDRVINS pDrvIns = (PPDMDRVINS)pvUser;
    PVBOXDISK pThis = PDMINS_2_DATA(pDrvIns, PVBOXDISK);
    if (pThis->fErrorUseRuntime)
        /* We must not pass VMSETRTERR_FLAGS_FATAL as it could lead to a
         * deadlock: We are probably executed in a thread context != EMT
         * and the EM thread would wait until every thread is suspended
         * but we would wait for the EM thread ... */

        PDMDrvHlpVMSetRuntimeErrorV(pDrvIns, /* fFlags=*/ 0, "DrvVD", pszFormat, va);
    else
        PDMDrvHlpVMSetErrorV(pDrvIns, rc, RT_SRC_POS_ARGS, pszFormat, va);
}


/*********************************************************************************************************************************
*   VD Async I/O interface implementation                                                                                        *
*********************************************************************************************************************************/

#ifdef VBOX_WITH_PDM_ASYNC_COMPLETION

static DECLCALLBACK(void) drvvdAsyncTaskCompleted(PPDMDRVINS pDrvIns, void *pvTemplateUser, void *pvUser, int rcReq)
{
    PVBOXDISK pThis = PDMINS_2_DATA(pDrvIns, PVBOXDISK);
    PDRVVDSTORAGEBACKEND pStorageBackend = (PDRVVDSTORAGEBACKEND)pvTemplateUser;

    LogFlowFunc(("pDrvIns=%#p pvTemplateUser=%#p pvUser=%#p rcReq=%d\n",
                 pDrvIns, pvTemplateUser, pvUser, rcReq));

    if (pStorageBackend->fSyncIoPending)
    {
        Assert(!pvUser);
        pStorageBackend->rcReqLast      = rcReq;
        ASMAtomicWriteBool(&pStorageBackend->fSyncIoPending, false);
        RTSemEventSignal(pStorageBackend->EventSem);
    }
    else
    {
        int rc;

        AssertPtr(pvUser);

        AssertPtr(pStorageBackend->pfnCompleted);
        rc = pStorageBackend->pfnCompleted(pvUser, rcReq);
        AssertRC(rc);
    }
}

static DECLCALLBACK(int) drvvdAsyncIOOpen(void *pvUser, const char *pszLocation,
                                          uint32_t fOpen,
                                          PFNVDCOMPLETED pfnCompleted,
                                          void **ppStorage)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;
    PDRVVDSTORAGEBACKEND pStorageBackend = NULL;
    int rc = VINF_SUCCESS;

    /*
     * Check whether the backend wants to open a block device and try to prepare it
     * if we didn't claim it yet.
     *
     * We only create a block device manager on demand to not waste any resources.
     */
    if (HBDMgrIsBlockDevice(pszLocation))
    {
        if (pThis->hHbdMgr == NIL_HBDMGR)
            rc = HBDMgrCreate(&pThis->hHbdMgr);

        if (   RT_SUCCESS(rc)
            && !HBDMgrIsBlockDeviceClaimed(pThis->hHbdMgr, pszLocation))
            rc = HBDMgrClaimBlockDevice(pThis->hHbdMgr, pszLocation);

        if (RT_FAILURE(rc))
            return rc;
    }

    pStorageBackend = (PDRVVDSTORAGEBACKEND)RTMemAllocZ(sizeof(DRVVDSTORAGEBACKEND));
    if (pStorageBackend)
    {
        pStorageBackend->fSyncIoPending = false;
        pStorageBackend->rcReqLast      = VINF_SUCCESS;
        pStorageBackend->pfnCompleted   = pfnCompleted;

        rc = RTSemEventCreate(&pStorageBackend->EventSem);
        if (RT_SUCCESS(rc))
        {
            rc = PDMDrvHlpAsyncCompletionTemplateCreate(pThis->pDrvIns, &pStorageBackend->pTemplate,
                                                        drvvdAsyncTaskCompleted, pStorageBackend, "AsyncTaskCompleted");
            if (RT_SUCCESS(rc))
            {
                uint32_t fFlags = (fOpen & RTFILE_O_ACCESS_MASK) == RTFILE_O_READ
                                ? PDMACEP_FILE_FLAGS_READ_ONLY
                                : 0;
                if (pThis->fShareable)
                {
                    Assert((fOpen & RTFILE_O_DENY_MASK) == RTFILE_O_DENY_NONE);

                    fFlags |= PDMACEP_FILE_FLAGS_DONT_LOCK;
                }
                if (pThis->fAsyncIoWithHostCache)
                    fFlags |= PDMACEP_FILE_FLAGS_HOST_CACHE_ENABLED;

                rc = PDMR3AsyncCompletionEpCreateForFile(&pStorageBackend->pEndpoint,
                                                         pszLocation, fFlags,
                                                         pStorageBackend->pTemplate);

                if (RT_SUCCESS(rc))
                {
                    if (pThis->pszBwGroup)
                        rc = PDMR3AsyncCompletionEpSetBwMgr(pStorageBackend->pEndpoint, pThis->pszBwGroup);

                    if (RT_SUCCESS(rc))
                    {
                        LogFlow(("drvvdAsyncIOOpen: Successfully opened '%s'; fOpen=%#x pStorage=%p\n",
                                 pszLocation, fOpen, pStorageBackend));
                        *ppStorage = pStorageBackend;
                        return VINF_SUCCESS;
                    }

                    PDMR3AsyncCompletionEpClose(pStorageBackend->pEndpoint);
                }

                PDMR3AsyncCompletionTemplateDestroy(pStorageBackend->pTemplate);
            }
            RTSemEventDestroy(pStorageBackend->EventSem);
        }
        RTMemFree(pStorageBackend);
    }
    else
        rc = VERR_NO_MEMORY;

    return rc;
}

static DECLCALLBACK(int) drvvdAsyncIOClose(void *pvUser, void *pStorage)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;
    PDRVVDSTORAGEBACKEND pStorageBackend = (PDRVVDSTORAGEBACKEND)pStorage;

    /*
     * We don't unclaim any block devices on purpose here because they
     * might get reopened shortly (switching to readonly during suspend)
     *
     * Block devices will get unclaimed during destruction of the driver.
     */

    PDMR3AsyncCompletionEpClose(pStorageBackend->pEndpoint);
    PDMR3AsyncCompletionTemplateDestroy(pStorageBackend->pTemplate);
    RTSemEventDestroy(pStorageBackend->EventSem);
    RTMemFree(pStorageBackend);
    return VINF_SUCCESS;;
}

static DECLCALLBACK(int) drvvdAsyncIOReadSync(void *pvUser, void *pStorage, uint64_t uOffset,
                                              void *pvBuf, size_t cbRead, size_t *pcbRead)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;
    PDRVVDSTORAGEBACKEND pStorageBackend = (PDRVVDSTORAGEBACKEND)pStorage;
    RTSGSEG DataSeg;
    PPDMASYNCCOMPLETIONTASK pTask;

    bool fOld = ASMAtomicXchgBool(&pStorageBackend->fSyncIoPending, true);
    Assert(!fOld);
    DataSeg.cbSeg = cbRead;
    DataSeg.pvSeg = pvBuf;

    int rc = PDMR3AsyncCompletionEpRead(pStorageBackend->pEndpoint, uOffset, &DataSeg, 1, cbRead, NULL, &pTask);
    if (RT_FAILURE(rc))
        return rc;

    if (rc == VINF_AIO_TASK_PENDING)
    {
        /* Wait */
        rc = RTSemEventWait(pStorageBackend->EventSem, RT_INDEFINITE_WAIT);
        AssertRC(rc);
    }
    else
        ASMAtomicXchgBool(&pStorageBackend->fSyncIoPending, false);

    if (pcbRead)
        *pcbRead = cbRead;

    return pStorageBackend->rcReqLast;
}

static DECLCALLBACK(int) drvvdAsyncIOWriteSync(void *pvUser, void *pStorage, uint64_t uOffset,
                                               const void *pvBuf, size_t cbWrite, size_t *pcbWritten)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;
    PDRVVDSTORAGEBACKEND pStorageBackend = (PDRVVDSTORAGEBACKEND)pStorage;
    RTSGSEG DataSeg;
    PPDMASYNCCOMPLETIONTASK pTask;

    bool fOld = ASMAtomicXchgBool(&pStorageBackend->fSyncIoPending, true);
    Assert(!fOld);
    DataSeg.cbSeg = cbWrite;
    DataSeg.pvSeg = (void *)pvBuf;

    int rc = PDMR3AsyncCompletionEpWrite(pStorageBackend->pEndpoint, uOffset, &DataSeg, 1, cbWrite, NULL, &pTask);
    if (RT_FAILURE(rc))
        return rc;

    if (rc == VINF_AIO_TASK_PENDING)
    {
        /* Wait */
        rc = RTSemEventWait(pStorageBackend->EventSem, RT_INDEFINITE_WAIT);
        AssertRC(rc);
    }
    else
        ASMAtomicXchgBool(&pStorageBackend->fSyncIoPending, false);

    if (pcbWritten)
        *pcbWritten = cbWrite;

    return pStorageBackend->rcReqLast;
}

static DECLCALLBACK(int) drvvdAsyncIOFlushSync(void *pvUser, void *pStorage)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;
    PDRVVDSTORAGEBACKEND pStorageBackend = (PDRVVDSTORAGEBACKEND)pStorage;
    PPDMASYNCCOMPLETIONTASK pTask;

    LogFlowFunc(("pvUser=%#p pStorage=%#p\n", pvUser, pStorage));

    bool fOld = ASMAtomicXchgBool(&pStorageBackend->fSyncIoPending, true);
    Assert(!fOld);

    int rc = PDMR3AsyncCompletionEpFlush(pStorageBackend->pEndpoint, NULL, &pTask);
    if (RT_FAILURE(rc))
        return rc;

    if (rc == VINF_AIO_TASK_PENDING)
    {
        /* Wait */
        LogFlowFunc(("Waiting for flush to complete\n"));
        rc = RTSemEventWait(pStorageBackend->EventSem, RT_INDEFINITE_WAIT);
        AssertRC(rc);
    }
    else
        ASMAtomicXchgBool(&pStorageBackend->fSyncIoPending, false);

    return pStorageBackend->rcReqLast;
}

static DECLCALLBACK(int) drvvdAsyncIOReadAsync(void *pvUser, void *pStorage, uint64_t uOffset,
                                               PCRTSGSEG paSegments, size_t cSegments,
                                               size_t cbRead, void *pvCompletion,
                                               void **ppTask)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;
    PDRVVDSTORAGEBACKEND pStorageBackend = (PDRVVDSTORAGEBACKEND)pStorage;

    int rc = PDMR3AsyncCompletionEpRead(pStorageBackend->pEndpoint, uOffset, paSegments, (unsigned)cSegments, cbRead,
                                        pvCompletion, (PPPDMASYNCCOMPLETIONTASK)ppTask);
    if (rc == VINF_AIO_TASK_PENDING)
        rc = VERR_VD_ASYNC_IO_IN_PROGRESS;

    return rc;
}

static DECLCALLBACK(int) drvvdAsyncIOWriteAsync(void *pvUser, void *pStorage, uint64_t uOffset,
                                                PCRTSGSEG paSegments, size_t cSegments,
                                                size_t cbWrite, void *pvCompletion,
                                                void **ppTask)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;
    PDRVVDSTORAGEBACKEND pStorageBackend = (PDRVVDSTORAGEBACKEND)pStorage;

    int rc = PDMR3AsyncCompletionEpWrite(pStorageBackend->pEndpoint, uOffset, paSegments, (unsigned)cSegments, cbWrite,
                                         pvCompletion, (PPPDMASYNCCOMPLETIONTASK)ppTask);
    if (rc == VINF_AIO_TASK_PENDING)
        rc = VERR_VD_ASYNC_IO_IN_PROGRESS;

    return rc;
}

static DECLCALLBACK(int) drvvdAsyncIOFlushAsync(void *pvUser, void *pStorage,
                                                void *pvCompletion, void **ppTask)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;
    PDRVVDSTORAGEBACKEND pStorageBackend = (PDRVVDSTORAGEBACKEND)pStorage;

    int rc = PDMR3AsyncCompletionEpFlush(pStorageBackend->pEndpoint, pvCompletion,
                                         (PPPDMASYNCCOMPLETIONTASK)ppTask);
    if (rc == VINF_AIO_TASK_PENDING)
        rc = VERR_VD_ASYNC_IO_IN_PROGRESS;

    return rc;
}

static DECLCALLBACK(int) drvvdAsyncIOGetSize(void *pvUser, void *pStorage, uint64_t *pcbSize)
{
    PVBOXDISK pDrvVD = (PVBOXDISK)pvUser;
    PDRVVDSTORAGEBACKEND pStorageBackend = (PDRVVDSTORAGEBACKEND)pStorage;

    return PDMR3AsyncCompletionEpGetSize(pStorageBackend->pEndpoint, pcbSize);
}

static DECLCALLBACK(int) drvvdAsyncIOSetSize(void *pvUser, void *pStorage, uint64_t cbSize)
{
    PVBOXDISK pDrvVD = (PVBOXDISK)pvUser;
    PDRVVDSTORAGEBACKEND pStorageBackend = (PDRVVDSTORAGEBACKEND)pStorage;

    return PDMR3AsyncCompletionEpSetSize(pStorageBackend->pEndpoint, cbSize);
}

#endif /* VBOX_WITH_PDM_ASYNC_COMPLETION */


/*********************************************************************************************************************************
*   VD Thread Synchronization interface implementation                                                                           *
*********************************************************************************************************************************/

static DECLCALLBACK(int) drvvdThreadStartRead(void *pvUser)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;

    return RTSemRWRequestRead(pThis->MergeLock, RT_INDEFINITE_WAIT);
}

static DECLCALLBACK(int) drvvdThreadFinishRead(void *pvUser)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;

    return RTSemRWReleaseRead(pThis->MergeLock);
}

static DECLCALLBACK(int) drvvdThreadStartWrite(void *pvUser)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;

    return RTSemRWRequestWrite(pThis->MergeLock, RT_INDEFINITE_WAIT);
}

static DECLCALLBACK(int) drvvdThreadFinishWrite(void *pvUser)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;

    return RTSemRWReleaseWrite(pThis->MergeLock);
}


/*********************************************************************************************************************************
*   VD Configuration interface implementation                                                                                    *
*********************************************************************************************************************************/

static DECLCALLBACK(bool) drvvdCfgAreKeysValid(void *pvUser, const char *pszzValid)
{
    return CFGMR3AreValuesValid((PCFGMNODE)pvUser, pszzValid);
}

static DECLCALLBACK(int) drvvdCfgQuerySize(void *pvUser, const char *pszName, size_t *pcb)
{
    return CFGMR3QuerySize((PCFGMNODE)pvUser, pszName, pcb);
}

static DECLCALLBACK(int) drvvdCfgQuery(void *pvUser, const char *pszName, char *pszString, size_t cchString)
{
    return CFGMR3QueryString((PCFGMNODE)pvUser, pszName, pszString, cchString);
}

static DECLCALLBACK(int) drvvdCfgQueryBytes(void *pvUser, const char *pszName, void *ppvData, size_t cbData)
{
    return CFGMR3QueryBytes((PCFGMNODE)pvUser, pszName, ppvData, cbData);
}


/*******************************************************************************
*   VD Crypto interface implementation for the encryption support       *
*******************************************************************************/

static DECLCALLBACK(int) drvvdCryptoKeyRetain(void *pvUser, const char *pszId, const uint8_t **ppbKey, size_t *pcbKey)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;
    int rc = VINF_SUCCESS;

    AssertPtr(pThis->pIfSecKey);
    if (pThis->pIfSecKey)
        rc = pThis->pIfSecKey->pfnKeyRetain(pThis->pIfSecKey, pszId, ppbKey, pcbKey);
    else
        rc = VERR_NOT_SUPPORTED;

    return rc;
}

static DECLCALLBACK(int) drvvdCryptoKeyRelease(void *pvUser, const char *pszId)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;
    int rc = VINF_SUCCESS;

    AssertPtr(pThis->pIfSecKey);
    if (pThis->pIfSecKey)
        rc = pThis->pIfSecKey->pfnKeyRelease(pThis->pIfSecKey, pszId);
    else
        rc = VERR_NOT_SUPPORTED;

    return rc;
}

static DECLCALLBACK(int) drvvdCryptoKeyStorePasswordRetain(void *pvUser, const char *pszId, const char **ppszPassword)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;
    int rc = VINF_SUCCESS;

    AssertPtr(pThis->pIfSecKey);
    if (pThis->pIfSecKey)
        rc = pThis->pIfSecKey->pfnPasswordRetain(pThis->pIfSecKey, pszId, ppszPassword);
    else
        rc = VERR_NOT_SUPPORTED;

    return rc;
}

static DECLCALLBACK(int) drvvdCryptoKeyStorePasswordRelease(void *pvUser, const char *pszId)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser;
    int rc = VINF_SUCCESS;

    AssertPtr(pThis->pIfSecKey);
    if (pThis->pIfSecKey)
        rc = pThis->pIfSecKey->pfnPasswordRelease(pThis->pIfSecKey, pszId);
    else
        rc = VERR_NOT_SUPPORTED;

    return rc;
}

#ifdef VBOX_WITH_INIP


/*********************************************************************************************************************************
*   VD TCP network stack interface implementation - INIP case                                                                    *
*********************************************************************************************************************************/

/**
 * vvl: this structure duplicate meaning of sockaddr,
 * perhaps it'd be better to get rid of it.
 */
typedef union INIPSOCKADDRUNION
{
    struct sockaddr     Addr;
    struct sockaddr_in  Ipv4;
#if LWIP_IPV6
    struct sockaddr_in6 Ipv6;
#endif
} INIPSOCKADDRUNION;

typedef struct INIPSOCKET
{
    int hSock;
} INIPSOCKET, *PINIPSOCKET;

static DECLCALLBACK(int) drvvdINIPFlush(VDSOCKET Sock);

/** @interface_method_impl{VDINTERFACETCPNET,pfnSocketCreate} */
static DECLCALLBACK(int) drvvdINIPSocketCreate(uint32_t fFlags, PVDSOCKET pSock)
{
    PINIPSOCKET pSocketInt = NULL;

    /*
     * The extended select method is not supported because it is impossible to wakeup
     * the thread.
     */
    if (fFlags & VD_INTERFACETCPNET_CONNECT_EXTENDED_SELECT)
        return VERR_NOT_SUPPORTED;

    pSocketInt = (PINIPSOCKET)RTMemAllocZ(sizeof(INIPSOCKET));
    if (pSocketInt)
    {
        pSocketInt->hSock = INT32_MAX;
        *pSock = (VDSOCKET)pSocketInt;
        return VINF_SUCCESS;
    }

    return VERR_NO_MEMORY;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnSocketCreate} */
static DECLCALLBACK(int) drvvdINIPSocketDestroy(VDSOCKET Sock)
{
    PINIPSOCKET pSocketInt = (PINIPSOCKET)Sock;

    RTMemFree(pSocketInt);
    return VINF_SUCCESS;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnClientConnect} */
static DECLCALLBACK(int) drvvdINIPClientConnect(VDSOCKET Sock, const char *pszAddress, uint32_t uPort,
                                                RTMSINTERVAL cMillies)
{
    int rc = VINF_SUCCESS;
    PINIPSOCKET pSocketInt = (PINIPSOCKET)Sock;
    int iInetFamily = PF_INET;
    struct in_addr ip;
#if LWIP_IPV6
    ip6_addr_t ip6;
#endif

    NOREF(cMillies); /** LwIP doesn't support connect timeout. */

    /* Check whether lwIP is set up in this VM instance. */
    if (!DevINIPConfigured())
    {
        LogRelFunc(("no IP stack\n"));
        return VERR_NET_HOST_UNREACHABLE;
    }
    /* Resolve hostname. As there is no standard resolver for lwIP yet,
     * just accept numeric IP addresses for now. */
#if LWIP_IPV6
    if (inet6_aton(pszAddress, &ip6))
        iInetFamily = PF_INET6;
    else /* concatination with if */
#endif
      if (!lwip_inet_aton(pszAddress, &ip))
    {
        LogRelFunc(("cannot resolve IP %s\n", pszAddress));
        return VERR_NET_HOST_UNREACHABLE;
    }
    /* Create socket and connect. */
    int iSock = lwip_socket(iInetFamily, SOCK_STREAM, 0);
    if (iSock != -1)
    {
        struct sockaddr *pSockAddr = NULL;
        struct sockaddr_in InAddr = {0};
#if LWIP_IPV6
        struct sockaddr_in6 In6Addr = {0};
#endif
        if (iInetFamily == PF_INET)
        {
            InAddr.sin_family = AF_INET;
            InAddr.sin_port = htons(uPort);
            InAddr.sin_addr = ip;
            InAddr.sin_len = sizeof(InAddr);
            pSockAddr = (struct sockaddr *)&InAddr;
        }
#if LWIP_IPV6
        else
        {
            In6Addr.sin6_family = AF_INET6;
            In6Addr.sin6_port = htons(uPort);
            memcpy(&In6Addr.sin6_addr, &ip6, sizeof(ip6));
            In6Addr.sin6_len = sizeof(In6Addr);
            pSockAddr = (struct sockaddr *)&In6Addr;
        }
#endif
        if (   pSockAddr
            && !lwip_connect(iSock, pSockAddr, pSockAddr->sa_len))
        {
            pSocketInt->hSock = iSock;
            return VINF_SUCCESS;
        }
        rc = VERR_NET_CONNECTION_REFUSED; /* @todo real solution needed */
        lwip_close(iSock);
    }
    else
        rc = VERR_NET_CONNECTION_REFUSED; /* @todo real solution needed */
    return rc;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnClientClose} */
static DECLCALLBACK(int) drvvdINIPClientClose(VDSOCKET Sock)
{
    PINIPSOCKET pSocketInt = (PINIPSOCKET)Sock;

    lwip_close(pSocketInt->hSock);
    pSocketInt->hSock = INT32_MAX;
    return VINF_SUCCESS; /** @todo real solution needed */
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnIsClientConnected} */
static DECLCALLBACK(bool) drvvdINIPIsClientConnected(VDSOCKET Sock)
{
    PINIPSOCKET pSocketInt = (PINIPSOCKET)Sock;

    return pSocketInt->hSock != INT32_MAX;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnSelectOne} */
static DECLCALLBACK(int) drvvdINIPSelectOne(VDSOCKET Sock, RTMSINTERVAL cMillies)
{
    PINIPSOCKET pSocketInt = (PINIPSOCKET)Sock;
    fd_set fdsetR;
    FD_ZERO(&fdsetR);
    FD_SET((uintptr_t)pSocketInt->hSock, &fdsetR);
    fd_set fdsetE = fdsetR;

    int rc;
    if (cMillies == RT_INDEFINITE_WAIT)
        rc = lwip_select(pSocketInt->hSock + 1, &fdsetR, NULL, &fdsetE, NULL);
    else
    {
        struct timeval timeout;
        timeout.tv_sec = cMillies / 1000;
        timeout.tv_usec = (cMillies % 1000) * 1000;
        rc = lwip_select(pSocketInt->hSock + 1, &fdsetR, NULL, &fdsetE, &timeout);
    }
    if (rc > 0)
        return VINF_SUCCESS;
    if (rc == 0)
        return VERR_TIMEOUT;
    return VERR_NET_CONNECTION_REFUSED; /** @todo real solution needed */
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnRead} */
static DECLCALLBACK(int) drvvdINIPRead(VDSOCKET Sock, void *pvBuffer, size_t cbBuffer, size_t *pcbRead)
{
    PINIPSOCKET pSocketInt = (PINIPSOCKET)Sock;

    /* Do params checking */
    if (!pvBuffer || !cbBuffer)
    {
        AssertMsgFailed(("Invalid params\n"));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Read loop.
     * If pcbRead is NULL we have to fill the entire buffer!
     */
    size_t cbRead = 0;
    size_t cbToRead = cbBuffer;
    for (;;)
    {
        /** @todo this clipping here is just in case (the send function
         * needed it, so I added it here, too). Didn't investigate if this
         * really has issues. Better be safe than sorry. */
        ssize_t cbBytesRead = lwip_recv(pSocketInt->hSock, (char *)pvBuffer + cbRead,
                                        RT_MIN(cbToRead, 32768), 0);
        if (cbBytesRead < 0)
            return VERR_NET_CONNECTION_REFUSED; /** @todo real solution */
        if (cbBytesRead == 0 && errno) /** @todo r=bird: lwip_recv will not touch errno on Windows.  This may apply to other hosts as well  */
            return VERR_NET_CONNECTION_REFUSED; /** @todo real solution */
        if (pcbRead)
        {
            /* return partial data */
            *pcbRead = cbBytesRead;
            break;
        }

        /* read more? */
        cbRead += cbBytesRead;
        if (cbRead == cbBuffer)
            break;

        /* next */
        cbToRead = cbBuffer - cbRead;
    }

    return VINF_SUCCESS;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnWrite} */
static DECLCALLBACK(int) drvvdINIPWrite(VDSOCKET Sock, const void *pvBuffer, size_t cbBuffer)
{
    PINIPSOCKET pSocketInt = (PINIPSOCKET)Sock;

    do
    {
        /** @todo lwip send only supports up to 65535 bytes in a single
         * send (stupid limitation buried in the code), so make sure we
         * don't get any wraparounds. This should be moved to DevINIP
         * stack interface once that's implemented. */
        ssize_t cbWritten = lwip_send(pSocketInt->hSock, (void *)pvBuffer,
                                      RT_MIN(cbBuffer, 32768), 0);
        if (cbWritten < 0)
            return VERR_NET_CONNECTION_REFUSED; /** @todo real solution needed */
        AssertMsg(cbBuffer >= (size_t)cbWritten, ("Wrote more than we requested!!! cbWritten=%d cbBuffer=%d\n",
                                                  cbWritten, cbBuffer));
        cbBuffer -= cbWritten;
        pvBuffer = (const char *)pvBuffer + cbWritten;
    } while (cbBuffer);

    return VINF_SUCCESS;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnSgWrite} */
static DECLCALLBACK(int) drvvdINIPSgWrite(VDSOCKET Sock, PCRTSGBUF pSgBuf)
{
    int rc = VINF_SUCCESS;

    /* This is an extremely crude emulation, however it's good enough
     * for our iSCSI code. INIP has no sendmsg(). */
    for (unsigned i = 0; i < pSgBuf->cSegs; i++)
    {
        rc = drvvdINIPWrite(Sock, pSgBuf->paSegs[i].pvSeg,
                            pSgBuf->paSegs[i].cbSeg);
        if (RT_FAILURE(rc))
            break;
    }
    if (RT_SUCCESS(rc))
        drvvdINIPFlush(Sock);

    return rc;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnFlush} */
static DECLCALLBACK(int) drvvdINIPFlush(VDSOCKET Sock)
{
    PINIPSOCKET pSocketInt = (PINIPSOCKET)Sock;

    int fFlag = 1;
    lwip_setsockopt(pSocketInt->hSock, IPPROTO_TCP, TCP_NODELAY,
                    (const char *)&fFlag, sizeof(fFlag));
    fFlag = 0;
    lwip_setsockopt(pSocketInt->hSock, IPPROTO_TCP, TCP_NODELAY,
                    (const char *)&fFlag, sizeof(fFlag));
    return VINF_SUCCESS;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnSetSendCoalescing} */
static DECLCALLBACK(int) drvvdINIPSetSendCoalescing(VDSOCKET Sock, bool fEnable)
{
    PINIPSOCKET pSocketInt = (PINIPSOCKET)Sock;

    int fFlag = fEnable ? 0 : 1;
    lwip_setsockopt(pSocketInt->hSock, IPPROTO_TCP, TCP_NODELAY,
                    (const char *)&fFlag, sizeof(fFlag));
    return VINF_SUCCESS;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnGetLocalAddress} */
static DECLCALLBACK(int) drvvdINIPGetLocalAddress(VDSOCKET Sock, PRTNETADDR pAddr)
{
    PINIPSOCKET pSocketInt = (PINIPSOCKET)Sock;
    INIPSOCKADDRUNION u;
    socklen_t cbAddr = sizeof(u);
    RT_ZERO(u);
    if (!lwip_getsockname(pSocketInt->hSock, &u.Addr, &cbAddr))
    {
        /*
         * Convert the address.
         */
        if (   cbAddr == sizeof(struct sockaddr_in)
            && u.Addr.sa_family == AF_INET)
        {
            RT_ZERO(*pAddr);
            pAddr->enmType      = RTNETADDRTYPE_IPV4;
            pAddr->uPort        = RT_N2H_U16(u.Ipv4.sin_port);
            pAddr->uAddr.IPv4.u = u.Ipv4.sin_addr.s_addr;
        }
#if LWIP_IPV6
        else if (   cbAddr == sizeof(struct sockaddr_in6)
            && u.Addr.sa_family == AF_INET6)
        {
            RT_ZERO(*pAddr);
            pAddr->enmType      = RTNETADDRTYPE_IPV6;
            pAddr->uPort        = RT_N2H_U16(u.Ipv6.sin6_port);
            memcpy(&pAddr->uAddr.IPv6, &u.Ipv6.sin6_addr, sizeof(RTNETADDRIPV6));
        }
#endif
        else
            return VERR_NET_ADDRESS_FAMILY_NOT_SUPPORTED;
        return VINF_SUCCESS;
    }
    return VERR_NET_OPERATION_NOT_SUPPORTED;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnGetPeerAddress} */
static DECLCALLBACK(int) drvvdINIPGetPeerAddress(VDSOCKET Sock, PRTNETADDR pAddr)
{
    PINIPSOCKET pSocketInt = (PINIPSOCKET)Sock;
    INIPSOCKADDRUNION u;
    socklen_t cbAddr = sizeof(u);
    RT_ZERO(u);
    if (!lwip_getpeername(pSocketInt->hSock, &u.Addr, &cbAddr))
    {
        /*
         * Convert the address.
         */
        if (   cbAddr == sizeof(struct sockaddr_in)
            && u.Addr.sa_family == AF_INET)
        {
            RT_ZERO(*pAddr);
            pAddr->enmType      = RTNETADDRTYPE_IPV4;
            pAddr->uPort        = RT_N2H_U16(u.Ipv4.sin_port);
            pAddr->uAddr.IPv4.u = u.Ipv4.sin_addr.s_addr;
        }
#if LWIP_IPV6
        else if (   cbAddr == sizeof(struct sockaddr_in6)
                 && u.Addr.sa_family == AF_INET6)
        {
            RT_ZERO(*pAddr);
            pAddr->enmType      = RTNETADDRTYPE_IPV6;
            pAddr->uPort        = RT_N2H_U16(u.Ipv6.sin6_port);
            memcpy(&pAddr->uAddr.IPv6, &u.Ipv6.sin6_addr, sizeof(RTNETADDRIPV6));
        }
#endif
        else
            return VERR_NET_ADDRESS_FAMILY_NOT_SUPPORTED;
        return VINF_SUCCESS;
    }
    return VERR_NET_OPERATION_NOT_SUPPORTED;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnSelectOneEx} */
static DECLCALLBACK(int) drvvdINIPSelectOneEx(VDSOCKET Sock, uint32_t fEvents, uint32_t *pfEvents, RTMSINTERVAL cMillies)
{
    AssertMsgFailed(("Not supported!\n"));
    return VERR_NOT_SUPPORTED;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnPoke} */
static DECLCALLBACK(int) drvvdINIPPoke(VDSOCKET Sock)
{
    AssertMsgFailed(("Not supported!\n"));
    return VERR_NOT_SUPPORTED;
}

#endif /* VBOX_WITH_INIP */


/*********************************************************************************************************************************
*   VD TCP network stack interface implementation - Host TCP case                                                                *
*********************************************************************************************************************************/

/**
 * Socket data.
 */
typedef struct VDSOCKETINT
{
    /** IPRT socket handle. */
    RTSOCKET      hSocket;
    /** Pollset with the wakeup pipe and socket. */
    RTPOLLSET     hPollSet;
    /** Pipe endpoint - read (in the pollset). */
    RTPIPE        hPipeR;
    /** Pipe endpoint - write. */
    RTPIPE        hPipeW;
    /** Flag whether the thread was woken up. */
    volatile bool fWokenUp;
    /** Flag whether the thread is waiting in the select call. */
    volatile bool fWaiting;
    /** Old event mask. */
    uint32_t      fEventsOld;
} VDSOCKETINT, *PVDSOCKETINT;

/** Pollset id of the socket. */
#define VDSOCKET_POLL_ID_SOCKET 0
/** Pollset id of the pipe. */
#define VDSOCKET_POLL_ID_PIPE   1

/** @interface_method_impl{VDINTERFACETCPNET,pfnSocketCreate} */
static DECLCALLBACK(int) drvvdTcpSocketCreate(uint32_t fFlags, PVDSOCKET pSock)
{
    int rc = VINF_SUCCESS;
    int rc2 = VINF_SUCCESS;
    PVDSOCKETINT pSockInt = NULL;

    pSockInt = (PVDSOCKETINT)RTMemAllocZ(sizeof(VDSOCKETINT));
    if (!pSockInt)
        return VERR_NO_MEMORY;

    pSockInt->hSocket  = NIL_RTSOCKET;
    pSockInt->hPollSet = NIL_RTPOLLSET;
    pSockInt->hPipeR   = NIL_RTPIPE;
    pSockInt->hPipeW   = NIL_RTPIPE;
    pSockInt->fWokenUp = false;
    pSockInt->fWaiting = false;

    if (fFlags & VD_INTERFACETCPNET_CONNECT_EXTENDED_SELECT)
    {
        /* Init pipe and pollset. */
        rc = RTPipeCreate(&pSockInt->hPipeR, &pSockInt->hPipeW, 0);
        if (RT_SUCCESS(rc))
        {
            rc = RTPollSetCreate(&pSockInt->hPollSet);
            if (RT_SUCCESS(rc))
            {
                rc = RTPollSetAddPipe(pSockInt->hPollSet, pSockInt->hPipeR,
                                      RTPOLL_EVT_READ, VDSOCKET_POLL_ID_PIPE);
                if (RT_SUCCESS(rc))
                {
                    *pSock = pSockInt;
                    return VINF_SUCCESS;
                }

                RTPollSetRemove(pSockInt->hPollSet, VDSOCKET_POLL_ID_PIPE);
                rc2 = RTPollSetDestroy(pSockInt->hPollSet);
                AssertRC(rc2);
            }

            rc2 = RTPipeClose(pSockInt->hPipeR);
            AssertRC(rc2);
            rc2 = RTPipeClose(pSockInt->hPipeW);
            AssertRC(rc2);
        }
    }
    else
    {
        *pSock = pSockInt;
        return VINF_SUCCESS;
    }

    RTMemFree(pSockInt);

    return rc;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnSocketDestroy} */
static DECLCALLBACK(int) drvvdTcpSocketDestroy(VDSOCKET Sock)
{
    int rc = VINF_SUCCESS;
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    /* Destroy the pipe and pollset if necessary. */
    if (pSockInt->hPollSet != NIL_RTPOLLSET)
    {
        if (pSockInt->hSocket != NIL_RTSOCKET)
        {
            rc = RTPollSetRemove(pSockInt->hPollSet, VDSOCKET_POLL_ID_SOCKET);
            Assert(RT_SUCCESS(rc) || rc == VERR_POLL_HANDLE_ID_NOT_FOUND);
        }
        rc = RTPollSetRemove(pSockInt->hPollSet, VDSOCKET_POLL_ID_PIPE);
        AssertRC(rc);
        rc = RTPollSetDestroy(pSockInt->hPollSet);
        AssertRC(rc);
        rc = RTPipeClose(pSockInt->hPipeR);
        AssertRC(rc);
        rc = RTPipeClose(pSockInt->hPipeW);
        AssertRC(rc);
    }

    if (pSockInt->hSocket != NIL_RTSOCKET)
        rc = RTTcpClientCloseEx(pSockInt->hSocket, false /*fGracefulShutdown*/);

    RTMemFree(pSockInt);

    return rc;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnClientConnect} */
static DECLCALLBACK(int) drvvdTcpClientConnect(VDSOCKET Sock, const char *pszAddress, uint32_t uPort,
                                               RTMSINTERVAL cMillies)
{
    int rc = VINF_SUCCESS;
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    rc = RTTcpClientConnectEx(pszAddress, uPort, &pSockInt->hSocket, cMillies, NULL);
    if (RT_SUCCESS(rc))
    {
        /* Add to the pollset if required. */
        if (pSockInt->hPollSet != NIL_RTPOLLSET)
        {
            pSockInt->fEventsOld = RTPOLL_EVT_READ | RTPOLL_EVT_WRITE | RTPOLL_EVT_ERROR;

            rc = RTPollSetAddSocket(pSockInt->hPollSet, pSockInt->hSocket,
                                    pSockInt->fEventsOld, VDSOCKET_POLL_ID_SOCKET);
        }

        if (RT_SUCCESS(rc))
            return VINF_SUCCESS;

        rc = RTTcpClientCloseEx(pSockInt->hSocket, false /*fGracefulShutdown*/);
    }

    return rc;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnClientClose} */
static DECLCALLBACK(int) drvvdTcpClientClose(VDSOCKET Sock)
{
    int rc = VINF_SUCCESS;
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    if (pSockInt->hPollSet != NIL_RTPOLLSET)
    {
        rc = RTPollSetRemove(pSockInt->hPollSet, VDSOCKET_POLL_ID_SOCKET);
        AssertRC(rc);
    }

    rc = RTTcpClientCloseEx(pSockInt->hSocket, false /*fGracefulShutdown*/);
    pSockInt->hSocket = NIL_RTSOCKET;

    return rc;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnIsClientConnected} */
static DECLCALLBACK(bool) drvvdTcpIsClientConnected(VDSOCKET Sock)
{
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    return pSockInt->hSocket != NIL_RTSOCKET;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnSelectOne} */
static DECLCALLBACK(int) drvvdTcpSelectOne(VDSOCKET Sock, RTMSINTERVAL cMillies)
{
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    return RTTcpSelectOne(pSockInt->hSocket, cMillies);
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnRead} */
static DECLCALLBACK(int) drvvdTcpRead(VDSOCKET Sock, void *pvBuffer, size_t cbBuffer, size_t *pcbRead)
{
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    return RTTcpRead(pSockInt->hSocket, pvBuffer, cbBuffer, pcbRead);
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnWrite} */
static DECLCALLBACK(int) drvvdTcpWrite(VDSOCKET Sock, const void *pvBuffer, size_t cbBuffer)
{
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    return RTTcpWrite(pSockInt->hSocket, pvBuffer, cbBuffer);
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnSgWrite} */
static DECLCALLBACK(int) drvvdTcpSgWrite(VDSOCKET Sock, PCRTSGBUF pSgBuf)
{
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    return RTTcpSgWrite(pSockInt->hSocket, pSgBuf);
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnReadNB} */
static DECLCALLBACK(int) drvvdTcpReadNB(VDSOCKET Sock, void *pvBuffer, size_t cbBuffer, size_t *pcbRead)
{
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    return RTTcpReadNB(pSockInt->hSocket, pvBuffer, cbBuffer, pcbRead);
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnWriteNB} */
static DECLCALLBACK(int) drvvdTcpWriteNB(VDSOCKET Sock, const void *pvBuffer, size_t cbBuffer, size_t *pcbWritten)
{
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    return RTTcpWriteNB(pSockInt->hSocket, pvBuffer, cbBuffer, pcbWritten);
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnSgWriteNB} */
static DECLCALLBACK(int) drvvdTcpSgWriteNB(VDSOCKET Sock, PRTSGBUF pSgBuf, size_t *pcbWritten)
{
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    return RTTcpSgWriteNB(pSockInt->hSocket, pSgBuf, pcbWritten);
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnFlush} */
static DECLCALLBACK(int) drvvdTcpFlush(VDSOCKET Sock)
{
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    return RTTcpFlush(pSockInt->hSocket);
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnSetSendCoalescing} */
static DECLCALLBACK(int) drvvdTcpSetSendCoalescing(VDSOCKET Sock, bool fEnable)
{
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    return RTTcpSetSendCoalescing(pSockInt->hSocket, fEnable);
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnGetLocalAddress} */
static DECLCALLBACK(int) drvvdTcpGetLocalAddress(VDSOCKET Sock, PRTNETADDR pAddr)
{
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    return RTTcpGetLocalAddress(pSockInt->hSocket, pAddr);
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnGetPeerAddress} */
static DECLCALLBACK(int) drvvdTcpGetPeerAddress(VDSOCKET Sock, PRTNETADDR pAddr)
{
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    return RTTcpGetPeerAddress(pSockInt->hSocket, pAddr);
}

static DECLCALLBACK(int) drvvdTcpSelectOneExPoll(VDSOCKET Sock, uint32_t fEvents,
                                                 uint32_t *pfEvents, RTMSINTERVAL cMillies)
{
    int rc = VINF_SUCCESS;
    uint32_t id = 0;
    uint32_t fEventsRecv = 0;
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    *pfEvents = 0;

    if (   pSockInt->fEventsOld != fEvents
        && pSockInt->hSocket != NIL_RTSOCKET)
    {
        uint32_t fPollEvents = 0;

        if (fEvents & VD_INTERFACETCPNET_EVT_READ)
            fPollEvents |= RTPOLL_EVT_READ;
        if (fEvents & VD_INTERFACETCPNET_EVT_WRITE)
            fPollEvents |= RTPOLL_EVT_WRITE;
        if (fEvents & VD_INTERFACETCPNET_EVT_ERROR)
            fPollEvents |= RTPOLL_EVT_ERROR;

        rc = RTPollSetEventsChange(pSockInt->hPollSet, VDSOCKET_POLL_ID_SOCKET, fPollEvents);
        if (RT_FAILURE(rc))
            return rc;

        pSockInt->fEventsOld = fEvents;
    }

    ASMAtomicXchgBool(&pSockInt->fWaiting, true);
    if (ASMAtomicXchgBool(&pSockInt->fWokenUp, false))
    {
        ASMAtomicXchgBool(&pSockInt->fWaiting, false);
        return VERR_INTERRUPTED;
    }

    rc = RTPoll(pSockInt->hPollSet, cMillies, &fEventsRecv, &id);
    Assert(RT_SUCCESS(rc) || rc == VERR_TIMEOUT);

    ASMAtomicXchgBool(&pSockInt->fWaiting, false);

    if (RT_SUCCESS(rc))
    {
        if (id == VDSOCKET_POLL_ID_SOCKET)
        {
            fEventsRecv &= RTPOLL_EVT_VALID_MASK;

            if (fEventsRecv & RTPOLL_EVT_READ)
                *pfEvents |= VD_INTERFACETCPNET_EVT_READ;
            if (fEventsRecv & RTPOLL_EVT_WRITE)
                *pfEvents |= VD_INTERFACETCPNET_EVT_WRITE;
            if (fEventsRecv & RTPOLL_EVT_ERROR)
                *pfEvents |= VD_INTERFACETCPNET_EVT_ERROR;
        }
        else
        {
            size_t cbRead = 0;
            uint8_t abBuf[10];
            Assert(id == VDSOCKET_POLL_ID_PIPE);
            Assert((fEventsRecv & RTPOLL_EVT_VALID_MASK) == RTPOLL_EVT_READ);

            /* We got interrupted, drain the pipe. */
            rc = RTPipeRead(pSockInt->hPipeR, abBuf, sizeof(abBuf), &cbRead);
            AssertRC(rc);

            ASMAtomicXchgBool(&pSockInt->fWokenUp, false);

            rc = VERR_INTERRUPTED;
        }
    }

    return rc;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnSelectOneEx} */
static DECLCALLBACK(int) drvvdTcpSelectOneExNoPoll(VDSOCKET Sock, uint32_t fEvents,
                                                   uint32_t *pfEvents, RTMSINTERVAL cMillies)
{
    int rc = VINF_SUCCESS;
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    *pfEvents = 0;

    ASMAtomicXchgBool(&pSockInt->fWaiting, true);
    if (ASMAtomicXchgBool(&pSockInt->fWokenUp, false))
    {
        ASMAtomicXchgBool(&pSockInt->fWaiting, false);
        return VERR_INTERRUPTED;
    }

    if (   pSockInt->hSocket == NIL_RTSOCKET
        || !fEvents)
    {
        /*
         * Only the pipe is configured or the caller doesn't wait for a socket event,
         * wait until there is something to read from the pipe.
         */
        size_t cbRead = 0;
        char ch = 0;
        rc = RTPipeReadBlocking(pSockInt->hPipeR, &ch, 1, &cbRead);
        if (RT_SUCCESS(rc))
        {
            Assert(cbRead == 1);
            rc = VERR_INTERRUPTED;
            ASMAtomicXchgBool(&pSockInt->fWokenUp, false);
        }
    }
    else
    {
        uint32_t fSelectEvents = 0;

        if (fEvents & VD_INTERFACETCPNET_EVT_READ)
            fSelectEvents |= RTSOCKET_EVT_READ;
        if (fEvents & VD_INTERFACETCPNET_EVT_WRITE)
            fSelectEvents |= RTSOCKET_EVT_WRITE;
        if (fEvents & VD_INTERFACETCPNET_EVT_ERROR)
            fSelectEvents |= RTSOCKET_EVT_ERROR;

        if (fEvents & VD_INTERFACETCPNET_HINT_INTERRUPT)
        {
            uint32_t fEventsRecv = 0;

            /* Make sure the socket is not in the pollset. */
            rc = RTPollSetRemove(pSockInt->hPollSet, VDSOCKET_POLL_ID_SOCKET);
            Assert(RT_SUCCESS(rc) || rc == VERR_POLL_HANDLE_ID_NOT_FOUND);

            for (;;)
            {
                uint32_t id = 0;
                rc = RTPoll(pSockInt->hPollSet, 5, &fEvents, &id);
                if (rc == VERR_TIMEOUT)
                {
                    /* Check the socket. */
                    rc = RTTcpSelectOneEx(pSockInt->hSocket, fSelectEvents, &fEventsRecv, 0);
                    if (RT_SUCCESS(rc))
                    {
                        if (fEventsRecv & RTSOCKET_EVT_READ)
                            *pfEvents |= VD_INTERFACETCPNET_EVT_READ;
                        if (fEventsRecv & RTSOCKET_EVT_WRITE)
                            *pfEvents |= VD_INTERFACETCPNET_EVT_WRITE;
                        if (fEventsRecv & RTSOCKET_EVT_ERROR)
                            *pfEvents |= VD_INTERFACETCPNET_EVT_ERROR;
                        break; /* Quit */
                    }
                    else if (rc != VERR_TIMEOUT)
                        break;
                }
                else if (RT_SUCCESS(rc))
                {
                    size_t cbRead = 0;
                    uint8_t abBuf[10];
                    Assert(id == VDSOCKET_POLL_ID_PIPE);
                    Assert((fEventsRecv & RTPOLL_EVT_VALID_MASK) == RTPOLL_EVT_READ);

                    /* We got interrupted, drain the pipe. */
                    rc = RTPipeRead(pSockInt->hPipeR, abBuf, sizeof(abBuf), &cbRead);
                    AssertRC(rc);

                    ASMAtomicXchgBool(&pSockInt->fWokenUp, false);

                    rc = VERR_INTERRUPTED;
                    break;
                }
                else
                    break;
            }
        }
        else /* The caller waits for a socket event. */
        {
            uint32_t fEventsRecv = 0;

            /* Loop until we got woken up or a socket event occurred. */
            for (;;)
            {
                /** @todo find an adaptive wait algorithm based on the
                 * number of wakeups in the past. */
                rc = RTTcpSelectOneEx(pSockInt->hSocket, fSelectEvents, &fEventsRecv, 5);
                if (rc == VERR_TIMEOUT)
                {
                    /* Check if there is an event pending. */
                    size_t cbRead = 0;
                    char ch = 0;
                    rc = RTPipeRead(pSockInt->hPipeR, &ch, 1, &cbRead);
                    if (RT_SUCCESS(rc) && rc != VINF_TRY_AGAIN)
                    {
                        Assert(cbRead == 1);
                        rc = VERR_INTERRUPTED;
                        ASMAtomicXchgBool(&pSockInt->fWokenUp, false);
                        break; /* Quit */
                    }
                    else
                        Assert(rc == VINF_TRY_AGAIN);
                }
                else if (RT_SUCCESS(rc))
                {
                    if (fEventsRecv & RTSOCKET_EVT_READ)
                        *pfEvents |= VD_INTERFACETCPNET_EVT_READ;
                    if (fEventsRecv & RTSOCKET_EVT_WRITE)
                        *pfEvents |= VD_INTERFACETCPNET_EVT_WRITE;
                    if (fEventsRecv & RTSOCKET_EVT_ERROR)
                        *pfEvents |= VD_INTERFACETCPNET_EVT_ERROR;
                    break; /* Quit */
                }
                else
                    break;
            }
        }
    }

    ASMAtomicXchgBool(&pSockInt->fWaiting, false);

    return rc;
}

/** @interface_method_impl{VDINTERFACETCPNET,pfnPoke} */
static DECLCALLBACK(int) drvvdTcpPoke(VDSOCKET Sock)
{
    int rc = VINF_SUCCESS;
    size_t cbWritten = 0;
    PVDSOCKETINT pSockInt = (PVDSOCKETINT)Sock;

    ASMAtomicXchgBool(&pSockInt->fWokenUp, true);

    if (ASMAtomicReadBool(&pSockInt->fWaiting))
    {
        rc = RTPipeWrite(pSockInt->hPipeW, "", 1, &cbWritten);
        Assert(RT_SUCCESS(rc) || cbWritten == 0);
    }

    return VINF_SUCCESS;
}

/**
 * Checks the prerequisites for encrypted I/O.
 *
 * @returns VBox status code.
 * @param   pThis    The VD driver instance data.
 */
static int drvvdKeyCheckPrereqs(PVBOXDISK pThis)
{
    if (   pThis->pCfgCrypto
        && !pThis->pIfSecKey)
    {
        AssertPtr(pThis->pIfSecKeyHlp);
        pThis->pIfSecKeyHlp->pfnKeyMissingNotify(pThis->pIfSecKeyHlp);

        int rc = PDMDrvHlpVMSetRuntimeError(pThis->pDrvIns, VMSETRTERR_FLAGS_SUSPEND | VMSETRTERR_FLAGS_NO_WAIT, "DrvVD_DEKMISSING",
                                            N_("VD: The DEK for this disk is missing"));
        AssertRC(rc);
        return VERR_VD_DEK_MISSING;
    }

    return VINF_SUCCESS;
}


/*********************************************************************************************************************************
*   Media interface methods                                                                                                      *
*********************************************************************************************************************************/

/** @interface_method_impl{PDMIMEDIA,pfnRead} */
static DECLCALLBACK(int) drvvdRead(PPDMIMEDIA pInterface,
                                   uint64_t off, void *pvBuf, size_t cbRead)
{
    int rc = VINF_SUCCESS;

    LogFlowFunc(("off=%#llx pvBuf=%p cbRead=%d\n", off, pvBuf, cbRead));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
    {
        AssertMsgFailed(("Invalid state! Not mounted!\n"));
        return VERR_PDM_MEDIA_NOT_MOUNTED;
    }

    rc = drvvdKeyCheckPrereqs(pThis);
    if (RT_FAILURE(rc))
        return rc;

    if (!pThis->fBootAccelActive)
        rc = VDRead(pThis->pDisk, off, pvBuf, cbRead);
    else
    {
        /* Can we serve the request from the buffer? */
        if (   off >= pThis->offDisk
            && off - pThis->offDisk < pThis->cbDataValid)
        {
            size_t cbToCopy = RT_MIN(cbRead, pThis->offDisk + pThis->cbDataValid - off);

            memcpy(pvBuf, pThis->pbData + (off - pThis->offDisk), cbToCopy);
            cbRead -= cbToCopy;
            off    += cbToCopy;
            pvBuf   = (char *)pvBuf + cbToCopy;
        }

        if (   cbRead > 0
            && cbRead < pThis->cbBootAccelBuffer)
        {
            /* Increase request to the buffer size and read. */
            pThis->cbDataValid = RT_MIN(pThis->cbDisk - off, pThis->cbBootAccelBuffer);
            pThis->offDisk = off;
            rc = VDRead(pThis->pDisk, off, pThis->pbData, pThis->cbDataValid);
            if (RT_FAILURE(rc))
                pThis->cbDataValid = 0;
            else
                memcpy(pvBuf, pThis->pbData, cbRead);
        }
        else if (cbRead >= pThis->cbBootAccelBuffer)
        {
            pThis->fBootAccelActive = false; /* Deactiviate */
        }
    }

    if (RT_SUCCESS(rc))
        Log2(("%s: off=%#llx pvBuf=%p cbRead=%d\n%.*Rhxd\n", __FUNCTION__,
              off, pvBuf, cbRead, cbRead, pvBuf));
    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}

/** @interface_method_impl{PDMIMEDIA,pfnRead} */
static DECLCALLBACK(int) drvvdReadPcBios(PPDMIMEDIA pInterface,
                                         uint64_t off, void *pvBuf, size_t cbRead)
{
    int rc = VINF_SUCCESS;

    LogFlowFunc(("off=%#llx pvBuf=%p cbRead=%d\n", off, pvBuf, cbRead));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
    {
        AssertMsgFailed(("Invalid state! Not mounted!\n"));
        return VERR_PDM_MEDIA_NOT_MOUNTED;
    }

    if (   pThis->pCfgCrypto
        && !pThis->pIfSecKey)
        return VERR_VD_DEK_MISSING;

    if (!pThis->fBootAccelActive)
        rc = VDRead(pThis->pDisk, off, pvBuf, cbRead);
    else
    {
        /* Can we serve the request from the buffer? */
        if (   off >= pThis->offDisk
            && off - pThis->offDisk < pThis->cbDataValid)
        {
            size_t cbToCopy = RT_MIN(cbRead, pThis->offDisk + pThis->cbDataValid - off);

            memcpy(pvBuf, pThis->pbData + (off - pThis->offDisk), cbToCopy);
            cbRead -= cbToCopy;
            off    += cbToCopy;
            pvBuf   = (char *)pvBuf + cbToCopy;
        }

        if (   cbRead > 0
            && cbRead < pThis->cbBootAccelBuffer)
        {
            /* Increase request to the buffer size and read. */
            pThis->cbDataValid = RT_MIN(pThis->cbDisk - off, pThis->cbBootAccelBuffer);
            pThis->offDisk = off;
            rc = VDRead(pThis->pDisk, off, pThis->pbData, pThis->cbDataValid);
            if (RT_FAILURE(rc))
                pThis->cbDataValid = 0;
            else
                memcpy(pvBuf, pThis->pbData, cbRead);
        }
        else if (cbRead >= pThis->cbBootAccelBuffer)
        {
            pThis->fBootAccelActive = false; /* Deactiviate */
        }
    }

    if (RT_SUCCESS(rc))
        Log2(("%s: off=%#llx pvBuf=%p cbRead=%d\n%.*Rhxd\n", __FUNCTION__,
              off, pvBuf, cbRead, cbRead, pvBuf));
    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}


/** @interface_method_impl{PDMIMEDIA,pfnWrite} */
static DECLCALLBACK(int) drvvdWrite(PPDMIMEDIA pInterface,
                                    uint64_t off, const void *pvBuf,
                                    size_t cbWrite)
{
    LogFlowFunc(("off=%#llx pvBuf=%p cbWrite=%d\n", off, pvBuf, cbWrite));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);
    Log2(("%s: off=%#llx pvBuf=%p cbWrite=%d\n%.*Rhxd\n", __FUNCTION__,
          off, pvBuf, cbWrite, cbWrite, pvBuf));

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
    {
        AssertMsgFailed(("Invalid state! Not mounted!\n"));
        return VERR_PDM_MEDIA_NOT_MOUNTED;
    }

    /* Set an FTM checkpoint as this operation changes the state permanently. */
    PDMDrvHlpFTSetCheckpoint(pThis->pDrvIns, FTMCHECKPOINTTYPE_STORAGE);

    int rc = drvvdKeyCheckPrereqs(pThis);
    if (RT_FAILURE(rc))
        return rc;

    /* Invalidate any buffer if boot acceleration is enabled. */
    if (pThis->fBootAccelActive)
    {
        pThis->cbDataValid = 0;
        pThis->offDisk     = 0;
    }

    rc = VDWrite(pThis->pDisk, off, pvBuf, cbWrite);
#ifdef VBOX_PERIODIC_FLUSH
    if (pThis->cbFlushInterval)
    {
        pThis->cbDataWritten += (uint32_t)cbWrite;
        if (pThis->cbDataWritten > pThis->cbFlushInterval)
        {
            pThis->cbDataWritten = 0;
            VDFlush(pThis->pDisk);
        }
    }
#endif /* VBOX_PERIODIC_FLUSH */

    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}

/** @interface_method_impl{PDMIMEDIA,pfnFlush} */
static DECLCALLBACK(int) drvvdFlush(PPDMIMEDIA pInterface)
{
    LogFlowFunc(("\n"));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
    {
        AssertMsgFailed(("Invalid state! Not mounted!\n"));
        return VERR_PDM_MEDIA_NOT_MOUNTED;
    }

#ifdef VBOX_IGNORE_FLUSH
    if (pThis->fIgnoreFlush)
        return VINF_SUCCESS;
#endif /* VBOX_IGNORE_FLUSH */

    int rc = VDFlush(pThis->pDisk);
    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}

/** @interface_method_impl{PDMIMEDIA,pfnMerge} */
static DECLCALLBACK(int) drvvdMerge(PPDMIMEDIA pInterface,
                                    PFNSIMPLEPROGRESS pfnProgress,
                                    void *pvUser)
{
    LogFlowFunc(("\n"));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);
    int rc = VINF_SUCCESS;

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
    {
        AssertMsgFailed(("Invalid state! Not mounted!\n"));
        return VERR_PDM_MEDIA_NOT_MOUNTED;
    }

    /* Note: There is an unavoidable race between destruction and another
     * thread invoking this function. This is handled safely and gracefully by
     * atomically invalidating the lock handle in drvvdDestruct. */
    int rc2 = RTSemFastMutexRequest(pThis->MergeCompleteMutex);
    AssertRC(rc2);
    if (RT_SUCCESS(rc2) && pThis->fMergePending)
    {
        /* Take shortcut: PFNSIMPLEPROGRESS is exactly the same type as
         * PFNVDPROGRESS, so there's no need for a conversion function. */
        /** @todo maybe introduce a conversion which limits update frequency. */
        PVDINTERFACE pVDIfsOperation = NULL;
        VDINTERFACEPROGRESS VDIfProgress;
        VDIfProgress.pfnProgress  = pfnProgress;
        rc2 = VDInterfaceAdd(&VDIfProgress.Core, "DrvVD_VDIProgress", VDINTERFACETYPE_PROGRESS,
                             pvUser, sizeof(VDINTERFACEPROGRESS), &pVDIfsOperation);
        AssertRC(rc2);
        pThis->fMergePending = false;
        rc = VDMerge(pThis->pDisk, pThis->uMergeSource,
                     pThis->uMergeTarget, pVDIfsOperation);
    }
    rc2 = RTSemFastMutexRelease(pThis->MergeCompleteMutex);
    AssertRC(rc2);
    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}

/** @interface_method_impl{PDMIMEDIA,pfnSetSecKeyIf} */
static DECLCALLBACK(int) drvvdSetSecKeyIf(PPDMIMEDIA pInterface, PPDMISECKEY pIfSecKey, PPDMISECKEYHLP pIfSecKeyHlp)
{
    LogFlowFunc(("\n"));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);
    int rc = VINF_SUCCESS;

    if (pThis->pCfgCrypto)
    {
        PVDINTERFACE pVDIfFilter = NULL;

        pThis->pIfSecKeyHlp = pIfSecKeyHlp;

        if (   pThis->pIfSecKey
            && !pIfSecKey)
        {
            /* Unload the crypto filter first to make sure it doesn't access the keys anymore. */
            rc = VDFilterRemove(pThis->pDisk, VD_FILTER_FLAGS_DEFAULT);
            AssertRC(rc);

            pThis->pIfSecKey = NULL;
        }

        if (   pIfSecKey
            && RT_SUCCESS(rc))
        {
            pThis->pIfSecKey = pIfSecKey;

            rc = VDInterfaceAdd(&pThis->VDIfCfg.Core, "DrvVD_Config", VDINTERFACETYPE_CONFIG,
                                pThis->pCfgCrypto, sizeof(VDINTERFACECONFIG), &pVDIfFilter);
            AssertRC(rc);

            rc = VDInterfaceAdd(&pThis->VDIfCrypto.Core, "DrvVD_Crypto", VDINTERFACETYPE_CRYPTO,
                                pThis, sizeof(VDINTERFACECRYPTO), &pVDIfFilter);
            AssertRC(rc);

            /* Load the crypt filter plugin. */
            rc = VDFilterAdd(pThis->pDisk, "CRYPT", VD_FILTER_FLAGS_DEFAULT, pVDIfFilter);
            if (RT_FAILURE(rc))
                pThis->pIfSecKey = NULL;
        }
    }
    else
        rc = VERR_NOT_SUPPORTED;

    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}

/** @interface_method_impl{PDMIMEDIA,pfnGetSize} */
static DECLCALLBACK(uint64_t) drvvdGetSize(PPDMIMEDIA pInterface)
{
    LogFlowFunc(("\n"));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
        return 0;

    uint64_t cb = VDGetSize(pThis->pDisk, VD_LAST_IMAGE);
    LogFlowFunc(("returns %#llx (%llu)\n", cb, cb));
    return cb;
}

/** @interface_method_impl{PDMIMEDIA,pfnGetSectorSize} */
static DECLCALLBACK(uint32_t) drvvdGetSectorSize(PPDMIMEDIA pInterface)
{
    LogFlowFunc(("\n"));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
        return 0;

    uint32_t cb = VDGetSectorSize(pThis->pDisk, VD_LAST_IMAGE);
    LogFlowFunc(("returns %u\n", cb));
    return cb;
}

/** @interface_method_impl{PDMIMEDIA,pfnIsReadOnly} */
static DECLCALLBACK(bool) drvvdIsReadOnly(PPDMIMEDIA pInterface)
{
    LogFlowFunc(("\n"));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
        return false;

    bool f = VDIsReadOnly(pThis->pDisk);
    LogFlowFunc(("returns %d\n", f));
    return f;
}

/** @interface_method_impl{PDMIMEDIA,pfnBiosGetPCHSGeometry} */
static DECLCALLBACK(int) drvvdBiosGetPCHSGeometry(PPDMIMEDIA pInterface,
                                                  PPDMMEDIAGEOMETRY pPCHSGeometry)
{
    LogFlowFunc(("\n"));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);
    VDGEOMETRY geo;

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
        return VERR_PDM_MEDIA_NOT_MOUNTED;

    /*
     * Use configured/cached values if present.
     */
    if (    pThis->PCHSGeometry.cCylinders > 0
        &&  pThis->PCHSGeometry.cHeads > 0
        &&  pThis->PCHSGeometry.cSectors > 0)
    {
        *pPCHSGeometry = pThis->PCHSGeometry;
        LogFlow(("%s: returns VINF_SUCCESS {%d,%d,%d}\n", __FUNCTION__, pThis->PCHSGeometry.cCylinders, pThis->PCHSGeometry.cHeads, pThis->PCHSGeometry.cSectors));
        return VINF_SUCCESS;
    }

    int rc = VDGetPCHSGeometry(pThis->pDisk, VD_LAST_IMAGE, &geo);
    if (RT_SUCCESS(rc))
    {
        pPCHSGeometry->cCylinders = geo.cCylinders;
        pPCHSGeometry->cHeads = geo.cHeads;
        pPCHSGeometry->cSectors = geo.cSectors;
        pThis->PCHSGeometry = *pPCHSGeometry;
    }
    else
    {
        LogFunc(("geometry not available.\n"));
        rc = VERR_PDM_GEOMETRY_NOT_SET;
    }
    LogFlowFunc(("returns %Rrc (CHS=%d/%d/%d)\n",
                 rc, pPCHSGeometry->cCylinders, pPCHSGeometry->cHeads, pPCHSGeometry->cSectors));
    return rc;
}

/** @interface_method_impl{PDMIMEDIA,pfnBiosSetPCHSGeometry} */
static DECLCALLBACK(int) drvvdBiosSetPCHSGeometry(PPDMIMEDIA pInterface,
                                                  PCPDMMEDIAGEOMETRY pPCHSGeometry)
{
    LogFlowFunc(("CHS=%d/%d/%d\n",
                 pPCHSGeometry->cCylinders, pPCHSGeometry->cHeads, pPCHSGeometry->cSectors));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);
    VDGEOMETRY geo;

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
    {
        AssertMsgFailed(("Invalid state! Not mounted!\n"));
        return VERR_PDM_MEDIA_NOT_MOUNTED;
    }

    geo.cCylinders = pPCHSGeometry->cCylinders;
    geo.cHeads = pPCHSGeometry->cHeads;
    geo.cSectors = pPCHSGeometry->cSectors;
    int rc = VDSetPCHSGeometry(pThis->pDisk, VD_LAST_IMAGE, &geo);
    if (rc == VERR_VD_GEOMETRY_NOT_SET)
        rc = VERR_PDM_GEOMETRY_NOT_SET;
    if (RT_SUCCESS(rc))
        pThis->PCHSGeometry = *pPCHSGeometry;
    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}

/** @interface_method_impl{PDMIMEDIA,pfnBiosGetLCHSGeometry} */
static DECLCALLBACK(int) drvvdBiosGetLCHSGeometry(PPDMIMEDIA pInterface,
                                                  PPDMMEDIAGEOMETRY pLCHSGeometry)
{
    LogFlowFunc(("\n"));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);
    VDGEOMETRY geo;

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
        return VERR_PDM_MEDIA_NOT_MOUNTED;

    /*
     * Use configured/cached values if present.
     */
    if (    pThis->LCHSGeometry.cCylinders > 0
        &&  pThis->LCHSGeometry.cHeads > 0
        &&  pThis->LCHSGeometry.cSectors > 0)
    {
        *pLCHSGeometry = pThis->LCHSGeometry;
        LogFlow(("%s: returns VINF_SUCCESS {%d,%d,%d}\n", __FUNCTION__, pThis->LCHSGeometry.cCylinders, pThis->LCHSGeometry.cHeads, pThis->LCHSGeometry.cSectors));
        return VINF_SUCCESS;
    }

    int rc = VDGetLCHSGeometry(pThis->pDisk, VD_LAST_IMAGE, &geo);
    if (RT_SUCCESS(rc))
    {
        pLCHSGeometry->cCylinders = geo.cCylinders;
        pLCHSGeometry->cHeads = geo.cHeads;
        pLCHSGeometry->cSectors = geo.cSectors;
        pThis->LCHSGeometry = *pLCHSGeometry;
    }
    else
    {
        LogFunc(("geometry not available.\n"));
        rc = VERR_PDM_GEOMETRY_NOT_SET;
    }
    LogFlowFunc(("returns %Rrc (CHS=%d/%d/%d)\n",
                 rc, pLCHSGeometry->cCylinders, pLCHSGeometry->cHeads, pLCHSGeometry->cSectors));
    return rc;
}

/** @interface_method_impl{PDMIMEDIA,pfnBiosSetLCHSGeometry} */
static DECLCALLBACK(int) drvvdBiosSetLCHSGeometry(PPDMIMEDIA pInterface,
                                                  PCPDMMEDIAGEOMETRY pLCHSGeometry)
{
    LogFlowFunc(("CHS=%d/%d/%d\n",
                 pLCHSGeometry->cCylinders, pLCHSGeometry->cHeads, pLCHSGeometry->cSectors));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);
    VDGEOMETRY geo;

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
    {
        AssertMsgFailed(("Invalid state! Not mounted!\n"));
        return VERR_PDM_MEDIA_NOT_MOUNTED;
    }

    geo.cCylinders = pLCHSGeometry->cCylinders;
    geo.cHeads = pLCHSGeometry->cHeads;
    geo.cSectors = pLCHSGeometry->cSectors;
    int rc = VDSetLCHSGeometry(pThis->pDisk, VD_LAST_IMAGE, &geo);
    if (rc == VERR_VD_GEOMETRY_NOT_SET)
        rc = VERR_PDM_GEOMETRY_NOT_SET;
    if (RT_SUCCESS(rc))
        pThis->LCHSGeometry = *pLCHSGeometry;
    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}

/** @interface_method_impl{PDMIMEDIA,pfnBiosIsVisible} */
static DECLCALLBACK(bool) drvvdBiosIsVisible(PPDMIMEDIA pInterface)
{
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);
    LogFlow(("drvvdBiosIsVisible: returns %d\n", pThis->fBiosVisible));
    return pThis->fBiosVisible;
}

/** @interface_method_impl{PDMIMEDIA,pfnGetType} */
static DECLCALLBACK(PDMMEDIATYPE) drvvdGetType(PPDMIMEDIA pInterface)
{
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);
    LogFlow(("drvvdBiosIsVisible: returns %d\n", pThis->fBiosVisible));
    return pThis->enmType;
}

/** @interface_method_impl{PDMIMEDIA,pfnGetUuid} */
static DECLCALLBACK(int) drvvdGetUuid(PPDMIMEDIA pInterface, PRTUUID pUuid)
{
    LogFlowFunc(("\n"));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);

    /*
     * Copy the uuid.
     */
    *pUuid = pThis->Uuid;
    LogFlowFunc(("returns {%RTuuid}\n", pUuid));
    return VINF_SUCCESS;
}

static DECLCALLBACK(int) drvvdDiscard(PPDMIMEDIA pInterface, PCRTRANGE paRanges, unsigned cRanges)
{
    LogFlowFunc(("\n"));
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);

    int rc = VDDiscardRanges(pThis->pDisk, paRanges, cRanges);
    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}

/** @interface_method_impl{PDMIMEDIA,pfnIoBufAlloc} */
static DECLCALLBACK(int) drvvdIoBufAlloc(PPDMIMEDIA pInterface, size_t cb, void **ppvNew)
{
    LogFlowFunc(("\n"));
    int rc;
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);

    /* Configured encryption requires locked down memory. */
    if (pThis->pCfgCrypto)
        rc = RTMemSaferAllocZEx(ppvNew, cb, RTMEMSAFER_F_REQUIRE_NOT_PAGABLE);
    else
    {
        cb = RT_ALIGN_Z(cb, _4K);
        void *pvNew = RTMemPageAlloc(cb);
        if (RT_LIKELY(pvNew))
        {
            *ppvNew = pvNew;
            rc = VINF_SUCCESS;
        }
        else
            rc = VERR_NO_MEMORY;
    }

    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}

/** @interface_method_impl{PDMIMEDIA,pfnIoBufFree} */
static DECLCALLBACK(int) drvvdIoBufFree(PPDMIMEDIA pInterface, void *pv, size_t cb)
{
    LogFlowFunc(("\n"));
    int rc = VINF_SUCCESS;
    PVBOXDISK pThis = PDMIMEDIA_2_VBOXDISK(pInterface);

    if (pThis->pCfgCrypto)
        RTMemSaferFree(pv, cb);
    else
    {
        cb = RT_ALIGN_Z(cb, _4K);
        RTMemPageFree(pv, cb);
    }

    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}


/* -=-=-=-=- IMount -=-=-=-=- */

/** @copydoc PDMIMOUNT::pfnUnmount */
static DECLCALLBACK(int) drvvdUnmount(PPDMIMOUNT pInterface, bool fForce, bool fEject)
{
    PVBOXDISK pThis = RT_FROM_MEMBER(pInterface, VBOXDISK, IMount);

    /*
     * Validate state.
     */
    if (!pThis->pDisk)
    {
        Log(("drvvdUnmount: Not mounted\n"));
        return VERR_PDM_MEDIA_NOT_MOUNTED;
    }
    if (pThis->fLocked && !fForce)
    {
        Log(("drvvdUnmount: Locked\n"));
        return VERR_PDM_MEDIA_LOCKED;
    }

    /* Media is no longer locked even if it was previously. */
    pThis->fLocked = false;

    /*
     * Notify driver/device above us.
     */
    if (pThis->pDrvMountNotify)
        pThis->pDrvMountNotify->pfnUnmountNotify(pThis->pDrvMountNotify);
    Log(("drvblockUnmount: success\n"));
    return VINF_SUCCESS;
}


/** @copydoc PDMIMOUNT::pfnIsMounted */
static DECLCALLBACK(bool) drvvdIsMounted(PPDMIMOUNT pInterface)
{
    PVBOXDISK pThis = RT_FROM_MEMBER(pInterface, VBOXDISK, IMount);
    return pThis->pDisk != NULL;
}

/** @copydoc PDMIMOUNT::pfnLock */
static DECLCALLBACK(int) drvvdLock(PPDMIMOUNT pInterface)
{
    PVBOXDISK pThis = RT_FROM_MEMBER(pInterface, VBOXDISK, IMount);
    Log(("drvblockLock: %d -> %d\n", pThis->fLocked, true));
    pThis->fLocked = true;
    return VINF_SUCCESS;
}

/** @copydoc PDMIMOUNT::pfnUnlock */
static DECLCALLBACK(int) drvvdUnlock(PPDMIMOUNT pInterface)
{
    PVBOXDISK pThis = RT_FROM_MEMBER(pInterface, VBOXDISK, IMount);
    Log(("drvblockUnlock: %d -> %d\n", pThis->fLocked, false));
    pThis->fLocked = false;
    return VINF_SUCCESS;
}

/** @copydoc PDMIMOUNT::pfnIsLocked */
static DECLCALLBACK(bool) drvvdIsLocked(PPDMIMOUNT pInterface)
{
    PVBOXDISK pThis = RT_FROM_MEMBER(pInterface, VBOXDISK, IMount);
    return pThis->fLocked;
}

/*********************************************************************************************************************************
*   Async Media interface methods                                                                                                *
*********************************************************************************************************************************/

static DECLCALLBACK(void) drvvdAsyncReqComplete(void *pvUser1, void *pvUser2, int rcReq)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser1;

    if (!pThis->pBlkCache)
    {
        int rc = pThis->pDrvMediaAsyncPort->pfnTransferCompleteNotify(pThis->pDrvMediaAsyncPort,
                                                                      pvUser2, rcReq);
        AssertRC(rc);
    }
    else
        PDMR3BlkCacheIoXferComplete(pThis->pBlkCache, (PPDMBLKCACHEIOXFER)pvUser2, rcReq);
}

static DECLCALLBACK(int) drvvdStartRead(PPDMIMEDIAASYNC pInterface, uint64_t uOffset,
                                        PCRTSGSEG paSeg, unsigned cSeg,
                                        size_t cbRead, void *pvUser)
{
    LogFlowFunc(("uOffset=%#llx paSeg=%#p cSeg=%u cbRead=%d pvUser=%#p\n",
                 uOffset, paSeg, cSeg, cbRead, pvUser));
    int rc = VINF_SUCCESS;
    PVBOXDISK pThis = PDMIMEDIAASYNC_2_VBOXDISK(pInterface);

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
    {
        AssertMsgFailed(("Invalid state! Not mounted!\n"));
        return VERR_PDM_MEDIA_NOT_MOUNTED;
    }

    rc = drvvdKeyCheckPrereqs(pThis);
    if (RT_FAILURE(rc))
        return rc;

    pThis->fBootAccelActive = false;

    RTSGBUF SgBuf;
    RTSgBufInit(&SgBuf, paSeg, cSeg);
    if (!pThis->pBlkCache)
        rc = VDAsyncRead(pThis->pDisk, uOffset, cbRead, &SgBuf,
                         drvvdAsyncReqComplete, pThis, pvUser);
    else
    {
        rc = PDMR3BlkCacheRead(pThis->pBlkCache, uOffset, &SgBuf, cbRead, pvUser);
        if (rc == VINF_AIO_TASK_PENDING)
            rc = VERR_VD_ASYNC_IO_IN_PROGRESS;
        else if (rc == VINF_SUCCESS)
            rc = VINF_VD_ASYNC_IO_FINISHED;
    }

    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}

static DECLCALLBACK(int) drvvdStartWrite(PPDMIMEDIAASYNC pInterface, uint64_t uOffset,
                                         PCRTSGSEG paSeg, unsigned cSeg,
                                         size_t cbWrite, void *pvUser)
{
    LogFlowFunc(("uOffset=%#llx paSeg=%#p cSeg=%u cbWrite=%d pvUser=%#p\n",
                 uOffset, paSeg, cSeg, cbWrite, pvUser));
    int rc = VINF_SUCCESS;
    PVBOXDISK pThis = PDMIMEDIAASYNC_2_VBOXDISK(pInterface);

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
    {
        AssertMsgFailed(("Invalid state! Not mounted!\n"));
        return VERR_PDM_MEDIA_NOT_MOUNTED;
    }

    rc = drvvdKeyCheckPrereqs(pThis);
    if (RT_FAILURE(rc))
        return rc;

    pThis->fBootAccelActive = false;

    RTSGBUF SgBuf;
    RTSgBufInit(&SgBuf, paSeg, cSeg);

    if (!pThis->pBlkCache)
        rc = VDAsyncWrite(pThis->pDisk, uOffset, cbWrite, &SgBuf,
                          drvvdAsyncReqComplete, pThis, pvUser);
    else
    {
        rc = PDMR3BlkCacheWrite(pThis->pBlkCache, uOffset, &SgBuf, cbWrite, pvUser);
        if (rc == VINF_AIO_TASK_PENDING)
            rc = VERR_VD_ASYNC_IO_IN_PROGRESS;
        else if (rc == VINF_SUCCESS)
            rc = VINF_VD_ASYNC_IO_FINISHED;
    }

    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}

static DECLCALLBACK(int) drvvdStartFlush(PPDMIMEDIAASYNC pInterface, void *pvUser)
{
    LogFlowFunc(("pvUser=%#p\n", pvUser));
    int rc = VINF_SUCCESS;
    PVBOXDISK pThis = PDMIMEDIAASYNC_2_VBOXDISK(pInterface);

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
    {
        AssertMsgFailed(("Invalid state! Not mounted!\n"));
        return VERR_PDM_MEDIA_NOT_MOUNTED;
    }

#ifdef VBOX_IGNORE_FLUSH
    if (pThis->fIgnoreFlushAsync)
        return VINF_VD_ASYNC_IO_FINISHED;
#endif /* VBOX_IGNORE_FLUSH */

    if (!pThis->pBlkCache)
        rc = VDAsyncFlush(pThis->pDisk, drvvdAsyncReqComplete, pThis, pvUser);
    else
    {
        rc = PDMR3BlkCacheFlush(pThis->pBlkCache, pvUser);
        if (rc == VINF_AIO_TASK_PENDING)
            rc = VERR_VD_ASYNC_IO_IN_PROGRESS;
        else if (rc == VINF_SUCCESS)
            rc = VINF_VD_ASYNC_IO_FINISHED;
    }
    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}

static DECLCALLBACK(int) drvvdStartDiscard(PPDMIMEDIAASYNC pInterface, PCRTRANGE paRanges,
                                           unsigned cRanges, void *pvUser)
{
    int rc = VINF_SUCCESS;
    PVBOXDISK pThis = PDMIMEDIAASYNC_2_VBOXDISK(pInterface);

    LogFlowFunc(("paRanges=%#p cRanges=%u pvUser=%#p\n",
                 paRanges, cRanges, pvUser));

    /*
     * Check the state.
     */
    if (!pThis->pDisk)
    {
        AssertMsgFailed(("Invalid state! Not mounted!\n"));
        return VERR_PDM_MEDIA_NOT_MOUNTED;
    }

    if (!pThis->pBlkCache)
        rc = VDAsyncDiscardRanges(pThis->pDisk, paRanges, cRanges, drvvdAsyncReqComplete,
                                  pThis, pvUser);
    else
    {
        rc = PDMR3BlkCacheDiscard(pThis->pBlkCache, paRanges, cRanges, pvUser);
        if (rc == VINF_AIO_TASK_PENDING)
            rc = VERR_VD_ASYNC_IO_IN_PROGRESS;
        else if (rc == VINF_SUCCESS)
            rc = VINF_VD_ASYNC_IO_FINISHED;
    }
    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}

/** @copydoc FNPDMBLKCACHEXFERCOMPLETEDRV */
static DECLCALLBACK(void) drvvdBlkCacheXferComplete(PPDMDRVINS pDrvIns, void *pvUser, int rcReq)
{
    PVBOXDISK pThis = PDMINS_2_DATA(pDrvIns, PVBOXDISK);

    int rc = pThis->pDrvMediaAsyncPort->pfnTransferCompleteNotify(pThis->pDrvMediaAsyncPort,
                                                                  pvUser, rcReq);
    AssertRC(rc);
}

/** @copydoc FNPDMBLKCACHEXFERENQUEUEDRV */
static DECLCALLBACK(int) drvvdBlkCacheXferEnqueue(PPDMDRVINS pDrvIns,
                                                  PDMBLKCACHEXFERDIR enmXferDir,
                                                  uint64_t off, size_t cbXfer,
                                                  PCRTSGBUF pcSgBuf, PPDMBLKCACHEIOXFER hIoXfer)
{
    int rc = VINF_SUCCESS;
    PVBOXDISK pThis = PDMINS_2_DATA(pDrvIns, PVBOXDISK);

    Assert (!pThis->pCfgCrypto);

    switch (enmXferDir)
    {
        case PDMBLKCACHEXFERDIR_READ:
            rc = VDAsyncRead(pThis->pDisk, off, cbXfer, pcSgBuf, drvvdAsyncReqComplete,
                             pThis, hIoXfer);
            break;
        case PDMBLKCACHEXFERDIR_WRITE:
            rc = VDAsyncWrite(pThis->pDisk, off, cbXfer, pcSgBuf, drvvdAsyncReqComplete,
                              pThis, hIoXfer);
            break;
        case PDMBLKCACHEXFERDIR_FLUSH:
            rc = VDAsyncFlush(pThis->pDisk, drvvdAsyncReqComplete, pThis, hIoXfer);
            break;
        default:
            AssertMsgFailed(("Invalid transfer type %d\n", enmXferDir));
            rc = VERR_INVALID_PARAMETER;
    }

    if (rc == VINF_VD_ASYNC_IO_FINISHED)
        PDMR3BlkCacheIoXferComplete(pThis->pBlkCache, hIoXfer, VINF_SUCCESS);
    else if (RT_FAILURE(rc) && rc != VERR_VD_ASYNC_IO_IN_PROGRESS)
        PDMR3BlkCacheIoXferComplete(pThis->pBlkCache, hIoXfer, rc);

    return VINF_SUCCESS;
}

/** @copydoc FNPDMBLKCACHEXFERENQUEUEDISCARDDRV */
static DECLCALLBACK(int) drvvdBlkCacheXferEnqueueDiscard(PPDMDRVINS pDrvIns, PCRTRANGE paRanges,
                                                         unsigned cRanges, PPDMBLKCACHEIOXFER hIoXfer)
{
    int rc = VINF_SUCCESS;
    PVBOXDISK pThis = PDMINS_2_DATA(pDrvIns, PVBOXDISK);

    rc = VDAsyncDiscardRanges(pThis->pDisk, paRanges, cRanges,
                              drvvdAsyncReqComplete, pThis, hIoXfer);

    if (rc == VINF_VD_ASYNC_IO_FINISHED)
        PDMR3BlkCacheIoXferComplete(pThis->pBlkCache, hIoXfer, VINF_SUCCESS);
    else if (RT_FAILURE(rc) && rc != VERR_VD_ASYNC_IO_IN_PROGRESS)
        PDMR3BlkCacheIoXferComplete(pThis->pBlkCache, hIoXfer, rc);

    return VINF_SUCCESS;
}

/*********************************************************************************************************************************
*   Extended media interface methods                                                                                             *
*********************************************************************************************************************************/

/**
 * Syncs the memory buffers between the I/O request allocator and the internal buffer.
 *
 * @returns VBox status code.
 * @param   pThis     VBox disk container instance data.
 * @param   pIoReq    I/O request to sync.
 * @param   fToIoBuf  Flag indicating the sync direction.
 *                    true to copy data from the allocators buffer to our internal buffer.
 *                    false for the other direction.
 */
DECLINLINE(int) drvvdMediaExIoReqBufSync(PVBOXDISK pThis, PPDMMEDIAEXIOREQINT pIoReq, bool fToIoBuf)
{
    if (fToIoBuf)
        return pThis->pDrvMediaExPort->pfnIoReqCopyToBuf(pThis->pDrvMediaExPort, pIoReq, &pIoReq->abAlloc[0],
                                                         0, pIoReq->DataSeg.pvSeg, pIoReq->DataSeg.cbSeg);
    else
        return pThis->pDrvMediaExPort->pfnIoReqCopyFromBuf(pThis->pDrvMediaExPort, pIoReq, &pIoReq->abAlloc[0],
                                                           0, pIoReq->DataSeg.pvSeg, pIoReq->DataSeg.cbSeg);
}

/**
 * Hashes the I/O request ID to an index for the allocated I/O request bin.
 */
DECLINLINE(unsigned) drvvdMediaExIoReqIdHash(PDMMEDIAEXIOREQID uIoReqId)
{
    return uIoReqId % DRVVD_VDIOREQ_ALLOC_BINS; /** @todo: Find something better? */
}

/**
 * Inserts the given I/O request in to the list of allocated I/O requests.
 *
 * @returns VBox status code.
 * @param   pThis     VBox disk container instance data.
 * @param   pIoReq    I/O request to insert.
 */
static int drvvdMediaExIoReqInsert(PVBOXDISK pThis, PPDMMEDIAEXIOREQINT pIoReq)
{
    int rc = VINF_SUCCESS;
    unsigned idxBin = drvvdMediaExIoReqIdHash(pIoReq->uIoReqId);

    rc = RTSemFastMutexRequest(pThis->aIoReqAllocBins[idxBin].hMtxLstIoReqAlloc);
    if (RT_SUCCESS(rc))
    {
        /* Search for conflicting I/O request ID. */
        PPDMMEDIAEXIOREQINT pIt;
        RTListForEach(&pThis->aIoReqAllocBins[idxBin].LstIoReqAlloc, pIt, PDMMEDIAEXIOREQINT, NdAllocatedList)
        {
            if (RT_UNLIKELY(pIt->uIoReqId == pIoReq->uIoReqId))
            {
                rc = VERR_PDM_MEDIAEX_IOREQID_CONFLICT;
                break;
            }
        }
        if (RT_SUCCESS(rc))
            RTListAppend(&pThis->aIoReqAllocBins[idxBin].LstIoReqAlloc, &pIoReq->NdAllocatedList);
        RTSemFastMutexRelease(pThis->aIoReqAllocBins[idxBin].hMtxLstIoReqAlloc);
    }

    return rc;
}

/**
 * Removes the given I/O request from the list of allocated I/O requests.
 *
 * @returns VBox status code.
 * @param   pThis     VBox disk container instance data.
 * @param   pIoReq    I/O request to insert.
 */
static int drvvdMediaExIoReqRemove(PVBOXDISK pThis, PPDMMEDIAEXIOREQINT pIoReq)
{
    int rc = VINF_SUCCESS;
    unsigned idxBin = drvvdMediaExIoReqIdHash(pIoReq->uIoReqId);

    rc = RTSemFastMutexRequest(pThis->aIoReqAllocBins[idxBin].hMtxLstIoReqAlloc);
    if (RT_SUCCESS(rc))
    {
        RTListNodeRemove(&pIoReq->NdAllocatedList);
        RTSemFastMutexRelease(pThis->aIoReqAllocBins[idxBin].hMtxLstIoReqAlloc);
    }

    return rc;
}

/**
 * Allocates a memory buffer suitable for I/O for the given request.
 *
 * @returns VBox status code.
 * @param   pThis     VBox disk container instance data.
 * @param   pIoReq    I/O request to allocate memory for.
 * @param   cb        Size of the buffer.
 */
static int drvvdMediaExIoReqBufAlloc(PVBOXDISK pThis, PPDMMEDIAEXIOREQINT pIoReq, size_t cb)
{
    int rc = VINF_SUCCESS;
    void *pvBuf = NULL;

    /* Configured encryption requires locked down memory. */
    if (pThis->pCfgCrypto)
        rc = RTMemSaferAllocZEx(&pvBuf, cb, RTMEMSAFER_F_REQUIRE_NOT_PAGABLE);
    else
    {
        pvBuf = RTMemPageAlloc(RT_ALIGN_Z(cb, _4K));
        if (RT_UNLIKELY(!pvBuf))
            rc = VERR_NO_MEMORY;
    }

    if (RT_SUCCESS(rc))
    {
        pIoReq->DataSeg.pvSeg = pvBuf;
        pIoReq->DataSeg.cbSeg = cb;
        RTSgBufInit(&pIoReq->SgBuf, &pIoReq->DataSeg, 1);
    }

    return rc;
}

/**
 * Frees a I/O memory buffer allocated previously.
 *
 * @returns nothing.
 * @param   pThis     VBox disk container instance data.
 * @param   pIoReq    I/O request for which to free memory.
 */
static void drvvdMediaExIoReqBufFree(PVBOXDISK pThis, PPDMMEDIAEXIOREQINT pIoReq)
{
    if (pIoReq->DataSeg.pvSeg)
    {
        if (pThis->pCfgCrypto)
            RTMemSaferFree(pIoReq->DataSeg.pvSeg, pIoReq->DataSeg.cbSeg);
        else
        {
            size_t cb = RT_ALIGN_Z(pIoReq->DataSeg.cbSeg, _4K);
            RTMemPageFree(pIoReq->DataSeg.pvSeg, cb);
        }
    }
}

/**
 * I/O request completion worker.
 *
 * @returns VBox status code.
 * @param   pThis     VBox disk container instance data.
 * @param   pIoReq    I/O request to complete.
 * @param   rcReq     The status code the request completed with.
 * @param   fUpNotify Flag whether to notify the driver/device above us about the completion.
 */
static int drvvdMediaExIoReqCompleteWorker(PVBOXDISK pThis, PPDMMEDIAEXIOREQINT pIoReq, int rcReq, bool fUpNotify)
{
    int rc;
    bool fXchg = ASMAtomicCmpXchgU32((volatile uint32_t *)&pIoReq->enmState, VDIOREQSTATE_COMPLETING, VDIOREQSTATE_ACTIVE);
    if (fXchg)
    {
        ASMAtomicDecU32(&pThis->cIoReqsActive);

        if (   RT_SUCCESS(rcReq)
            && pIoReq->enmType == PDMMEDIAEXIOREQTYPE_READ)
        {
            /* Sync memory buffer with caller. */
            rc = drvvdMediaExIoReqBufSync(pThis, pIoReq, false /* fToIoBuf */);
            if (RT_FAILURE(rc))
                rcReq = rc;
        }
    }
    else
    {
        Assert(pIoReq->enmState == VDIOREQSTATE_CANCELED);
        rcReq = VERR_PDM_MEDIAEX_IOREQ_CANCELED;
    }

    ASMAtomicXchgU32((volatile uint32_t *)&pIoReq->enmState, VDIOREQSTATE_COMPLETED);

    if (fUpNotify)
    {
        rc = pThis->pDrvMediaExPort->pfnIoReqCompleteNotify(pThis->pDrvMediaExPort,
                                                            pIoReq, &pIoReq->abAlloc[0], rcReq);
        AssertRC(rc);
    }

    return rcReq;
}

/**
 * @copydoc FNVDASYNCTRANSFERCOMPLETE
 */
static DECLCALLBACK(void) drvvdMediaExIoReqComplete(void *pvUser1, void *pvUser2, int rcReq)
{
    PVBOXDISK pThis = (PVBOXDISK)pvUser1;
    PPDMMEDIAEXIOREQINT pIoReq = (PPDMMEDIAEXIOREQINT)pvUser2;

    drvvdMediaExIoReqCompleteWorker(pThis, pIoReq, rcReq, true /* fUpNotify */);
}

/**
 * @interface_method_impl{PDMIMEDIAEX,pfnIoReqAllocSizeSet}
 */
static DECLCALLBACK(int) drvvdIoReqAllocSizeSet(PPDMIMEDIAEX pInterface, size_t cbIoReqAlloc)
{
    PVBOXDISK pThis = RT_FROM_MEMBER(pInterface, VBOXDISK, IMediaEx);
    int rc = VINF_SUCCESS;

    if (RT_UNLIKELY(pThis->hIoReqCache != NIL_RTMEMCACHE))
        return VERR_INVALID_STATE;

    return RTMemCacheCreate(&pThis->hIoReqCache, sizeof(PDMMEDIAEXIOREQINT) + cbIoReqAlloc, 0, UINT32_MAX,
                            NULL, NULL, NULL, 0);
}

/**
 * @interface_method_impl{PDMIMEDIAEX,pfnIoReqAlloc}
 */
static DECLCALLBACK(int) drvvdIoReqAlloc(PPDMIMEDIAEX pInterface, PPDMMEDIAEXIOREQ phIoReq, void **ppvIoReqAlloc,
                                         PDMMEDIAEXIOREQID uIoReqId, uint32_t fFlags)
{
    PVBOXDISK pThis = RT_FROM_MEMBER(pInterface, VBOXDISK, IMediaEx);
    PPDMMEDIAEXIOREQINT pIoReq = (PPDMMEDIAEXIOREQINT)RTMemCacheAlloc(pThis->hIoReqCache);

    if (RT_UNLIKELY(!pIoReq))
        return VERR_NO_MEMORY;

    pIoReq->uIoReqId      = uIoReqId;
    pIoReq->fFlags        = fFlags;
    pIoReq->pDisk         = pThis;
    pIoReq->enmState      = VDIOREQSTATE_ALLOCATED;
    pIoReq->enmType       = PDMMEDIAEXIOREQTYPE_INVALID;
    pIoReq->DataSeg.pvSeg = NULL;
    pIoReq->DataSeg.cbSeg = 0;

    int rc = drvvdMediaExIoReqInsert(pThis, pIoReq);
    if (RT_SUCCESS(rc))
    {
        *phIoReq = pIoReq;
        *ppvIoReqAlloc = &pIoReq->abAlloc[0];
    }
    else
        RTMemCacheFree(pThis->hIoReqCache, pIoReq);

    return rc;
}

/**
 * @interface_method_impl{PDMIMEDIAEX,pfnIoReqFree}
 */
static DECLCALLBACK(int) drvvdIoReqFree(PPDMIMEDIAEX pInterface, PDMMEDIAEXIOREQ hIoReq)
{
    PVBOXDISK pThis = RT_FROM_MEMBER(pInterface, VBOXDISK, IMediaEx);
    PPDMMEDIAEXIOREQINT pIoReq = hIoReq;

    if (   pIoReq->enmState != VDIOREQSTATE_COMPLETED
        && pIoReq->enmState != VDIOREQSTATE_ALLOCATED)
        return VERR_PDM_MEDIAEX_IOREQ_INVALID_STATE;

    /* Remove from allocated list. */
    int rc = drvvdMediaExIoReqRemove(pThis, pIoReq);
    if (RT_FAILURE(rc))
        return rc;

    /* Free any associated I/O memory. */
    drvvdMediaExIoReqBufFree(pThis, pIoReq);

    pIoReq->enmState = VDIOREQSTATE_FREE;
    RTMemCacheFree(pThis->hIoReqCache, pIoReq);
    return VINF_SUCCESS;
}

/**
 * @interface_method_impl{PDMIMEDIAEX,pfnIoReqCancel}
 */
static DECLCALLBACK(int) drvvdIoReqCancel(PPDMIMEDIAEX pInterface, PDMMEDIAEXIOREQID uIoReqId)
{
    PVBOXDISK pThis = RT_FROM_MEMBER(pInterface, VBOXDISK, IMediaEx);
    unsigned idxBin = drvvdMediaExIoReqIdHash(uIoReqId);

    int rc = RTSemFastMutexRequest(pThis->aIoReqAllocBins[idxBin].hMtxLstIoReqAlloc);
    if (RT_SUCCESS(rc))
    {
        /* Search for I/O request with ID. */
        PPDMMEDIAEXIOREQINT pIt;
        rc = VERR_PDM_MEDIAEX_IOREQID_NOT_FOUND;

        RTListForEach(&pThis->aIoReqAllocBins[idxBin].LstIoReqAlloc, pIt, PDMMEDIAEXIOREQINT, NdAllocatedList)
        {
            if (pIt->uIoReqId == uIoReqId)
            {
                bool fXchg = true;
                VDIOREQSTATE enmStateOld = (VDIOREQSTATE)ASMAtomicReadU32((volatile uint32_t *)&pIt->enmState);

                /*
                 * We might have to try canceling the request multiple times if it transitioned from
                 * ALLOCATED to ACTIVE between reading the state and trying to change it.
                 */
                while (   (   enmStateOld == VDIOREQSTATE_ALLOCATED
                           || enmStateOld == VDIOREQSTATE_ACTIVE)
                       && !fXchg)
                {
                    fXchg = ASMAtomicCmpXchgU32((volatile uint32_t *)&pIt->enmState, VDIOREQSTATE_CANCELED, enmStateOld);
                    if (!fXchg)
                        enmStateOld = (VDIOREQSTATE)ASMAtomicReadU32((volatile uint32_t *)&pIt->enmState);
                }

                if (fXchg)
                {
                    ASMAtomicDecU32(&pThis->cIoReqsActive);
                    rc = VINF_SUCCESS;
                }
                break;
            }
        }
        RTSemFastMutexRelease(pThis->aIoReqAllocBins[idxBin].hMtxLstIoReqAlloc);
    }

    return rc;
}

/**
 * @interface_method_impl{PDMIMEDIAEX,pfnIoReqRead}
 */
static DECLCALLBACK(int) drvvdIoReqRead(PPDMIMEDIAEX pInterface, PDMMEDIAEXIOREQ hIoReq, uint64_t off, size_t cbRead)
{
    PVBOXDISK pThis = RT_FROM_MEMBER(pInterface, VBOXDISK, IMediaEx);
    PPDMMEDIAEXIOREQINT pIoReq = hIoReq;
    VDIOREQSTATE enmState = (VDIOREQSTATE)ASMAtomicReadU32((volatile uint32_t *)&pIoReq->enmState);

    if (RT_UNLIKELY(enmState == VDIOREQSTATE_CANCELED))
        return VERR_PDM_MEDIAEX_IOREQ_CANCELED;

    if (RT_UNLIKELY(enmState != VDIOREQSTATE_ALLOCATED))
        return VERR_PDM_MEDIAEX_IOREQ_INVALID_STATE;

    pIoReq->enmType = PDMMEDIAEXIOREQTYPE_READ;
    /* Allocate a suitable I/O buffer for this request. */
    int rc = drvvdMediaExIoReqBufAlloc(pThis, pIoReq, cbRead);
    if (RT_SUCCESS(rc))
    {
        bool fXchg = ASMAtomicCmpXchgU32((volatile uint32_t *)&pIoReq->enmState, VDIOREQSTATE_ACTIVE, VDIOREQSTATE_ALLOCATED);
        if (RT_UNLIKELY(!fXchg))
        {
            /* Must have been canceled inbetween. */
            Assert(pIoReq->enmState == VDIOREQSTATE_CANCELED);
            return VERR_PDM_MEDIAEX_IOREQ_CANCELED;
        }
        ASMAtomicIncU32(&pThis->cIoReqsActive);
        rc = VDAsyncRead(pThis->pDisk, off, cbRead, &pIoReq->SgBuf,
                         drvvdMediaExIoReqComplete, pThis, pIoReq);
        if (rc == VERR_VD_ASYNC_IO_IN_PROGRESS)
            rc = VINF_PDM_MEDIAEX_IOREQ_IN_PROGRESS;
        else if (rc == VINF_VD_ASYNC_IO_FINISHED)
            rc = VINF_SUCCESS;

        if (rc != VINF_PDM_MEDIAEX_IOREQ_IN_PROGRESS)
            rc = drvvdMediaExIoReqCompleteWorker(pThis, pIoReq, rc, false /* fUpNotify */);
    }

    return rc;
}

/**
 * @interface_method_impl{PDMIMEDIAEX,pfnIoReqWrite}
 */
static DECLCALLBACK(int) drvvdIoReqWrite(PPDMIMEDIAEX pInterface, PDMMEDIAEXIOREQ hIoReq, uint64_t off, size_t cbWrite)
{
    PVBOXDISK pThis = RT_FROM_MEMBER(pInterface, VBOXDISK, IMediaEx);
    PPDMMEDIAEXIOREQINT pIoReq = hIoReq;
    VDIOREQSTATE enmState = (VDIOREQSTATE)ASMAtomicReadU32((volatile uint32_t *)&pIoReq->enmState);

    if (RT_UNLIKELY(enmState == VDIOREQSTATE_CANCELED))
        return VERR_PDM_MEDIAEX_IOREQ_CANCELED;

    if (RT_UNLIKELY(enmState != VDIOREQSTATE_ALLOCATED))
        return VERR_PDM_MEDIAEX_IOREQ_INVALID_STATE;

    pIoReq->enmType = PDMMEDIAEXIOREQTYPE_WRITE;
    /* Allocate a suitable I/O buffer for this request. */
    int rc = drvvdMediaExIoReqBufAlloc(pThis, pIoReq, cbWrite);
    if (RT_SUCCESS(rc))
    {
        /* Sync memory buffer with caller. */
        rc = drvvdMediaExIoReqBufSync(pThis, pIoReq, true /* fToIoBuf */);
        if (RT_SUCCESS(rc))
        {
            bool fXchg = ASMAtomicCmpXchgU32((volatile uint32_t *)&pIoReq->enmState, VDIOREQSTATE_ACTIVE, VDIOREQSTATE_ALLOCATED);
            if (RT_UNLIKELY(!fXchg))
            {
                /* Must have been canceled inbetween. */
                Assert(pIoReq->enmState == VDIOREQSTATE_CANCELED);
                return VERR_PDM_MEDIAEX_IOREQ_CANCELED;
            }

            ASMAtomicIncU32(&pThis->cIoReqsActive);
            rc = VDAsyncWrite(pThis->pDisk, off, cbWrite, &pIoReq->SgBuf,
                              drvvdMediaExIoReqComplete, pThis, pIoReq);
            if (rc == VERR_VD_ASYNC_IO_IN_PROGRESS)
                rc = VINF_PDM_MEDIAEX_IOREQ_IN_PROGRESS;
            else if (rc == VINF_VD_ASYNC_IO_FINISHED)
                rc = VINF_SUCCESS;

            if (rc != VINF_PDM_MEDIAEX_IOREQ_IN_PROGRESS)
                rc = drvvdMediaExIoReqCompleteWorker(pThis, pIoReq, rc, false /* fUpNotify */);
        }
    }

    return rc;
}

/**
 * @interface_method_impl{PDMIMEDIAEX,pfnIoReqFlush}
 */
static DECLCALLBACK(int) drvvdIoReqFlush(PPDMIMEDIAEX pInterface, PDMMEDIAEXIOREQ hIoReq)
{
    PVBOXDISK pThis = RT_FROM_MEMBER(pInterface, VBOXDISK, IMediaEx);
    PPDMMEDIAEXIOREQINT pIoReq = hIoReq;
    VDIOREQSTATE enmState = (VDIOREQSTATE)ASMAtomicReadU32((volatile uint32_t *)&pIoReq->enmState);

    if (RT_UNLIKELY(enmState == VDIOREQSTATE_CANCELED))
        return VERR_PDM_MEDIAEX_IOREQ_CANCELED;

    if (RT_UNLIKELY(enmState != VDIOREQSTATE_ALLOCATED))
        return VERR_PDM_MEDIAEX_IOREQ_INVALID_STATE;

    pIoReq->enmType = PDMMEDIAEXIOREQTYPE_FLUSH;

    bool fXchg = ASMAtomicCmpXchgU32((volatile uint32_t *)&pIoReq->enmState, VDIOREQSTATE_ACTIVE, VDIOREQSTATE_ALLOCATED);
    if (RT_UNLIKELY(!fXchg))
    {
        /* Must have been canceled inbetween. */
        Assert(pIoReq->enmState == VDIOREQSTATE_CANCELED);
        return VERR_PDM_MEDIAEX_IOREQ_CANCELED;
    }

    ASMAtomicIncU32(&pThis->cIoReqsActive);
    int rc = VDAsyncFlush(pThis->pDisk, drvvdMediaExIoReqComplete, pThis, pIoReq);
    if (rc == VERR_VD_ASYNC_IO_IN_PROGRESS)
        rc = VINF_PDM_MEDIAEX_IOREQ_IN_PROGRESS;
    else if (rc == VINF_VD_ASYNC_IO_FINISHED)
        rc = VINF_SUCCESS;

    if (rc != VINF_PDM_MEDIAEX_IOREQ_IN_PROGRESS)
        rc = drvvdMediaExIoReqCompleteWorker(pThis, pIoReq, rc, false /* fUpNotify */);

    return rc;
}

/**
 * @interface_method_impl{PDMIMEDIAEX,pfnIoReqDiscard}
 */
static DECLCALLBACK(int) drvvdIoReqDiscard(PPDMIMEDIAEX pInterface, PDMMEDIAEXIOREQ hIoReq, PCRTRANGE paRanges, unsigned cRanges)
{
    PVBOXDISK pThis = RT_FROM_MEMBER(pInterface, VBOXDISK, IMediaEx);
    PPDMMEDIAEXIOREQINT pIoReq = hIoReq;
    VDIOREQSTATE enmState = (VDIOREQSTATE)ASMAtomicReadU32((volatile uint32_t *)&pIoReq->enmState);

    if (RT_UNLIKELY(enmState == VDIOREQSTATE_CANCELED))
        return VERR_PDM_MEDIAEX_IOREQ_CANCELED;

    if (RT_UNLIKELY(enmState != VDIOREQSTATE_ALLOCATED))
        return VERR_PDM_MEDIAEX_IOREQ_INVALID_STATE;

    pIoReq->enmType = PDMMEDIAEXIOREQTYPE_DISCARD;

    bool fXchg = ASMAtomicCmpXchgU32((volatile uint32_t *)&pIoReq->enmState, VDIOREQSTATE_ACTIVE, VDIOREQSTATE_ALLOCATED);
    if (RT_UNLIKELY(!fXchg))
    {
        /* Must have been canceled inbetween. */
        Assert(pIoReq->enmState == VDIOREQSTATE_CANCELED);
        return VERR_PDM_MEDIAEX_IOREQ_CANCELED;
    }

    ASMAtomicIncU32(&pThis->cIoReqsActive);
    int rc = VDAsyncDiscardRanges(pThis->pDisk, paRanges, cRanges,
                                  drvvdMediaExIoReqComplete, pThis, pIoReq);
    if (rc == VERR_VD_ASYNC_IO_IN_PROGRESS)
        rc = VINF_PDM_MEDIAEX_IOREQ_IN_PROGRESS;
    else if (rc == VINF_VD_ASYNC_IO_FINISHED)
        rc = VINF_SUCCESS;

    if (rc != VINF_PDM_MEDIAEX_IOREQ_IN_PROGRESS)
        rc = drvvdMediaExIoReqCompleteWorker(pThis, pIoReq, rc, false /* fUpNotify */);

    return rc;
}

/**
 * @interface_method_impl{PDMIMEDIAEX,pfnIoReqGetActiveCount}
 */
static DECLCALLBACK(uint32_t) drvvdIoReqGetActiveCount(PPDMIMEDIAEX pInterface)
{
    PVBOXDISK pThis = RT_FROM_MEMBER(pInterface, VBOXDISK, IMediaEx);
    return ASMAtomicReadU32(&pThis->cIoReqsActive);
}

/**
 * Loads all configured plugins.
 *
 * @returns VBox status code.
 * @param   pThis    The disk instance.
 * @param   pCfg     CFGM node holding plugin list.
 */
static int drvvdLoadPlugins(PVBOXDISK pThis, PCFGMNODE pCfg)
{
    int rc = VINF_SUCCESS;
    PCFGMNODE pCfgPlugins = CFGMR3GetChild(pCfg, "Plugins");

    if (pCfgPlugins)
    {
        PCFGMNODE pPluginCur = CFGMR3GetFirstChild(pCfgPlugins);
        while (   pPluginCur
               && RT_SUCCESS(rc))
        {
            char *pszPluginFilename = NULL;
            rc = CFGMR3QueryStringAlloc(pPluginCur, "Path", &pszPluginFilename);
            if (RT_SUCCESS(rc))
                rc = VDPluginLoadFromFilename(pszPluginFilename);

            pPluginCur = CFGMR3GetNextChild(pPluginCur);
        }
    }

    return rc;
}


/**
 * Sets up the disk filter chain.
 *
 * @returns VBox status code.
 * @param   pThis    The disk instance.
 * @param   pCfg     CFGM node holding the filter parameters.
 */
static int drvvdSetupFilters(PVBOXDISK pThis, PCFGMNODE pCfg)
{
    int rc = VINF_SUCCESS;
    PCFGMNODE pCfgFilter = CFGMR3GetChild(pCfg, "Filters");

    if (pCfgFilter)
    {
        PCFGMNODE pCfgFilterConfig = CFGMR3GetChild(pCfgFilter, "VDConfig");
        char *pszFilterName = NULL;
        VDINTERFACECONFIG VDIfConfig;
        PVDINTERFACE pVDIfsFilter = NULL;

        rc = CFGMR3QueryStringAlloc(pCfgFilter, "FilterName", &pszFilterName);
        if (RT_SUCCESS(rc))
        {
            VDIfConfig.pfnAreKeysValid = drvvdCfgAreKeysValid;
            VDIfConfig.pfnQuerySize    = drvvdCfgQuerySize;
            VDIfConfig.pfnQuery        = drvvdCfgQuery;
            VDIfConfig.pfnQueryBytes   = drvvdCfgQueryBytes;
            rc = VDInterfaceAdd(&VDIfConfig.Core, "DrvVD_Config", VDINTERFACETYPE_CONFIG,
                                pCfgFilterConfig, sizeof(VDINTERFACECONFIG), &pVDIfsFilter);
            AssertRC(rc);

            rc = VDFilterAdd(pThis->pDisk, pszFilterName, VD_FILTER_FLAGS_DEFAULT, pVDIfsFilter);

            MMR3HeapFree(pszFilterName);
        }
    }

    return rc;
}


/**
 * Translates a PDMMEDIATYPE value into a string.
 *
 * @returns Read only string.
 * @param   enmType             The type value.
 */
static const char *drvvdGetTypeName(PDMMEDIATYPE enmType)
{
    switch (enmType)
    {
        case PDMMEDIATYPE_ERROR:                return "ERROR";
        case PDMMEDIATYPE_FLOPPY_360:           return "FLOPPY_360";
        case PDMMEDIATYPE_FLOPPY_720:           return "FLOPPY_720";
        case PDMMEDIATYPE_FLOPPY_1_20:          return "FLOPPY_1_20";
        case PDMMEDIATYPE_FLOPPY_1_44:          return "FLOPPY_1_44";
        case PDMMEDIATYPE_FLOPPY_2_88:          return "FLOPPY_2_88";
        case PDMMEDIATYPE_FLOPPY_FAKE_15_6:     return "FLOPPY_FAKE_15_6";
        case PDMMEDIATYPE_FLOPPY_FAKE_63_5:     return "FLOPPY_FAKE_63_5";
        case PDMMEDIATYPE_CDROM:                return "CDROM";
        case PDMMEDIATYPE_DVD:                  return "DVD";
        case PDMMEDIATYPE_HARD_DISK:            return "HARD_DISK";
        default:                                return "Unknown";
    }
}

/**
 * Returns the appropriate PDMMEDIATYPE for t he given string.
 *
 * @returns PDMMEDIATYPE
 * @param   pszType    The string representation of the media type.
 */
static PDMMEDIATYPE drvvdGetMediaTypeFromString(const char *pszType)
{
    PDMMEDIATYPE enmType = PDMMEDIATYPE_ERROR;

    if (!strcmp(pszType, "HardDisk"))
        enmType = PDMMEDIATYPE_HARD_DISK;
    else if (!strcmp(pszType, "DVD"))
        enmType = PDMMEDIATYPE_DVD;
    else if (!strcmp(pszType, "CDROM"))
        enmType = PDMMEDIATYPE_CDROM;
    else if (!strcmp(pszType, "Floppy 2.88"))
        enmType = PDMMEDIATYPE_FLOPPY_2_88;
    else if (!strcmp(pszType, "Floppy 1.44"))
        enmType = PDMMEDIATYPE_FLOPPY_1_44;
    else if (!strcmp(pszType, "Floppy 1.20"))
        enmType = PDMMEDIATYPE_FLOPPY_1_20;
    else if (!strcmp(pszType, "Floppy 720"))
        enmType = PDMMEDIATYPE_FLOPPY_720;
    else if (!strcmp(pszType, "Floppy 360"))
        enmType = PDMMEDIATYPE_FLOPPY_360;
    else if (!strcmp(pszType, "Floppy 15.6"))
        enmType = PDMMEDIATYPE_FLOPPY_FAKE_15_6;
    else if (!strcmp(pszType, "Floppy 63.5"))
        enmType = PDMMEDIATYPE_FLOPPY_FAKE_63_5;

    return enmType;
}

/**
 * Converts PDMMEDIATYPE to the appropriate VDTYPE.
 *
 * @returns The VDTYPE.
 * @param   enmType    The PDMMEDIATYPE to convert from.
 */
static VDTYPE drvvdGetVDFromMediaType(PDMMEDIATYPE enmType)
{
    if (PDMMEDIATYPE_IS_FLOPPY(enmType))
        return VDTYPE_FLOPPY;
    else if (enmType == PDMMEDIATYPE_DVD || enmType == PDMMEDIATYPE_CDROM)
        return VDTYPE_DVD;
    else if (enmType == PDMMEDIATYPE_HARD_DISK)
        return VDTYPE_HDD;

    AssertMsgFailed(("Invalid media type %d{%s} given!\n", enmType, drvvdGetTypeName(enmType)));
    return VDTYPE_HDD;
}

/*********************************************************************************************************************************
*   Base interface methods                                                                                                       *
*********************************************************************************************************************************/

/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
static DECLCALLBACK(void *) drvvdQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PPDMDRVINS  pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PVBOXDISK   pThis   = PDMINS_2_DATA(pDrvIns, PVBOXDISK);

    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIMEDIA, &pThis->IMedia);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIMOUNT, pThis->fMountable ? &pThis->IMount : NULL);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIMEDIAASYNC, pThis->fAsyncIOSupported ? &pThis->IMediaAsync : NULL);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIMEDIAEX, pThis->pDrvMediaExPort ? &pThis->IMediaEx : NULL);
    return NULL;
}


/*********************************************************************************************************************************
*   Saved state notification methods                                                                                             *
*********************************************************************************************************************************/

/**
 * Load done callback for re-opening the image writable during teleportation.
 *
 * This is called both for successful and failed load runs, we only care about
 * successful ones.
 *
 * @returns VBox status code.
 * @param   pDrvIns         The driver instance.
 * @param   pSSM            The saved state handle.
 */
static DECLCALLBACK(int) drvvdLoadDone(PPDMDRVINS pDrvIns, PSSMHANDLE pSSM)
{
    PVBOXDISK pThis = PDMINS_2_DATA(pDrvIns, PVBOXDISK);
    Assert(!pThis->fErrorUseRuntime);

    /* Drop out if we don't have any work to do or if it's a failed load. */
    if (   !pThis->fTempReadOnly
        || RT_FAILURE(SSMR3HandleGetStatus(pSSM)))
        return VINF_SUCCESS;

    int rc = drvvdSetWritable(pThis);
    if (RT_FAILURE(rc)) /** @todo does the bugger set any errors? */
        return SSMR3SetLoadError(pSSM, rc, RT_SRC_POS,
                                 N_("Failed to write lock the images"));
    return VINF_SUCCESS;
}


/*********************************************************************************************************************************
*   Driver methods                                                                                                               *
*********************************************************************************************************************************/

/**
 * Worker for the power off or destruct callback.
 *
 * @returns nothing.
 * @param   pDrvIns    The driver instance.
 */
static void drvvdPowerOffOrDestruct(PPDMDRVINS pDrvIns)
{
    PVBOXDISK pThis = PDMINS_2_DATA(pDrvIns, PVBOXDISK);
    LogFlowFunc(("\n"));

    RTSEMFASTMUTEX mutex;
    ASMAtomicXchgHandle(&pThis->MergeCompleteMutex, NIL_RTSEMFASTMUTEX, &mutex);
    if (mutex != NIL_RTSEMFASTMUTEX)
    {
        /* Request the semaphore to wait until a potentially running merge
         * operation has been finished. */
        int rc = RTSemFastMutexRequest(mutex);
        AssertRC(rc);
        pThis->fMergePending = false;
        rc = RTSemFastMutexRelease(mutex);
        AssertRC(rc);
        rc = RTSemFastMutexDestroy(mutex);
        AssertRC(rc);
    }

    if (RT_VALID_PTR(pThis->pBlkCache))
    {
        PDMR3BlkCacheRelease(pThis->pBlkCache);
        pThis->pBlkCache = NULL;
    }

    if (RT_VALID_PTR(pThis->pDisk))
    {
        VDDestroy(pThis->pDisk);
        pThis->pDisk = NULL;
    }
    drvvdFreeImages(pThis);
}

/**
 * @copydoc FNPDMDRVPOWEROFF
 */
static DECLCALLBACK(void) drvvdPowerOff(PPDMDRVINS pDrvIns)
{
    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);
    drvvdPowerOffOrDestruct(pDrvIns);
}

/**
 * VM resume notification that we use to undo what the temporary read-only image
 * mode set by drvvdSuspend.
 *
 * Also switch to runtime error mode if we're resuming after a state load
 * without having been powered on first.
 *
 * @param   pDrvIns     The driver instance data.
 *
 * @todo    The VMSetError vs VMSetRuntimeError mess must be fixed elsewhere,
 *          we're making assumptions about Main behavior here!
 */
static DECLCALLBACK(void) drvvdResume(PPDMDRVINS pDrvIns)
{
    LogFlowFunc(("\n"));
    PVBOXDISK pThis = PDMINS_2_DATA(pDrvIns, PVBOXDISK);

    drvvdSetWritable(pThis);
    pThis->fErrorUseRuntime = true;

    if (pThis->pBlkCache)
    {
        int rc = PDMR3BlkCacheResume(pThis->pBlkCache);
        AssertRC(rc);
    }
}

/**
 * The VM is being suspended, temporarily change to read-only image mode.
 *
 * This is important for several reasons:
 *   -# It makes sure that there are no pending writes to the image.  Most
 *      backends implements this by closing and reopening the image in read-only
 *      mode.
 *   -# It allows Main to read the images during snapshotting without having
 *      to account for concurrent writes.
 *   -# This is essential for making teleportation targets sharing images work
 *      right.  Both with regards to caching and with regards to file sharing
 *      locks (RTFILE_O_DENY_*).  (See also drvvdLoadDone.)
 *
 * @param   pDrvIns     The driver instance data.
 */
static DECLCALLBACK(void) drvvdSuspend(PPDMDRVINS pDrvIns)
{
    LogFlowFunc(("\n"));
    PVBOXDISK pThis = PDMINS_2_DATA(pDrvIns, PVBOXDISK);

    if (pThis->pBlkCache)
    {
        int rc = PDMR3BlkCacheSuspend(pThis->pBlkCache);
        AssertRC(rc);
    }

    drvvdSetReadonly(pThis);
}

/**
 * VM PowerOn notification for undoing the TempReadOnly config option and
 * changing to runtime error mode.
 *
 * @param   pDrvIns     The driver instance data.
 *
 * @todo    The VMSetError vs VMSetRuntimeError mess must be fixed elsewhere,
 *          we're making assumptions about Main behavior here!
 */
static DECLCALLBACK(void) drvvdPowerOn(PPDMDRVINS pDrvIns)
{
    LogFlowFunc(("\n"));
    PVBOXDISK pThis = PDMINS_2_DATA(pDrvIns, PVBOXDISK);
    drvvdSetWritable(pThis);
    pThis->fErrorUseRuntime = true;
}

/**
 * @copydoc FNPDMDRVRESET
 */
static DECLCALLBACK(void) drvvdReset(PPDMDRVINS pDrvIns)
{
    LogFlowFunc(("\n"));
    PVBOXDISK pThis = PDMINS_2_DATA(pDrvIns, PVBOXDISK);

    if (pThis->pBlkCache)
    {
        int rc = PDMR3BlkCacheClear(pThis->pBlkCache);
        AssertRC(rc);
    }

    if (pThis->fBootAccelEnabled)
    {
        pThis->fBootAccelActive = true;
        pThis->cbDataValid      = 0;
        pThis->offDisk          = 0;
    }
}

/**
 * @copydoc FNPDMDRVDESTRUCT
 */
static DECLCALLBACK(void) drvvdDestruct(PPDMDRVINS pDrvIns)
{
    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);
    PVBOXDISK pThis = PDMINS_2_DATA(pDrvIns, PVBOXDISK);
    LogFlowFunc(("\n"));

    /*
     * Make sure the block cache and disks are closed when this driver is
     * destroyed. This method will get called without calling the power off
     * callback first when we reconfigure the driver chain after a snapshot.
     */
    drvvdPowerOffOrDestruct(pDrvIns);
    if (pThis->MergeLock != NIL_RTSEMRW)
    {
        int rc = RTSemRWDestroy(pThis->MergeLock);
        AssertRC(rc);
        pThis->MergeLock = NIL_RTSEMRW;
    }
    if (pThis->pbData)
    {
        RTMemFree(pThis->pbData);
        pThis->pbData = NULL;
    }
    if (pThis->pszBwGroup)
    {
        MMR3HeapFree(pThis->pszBwGroup);
        pThis->pszBwGroup = NULL;
    }
    if (pThis->hHbdMgr != NIL_HBDMGR)
        HBDMgrDestroy(pThis->hHbdMgr);
    if (pThis->hIoReqCache != NIL_RTMEMCACHE)
        RTMemCacheDestroy(pThis->hIoReqCache);
    for (unsigned i = 0; i < RT_ELEMENTS(pThis->aIoReqAllocBins); i++)
    {
        if (pThis->aIoReqAllocBins[i].hMtxLstIoReqAlloc != NIL_RTSEMFASTMUTEX)
            RTSemFastMutexDestroy(pThis->aIoReqAllocBins[i].hMtxLstIoReqAlloc);
    }
}

/**
 * Construct a VBox disk media driver instance.
 *
 * @copydoc FNPDMDRVCONSTRUCT
 */
static DECLCALLBACK(int) drvvdConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags)
{
    LogFlowFunc(("\n"));
    PDMDRV_CHECK_VERSIONS_RETURN(pDrvIns);
    PVBOXDISK pThis = PDMINS_2_DATA(pDrvIns, PVBOXDISK);
    int rc = VINF_SUCCESS;
    char *pszName = NULL;        /**< The path of the disk image file. */
    char *pszFormat = NULL;      /**< The format backed to use for this image. */
    char *pszCachePath = NULL;   /**< The path to the cache image. */
    char *pszCacheFormat = NULL; /**< The format backend to use for the cache image. */
    bool fReadOnly;              /**< True if the media is read-only. */
    bool fMaybeReadOnly;         /**< True if the media may or may not be read-only. */
    bool fHonorZeroWrites;       /**< True if zero blocks should be written. */

    /*
     * Init the static parts.
     */
    pDrvIns->IBase.pfnQueryInterface    = drvvdQueryInterface;
    pThis->pDrvIns                      = pDrvIns;
    pThis->fTempReadOnly                = false;
    pThis->pDisk                        = NULL;
    pThis->fAsyncIOSupported            = false;
    pThis->fShareable                   = false;
    pThis->fMergePending                = false;
    pThis->MergeCompleteMutex           = NIL_RTSEMFASTMUTEX;
    pThis->MergeLock                    = NIL_RTSEMRW;
    pThis->uMergeSource                 = VD_LAST_IMAGE;
    pThis->uMergeTarget                 = VD_LAST_IMAGE;
    pThis->pCfgCrypto                   = NULL;
    pThis->pIfSecKey                    = NULL;
    pThis->hIoReqCache                  = NIL_RTMEMCACHE;

    for (unsigned i = 0; i < RT_ELEMENTS(pThis->aIoReqAllocBins); i++)
        pThis->aIoReqAllocBins[i].hMtxLstIoReqAlloc = NIL_RTSEMFASTMUTEX;

    /* IMedia */
    pThis->IMedia.pfnRead               = drvvdRead;
    pThis->IMedia.pfnReadPcBios         = drvvdReadPcBios;
    pThis->IMedia.pfnWrite              = drvvdWrite;
    pThis->IMedia.pfnFlush              = drvvdFlush;
    pThis->IMedia.pfnMerge              = drvvdMerge;
    pThis->IMedia.pfnSetSecKeyIf        = drvvdSetSecKeyIf;
    pThis->IMedia.pfnGetSize            = drvvdGetSize;
    pThis->IMedia.pfnGetSectorSize      = drvvdGetSectorSize;
    pThis->IMedia.pfnIsReadOnly         = drvvdIsReadOnly;
    pThis->IMedia.pfnBiosGetPCHSGeometry = drvvdBiosGetPCHSGeometry;
    pThis->IMedia.pfnBiosSetPCHSGeometry = drvvdBiosSetPCHSGeometry;
    pThis->IMedia.pfnBiosGetLCHSGeometry = drvvdBiosGetLCHSGeometry;
    pThis->IMedia.pfnBiosSetLCHSGeometry = drvvdBiosSetLCHSGeometry;
    pThis->IMedia.pfnBiosIsVisible       = drvvdBiosIsVisible;
    pThis->IMedia.pfnGetType             = drvvdGetType;
    pThis->IMedia.pfnGetUuid             = drvvdGetUuid;
    pThis->IMedia.pfnDiscard             = drvvdDiscard;
    pThis->IMedia.pfnIoBufAlloc          = drvvdIoBufAlloc;
    pThis->IMedia.pfnIoBufFree           = drvvdIoBufFree;
    pThis->IMedia.pfnSendCmd             = NULL;

    /* IMount */
    pThis->IMount.pfnUnmount                = drvvdUnmount;
    pThis->IMount.pfnIsMounted              = drvvdIsMounted;
    pThis->IMount.pfnLock                   = drvvdLock;
    pThis->IMount.pfnUnlock                 = drvvdUnlock;
    pThis->IMount.pfnIsLocked               = drvvdIsLocked;

    /* IMediaAsync */
    pThis->IMediaAsync.pfnStartRead       = drvvdStartRead;
    pThis->IMediaAsync.pfnStartWrite      = drvvdStartWrite;
    pThis->IMediaAsync.pfnStartFlush      = drvvdStartFlush;
    pThis->IMediaAsync.pfnStartDiscard    = drvvdStartDiscard;

    /* IMediaEx */
    pThis->IMediaEx.pfnIoReqAllocSizeSet   = drvvdIoReqAllocSizeSet;
    pThis->IMediaEx.pfnIoReqAlloc          = drvvdIoReqAlloc;
    pThis->IMediaEx.pfnIoReqFree           = drvvdIoReqFree;
    pThis->IMediaEx.pfnIoReqCancel         = drvvdIoReqCancel;
    pThis->IMediaEx.pfnIoReqRead           = drvvdIoReqRead;
    pThis->IMediaEx.pfnIoReqWrite          = drvvdIoReqWrite;
    pThis->IMediaEx.pfnIoReqFlush          = drvvdIoReqFlush;
    pThis->IMediaEx.pfnIoReqDiscard        = drvvdIoReqDiscard;
    pThis->IMediaEx.pfnIoReqGetActiveCount = drvvdIoReqGetActiveCount;

    /* Initialize supported VD interfaces. */
    pThis->pVDIfsDisk = NULL;

    pThis->VDIfError.pfnError     = drvvdErrorCallback;
    pThis->VDIfError.pfnMessage   = NULL;
    rc = VDInterfaceAdd(&pThis->VDIfError.Core, "DrvVD_VDIError", VDINTERFACETYPE_ERROR,
                        pDrvIns, sizeof(VDINTERFACEERROR), &pThis->pVDIfsDisk);
    AssertRC(rc);

    /* List of images is empty now. */
    pThis->pImages = NULL;

    pThis->pDrvMediaPort = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMIMEDIAPORT);
    if (!pThis->pDrvMediaPort)
        return PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_MISSING_INTERFACE_ABOVE,
                                N_("No media port interface above"));

    /* Try to attach async media port interface above.*/
    pThis->pDrvMediaAsyncPort = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMIMEDIAASYNCPORT);
    pThis->pDrvMountNotify    = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMIMOUNTNOTIFY);

    /*
     * Try to attach the optional extended media interface port above and initialize associated
     * structures if available.
     */
    pThis->pDrvMediaExPort = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMIMEDIAEXPORT);
    if (pThis->pDrvMediaExPort)
    {
        for (unsigned i = 0; i < RT_ELEMENTS(pThis->aIoReqAllocBins); i++)
        {
            rc = RTSemFastMutexCreate(&pThis->aIoReqAllocBins[i].hMtxLstIoReqAlloc);
            if (RT_FAILURE(rc))
                break;
            RTListInit(&pThis->aIoReqAllocBins[i].LstIoReqAlloc);
        }

        if (RT_FAILURE(rc))
            return PDMDRV_SET_ERROR(pDrvIns, rc, N_("Creating Mutex failed"));
    }

    /* Before we access any VD API load all given plugins. */
    rc = drvvdLoadPlugins(pThis, pCfg);
    if (RT_FAILURE(rc))
        return PDMDRV_SET_ERROR(pDrvIns, rc, N_("Loading VD plugins failed"));

    /*
     * Validate configuration and find all parent images.
     * It's sort of up side down from the image dependency tree.
     */
    bool        fHostIP = false;
    bool        fUseNewIo = false;
    bool        fUseBlockCache = false;
    bool        fDiscard = false;
    bool        fInformAboutZeroBlocks = false;
    bool        fSkipConsistencyChecks = false;
    bool        fEmptyDrive            = false;
    unsigned    iLevel = 0;
    PCFGMNODE   pCurNode = pCfg;

    for (;;)
    {
        bool fValid;

        if (pCurNode == pCfg)
        {
            /* Toplevel configuration additionally contains the global image
             * open flags. Some might be converted to per-image flags later. */
            fValid = CFGMR3AreValuesValid(pCurNode,
                                          "Format\0Path\0"
                                          "ReadOnly\0MaybeReadOnly\0TempReadOnly\0Shareable\0HonorZeroWrites\0"
                                          "HostIPStack\0UseNewIo\0BootAcceleration\0BootAccelerationBuffer\0"
                                          "SetupMerge\0MergeSource\0MergeTarget\0BwGroup\0Type\0BlockCache\0"
                                          "CachePath\0CacheFormat\0Discard\0InformAboutZeroBlocks\0"
                                          "SkipConsistencyChecks\0"
                                          "Locked\0BIOSVisible\0Cylinders\0Heads\0Sectors\0Mountable\0"
                                          "EmptyDrive\0"
#if defined(VBOX_PERIODIC_FLUSH) || defined(VBOX_IGNORE_FLUSH)
                                          "FlushInterval\0IgnoreFlush\0IgnoreFlushAsync\0"
#endif /* !(VBOX_PERIODIC_FLUSH || VBOX_IGNORE_FLUSH) */
                                           );
        }
        else
        {
            /* All other image configurations only contain image name and
             * the format information. */
            fValid = CFGMR3AreValuesValid(pCurNode, "Format\0Path\0"
                                                    "MergeSource\0MergeTarget\0");
        }
        if (!fValid)
        {
            rc = PDMDrvHlpVMSetError(pDrvIns, VERR_PDM_DRVINS_UNKNOWN_CFG_VALUES,
                                     RT_SRC_POS, N_("DrvVD: Configuration error: keys incorrect at level %d"), iLevel);
            break;
        }

        if (pCurNode == pCfg)
        {
            rc = CFGMR3QueryBoolDef(pCurNode, "HostIPStack", &fHostIP, true);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"HostIPStack\" as boolean failed"));
                break;
            }

            rc = CFGMR3QueryBoolDef(pCurNode, "HonorZeroWrites", &fHonorZeroWrites, false);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"HonorZeroWrites\" as boolean failed"));
                break;
            }

            rc = CFGMR3QueryBoolDef(pCurNode, "ReadOnly", &fReadOnly, false);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"ReadOnly\" as boolean failed"));
                break;
            }

            rc = CFGMR3QueryBoolDef(pCurNode, "MaybeReadOnly", &fMaybeReadOnly, false);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"MaybeReadOnly\" as boolean failed"));
                break;
            }

            rc = CFGMR3QueryBoolDef(pCurNode, "TempReadOnly", &pThis->fTempReadOnly, false);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"TempReadOnly\" as boolean failed"));
                break;
            }
            if (fReadOnly && pThis->fTempReadOnly)
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_DRIVER_INVALID_PROPERTIES,
                                      N_("DrvVD: Configuration error: Both \"ReadOnly\" and \"TempReadOnly\" are set"));
                break;
            }

            rc = CFGMR3QueryBoolDef(pCurNode, "Shareable", &pThis->fShareable, false);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"Shareable\" as boolean failed"));
                break;
            }

            rc = CFGMR3QueryBoolDef(pCurNode, "UseNewIo", &fUseNewIo, false);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"UseNewIo\" as boolean failed"));
                break;
            }
            rc = CFGMR3QueryBoolDef(pCurNode, "SetupMerge", &pThis->fMergePending, false);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"SetupMerge\" as boolean failed"));
                break;
            }
            if (fReadOnly && pThis->fMergePending)
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_DRIVER_INVALID_PROPERTIES,
                                      N_("DrvVD: Configuration error: Both \"ReadOnly\" and \"MergePending\" are set"));
                break;
            }
            rc = CFGMR3QueryBoolDef(pCurNode, "BootAcceleration", &pThis->fBootAccelEnabled, false);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"BootAcceleration\" as boolean failed"));
                break;
            }
            rc = CFGMR3QueryU32Def(pCurNode, "BootAccelerationBuffer", (uint32_t *)&pThis->cbBootAccelBuffer, 16 * _1K);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"BootAccelerationBuffer\" as integer failed"));
                break;
            }
            rc = CFGMR3QueryBoolDef(pCurNode, "BlockCache", &fUseBlockCache, false);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"BlockCache\" as boolean failed"));
                break;
            }
            rc = CFGMR3QueryStringAlloc(pCurNode, "BwGroup", &pThis->pszBwGroup);
            if (RT_FAILURE(rc) && rc != VERR_CFGM_VALUE_NOT_FOUND)
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"BwGroup\" as string failed"));
                break;
            }
            else
                rc = VINF_SUCCESS;
            rc = CFGMR3QueryBoolDef(pCurNode, "Discard", &fDiscard, false);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"Discard\" as boolean failed"));
                break;
            }
            if (fReadOnly && fDiscard)
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_DRIVER_INVALID_PROPERTIES,
                                      N_("DrvVD: Configuration error: Both \"ReadOnly\" and \"Discard\" are set"));
                break;
            }
            rc = CFGMR3QueryBoolDef(pCurNode, "InformAboutZeroBlocks", &fInformAboutZeroBlocks, false);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"InformAboutZeroBlocks\" as boolean failed"));
                break;
            }
            rc = CFGMR3QueryBoolDef(pCurNode, "SkipConsistencyChecks", &fSkipConsistencyChecks, true);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"SKipConsistencyChecks\" as boolean failed"));
                break;
            }

            char *psz = NULL;
           rc = CFGMR3QueryStringAlloc(pCfg, "Type", &psz);
            if (RT_FAILURE(rc))
                return PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_BLOCK_NO_TYPE, N_("Failed to obtain the sub type"));
            pThis->enmType = drvvdGetMediaTypeFromString(psz);
            if (pThis->enmType == PDMMEDIATYPE_ERROR)
            {
                PDMDrvHlpVMSetError(pDrvIns, VERR_PDM_BLOCK_UNKNOWN_TYPE, RT_SRC_POS,
                                    N_("Unknown type \"%s\""), psz);
                MMR3HeapFree(psz);
                return VERR_PDM_BLOCK_UNKNOWN_TYPE;
            }
            MMR3HeapFree(psz); psz = NULL;

            rc = CFGMR3QueryStringAlloc(pCurNode, "CachePath", &pszCachePath);
            if (RT_FAILURE(rc) && rc != VERR_CFGM_VALUE_NOT_FOUND)
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"CachePath\" as string failed"));
                break;
            }
            else
                rc = VINF_SUCCESS;

            if (pszCachePath)
            {
                rc = CFGMR3QueryStringAlloc(pCurNode, "CacheFormat", &pszCacheFormat);
                if (RT_FAILURE(rc))
                {
                    rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                          N_("DrvVD: Configuration error: Querying \"CacheFormat\" as string failed"));
                    break;
                }
            }

            /* Mountable */
            rc = CFGMR3QueryBoolDef(pCfg, "Mountable", &pThis->fMountable, false);
            if (RT_FAILURE(rc))
                return PDMDRV_SET_ERROR(pDrvIns, rc, N_("Failed to query \"Mountable\" from the config"));

            /* Locked */
            rc = CFGMR3QueryBoolDef(pCfg, "Locked", &pThis->fLocked, false);
            if (RT_FAILURE(rc))
                return PDMDRV_SET_ERROR(pDrvIns, rc, N_("Failed to query \"Locked\" from the config"));

            /* BIOS visible */
            rc = CFGMR3QueryBoolDef(pCfg, "BIOSVisible", &pThis->fBiosVisible, true);
            if (RT_FAILURE(rc))
                return PDMDRV_SET_ERROR(pDrvIns, rc, N_("Failed to query \"BIOSVisible\" from the config"));

            /* Cylinders */
            rc = CFGMR3QueryU32Def(pCfg, "Cylinders", &pThis->LCHSGeometry.cCylinders, 0);
            if (RT_FAILURE(rc))
                return PDMDRV_SET_ERROR(pDrvIns, rc, N_("Failed to query \"Cylinders\" from the config"));

            /* Heads */
            rc = CFGMR3QueryU32Def(pCfg, "Heads", &pThis->LCHSGeometry.cHeads, 0);
            if (RT_FAILURE(rc))
                return PDMDRV_SET_ERROR(pDrvIns, rc, N_("Failed to query \"Heads\" from the config"));

            /* Sectors */
            rc = CFGMR3QueryU32Def(pCfg, "Sectors", &pThis->LCHSGeometry.cSectors, 0);
            if (RT_FAILURE(rc))
                return PDMDRV_SET_ERROR(pDrvIns, rc, N_("Failed to query \"Sectors\" from the config"));

            /* Uuid */
            rc = CFGMR3QueryStringAlloc(pCfg, "Uuid", &psz);
            if (rc == VERR_CFGM_VALUE_NOT_FOUND)
                RTUuidClear(&pThis->Uuid);
            else if (RT_SUCCESS(rc))
            {
                rc = RTUuidFromStr(&pThis->Uuid, psz);
                if (RT_FAILURE(rc))
                {
                    PDMDrvHlpVMSetError(pDrvIns, rc, RT_SRC_POS, N_("Uuid from string failed on \"%s\""), psz);
                    MMR3HeapFree(psz);
                    return rc;
                }
                MMR3HeapFree(psz); psz = NULL;
            }
            else
                return PDMDRV_SET_ERROR(pDrvIns, rc, N_("Failed to query \"Uuid\" from the config"));

#ifdef VBOX_PERIODIC_FLUSH
            rc = CFGMR3QueryU32Def(pCfg, "FlushInterval", &pThis->cbFlushInterval, 0);
            if (RT_FAILURE(rc))
                return PDMDRV_SET_ERROR(pDrvIns, rc, N_("Failed to query \"FlushInterval\" from the config"));
#endif /* VBOX_PERIODIC_FLUSH */

#ifdef VBOX_IGNORE_FLUSH
            rc = CFGMR3QueryBoolDef(pCfg, "IgnoreFlush", &pThis->fIgnoreFlush, true);
            if (RT_FAILURE(rc))
                return PDMDRV_SET_ERROR(pDrvIns, rc, N_("Failed to query \"IgnoreFlush\" from the config"));

            if (pThis->fIgnoreFlush)
                LogRel(("DrvVD: Flushes will be ignored\n"));
            else
                LogRel(("DrvVD: Flushes will be passed to the disk\n"));

            rc = CFGMR3QueryBoolDef(pCfg, "IgnoreFlushAsync", &pThis->fIgnoreFlushAsync, false);
            if (RT_FAILURE(rc))
                return PDMDRV_SET_ERROR(pDrvIns, rc, N_("Failed to query \"IgnoreFlushAsync\" from the config"));

            if (pThis->fIgnoreFlushAsync)
                LogRel(("DrvVD: Async flushes will be ignored\n"));
            else
                LogRel(("DrvVD: Async flushes will be passed to the disk\n"));
#endif /* VBOX_IGNORE_FLUSH */

            rc = CFGMR3QueryBoolDef(pCurNode, "EmptyDrive", &fEmptyDrive, false);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"EmptyDrive\" as boolean failed"));
                break;
            }
        }

        PCFGMNODE pParent = CFGMR3GetChild(pCurNode, "Parent");
        if (!pParent)
            break;
        pCurNode = pParent;
        iLevel++;
    }

    if (!fEmptyDrive)
    {
        /*
         * Create the image container and the necessary interfaces.
         */
        if (RT_SUCCESS(rc))
        {
            /*
             * The image has a bandwidth group but the host cache is enabled.
             * Use the async I/O framework but tell it to enable the host cache.
             */
            if (!fUseNewIo && pThis->pszBwGroup)
            {
                pThis->fAsyncIoWithHostCache = true;
                fUseNewIo = true;
            }

            /** @todo quick hack to work around problems in the async I/O
             * implementation (rw semaphore thread ownership problem)
             * while a merge is running. Remove once this is fixed. */
            if (pThis->fMergePending)
                fUseNewIo = false;

            if (RT_SUCCESS(rc) && pThis->fMergePending)
            {
                rc = RTSemFastMutexCreate(&pThis->MergeCompleteMutex);
                if (RT_SUCCESS(rc))
                    rc = RTSemRWCreate(&pThis->MergeLock);
                if (RT_SUCCESS(rc))
                {
                    pThis->VDIfThreadSync.pfnStartRead   = drvvdThreadStartRead;
                    pThis->VDIfThreadSync.pfnFinishRead  = drvvdThreadFinishRead;
                    pThis->VDIfThreadSync.pfnStartWrite  = drvvdThreadStartWrite;
                    pThis->VDIfThreadSync.pfnFinishWrite = drvvdThreadFinishWrite;

                    rc = VDInterfaceAdd(&pThis->VDIfThreadSync.Core, "DrvVD_ThreadSync", VDINTERFACETYPE_THREADSYNC,
                                        pThis, sizeof(VDINTERFACETHREADSYNC), &pThis->pVDIfsDisk);
                }
                else
                {
                    rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                          N_("DrvVD: Failed to create semaphores for \"MergePending\""));
                }
            }

            if (RT_SUCCESS(rc))
            {
                rc = VDCreate(pThis->pVDIfsDisk, drvvdGetVDFromMediaType(pThis->enmType), &pThis->pDisk);
                /* Error message is already set correctly. */
            }
        }

        if (pThis->pDrvMediaAsyncPort && fUseNewIo)
            pThis->fAsyncIOSupported = true;

        uint64_t tsStart = RTTimeNanoTS();

        unsigned iImageIdx = 0;
        while (pCurNode && RT_SUCCESS(rc))
        {
            /* Allocate per-image data. */
            PVBOXIMAGE pImage = drvvdNewImage(pThis);
            if (!pImage)
            {
                rc = VERR_NO_MEMORY;
                break;
            }

            /*
             * Read the image configuration.
             */
            rc = CFGMR3QueryStringAlloc(pCurNode, "Path", &pszName);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"Path\" as string failed"));
                break;
            }

            rc = CFGMR3QueryStringAlloc(pCurNode, "Format", &pszFormat);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"Format\" as string failed"));
                break;
            }

            bool fMergeSource;
            rc = CFGMR3QueryBoolDef(pCurNode, "MergeSource", &fMergeSource, false);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"MergeSource\" as boolean failed"));
                break;
            }
            if (fMergeSource)
            {
                if (pThis->uMergeSource == VD_LAST_IMAGE)
                    pThis->uMergeSource = iImageIdx;
                else
                {
                    rc = PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_DRIVER_INVALID_PROPERTIES,
                                          N_("DrvVD: Configuration error: Multiple \"MergeSource\" occurrences"));
                    break;
                }
            }

            bool fMergeTarget;
            rc = CFGMR3QueryBoolDef(pCurNode, "MergeTarget", &fMergeTarget, false);
            if (RT_FAILURE(rc))
            {
                rc = PDMDRV_SET_ERROR(pDrvIns, rc,
                                      N_("DrvVD: Configuration error: Querying \"MergeTarget\" as boolean failed"));
                break;
            }
            if (fMergeTarget)
            {
                if (pThis->uMergeTarget == VD_LAST_IMAGE)
                    pThis->uMergeTarget = iImageIdx;
                else
                {
                    rc = PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_DRIVER_INVALID_PROPERTIES,
                                          N_("DrvVD: Configuration error: Multiple \"MergeTarget\" occurrences"));
                    break;
                }
            }

            PCFGMNODE pCfgVDConfig = CFGMR3GetChild(pCurNode, "VDConfig");
            pImage->VDIfConfig.pfnAreKeysValid = drvvdCfgAreKeysValid;
            pImage->VDIfConfig.pfnQuerySize    = drvvdCfgQuerySize;
            pImage->VDIfConfig.pfnQuery        = drvvdCfgQuery;
            pImage->VDIfConfig.pfnQueryBytes   = NULL;
            rc = VDInterfaceAdd(&pImage->VDIfConfig.Core, "DrvVD_Config", VDINTERFACETYPE_CONFIG,
                                pCfgVDConfig, sizeof(VDINTERFACECONFIG), &pImage->pVDIfsImage);
            AssertRC(rc);

            /* Check VDConfig for encryption config. */
            if (pCfgVDConfig)
                pThis->pCfgCrypto = CFGMR3GetChild(pCfgVDConfig, "CRYPT");

            if (pThis->pCfgCrypto)
            {
                /* Setup VDConfig interface for disk encryption support. */
                pThis->VDIfCfg.pfnAreKeysValid  = drvvdCfgAreKeysValid;
                pThis->VDIfCfg.pfnQuerySize     = drvvdCfgQuerySize;
                pThis->VDIfCfg.pfnQuery         = drvvdCfgQuery;
                pThis->VDIfCfg.pfnQueryBytes    = NULL;

                pThis->VDIfCrypto.pfnKeyRetain               = drvvdCryptoKeyRetain;
                pThis->VDIfCrypto.pfnKeyRelease              = drvvdCryptoKeyRelease;
                pThis->VDIfCrypto.pfnKeyStorePasswordRetain  = drvvdCryptoKeyStorePasswordRetain;
                pThis->VDIfCrypto.pfnKeyStorePasswordRelease = drvvdCryptoKeyStorePasswordRelease;
            }

            /* Unconditionally insert the TCPNET interface, don't bother to check
             * if an image really needs it. Will be ignored. Since the TCPNET
             * interface is per image we could make this more flexible in the
             * future if we want to. */
            /* Construct TCPNET callback table depending on the config. This is
             * done unconditionally, as uninterested backends will ignore it. */
            if (fHostIP)
            {
                pImage->VDIfTcpNet.pfnSocketCreate = drvvdTcpSocketCreate;
                pImage->VDIfTcpNet.pfnSocketDestroy = drvvdTcpSocketDestroy;
                pImage->VDIfTcpNet.pfnClientConnect = drvvdTcpClientConnect;
                pImage->VDIfTcpNet.pfnIsClientConnected = drvvdTcpIsClientConnected;
                pImage->VDIfTcpNet.pfnClientClose = drvvdTcpClientClose;
                pImage->VDIfTcpNet.pfnSelectOne = drvvdTcpSelectOne;
                pImage->VDIfTcpNet.pfnRead = drvvdTcpRead;
                pImage->VDIfTcpNet.pfnWrite = drvvdTcpWrite;
                pImage->VDIfTcpNet.pfnSgWrite = drvvdTcpSgWrite;
                pImage->VDIfTcpNet.pfnReadNB = drvvdTcpReadNB;
                pImage->VDIfTcpNet.pfnWriteNB = drvvdTcpWriteNB;
                pImage->VDIfTcpNet.pfnSgWriteNB = drvvdTcpSgWriteNB;
                pImage->VDIfTcpNet.pfnFlush = drvvdTcpFlush;
                pImage->VDIfTcpNet.pfnSetSendCoalescing = drvvdTcpSetSendCoalescing;
                pImage->VDIfTcpNet.pfnGetLocalAddress = drvvdTcpGetLocalAddress;
                pImage->VDIfTcpNet.pfnGetPeerAddress = drvvdTcpGetPeerAddress;

                /*
                 * There is a 15ms delay between receiving the data and marking the socket
                 * as readable on Windows XP which hurts async I/O performance of
                 * TCP backends badly. Provide a different select method without
                 * using poll on XP.
                 * This is only used on XP because it is not as efficient as the one using poll
                 * and all other Windows versions are working fine.
                 */
                char szOS[64];
                memset(szOS, 0, sizeof(szOS));
                rc = RTSystemQueryOSInfo(RTSYSOSINFO_PRODUCT, &szOS[0], sizeof(szOS));

                if (RT_SUCCESS(rc) && !strncmp(szOS, "Windows XP", 10))
                {
                    LogRel(("VD: Detected Windows XP, disabled poll based waiting for TCP\n"));
                    pImage->VDIfTcpNet.pfnSelectOneEx = drvvdTcpSelectOneExNoPoll;
                }
                else
                    pImage->VDIfTcpNet.pfnSelectOneEx = drvvdTcpSelectOneExPoll;

                pImage->VDIfTcpNet.pfnPoke = drvvdTcpPoke;
            }
            else
            {
#ifndef VBOX_WITH_INIP
                rc = PDMDrvHlpVMSetError(pDrvIns, VERR_PDM_DRVINS_UNKNOWN_CFG_VALUES,
                                         RT_SRC_POS, N_("DrvVD: Configuration error: TCP over Internal Networking not compiled in"));
#else /* VBOX_WITH_INIP */
                pImage->VDIfTcpNet.pfnSocketCreate = drvvdINIPSocketCreate;
                pImage->VDIfTcpNet.pfnSocketDestroy = drvvdINIPSocketDestroy;
                pImage->VDIfTcpNet.pfnClientConnect = drvvdINIPClientConnect;
                pImage->VDIfTcpNet.pfnClientClose = drvvdINIPClientClose;
                pImage->VDIfTcpNet.pfnIsClientConnected = drvvdINIPIsClientConnected;
                pImage->VDIfTcpNet.pfnSelectOne = drvvdINIPSelectOne;
                pImage->VDIfTcpNet.pfnRead = drvvdINIPRead;
                pImage->VDIfTcpNet.pfnWrite = drvvdINIPWrite;
                pImage->VDIfTcpNet.pfnSgWrite = drvvdINIPSgWrite;
                pImage->VDIfTcpNet.pfnFlush = drvvdINIPFlush;
                pImage->VDIfTcpNet.pfnSetSendCoalescing = drvvdINIPSetSendCoalescing;
                pImage->VDIfTcpNet.pfnGetLocalAddress = drvvdINIPGetLocalAddress;
                pImage->VDIfTcpNet.pfnGetPeerAddress = drvvdINIPGetPeerAddress;
                pImage->VDIfTcpNet.pfnSelectOneEx = drvvdINIPSelectOneEx;
                pImage->VDIfTcpNet.pfnPoke = drvvdINIPPoke;
#endif /* VBOX_WITH_INIP */
            }
            rc = VDInterfaceAdd(&pImage->VDIfTcpNet.Core, "DrvVD_TCPNET",
                                VDINTERFACETYPE_TCPNET, NULL,
                                sizeof(VDINTERFACETCPNET), &pImage->pVDIfsImage);
            AssertRC(rc);

            /* Insert the custom I/O interface only if we're told to use new IO.
             * Since the I/O interface is per image we could make this more
             * flexible in the future if we want to. */
            if (fUseNewIo)
            {
#ifdef VBOX_WITH_PDM_ASYNC_COMPLETION
                pImage->VDIfIo.pfnOpen       = drvvdAsyncIOOpen;
                pImage->VDIfIo.pfnClose      = drvvdAsyncIOClose;
                pImage->VDIfIo.pfnGetSize    = drvvdAsyncIOGetSize;
                pImage->VDIfIo.pfnSetSize    = drvvdAsyncIOSetSize;
                pImage->VDIfIo.pfnReadSync   = drvvdAsyncIOReadSync;
                pImage->VDIfIo.pfnWriteSync  = drvvdAsyncIOWriteSync;
                pImage->VDIfIo.pfnFlushSync  = drvvdAsyncIOFlushSync;
                pImage->VDIfIo.pfnReadAsync  = drvvdAsyncIOReadAsync;
                pImage->VDIfIo.pfnWriteAsync = drvvdAsyncIOWriteAsync;
                pImage->VDIfIo.pfnFlushAsync = drvvdAsyncIOFlushAsync;
#else /* !VBOX_WITH_PDM_ASYNC_COMPLETION */
                rc = PDMDrvHlpVMSetError(pDrvIns, VERR_PDM_DRVINS_UNKNOWN_CFG_VALUES,
                                         RT_SRC_POS, N_("DrvVD: Configuration error: Async Completion Framework not compiled in"));
#endif /* !VBOX_WITH_PDM_ASYNC_COMPLETION */
                if (RT_SUCCESS(rc))
                    rc = VDInterfaceAdd(&pImage->VDIfIo.Core, "DrvVD_IO", VDINTERFACETYPE_IO,
                                        pThis, sizeof(VDINTERFACEIO), &pImage->pVDIfsImage);
                AssertRC(rc);
            }

            /*
             * Open the image.
             */
            unsigned uOpenFlags;
            if (fReadOnly || pThis->fTempReadOnly || iLevel != 0)
                uOpenFlags = VD_OPEN_FLAGS_READONLY;
            else
                uOpenFlags = VD_OPEN_FLAGS_NORMAL;
            if (fHonorZeroWrites)
                uOpenFlags |= VD_OPEN_FLAGS_HONOR_ZEROES;
            if (pThis->fAsyncIOSupported)
                uOpenFlags |= VD_OPEN_FLAGS_ASYNC_IO;
            if (pThis->fShareable)
                uOpenFlags |= VD_OPEN_FLAGS_SHAREABLE;
            if (fDiscard && iLevel == 0)
                uOpenFlags |= VD_OPEN_FLAGS_DISCARD;
            if (fInformAboutZeroBlocks)
                uOpenFlags |= VD_OPEN_FLAGS_INFORM_ABOUT_ZERO_BLOCKS;
            if (   (uOpenFlags & VD_OPEN_FLAGS_READONLY)
                && fSkipConsistencyChecks)
                uOpenFlags |= VD_OPEN_FLAGS_SKIP_CONSISTENCY_CHECKS;

            /* Try to open backend in async I/O mode first. */
            rc = VDOpen(pThis->pDisk, pszFormat, pszName, uOpenFlags, pImage->pVDIfsImage);
            if (rc == VERR_NOT_SUPPORTED)
            {
                pThis->fAsyncIOSupported = false;
                uOpenFlags &= ~VD_OPEN_FLAGS_ASYNC_IO;
                rc = VDOpen(pThis->pDisk, pszFormat, pszName, uOpenFlags, pImage->pVDIfsImage);
            }

            if (rc == VERR_VD_DISCARD_NOT_SUPPORTED)
            {
                fDiscard = false;
                uOpenFlags &= ~VD_OPEN_FLAGS_DISCARD;
                rc = VDOpen(pThis->pDisk, pszFormat, pszName, uOpenFlags, pImage->pVDIfsImage);
            }

            if (!fDiscard)
            {
                pThis->IMedia.pfnDiscard = NULL;
                pThis->IMediaAsync.pfnStartDiscard = NULL;
            }

            if (RT_SUCCESS(rc))
            {
                LogFunc(("%d - Opened '%s' in %s mode\n",
                         iLevel, pszName,
                         VDIsReadOnly(pThis->pDisk) ? "read-only" : "read-write"));
                if (  VDIsReadOnly(pThis->pDisk)
                    && !fReadOnly
                    && !fMaybeReadOnly
                    && !pThis->fTempReadOnly
                    && iLevel == 0)
                {
                    rc = PDMDrvHlpVMSetError(pDrvIns, VERR_VD_IMAGE_READ_ONLY, RT_SRC_POS,
                                             N_("Failed to open image '%s' for writing due to wrong permissions"),
                                             pszName);
                    break;
                }
            }
            else
            {
               rc = PDMDrvHlpVMSetError(pDrvIns, rc, RT_SRC_POS,
                                        N_("Failed to open image '%s' in %s mode"), pszName,
                                        (uOpenFlags & VD_OPEN_FLAGS_READONLY) ? "read-only" : "read-write");
               break;
            }

            MMR3HeapFree(pszName);
            pszName = NULL;
            MMR3HeapFree(pszFormat);
            pszFormat = NULL;

            /* next */
            iLevel--;
            iImageIdx++;
            pCurNode = CFGMR3GetParent(pCurNode);
        }

        LogRel(("VD: Opening the disk took %lld ns\n", RTTimeNanoTS() - tsStart));

        /* Open the cache image if set. */
        if (   RT_SUCCESS(rc)
            && RT_VALID_PTR(pszCachePath))
        {
            /* Insert the custom I/O interface only if we're told to use new IO.
             * Since the I/O interface is per image we could make this more
             * flexible in the future if we want to. */
            if (fUseNewIo)
            {
#ifdef VBOX_WITH_PDM_ASYNC_COMPLETION
                pThis->VDIfIoCache.pfnOpen       = drvvdAsyncIOOpen;
                pThis->VDIfIoCache.pfnClose      = drvvdAsyncIOClose;
                pThis->VDIfIoCache.pfnGetSize    = drvvdAsyncIOGetSize;
                pThis->VDIfIoCache.pfnSetSize    = drvvdAsyncIOSetSize;
                pThis->VDIfIoCache.pfnReadSync   = drvvdAsyncIOReadSync;
                pThis->VDIfIoCache.pfnWriteSync  = drvvdAsyncIOWriteSync;
                pThis->VDIfIoCache.pfnFlushSync  = drvvdAsyncIOFlushSync;
                pThis->VDIfIoCache.pfnReadAsync  = drvvdAsyncIOReadAsync;
                pThis->VDIfIoCache.pfnWriteAsync = drvvdAsyncIOWriteAsync;
                pThis->VDIfIoCache.pfnFlushAsync = drvvdAsyncIOFlushAsync;
#else /* !VBOX_WITH_PDM_ASYNC_COMPLETION */
                rc = PDMDrvHlpVMSetError(pDrvIns, VERR_PDM_DRVINS_UNKNOWN_CFG_VALUES,
                                         RT_SRC_POS, N_("DrvVD: Configuration error: Async Completion Framework not compiled in"));
#endif /* !VBOX_WITH_PDM_ASYNC_COMPLETION */
                if (RT_SUCCESS(rc))
                    rc = VDInterfaceAdd(&pThis->VDIfIoCache.Core, "DrvVD_IO", VDINTERFACETYPE_IO,
                                        pThis, sizeof(VDINTERFACEIO), &pThis->pVDIfsCache);
                AssertRC(rc);
            }

            rc = VDCacheOpen(pThis->pDisk, pszCacheFormat, pszCachePath, VD_OPEN_FLAGS_NORMAL, pThis->pVDIfsCache);
            if (RT_FAILURE(rc))
                rc = PDMDRV_SET_ERROR(pDrvIns, rc, N_("DrvVD: Could not open cache image"));
        }

        if (RT_VALID_PTR(pszCachePath))
            MMR3HeapFree(pszCachePath);
        if (RT_VALID_PTR(pszCacheFormat))
            MMR3HeapFree(pszCacheFormat);

        if (   RT_SUCCESS(rc)
            && pThis->fMergePending
            && (   pThis->uMergeSource == VD_LAST_IMAGE
                || pThis->uMergeTarget == VD_LAST_IMAGE))
        {
            rc = PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_DRIVER_INVALID_PROPERTIES,
                                  N_("DrvVD: Configuration error: Inconsistent image merge data"));
        }

        /* Create the block cache if enabled. */
        if (   fUseBlockCache
            && !pThis->fShareable
            && !fDiscard
            && !pThis->pCfgCrypto /* Disk encryption disables the block cache for security reasons */
            && RT_SUCCESS(rc))
        {
            /*
             * We need a unique ID for the block cache (to identify the owner of data
             * blocks in a saved state). UUIDs are not really suitable because
             * there are image formats which don't support them. Furthermore it is
             * possible that a new diff image was attached after a saved state
             * which changes the UUID.
             * However the device "name + device instance + LUN" triple the disk is
             * attached to is always constant for saved states.
             */
            char *pszId = NULL;
            uint32_t iInstance, iLUN;
            const char *pcszController;

            rc = pThis->pDrvMediaPort->pfnQueryDeviceLocation(pThis->pDrvMediaPort, &pcszController,
                                                              &iInstance, &iLUN);
            if (RT_FAILURE(rc))
                rc = PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_DRIVER_INVALID_PROPERTIES,
                                      N_("DrvVD: Configuration error: Could not query device data"));
            else
            {
                int cbStr = RTStrAPrintf(&pszId, "%s-%d-%d", pcszController, iInstance, iLUN);

                if (cbStr > 0)
                {
                    rc = PDMDrvHlpBlkCacheRetain(pDrvIns, &pThis->pBlkCache,
                                                 drvvdBlkCacheXferComplete,
                                                 drvvdBlkCacheXferEnqueue,
                                                 drvvdBlkCacheXferEnqueueDiscard,
                                                 pszId);
                    if (rc == VERR_NOT_SUPPORTED)
                    {
                        LogRel(("VD: Block cache is not supported\n"));
                        rc = VINF_SUCCESS;
                    }
                    else
                        AssertRC(rc);

                    RTStrFree(pszId);
                }
                else
                    rc = PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_DRIVER_INVALID_PROPERTIES,
                                          N_("DrvVD: Out of memory when creating block cache"));
            }
        }

        if (RT_SUCCESS(rc))
            rc = drvvdSetupFilters(pThis, pCfg);

        /*
         * Register a load-done callback so we can undo TempReadOnly config before
         * we get to drvvdResume.  Autoamtically deregistered upon destruction.
         */
        if (RT_SUCCESS(rc))
            rc = PDMDrvHlpSSMRegisterEx(pDrvIns, 0 /* version */, 0 /* cbGuess */,
                                        NULL /*pfnLivePrep*/, NULL /*pfnLiveExec*/, NULL /*pfnLiveVote*/,
                                        NULL /*pfnSavePrep*/, NULL /*pfnSaveExec*/, NULL /*pfnSaveDone*/,
                                        NULL /*pfnDonePrep*/, NULL /*pfnLoadExec*/, drvvdLoadDone);

        /* Setup the boot acceleration stuff if enabled. */
        if (RT_SUCCESS(rc) && pThis->fBootAccelEnabled)
        {
            pThis->cbDisk = VDGetSize(pThis->pDisk, VD_LAST_IMAGE);
            Assert(pThis->cbDisk > 0);
            pThis->pbData = (uint8_t *)RTMemAllocZ(pThis->cbBootAccelBuffer);
            if (pThis->pbData)
            {
                pThis->fBootAccelActive = true;
                pThis->offDisk          = 0;
                pThis->cbDataValid      = 0;
                LogRel(("VD: Boot acceleration enabled\n"));
            }
            else
                LogRel(("VD: Boot acceleration, out of memory, disabled\n"));
        }

        if (   RTUuidIsNull(&pThis->Uuid)
            && pThis->enmType == PDMMEDIATYPE_HARD_DISK)
            rc = VDGetUuid(pThis->pDisk, 0, &pThis->Uuid);

        /*
         * Automatically upgrade the floppy drive if the specified one is too
         * small to represent the whole boot time image. (We cannot do this later
         * since the BIOS (and others) gets the info via CMOS.)
         *
         * This trick should make 2.88 images as well as the fake 15.6 and 63.5 MB
         * images despite the hardcoded default 1.44 drive.
         */
        if (   PDMMEDIATYPE_IS_FLOPPY(pThis->enmType)
            && pThis->pDisk)
        {
            uint64_t     const cbFloppyImg = VDGetSize(pThis->pDisk, VD_LAST_IMAGE);
            PDMMEDIATYPE const enmCfgType  = pThis->enmType;
            switch (enmCfgType)
            {
                default:
                    AssertFailed();
                case PDMMEDIATYPE_FLOPPY_360:
                    if (cbFloppyImg > 40 * 2 * 9 * 512)
                        pThis->enmType = PDMMEDIATYPE_FLOPPY_720;
                    /* fall thru */
                case PDMMEDIATYPE_FLOPPY_720:
                    if (cbFloppyImg > 80 * 2 * 14 * 512)
                        pThis->enmType = PDMMEDIATYPE_FLOPPY_1_20;
                    /* fall thru */
                case PDMMEDIATYPE_FLOPPY_1_20:
                    if (cbFloppyImg > 80 * 2 * 20 * 512)
                        pThis->enmType = PDMMEDIATYPE_FLOPPY_1_44;
                    /* fall thru */
                case PDMMEDIATYPE_FLOPPY_1_44:
                    if (cbFloppyImg > 80 * 2 * 24 * 512)
                        pThis->enmType = PDMMEDIATYPE_FLOPPY_2_88;
                    /* fall thru */
                case PDMMEDIATYPE_FLOPPY_2_88:
                    if (cbFloppyImg > 80 * 2 * 48 * 512)
                        pThis->enmType = PDMMEDIATYPE_FLOPPY_FAKE_15_6;
                    /* fall thru */
                case PDMMEDIATYPE_FLOPPY_FAKE_15_6:
                    if (cbFloppyImg > 255 * 2 * 63 * 512)
                        pThis->enmType = PDMMEDIATYPE_FLOPPY_FAKE_63_5;
                case PDMMEDIATYPE_FLOPPY_FAKE_63_5:
                    if (cbFloppyImg > 255 * 2 * 255 * 512)
                        LogRel(("Warning: Floppy image is larger that 63.5 MB! (%llu bytes)\n", cbFloppyImg));
                    break;
            }
            if (pThis->enmType != enmCfgType)
                LogRel(("DrvVD: Automatically upgraded floppy drive from %s to %s to better support the %u byte image\n",
                        drvvdGetTypeName(enmCfgType), drvvdGetTypeName(pThis->enmType), cbFloppyImg));
        }
    } /* !fEmptyDrive */

    if (RT_FAILURE(rc))
    {
        if (RT_VALID_PTR(pszName))
            MMR3HeapFree(pszName);
        if (RT_VALID_PTR(pszFormat))
            MMR3HeapFree(pszFormat);
        /* drvvdDestruct does the rest. */
    }

    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}

/**
 * VBox disk container media driver registration record.
 */
const PDMDRVREG g_DrvVD =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szName */
    "VD",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "Generic VBox disk media driver.",
    /* fFlags */
    PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass. */
    PDM_DRVREG_CLASS_MEDIA,
    /* cMaxInstances */
    ~0U,
    /* cbInstance */
    sizeof(VBOXDISK),
    /* pfnConstruct */
    drvvdConstruct,
    /* pfnDestruct */
    drvvdDestruct,
    /* pfnRelocate */
    NULL,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    drvvdPowerOn,
    /* pfnReset */
    drvvdReset,
    /* pfnSuspend */
    drvvdSuspend,
    /* pfnResume */
    drvvdResume,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnPowerOff */
    drvvdPowerOff,
    /* pfnSoftReset */
    NULL,
    /* u32EndVersion */
    PDM_DRVREG_VERSION
};

