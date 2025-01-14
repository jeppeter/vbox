/* $Id$ */
/** @file
 * GIM - Guest Interface Manager, Microsoft Hyper-V, All Contexts.
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
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_GIM
#include "GIMHvInternal.h"
#include "GIMInternal.h"

#include <iprt/asm-amd64-x86.h>
#ifdef IN_RING3
# include <iprt/mem.h>
#endif

#include <VBox/err.h>
#include <VBox/vmm/em.h>
#include <VBox/vmm/hm.h>
#include <VBox/vmm/tm.h>
#include <VBox/vmm/vm.h>
#include <VBox/vmm/pgm.h>
#include <VBox/vmm/pdmdev.h>
#include <VBox/vmm/pdmapi.h>


#ifdef IN_RING3
/**
 * Read and validate slow hypercall parameters.
 *
 * @returns VBox status code.
 * @param   pVM               The cross context VM structure.
 * @param   pCtx              Pointer to the guest-CPU context.
 * @param   fIs64BitMode      Whether the guest is currently in 64-bit mode or not.
 * @param   enmParam          The hypercall parameter type.
 * @param   prcHv             Where to store the Hyper-V status code. Only valid
 *                            to the caller when this function returns
 *                            VINF_SUCCESS.
 */
static int gimHvReadSlowHypercallParam(PVM pVM, PCPUMCTX pCtx, bool fIs64BitMode, GIMHVHYPERCALLPARAM enmParam, int *prcHv)
{
    int       rc = VINF_SUCCESS;
    PGIMHV    pHv = &pVM->gim.s.u.Hv;
    RTGCPHYS  GCPhysParam;
    void     *pvDst;
    if (enmParam == GIMHVHYPERCALLPARAM_IN)
    {
        GCPhysParam = fIs64BitMode ? pCtx->rdx : (pCtx->rbx << 32) | pCtx->ecx;
        pvDst = pHv->pbHypercallIn;
        pHv->GCPhysHypercallIn = GCPhysParam;
    }
    else
    {
        GCPhysParam = fIs64BitMode ? pCtx->r8 : (pCtx->rdi << 32) | pCtx->esi;
        pvDst = pHv->pbHypercallOut;
        pHv->GCPhysHypercallOut = GCPhysParam;
        Assert(enmParam == GIMHVHYPERCALLPARAM_OUT);
    }

    const char *pcszParam = enmParam == GIMHVHYPERCALLPARAM_IN ? "input" : "output";  NOREF(pcszParam);
    if (RT_ALIGN_64(GCPhysParam, 8) == GCPhysParam)
    {
        if (PGMPhysIsGCPhysNormal(pVM, GCPhysParam))
        {
            rc = PGMPhysSimpleReadGCPhys(pVM, pvDst, GCPhysParam, GIM_HV_PAGE_SIZE);
            if (RT_SUCCESS(rc))
            {
                *prcHv = GIM_HV_STATUS_SUCCESS;
                return VINF_SUCCESS;
            }
            LogRel(("GIM: HyperV: Failed reading %s param at %#RGp. rc=%Rrc\n", pcszParam, GCPhysParam, rc));
            rc = VERR_GIM_HYPERCALL_MEMORY_READ_FAILED;
        }
        else
        {
            Log(("GIM: HyperV: Invalid %s param address %#RGp\n", pcszParam, GCPhysParam));
            *prcHv = GIM_HV_STATUS_INVALID_PARAMETER;
        }
    }
    else
    {
        Log(("GIM: HyperV: Misaligned %s param address %#RGp\n", pcszParam, GCPhysParam));
        *prcHv = GIM_HV_STATUS_INVALID_ALIGNMENT;
    }
    return rc;
}


/**
 * Helper for reading and validating slow hypercall input and output parameters.
 *
 * @returns VBox status code.
 * @param   pVM               The cross context VM structure.
 * @param   pCtx              Pointer to the guest-CPU context.
 * @param   fIs64BitMode      Whether the guest is currently in 64-bit mode or not.
 * @param   prcHv             Where to store the Hyper-V status code. Only valid
 *                            to the caller when this function returns
 *                            VINF_SUCCESS.
 */
static int gimHvReadSlowHypercallParamsInOut(PVM pVM, PCPUMCTX pCtx, bool fIs64BitMode, int *prcHv)
{
    int rc = gimHvReadSlowHypercallParam(pVM, pCtx, fIs64BitMode, GIMHVHYPERCALLPARAM_IN, prcHv);
    if (   RT_SUCCESS(rc)
        && *prcHv == GIM_HV_STATUS_SUCCESS)
        rc = gimHvReadSlowHypercallParam(pVM, pCtx, fIs64BitMode, GIMHVHYPERCALLPARAM_OUT, prcHv);
    return rc;
}
#endif


/**
 * Handles all Hyper-V hypercalls.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pCtx            Pointer to the guest-CPU context.
 *
 * @thread  EMT.
 */
