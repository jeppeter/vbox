/* $Id */
/** @file
 * OSS (Open Sound System) host audio backend.
 */

/*
 * Copyright (C) 2014 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 * --------------------------------------------------------------------
 */
#include "DrvAudio.h"
#include "AudioMixBuffer.h"

#include "VBoxDD.h"
#include "vl_vbox.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/soundcard.h>
#include <unistd.h>

#include <iprt/alloc.h>
#include <iprt/uuid.h> /* For PDMIBASE_2_PDMDRV. */
#include <VBox/vmm/pdmaudioifs.h>

#ifdef LOG_GROUP
# undef LOG_GROUP
#endif
#define LOG_GROUP LOG_GROUP_DEV_AUDIO
#include <VBox/log.h>


/**
 * OSS host audio driver instance data.
 * @implements PDMIAUDIOCONNECTOR
 */
typedef struct DRVHOSTOSSAUDIO
{
    /** Pointer to the driver instance structure. */
    PPDMDRVINS         pDrvIns;
    /** Pointer to R3 host audio interface. */
    PDMIHOSTAUDIO      IHostAudioR3;
    /** Error count for not flooding the release log.
     *  UINT32_MAX for unlimited logging. */
    uint32_t           cLogErrors;
} DRVHOSTOSSAUDIO, *PDRVHOSTOSSAUDIO;

typedef struct OSSAUDIOSTREAMCFG
{
    PDMAUDIOFMT       enmFormat;
    PDMAUDIOENDIANESS enmEndianess;
    uint16_t          uFreq;
    uint8_t           cChannels;
    uint16_t          cFragments;
    uint32_t          cbFragmentSize;
} OSSAUDIOSTREAMCFG, *POSSAUDIOSTREAMCFG;

typedef struct OSSAUDIOSTREAMIN
{
    /** Note: Always must come first! */
    PDMAUDIOHSTSTRMIN  hw;
    int                hFile;
    int                cFragments;
    int                cbFragmentSize;
    void              *pvPCMBuf;
    int                old_optr;
} OSSAUDIOSTREAMIN, *POSSAUDIOSTREAMIN;

typedef struct OSSAUDIOSTREAMOUT
{
    /** Note: Always must come first! */
    PDMAUDIOHSTSTRMOUT  hw;
    int                 hFile;
    int                 cFragments;
    int                 cbFragmentSize;
#ifndef RT_OS_L4
    bool                fMemMapped;
#endif
    void               *pvPCMBuf;
    int                 old_optr;
} OSSAUDIOSTREAMOUT, *POSSAUDIOSTREAMOUT;

typedef struct OSSAUDIOCFG
{
#ifndef RT_OS_L4
    bool try_mmap;
#endif
    int nfrags;
    int fragsize;
    const char *devpath_out;
    const char *devpath_in;
    int debug;
} OSSAUDIOCFG, *POSSAUDIOCFG;

static OSSAUDIOCFG s_OSSConf = 
{
#ifndef RT_OS_L4
    false,
#endif
    4,
    4096,
    "/dev/dsp",
    "/dev/dsp",
    0
};


/* http://www.df.lth.se/~john_e/gems/gem002d.html */
static uint32_t popcount(uint32_t u)
{
    u = ((u&0x55555555) + ((u>>1)&0x55555555));
    u = ((u&0x33333333) + ((u>>2)&0x33333333));
    u = ((u&0x0f0f0f0f) + ((u>>4)&0x0f0f0f0f));
    u = ((u&0x00ff00ff) + ((u>>8)&0x00ff00ff));
    u = ( u&0x0000ffff) + (u>>16);
    return u;
}

static uint32_t lsbindex(uint32_t u)
{
    return popcount ((u&-u)-1);
}

static int drvHostOSSAudioFmtToOSS(PDMAUDIOFMT fmt)
{
    switch (fmt)
    {
        case AUD_FMT_S8:
            return AFMT_S8;

        case AUD_FMT_U8:
            return AFMT_U8;

        case AUD_FMT_S16:
            return AFMT_S16_LE;

        case AUD_FMT_U16:
            return AFMT_U16_LE;

        default:
            break;
    }

    AssertMsgFailed(("Format %ld not supported\n", fmt));
    return AFMT_U8;
}

