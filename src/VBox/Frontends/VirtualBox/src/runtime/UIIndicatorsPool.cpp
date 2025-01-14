/* $Id$ */
/** @file
 * VBox Qt GUI - UIIndicatorsPool class implementation.
 */

/*
 * Copyright (C) 2010-2014 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifdef VBOX_WITH_PRECOMPILED_HEADERS
# include <precomp.h>
#else  /* !VBOX_WITH_PRECOMPILED_HEADERS */

/* Qt includes: */
# include <QTimer>
# include <QPainter>
# include <QHBoxLayout>

/* GUI includes: */
# include "UIIndicatorsPool.h"
# include "QIWithRetranslateUI.h"
# include "UIExtraDataManager.h"
# include "UIMachineDefs.h"
# include "UIConverter.h"
# include "UIAnimationFramework.h"
# include "UISession.h"
# include "UIMedium.h"
# include "UIIconPool.h"
# include "UIHostComboEditor.h"
# include "QIStatusBarIndicator.h"
# include "VBoxGlobal.h"

/* COM includes: */
# include "CConsole.h"
# include "CMachine.h"
# include "CSystemProperties.h"
# include "CMachineDebugger.h"
# include "CGuest.h"
# include "CStorageController.h"
# include "CMediumAttachment.h"
# include "CNetworkAdapter.h"
# include "CUSBController.h"
# include "CUSBDeviceFilters.h"
# include "CUSBDevice.h"
# include "CSharedFolder.h"
# include "CVRDEServer.h"

/* Other VBox includes: */
# include <iprt/time.h>

#endif /* !VBOX_WITH_PRECOMPILED_HEADERS */



/** QIStateStatusBarIndicator extension for Runtime UI. */
class UISessionStateStatusBarIndicator : public QIWithRetranslateUI<QIStateStatusBarIndicator>
{
    Q_OBJECT;

public:

    /** Constructor which remembers passed @a session object. */
    UISessionStateStatusBarIndicator(UISession *pSession) : m_pSession(pSession) {}

    /** Abstract update routine. */
    virtual void updateAppearance() = 0;

protected:

    /** Holds the session UI reference. */
    UISession *m_pSession;
};

/** UISessionStateStatusBarIndicator extension for Runtime UI: Hard-drive indicator. */
class UIIndicatorHardDrive : public UISessionStateStatusBarIndicator
{
    Q_OBJECT;

public:

    /** Constructor, passes @a pSession to the UISessionStateStatusBarIndicator constructor. */
    UIIndicatorHardDrive(UISession *pSession)
        : UISessionStateStatusBarIndicator(pSession)
    {
        /* Assign state-icons: */
        setStateIcon(KDeviceActivity_Idle,    UIIconPool::iconSet(":/hd_16px.png"));
        setStateIcon(KDeviceActivity_Reading, UIIconPool::iconSet(":/hd_read_16px.png"));
        setStateIcon(KDeviceActivity_Writing, UIIconPool::iconSet(":/hd_write_16px.png"));
        setStateIcon(KDeviceActivity_Null,    UIIconPool::iconSet(":/hd_disabled_16px.png"));
        /* Translate finally: */
        retranslateUi();
    }

private:

    /** Retranslation routine. */
    void retranslateUi()
    {
        updateAppearance();
    }

    /** Update routine. */
    void updateAppearance()
    {
        /* Get machine: */
        const CMachine machine = m_pSession->machine();

        /* Prepare tool-tip: */
        QString strToolTip = QApplication::translate("UIIndicatorsPool",
                                                     "<p style='white-space:pre'><nobr>Indicates the activity "
                                                     "of the hard disks:</nobr>%1</p>", "HDD tooltip");
        QString strFullData;

        /* Enumerate all the controllers: */
        bool fAttachmentsPresent = false;
        foreach (const CStorageController &controller, machine.GetStorageControllers())
        {
            QString strAttData;
            /* Enumerate all the attachments: */
            foreach (const CMediumAttachment &attachment, machine.GetMediumAttachmentsOfController(controller.GetName()))
            {
                /* Skip unrelated attachments: */
                if (attachment.GetType() != KDeviceType_HardDisk)
                    continue;
                /* Append attachment data: */
                strAttData += QString("<br>&nbsp;<nobr>%1:&nbsp;%2</nobr>")
                    .arg(gpConverter->toString(StorageSlot(controller.GetBus(), attachment.GetPort(), attachment.GetDevice())))
                    .arg(UIMedium(attachment.GetMedium(), UIMediumType_HardDisk).location());
                fAttachmentsPresent = true;
            }
            /* Append controller data: */
            if (!strAttData.isNull())
                strFullData += QString("<br><nobr><b>%1</b></nobr>").arg(controller.GetName()) + strAttData;
        }

        /* Hide indicator if there are no attachments: */
        if (!fAttachmentsPresent)
            hide();

        /* Update tool-tip: */
        setToolTip(strToolTip.arg(strFullData));
        /* Update indicator state: */
        setState(fAttachmentsPresent ? KDeviceActivity_Idle : KDeviceActivity_Null);
    }
};

/** UISessionStateStatusBarIndicator extension for Runtime UI: Optical-drive indicator. */
class UIIndicatorOpticalDisks : public UISessionStateStatusBarIndicator
{
    Q_OBJECT;

public:

    /** Constructor, passes @a pSession to the UISessionStateStatusBarIndicator constructor. */
    UIIndicatorOpticalDisks(UISession *pSession)
        : UISessionStateStatusBarIndicator(pSession)
    {
        /* Assign state-icons: */
        setStateIcon(KDeviceActivity_Idle,    UIIconPool::iconSet(":/cd_16px.png"));
        setStateIcon(KDeviceActivity_Reading, UIIconPool::iconSet(":/cd_read_16px.png"));
        setStateIcon(KDeviceActivity_Writing, UIIconPool::iconSet(":/cd_write_16px.png"));
        setStateIcon(KDeviceActivity_Null,    UIIconPool::iconSet(":/cd_disabled_16px.png"));
        /* Translate finally: */
        retranslateUi();
    }

private:

    /** Retranslation routine. */
    void retranslateUi()
    {
        updateAppearance();
    }

