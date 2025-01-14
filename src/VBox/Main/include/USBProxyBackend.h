/* $Id$ */
/** @file
 * VirtualBox USB Proxy Backend (base) class.
 */

/*
 * Copyright (C) 2005-2015 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


#ifndef ____H_USBPROXYBACKEND
#define ____H_USBPROXYBACKEND

#include <VBox/usb.h>
#include <VBox/usbfilter.h>

#include <iprt/socket.h>
#include <iprt/poll.h>
#include <iprt/semaphore.h>

#include "VirtualBoxBase.h"
#include "VirtualBoxImpl.h"
#include "HostUSBDeviceImpl.h"
class USBProxyService;

/**
 * Base class for the USB Proxy Backend.
 */
class USBProxyBackend
    : public VirtualBoxTranslatable
{
public:
    USBProxyBackend(USBProxyService *pUsbProxyService);
    virtual int init(void);
    virtual ~USBProxyBackend();

    /**
     * Override of the default locking class to be used for validating lock
     * order with the standard member lock handle.
     */
    virtual VBoxLockingClass getLockingClass() const
    {
        // the USB proxy service uses the Host object lock, so return the
        // same locking class as the host
        return LOCKCLASS_HOSTOBJECT;
    }

    bool isActive(void);

    RWLockHandle *lockHandle() const;

    /** @name Interface for the USBController and the Host object.
     * @{ */
    virtual void *insertFilter(PCUSBFILTER aFilter);
    virtual void removeFilter(void *aId);
    /** @} */

    /** @name Interfaces for the HostUSBDevice
     * @{ */
    virtual int captureDevice(HostUSBDevice *aDevice);
    virtual void captureDeviceCompleted(HostUSBDevice *aDevice, bool aSuccess);
    /** @todo unused */
    virtual void detachingDevice(HostUSBDevice *aDevice);
    virtual int releaseDevice(HostUSBDevice *aDevice);
    virtual void releaseDeviceCompleted(HostUSBDevice *aDevice, bool aSuccess);
    /** @} */

    static void freeDevice(PUSBDEVICE pDevice);

    HRESULT runAllFiltersOnDevice(ComObjPtr<HostUSBDevice> &aDevice,
                                  SessionMachinesList &llOpenedMachines,
                                  SessionMachine *aIgnoreMachine);
    bool runMachineFilters(SessionMachine *aMachine, ComObjPtr<HostUSBDevice> &aDevice);

    virtual void deviceAdded(ComObjPtr<HostUSBDevice> &aDevice, SessionMachinesList &llOpenedMachines, PUSBDEVICE aUSBDevice);
    virtual void deviceRemoved(ComObjPtr<HostUSBDevice> &aDevice);
    virtual void deviceChanged(ComObjPtr<HostUSBDevice> &aDevice, SessionMachinesList *pllOpenedMachines, SessionMachine *aIgnoreMachine);
    virtual bool updateDeviceState(HostUSBDevice *aDevice, PUSBDEVICE aUSBDevice, bool *aRunFilters, SessionMachine **aIgnoreMachine);

protected:
    int start(void);
    int stop(void);
    virtual void serviceThreadInit(void);
    virtual void serviceThreadTerm(void);

    virtual int wait(RTMSINTERVAL aMillies);
    virtual int interruptWait(void);
    virtual PUSBDEVICE getDevices(void);
    bool updateDeviceStateFake(HostUSBDevice *aDevice, PUSBDEVICE aUSBDevice, bool *aRunFilters, SessionMachine **aIgnoreMachine);

    static HRESULT setError(HRESULT aResultCode, const char *aText, ...);

    static void initFilterFromDevice(PUSBFILTER aFilter, HostUSBDevice *aDevice);
    static void freeDeviceMembers(PUSBDEVICE pDevice);

private:

    static DECLCALLBACK(int) serviceThread(RTTHREAD Thread, void *pvUser);

protected:
    /** Pointer to the owning USB Proxy Service object. */
    USBProxyService *m_pUsbProxyService;
    /** Thread handle of the service thread. */
    RTTHREAD         mThread;
    /** Flag which stop() sets to cause serviceThread to return. */
    bool volatile    mTerminate;
};


