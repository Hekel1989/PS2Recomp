#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>

#include "runtime/ps2_vu1_inventory.h"

// PS2X(vu1-aot-v2): build-time specialised pair step.
//
// The generator instantiates aotStepConstant<kPc, kPair>() once per microcode
// pair.  Every field of the decoded pair, the pc and the branch displacement
// are compile-time constants, so the compiler folds the generic step routine's
// bookkeeping (reserved checks, usage loops, writeback tests) away.  The
// execution semantics are copied verbatim from VU1Interpreter::aotStepPair();
// only the constants are different.
//
// Included at the end of ps2_vu1.h; the definitions are member templates and
// therefore have access to the interpreter's private scheduler state.

template <uint32_t kPc, const ps2x::vu1::DecodedPair &D>
inline uint64_t VU1Interpreter::pairReadyCycleConstant() const
{
    // Plain compare/select instead of std::max: clang vectorises a 4-lane
    // maximum of uint64_t into a long pcmpgtd/pshufd/blendvpd sequence, which
    // costs more than the four scalar cmovs it replaces.
    uint64_t ready = m_cycle;
    const auto &usage = D.upperUsage;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        const VfAccess &access = usage.vfRead[index];
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((access.lanes & lane(component)) != 0u)
            {
                const uint64_t value = m_vfReady[access.reg][component];
                if (value > ready)
                    ready = value;
            }
        }
    }
    uint32_t viRead = usage.viRead;
    while (viRead != 0u)
    {
        const uint32_t reg = static_cast<uint32_t>(std::countr_zero(viRead));
        const uint64_t value = m_viReady[reg];
        if (value > ready)
            ready = value;
        viRead &= viRead - 1u;
    }
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((usage.accRead & lane(component)) != 0u)
        {
            const uint64_t value = m_accReady[component];
            if (value > ready)
                ready = value;
        }
    }

    const auto &lower = D.lowerUsage;
    for (uint32_t index = 0; index < lower.vfReadCount; ++index)
    {
        const VfAccess &access = lower.vfRead[index];
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((access.lanes & lane(component)) != 0u)
            {
                const uint64_t value = m_vfReady[access.reg][component];
                if (value > ready)
                    ready = value;
            }
        }
    }
    uint32_t lowerViRead = lower.viRead;
    while (lowerViRead != 0u)
    {
        const uint32_t reg = static_cast<uint32_t>(std::countr_zero(lowerViRead));
        const uint64_t value = m_viReady[reg];
        if (value > ready)
            ready = value;
        lowerViRead &= lowerViRead - 1u;
    }
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((lower.accRead & lane(component)) != 0u)
        {
            const uint64_t value = m_accReady[component];
            if (value > ready)
                ready = value;
        }
    }
    if (lower.pipeline == PipelineFdiv && m_fdiv.valid && m_fdiv.readyCycle > ready)
        ready = m_fdiv.readyCycle;
    if (lower.pipeline == PipelineEfu && m_efuResourceReady > ready)
        ready = m_efuResourceReady;
    if (lower.waitQ && m_fdiv.valid && m_fdiv.readyCycle > ready)
        ready = m_fdiv.readyCycle;
    if (lower.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid && entry.readyCycle > ready)
                ready = entry.readyCycle;
    }
    if (lower.pipeline == PipelineXgkick && m_xgkick.active && m_cycle + 1u > ready)
        ready = m_cycle + 1u;
    return ready;
}

template <const ps2x::vu1::DecodedPair &D>
inline void VU1Interpreter::markPairWritesConstant()
{
    const VfAccess lowerWrite = D.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u && D.suppressedLowerVf != lowerWrite.reg)
    {
        const uint32_t latency = D.lowerUsage.vfLatency != 0u
                                     ? D.lowerUsage.vfLatency
                                     : D.lowerUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((lowerWrite.lanes & lane(component)) != 0u)
                m_vfReady[lowerWrite.reg][component] = m_cycle + latency;
        }
    }

    const VfAccess upperWrite = D.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        const uint32_t latency = D.upperUsage.vfLatency != 0u
                                     ? D.upperUsage.vfLatency
                                     : D.upperUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((upperWrite.lanes & lane(component)) != 0u)
                m_vfReady[upperWrite.reg][component] = m_cycle + latency;
        }
    }

    uint32_t viWrite = D.lowerUsage.viWrite;
    while (viWrite != 0u)
    {
        const uint32_t reg = static_cast<uint32_t>(std::countr_zero(viWrite));
        m_viReady[reg] = m_cycle + (D.lowerUsage.viLatency != 0u
                                        ? D.lowerUsage.viLatency
                                        : D.lowerUsage.latency);
        viWrite &= viWrite - 1u;
    }
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((D.upperUsage.accWrite & lane(component)) != 0u)
            m_accReady[component] = m_cycle + kAccForwardLatency;
    }
}