    /** Update routine. */
    void updateAppearance()
    {
        /* Get machine: */
        const CMachine machine = m_pSession->machine();

        /* Prepare tool-tip: */
        QString strToolTip = QApplication::translate("UIIndicatorsPool",
                                                     "<p style='white-space:pre'><nobr>Indicates the activity "
                                                     "of the optical drives:</nobr>%1</p>", "CD tooltip");
        QString strFullData;

        /* Enumerate all the controllers: */
        bool fAttachmentsPresent = false;
        bool fAttachmentsMounted = false;
        foreach (const CStorageController &controller, machine.GetStorageControllers())
        {
            QString strAttData;
            /* Enumerate all the attachments: */
            foreach (const CMediumAttachment &attachment, machine.GetMediumAttachmentsOfController(controller.GetName()))
            {
                /* Skip unrelated attachments: */
                if (attachment.GetType() != KDeviceType_DVD)
                    continue;
                /* Append attachment data: */
                UIMedium vboxMedium(attachment.GetMedium(), UIMediumType_DVD);
                strAttData += QString("<br>&nbsp;<nobr>%1:&nbsp;%2</nobr>")
                    .arg(gpConverter->toString(StorageSlot(controller.GetBus(), attachment.GetPort(), attachment.GetDevice())))
                    .arg(vboxMedium.isNull() || vboxMedium.isHostDrive() ? vboxMedium.name() : vboxMedium.location());
                fAttachmentsPresent = true;
                if (!vboxMedium.isNull())
                    fAttachmentsMounted = true;
            }
            /* Append controller data: */
            if (!strAttData.isNull())
                strFullData += QString("<br><nobr><b>%1</b></nobr>").arg(controller.GetName()) + strAttData;
        }

        /* Hide indicator if there are no attachments: */
        if (!fAttachmentsPresent)
            hide();

        /* Update tool-tip: */
        setToolTip(strToolTip.arg(strFullData));
        /* Update indicator state: */
        setState(fAttachmentsMounted ? KDeviceActivity_Idle : KDeviceActivity_Null);
    }
};

/** UISessionStateStatusBarIndicator extension for Runtime UI: Floppy-drive indicator. */
class UIIndicatorFloppyDisks : public UISessionStateStatusBarIndicator
{
    Q_OBJECT;

public:

    /** Constructor, passes @a pSession to the UISessionStateStatusBarIndicator constructor. */
    UIIndicatorFloppyDisks(UISession *pSession)
        : UISessionStateStatusBarIndicator(pSession)
    {
        /* Assign state-icons: */
        setStateIcon(KDeviceActivity_Idle,    UIIconPool::iconSet(":/fd_16px.png"));
        setStateIcon(KDeviceActivity_Reading, UIIconPool::iconSet(":/fd_read_16px.png"));
        setStateIcon(KDeviceActivity_Writing, UIIconPool::iconSet(":/fd_write_16px.png"));
        setStateIcon(KDeviceActivity_Null,    UIIconPool::iconSet(":/fd_disabled_16px.png"));
        /* Translate finally: */
        retranslateUi();
    }

private:

    /** Retranslation routine. */
    void retranslateUi()
    {
        updateAppearance();
    }

    /** Update routine. */
    void updateAppearance()
    {
        /* Get machine: */
        const CMachine machine = m_pSession->machine();

        /* Prepare tool-tip: */
        QString strToolTip = QApplication::translate("UIIndicatorsPool",
                                                     "<p style='white-space:pre'><nobr>Indicates the activity "
                                                     "of the floppy drives:</nobr>%1</p>", "FD tooltip");
        QString strFullData;

        /* Enumerate all the controllers: */
        bool fAttachmentsPresent = false;
        bool fAttachmentsMounted = false;
        foreach (const CStorageController &controller, machine.GetStorageControllers())
        {
            QString strAttData;
            /* Enumerate all the attachments: */
            foreach (const CMediumAttachment &attachment, machine.GetMediumAttachmentsOfController(controller.GetName()))
            {
                /* Skip unrelated attachments: */
                if (attachment.GetType() != KDeviceType_Floppy)
                    continue;
                /* Append attachment data: */
                UIMedium vboxMedium(attachment.GetMedium(), UIMediumType_Floppy);
                strAttData += QString("<br>&nbsp;<nobr>%1:&nbsp;%2</nobr>")
                    .arg(gpConverter->toString(StorageSlot(controller.GetBus(), attachment.GetPort(), attachment.GetDevice())))
                    .arg(vboxMedium.isNull() || vboxMedium.isHostDrive() ? vboxMedium.name() : vboxMedium.location());
                fAttachmentsPresent = true;
                if (!vboxMedium.isNull())
                    fAttachmentsMounted = true;
            }
            /* Append controller data: */
            if (!strAttData.isNull())
                strFullData += QString("<br><nobr><b>%1</b></nobr>").arg(controller.GetName()) + strAttData;
        }

        /* Hide indicator if there are no attachments: */
        if (!fAttachmentsPresent)
            hide();

        /* Update tool-tip: */
        setToolTip(strToolTip.arg(strFullData));
        /* Update indicator state: */
        setState(fAttachmentsMounted ? KDeviceActivity_Idle : KDeviceActivity_Null);
    }
};

/** UISessionStateStatusBarIndicator extension for Runtime UI: Network indicator. */
class UIIndicatorNetwork : public UISessionStateStatusBarIndicator
{
    Q_OBJECT;

public:

    /** Constructor, passes @a pSession to the UISessionStateStatusBarIndicator constructor. */
    UIIndicatorNetwork(UISession *pSession)
        : UISessionStateStatusBarIndicator(pSession)
        , m_pTimerAutoUpdate(0)
        , m_cMaxNetworkAdapters(0)
    {
        /* Assign state-icons: */
        setStateIcon(KDeviceActivity_Idle,    UIIconPool::iconSet(":/nw_16px.png"));
        setStateIcon(KDeviceActivity_Reading, UIIconPool::iconSet(":/nw_read_16px.png"));
        setStateIcon(KDeviceActivity_Writing, UIIconPool::iconSet(":/nw_write_16px.png"));
        setStateIcon(KDeviceActivity_Null,    UIIconPool::iconSet(":/nw_disabled_16px.png"));
        /* Configure machine state-change listener: */
        connect(m_pSession, SIGNAL(sigMachineStateChange()),
                this, SLOT(sltHandleMachineStateChange()));
        /* Fetch maximum network adapters count: */
        const CVirtualBox vbox = vboxGlobal().virtualBox();
        const CMachine machine = m_pSession->machine();
        m_cMaxNetworkAdapters = vbox.GetSystemProperties().GetMaxNetworkAdapters(machine.GetChipsetType());
        /* Create auto-update timer: */
        m_pTimerAutoUpdate = new QTimer(this);
        if (m_pTimerAutoUpdate)
        {
            /* Configure auto-update timer: */
            connect(m_pTimerAutoUpdate, SIGNAL(timeout()), SLOT(sltUpdateNetworkIPs()));
            /* Start timer immediately if machine is running: */
            sltHandleMachineStateChange();
        }
        /* Translate finally: */
        retranslateUi();
    }

private slots:

