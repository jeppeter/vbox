/* $Id$ */
/** @file
 * VirtualBox USB Proxy Service, Solaris Specialization.
 */

/*
 * Copyright (C) 2005-2014 Oracle Corporation
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
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include "USBProxyBackend.h"
#include "Logging.h"

#include <VBox/usb.h>
#include <VBox/usblib.h>
#include <VBox/err.h>
#include <iprt/semaphore.h>
#include <iprt/path.h>

#include <sys/usb/usba.h>
#include <syslog.h>


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/
static int solarisWalkDeviceNode(di_node_t Node, void *pvArg);
static void solarisFreeUSBDevice(PUSBDEVICE pDevice);
static USBDEVICESTATE solarisDetermineUSBDeviceState(PUSBDEVICE pDevice, di_node_t Node);


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
typedef struct USBDEVICELIST
{
    PUSBDEVICE pHead;
    PUSBDEVICE pTail;
} USBDEVICELIST;
typedef USBDEVICELIST *PUSBDEVICELIST;


/**
 * Initialize data members.
 */
USBProxyBackendSolaris::USBProxyBackendSolaris(USBProxyService *aUsbProxyService)
    : USBProxyBackend(aUsbProxyService), mUSBLibInitialized(false)
{
    LogFlowThisFunc(("aUsbProxyService=%p\n", aUsbProxyService));
}


/**
 * Initializes the object (called right after construction).
 *
 * @returns VBox status code.
 */
int USBProxyBackendSolaris::init(void)
{
    /*
     * Create semaphore.
     */
    int rc = RTSemEventCreate(&mNotifyEventSem);
    if (RT_FAILURE(rc))
        return rc;

    /*
     * Initialize the USB library.
     */
    rc = USBLibInit();
    if (RT_FAILURE(rc))
    {
        RTSemEventDestroy(mNotifyEventSem);
        return rc;
    }

    mUSBLibInitialized = true;

    /*
     * Start the poller thread.
     */
    start();
    return VINF_SUCCESS;
}


/**
 * Stop all service threads and free the device chain.
 */
USBProxyBackendSolaris::~USBProxyBackendSolaris()
{
    LogFlowThisFunc(("destruct\n"));

    /*
     * Stop the service.
     */
    if (isActive())
        stop();

    /*
     * Terminate the USB library
     */
    if (mUSBLibInitialized)
    {
        USBLibTerm();
        mUSBLibInitialized = false;
    }

    RTSemEventDestroy(mNotifyEventSem);
    mNotifyEventSem = NULL;
}


void *USBProxyBackendSolaris::insertFilter(PCUSBFILTER aFilter)
{
    return USBLibAddFilter(aFilter);
}


void USBProxyBackendSolaris::removeFilter(void *pvID)
{
    USBLibRemoveFilter(pvID);
}


int USBProxyBackendSolaris::wait(RTMSINTERVAL aMillies)
{
    return RTSemEventWait(mNotifyEventSem, aMillies < 1000 ? 1000 : RT_MIN(aMillies, 5000));
}


int USBProxyBackendSolaris::interruptWait(void)
{
    return RTSemEventSignal(mNotifyEventSem);
}


PUSBDEVICE USBProxyBackendSolaris::getDevices(void)
{
    USBDEVICELIST DevList;
    DevList.pHead = NULL;
    DevList.pTail = NULL;
    di_node_t RootNode = di_init("/", DINFOCPYALL);
    if (RootNode != DI_NODE_NIL)
        di_walk_node(RootNode, DI_WALK_CLDFIRST, &DevList, solarisWalkDeviceNode);

    di_fini(RootNode);
    return DevList.pHead;
}