# ifdef RT_OS_DARWIN
#  include <VBox/param.h>
#  undef PAGE_SHIFT
#  undef PAGE_SIZE
#  define OSType Carbon_OSType
#  include <Carbon/Carbon.h>
#  undef OSType

/**
 * The Darwin hosted USB Proxy Backend.
 */
class USBProxyBackendDarwin : public USBProxyBackend
{
public:
    USBProxyBackendDarwin(USBProxyService *pUsbProxyService);
    int init(void);
    ~USBProxyBackendDarwin();

    virtual void *insertFilter(PCUSBFILTER aFilter);
    virtual void removeFilter(void *aId);

    virtual int captureDevice(HostUSBDevice *aDevice);
    virtual void captureDeviceCompleted(HostUSBDevice *aDevice, bool aSuccess);
    /** @todo unused */
    virtual void detachingDevice(HostUSBDevice *aDevice);
    virtual int releaseDevice(HostUSBDevice *aDevice);
    virtual void releaseDeviceCompleted(HostUSBDevice *aDevice, bool aSuccess);

protected:
    virtual int wait(RTMSINTERVAL aMillies);
    virtual int interruptWait (void);
    virtual PUSBDEVICE getDevices (void);
    virtual void serviceThreadInit (void);
    virtual void serviceThreadTerm (void);
    virtual bool updateDeviceState (HostUSBDevice *aDevice, PUSBDEVICE aUSBDevice, bool *aRunFilters, SessionMachine **aIgnoreMachine);

private:
    /** Reference to the runloop of the service thread.
     * This is NULL if the service thread isn't running. */
    CFRunLoopRef mServiceRunLoopRef;
    /** The opaque value returned by DarwinSubscribeUSBNotifications. */
    void *mNotifyOpaque;
    /** A hack to work around the problem with the usb device enumeration
     * not including newly attached devices. */
    bool mWaitABitNextTime;
    /** Whether we've successfully initialized the USBLib and should call USBLibTerm in the destructor. */
    bool mUSBLibInitialized;
};
# endif /* RT_OS_DARWIN */


# ifdef RT_OS_LINUX
#  include <stdio.h>
#  ifdef VBOX_USB_WITH_SYSFS
#   include <HostHardwareLinux.h>
#  endif

/**
 * The Linux hosted USB Proxy Backend.
 */
class USBProxyBackendLinux: public USBProxyBackend
{
public:
    USBProxyBackendLinux(USBProxyService *pUsbProxyService);
    int init(void);
    ~USBProxyBackendLinux();

    virtual int captureDevice(HostUSBDevice *aDevice);
    virtual int releaseDevice(HostUSBDevice *aDevice);

protected:
    int initUsbfs(void);
    int initSysfs(void);
    void doUsbfsCleanupAsNeeded(void);
    virtual int wait(RTMSINTERVAL aMillies);
    virtual int interruptWait(void);
    virtual PUSBDEVICE getDevices(void);
    virtual void deviceAdded(ComObjPtr<HostUSBDevice> &aDevice, SessionMachinesList &llOpenedMachines, PUSBDEVICE aUSBDevice);
    virtual bool updateDeviceState(HostUSBDevice *aDevice, PUSBDEVICE aUSBDevice, bool *aRunFilters, SessionMachine **aIgnoreMachine);

private:
    int waitUsbfs(RTMSINTERVAL aMillies);
    int waitSysfs(RTMSINTERVAL aMillies);

private:
    /** File handle to the '/proc/bus/usb/devices' file. */
    RTFILE mhFile;
    /** Pipe used to interrupt wait(), the read end. */
    RTPIPE mhWakeupPipeR;
    /** Pipe used to interrupt wait(), the write end. */
    RTPIPE mhWakeupPipeW;
    /** The root of usbfs. */
    Utf8Str mDevicesRoot;
    /** Whether we're using <mUsbfsRoot>/devices or /sys/whatever. */
    bool mUsingUsbfsDevices;
    /** Number of 500ms polls left to do. See usbDeterminState for details. */
    unsigned mUdevPolls;
#  ifdef VBOX_USB_WITH_SYSFS
    /** Object used for polling for hotplug events from hal. */
    VBoxMainHotplugWaiter *mpWaiter;
#  endif
};
# endif /* RT_OS_LINUX */