    /** Updates auto-update timer depending on machine state. */
    void sltHandleMachineStateChange()
    {
        if (m_pSession->machineState() == KMachineState_Running)
        {
            /* Start auto-update timer otherwise: */
            m_pTimerAutoUpdate->start(5000);
            return;
        }
        /* Stop auto-update timer otherwise: */
        m_pTimerAutoUpdate->stop();
    }

    /** Updates network IP addresses. */
    void sltUpdateNetworkIPs()
    {
        updateAppearance();
    }

private:

    /** Retranslation routine. */
    void retranslateUi()
    {
        updateAppearance();
    }

    /** Update routine. */
    void updateAppearance()
    {
        /* Get machine: */
        const CMachine machine = m_pSession->machine();

        /* Prepare tool-tip: */
        QString strToolTip = QApplication::translate("UIIndicatorsPool",
                                                     "<p style='white-space:pre'><nobr>Indicates the activity of the "
                                                     "network interfaces:</nobr>%1</p>", "Network adapters tooltip");
        QString strFullData;

        /* Gather adapter properties: */
        RTTIMESPEC time;
        uint64_t u64Now = RTTimeSpecGetNano(RTTimeNow(&time));
        QString strFlags, strCount;
        LONG64 iTimestamp;
        machine.GetGuestProperty("/VirtualBox/GuestInfo/Net/Count", strCount, iTimestamp, strFlags);
        bool fPropsValid = (u64Now - iTimestamp < UINT64_C(60000000000)); /* timeout beacon */
        QStringList ipList, macList;
        if (fPropsValid)
        {
            const int cAdapters = RT_MIN(strCount.toInt(), (int)m_cMaxNetworkAdapters);
            for (int i = 0; i < cAdapters; ++i)
            {
                ipList << machine.GetGuestPropertyValue(QString("/VirtualBox/GuestInfo/Net/%1/V4/IP").arg(i));
                macList << machine.GetGuestPropertyValue(QString("/VirtualBox/GuestInfo/Net/%1/MAC").arg(i));
            }
        }

        /* Enumerate up to m_cMaxNetworkAdapters adapters: */
        bool fAdaptersPresent = false;
        bool fCablesDisconnected = true;
        for (ulong uSlot = 0; uSlot < m_cMaxNetworkAdapters; ++uSlot)
        {
            const CNetworkAdapter &adapter = machine.GetNetworkAdapter(uSlot);
            if (machine.isOk() && !adapter.isNull() && adapter.GetEnabled())
            {
                fAdaptersPresent = true;
                QString strGuestIp;
                if (fPropsValid)
                {
                    const QString strGuestMac = adapter.GetMACAddress();
                    int iIp = macList.indexOf(strGuestMac);
                    if (iIp >= 0)
                        strGuestIp = ipList[iIp];
                }
                /* Check if the adapter's cable is connected: */
                const bool fCableConnected = adapter.GetCableConnected();
                if (fCablesDisconnected && fCableConnected)
                    fCablesDisconnected = false;
                /* Append adapter data: */
                strFullData += QApplication::translate("UIIndicatorsPool",
                    "<br><nobr><b>Adapter %1 (%2)</b>: %3 cable %4</nobr>", "Network adapters tooltip")
                    .arg(uSlot + 1)
                    .arg(gpConverter->toString(adapter.GetAttachmentType()))
                    .arg(strGuestIp.isEmpty() ? "" : "IP " + strGuestIp + ", ")
                    .arg(fCableConnected ?
                         QApplication::translate("UIIndicatorsPool", "connected", "Network adapters tooltip") :
                         QApplication::translate("UIIndicatorsPool", "disconnected", "Network adapters tooltip"));
            }
        }
        /* Handle 'no-adapters' case: */
        if (strFullData.isNull())
            strFullData = QApplication::translate("UIIndicatorsPool",
                              "<br><nobr><b>All network adapters are disabled</b></nobr>", "Network adapters tooltip");

        /* Hide indicator if there are no enabled adapters: */
        if (!fAdaptersPresent)
            hide();

        /* Update tool-tip: */
        setToolTip(strToolTip.arg(strFullData));
        /* Update indicator state: */
        setState(fAdaptersPresent && !fCablesDisconnected ? KDeviceActivity_Idle : KDeviceActivity_Null);
    }

    /** Holds the auto-update timer instance. */
    QTimer *m_pTimerAutoUpdate;
    /** Holds the maximum amount of the network adapters. */
    ulong m_cMaxNetworkAdapters;
};

/** UISessionStateStatusBarIndicator extension for Runtime UI: USB indicator. */
class UIIndicatorUSB : public UISessionStateStatusBarIndicator
{
    Q_OBJECT;

public:

    /** Constructor, passes @a pSession to the UISessionStateStatusBarIndicator constructor. */
    UIIndicatorUSB(UISession *pSession)
        : UISessionStateStatusBarIndicator(pSession)
    {
        /* Assign state-icons: */
        setStateIcon(KDeviceActivity_Idle,    UIIconPool::iconSet(":/usb_16px.png"));
        setStateIcon(KDeviceActivity_Reading, UIIconPool::iconSet(":/usb_read_16px.png"));
        setStateIcon(KDeviceActivity_Writing, UIIconPool::iconSet(":/usb_write_16px.png"));
        setStateIcon(KDeviceActivity_Null,    UIIconPool::iconSet(":/usb_disabled_16px.png"));
        /* Translate finally: */
        retranslateUi();
    }

private:

    /** Retranslation routine. */
    void retranslateUi()
    {
        updateAppearance();
    }

    /** Update routine. */
    void updateAppearance()
    {
        /* Get machine: */
        const CMachine machine = m_pSession->machine();

        /* Prepare tool-tip: */
        QString strToolTip = QApplication::translate("UIIndicatorsPool",
                                                     "<p style='white-space:pre'><nobr>Indicates the activity of "
                                                     "the attached USB devices:</nobr>%1</p>", "USB device tooltip");
        QString strFullData;

        /* Check whether there is at least one USB controller with an available proxy. */
        bool fUSBEnabled =    !machine.GetUSBDeviceFilters().isNull()
                           && !machine.GetUSBControllers().isEmpty()
                           && machine.GetUSBProxyAvailable();
        if (fUSBEnabled)
        {
            /* Enumerate all the USB devices: */
            const CConsole console = m_pSession->console();
            foreach (const CUSBDevice &usbDevice, console.GetUSBDevices())
                strFullData += QString("<br><b><nobr>%1</nobr></b>").arg(vboxGlobal().details(usbDevice));
            /* Handle 'no-usb-devices' case: */
            if (strFullData.isNull())
                strFullData = QApplication::translate("UIIndicatorsPool", "<br><nobr><b>No USB devices attached</b></nobr>", "USB device tooltip");
        }

        /* Hide indicator if there are USB controllers: */
        if (!fUSBEnabled)
            hide();

        /* Update tool-tip: */
        setToolTip(strToolTip.arg(strFullData));
        /* Update indicator state: */
        setState(fUSBEnabled ? KDeviceActivity_Idle : KDeviceActivity_Null);
    }
};

