# -*- coding: utf-8 -*-
# $Id$
# pylint: disable=C0302

"""
VirtualBox USB gadget control class
"""

__copyright__ = \
"""
Copyright (C) 2014-2015 Oracle Corporation

This file is part of VirtualBox Open Source Edition (OSE), as
available from http://www.virtualbox.org. This file is free software;
you can redistribute it and/or modify it under the terms of the GNU
General Public License (GPL) as published by the Free Software
Foundation, in version 2 as it comes in the "COPYING" file of the
VirtualBox OSE distribution. VirtualBox OSE is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.

The contents of this file may alternatively be used under the terms
of the Common Development and Distribution License Version 1.0
(CDDL) only, as it comes in the "COPYING.CDDL" file of the
VirtualBox OSE distribution, in which case the provisions of the
CDDL are applicable instead of those of the GPL.

You may elect to license modified versions of this file under the
terms and conditions of either the GPL or the CDDL or both.
"""
__version__ = "$Revision$"


# Validation Kit imports.
import testdriver.txsclient as txsclient;
import testdriver.reporter as reporter;

## @name USB gadget type string constants.
## @{
g_ksGadgetTypeInvalid     = 'Invalid';
g_ksGadgetTypeBeaglebone  = 'BeagleBone';
g_ksGadgetTypeODroidXu3   = 'ODroidXu3';
## @}

## @name USB gadget imeprsonation string constants.
## @{
g_ksGadgetImpersonationInvalid = 'Invalid';
g_ksGadgetImpersonationTest    = 'Test';
g_ksGadgetImpersonationMsd     = 'Msd';
g_ksGadgetImpersonationWebcam  = 'Webcam';
g_ksGadgetImpersonationEther   = 'Ether';
## @}

class UsbGadget(object):
    """
    USB Gadget control class using the TesteXecService to talk to the external
    board behaving like a USB device.
    The board needs to run an embedded Linux system with the TXS service running.
    """

    def __init__(self):
        self.oTxsSession = None;
        self.sImpersonation = g_ksGadgetImpersonationInvalid;
        self.sGadgetType    = g_ksGadgetTypeInvalid;

    def _loadModule(self, sModule):
        """
        Loads the given module on the USB gadget.
        Returns True on success.
        Returns False otherwise.
        """
        fRc = False;
        if self.oTxsSession is not None:
            fRc = self.oTxsSession.syncExecEx('/usr/bin/modprobe', ('/usr/bin/modprobe', sModule));
            fRc = fRc and self.connectUsb();
        return fRc;

    def _unloadModule(self, sModule):
        """
        Unloads the given module on the USB gadget.
        Returns True on success.
        Returns False otherwise.
        """
        fRc = False;
        if self.oTxsSession is not None:
            self.disconnectUsb();
            fRc = self.oTxsSession.syncExecEx('/usr/bin/rmmod', ('/usr/bin/rmmod', sModule));

        return fRc;

    def _clearImpersonation(self):
        """
        Removes the current impersonation of the gadget.
        """
        if self.sImpersonation == g_ksGadgetImpersonationInvalid:
            self._unloadModule('g_zero');
            self._unloadModule('g_mass_storage');
            self._unloadModule('g_webcam');
            self._unloadModule('g_ether');
            return True;
        elif self.sImpersonation == g_ksGadgetImpersonationTest:
            return self._unloadModule('g_zero');
        elif self.sImpersonation == g_ksGadgetImpersonationMsd:
            return self._unloadModule('g_mass_storage');
        elif self.sImpersonation == g_ksGadgetImpersonationWebcam:
            return self._unloadModule('g_webcam');
        elif self.sImpersonation == g_ksGadgetImpersonationEther:
            return self._unloadModule('g_ether');
        else:
            reporter.log('Invalid impersonation');

        return False;

    def disconnectUsb(self):
        """
        Disconnects the USB gadget from the host. (USB connection not network
        connection used for control)
        """
        if self.sGadgetType == g_ksGadgetTypeODroidXu3:
            fRc = self.oTxsSession.syncExecEx('/usr/bin/sh', \
                    ('/usr/bin/sh', '-c', 'echo disconnect > /sys/class/udc/12400000.dwc3/soft_connect'));
        elif self.sGadgetType == g_ksGadgetTypeBeaglebone:
            fRc = self.oTxsSession.syncExecEx('/usr/bin/sh', \
                    ('/usr/bin/sh', '-c', 'echo disconnect > /sys/class/udc/musb-hdrc.0.auto/soft_connect'));
        return fRc;

    def connectUsb(self):
        """
        Connect the USB gadget to the host.
        """
        if self.sGadgetType == g_ksGadgetTypeODroidXu3:
            fRc = self.oTxsSession.syncExecEx('/usr/bin/sh', \
                    ('/usr/bin/sh', '-c', 'echo connect > /sys/class/udc/12400000.dwc3/soft_connect'));
        elif self.sGadgetType == g_ksGadgetTypeBeaglebone:
            fRc = self.oTxsSession.syncExecEx('/usr/bin/sh', \
                    ('/usr/bin/sh', '-c', 'echo connect > /sys/class/udc/musb-hdrc.0.auto/soft_connect'));
        return fRc;

    def impersonate(self, sImpersonation):
        """
        Impersonate a given device.
        """

        # Clear any previous impersonation
        self._clearImpersonation();
        self.sImpersonation = sImpersonation;

        if sImpersonation == g_ksGadgetImpersonationInvalid:
            return False;
        elif sImpersonation == g_ksGadgetImpersonationTest:
            return self._loadModule('g_zero');
        elif sImpersonation == g_ksGadgetImpersonationMsd:
            # @todo: Not complete
            return self._loadModule('g_mass_storage');
        elif sImpersonation == g_ksGadgetImpersonationWebcam:
            # @todo: Not complete
            return self._loadModule('g_webcam');
        elif sImpersonation == g_ksGadgetImpersonationEther:
            return self._loadModule('g_ether');
        else:
            reporter.log('Invalid impersonation');

        return False;

    def connectTo(self, cMsTimeout, sGadgetType, sHostname, uPort = None):
        """
        Connects to the specified target device.
        Returns True on Success.
        Returns False otherwise.
        """
        if uPort is None:
            self.oTxsSession = txsclient.openTcpSession(cMsTimeout, sHostname);
        else:
            self.oTxsSession = txsclient.openTcpSession(cMsTimeout, sHostname, uPort = uPort);
        if self.oTxsSession is None:
            return False;

        fDone = self.oTxsSession.waitForTask(30*1000);
        print 'connect: waitForTask -> %s, result %s' % (fDone, self.oTxsSession.getResult());
        if fDone is True and self.oTxsSession.isSuccess():
            fRc = True;
        else:
            fRc = False;

        if fRc is True:
            self.sGadgetType = sGadgetType;

        return fRc;

    def disconnectFrom(self):
        """
        Disconnects from the target device.
        """
        fRc = True;

        if self.oTxsSession is not None:
            self._clearImpersonation();
            fRc = self.oTxsSession.syncDisconnect();

        return fRc;
