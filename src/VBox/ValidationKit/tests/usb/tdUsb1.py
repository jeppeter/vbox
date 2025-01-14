#!/usr/bin/env python
# -*- coding: utf-8 -*-
# $Id$

"""
VirtualBox Validation Kit - USB testcase and benchmark.
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


# Standard Python imports.
import os;
import sys;
import socket;

# Only the main script needs to modify the path.
try:    __file__
except: __file__ = sys.argv[0];
g_ksValidationKitDir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))));
sys.path.append(g_ksValidationKitDir);

# Validation Kit imports.
from testdriver import reporter;
from testdriver import base;
from testdriver import vbox;
from testdriver import vboxcon;

# USB gadget control import
import usbgadget;

class tdUsbBenchmark(vbox.TestDriver):                                      # pylint: disable=R0902
    """
    USB benchmark.
    """

    # The available test devices
    #
    # The first key is the hostname of the host the test is running on.
    # It contains a new dictionary with the attached gadgets based on the
    # USB speed we want to test (Low, Full, High, Super).
    # The parameters consist of the hostname of the gadget in the network
    # and the hardware type.
    kdGadgetParams = {
        # The following is for local testing and not for the test lab.
        'adaris': {
            'Low':   ('beaglebone', usbgadget.g_ksGadgetTypeBeaglebone),
            'Full':  ('beaglebone', usbgadget.g_ksGadgetTypeBeaglebone),
            'High':  ('beaglebone', usbgadget.g_ksGadgetTypeBeaglebone),
            'Super': ('odroidxu3',  usbgadget.g_ksGadgetTypeODroidXu3)
        },
        'archusb': {
            'Low':   ('beaglebone', usbgadget.g_ksGadgetTypeBeaglebone),
            'Full':  ('beaglebone', usbgadget.g_ksGadgetTypeBeaglebone),
            'High':  ('beaglebone', usbgadget.g_ksGadgetTypeBeaglebone),
            'Super': ('odroidxu3',  usbgadget.g_ksGadgetTypeODroidXu3)
        },
    };

    # Mappings of USB controllers to supported USB device speeds.
    kdUsbSpeedMappings = {
        'OHCI': ['Low', 'Full'],
        'EHCI': ['High'],
        'XHCI': ['Low', 'Full', 'High', 'Super']
    };

    def __init__(self):
        vbox.TestDriver.__init__(self);
        self.asRsrcs               = None;
        self.asTestVMsDef          = ['tst-arch'];
        self.asTestVMs             = self.asTestVMsDef;
        self.asSkipVMs             = [];
        self.asVirtModesDef        = ['hwvirt', 'hwvirt-np', 'raw'];
        self.asVirtModes           = self.asVirtModesDef;
        self.acCpusDef             = [1, 2,];
        self.acCpus                = self.acCpusDef;
        self.asUsbCtrlsDef         = ['OHCI', 'EHCI', 'XHCI'];
        self.asUsbCtrls            = self.asUsbCtrlsDef;
        self.asUsbSpeedDef         = ['Low', 'Full', 'High', 'Super'];
        self.asUsbSpeed            = self.asUsbSpeedDef;
        self.asUsbTestsDef         = ['Compliance', 'Reattach'];
        self.asUsbTests            = self.asUsbTestsDef;
        self.cUsbReattachCyclesDef = 100;
        self.cUsbReattachCycles    = self.cUsbReattachCyclesDef;
        self.sHostname             = socket.gethostname().lower();

    #
    # Overridden methods.
    #
    def showUsage(self):
        rc = vbox.TestDriver.showUsage(self);
        reporter.log('');
        reporter.log('tdStorageBenchmark1 Options:');
        reporter.log('  --virt-modes    <m1[:m2[:]]');
        reporter.log('      Default: %s' % (':'.join(self.asVirtModesDef)));
        reporter.log('  --cpu-counts    <c1[:c2[:]]');
        reporter.log('      Default: %s' % (':'.join(str(c) for c in self.acCpusDef)));
        reporter.log('  --test-vms      <vm1[:vm2[:...]]>');
        reporter.log('      Test the specified VMs in the given order. Use this to change');
        reporter.log('      the execution order or limit the choice of VMs');
        reporter.log('      Default: %s  (all)' % (':'.join(self.asTestVMsDef)));
        reporter.log('  --skip-vms      <vm1[:vm2[:...]]>');
        reporter.log('      Skip the specified VMs when testing.');
        reporter.log('  --usb-ctrls     <u1[:u2[:]]');
        reporter.log('      Default: %s' % (':'.join(str(c) for c in self.asUsbCtrlsDef)));
        reporter.log('  --usb-speed     <s1[:s2[:]]');
        reporter.log('      Default: %s' % (':'.join(str(c) for c in self.asUsbSpeedDef)));
        reporter.log('  --usb-tests     <s1[:s2[:]]');
        reporter.log('      Default: %s' % (':'.join(str(c) for c in self.asUsbTestsDef)));
        reporter.log('  --usb-reattach-cycles <cycles>');
        reporter.log('      Default: %s' % (self.cUsbReattachCyclesDef));
        return rc;

    def parseOption(self, asArgs, iArg):                                        # pylint: disable=R0912,R0915
        if asArgs[iArg] == '--virt-modes':
            iArg += 1;
            if iArg >= len(asArgs): raise base.InvalidOption('The "--virt-modes" takes a colon separated list of modes');
            self.asVirtModes = asArgs[iArg].split(':');
            for s in self.asVirtModes:
                if s not in self.asVirtModesDef:
                    raise base.InvalidOption('The "--virt-modes" value "%s" is not valid; valid values are: %s' \
                        % (s, ' '.join(self.asVirtModesDef)));
        elif asArgs[iArg] == '--cpu-counts':
            iArg += 1;
            if iArg >= len(asArgs): raise base.InvalidOption('The "--cpu-counts" takes a colon separated list of cpu counts');
            self.acCpus = [];
            for s in asArgs[iArg].split(':'):
                try: c = int(s);
                except: raise base.InvalidOption('The "--cpu-counts" value "%s" is not an integer' % (s,));
                if c <= 0:  raise base.InvalidOption('The "--cpu-counts" value "%s" is zero or negative' % (s,));
                self.acCpus.append(c);
        elif asArgs[iArg] == '--test-vms':
            iArg += 1;
            if iArg >= len(asArgs): raise base.InvalidOption('The "--test-vms" takes colon separated list');
            self.asTestVMs = asArgs[iArg].split(':');
            for s in self.asTestVMs:
                if s not in self.asTestVMsDef:
                    raise base.InvalidOption('The "--test-vms" value "%s" is not valid; valid values are: %s' \
                        % (s, ' '.join(self.asTestVMsDef)));
        elif asArgs[iArg] == '--skip-vms':
            iArg += 1;
            if iArg >= len(asArgs): raise base.InvalidOption('The "--skip-vms" takes colon separated list');
            self.asSkipVMs = asArgs[iArg].split(':');
            for s in self.asSkipVMs:
                if s not in self.asTestVMsDef:
                    reporter.log('warning: The "--test-vms" value "%s" does not specify any of our test VMs.' % (s));
        elif asArgs[iArg] == '--usb-ctrls':
            iArg += 1;
            if iArg >= len(asArgs): raise base.InvalidOption('The "--usb-ctrls" takes a colon separated list of USB controllers');
            self.asUsbCtrls = asArgs[iArg].split(':');
            for s in self.asUsbCtrls:
                if s not in self.asUsbCtrlsDef:
                    reporter.log('warning: The "--usb-ctrls" value "%s" is not a valid USB controller.' % (s));
        elif asArgs[iArg] == '--usb-speed':
            iArg += 1;
            if iArg >= len(asArgs): raise base.InvalidOption('The "--usb-speed" takes a colon separated list of USB speeds');
            self.asUsbSpeed = asArgs[iArg].split(':');
            for s in self.asUsbSpeed:
                if s not in self.asUsbSpeedDef:
                    reporter.log('warning: The "--usb-speed" value "%s" is not a valid USB speed.' % (s));
        elif asArgs[iArg] == '--usb-tests':
            iArg += 1;
            if iArg >= len(asArgs): raise base.InvalidOption('The "--usb-tests" takes a colon separated list of USB tests');
            self.asUsbTests = asArgs[iArg].split(':');
            for s in self.asUsbTests:
                if s not in self.asUsbTestsDef:
                    reporter.log('warning: The "--usb-tests" value "%s" is not a valid USB test.' % (s));
        elif asArgs[iArg] == '--usb-reattach-cycles':
            iArg += 1;
            if iArg >= len(asArgs): raise base.InvalidOption('The "--usb-reattach-cycles" takes cycle count');
            try:    self.cUsbReattachCycles = int(asArgs[iArg]);
            except: raise base.InvalidOption('The "--usb-reattach-cycles" value "%s" is not an integer' % (asArgs[iArg],));
            if self.cUsbReattachCycles <= 0:
                raise base.InvalidOption('The "--usb-reattach-cycles" value "%s" is zero or negative.' % (self.cUsbReattachCycles,));
        else:
            return vbox.TestDriver.parseOption(self, asArgs, iArg);
        return iArg + 1;

    def completeOptions(self):
        # Remove skipped VMs from the test list.
        for sVM in self.asSkipVMs:
            try:    self.asTestVMs.remove(sVM);
            except: pass;

        return vbox.TestDriver.completeOptions(self);

    def getResourceSet(self):
        # Construct the resource list the first time it's queried.
        if self.asRsrcs is None:
            self.asRsrcs = [];

            if 'tst-arch' in self.asTestVMs:
                self.asRsrcs.append('4.2/usb/tst-arch.vdi');

        return self.asRsrcs;

    def actionConfig(self):

        # Some stupid trickery to guess the location of the iso. ## fixme - testsuite unzip ++
        sVBoxValidationKit_iso = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../VBoxValidationKit.iso'));
        if not os.path.isfile(sVBoxValidationKit_iso):
            sVBoxValidationKit_iso = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../VBoxTestSuite.iso'));
        if not os.path.isfile(sVBoxValidationKit_iso):
            sVBoxValidationKit_iso = '/mnt/ramdisk/vbox/svn/trunk/validationkit/VBoxValidationKit.iso';
        if not os.path.isfile(sVBoxValidationKit_iso):
            sVBoxValidationKit_iso = '/mnt/ramdisk/vbox/svn/trunk/testsuite/VBoxTestSuite.iso';
        if not os.path.isfile(sVBoxValidationKit_iso):
            sCur = os.getcwd();
            for i in range(0, 10):
                sVBoxValidationKit_iso = os.path.join(sCur, 'validationkit/VBoxValidationKit.iso');
                if os.path.isfile(sVBoxValidationKit_iso):
                    break;
                sVBoxValidationKit_iso = os.path.join(sCur, 'testsuite/VBoxTestSuite.iso');
                if os.path.isfile(sVBoxValidationKit_iso):
                    break;
                sCur = os.path.abspath(os.path.join(sCur, '..'));
                if i is None: pass; # shut up pychecker/pylint.
        if not os.path.isfile(sVBoxValidationKit_iso):
            sVBoxValidationKit_iso = '/home/bird/validationkit/VBoxValidationKit.iso';
        if not os.path.isfile(sVBoxValidationKit_iso):
            sVBoxValidationKit_iso = '/home/bird/testsuite/VBoxTestSuite.iso';

        # Make sure vboxapi has been imported so we can use the constants.
        if not self.importVBoxApi():
            return False;

        #
        # Configure the VMs we're going to use.
        #

        # Linux VMs
        if 'tst-arch' in self.asTestVMs:
            oVM = self.createTestVM('tst-arch', 1, '4.2/usb/tst-arch.vdi', sKind = 'ArchLinux_64', fIoApic = True, \
                                    eNic0AttachType = vboxcon.NetworkAttachmentType_NAT, \
                                    sDvdImage = sVBoxValidationKit_iso);
            if oVM is None:
                return False;

        return True;

    def actionExecute(self):
        """
        Execute the testcase.
        """
        fRc = self.testUsb();
        return fRc;

    def getGadgetParams(self, sHostname, sSpeed):
        """
        Returns the gadget hostname and type from the
        given hostname the test is running on and device speed we want to test.
        """
        kdGadgetsConfigured = self.kdGadgetParams.get(sHostname);
        if kdGadgetsConfigured is not None:
            return kdGadgetsConfigured.get(sSpeed);

        return (None, None);

    #
    # Test execution helpers.
    #
    def testUsbCompliance(self, oSession, oTxsSession, sUsbCtrl, sSpeed):
        """
        Test VirtualBoxs USB stack in a VM.
        """
        # Get configured USB test devices from hostname we are running on
        sGadgetHost, sGadgetType = self.getGadgetParams(self.sHostname, sSpeed);

        # Create device filter
        fRc = oSession.addUsbDeviceFilter('Compliance device', '0525', 'a4a0');
        if fRc is True:
            oUsbGadget = usbgadget.UsbGadget();
            reporter.log('Connecting to gadget: ' + sGadgetType);
            fRc = oUsbGadget.connectTo(30 * 1000, sGadgetType, sGadgetHost);
            if fRc is True:
                reporter.log('Connect succeeded');
                fRc = oUsbGadget.impersonate(usbgadget.g_ksGadgetImpersonationTest);
                if fRc is True:

                    # Wait a moment to let the USB device appear
                    self.sleep(3);

                    tupCmdLine = ('UsbTest', );
                    # Exclude a few tests which hang and cause a timeout, need investigation.
                    if sUsbCtrl is 'XHCI':
                        tupCmdLine = tupCmdLine + ('--exclude', '10', '--exclude', '24');

                    fRc = self.txsRunTest(oTxsSession, 'UsbTest', 3600 * 1000, \
                        '${CDROM}/${OS/ARCH}/UsbTest${EXESUFF}', tupCmdLine);
                    if not fRc:
                        reporter.testFailure('Running USB test utility failed');

                else:
                    reporter.testFailure('Failed to impersonate test device');

                oUsbGadget.disconnectFrom();
            else:
                reporter.testFailure('Failed to connect to USB gadget');
        else:
            reporter.testFailure('Failed to create USB device filter');

        return fRc;

    def testUsbReattach(self, oSession, oTxsSession, sUsbCtrl, sSpeed):
        """
        Tests that rapid connect/disconnect cycles work.
        """
        # Get configured USB test devices from hostname we are running on
        sGadgetHost, sGadgetType = self.getGadgetParams(self.sHostname, sSpeed);

        # Create device filter
        fRc = oSession.addUsbDeviceFilter('Compliance device', '0525', 'a4a0');
        if fRc is True:
            oUsbGadget = usbgadget.UsbGadget();
            reporter.log('Connecting to gadget: ' + sGadgetType);
            fRc = oUsbGadget.connectTo(30 * 1000, sGadgetType, sGadgetHost);
            if fRc is True:
                reporter.log('Connect succeeded');
                fRc = oUsbGadget.impersonate(usbgadget.g_ksGadgetImpersonationTest);
                if fRc is True:

                    self.sleep(1);

                    # Do a rapid disconnect reconnect cycle. Wait a second before disconnecting
                    # again or it will happen so fast that the VM can't attach the new device.
                    # @todo: Get rid of the constant wait and use an event to get notified when
                    # the device was attached.
                    for iCycle in xrange (0, self.cUsbReattachCycles):
                        fRc = oUsbGadget.disconnectUsb();
                        fRc = fRc and oUsbGadget.connectUsb();
                        if not fRc:
                            reporter.testFailure('Reattach cycle %s failed on the gadget device' % (iCycle));
                            break;
                        self.sleep(1);

                else:
                    reporter.testFailure('Failed to impersonate test device');

                oUsbGadget.disconnectFrom();
            else:
                reporter.testFailure('Failed to connect to USB gadget');
        else:
            reporter.testFailure('Failed to create USB device filter');

        return fRc;

    def testUsbOneCfg(self, sVmName, sUsbCtrl, sSpeed, sUsbTest):
        """
        Runs the specified VM thru one specified test.

        Returns a success indicator on the general test execution. This is not
        the actual test result.
        """
        oVM = self.getVmByName(sVmName);

        # Reconfigure the VM
        fRc = True;
        oSession = self.openSession(oVM);
        if oSession is not None:
            fRc = fRc and oSession.enableVirtEx(True);
            fRc = fRc and oSession.enableNestedPaging(True);

            # Make sure controllers are disabled initially.
            fRc = fRc and oSession.enableUsbOhci(False);
            fRc = fRc and oSession.enableUsbEhci(False);
            fRc = fRc and oSession.enableUsbXhci(False);

            if sUsbCtrl == 'OHCI':
                fRc = fRc and oSession.enableUsbOhci(True);
            elif sUsbCtrl == 'EHCI':
                fRc = fRc and oSession.enableUsbEhci(True);
            elif sUsbCtrl == 'XHCI':
                fRc = fRc and oSession.enableUsbXhci(True);
            fRc = fRc and oSession.saveSettings();
            fRc = oSession.close() and fRc and True; # pychecker hack.
            oSession = None;
        else:
            fRc = False;

        # Start up.
        if fRc is True:
            self.logVmInfo(oVM);
            oSession, oTxsSession = self.startVmAndConnectToTxsViaTcp(sVmName, fCdWait = False, fNatForwardingForTxs = False);
            if oSession is not None:
                self.addTask(oSession);

                # Fudge factor - Allow the guest to finish starting up.
                self.sleep(5);

                if sUsbTest == 'Compliance':
                    fRc = self.testUsbCompliance(oSession, oTxsSession, sUsbCtrl, sSpeed);
                elif sUsbTest == 'Reattach':
                    fRc = self.testUsbReattach(oSession, oTxsSession, sUsbCtrl, sSpeed);

                # cleanup.
                self.removeTask(oTxsSession);
                self.terminateVmBySession(oSession)
            else:
                fRc = False;
        return fRc;

    def testUsbForOneVM(self, sVmName):
        """
        Runs one VM thru the various configurations.
        """
        reporter.testStart(sVmName);
        for sUsbCtrl in self.asUsbCtrls:
            reporter.testStart(sUsbCtrl)
            for sUsbSpeed in self.asUsbSpeed:
                asSupportedSpeeds = self.kdUsbSpeedMappings.get(sUsbCtrl);
                if sUsbSpeed in asSupportedSpeeds:
                    reporter.testStart(sUsbSpeed)
                    for sUsbTest in self.asUsbTests:
                        reporter.testStart(sUsbTest)
                        fRc = self.testUsbOneCfg(sVmName, sUsbCtrl, sUsbSpeed, sUsbTest);
                        reporter.testDone();
                    reporter.testDone();
            reporter.testDone();
        reporter.testDone();
        return fRc;

    def testUsb(self):
        """
        Executes USB test.
        """

        reporter.log("Running on host: " + self.sHostname);

        # Loop thru the test VMs.
        for sVM in self.asTestVMs:
            # run test on the VM.
            fRc = self.testUsbForOneVM(sVM);

        return fRc;



if __name__ == '__main__':
    sys.exit(tdUsbBenchmark().main(sys.argv));