VMM_INT_DECL(int) gimHvHypercall(PVMCPU pVCpu, PCPUMCTX pCtx)
{
#ifndef IN_RING3
    return VINF_GIM_R3_HYPERCALL;
#else
    PVM pVM = pVCpu->CTX_SUFF(pVM);

    /*
     * Verify that hypercalls are enabled.
     */
    if (!gimHvAreHypercallsEnabled(pVCpu))
        return VERR_GIM_HYPERCALLS_NOT_ENABLED;

    /*
     * Verify guest is in ring-0 protected mode.
     */
    uint32_t uCpl = CPUMGetGuestCPL(pVCpu);
    if (   uCpl
        || CPUMIsGuestInRealModeEx(pCtx))
    {
        return VERR_GIM_HYPERCALL_ACCESS_DENIED;
    }

    /*
     * Get the hypercall operation code and modes.
     */
    const bool       fIs64BitMode     = CPUMIsGuestIn64BitCodeEx(pCtx);
    const uint64_t   uHyperIn         = fIs64BitMode ? pCtx->rcx : (pCtx->rdx << 32) | pCtx->eax;
    const uint16_t   uHyperOp         = GIM_HV_HYPERCALL_IN_CALL_CODE(uHyperIn);
    const bool       fHyperFast       = GIM_HV_HYPERCALL_IN_IS_FAST(uHyperIn);
    const uint16_t   cHyperReps       = GIM_HV_HYPERCALL_IN_REP_COUNT(uHyperIn);
    const uint16_t   idxHyperRepStart = GIM_HV_HYPERCALL_IN_REP_START_IDX(uHyperIn);
    uint64_t         cHyperRepsDone   = 0;

    int rc     = VINF_SUCCESS;
    int rcHv   = GIM_HV_STATUS_OPERATION_DENIED;
    PGIMHV pHv = &pVM->gim.s.u.Hv;

    /*
     * Validate common hypercall input parameters.
     */
    if (   !GIM_HV_HYPERCALL_IN_RSVD_1(uHyperIn)
        && !GIM_HV_HYPERCALL_IN_RSVD_2(uHyperIn)
        && !GIM_HV_HYPERCALL_IN_RSVD_3(uHyperIn))
    {
        /*
         * Perform the hypercall.
         */
        switch (uHyperOp)
        {
            case GIM_HV_HYPERCALL_OP_RETREIVE_DEBUG_DATA:   /* Non-rep, memory IO. */
            {
                if (pHv->uPartFlags & GIM_HV_PART_FLAGS_DEBUGGING)
                {
                    rc  = gimHvReadSlowHypercallParamsInOut(pVM, pCtx, fIs64BitMode, &rcHv);
                    if (   RT_SUCCESS(rc)
                        && rcHv == GIM_HV_STATUS_SUCCESS)
                    {
                        LogRelMax(1, ("GIM: HyperV: Initiated debug data reception via hypercall\n"));
                        rc = gimR3HvHypercallRetrieveDebugData(pVM, &rcHv);
                        if (RT_FAILURE(rc))
                            LogRelMax(10, ("GIM: HyperV: gimR3HvHypercallRetrieveDebugData failed. rc=%Rrc\n", rc));
                    }
                }
                else
                    rcHv = GIM_HV_STATUS_ACCESS_DENIED;
                break;
            }

            case GIM_HV_HYPERCALL_OP_POST_DEBUG_DATA:   /* Non-rep, memory IO. */
            {
                if (pHv->uPartFlags & GIM_HV_PART_FLAGS_DEBUGGING)
                {
                    rc = gimHvReadSlowHypercallParamsInOut(pVM, pCtx, fIs64BitMode, &rcHv);
                    if (   RT_SUCCESS(rc)
                        && rcHv == GIM_HV_STATUS_SUCCESS)
                    {
                        LogRelMax(1, ("GIM: HyperV: Initiated debug data transmission via hypercall\n"));
                        rc = gimR3HvHypercallPostDebugData(pVM, &rcHv);
                        if (RT_FAILURE(rc))
                            LogRelMax(10, ("GIM: HyperV: gimR3HvHypercallPostDebugData failed. rc=%Rrc\n", rc));
                    }
                }
                else
                    rcHv = GIM_HV_STATUS_ACCESS_DENIED;
                break;
            }

            case GIM_HV_HYPERCALL_OP_RESET_DEBUG_SESSION:   /* Non-rep, fast (register IO). */
            {
                if (pHv->uPartFlags & GIM_HV_PART_FLAGS_DEBUGGING)
                {
                    uint32_t fFlags = 0;
                    if (!fHyperFast)
                    {
                        rc = gimHvReadSlowHypercallParam(pVM, pCtx, fIs64BitMode, GIMHVHYPERCALLPARAM_IN, &rcHv);
                        if (   RT_SUCCESS(rc)
                            && rcHv == GIM_HV_STATUS_SUCCESS)
                        {
                            PGIMHVDEBUGRESETIN pIn = (PGIMHVDEBUGRESETIN)pHv->pbHypercallIn;
                            fFlags = pIn->fFlags;
                        }
                    }
                    else
                    {
                        rcHv = GIM_HV_STATUS_SUCCESS;
                        fFlags = fIs64BitMode ? pCtx->rdx : pCtx->ebx;
                    }

                    /*
                     * Nothing to flush on the sending side as we don't maintain our own buffers.
                     */
                    /** @todo We should probably ask the debug receive thread to flush it's buffer. */
                    if (rcHv == GIM_HV_STATUS_SUCCESS)
                    {
                        if (fFlags)
                            LogRel(("GIM: HyperV: Resetting debug session via hypercall\n"));
                        else
                            rcHv = GIM_HV_STATUS_INVALID_PARAMETER;
                    }
                }
                else
                    rcHv = GIM_HV_STATUS_ACCESS_DENIED;
                break;
            }

            case GIM_HV_HYPERCALL_OP_POST_MESSAGE:      /* Non-rep, memory IO. */
            {
                if (pHv->fIsInterfaceVs)
                {
                    rc = gimHvReadSlowHypercallParam(pVM, pCtx, fIs64BitMode, GIMHVHYPERCALLPARAM_IN, &rcHv);
                    if (   RT_SUCCESS(rc)
                        && rcHv == GIM_HV_STATUS_SUCCESS)
                    {
                        PGIMHVPOSTMESSAGEIN pMsgIn = (PGIMHVPOSTMESSAGEIN)pHv->pbHypercallIn;
                        PGIMHVCPU pHvCpu = &pVCpu->gim.s.u.HvCpu;
                        if (    pMsgIn->uConnectionId  == GIM_HV_VMBUS_MSG_CONNECTION_ID
                            &&  pMsgIn->enmMessageType == GIMHVMSGTYPE_VMBUS
                            && !MSR_GIM_HV_SINT_IS_MASKED(pHvCpu->uSint2Msr)
                            &&  MSR_GIM_HV_SIMP_IS_ENABLED(pHvCpu->uSimpMsr))
                        {
                            RTGCPHYS GCPhysSimp = MSR_GIM_HV_SIMP_GPA(pHvCpu->uSimpMsr);
                            if (PGMPhysIsGCPhysNormal(pVM, GCPhysSimp))
                            {
                                /*
                                 * The VMBus client (guest) expects to see 0xf at offsets 4 and 16 and 1 at offset 0.
                                 */
                                GIMHVMSG HvMsg;
                                RT_ZERO(HvMsg);
                                HvMsg.MsgHdr.enmMessageType = GIMHVMSGTYPE_VMBUS;
                                HvMsg.MsgHdr.cbPayload = 0xf;
                                HvMsg.aPayload[0]      = 0xf;
                                uint16_t const offMsg = GIM_HV_VMBUS_MSG_SINT * sizeof(GIMHVMSG);
                                int rc2 = PGMPhysSimpleWriteGCPhys(pVM, GCPhysSimp + offMsg, &HvMsg, sizeof(HvMsg));
                                if (RT_SUCCESS(rc2))
                                    LogRel(("GIM: HyperV: SIMP hypercall faking message at %#RGp:%u\n", GCPhysSimp, offMsg));
                                else
                                {
                                    LogRel(("GIM: HyperV: Failed to write SIMP message at %#RGp:%u, rc=%Rrc\n", GCPhysSimp,
                                            offMsg, rc));
                                }
                            }
                        }

                        /*
                         * Make the call fail after updating the SIMP, so the guest can go back to using
                         * the Hyper-V debug MSR interface. Any error code below GIM_HV_STATUS_NOT_ACKNOWLEDGED
                         * and the guest tries to proceed with initializing VMBus which is totally unnecessary
                         * for what we're trying to accomplish, i.e. convince guest to use Hyper-V debugging. Also,
                         * we don't implement other VMBus/SynIC functionality so the guest would #GP and die.
                         */
                        rcHv = GIM_HV_STATUS_NOT_ACKNOWLEDGED;
                    }
                    else
                        rcHv = GIM_HV_STATUS_INVALID_PARAMETER;
                }
                else
                    rcHv = GIM_HV_STATUS_ACCESS_DENIED;
                break;
            }

            default:
                rcHv = GIM_HV_STATUS_INVALID_HYPERCALL_CODE;
                break;
        }
    }
    else
        rcHv = GIM_HV_STATUS_INVALID_HYPERCALL_INPUT;

    /*
     * Update the guest with results of the hypercall.
     */
    if (RT_SUCCESS(rc))
    {
        if (fIs64BitMode)
            pCtx->rax = (cHyperRepsDone << 32) | rcHv;
        else
        {
            pCtx->edx = cHyperRepsDone;
            pCtx->eax = rcHv;
        }
    }

    return rc;
#endif
}