/** UISessionStateStatusBarIndicator extension for Runtime UI: Shared-folders indicator. */
class UIIndicatorSharedFolders : public UISessionStateStatusBarIndicator
{
    Q_OBJECT;

public:

    /** Constructor, passes @a pSession to the UISessionStateStatusBarIndicator constructor. */
    UIIndicatorSharedFolders(UISession *pSession)
        : UISessionStateStatusBarIndicator(pSession)
    {
        /* Assign state-icons: */
        setStateIcon(KDeviceActivity_Idle,    UIIconPool::iconSet(":/sf_16px.png"));
        setStateIcon(KDeviceActivity_Reading, UIIconPool::iconSet(":/sf_read_16px.png"));
        setStateIcon(KDeviceActivity_Writing, UIIconPool::iconSet(":/sf_write_16px.png"));
        setStateIcon(KDeviceActivity_Null,    UIIconPool::iconSet(":/sf_disabled_16px.png"));
        /* Translate finally: */
        retranslateUi();
    }

private:

    /** Retranslation routine. */
    void retranslateUi()
    {
        updateAppearance();
    }

    /** Update routine. */
    void updateAppearance()
    {
        /* Get objects: */
        const CMachine machine = m_pSession->machine();
        const CConsole console = m_pSession->console();
        const CGuest guest = m_pSession->guest();

        /* Prepare tool-tip: */
        QString strToolTip = QApplication::translate("UIIndicatorsPool",
                                                     "<p style='white-space:pre'><nobr>Indicates the activity of "
                                                     "the machine's shared folders:</nobr>%1</p>", "Shared folders tooltip");
        QString strFullData;

        /* Enumerate all the folders: */
        QMap<QString, QString> sfs;
        foreach (const CSharedFolder &sf, machine.GetSharedFolders())
            sfs.insert(sf.GetName(), sf.GetHostPath());
        foreach (const CSharedFolder &sf, console.GetSharedFolders())
            sfs.insert(sf.GetName(), sf.GetHostPath());

        /* Append attachment data: */
        for (QMap<QString, QString>::const_iterator it = sfs.constBegin(); it != sfs.constEnd(); ++it)
        {
            /* Select slashes depending on the OS type: */
            if (VBoxGlobal::isDOSType(guest.GetOSTypeId()))
                strFullData += QString("<br><nobr><b>\\\\vboxsvr\\%1&nbsp;</b></nobr><nobr>%2</nobr>")
                                       .arg(it.key(), it.value());
            else
                strFullData += QString("<br><nobr><b>%1&nbsp;</b></nobr><nobr>%2</nobr>")
                                       .arg(it.key(), it.value());
        }
        /* Handle 'no-folders' case: */
        if (sfs.isEmpty())
            strFullData = QApplication::translate("UIIndicatorsPool", "<br><nobr><b>No shared folders</b></nobr>", "Shared folders tooltip");

        /* Update tool-tip: */
        setToolTip(strToolTip.arg(strFullData));
        /* Update indicator state: */
        setState(!sfs.isEmpty() ? KDeviceActivity_Idle : KDeviceActivity_Null);
    }
};

/** UISessionStateStatusBarIndicator extension for Runtime UI: Display indicator. */
class UIIndicatorDisplay : public UISessionStateStatusBarIndicator
{
    Q_OBJECT;

public:

    /** Constructor, passes @a pSession to the UISessionStateStatusBarIndicator constructor. */
    UIIndicatorDisplay(UISession *pSession)
        : UISessionStateStatusBarIndicator(pSession)
    {
        /* Assign state-icons: */
        setStateIcon(KDeviceActivity_Null,    UIIconPool::iconSet(":/display_software_16px.png"));
        setStateIcon(KDeviceActivity_Idle,    UIIconPool::iconSet(":/display_hardware_16px.png"));
        setStateIcon(KDeviceActivity_Writing, UIIconPool::iconSet(":/display_hardware_write_16px.png"));
        /* Translate finally: */
        retranslateUi();
    }

private:

    /** Retranslation routine. */
    void retranslateUi()
    {
        updateAppearance();
    }

    /** Update routine. */
    void updateAppearance()
    {
        /* Get machine: */
        const CMachine machine = m_pSession->machine();

        /* Prepare tool-tip: */
        QString strToolTip = QApplication::translate("UIIndicatorsPool",
                                                     "<p style='white-space:pre'>"
                                                     "<nobr>Indicates the activity of the display:</nobr>%1</p>");
        QString strFullData;

        /* Video Memory: */
        const ULONG uVRAMSize = machine.GetVRAMSize();
        const QString strVRAMSize = VBoxGlobal::tr("<nobr>%1 MB</nobr>", "details report").arg(uVRAMSize);
        strFullData += QString("<br><nobr><b>%1:</b>&nbsp;%2</nobr>")
                               .arg(VBoxGlobal::tr("Video Memory", "details report"), strVRAMSize);

        /* Monitor Count: */
        const ULONG uMonitorCount = machine.GetMonitorCount();
        if (uMonitorCount > 1)
        {
            const QString strMonitorCount = QString::number(uMonitorCount);
            strFullData += QString("<br><nobr><b>%1:</b>&nbsp;%2</nobr>")
                                   .arg(VBoxGlobal::tr("Screens", "details report"), strMonitorCount);
        }

        /* 3D acceleration: */
        const bool fAcceleration3D = machine.GetAccelerate3DEnabled() && vboxGlobal().is3DAvailable();
        if (fAcceleration3D)
        {
            const QString strAcceleration3D = fAcceleration3D
                ? VBoxGlobal::tr("Enabled", "details report (3D Acceleration)")
                : VBoxGlobal::tr("Disabled", "details report (3D Acceleration)");
            strFullData += QString("<br><nobr><b>%1:</b>&nbsp;%2</nobr>")
                                   .arg(VBoxGlobal::tr("3D Acceleration", "details report"), strAcceleration3D);
        }

        /* Update tool-tip: */
        setToolTip(strToolTip.arg(strFullData));
        /* Set initial indicator state: */
        setState(fAcceleration3D ? KDeviceActivity_Idle : KDeviceActivity_Null);
    }
};