# ifdef RT_OS_OS2
#  include <usbcalls.h>

/**
 * The Linux hosted USB Proxy Backend.
 */
class USBProxyBackendOs2 : public USBProxyBackend
{
public:
    USBProxyBackendOs2 (USBProxyService *pUsbProxyService);
    /// @todo virtual int init(void);
    ~USBProxyBackendOs2();

    virtual int captureDevice (HostUSBDevice *aDevice);
    virtual int releaseDevice (HostUSBDevice *aDevice);

protected:
    virtual int wait(RTMSINTERVAL aMillies);
    virtual int interruptWait(void);
    virtual PUSBDEVICE getDevices(void);
    int addDeviceToChain(PUSBDEVICE pDev, PUSBDEVICE *ppFirst, PUSBDEVICE **pppNext, int rc);
    virtual bool updateDeviceState(HostUSBDevice *aDevice, PUSBDEVICE aUSBDevice, bool *aRunFilters, SessionMachine **aIgnoreMachine);

private:
    /** The notification event semaphore */
    HEV mhev;
    /** The notification id. */
    USBNOTIFY mNotifyId;
    /** The usbcalls.dll handle. */
    HMODULE mhmod;
    /** UsbRegisterChangeNotification */
    APIRET (APIENTRY *mpfnUsbRegisterChangeNotification)(PUSBNOTIFY, HEV, HEV);
    /** UsbDeregisterNotification */
    APIRET (APIENTRY *mpfnUsbDeregisterNotification)(USBNOTIFY);
    /** UsbQueryNumberDevices */
    APIRET (APIENTRY *mpfnUsbQueryNumberDevices)(PULONG);
    /** UsbQueryDeviceReport */
    APIRET (APIENTRY *mpfnUsbQueryDeviceReport)(ULONG, PULONG, PVOID);
};
# endif /* RT_OS_LINUX */


# ifdef RT_OS_SOLARIS
#  include <libdevinfo.h>

/**
 * The Solaris hosted USB Proxy Backend.
 */
class USBProxyBackendSolaris : public USBProxyBackend
{
public:
    USBProxyBackendSolaris(USBProxyService *pUsbProxyService);
    int init(void);
    ~USBProxyBackendSolaris();

    virtual void *insertFilter (PCUSBFILTER aFilter);
    virtual void removeFilter (void *aID);

    virtual int captureDevice (HostUSBDevice *aDevice);
    virtual int releaseDevice (HostUSBDevice *aDevice);
    virtual void captureDeviceCompleted(HostUSBDevice *aDevice, bool aSuccess);
    virtual void releaseDeviceCompleted(HostUSBDevice *aDevice, bool aSuccess);

protected:
    virtual int wait(RTMSINTERVAL aMillies);
    virtual int interruptWait(void);
    virtual PUSBDEVICE getDevices(void);
    virtual bool updateDeviceState(HostUSBDevice *aDevice, PUSBDEVICE aUSBDevice, bool *aRunFilters, SessionMachine **aIgnoreMachine);

private:
    RTSEMEVENT mNotifyEventSem;
    /** Whether we've successfully initialized the USBLib and should call USBLibTerm in the destructor. */
    bool mUSBLibInitialized;
};
#endif  /* RT_OS_SOLARIS */


# ifdef RT_OS_WINDOWS
/**
 * The Windows hosted USB Proxy Backend.
 */
class USBProxyBackendWindows : public USBProxyBackend
{
public:
    USBProxyBackendWindows(USBProxyService *pUsbProxyService);
    int init(void);
    ~USBProxyBackendWindows();

    virtual void *insertFilter (PCUSBFILTER aFilter);
    virtual void removeFilter (void *aID);

