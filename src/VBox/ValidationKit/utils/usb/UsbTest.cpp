/* $Id$ */
/** @file
 * UsbTest - User frontend for the Linux usbtest USB test and benchmarking module.
 *           Integrates with our test framework for nice outputs.
 */

/*
 * Copyright (C) 2014-2015 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include <iprt/err.h>
#include <iprt/getopt.h>
#include <iprt/path.h>
#include <iprt/param.h>
#include <iprt/process.h>
#include <iprt/stream.h>
#include <iprt/string.h>
#include <iprt/test.h>
#include <iprt/file.h>

#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/**
 * USB test request data.
 * There is no public header with this information so we define it ourself here.
 */
typedef struct USBTESTPARMS
{
    /** Specifies the test to run. */
    uint32_t        idxTest;
    /** How many iterations the test should be executed. */
    uint32_t        cIterations;
    /** Size of the data packets. */
    uint32_t        cbData;
    /** Size of  */
    uint32_t        cbVariation;
    /** Length of the S/G list for the test. */
    uint32_t        cSgLength;
    /** Returned time data after completing the test. */
    struct timeval  TimeTest;
} USBTESTPARAMS;
/** Pointer to a test parameter structure. */
typedef USBTESTPARAMS *PUSBTESTPARAMS;

/**
 * USB device descriptor. Used to search for the test device based
 * on the vendor and product id.
 */
#pragma pack(1)
typedef struct USBDEVDESC
{
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} USBDEVDESC;
#pragma pack()

#define USBTEST_REQUEST _IOWR('U', 100, USBTESTPARMS)

/**
 * Callback to set up the test parameters for a specific test.
 *
 * @returns IPRT status code.
 * @retval  VINF_SUCCESS    if setting the parameters up succeeded. Any other error code
 *                          otherwise indicating the kind of error.
 * @param   idxTest         The test index.
 * @param   pszTest         Test name.
 * @param   pParams         The USB test parameters to set up.
 */
typedef DECLCALLBACK(int) FNUSBTESTPARAMSSETUP(unsigned idxTest, const char *pszTest, PUSBTESTPARAMS pParams);
/** Pointer to a USB test parameters setup callback. */
typedef FNUSBTESTPARAMSSETUP *PFNUSBTESTPARAMSSETUP;

/**
 * USB test descriptor.
 */
typedef struct USBTESTDESC
{
    /** (Sort of) Descriptive test name. */
    const char           *pszName;
    /** Flag whether the test is excluded. */
    bool                  fExcluded;
    /** The parameter setup callback. */
    PFNUSBTESTPARAMSSETUP pfnParamsSetup;
} USBTESTDESC;
/** Pointer a USB test descriptor. */
typedef USBTESTDESC *PUSBTESTDESC;

/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/** Some forward method declarations. */
static DECLCALLBACK(int) usbTestParamsSetupReadWrite(unsigned idxTest, const char *pszTest, PUSBTESTPARAMS pParams);
static DECLCALLBACK(int) usbTestParamsSetupControlWrites(unsigned idxTest, const char *pszTest, PUSBTESTPARAMS pParams);

/** Command line parameters */
static const RTGETOPTDEF g_aCmdOptions[] =
{
    {"--device",           'd', RTGETOPT_REQ_STRING },
    {"--help",             'h', RTGETOPT_REQ_NOTHING},
    {"--exclude",          'e', RTGETOPT_REQ_UINT32}
};

static USBTESTDESC g_aTests[] =
{
    /* pszTest                             fExcluded      pfnParamsSetup */
    {"NOP",                                false,         usbTestParamsSetupReadWrite},
    {"Non-queued Bulk write",              false,         usbTestParamsSetupReadWrite},
    {"Non-queued Bulk read",               false,         usbTestParamsSetupReadWrite},
    {"Non-queued Bulk write variabe size", false,         usbTestParamsSetupReadWrite},
    {"Non-queued Bulk read variabe size",  false,         usbTestParamsSetupReadWrite},
    {"Queued Bulk write",                  false,         usbTestParamsSetupReadWrite},
    {"Queued Bulk read",                   false,         usbTestParamsSetupReadWrite},
    {"Queued Bulk write variabe size",     false,         usbTestParamsSetupReadWrite},
    {"Queued Bulk read variabe size",      false,         usbTestParamsSetupReadWrite},
    {"Chapter 9 Control Test",             false,         usbTestParamsSetupReadWrite},
    {"Queued control messaging",           false,         usbTestParamsSetupReadWrite},
    {"Unlink reads",                       false,         usbTestParamsSetupReadWrite},
    {"Unlink writes",                      false,         usbTestParamsSetupReadWrite},
    {"Set/Clear halts",                    false,         usbTestParamsSetupReadWrite},
    {"Control writes",                     false,         usbTestParamsSetupControlWrites},
    {"Isochronous write",                  false,         usbTestParamsSetupReadWrite},
    {"Isochronous read",                   false,         usbTestParamsSetupReadWrite},
    {"Bulk write unaligned (DMA)",         false,         usbTestParamsSetupReadWrite},
    {"Bulk read unaligned (DMA)",          false,         usbTestParamsSetupReadWrite},
    {"Bulk write unaligned (no DMA)",      false,         usbTestParamsSetupReadWrite},
    {"Bulk read unaligned (no DMA)",       false,         usbTestParamsSetupReadWrite},
    {"Control writes unaligned",           false,         usbTestParamsSetupControlWrites},
    {"Isochronous write unaligned",        false,         usbTestParamsSetupReadWrite},
    {"Isochronous read unaligned",         false,         usbTestParamsSetupReadWrite},
    {"Unlink queued Bulk",                 false,         usbTestParamsSetupReadWrite}
};