static int drvHostOSSAudioOSSToFmt(int fmt, 
                                   PDMAUDIOFMT *pFmt, PDMAUDIOENDIANESS *pEndianess)
{
    switch (fmt)
    {
        case AFMT_S8:
            *pFmt = AUD_FMT_S8;
            if (pEndianess)
                *pEndianess = PDMAUDIOENDIANESS_LITTLE;
            break;

        case AFMT_U8:
            *pFmt = AUD_FMT_U8;
            if (pEndianess)
                *pEndianess = PDMAUDIOENDIANESS_LITTLE;
            break;

        case AFMT_S16_LE:
            *pFmt = AUD_FMT_S16;
            if (pEndianess)
                *pEndianess = PDMAUDIOENDIANESS_LITTLE;
            break;

        case AFMT_U16_LE:
            *pFmt = AUD_FMT_U16;
            if (pEndianess)
                *pEndianess = PDMAUDIOENDIANESS_LITTLE;
            break;

        case AFMT_S16_BE:
            *pFmt = AUD_FMT_S16;
            if (pEndianess)
                *pEndianess = PDMAUDIOENDIANESS_BIG;
            break;

        case AFMT_U16_BE:
            *pFmt = AUD_FMT_U16;
            if (pEndianess)
                *pEndianess = PDMAUDIOENDIANESS_BIG;
            break;

        default:
            AssertMsgFailed(("Format %ld not supported\n", fmt));
            return VERR_NOT_SUPPORTED;
    }

    return VINF_SUCCESS;
}

static void drvHostOSSAudioClose(int *phFile)
{
    if (!phFile)
        return;

    if (*phFile) 
    {
        if (close(*phFile))
            LogRel(("Audio: Closing descriptor failed: %s\n", 
                    strerror(errno)));
        else
            *phFile = -1;
    }
}