/** UISessionStateStatusBarIndicator extension for Runtime UI: Video-capture indicator. */
class UIIndicatorVideoCapture : public UISessionStateStatusBarIndicator
{
    Q_OBJECT;
    Q_PROPERTY(double rotationAngleStart READ rotationAngleStart);
    Q_PROPERTY(double rotationAngleFinal READ rotationAngleFinal);
    Q_PROPERTY(double rotationAngle READ rotationAngle WRITE setRotationAngle);

    /** Video-capture states. */
    enum UIIndicatorStateVideoCapture
    {
        UIIndicatorStateVideoCapture_Disabled = 0,
        UIIndicatorStateVideoCapture_Enabled  = 1
    };

public:

    /** Constructor, passes @a pSession to the UISessionStateStatusBarIndicator constructor. */
    UIIndicatorVideoCapture(UISession *pSession)
        : UISessionStateStatusBarIndicator(pSession)
        , m_pAnimation(0)
        , m_dRotationAngle(0)
    {
        /* Assign state-icons: */
        setStateIcon(UIIndicatorStateVideoCapture_Disabled, UIIconPool::iconSet(":/video_capture_16px.png"));
        setStateIcon(UIIndicatorStateVideoCapture_Enabled,  UIIconPool::iconSet(":/movie_reel_16px.png"));
        /* Create *enabled* state animation: */
        m_pAnimation = UIAnimationLoop::installAnimationLoop(this, "rotationAngle",
                                                                   "rotationAngleStart", "rotationAngleFinal",
                                                                   1000);
        /* Translate finally: */
        retranslateUi();
    }

private slots:

    /** Handles state change. */
    void setState(int iState)
    {
        /* Update animation state: */
        switch (iState)
        {
            case UIIndicatorStateVideoCapture_Disabled:
                m_pAnimation->stop();
                m_dRotationAngle = 0;
                break;
            case UIIndicatorStateVideoCapture_Enabled:
                m_pAnimation->start();
                break;
            default:
                break;
        }
        /* Call to base-class: */
        QIStateStatusBarIndicator::setState(iState);
    }

private:

    /** Retranslation routine. */
    void retranslateUi()
    {
        updateAppearance();
    }

    /** Paint-event handler. */
    void paintEvent(QPaintEvent*)
    {
        /* Create new painter: */
        QPainter painter(this);
        /* Configure painter for *enabled* state: */
        if (state() == UIIndicatorStateVideoCapture_Enabled)
        {
            /* Configure painter for smooth animation: */
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            /* Shift rotation origin according pixmap center: */
            painter.translate(height() / 2, height() / 2);
            /* Rotate painter: */
            painter.rotate(rotationAngle());
            /* Unshift rotation origin according pixmap center: */
            painter.translate(- height() / 2, - height() / 2);
        }
        /* Draw contents: */
        drawContents(&painter);
    }

    /** Update routine. */
    void updateAppearance()
    {
        /* Get machine: */
        const CMachine machine = m_pSession->machine();

        /* Prepare tool-tip: */
        QString strToolTip = QApplication::translate("UIIndicatorsPool", "<nobr>Indicates video capturing activity:</nobr><br>%1");
        switch (state())
        {
            case UIIndicatorStateVideoCapture_Disabled:
            {
                strToolTip = strToolTip.arg(QApplication::translate("UIIndicatorsPool", "<nobr><b>Video capture disabled</b></nobr>"));
                break;
            }
            case UIIndicatorStateVideoCapture_Enabled:
            {
                strToolTip = strToolTip.arg(QApplication::translate("UIIndicatorsPool", "<nobr><b>Video capture file:</b> %1</nobr>"));
                strToolTip = strToolTip.arg(machine.GetVideoCaptureFile());
                break;
            }
            default:
                break;
        }

        /* Update tool-tip: */
        setToolTip(strToolTip);
        /* Update indicator state: */
        setState(machine.GetVideoCaptureEnabled());
    }

    /** Returns rotation start angle. */
    double rotationAngleStart() const { return 0; }
    /** Returns rotation finish angle. */
    double rotationAngleFinal() const { return 360; }
    /** Returns current rotation angle. */
    double rotationAngle() const { return m_dRotationAngle; }
    /** Defines current rotation angle. */
    void setRotationAngle(double dRotationAngle) { m_dRotationAngle = dRotationAngle; update(); }

    /** Holds the rotation animation instance. */
    UIAnimationLoop *m_pAnimation;
    /** Holds current rotation angle. */
    double m_dRotationAngle;
};

/** UISessionStateStatusBarIndicator extension for Runtime UI: Features indicator. */
class UIIndicatorFeatures : public UISessionStateStatusBarIndicator
{
    Q_OBJECT;

public:

    /** Constructor, passes @a pSession to the UISessionStateStatusBarIndicator constructor. */
    UIIndicatorFeatures(UISession *pSession)
        : UISessionStateStatusBarIndicator(pSession)
    {
        /* Assign state-icons: */
        setStateIcon(0, UIIconPool::iconSet(":/vtx_amdv_disabled_16px.png"));
        setStateIcon(1, UIIconPool::iconSet(":/vtx_amdv_16px.png"));
        /* Translate finally: */
        retranslateUi();
    }

private:

    /** Retranslation routine. */
    void retranslateUi()
    {
        updateAppearance();
    }

    /** Update routine. */
    void updateAppearance()
    {
        /* Get objects: */
        const CMachine machine = m_pSession->machine();
        const CMachineDebugger debugger = m_pSession->debugger();

        /* VT-x/AMD-V feature: */
        const QString strVirtualization = m_pSession->isHWVirtExEnabled() ?
                                          VBoxGlobal::tr("Active", "details report (VT-x/AMD-V)") :
                                          VBoxGlobal::tr("Inactive", "details report (VT-x/AMD-V)");

        /* Nested Paging feature: */
        const QString strNestedPaging = m_pSession->isHWVirtExNestedPagingEnabled() ?
                                        VBoxGlobal::tr("Active", "details report (Nested Paging)") :
                                        VBoxGlobal::tr("Inactive", "details report (Nested Paging)");

        /* Unrestricted Execution feature: */
        const QString strUnrestrictExec = m_pSession->isHWVirtExUXEnabled() ?
                                          VBoxGlobal::tr("Active", "details report (Unrestricted Execution)") :
                                          VBoxGlobal::tr("Inactive", "details report (Unrestricted Execution)");

        /* CPU Execution Cap feature: */
        QString strCPUExecCap = QString::number(machine.GetCPUExecutionCap());

        /* Paravirtualization feature: */
        const QString strParavirt = gpConverter->toString(m_pSession->paraVirtProvider());

        /* Prepare tool-tip: */
        QString tip(QApplication::translate("UIIndicatorsPool",
                                            "Additional feature status:"
                                            "<br><nobr><b>%1:</b>&nbsp;%2</nobr>"
                                            "<br><nobr><b>%3:</b>&nbsp;%4</nobr>"
                                            "<br><nobr><b>%5:</b>&nbsp;%6</nobr>"
                                            "<br><nobr><b>%7:</b>&nbsp;%8%</nobr>",
                                            "Virtualization Stuff LED")
                    .arg(VBoxGlobal::tr("VT-x/AMD-V", "details report"), strVirtualization)
                    .arg(VBoxGlobal::tr("Nested Paging"), strNestedPaging)
                    .arg(VBoxGlobal::tr("Unrestricted Execution"), strUnrestrictExec)
                    .arg(VBoxGlobal::tr("Execution Cap", "details report"), strCPUExecCap));

        // TODO: We had to use that large NLS above for now.
        //       Later it should be reworked to be well-maintainable..
        /* Separately add information about paravirtualization interface feature: */
        tip += QApplication::translate("UIIndicatorsPool", "<br><nobr><b>%1:</b>&nbsp;%2</nobr>", "Virtualization Stuff LED")
                                      .arg(VBoxGlobal::tr("Paravirtualization Interface", "details report"), strParavirt);

        /* CPU count: */
        int cpuCount = machine.GetCPUCount();
        if (cpuCount > 1)
            tip += QApplication::translate("UIIndicatorsPool", "<br><nobr><b>%1:</b>&nbsp;%2</nobr>", "Virtualization Stuff LED")
                      .arg(VBoxGlobal::tr("Processor(s)", "details report")).arg(cpuCount);

        /* Update tool-tip: */
        setToolTip(tip);
        /* Update indicator state: */
        setState(m_pSession->isHWVirtExEnabled());
    }
};