/** The test handle. */
static RTTEST g_hTest;

/**
 * Setup callback for basic read/write (bulk, isochronous) tests.
 *
 * @copydoc FNUSBTESTPARAMSSETUP
 */
static DECLCALLBACK(int) usbTestParamsSetupReadWrite(unsigned idxTest, const char *pszTest, PUSBTESTPARAMS pParams)
{
    NOREF(idxTest);
    NOREF(pszTest);

    pParams->cIterations = 1000;
    pParams->cbData = 512;
    pParams->cbVariation = 512;
    pParams->cSgLength = 32;

    return VINF_SUCCESS;
}

/**
 * Setup callback for the control writes test.
 *
 * @copydoc FNUSBTESTPARAMSSETUP
 */
static DECLCALLBACK(int) usbTestParamsSetupControlWrites(unsigned idxTest, const char *pszTest, PUSBTESTPARAMS pParams)
{
    NOREF(idxTest);
    NOREF(pszTest);

    pParams->cIterations = 1000;
    pParams->cbData = 512;
    /*
     * Must be smaller than cbData or the parameter check in the usbtest module fails,
     * no idea yet why it must be this.
     */
    pParams->cbVariation = 256;
    pParams->cSgLength = 32;

    return VINF_SUCCESS;
}

/**
 * Shows tool usage text.
 */
static void usbTestUsage(PRTSTREAM pStrm)
{
    char szExec[RTPATH_MAX];
    RTStrmPrintf(pStrm, "usage: %s [options]\n",
                 RTPathFilename(RTProcGetExecutablePath(szExec, sizeof(szExec))));
    RTStrmPrintf(pStrm, "\n");
    RTStrmPrintf(pStrm, "options: \n");


    for (unsigned i = 0; i < RT_ELEMENTS(g_aCmdOptions); i++)
    {
        const char *pszHelp;
        switch (g_aCmdOptions[i].iShort)
        {
            case 'h':
                pszHelp = "Displays this help and exit";
                break;
            case 'd':
                pszHelp = "Use the specified test device";
                break;
            case 'e':
                pszHelp = "Exclude the given test id from the list";
                break;
            default:
                pszHelp = "Option undocumented";
                break;
        }
        char szOpt[256];
        RTStrPrintf(szOpt, sizeof(szOpt), "%s, -%c", g_aCmdOptions[i].pszLong, g_aCmdOptions[i].iShort);
        RTStrmPrintf(pStrm, "  %-20s%s\n", szOpt, pszHelp);
    }
}

/**
 * Search for a USB test device and return the device path.
 *
 * @returns Path to the USB test device or NULL if none was found.
 */
static char *usbTestFindDevice(void)
{
    /*
     * Very crude and quick way to search for the correct test device.
     * Assumption is that the path looks like /dev/bus/usb/%3d/%3d.
     */
    uint8_t uBus = 1;
    bool fBusExists = false;
    char aszDevPath[64];

    RT_ZERO(aszDevPath);

    do
    {
        RTStrPrintf(aszDevPath, sizeof(aszDevPath), "/dev/bus/usb/%03d", uBus);

        fBusExists = RTPathExists(aszDevPath);

        if (fBusExists)
        {
            /* Check every device. */
            bool fDevExists = false;
            uint8_t uDev = 1;

            do
            {
                RTStrPrintf(aszDevPath, sizeof(aszDevPath), "/dev/bus/usb/%03d/%03d", uBus, uDev);

                fDevExists = RTPathExists(aszDevPath);

                if (fDevExists)
                {
                    RTFILE hFileDev;
                    int rc = RTFileOpen(&hFileDev, aszDevPath, RTFILE_O_OPEN | RTFILE_O_READ | RTFILE_O_DENY_NONE);
                    if (RT_SUCCESS(rc))
                    {
                        USBDEVDESC DevDesc;

                        rc = RTFileRead(hFileDev, &DevDesc, sizeof(DevDesc), NULL);
                        RTFileClose(hFileDev);

                        if (   RT_SUCCESS(rc)
                            && DevDesc.idVendor == 0x0525
                            && DevDesc.idProduct == 0xa4a0)
                            return RTStrDup(aszDevPath);
                    }
                }

                uDev++;
            } while (fDevExists);
        }

        uBus++;
    } while (fBusExists);

    return NULL;
}