static int drvHostOSSAudioOpen(bool fIn, 
                               POSSAUDIOSTREAMCFG pReq, POSSAUDIOSTREAMCFG pObt,
                               int *phFile)
{
    AssertPtrReturn(pReq, VERR_INVALID_POINTER);
    AssertPtrReturn(pObt, VERR_INVALID_POINTER);
    AssertPtrReturn(phFile, VERR_INVALID_POINTER);

    int rc;
    int hFile;

    do 
    {
        const char *pszDSP = fIn ? s_OSSConf.devpath_in : s_OSSConf.devpath_out;
        if (!pszDSP)
        {
            LogRel(("Audio: Invalid or no %s DSP set\n", 
                    fIn ? "input" : "output"));
            rc = VERR_INVALID_PARAMETER;
            break;
        }

        hFile = open(pszDSP, (fIn ? O_RDONLY : O_WRONLY) | O_NONBLOCK);
        if (!hFile) 
        {
            LogRel(("Audio: Failed to open %s: %s\n", pszDSP, strerror(errno)));
            rc = RTErrConvertFromErrno(errno);
            break;
        }

        int iFormat = drvHostOSSAudioFmtToOSS(pReq->enmFormat);
        if (ioctl(hFile, SNDCTL_DSP_SAMPLESIZE, &iFormat)) 
        {
            LogRel(("Audio: Failed to set audio format to %ld\n",
                    iFormat, strerror(errno)));
            rc = RTErrConvertFromErrno(errno);
            break;
        }
        
        int cChannels = pReq->cChannels;
        if (ioctl(hFile, SNDCTL_DSP_CHANNELS, &cChannels)) 
        {
            LogRel(("Audio: Failed to set number of audio channels (%d): %s\n",
                     pReq->cChannels, strerror(errno)));
            rc = RTErrConvertFromErrno(errno);
            break;
        }

        int freq = pReq->uFreq;
        if (ioctl(hFile, SNDCTL_DSP_SPEED, &freq)) 
        {
            LogRel(("Audio: Failed to set audio frequency (%dHZ): %s\n", 
                    pReq->uFreq, strerror(errno)));
            rc = RTErrConvertFromErrno(errno);
            break;
        }

        /* Obsolete on Solaris (using O_NONBLOCK is sufficient). */
#if !(defined(VBOX) && defined(RT_OS_SOLARIS))
        if (ioctl(hFile, SNDCTL_DSP_NONBLOCK)) 
        {
            LogRel(("Audio: Failed to set non-blocking mode: %s\n",
                    strerror(errno)));
            rc = RTErrConvertFromErrno(errno);
            break;
        }
#endif
        int mmmmssss = (pReq->cFragments << 16) | lsbindex(pReq->cbFragmentSize);
        if (ioctl(hFile, SNDCTL_DSP_SETFRAGMENT, &mmmmssss)) 
        {
            LogRel(("Audio: Failed to set %RU16 fragments to %RU32 bytes each: %s\n",
                    pReq->cFragments, pReq->cbFragmentSize, strerror(errno)));
            rc = RTErrConvertFromErrno(errno);
            break;
        }

        audio_buf_info abinfo;
        if (ioctl(hFile, fIn ? SNDCTL_DSP_GETISPACE : SNDCTL_DSP_GETOSPACE, 
                  &abinfo)) 
        {
            LogRel(("Audio: Failed to retrieve buffer length: %s\n", strerror(errno)));
            rc = RTErrConvertFromErrno(errno);
            break;
        }

        rc = drvHostOSSAudioOSSToFmt(iFormat, 
                                     &pObt->enmFormat, &pObt->enmEndianess);
        if (RT_SUCCESS(rc)) 
        {
            pObt->cChannels      = cChannels;
            pObt->uFreq          = freq;
            pObt->cFragments     = abinfo.fragstotal;
            pObt->cbFragmentSize = abinfo.fragsize;

            *phFile = hFile;
        }
    }
    while (0); 

    if (RT_FAILURE(rc)) 
        drvHostOSSAudioClose(&hFile);

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static DECLCALLBACK(int) drvHostOSSAudioControlIn(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHSTSTRMIN pHstStrmIn,
                                                   PDMAUDIOSTREAMCMD enmStreamCmd)
{
    NOREF(pInterface);
    NOREF(pHstStrmIn);
    NOREF(enmStreamCmd);

    /** @todo Nothing to do here right now!? */

    return VINF_SUCCESS;
}

static DECLCALLBACK(int) drvHostOSSAudioControlOut(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHSTSTRMOUT pHstStrmOut,
                                                   PDMAUDIOSTREAMCMD enmStreamCmd)
{
    NOREF(pInterface);
    AssertPtrReturn(pHstStrmOut, VERR_INVALID_POINTER);

    POSSAUDIOSTREAMOUT pThisStrmOut = (POSSAUDIOSTREAMOUT)pHstStrmOut;

#ifdef RT_OS_L4
    return VINF_SUCCESS;
#else
    if (!pThisStrmOut->fMemMapped)
        return VINF_SUCCESS;
#endif

    int rc, mask;
    switch (enmStreamCmd)
    {
        case PDMAUDIOSTREAMCMD_ENABLE:
        {
            audio_pcm_info_clear_buf(&pHstStrmOut->Props, 
                                     pThisStrmOut->pvPCMBuf, audioMixBufSize(&pHstStrmOut->MixBuf));

            mask = PCM_ENABLE_OUTPUT;
            if (ioctl(pThisStrmOut->hFile, SNDCTL_DSP_SETTRIGGER, &mask) < 0) 
            {
                LogRel(("Audio: Failed to enable output stream: %s\n", 
                        strerror(errno)));
                rc = RTErrConvertFromErrno(errno);
            }

            break;
        }

        case PDMAUDIOSTREAMCMD_DISABLE:
        {
            mask = 0;
            if (ioctl(pThisStrmOut->hFile, SNDCTL_DSP_SETTRIGGER, &mask) < 0) 
            {
                LogRel(("Audio: Failed to disable output stream: %s\n", 
                       strerror(errno)));
                rc = RTErrConvertFromErrno(errno);
            }

            break;
        }

        default:
            AssertMsgFailed(("Invalid command %ld\n", enmStreamCmd));
            rc = VERR_INVALID_PARAMETER;
            break;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static DECLCALLBACK(int) drvHostOSSAudioInit(PPDMIHOSTAUDIO pInterface)
{
    NOREF(pInterface);

    LogFlowFuncEnter();

    return VINF_SUCCESS;
}

static DECLCALLBACK(int) drvHostOSSAudioCaptureIn(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHSTSTRMIN pHstStrmIn,
                                                  uint32_t *pcSamplesCaptured)
{
    *pcSamplesCaptured = 0;
    return VINF_SUCCESS;
}

static DECLCALLBACK(int) drvHostOSSAudioFiniIn(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHSTSTRMIN pHstStrmIn)
{
    NOREF(pInterface);
    AssertPtrReturn(pHstStrmIn, VERR_INVALID_POINTER);

    POSSAUDIOSTREAMIN pThisStrmIn = (POSSAUDIOSTREAMIN)pHstStrmIn;

    LogFlowFuncEnter();

    RTMemFree(pThisStrmIn->pvPCMBuf);
    pThisStrmIn->pvPCMBuf = NULL;

    return VINF_SUCCESS;
}

static DECLCALLBACK(int) drvHostOSSAudioFiniOut(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHSTSTRMOUT pHstStrmOut)
{
    NOREF(pInterface);
    AssertPtrReturn(pHstStrmOut, VERR_INVALID_POINTER);

    POSSAUDIOSTREAMOUT pThisStrmOut = (POSSAUDIOSTREAMOUT)pHstStrmOut;

    LogFlowFuncEnter();

#ifndef RT_OS_L4
    if (!pThisStrmOut->fMemMapped) 
    {
        RTMemFree(pThisStrmOut->pvPCMBuf);
        pThisStrmOut->pvPCMBuf = NULL;
    }
#endif

    return VINF_SUCCESS;
}

static DECLCALLBACK(int) drvHostOSSAudioGetConf(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDCFG pCfg)
{
    NOREF(pInterface);

    pCfg->cbStreamOut = sizeof(OSSAUDIOSTREAMOUT);
    pCfg->cbStreamIn = sizeof(OSSAUDIOSTREAMIN);
    pCfg->cMaxHstStrmsOut = INT_MAX;
    pCfg->cMaxHstStrmsIn = INT_MAX;

    return VINF_SUCCESS;
}

static DECLCALLBACK(int) drvHostOSSAudioInitIn(PPDMIHOSTAUDIO pInterface,
                                               PPDMAUDIOHSTSTRMIN pHstStrmIn, PPDMAUDIOSTREAMCFG pCfg,
                                               PDMAUDIORECSOURCE enmRecSource,
                                               uint32_t *pcSamples)
{
    NOREF(pInterface);
    AssertPtrReturn(pHstStrmIn, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfg, VERR_INVALID_POINTER);

    POSSAUDIOSTREAMIN pThisStrmIn = (POSSAUDIOSTREAMIN)pHstStrmIn;

    int rc;
    int hFile = -1;

    do
    {
        uint32_t cSamples;

        OSSAUDIOSTREAMCFG reqStream, obtStream;
        reqStream.enmFormat      = pCfg->enmFormat;
        reqStream.uFreq          = pCfg->uHz;
        reqStream.cChannels      = pCfg->cChannels;
        reqStream.cFragments     = s_OSSConf.nfrags;
        reqStream.cbFragmentSize = s_OSSConf.fragsize;

        rc = drvHostOSSAudioOpen(true /* fIn */, 
                                 &reqStream, &obtStream, &hFile);
        if (RT_SUCCESS(rc))
        {
            if (obtStream.cFragments * obtStream.cbFragmentSize & pHstStrmIn->Props.uAlign) 
                LogRel(("Audio: Warning: Misaligned DAC output buffer: Size = %zu, Alignment = %u\n",
                        obtStream.cFragments * obtStream.cbFragmentSize, 
                        pHstStrmIn->Props.uAlign + 1));

            pThisStrmIn->hFile = hFile;

            PDMAUDIOSTREAMCFG streamCfg;
            streamCfg.enmFormat     = obtStream.enmFormat;
            streamCfg.uHz           = obtStream.uFreq;
            streamCfg.cChannels     = pCfg->cChannels;
            streamCfg.enmEndianness = obtStream.enmEndianess;

            rc = drvAudioStreamCfgToProps(&streamCfg, &pHstStrmIn->Props);
            if (RT_SUCCESS(rc)) 
            {
                cSamples = (obtStream.cFragments * obtStream.cbFragmentSize) 
                           >> pHstStrmIn->Props.cShift;  
            }
        }

        if (RT_SUCCESS(rc)) 
        {
            pThisStrmIn->pvPCMBuf = RTMemAlloc(cSamples * (1 << pHstStrmIn->Props.cShift));
            if (!pThisStrmIn->pvPCMBuf) 
            {
                LogRel(("Audio: Failed allocating ADC buffer with %RU32 samples, each %d bytes\n",
                        cSamples, 1 << pHstStrmIn->Props.cShift));
                rc = VERR_NO_MEMORY;
            }

            if (pcSamples)
                *pcSamples = cSamples;
        }

    } while (0);

    if (RT_FAILURE(rc)) 
        drvHostOSSAudioClose(&hFile);
            
    LogFlowFuncLeaveRC(rc);
    return rc;
}

static DECLCALLBACK(int) drvHostOSSAudioInitOut(PPDMIHOSTAUDIO pInterface,
                                                PPDMAUDIOHSTSTRMOUT pHstStrmOut, PPDMAUDIOSTREAMCFG pCfg,
                                                uint32_t *pcSamples)
{
    NOREF(pInterface);
    AssertPtrReturn(pHstStrmOut, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfg, VERR_INVALID_POINTER);

    POSSAUDIOSTREAMOUT pThisStrmOut = (POSSAUDIOSTREAMOUT)pHstStrmOut;

    int rc;
    int hFile = -1;

    do
    {
        uint32_t cSamples;

        OSSAUDIOSTREAMCFG reqStream, obtStream;
        reqStream.enmFormat      = pCfg->enmFormat;
        reqStream.uFreq          = pCfg->uHz;
        reqStream.cChannels      = pCfg->cChannels;
        reqStream.cFragments     = s_OSSConf.nfrags;
        reqStream.cbFragmentSize = s_OSSConf.fragsize;

        rc = drvHostOSSAudioOpen(false /* fIn */, 
                                 &reqStream, &obtStream, &hFile);
        if (RT_SUCCESS(rc))
        {
            if (obtStream.cFragments * obtStream.cbFragmentSize & pHstStrmOut->Props.uAlign) 
                LogRel(("Audio: Warning: Misaligned DAC output buffer: Size = %zu, Alignment = %u\n",
                        obtStream.cFragments * obtStream.cbFragmentSize, 
                        pHstStrmOut->Props.uAlign + 1));

            pThisStrmOut->hFile = hFile;

            PDMAUDIOSTREAMCFG streamCfg;
            streamCfg.enmFormat     = obtStream.enmFormat;
            streamCfg.uHz           = obtStream.uFreq;
            streamCfg.cChannels     = pCfg->cChannels;
            streamCfg.enmEndianness = obtStream.enmEndianess;

            rc = drvAudioStreamCfgToProps(&streamCfg, &pHstStrmOut->Props);
            if (RT_SUCCESS(rc)) 
            {
                cSamples = (obtStream.cFragments * obtStream.cbFragmentSize) 
                           >> pHstStrmOut->Props.cShift;  
            }
        }

        if (RT_SUCCESS(rc))
        {
#ifndef RT_OS_L4
            pThisStrmOut->fMemMapped = false;
            if (s_OSSConf.try_mmap) 
            {
                pThisStrmOut->pvPCMBuf = mmap(0, cSamples << pHstStrmOut->Props.cShift,
                                              PROT_READ | PROT_WRITE, MAP_SHARED, hFile, 0);
                if (pThisStrmOut->pvPCMBuf == MAP_FAILED) 
                {
                    LogRel(("Audio: Failed to memory map %zu bytes of DAC output file: %s\n", 
                            cSamples << pHstStrmOut->Props.cShift, strerror(errno)));
                    rc = RTErrConvertFromErrno(errno);
                    break;
                }
                else 
                {
                    int mask = 0;
                    if (ioctl(hFile, SNDCTL_DSP_SETTRIGGER, &mask) < 0) 
                    {
                        LogRel(("Audio: Failed to retrieve initial trigger mask: %s\n", 
                                strerror(errno)));
                        rc = RTErrConvertFromErrno(errno);
                        /* Note: No break here, need to unmap file first! */
                    }
                    else 
                    {
                        mask = PCM_ENABLE_OUTPUT;
                        if (ioctl (hFile, SNDCTL_DSP_SETTRIGGER, &mask) < 0) 
                        {
                            LogRel(("Audio: Failed to retrieve PCM_ENABLE_OUTPUT mask: %s\n", 
                                strerror(errno)));
                            rc = RTErrConvertFromErrno(errno);
                            /* Note: No break here, need to unmap file first! */
                        }
                        else
                            pThisStrmOut->fMemMapped = true;
                    }

                    if (!pThisStrmOut->fMemMapped) 
                    {
                        int rc2 = munmap(pThisStrmOut->pvPCMBuf, 
                                         cSamples << pHstStrmOut->Props.cShift);
                        if (rc2)
                            LogRel(("Audio: Failed to unmap DAC output file: %s\n", 
                                    strerror(errno)));

                        break;
                    }
                }
            }
#endif /* !RT_OS_L4 */

            /* Memory mapping failed above? Try allocating an own buffer. */
#ifndef RT_OS_L4
            if (!pThisStrmOut->fMemMapped) 
            {
#endif
                pThisStrmOut->pvPCMBuf = RTMemAlloc(cSamples * (1 << pHstStrmOut->Props.cShift));
                if (!pThisStrmOut->pvPCMBuf) 
                {
                    LogRel(("Audio: Failed allocating DAC buffer with %RU32 samples, each %d bytes\n",
                            cSamples, 1 << pHstStrmOut->Props.cShift));
                    rc = VERR_NO_MEMORY;
                }

                break;
#ifndef RT_OS_L4
            }
#endif
            if (pcSamples)
                *pcSamples = cSamples;
        }

    } while (0);

    if (RT_FAILURE(rc)) 
        drvHostOSSAudioClose(&hFile);
            
    LogFlowFuncLeaveRC(rc);
    return rc;
}

static DECLCALLBACK(int) drvHostOSSAudioPlayOut(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHSTSTRMOUT pHstStrmOut,
                                                uint32_t *pcSamplesPlayed)
{
    NOREF(pInterface);
    AssertPtrReturn(pHstStrmOut, VERR_INVALID_POINTER);

    POSSAUDIOSTREAMOUT pThisStrmOut = (POSSAUDIOSTREAMOUT)pHstStrmOut;

    int rc;
    uint32_t cbReadTotal = 0;
    count_info cntinfo;

    do
    {
        size_t cbBuf = audioMixBufSizeBytes(&pHstStrmOut->MixBuf);

        uint32_t cLive = drvAudioHstOutSamplesLive(pHstStrmOut, 
                                                   NULL /* pcStreamsLive */);
        uint32_t cToRead;

#ifndef RT_OS_L4
        if (pThisStrmOut->fMemMapped) 
        {
            /* Get current playback pointer. */
            int rc2 = ioctl(pThisStrmOut->hFile, SNDCTL_DSP_GETOPTR, &cntinfo);
            if (!rc2) 
            {
                LogRel(("Audio: Failed to retrieve current playback pointer: %s\n", 
                        strerror(errno)));
                rc = RTErrConvertFromErrno(errno);
                break;
            }

            /* Nothing to play? */
            if (cntinfo.ptr == pThisStrmOut->old_optr) 
                break;

            int cbData;
            if (cntinfo.ptr > pThisStrmOut->old_optr)
                cbData = cntinfo.ptr - pThisStrmOut->old_optr;
            else
                cbData = cbBuf + cntinfo.ptr - pThisStrmOut->old_optr;
            Assert(cbData);

            cToRead = RT_MIN((uint32_t)AUDIOMIXBUF_B2S(&pHstStrmOut->MixBuf, cbData), 
                             cLive);
        }
        else 
        {
#endif
            audio_buf_info abinfo;
            int rc2 = ioctl(pThisStrmOut->hFile, SNDCTL_DSP_GETOSPACE, &abinfo);
            if (!rc2) 
            {
                LogRel(("Audio: Failed to retrieve current playback buffer: %s\n", 
                        strerror(errno)));
                rc = RTErrConvertFromErrno(errno);
                break;
            }

            if ((size_t)abinfo.bytes > cbBuf) 
            {
                LogFlowFunc(("Warning: Invalid available size, size=%d, bufsize=%d\n",
                             abinfo.bytes, cbBuf));
                abinfo.bytes = cbBuf;
                /* Keep going. */
            }

            if (!abinfo.bytes < 0)
            {
                LogFlowFunc(("Warning: Invalid available size, size=%d, bufsize=%d\n",
                             abinfo.bytes, cbBuf));
                rc = VERR_INVALID_PARAMETER;
                break;
            }

            cToRead = RT_MIN((uint32_t)AUDIOMIXBUF_B2S(&pHstStrmOut->MixBuf, abinfo.bytes), 
                             cLive);
            if (!cToRead)
                break;
#ifndef RT_OS_L4
        }
#endif
        size_t cbToRead = AUDIOMIXBUF_S2B(&pHstStrmOut->MixBuf, cToRead);
        LogFlowFunc(("cbToRead=%zu\n", cbToRead));

        uint32_t cRead, cbRead;
        while (cbToRead)
        {
            rc = audioMixBufReadCirc(&pHstStrmOut->MixBuf, 
                                     pThisStrmOut->pvPCMBuf, cbToRead, &cRead);
            if (RT_FAILURE(rc))
                break;

            cbRead = AUDIOMIXBUF_S2B(&pHstStrmOut->MixBuf, cRead);
            ssize_t cbWritten = write(pThisStrmOut->hFile, pThisStrmOut->pvPCMBuf, 
                                      cbRead);
            if (cbWritten == -1) 
            {
                LogRel(("Audio: Failed writing output data %s\n", strerror(errno)));
                rc = RTErrConvertFromErrno(errno);
                break;
            }

            Assert(cbToRead >= cRead);
            cbToRead -= cbRead;
            cbReadTotal += cbRead;
        }

        if (RT_FAILURE(rc))
            break;

#ifndef RT_OS_L4
        /* Update read pointer. */
        if (pThisStrmOut->fMemMapped)
            pThisStrmOut->old_optr = cntinfo.ptr;
#endif

    } while(0);

    uint32_t cReadTotal = AUDIOMIXBUF_B2S(&pHstStrmOut->MixBuf, cbReadTotal);
    if (cReadTotal)
        audioMixBufFinish(&pHstStrmOut->MixBuf, cReadTotal);

    if (pcSamplesPlayed)
        *pcSamplesPlayed = cReadTotal;

    LogFlowFunc(("cReadTotal=%RU32 (%RU32 bytes), rc=%Rrc\n", 
                 cReadTotal, cbReadTotal, rc));
    return rc;
}

/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
static DECLCALLBACK(void *) drvHostOSSAudioQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PPDMDRVINS pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVHOSTOSSAUDIO  pThis   = PDMINS_2_DATA(pDrvIns, PDRVHOSTOSSAUDIO);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIHOSTAUDIO, &pThis->IHostAudioR3);

    return NULL;
}

static DECLCALLBACK(void) drvHostOSSAudioDestruct(PPDMDRVINS pDrvIns)
{
}

/**
 * Constructs an OSS audio driver instance.
 *
 * @copydoc FNPDMDRVCONSTRUCT
 */
static DECLCALLBACK(int) drvHostOSSAudioConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags)
{
    PDRVHOSTOSSAUDIO pThis = PDMINS_2_DATA(pDrvIns, PDRVHOSTOSSAUDIO);
    LogRel(("Audio: Initializing OSS driver\n"));

    /*
     * Init the static parts.
     */
    pThis->pDrvIns                    = pDrvIns;
    /* IBase */
    pDrvIns->IBase.pfnQueryInterface  = drvHostOSSAudioQueryInterface;
    pThis->IHostAudioR3.pfnInitIn     = drvHostOSSAudioInitIn;
    pThis->IHostAudioR3.pfnInitOut    = drvHostOSSAudioInitOut;
    pThis->IHostAudioR3.pfnControlIn  = drvHostOSSAudioControlIn;
    pThis->IHostAudioR3.pfnControlOut = drvHostOSSAudioControlOut;
    pThis->IHostAudioR3.pfnFiniIn     = drvHostOSSAudioFiniIn;
    pThis->IHostAudioR3.pfnFiniOut    = drvHostOSSAudioFiniOut;
    pThis->IHostAudioR3.pfnCaptureIn  = drvHostOSSAudioCaptureIn;
    pThis->IHostAudioR3.pfnPlayOut    = drvHostOSSAudioPlayOut;
    pThis->IHostAudioR3.pfnGetConf    = drvHostOSSAudioGetConf;
    pThis->IHostAudioR3.pfnInit       = drvHostOSSAudioInit;

    return VINF_SUCCESS;
}

/**
 * Char driver registration record.
 */
const PDMDRVREG g_DrvHostOSSAudio =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szName */
    "OSSAudio",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "OSS audio host driver",
    /* fFlags */
    PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass. */
    PDM_DRVREG_CLASS_AUDIO,
    /* cMaxInstances */
    ~0U,
    /* cbInstance */
    sizeof(DRVHOSTOSSAUDIO),
    /* pfnConstruct */
    drvHostOSSAudioConstruct,
    /* pfnDestruct */
    drvHostOSSAudioDestruct,
    /* pfnRelocate */
    NULL,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    NULL,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnPowerOff */
    NULL,
    /* pfnSoftReset */
    NULL,
    /* u32EndVersion */
    PDM_DRVREG_VERSION
};