static int solarisWalkDeviceNode(di_node_t Node, void *pvArg)
{
    PUSBDEVICELIST pList = (PUSBDEVICELIST)pvArg;
    AssertPtrReturn(pList, DI_WALK_TERMINATE);

    /*
     * Check if it's a USB device in the first place.
     */
    bool fUSBDevice = false;
    char *pszCompatNames = NULL;
    int cCompatNames = di_compatible_names(Node, &pszCompatNames);
    for (int i = 0; i < cCompatNames; i++, pszCompatNames += strlen(pszCompatNames) + 1)
        if (!strncmp(pszCompatNames, RT_STR_TUPLE("usb")))
        {
            fUSBDevice = true;
            break;
        }

    if (!fUSBDevice)
        return DI_WALK_CONTINUE;

    /*
     * Check if it's a device node or interface.
     */
    int *pInt = NULL;
    char *pStr = NULL;
    int rc = DI_WALK_CONTINUE;
    if (di_prop_lookup_ints(DDI_DEV_T_ANY, Node, "interface", &pInt) < 0)
    {
        /* It's a device node. */
        char *pszDevicePath = di_devfs_path(Node);
        PUSBDEVICE pCur = (PUSBDEVICE)RTMemAllocZ(sizeof(*pCur));
        if (!pCur)
        {
            LogRel(("USBService: failed to allocate %d bytes for PUSBDEVICE.\n", sizeof(*pCur)));
            return DI_WALK_TERMINATE;
        }

        bool fValidDevice = false;
        do
        {
            AssertBreak(pszDevicePath);

            char *pszDriverName = di_driver_name(Node);

            /*
             * Skip hubs
             */
            if (   pszDriverName
                && !strcmp(pszDriverName, "hubd"))
            {
                break;
            }

            /*
             * Mandatory.
             * snv_85 and above have usb-dev-descriptor node properties, but older one's do not.
             * So if we cannot obtain the entire device descriptor, we try falling back to the
             * individual properties (those must not fail, if it does we drop the device).
             */
            uchar_t *pDevData = NULL;
            int cbProp = di_prop_lookup_bytes(DDI_DEV_T_ANY, Node, "usb-dev-descriptor", &pDevData);
            if (   cbProp > 0
                && pDevData)
            {
                usb_dev_descr_t *pDeviceDescriptor = (usb_dev_descr_t *)pDevData;
                pCur->bDeviceClass = pDeviceDescriptor->bDeviceClass;
                pCur->bDeviceSubClass = pDeviceDescriptor->bDeviceSubClass;
                pCur->bDeviceProtocol = pDeviceDescriptor->bDeviceProtocol;
                pCur->idVendor = pDeviceDescriptor->idVendor;
                pCur->idProduct = pDeviceDescriptor->idProduct;
                pCur->bcdDevice = pDeviceDescriptor->bcdDevice;
                pCur->bcdUSB = pDeviceDescriptor->bcdUSB;
                pCur->bNumConfigurations = pDeviceDescriptor->bNumConfigurations;
                pCur->fPartialDescriptor = false;
            }
            else
            {
                AssertBreak(di_prop_lookup_ints(DDI_DEV_T_ANY, Node, "usb-vendor-id", &pInt) > 0);
                pCur->idVendor = (uint16_t)*pInt;

                AssertBreak(di_prop_lookup_ints(DDI_DEV_T_ANY, Node, "usb-product-id", &pInt) > 0);
                pCur->idProduct = (uint16_t)*pInt;

                AssertBreak(di_prop_lookup_ints(DDI_DEV_T_ANY, Node, "usb-revision-id", &pInt) > 0);
                pCur->bcdDevice = (uint16_t)*pInt;

                AssertBreak(di_prop_lookup_ints(DDI_DEV_T_ANY, Node, "usb-release", &pInt) > 0);
                pCur->bcdUSB = (uint16_t)*pInt;

                pCur->fPartialDescriptor = true;
            }

            char *pszPortAddr = di_bus_addr(Node);
            if (pszPortAddr)
                pCur->bPort = RTStrToUInt8(pszPortAddr);     /* Bus & Port are mixed up (kernel driver/userland) */
            else
                pCur->bPort = 0;

            char szBuf[PATH_MAX + 48];
            RTStrPrintf(szBuf, sizeof(szBuf), "%#x:%#x:%d:%s", pCur->idVendor, pCur->idProduct, pCur->bcdDevice, pszDevicePath);
            pCur->pszAddress = RTStrDup(szBuf);
            AssertBreak(pCur->pszAddress);

            pCur->pszDevicePath = RTStrDup(pszDevicePath);
            AssertBreak(pCur->pszDevicePath);

            pCur->pszBackend = RTStrDup("host");
            AssertBreak(pCur->pszBackend);

            /*
             * Optional (some devices don't have all these)
             */
            if (di_prop_lookup_strings(DDI_DEV_T_ANY, Node, "usb-product-name", &pStr) > 0)
                pCur->pszProduct = RTStrDup(pStr);

            if (di_prop_lookup_strings(DDI_DEV_T_ANY, Node, "usb-vendor-name", &pStr) > 0)
                pCur->pszManufacturer = RTStrDup(pStr);

            if (di_prop_lookup_strings(DDI_DEV_T_ANY, Node, "usb-serialno", &pStr) > 0)
                pCur->pszSerialNumber = RTStrDup(pStr);

            if (pCur->bcdUSB == 0x300)
                pCur->enmSpeed = USBDEVICESPEED_SUPER;
            else if (di_prop_lookup_ints(DDI_DEV_T_ANY, Node, "low-speed", &pInt) >= 0)
                pCur->enmSpeed = USBDEVICESPEED_LOW;
            else if (di_prop_lookup_ints(DDI_DEV_T_ANY, Node, "high-speed", &pInt) >= 0)
                pCur->enmSpeed = USBDEVICESPEED_HIGH;
            else
                pCur->enmSpeed = USBDEVICESPEED_FULL;

            /* Determine state of the USB device. */
            pCur->enmState = solarisDetermineUSBDeviceState(pCur, Node);

            /*
             * Valid device, add it to the list.
             */
            fValidDevice = true;
            pCur->pPrev = pList->pTail;
            if (pList->pTail)
                pList->pTail = pList->pTail->pNext = pCur;
            else
                pList->pTail = pList->pHead = pCur;

            rc = DI_WALK_CONTINUE;
        } while (0);

        di_devfs_path_free(pszDevicePath);
        if (!fValidDevice)
            solarisFreeUSBDevice(pCur);
    }
    return rc;
}