/**
 * Returns whether the guest has configured and enabled the use of Hyper-V's
 * hypercall interface.
 *
 * @returns true if hypercalls are enabled, false otherwise.
 * @param   pVCpu       The cross context virtual CPU structure.
 */
VMM_INT_DECL(bool) gimHvAreHypercallsEnabled(PVMCPU pVCpu)
{
    return RT_BOOL(pVCpu->CTX_SUFF(pVM)->gim.s.u.Hv.u64GuestOsIdMsr != 0);
}


/**
 * Returns whether the guest has configured and enabled the use of Hyper-V's
 * paravirtualized TSC.
 *
 * @returns true if paravirt. TSC is enabled, false otherwise.
 * @param   pVM     The cross context VM structure.
 */
VMM_INT_DECL(bool) gimHvIsParavirtTscEnabled(PVM pVM)
{
    return MSR_GIM_HV_REF_TSC_IS_ENABLED(pVM->gim.s.u.Hv.u64TscPageMsr);
}


#ifdef IN_RING3
/**
 * Gets the descriptive OS ID variant as identified via the
 * MSR_GIM_HV_GUEST_OS_ID MSR.
 *
 * @returns The name.
 * @param   uGuestOsIdMsr     The MSR_GIM_HV_GUEST_OS_ID MSR.
 */
static const char *gimHvGetGuestOsIdVariantName(uint64_t uGuestOsIdMsr)
{
    /* Refer the Hyper-V spec, section 3.6 "Reporting the Guest OS Identity". */
    uint32_t uVendor = MSR_GIM_HV_GUEST_OS_ID_VENDOR(uGuestOsIdMsr);
    if (uVendor == 1 /* Microsoft */)
    {
        uint32_t uOsVariant = MSR_GIM_HV_GUEST_OS_ID_OS_VARIANT(uGuestOsIdMsr);
        switch (uOsVariant)
        {
            case 0:  return "Undefined";
            case 1:  return "MS-DOS";
            case 2:  return "Windows 3.x";
            case 3:  return "Windows 9x";
            case 4:  return "Windows NT or derivative";
            case 5:  return "Windows CE";
            default: return "Unknown";
        }
    }
    return "Unknown";
}
#endif


/**
 * MSR read handler for Hyper-V.
 *
 * @returns Strict VBox status code like CPUMQueryGuestMsr().
 * @retval  VINF_CPUM_R3_MSR_READ
 * @retval  VERR_CPUM_RAISE_GP_0
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   idMsr       The MSR being read.
 * @param   pRange      The range this MSR belongs to.
 * @param   puValue     Where to store the MSR value read.
 *
 * @thread  EMT.
 */