static int usbTestIoctl(int iDevFd, int iInterface, PUSBTESTPARAMS pParams)
{
    struct usbdevfs_ioctl IoCtlData;

    IoCtlData.ifno = iInterface;
    IoCtlData.ioctl_code = (int)USBTEST_REQUEST;
    IoCtlData.data = pParams;
    return ioctl(iDevFd, USBDEVFS_IOCTL, &IoCtlData);
}

/**
 * Test execution worker.
 *
 * @returns nothing.
 * @param   pszDevice    The device to use for testing.
 */
static void usbTestExec(const char *pszDevice)
{
    int iDevFd;

    RTTestSub(g_hTest, "Opening device");
    iDevFd = open(pszDevice, O_RDWR);
    if (iDevFd != -1)
    {
        USBTESTPARAMS Params;

        RTTestPassed(g_hTest, "Opening device successful\n");

        for (unsigned i = 0; i < RT_ELEMENTS(g_aTests); i++)
        {
            RTTestSub(g_hTest, g_aTests[i].pszName);

            if (g_aTests[i].fExcluded)
            {
                RTTestSkipped(g_hTest, "Excluded from list");
                continue;
            }

            int rc = g_aTests[i].pfnParamsSetup(i, g_aTests[i].pszName, &Params);
            if (RT_SUCCESS(rc))
            {
                Params.idxTest = i;

                /* Assume the test interface has the number 0 for now. */
                int rcPosix = usbTestIoctl(iDevFd, 0, &Params);
                if (rcPosix < 0 && errno == EOPNOTSUPP)
                {
                    RTTestSkipped(g_hTest, "Not supported");
                    continue;
                }

                if (rcPosix < 0)
                    RTTestFailed(g_hTest, "Test failed with %Rrc\n", RTErrConvertFromErrno(errno));
                else
                {
                    uint64_t u64Ns = Params.TimeTest.tv_sec * RT_NS_1SEC + Params.TimeTest.tv_usec * RT_NS_1US;
                    RTTestValue(g_hTest, "Runtime", u64Ns, RTTESTUNIT_NS);
                }
            }
            else
                RTTestFailed(g_hTest, "Setting up test parameters failed with %Rrc\n", rc);
            RTTestSubDone(g_hTest);
        }

        close(iDevFd);
    }
    else
        RTTestFailed(g_hTest, "Opening device failed with %Rrc\n", RTErrConvertFromErrno(errno));

}

int main(int argc, char *argv[])
{
    /*
     * Init IPRT and globals.
     */
    int rc = RTTestInitAndCreate("UsbTest", &g_hTest);
    if (rc)
        return rc;

    /*
     * Default values.
     */
    const char *pszDevice = NULL;

    RTGETOPTUNION ValueUnion;
    RTGETOPTSTATE GetState;
    RTGetOptInit(&GetState, argc, argv, g_aCmdOptions, RT_ELEMENTS(g_aCmdOptions), 1, 0 /* fFlags */);
    while ((rc = RTGetOpt(&GetState, &ValueUnion)))
    {
        switch (rc)
        {
            case 'h':
                usbTestUsage(g_pStdOut);
                return RTEXITCODE_SUCCESS;
            case 'd':
                pszDevice = ValueUnion.psz;
                break;
            case 'e':
                if (ValueUnion.u32 < RT_ELEMENTS(g_aTests))
                    g_aTests[ValueUnion.u32].fExcluded = true;
                else
                {
                    RTTestPrintf(g_hTest, RTTESTLVL_FAILURE, "Invalid test number passed to --exclude\n");
                    RTTestErrorInc(g_hTest);
                    return RTGetOptPrintError(VERR_INVALID_PARAMETER, &ValueUnion);
                }
                break;
            default:
                return RTGetOptPrintError(rc, &ValueUnion);
        }
    }

    /*
     * Start testing.
     */
    RTTestBanner(g_hTest);

    /* Find the first test device if none was given. */
    if (!pszDevice)
    {
        RTTestSub(g_hTest, "Detecting device");
        pszDevice = usbTestFindDevice();
        if (!pszDevice)
            RTTestFailed(g_hTest, "Failed to find suitable device\n");

        RTTestSubDone(g_hTest);
    }

    if (pszDevice)
        usbTestExec(pszDevice);

    RTEXITCODE rcExit = RTTestSummaryAndDestroy(g_hTest);
    return rcExit;
}