static USBDEVICESTATE solarisDetermineUSBDeviceState(PUSBDEVICE pDevice, di_node_t Node)
{
    char *pszDriverName = di_driver_name(Node);

    /* Not possible unless a user explicitly unbinds the default driver. */
    if (!pszDriverName)
        return USBDEVICESTATE_UNUSED;

    if (!strncmp(pszDriverName, RT_STR_TUPLE(VBOXUSB_DRIVER_NAME)))
        return USBDEVICESTATE_HELD_BY_PROXY;

    NOREF(pDevice);
    return USBDEVICESTATE_USED_BY_HOST_CAPTURABLE;
}


int USBProxyBackendSolaris::captureDevice(HostUSBDevice *aDevice)
{
    /*
     * Check preconditions.
     */
    AssertReturn(aDevice, VERR_GENERAL_FAILURE);
    AssertReturn(!aDevice->isWriteLockOnCurrentThread(), VERR_GENERAL_FAILURE);

    AutoReadLock devLock(aDevice COMMA_LOCKVAL_SRC_POS);
    LogFlowThisFunc(("aDevice=%s\n", aDevice->i_getName().c_str()));

    Assert(aDevice->i_getUnistate() == kHostUSBDeviceState_Capturing);
    AssertReturn(aDevice->i_getUsbData(), VERR_INVALID_POINTER);

    /*
     * Create a one-shot capture filter for the device and reset the device.
     */
    USBFILTER Filter;
    USBFilterInit(&Filter, USBFILTERTYPE_ONESHOT_CAPTURE);
    initFilterFromDevice(&Filter, aDevice);

    void *pvId = USBLibAddFilter(&Filter);
    if (!pvId)
    {
        LogRel(("USBService: failed to add filter\n"));
        return VERR_GENERAL_FAILURE;
    }

    PUSBDEVICE pDev = aDevice->i_getUsbData();
    int rc = USBLibResetDevice(pDev->pszDevicePath, true);
    if (RT_SUCCESS(rc))
        aDevice->i_setBackendUserData(pvId);
    else
    {
        USBLibRemoveFilter(pvId);
        pvId = NULL;
    }
    LogFlowThisFunc(("returns %Rrc pvId=%p\n", rc, pvId));
    return rc;
}