/** UISessionStateStatusBarIndicator extension for Runtime UI: Mouse indicator. */
class UIIndicatorMouse : public UISessionStateStatusBarIndicator
{
    Q_OBJECT;

public:

    /** Constructor, using @a pSession for state-update routine. */
    UIIndicatorMouse(UISession *pSession)
        : UISessionStateStatusBarIndicator(pSession)
    {
        /* Assign state-icons: */
        setStateIcon(0, UIIconPool::iconSet(":/mouse_disabled_16px.png"));
        setStateIcon(1, UIIconPool::iconSet(":/mouse_16px.png"));
        setStateIcon(2, UIIconPool::iconSet(":/mouse_seamless_16px.png"));
        setStateIcon(3, UIIconPool::iconSet(":/mouse_can_seamless_16px.png"));
        setStateIcon(4, UIIconPool::iconSet(":/mouse_can_seamless_uncaptured_16px.png"));
        /* Configure connection: */
        connect(pSession, SIGNAL(sigMouseStateChange(int)), this, SLOT(setState(int)));
        setState(pSession->mouseState());
        /* Translate finally: */
        retranslateUi();
    }

private slots:

    /** Handles state change. */
    void setState(int iState)
    {
        if ((iState & UIMouseStateType_MouseAbsoluteDisabled) &&
            (iState & UIMouseStateType_MouseAbsolute) &&
            !(iState & UIMouseStateType_MouseCaptured))
        {
            QIStateStatusBarIndicator::setState(4);
        }
        else
        {
            QIStateStatusBarIndicator::setState(iState & (UIMouseStateType_MouseAbsolute | UIMouseStateType_MouseCaptured));
        }
    }

private:

    /** Retranslation routine. */
    void retranslateUi()
    {
        setToolTip(QApplication::translate("UIIndicatorsPool",
                   "Indicates whether the host mouse pointer is captured by the guest OS:<br>"
                   "<nobr><img src=:/mouse_disabled_16px.png/>&nbsp;&nbsp;pointer is not captured</nobr><br>"
                   "<nobr><img src=:/mouse_16px.png/>&nbsp;&nbsp;pointer is captured</nobr><br>"
                   "<nobr><img src=:/mouse_seamless_16px.png/>&nbsp;&nbsp;mouse integration (MI) is On</nobr><br>"
                   "<nobr><img src=:/mouse_can_seamless_16px.png/>&nbsp;&nbsp;MI is Off, pointer is captured</nobr><br>"
                   "<nobr><img src=:/mouse_can_seamless_uncaptured_16px.png/>&nbsp;&nbsp;MI is Off, pointer is not captured</nobr><br>"
                   "Note that the mouse integration feature requires Guest Additions to be installed in the guest OS."));
    }

    /** Update routine. */
    void updateAppearance() {}
};

/** UISessionStateStatusBarIndicator extension for Runtime UI: Keyboard indicator. */
class UIIndicatorKeyboard : public UISessionStateStatusBarIndicator
{
    Q_OBJECT;

public:

    /** Constructor, using @a pSession for state-update routine. */
    UIIndicatorKeyboard(UISession *pSession)
        : UISessionStateStatusBarIndicator(pSession)
    {
        /* Assign state-icons: */
        setStateIcon(0, UIIconPool::iconSet(":/hostkey_16px.png"));
        setStateIcon(1, UIIconPool::iconSet(":/hostkey_captured_16px.png"));
        setStateIcon(2, UIIconPool::iconSet(":/hostkey_pressed_16px.png"));
        setStateIcon(3, UIIconPool::iconSet(":/hostkey_captured_pressed_16px.png"));
        /* Configure connection: */
        connect(pSession, SIGNAL(sigKeyboardStateChange(int)), this, SLOT(setState(int)));
        setState(pSession->keyboardState());
        /* Translate finally: */
        retranslateUi();
    }

private:

    /** Retranslation routine. */
    void retranslateUi()
    {
        setToolTip(QApplication::translate("UIIndicatorsPool",
                   "Indicates whether the host keyboard is captured by the guest OS:<br>"
                   "<nobr><img src=:/hostkey_16px.png/>&nbsp;&nbsp;keyboard is not captured</nobr><br>"
                   "<nobr><img src=:/hostkey_captured_16px.png/>&nbsp;&nbsp;keyboard is captured</nobr>"));
    }

    /** Update routine. */
    void updateAppearance() {}
};

/** QITextStatusBarIndicator extension for Runtime UI: Keyboard-extension indicator. */
class UIIndicatorKeyboardExtension : public QIWithRetranslateUI<QITextStatusBarIndicator>
{
    Q_OBJECT;

public:

    /** Constructor. */
    UIIndicatorKeyboardExtension()
    {
        /* Make sure host-combination label will be updated: */
        connect(&vboxGlobal().settings(), SIGNAL(propertyChanged(const char *, const char *)),
                this, SLOT(sltUpdateAppearance()));
        /* Translate finally: */
        retranslateUi();
    }

public slots:

    /** Update routine. */
    void sltUpdateAppearance()
    {
        setText(UIHostCombo::toReadableString(vboxGlobal().settings().hostCombo()));
    }

private:

    /** Retranslation routine. */
    void retranslateUi()
    {
        sltUpdateAppearance();
        setToolTip(QApplication::translate("UIMachineWindowNormal",
                                           "Shows the currently assigned Host key.<br>"
                                           "This key, when pressed alone, toggles the keyboard and mouse "
                                           "capture state. It can also be used in combination with other keys "
                                           "to quickly perform actions from the main menu."));
    }
};


UIIndicatorsPool::UIIndicatorsPool(UISession *pSession, QWidget *pParent /* = 0 */)
    : QWidget(pParent)
    , m_pSession(pSession)
    , m_fEnabled(false)
    , m_pTimerAutoUpdate(0)
{
    /* Prepare: */
    prepare();
}

UIIndicatorsPool::~UIIndicatorsPool()
{
    /* Cleanup: */
    cleanup();
}

void UIIndicatorsPool::updateAppearance(IndicatorType indicatorType)
{
    /* Skip missed indicators: */
    if (!m_pool.contains(indicatorType))
        return;

    /* Get indicator: */
    QIStatusBarIndicator *pIndicator = m_pool.value(indicatorType);

    /* Assert indicators with NO appearance: */
    UISessionStateStatusBarIndicator *pSessionStateIndicator =
        qobject_cast<UISessionStateStatusBarIndicator*>(pIndicator);
    AssertPtrReturnVoid(pSessionStateIndicator);

    /* Update indicator appearance: */
    pSessionStateIndicator->updateAppearance();
}

void UIIndicatorsPool::setAutoUpdateIndicatorStates(bool fEnabled)
{
    /* Make sure auto-update timer exists: */
    AssertPtrReturnVoid(m_pTimerAutoUpdate);

    /* Start/stop timer: */
    if (fEnabled)
        m_pTimerAutoUpdate->start(100);
    else
        m_pTimerAutoUpdate->stop();
}

void UIIndicatorsPool::sltHandleConfigurationChange(const QString &strMachineID)
{
    /* Skip unrelated machine IDs: */
    if (vboxGlobal().managedVMUuid() != strMachineID)
        return;

    /* Update pool: */
    updatePool();
}

void UIIndicatorsPool::sltAutoUpdateIndicatorStates()
{
    /* We should update states for following indicators: */
    QVector<KDeviceType> deviceTypes;
    if (m_pool.contains(IndicatorType_HardDisks))
        deviceTypes.append(KDeviceType_HardDisk);
    if (m_pool.contains(IndicatorType_OpticalDisks))
        deviceTypes.append(KDeviceType_DVD);
    if (m_pool.contains(IndicatorType_FloppyDisks))
        deviceTypes.append(KDeviceType_Floppy);
    if (m_pool.contains(IndicatorType_USB))
        deviceTypes.append(KDeviceType_USB);
    if (m_pool.contains(IndicatorType_Network))
        deviceTypes.append(KDeviceType_Network);
    if (m_pool.contains(IndicatorType_SharedFolders))
        deviceTypes.append(KDeviceType_SharedFolder);
    if (m_pool.contains(IndicatorType_Display))
        deviceTypes.append(KDeviceType_Graphics3D);

    /* Acquire current states from the console: */
    CConsole console = m_pSession->console();
    if (console.isNull() || !console.isOk())
        return;
    const QVector<KDeviceActivity> states = console.GetDeviceActivity(deviceTypes);
    AssertReturnVoid(console.isOk());

    /* Update indicators with the acquired states: */
    for (int iIndicator = 0; iIndicator < states.size(); ++iIndicator)
    {
        QIStatusBarIndicator *pIndicator = 0;
        switch (deviceTypes[iIndicator])
        {
            case KDeviceType_HardDisk:     pIndicator = m_pool.value(IndicatorType_HardDisks); break;
            case KDeviceType_DVD:          pIndicator = m_pool.value(IndicatorType_OpticalDisks); break;
            case KDeviceType_Floppy:       pIndicator = m_pool.value(IndicatorType_FloppyDisks); break;
            case KDeviceType_USB:          pIndicator = m_pool.value(IndicatorType_USB); break;
            case KDeviceType_Network:      pIndicator = m_pool.value(IndicatorType_Network); break;
            case KDeviceType_SharedFolder: pIndicator = m_pool.value(IndicatorType_SharedFolders); break;
            case KDeviceType_Graphics3D:   pIndicator = m_pool.value(IndicatorType_Display); break;
            default: AssertFailed(); break;
        }
        if (pIndicator)
            updateIndicatorStateForDevice(pIndicator, states[iIndicator]);
    }
}

void UIIndicatorsPool::sltContextMenuRequest(QIStatusBarIndicator *pIndicator, QContextMenuEvent *pEvent)
{
    /* If that is one of pool indicators: */
    foreach (IndicatorType indicatorType, m_pool.keys())
        if (m_pool[indicatorType] == pIndicator)
        {
            /* Notify listener: */
            emit sigContextMenuRequest(indicatorType, pEvent->globalPos());
            return;
        }
}

void UIIndicatorsPool::prepare()
{
    /* Prepare connections: */
    prepareConnections();
    /* Prepare contents: */
    prepareContents();
    /* Prepare auto-update timer: */
    prepareUpdateTimer();
}

void UIIndicatorsPool::prepareConnections()
{
    /* Listen for the status-bar configuration changes: */
    connect(gEDataManager, SIGNAL(sigStatusBarConfigurationChange(const QString&)),
            this, SLOT(sltHandleConfigurationChange(const QString&)));
}

void UIIndicatorsPool::prepareContents()
{
    /* Create main-layout: */
    m_pMainLayout = new QHBoxLayout(this);
    AssertPtrReturnVoid(m_pMainLayout);
    {
        /* Configure main-layout: */
        m_pMainLayout->setContentsMargins(0, 0, 0, 0);
        m_pMainLayout->setSpacing(5);
        /* Update pool: */
        updatePool();
    }
}

void UIIndicatorsPool::prepareUpdateTimer()
{
    /* Create auto-update timer: */
    m_pTimerAutoUpdate = new QTimer(this);
    AssertPtrReturnVoid(m_pTimerAutoUpdate);
    {
        /* Configure auto-update timer: */
        connect(m_pTimerAutoUpdate, SIGNAL(timeout()),
                this, SLOT(sltAutoUpdateIndicatorStates()));
        setAutoUpdateIndicatorStates(true);
    }
}