template <uint32_t kPc, const ps2x::vu1::DecodedPair &D>
PS2X_VU1_INLINE ps2x::vu1::AotResult VU1Interpreter::aotStepConstant()
{
    if (m_stopRequested)
        return ps2x::vu1::AotResult::StopRequested;
    if (m_cycle >= m_budgetEnd)
        return ps2x::vu1::AotResult::BudgetEnded;
    // No commit here: advanceOneCycle() at the end of the previous pair (or
    // advanceTo() during a stall) already committed everything that became
    // visible at this cycle.  aotStepPair() keeps its own check for the
    // interpreter's shared entry point.

    if constexpr (D.upperUsage.reserved || D.lowerUsage.reserved)
    {
        m_stopReason = "reserved";
        reportReservedInstruction(D.upperUsage.reserved,
                                  D.upperUsage.reserved ? D.upper : D.lower);
        return ps2x::vu1::AotResult::StopRequested;
    }
    else
    {
        const uint64_t readyCycle = pairReadyCycleConstant<kPc, D>();
        if (readyCycle > m_cycle)
        {
            if (m_inventory != nullptr)
                m_inventory->onStall(readyCycle - m_cycle);
            if (readyCycle >= m_budgetEnd)
                advanceTo(m_budgetEnd);
            else
                advanceTo(readyCycle);
        }
        if (m_cycle >= m_budgetEnd)
            return ps2x::vu1::AotResult::BudgetEnded;

        uint8_t writtenVi = 0u;
        int32_t oldVi = 0;
        for (uint32_t reg = 1; reg < 16u; ++reg)
        {
            if ((D.lowerUsage.viWrite & (1u << reg)) != 0u)
            {
                writtenVi = static_cast<uint8_t>(reg);
                oldVi = m_state.vi[reg];
                break;
            }
        }

        const VfAccess upperWrite = D.upperUsage.vfWrite;
        const VfAccess lowerWrite = D.lowerUsage.vfWrite;
        const bool hasUpperWrite = upperWrite.reg != 0u;
        const bool hasLowerWrite = lowerWrite.reg != 0u && D.suppressedLowerVf != lowerWrite.reg;
        const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);
        float oldUpperVf[4]{};
        float newUpperVf[4]{};
        float oldLowerVf[4]{};
        float newLowerVf[4]{};
        float oldAcc[4]{};
        float newAcc[4]{};
        if (hasUpperWrite)
            std::memcpy(oldUpperVf, m_state.vf[upperWrite.reg], sizeof(oldUpperVf));
        if (hasDistinctLowerWrite)
            std::memcpy(oldLowerVf, m_state.vf[lowerWrite.reg], sizeof(oldLowerVf));
        if (D.upperUsage.accWrite != 0u)
            std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

        if constexpr (D.iBit)
        {
            execUpperConstant<D.upper>();
            float immediate = 0.0f;
            std::memcpy(&immediate, &D.lower, sizeof(immediate));
            m_state.i = normalizeOperand(immediate);
        }
        else if constexpr (D.upperVfShadowReg != 0u)
        {
            float oldVf[4]{};
            float upperVf[4]{};
            std::memcpy(oldVf, m_state.vf[D.upperVfShadowReg], sizeof(oldVf));
            execUpperConstant<D.upper>();
            std::memcpy(upperVf, m_state.vf[D.upperVfShadowReg], sizeof(upperVf));
            std::memcpy(m_state.vf[D.upperVfShadowReg], oldVf, sizeof(oldVf));
            execLowerConstant<kPc, D.lower>(m_activeVuData, m_activeVuDataSize, *m_activeGs, m_activeMemory, D.upper);
            std::memcpy(m_state.vf[D.upperVfShadowReg], upperVf, sizeof(upperVf));
        }
        else
        {
            execUpperConstant<D.upper>();
            execLowerConstant<kPc, D.lower>(m_activeVuData, m_activeVuDataSize, *m_activeGs, m_activeMemory, D.upper);
        }

        if (m_inventory != nullptr)
        {
            m_inventory->onPair(kPc, D.lower, D.upper,
                                D.iBit, D.eBit, D.dBit,
                                D.tBit, D.mBit, m_state.branchPending,
                                m_state.branchTarget);
        }

        m_viBranchBackupValid = false;

        if (hasUpperWrite)
        {
            std::memcpy(newUpperVf, m_state.vf[upperWrite.reg], sizeof(newUpperVf));
            std::memcpy(m_state.vf[upperWrite.reg], oldUpperVf, sizeof(oldUpperVf));
            const uint32_t latency =
                D.upperUsage.vfLatency != 0u
                    ? D.upperUsage.vfLatency
                    : D.upperUsage.latency;
            queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
        }
        if (hasDistinctLowerWrite)
        {
            std::memcpy(newLowerVf, m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
            std::memcpy(m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(oldLowerVf));
            const uint32_t latency = D.lowerUsage.vfLatency != 0u
                                         ? D.lowerUsage.vfLatency
                                         : D.lowerUsage.latency;
            queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
        }
        if (D.upperUsage.accWrite != 0u)
        {
            std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
            std::memcpy(m_state.acc, oldAcc, sizeof(oldAcc));
            queueAccWrite(D.upperUsage.accWrite, newAcc, kAccForwardLatency);
        }
        if (writtenVi != 0u)
        {
            const int32_t newVi = m_state.vi[writtenVi];
            m_state.vi[writtenVi] = oldVi;
            const uint32_t latency =
                D.lowerUsage.viLatency != 0u
                    ? D.lowerUsage.viLatency
                    : D.lowerUsage.latency;
            queueViWrite(writtenVi, newVi, latency);
        }

        markPairWritesConstant<D>();
        if (writtenVi != 0u && D.lowerUsage.delaysNextBranchRead)
            recordViWriteForBranch(writtenVi, oldVi);

        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        m_state.vi[0] = 0;

        uint32_t nextPc = kPc + 8u;
        if (nextPc >= m_activeCodeSize)
            nextPc = 0u;
        m_state.pc = nextPc;

        if (m_state.branchPending)
        {
            if (m_state.branchDelay == 0u)
            {
                m_state.pc = m_state.branchTarget & microAddressMask();
                m_state.branchPending = false;
            }
            else
            {
                --m_state.branchDelay;
            }
        }

        const bool dHalt = D.dBit && m_state.dBitEnabled;
        const bool tHalt = D.tBit && m_state.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && D.lowerUsage.pipeline == PipelineBranch;

        bool programEnded = false;
        if (m_state.haltAfterDelaySlot)
        {
            m_state.stoppedByD = m_pendingHaltD;
            m_state.stoppedByT = m_pendingHaltT;
            programEnded = true;
        }
        else if (m_state.ebit)
        {
            programEnded = true;
        }
        else if (haltBit && !haltBranch)
        {
            m_state.stoppedByD = dHalt;
            m_state.stoppedByT = tHalt;
            programEnded = true;
        }
        else if (D.eBit)
        {
            m_state.ebit = true;
        }
        else if (haltBranch)
        {
            m_state.haltAfterDelaySlot = true;
            m_pendingHaltD = dHalt;
            m_pendingHaltT = tHalt;
        }

        advanceOneCycle();
        ++m_traceInsns;
        if (programEnded)
        {
            m_stopReason = m_state.stoppedByD ? "d-bit" : m_state.stoppedByT ? "t-bit" : "e-bit";
            return ps2x::vu1::AotResult::ProgramEnded;
        }
        return ps2x::vu1::AotResult::Continue;
    }
}