void USBProxyBackendSolaris::captureDeviceCompleted(HostUSBDevice *aDevice, bool aSuccess)
{
    AssertReturnVoid(aDevice->isWriteLockOnCurrentThread());
    /*
     * Remove the one-shot filter if necessary.
     */
    LogFlowThisFunc(("aDevice=%s aSuccess=%RTbool mOneShotId=%p\n", aDevice->i_getName().c_str(), aSuccess, aDevice->i_getBackendUserData()));
    if (!aSuccess && aDevice->i_getBackendUserData())
        USBLibRemoveFilter(aDevice->i_getBackendUserData());
    aDevice->i_setBackendUserData(NULL);
}


int USBProxyBackendSolaris::releaseDevice(HostUSBDevice *aDevice)
{
    /*
     * Check preconditions.
     */
    AssertReturn(aDevice, VERR_GENERAL_FAILURE);
    AssertReturn(!aDevice->isWriteLockOnCurrentThread(), VERR_GENERAL_FAILURE);

    AutoReadLock devLock(aDevice COMMA_LOCKVAL_SRC_POS);
    LogFlowThisFunc(("aDevice=%s\n", aDevice->i_getName().c_str()));

    Assert(aDevice->i_getUnistate() == kHostUSBDeviceState_ReleasingToHost);
    AssertReturn(aDevice->i_getUsbData(), VERR_INVALID_POINTER);

    /*
     * Create a one-shot ignore filter for the device and reset it.
     */
    USBFILTER Filter;
    USBFilterInit(&Filter, USBFILTERTYPE_ONESHOT_IGNORE);
    initFilterFromDevice(&Filter, aDevice);

    void *pvId = USBLibAddFilter(&Filter);
    if (!pvId)
    {
        LogRel(("USBService: Adding ignore filter failed!\n"));
        return VERR_GENERAL_FAILURE;
    }

    PUSBDEVICE pDev = aDevice->i_getUsbData();
    int rc = USBLibResetDevice(pDev->pszDevicePath, true /* Re-attach */);
    if (RT_SUCCESS(rc))
        aDevice->i_setBackendUserData(pvId);
    else
    {
        USBLibRemoveFilter(pvId);
        pvId = NULL;
    }
    LogFlowThisFunc(("returns %Rrc pvId=%p\n", rc, pvId));
    return rc;
}


void USBProxyBackendSolaris::releaseDeviceCompleted(HostUSBDevice *aDevice, bool aSuccess)
{
    AssertReturnVoid(aDevice->isWriteLockOnCurrentThread());
    /*
     * Remove the one-shot filter if necessary.
     */
    LogFlowThisFunc(("aDevice=%s aSuccess=%RTbool mOneShotId=%p\n", aDevice->i_getName().c_str(), aSuccess, aDevice->i_getBackendUserData()));
    if (!aSuccess && aDevice->i_getBackendUserData())
        USBLibRemoveFilter(aDevice->i_getBackendUserData());
    aDevice->i_setBackendUserData(NULL);
}


bool USBProxyBackendSolaris::updateDeviceState(HostUSBDevice *aDevice, PUSBDEVICE aUSBDevice, bool *aRunFilters,
                                               SessionMachine **aIgnoreMachine)
{
    AssertReturn(aDevice, false);
    AssertReturn(!aDevice->isWriteLockOnCurrentThread(), false);
    return USBProxyBackend::updateDeviceState(aDevice, aUSBDevice, aRunFilters, aIgnoreMachine);
}

/**
 * Wrapper called by walkDeviceNode.
 *
 * @param   pDevice    The USB device to free.
 */
void solarisFreeUSBDevice(PUSBDEVICE pDevice)
{
    USBProxyBackend::freeDevice(pDevice);
}