    virtual int captureDevice (HostUSBDevice *aDevice);
    virtual int releaseDevice (HostUSBDevice *aDevice);

protected:
    virtual int wait(RTMSINTERVAL aMillies);
    virtual int interruptWait(void);
    virtual PUSBDEVICE getDevices(void);
    virtual bool updateDeviceState(HostUSBDevice *aDevice, PUSBDEVICE aUSBDevice, bool *aRunFilters, SessionMachine **aIgnoreMachine);

private:

    HANDLE mhEventInterrupt;
};
# endif /* RT_OS_WINDOWS */

# ifdef RT_OS_FREEBSD
/**
 * The FreeBSD hosted USB Proxy Backend.
 */
class USBProxyBackendFreeBSD : public USBProxyBackend
{
public:
    USBProxyBackendFreeBSD(USBProxyService *pUsbProxyService);
    int init(void);
    ~USBProxyBackendFreeBSD();

    virtual int captureDevice(HostUSBDevice *aDevice);
    virtual int releaseDevice(HostUSBDevice *aDevice);

protected:
    int initUsbfs(void);
    int initSysfs(void);
    virtual int wait(RTMSINTERVAL aMillies);
    virtual int interruptWait(void);
    virtual PUSBDEVICE getDevices(void);
    int addDeviceToChain(PUSBDEVICE pDev, PUSBDEVICE *ppFirst, PUSBDEVICE **pppNext, int rc);
    virtual void deviceAdded(ComObjPtr<HostUSBDevice> &aDevice, SessionMachinesList &llOpenedMachines, PUSBDEVICE aUSBDevice);
    virtual bool updateDeviceState(HostUSBDevice *aDevice, PUSBDEVICE aUSBDevice, bool *aRunFilters, SessionMachine **aIgnoreMachine);

private:
    RTSEMEVENT mNotifyEventSem;
};
# endif /* RT_OS_FREEBSD */

/**
 * USB/IP Proxy receive state.
 */
typedef enum USBIPRECVSTATE
{
    /** Invalid state. */
    kUsbIpRecvState_Invalid = 0,
    /** There is no request waiting for an answer. */
    kUsbIpRecvState_None,
    /** Waiting for the complete reception of UsbIpRetDevList. */
    kUsbIpRecvState_Hdr,
    /** Waiting for the complete reception of a UsbIpExportedDevice structure. */
    kUsbIpRecvState_ExportedDevice,
    /** Waiting for a complete reception a UsbIpDeviceInterface strucutre to skip. */
    kUsbIpRecvState_DeviceInterface,
    /** 32bit hack. */
    kUsbIpRecvState_32Bit_Hack = 0x7fffffff
} USBIPRECVSTATE;
/** Pointer to a USB/IP receive state enum. */
typedef USBIPRECVSTATE *PUSBIPRECVSTATE;

struct UsbIpExportedDevice;

/**
 * The USB/IP Proxy Backend.
 */
class USBProxyBackendUsbIp: public USBProxyBackend
{
public:
    USBProxyBackendUsbIp(USBProxyService *pUsbProxyService);
    int init(void);
    ~USBProxyBackendUsbIp();

    virtual int captureDevice(HostUSBDevice *aDevice);
    virtual int releaseDevice(HostUSBDevice *aDevice);

protected:
    virtual int wait(RTMSINTERVAL aMillies);
    virtual int interruptWait(void);
    virtual PUSBDEVICE getDevices(void);
    virtual bool updateDeviceState(HostUSBDevice *aDevice, PUSBDEVICE aUSBDevice, bool *aRunFilters, SessionMachine **aIgnoreMachine);

private:
    int  updateDeviceList(bool *pfDeviceListChanged);
    bool hasDevListChanged(PUSBDEVICE pDevices);
    void freeDeviceList(PUSBDEVICE pHead);
    void resetRecvState();
    int  reconnect();
    void disconnect();
    int  startListExportedDevicesReq();
    void advanceState(USBIPRECVSTATE enmRecvState);
    int  receiveData();
    int  processData();
    int  addDeviceToList(UsbIpExportedDevice *pDev);

    struct Data;            // opaque data struct, defined in USBProxyBackendUsbIp.cpp
    Data *m;
};

#endif /* !____H_USBPROXYBACKEND */