VMM_INT_DECL(VBOXSTRICTRC) gimHvReadMsr(PVMCPU pVCpu, uint32_t idMsr, PCCPUMMSRRANGE pRange, uint64_t *puValue)
{
    NOREF(pRange);
    PVM    pVM = pVCpu->CTX_SUFF(pVM);
    PGIMHV pHv = &pVM->gim.s.u.Hv;

    switch (idMsr)
    {
        case MSR_GIM_HV_TIME_REF_COUNT:
        {
            /* Hyper-V reports the time in 100 ns units (10 MHz). */
            uint64_t u64Tsc      = TMCpuTickGet(pVCpu);
            uint64_t u64TscHz    = pHv->cTscTicksPerSecond;
            uint64_t u64Tsc100Ns = u64TscHz / UINT64_C(10000000); /* 100 ns */
            *puValue = (u64Tsc / u64Tsc100Ns);
            return VINF_SUCCESS;
        }

        case MSR_GIM_HV_VP_INDEX:
            *puValue = pVCpu->idCpu;
            return VINF_SUCCESS;

        case MSR_GIM_HV_TPR:
            PDMApicReadMSR(pVM, pVCpu->idCpu, 0x80, puValue);
            return VINF_SUCCESS;

        case MSR_GIM_HV_EOI:
            PDMApicReadMSR(pVM, pVCpu->idCpu, 0x0B, puValue);
            return VINF_SUCCESS;

        case MSR_GIM_HV_ICR:
            PDMApicReadMSR(pVM, pVCpu->idCpu, 0x30, puValue);
            return VINF_SUCCESS;

        case MSR_GIM_HV_GUEST_OS_ID:
            *puValue = pHv->u64GuestOsIdMsr;
            return VINF_SUCCESS;

        case MSR_GIM_HV_HYPERCALL:
            *puValue = pHv->u64HypercallMsr;
            return VINF_SUCCESS;

        case MSR_GIM_HV_REF_TSC:
            *puValue = pHv->u64TscPageMsr;
            return VINF_SUCCESS;

        case MSR_GIM_HV_TSC_FREQ:
            *puValue = TMCpuTicksPerSecond(pVM);
            return VINF_SUCCESS;

        case MSR_GIM_HV_APIC_FREQ:
        {
            int rc = PDMApicGetTimerFreq(pVM, puValue);
            if (RT_FAILURE(rc))
                return VERR_CPUM_RAISE_GP_0;
            return VINF_SUCCESS;
        }

        case MSR_GIM_HV_SYNTH_DEBUG_STATUS:
            *puValue = pHv->uDbgStatusMsr;
            return VINF_SUCCESS;

        case MSR_GIM_HV_SINT2:
        {
            PGIMHVCPU pHvCpu = &pVCpu->gim.s.u.HvCpu;
            *puValue = pHvCpu->uSint2Msr;
            return VINF_SUCCESS;
        }

        case MSR_GIM_HV_SIMP:
        {
            PGIMHVCPU pHvCpu = &pVCpu->gim.s.u.HvCpu;
            *puValue = pHvCpu->uSimpMsr;
            return VINF_SUCCESS;
        }

        case MSR_GIM_HV_RESET:
            *puValue = 0;
            return VINF_SUCCESS;

        case MSR_GIM_HV_CRASH_CTL:
            *puValue = pHv->uCrashCtlMsr;
            return VINF_SUCCESS;

        case MSR_GIM_HV_CRASH_P0: *puValue = pHv->uCrashP0Msr;   return VINF_SUCCESS;
        case MSR_GIM_HV_CRASH_P1: *puValue = pHv->uCrashP1Msr;   return VINF_SUCCESS;
        case MSR_GIM_HV_CRASH_P2: *puValue = pHv->uCrashP2Msr;   return VINF_SUCCESS;
        case MSR_GIM_HV_CRASH_P3: *puValue = pHv->uCrashP3Msr;   return VINF_SUCCESS;
        case MSR_GIM_HV_CRASH_P4: *puValue = pHv->uCrashP4Msr;   return VINF_SUCCESS;

        case MSR_GIM_HV_DEBUG_OPTIONS_MSR:
        {
            if (pHv->fIsVendorMsHv)
            {
#ifndef IN_RING3
                return VINF_CPUM_R3_MSR_READ;
#else
                LogRelMax(1, ("GIM: HyperV: Guest querying debug options, suggesting %s interface\n",
                              pHv->fDbgHypercallInterface ? "hypercall" : "MSR"));
                *puValue = pHv->fDbgHypercallInterface ? GIM_HV_DEBUG_OPTIONS_USE_HYPERCALLS : 0;
                return VINF_SUCCESS;
#endif
            }
            return VERR_CPUM_RAISE_GP_0;
        }

        default:
        {
#ifdef IN_RING3
            static uint32_t s_cTimes = 0;
            if (s_cTimes++ < 20)
                LogRel(("GIM: HyperV: Unknown/invalid RdMsr (%#x) -> #GP(0)\n", idMsr));
#else
            return VINF_CPUM_R3_MSR_READ;
#endif
            LogFunc(("Unknown/invalid RdMsr (%#RX32) -> #GP(0)\n", idMsr));
            break;
        }
    }

    return VERR_CPUM_RAISE_GP_0;
}


/**
 * MSR write handler for Hyper-V.
 *
 * @returns Strict VBox status code like CPUMSetGuestMsr().
 * @retval  VINF_CPUM_R3_MSR_WRITE
 * @retval  VERR_CPUM_RAISE_GP_0
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   idMsr       The MSR being written.
 * @param   pRange      The range this MSR belongs to.
 * @param   uRawValue   The raw value with the ignored bits not masked.
 *
 * @thread  EMT.
 */