void UIIndicatorsPool::updatePool()
{
    /* Acquire status-bar availability: */
    m_fEnabled = gEDataManager->statusBarEnabled(vboxGlobal().managedVMUuid());
    /* If status-bar is not enabled: */
    if (!m_fEnabled)
    {
        /* Remove all indicators: */
        while (!m_pool.isEmpty())
        {
            const IndicatorType firstType = m_pool.keys().first();
            delete m_pool.value(firstType);
            m_pool.remove(firstType);
        }
        /* And return: */
        return;
    }

    /* Acquire status-bar restrictions: */
    m_restrictions = gEDataManager->restrictedStatusBarIndicators(vboxGlobal().managedVMUuid());
    /* Remove restricted indicators: */
    foreach (const IndicatorType &indicatorType, m_restrictions)
    {
        if (m_pool.contains(indicatorType))
        {
            delete m_pool.value(indicatorType);
            m_pool.remove(indicatorType);
        }
    }

    /* Acquire status-bar order: */
    m_order = gEDataManager->statusBarIndicatorOrder(vboxGlobal().managedVMUuid());
    /* Make sure the order is complete taking restrictions into account: */
    for (int iType = IndicatorType_Invalid; iType < IndicatorType_Max; ++iType)
    {
        /* Get iterated type: */
        IndicatorType type = (IndicatorType)iType;
        /* Skip invalid type: */
        if (type == IndicatorType_Invalid)
            continue;
        /* Take restriction/presence into account: */
        bool fRestricted = m_restrictions.contains(type);
        bool fPresent = m_order.contains(type);
        if (fRestricted && fPresent)
            m_order.removeAll(type);
        else if (!fRestricted && !fPresent)
            m_order << type;
    }

    /* Add/Update allowed indicators: */
    foreach (const IndicatorType &indicatorType, m_order)
    {
        /* Indicator exists: */
        if (m_pool.contains(indicatorType))
        {
            /* Get indicator: */
            QIStatusBarIndicator *pIndicator = m_pool.value(indicatorType);
            /* Make sure it have valid position: */
            const int iWantedIndex = indicatorPosition(indicatorType);
            const int iActualIndex = m_pMainLayout->indexOf(pIndicator);
            if (iActualIndex != iWantedIndex)
            {
                /* Re-inject indicator into main-layout at proper position: */
                m_pMainLayout->removeWidget(pIndicator);
                m_pMainLayout->insertWidget(iWantedIndex, pIndicator);
            }
        }
        /* Indicator missed: */
        else
        {
            /* Create indicator: */
            switch (indicatorType)
            {
                case IndicatorType_HardDisks:         m_pool[indicatorType] = new UIIndicatorHardDrive(m_pSession);     break;
                case IndicatorType_OpticalDisks:      m_pool[indicatorType] = new UIIndicatorOpticalDisks(m_pSession);  break;
                case IndicatorType_FloppyDisks:       m_pool[indicatorType] = new UIIndicatorFloppyDisks(m_pSession);   break;
                case IndicatorType_Network:           m_pool[indicatorType] = new UIIndicatorNetwork(m_pSession);       break;
                case IndicatorType_USB:               m_pool[indicatorType] = new UIIndicatorUSB(m_pSession);           break;
                case IndicatorType_SharedFolders:     m_pool[indicatorType] = new UIIndicatorSharedFolders(m_pSession); break;
                case IndicatorType_Display:           m_pool[indicatorType] = new UIIndicatorDisplay(m_pSession);       break;
                case IndicatorType_VideoCapture:      m_pool[indicatorType] = new UIIndicatorVideoCapture(m_pSession);  break;
                case IndicatorType_Features:          m_pool[indicatorType] = new UIIndicatorFeatures(m_pSession);      break;
                case IndicatorType_Mouse:             m_pool[indicatorType] = new UIIndicatorMouse(m_pSession);         break;
                case IndicatorType_Keyboard:          m_pool[indicatorType] = new UIIndicatorKeyboard(m_pSession);      break;
                case IndicatorType_KeyboardExtension: m_pool[indicatorType] = new UIIndicatorKeyboardExtension;         break;
                default: break;
            }
            /* Configure indicator: */
            connect(m_pool.value(indicatorType), SIGNAL(sigContextMenuRequest(QIStatusBarIndicator*, QContextMenuEvent*)),
                    this, SLOT(sltContextMenuRequest(QIStatusBarIndicator*, QContextMenuEvent*)));
            /* Insert indicator into main-layout at proper position: */
            m_pMainLayout->insertWidget(indicatorPosition(indicatorType), m_pool.value(indicatorType));
        }
    }
}

void UIIndicatorsPool::cleanupUpdateTimer()
{
    /* Destroy auto-update timer: */
    AssertPtrReturnVoid(m_pTimerAutoUpdate);
    {
        m_pTimerAutoUpdate->stop();
        delete m_pTimerAutoUpdate;
        m_pTimerAutoUpdate = 0;
    }
}

void UIIndicatorsPool::cleanupContents()
{
    /* Cleanup indicators: */
    while (!m_pool.isEmpty())
    {
        const IndicatorType firstType = m_pool.keys().first();
        delete m_pool.value(firstType);
        m_pool.remove(firstType);
    }
}

void UIIndicatorsPool::cleanup()
{
    /* Cleanup auto-update timer: */
    cleanupUpdateTimer();
    /* Cleanup indicators: */
    cleanupContents();
}

void UIIndicatorsPool::contextMenuEvent(QContextMenuEvent *pEvent)
{
    /* Do not pass-through context menu events,
     * otherwise they will raise the underlying status-bar context-menu. */
    pEvent->accept();
}

int UIIndicatorsPool::indicatorPosition(IndicatorType indicatorType) const
{
    int iPosition = 0;
    foreach (const IndicatorType &iteratedIndicatorType, m_order)
        if (iteratedIndicatorType == indicatorType)
            return iPosition;
        else
            ++iPosition;
    return iPosition;
}

void UIIndicatorsPool::updateIndicatorStateForDevice(QIStatusBarIndicator *pIndicator, KDeviceActivity state)
{
    /* Assert indicators with NO state: */
    QIStateStatusBarIndicator *pStateIndicator = qobject_cast<QIStateStatusBarIndicator*>(pIndicator);
    AssertPtrReturnVoid(pStateIndicator);

    /* Skip indicators with NULL state: */
    if (pStateIndicator->state() == KDeviceActivity_Null)
        return;

    /* Paused VM have all indicator states set to IDLE: */
    if (m_pSession->isPaused())
    {
        /* If current state differs from IDLE => set the IDLE one:  */
        if (pStateIndicator->state() != KDeviceActivity_Idle)
            pStateIndicator->setState(KDeviceActivity_Idle);
    }
    else
    {
        /* If current state differs from actual => set the actual one: */
        const int iState = (int)state;
        if (pStateIndicator->state() != iState)
            pStateIndicator->setState(iState);
    }
}

#include "UIIndicatorsPool.moc"