VMM_INT_DECL(VBOXSTRICTRC) gimHvWriteMsr(PVMCPU pVCpu, uint32_t idMsr, PCCPUMMSRRANGE pRange, uint64_t uRawValue)
{
    NOREF(pRange);
    PVM    pVM = pVCpu->CTX_SUFF(pVM);
    PGIMHV pHv = &pVM->gim.s.u.Hv;

    switch (idMsr)
    {
        case MSR_GIM_HV_TPR:
            PDMApicWriteMSR(pVM, pVCpu->idCpu, 0x80, uRawValue);
            return VINF_SUCCESS;

        case MSR_GIM_HV_EOI:
            PDMApicWriteMSR(pVM, pVCpu->idCpu, 0x0B, uRawValue);
            return VINF_SUCCESS;

        case MSR_GIM_HV_ICR:
            PDMApicWriteMSR(pVM, pVCpu->idCpu, 0x30, uRawValue);
            return VINF_SUCCESS;

        case MSR_GIM_HV_GUEST_OS_ID:
        {
#ifndef IN_RING3
            return VINF_CPUM_R3_MSR_WRITE;
#else
            /* Disable the hypercall-page and hypercalls if 0 is written to this MSR. */
            if (!uRawValue)
            {
                if (MSR_GIM_HV_HYPERCALL_PAGE_IS_ENABLED(pHv->u64HypercallMsr))
                {
                    gimR3HvDisableHypercallPage(pVM);
                    pHv->u64HypercallMsr &= ~MSR_GIM_HV_HYPERCALL_PAGE_ENABLE_BIT;
                    LogRel(("GIM: HyperV: Hypercall page disabled via Guest OS ID MSR\n"));
                }
            }
            else
            {
                LogRel(("GIM: HyperV: Guest OS reported ID %#RX64\n", uRawValue));
                LogRel(("GIM: HyperV: Open-source=%RTbool Vendor=%#x OS=%#x (%s) Major=%u Minor=%u ServicePack=%u Build=%u\n",
                        MSR_GIM_HV_GUEST_OS_ID_IS_OPENSOURCE(uRawValue),   MSR_GIM_HV_GUEST_OS_ID_VENDOR(uRawValue),
                        MSR_GIM_HV_GUEST_OS_ID_OS_VARIANT(uRawValue),      gimHvGetGuestOsIdVariantName(uRawValue),
                        MSR_GIM_HV_GUEST_OS_ID_MAJOR_VERSION(uRawValue),   MSR_GIM_HV_GUEST_OS_ID_MINOR_VERSION(uRawValue),
                        MSR_GIM_HV_GUEST_OS_ID_SERVICE_VERSION(uRawValue), MSR_GIM_HV_GUEST_OS_ID_BUILD(uRawValue)));

                /* Update the CPUID leaf, see Hyper-V spec. "Microsoft Hypervisor CPUID Leaves". */
                CPUMCPUIDLEAF HyperLeaf;
                RT_ZERO(HyperLeaf);
                HyperLeaf.uLeaf = UINT32_C(0x40000002);
                HyperLeaf.uEax  = MSR_GIM_HV_GUEST_OS_ID_BUILD(uRawValue);
                HyperLeaf.uEbx  =  MSR_GIM_HV_GUEST_OS_ID_MINOR_VERSION(uRawValue)
                                | (MSR_GIM_HV_GUEST_OS_ID_MAJOR_VERSION(uRawValue) << 16);
                HyperLeaf.uEcx  = MSR_GIM_HV_GUEST_OS_ID_SERVICE_VERSION(uRawValue);
                HyperLeaf.uEdx  =  MSR_GIM_HV_GUEST_OS_ID_SERVICE_VERSION(uRawValue)
                                | (MSR_GIM_HV_GUEST_OS_ID_BUILD(uRawValue) << 24);
                int rc2 = CPUMR3CpuIdInsert(pVM, &HyperLeaf);
                AssertRC(rc2);
            }

            pHv->u64GuestOsIdMsr = uRawValue;

            /*
             * Notify VMM that hypercalls are now disabled/enabled.
             */
            for (VMCPUID i = 0; i < pVM->cCpus; i++)
            {
                if (uRawValue)
                    VMMHypercallsEnable(&pVM->aCpus[i]);
                else
                    VMMHypercallsDisable(&pVM->aCpus[i]);
            }

            return VINF_SUCCESS;
#endif /* IN_RING3 */
        }

        case MSR_GIM_HV_HYPERCALL:
        {
#ifndef IN_RING3
            return VINF_CPUM_R3_MSR_WRITE;
#else
            /** @todo There is/was a problem with hypercalls for FreeBSD 10.1 guests,
             *  see @bugref{7270#c116}. */
            /* First, update all but the hypercall page enable bit. */
            pHv->u64HypercallMsr = (uRawValue & ~MSR_GIM_HV_HYPERCALL_PAGE_ENABLE_BIT);

            /* Hypercall page can only be enabled when the guest has enabled hypercalls. */
            bool fEnable = RT_BOOL(uRawValue & MSR_GIM_HV_HYPERCALL_PAGE_ENABLE_BIT);
            if (   fEnable
                && !gimHvAreHypercallsEnabled(pVCpu))
            {
                return VINF_SUCCESS;
            }

            /* Is the guest disabling the hypercall-page? Allow it regardless of the Guest-OS Id Msr. */
            if (!fEnable)
            {
                gimR3HvDisableHypercallPage(pVM);
                pHv->u64HypercallMsr = uRawValue;
                return VINF_SUCCESS;
            }

            /* Enable the hypercall-page. */
            RTGCPHYS GCPhysHypercallPage = MSR_GIM_HV_HYPERCALL_GUEST_PFN(uRawValue) << PAGE_SHIFT;
            int rc = gimR3HvEnableHypercallPage(pVM, GCPhysHypercallPage);
            if (RT_SUCCESS(rc))
            {
                pHv->u64HypercallMsr = uRawValue;
                return VINF_SUCCESS;
            }

            return VERR_CPUM_RAISE_GP_0;
#endif
        }

        case MSR_GIM_HV_REF_TSC:
        {
#ifndef IN_RING3
            return VINF_CPUM_R3_MSR_WRITE;
#else  /* IN_RING3 */
            /* First, update all but the TSC-page enable bit. */
            pHv->u64TscPageMsr = (uRawValue & ~MSR_GIM_HV_REF_TSC_ENABLE_BIT);

            /* Is the guest disabling the TSC-page? */
            bool fEnable = RT_BOOL(uRawValue & MSR_GIM_HV_REF_TSC_ENABLE_BIT);
            if (!fEnable)
            {
                gimR3HvDisableTscPage(pVM);
                pHv->u64TscPageMsr = uRawValue;
                return VINF_SUCCESS;
            }

            /* Enable the TSC-page. */
            RTGCPHYS GCPhysTscPage = MSR_GIM_HV_REF_TSC_GUEST_PFN(uRawValue) << PAGE_SHIFT;
            int rc = gimR3HvEnableTscPage(pVM, GCPhysTscPage, false /* fUseThisTscSequence */, 0 /* uTscSequence */);
            if (RT_SUCCESS(rc))
            {
                pHv->u64TscPageMsr = uRawValue;
                return VINF_SUCCESS;
            }

            return VERR_CPUM_RAISE_GP_0;
#endif /* IN_RING3 */
        }

        case MSR_GIM_HV_RESET:
        {
#ifndef IN_RING3
            return VINF_CPUM_R3_MSR_WRITE;
#else
            if (MSR_GIM_HV_RESET_IS_SET(uRawValue))
            {
                LogRel(("GIM: HyperV: Reset initiated through MSR\n"));
                int rc = PDMDevHlpVMReset(pVM->gim.s.pDevInsR3);
                AssertRC(rc);
            }
            /* else: Ignore writes to other bits. */
            return VINF_SUCCESS;
#endif /* IN_RING3 */
        }

        case MSR_GIM_HV_CRASH_CTL:
        {
#ifndef IN_RING3
            return VINF_CPUM_R3_MSR_WRITE;
#else
            if (uRawValue & MSR_GIM_HV_CRASH_CTL_NOTIFY_BIT)
            {
                LogRel(("GIM: HyperV: Guest indicates a fatal condition! P0=%#RX64 P1=%#RX64 P2=%#RX64 P3=%#RX64 P4=%#RX64\n",
                        pHv->uCrashP0Msr, pHv->uCrashP1Msr, pHv->uCrashP2Msr, pHv->uCrashP3Msr, pHv->uCrashP4Msr));
            }
            return VINF_SUCCESS;
#endif
        }

        case MSR_GIM_HV_SYNTH_DEBUG_SEND_BUFFER:
        {
            if (!pHv->fDbgEnabled)
                return VERR_CPUM_RAISE_GP_0;
#ifndef IN_RING3
            return VINF_CPUM_R3_MSR_WRITE;
#else
            RTGCPHYS GCPhysBuffer    = (RTGCPHYS)uRawValue;
            pHv->uDbgSendBufferMsr = GCPhysBuffer;
            if (PGMPhysIsGCPhysNormal(pVM, GCPhysBuffer))
                LogRel(("GIM: HyperV: Set up debug send buffer at %#RGp\n", GCPhysBuffer));
            else
                LogRel(("GIM: HyperV: Destroyed debug send buffer\n"));
            pHv->uDbgSendBufferMsr = uRawValue;
            return VINF_SUCCESS;
#endif
        }

        case MSR_GIM_HV_SYNTH_DEBUG_RECEIVE_BUFFER:
        {
            if (!pHv->fDbgEnabled)
                return VERR_CPUM_RAISE_GP_0;
#ifndef IN_RING3
            return VINF_CPUM_R3_MSR_WRITE;
#else
            RTGCPHYS GCPhysBuffer  = (RTGCPHYS)uRawValue;
            pHv->uDbgRecvBufferMsr = GCPhysBuffer;
            if (PGMPhysIsGCPhysNormal(pVM, GCPhysBuffer))
                LogRel(("GIM: HyperV: Set up debug receive buffer at %#RGp\n", GCPhysBuffer));
            else
                LogRel(("GIM: HyperV: Destroyed debug receive buffer\n"));
            return VINF_SUCCESS;
#endif
        }

        case MSR_GIM_HV_SYNTH_DEBUG_PENDING_BUFFER:
        {
            if (!pHv->fDbgEnabled)
                return VERR_CPUM_RAISE_GP_0;
#ifndef IN_RING3
            return VINF_CPUM_R3_MSR_WRITE;
#else
            RTGCPHYS GCPhysBuffer      = (RTGCPHYS)uRawValue;
            pHv->uDbgPendingBufferMsr  = GCPhysBuffer;
            if (PGMPhysIsGCPhysNormal(pVM, GCPhysBuffer))
                LogRel(("GIM: HyperV: Set up debug pending buffer at %#RGp\n", uRawValue));
            else
                LogRel(("GIM: HyperV: Destroyed debug pending buffer\n"));
            return VINF_SUCCESS;
#endif
        }

        case MSR_GIM_HV_SYNTH_DEBUG_CONTROL:
        {
            if (!pHv->fDbgEnabled)
                return VERR_CPUM_RAISE_GP_0;
#ifndef IN_RING3
            return VINF_CPUM_R3_MSR_WRITE;
#else
            if (   MSR_GIM_HV_SYNTH_DEBUG_CONTROL_IS_WRITE(uRawValue)
                && MSR_GIM_HV_SYNTH_DEBUG_CONTROL_IS_READ(uRawValue))
            {
                LogRel(("GIM: HyperV: Requesting both read and write through debug control MSR -> #GP(0)\n"));
                return VERR_CPUM_RAISE_GP_0;
            }

            if (MSR_GIM_HV_SYNTH_DEBUG_CONTROL_IS_WRITE(uRawValue))
            {
                uint32_t cbWrite = MSR_GIM_HV_SYNTH_DEBUG_CONTROL_W_LEN(uRawValue);
                if (   cbWrite > 0
                    && cbWrite < GIM_HV_PAGE_SIZE)
                {
                    if (PGMPhysIsGCPhysNormal(pVM, (RTGCPHYS)pHv->uDbgSendBufferMsr))
                    {
                        Assert(pHv->pvDbgBuffer);
                        int rc = PGMPhysSimpleReadGCPhys(pVM, pHv->pvDbgBuffer, (RTGCPHYS)pHv->uDbgSendBufferMsr, cbWrite);
                        if (RT_SUCCESS(rc))
                        {
                            LogRelMax(1, ("GIM: HyperV: Initiated debug data transmission via MSR\n"));
                            uint32_t cbWritten = 0;
                            rc = gimR3HvDebugWrite(pVM, pHv->pvDbgBuffer, cbWrite, &cbWritten, false /*fUdpPkt*/);
                            if (   RT_SUCCESS(rc)
                                && cbWrite == cbWritten)
                                pHv->uDbgStatusMsr = MSR_GIM_HV_SYNTH_DEBUG_STATUS_W_SUCCESS_BIT;
                            else
                                pHv->uDbgStatusMsr = 0;
                        }
                        else
                            LogRelMax(5, ("GIM: HyperV: Failed to read debug send buffer at %#RGp, rc=%Rrc\n",
                                          (RTGCPHYS)pHv->uDbgSendBufferMsr, rc));
                    }
                    else
                        LogRelMax(5, ("GIM: HyperV: Debug send buffer address %#RGp invalid! Ignoring debug write!\n",
                                      (RTGCPHYS)pHv->uDbgSendBufferMsr));
                }
                else
                    LogRelMax(5, ("GIM: HyperV: Invalid write size %u specified in MSR, ignoring debug write!\n",
                                  MSR_GIM_HV_SYNTH_DEBUG_CONTROL_W_LEN(uRawValue)));
            }
            else if (MSR_GIM_HV_SYNTH_DEBUG_CONTROL_IS_READ(uRawValue))
            {
                if (PGMPhysIsGCPhysNormal(pVM, (RTGCPHYS)pHv->uDbgRecvBufferMsr))
                {
                    LogRelMax(1, ("GIM: HyperV: Initiated debug data reception via MSR\n"));
                    uint32_t cbReallyRead;
                    Assert(pHv->pvDbgBuffer);
                    int rc = gimR3HvDebugRead(pVM, pHv->pvDbgBuffer, PAGE_SIZE, PAGE_SIZE, &cbReallyRead, 0, false /*fUdpPkt*/);
                    if (   RT_SUCCESS(rc)
                        && cbReallyRead > 0)
                    {
                        rc = PGMPhysSimpleWriteGCPhys(pVM, (RTGCPHYS)pHv->uDbgRecvBufferMsr, pHv->pvDbgBuffer, cbReallyRead);
                        if (RT_SUCCESS(rc))
                        {
                            pHv->uDbgStatusMsr  = ((uint16_t)cbReallyRead) << 16;
                            pHv->uDbgStatusMsr |= MSR_GIM_HV_SYNTH_DEBUG_STATUS_R_SUCCESS_BIT;
                        }
                        else
                        {
                            pHv->uDbgStatusMsr = 0;
                            LogRelMax(5, ("GIM: HyperV: PGMPhysSimpleWriteGCPhys failed. rc=%Rrc\n", rc));
                        }
                    }
                    else
                        pHv->uDbgStatusMsr = 0;
                }
                else
                    LogRelMax(5, ("GIM: HyperV: Debug receive buffer address %#RGp invalid! Ignoring debug read!\n"));
            }
            return VINF_SUCCESS;
#endif
        }

        case MSR_GIM_HV_SINT2:
        {
            if (!pHv->fDbgEnabled)
                return VERR_CPUM_RAISE_GP_0;
#ifndef IN_RING3
            return VINF_CPUM_R3_MSR_WRITE;
#else
            PGIMHVCPU pHvCpu = &pVCpu->gim.s.u.HvCpu;
            uint8_t uVector = MSR_GIM_HV_SINT_VECTOR(uRawValue);
            if (  !MSR_GIM_HV_SINT_IS_MASKED(uRawValue)
                && uVector < 16)
            {
                LogRel(("GIM: HyperV: Programmed an invalid vector in SINT2, uVector=%u -> #GP(0)\n", uVector));
                return VERR_CPUM_RAISE_GP_0;
            }

            pHvCpu->uSint2Msr = uRawValue;
            if (MSR_GIM_HV_SINT_IS_MASKED(uRawValue))
                LogRel(("GIM: HyperV: Masked SINT2\n"));
            else
                LogRel(("GIM: HyperV: Unmasked SINT2, uVector=%u\n", uVector));
            return VINF_SUCCESS;
#endif
        }

        case MSR_GIM_HV_SIMP:
        {
            if (!pHv->fDbgEnabled)
                return VERR_CPUM_RAISE_GP_0;
#ifndef IN_RING3
            return VINF_CPUM_R3_MSR_WRITE;
#else
            PGIMHVCPU pHvCpu = &pVCpu->gim.s.u.HvCpu;
            pHvCpu->uSimpMsr = uRawValue;
            if (MSR_GIM_HV_SIMP_IS_ENABLED(uRawValue))
            {
                RTGCPHYS GCPhysSimp = MSR_GIM_HV_SIMP_GPA(uRawValue);
                if (PGMPhysIsGCPhysNormal(pVM, GCPhysSimp))
                {
                    uint8_t abSimp[PAGE_SIZE];
                    RT_ZERO(abSimp);
                    int rc2 = PGMPhysSimpleWriteGCPhys(pVM, GCPhysSimp, &abSimp[0], sizeof(abSimp));
                    if (RT_SUCCESS(rc2))
                        LogRel(("GIM: HyperV: Enabled synthetic interrupt message page at %#RGp\n", GCPhysSimp));
                    else
                    {
                        LogRel(("GIM: HyperV: WrMsr on MSR_GIM_HV_SIMP failed to update SIMP at %#RGp rc=%Rrc -> #GP(0)\n",
                                GCPhysSimp, rc2));
                        return VERR_CPUM_RAISE_GP_0;
                    }
                }
                else
                    LogRel(("GIM: HyperV: Enabled synthetic interrupt message page at invalid address %#RGp\n",GCPhysSimp));
            }
            else
                LogRel(("GIM: HyperV: Disabled synthetic interrupt message page\n"));
            return VINF_SUCCESS;
#endif
        }

        case MSR_GIM_HV_CRASH_P0:  pHv->uCrashP0Msr = uRawValue;  return VINF_SUCCESS;
        case MSR_GIM_HV_CRASH_P1:  pHv->uCrashP1Msr = uRawValue;  return VINF_SUCCESS;
        case MSR_GIM_HV_CRASH_P2:  pHv->uCrashP2Msr = uRawValue;  return VINF_SUCCESS;
        case MSR_GIM_HV_CRASH_P3:  pHv->uCrashP3Msr = uRawValue;  return VINF_SUCCESS;
        case MSR_GIM_HV_CRASH_P4:  pHv->uCrashP4Msr = uRawValue;  return VINF_SUCCESS;

        case MSR_GIM_HV_TIME_REF_COUNT:     /* Read-only MSRs. */
        case MSR_GIM_HV_VP_INDEX:
        case MSR_GIM_HV_TSC_FREQ:
        case MSR_GIM_HV_APIC_FREQ:
            LogFunc(("WrMsr on read-only MSR %#RX32 -> #GP(0)\n", idMsr));
            return VERR_CPUM_RAISE_GP_0;

        case MSR_GIM_HV_DEBUG_OPTIONS_MSR:
        {
            if (pHv->fIsVendorMsHv)
            {
#ifndef IN_RING3
                return VINF_CPUM_R3_MSR_WRITE;
#else
                LogRelMax(5, ("GIM: HyperV: Write debug options MSR with %#RX64 ignored\n", uRawValue));
                return VINF_SUCCESS;
#endif
            }
            return VERR_CPUM_RAISE_GP_0;
        }

        default:
        {
#ifdef IN_RING3
            static uint32_t s_cTimes = 0;
            if (s_cTimes++ < 20)
                LogRel(("GIM: HyperV: Unknown/invalid WrMsr (%#x,%#x`%08x) -> #GP(0)\n", idMsr,
                        uRawValue & UINT64_C(0xffffffff00000000), uRawValue & UINT64_C(0xffffffff)));
#else
            return VINF_CPUM_R3_MSR_WRITE;
#endif
            LogFunc(("Unknown/invalid WrMsr (%#RX32,%#RX64) -> #GP(0)\n", idMsr, uRawValue));
            break;
        }
    }

    return VERR_CPUM_RAISE_GP_0;
}


/**
 * Whether we need to trap \#UD exceptions in the guest.
 *
 * We only need to trap \#UD exceptions for raw-mode guests when hypercalls are
 * enabled. For HM VMs, the hypercall would be handled via the
 * VMCALL/VMMCALL VM-exit.
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 */
VMM_INT_DECL(bool) gimHvShouldTrapXcptUD(PVMCPU pVCpu)
{
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    if (   !HMIsEnabled(pVM)
        && gimHvAreHypercallsEnabled(pVCpu))
        return true;
    return false;
}


/**
 * Exception handler for \#UD.
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pCtx        Pointer to the guest-CPU context.
 * @param   pDis        Pointer to the disassembled instruction state at RIP.
 *                      Optional, can be NULL.
 *
 * @thread  EMT.
 */
VMM_INT_DECL(int) gimHvXcptUD(PVMCPU pVCpu, PCPUMCTX pCtx, PDISCPUSTATE pDis)
{
    /*
     * If we didn't ask for #UD to be trapped, bail.
     */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    if (!gimHvShouldTrapXcptUD(pVCpu))
        return VERR_GIM_OPERATION_FAILED;

    int rc = VINF_SUCCESS;
    if (!pDis)
    {
        /*
         * Disassemble the instruction at RIP to figure out if it's the Intel VMCALL instruction
         * or the AMD VMMCALL instruction and if so, handle it as a hypercall.
         */
        DISCPUSTATE Dis;
        rc = EMInterpretDisasCurrent(pVM, pVCpu, &Dis, NULL /* pcbInstr */);
        pDis = &Dis;
    }

    if (RT_SUCCESS(rc))
    {
        CPUMCPUVENDOR enmGuestCpuVendor = CPUMGetGuestCpuVendor(pVM);
        if (   (   pDis->pCurInstr->uOpcode == OP_VMCALL
                && (   enmGuestCpuVendor == CPUMCPUVENDOR_INTEL
                    || enmGuestCpuVendor == CPUMCPUVENDOR_VIA))
            || (   pDis->pCurInstr->uOpcode == OP_VMMCALL
                && enmGuestCpuVendor == CPUMCPUVENDOR_AMD))
        {
            /*
             * Make sure guest ring-0 is the one making the hypercall.
             */
            if (CPUMGetGuestCPL(pVCpu))
                return VERR_GIM_HYPERCALL_ACCESS_DENIED;

            /*
             * Update RIP and perform the hypercall.
             */
            /** @todo pre-incrementing of RIP will break when we implement continuing hypercalls. */
            pCtx->rip += pDis->cbInstr;
            rc = gimHvHypercall(pVCpu, pCtx);
        }
        else
            rc = VERR_GIM_OPERATION_FAILED;
    }
    return rc;
}

