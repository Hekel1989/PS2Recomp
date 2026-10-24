#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_inventory.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <limits>
#include <mutex>
#include <set>
#include <ps2_log.h>

namespace
{
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }

    // --- PS2X(vu1-trace): PS2X_VU1_TRACE=1 -----------------------------------
    // Log every VU1 microprogram run (start PC, executed instruction count,
    // stop reason) and every XGKICK (source address, packet size, first GIFtag).
    bool vu1TraceEnabled()
    {
        static const bool s_enabled = std::getenv("PS2X_VU1_TRACE") != nullptr;
        return s_enabled;
    }

    bool vu1DiffEnabled()
    {
        static const bool s_enabled = std::getenv("PS2X_VU1_DIFF") != nullptr;
        return s_enabled;
    }

    // Atomic/locked: the VU1 thread and its verification shadow (game thread)
    // can both run VU1 programs (runtime/ps2_vu1_thread.h).
    std::atomic<uint64_t> s_diffRuns{0};
    std::atomic<uint64_t> s_diffMismatches{0};
    std::once_flag s_diffRegistered;
    std::atomic<bool> s_diffPrintedMatch{false};
    std::mutex s_globalsMutex;

    void reportVu1DiffTotals()
    {
        std::fprintf(stderr, "[vu1:diff] total runs=%llu mismatches=%llu\n",
                     static_cast<unsigned long long>(s_diffRuns.load()),
                     static_cast<unsigned long long>(s_diffMismatches.load()));
    }

    bool vu1DiffAotEnabled()
    {
        static const bool s_enabled = []()
        {
            const char *value = std::getenv("PS2X_VU1_DIFF");
            return value != nullptr && std::strcmp(value, "aot") == 0;
        }();
        return s_enabled;
    }

    bool vu1AotEnabled()
    {
        static const bool s_enabled = []()
        {
            const char *value = std::getenv("PS2X_VU1_AOT");
            if (value == nullptr)
                return true;
            return std::strcmp(value, "0") != 0 &&
                   std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "off") != 0;
        }();
        return s_enabled;
    }

    constexpr uint32_t kMaxCompiledPrograms = 64u;
    const ps2x::vu1::CompiledProgram *s_compiledPrograms[kMaxCompiledPrograms]{};
    uint32_t s_compiledProgramCount = 0u;
    bool s_compiledProgramsRegistered = false;
    std::set<uint64_t> s_aotMisses;
    std::set<uint64_t> s_aotHits;

    void logAotHit(uint32_t programHash, uint32_t pc)
    {
        const uint64_t key = (static_cast<uint64_t>(programHash) << 32) | pc;
        std::lock_guard<std::mutex> lock(s_globalsMutex);
        if (s_aotHits.insert(key).second)
        {
            std::fprintf(stderr, "[vu1-aot] run program=0x%08x pc=0x%x\n",
                         programHash, pc);
        }
    }

    void ensureCompiledProgramsRegistered()
    {
        static std::once_flag s_once;
        std::call_once(s_once, []
                       {
                           s_compiledProgramsRegistered = true;
                           ps2xVu1RegisterGeneratedPrograms();
                           if (s_compiledProgramCount != 0u)
                           {
                               std::fprintf(stderr, "[vu1-aot] registered %u compiled program(s)\n",
                                            s_compiledProgramCount);
                           } });
    }

    void logAotMiss(uint32_t programHash, uint32_t pc)
    {
        const uint64_t key = (static_cast<uint64_t>(programHash) << 32) | pc;
        std::lock_guard<std::mutex> lock(s_globalsMutex);
        if (s_aotMisses.insert(key).second)
        {
            std::fprintf(stderr, "[vu1-aot] miss program=0x%08x pc=0x%x\n",
                         programHash, pc);
        }
    }

    bool vu1AotStatsEnabled()
    {
        static const bool s_enabled = std::getenv("PS2X_VU1_AOT_STATS") != nullptr;
        return s_enabled;
    }

    uint64_t s_statsAotNs = 0;
    uint64_t s_statsAotPairs = 0;
    uint64_t s_statsInterpNs = 0;
    uint64_t s_statsInterpPairs = 0;
    uint64_t s_statsRuns = 0;

    void addAotStats(bool compiled, uint64_t hostNs, uint32_t pairs)
    {
        std::lock_guard<std::mutex> lock(s_globalsMutex);
        if (compiled)
        {
            s_statsAotNs += hostNs;
            s_statsAotPairs += pairs;
        }
        else
        {
            s_statsInterpNs += hostNs;
            s_statsInterpPairs += pairs;
        }
        ++s_statsRuns;
        if ((s_statsRuns & 0xFFFFu) == 0u)
        {
            const double aotPerPair = s_statsAotPairs
                                          ? static_cast<double>(s_statsAotNs) / s_statsAotPairs
                                          : 0.0;
            const double interpPerPair = s_statsInterpPairs
                                             ? static_cast<double>(s_statsInterpNs) / s_statsInterpPairs
                                             : 0.0;
            std::fprintf(stderr,
                         "[vu1:stats] aot pairs=%llu ns/pair=%.1f interp pairs=%llu ns/pair=%.1f\n",
                         static_cast<unsigned long long>(s_statsAotPairs), aotPerPair,
                         static_cast<unsigned long long>(s_statsInterpPairs), interpPerPair);
        }
    }
}

uint32_t ps2x::vu1::hash(const uint8_t *data, uint32_t size)
{
    uint32_t value = 0x811C9DC5u;
    for (uint32_t index = 0; index < size; ++index)
    {
        value ^= data[index];
        value *= 0x01000193u;
    }
    return value;
}

void ps2x::vu1::registerCompiledProgram(const CompiledProgram *program)
{
    if (program == nullptr || s_compiledProgramCount >= kMaxCompiledPrograms)
        return;
    s_compiledPrograms[s_compiledProgramCount++] = program;
}

uint32_t ps2x::vu1::compiledProgramCount()
{
    ensureCompiledProgramsRegistered();
    return s_compiledProgramCount;
}

const ps2x::vu1::CompiledProgram *ps2x::vu1::compiledProgram(uint32_t index)
{
    ensureCompiledProgramsRegistered();
    return index < s_compiledProgramCount ? s_compiledPrograms[index] : nullptr;
}

extern "C" __attribute__((weak)) void ps2xVu1RegisterGeneratedPrograms()
{
}

void VU1Interpreter::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (lanes == 0u)
        return;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
        {
            usage.vfRead[index].lanes |= lanes;
            return;
        }
    }
    if (usage.vfReadCount < usage.vfRead.size())
        usage.vfRead[usage.vfReadCount++] = {reg, lanes};
}

void VU1Interpreter::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

uint8_t VU1Interpreter::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

VU1Interpreter::VU1Interpreter(Unit unit)
    : m_unit(unit)
{
    reset();
}

void VU1Interpreter::resetScheduler()
{
    m_flagPipeline = {};
    m_fdiv = {};
    m_efu = {};
    m_storePipeline = {};
    m_vfWritePipeline = {};
    m_viWritePipeline = {};
    m_accWritePipeline = {};
    m_xgkick = {};
    m_vfReady = {};
    m_viReady = {};
    m_accReady = {};
    m_vfLatestWrite = {};
    m_viLatestWrite = {};
    m_accLatestWrite = {};
    m_nextWriteSequence = 0;
    m_efuResourceReady = 0;
    m_nextCommitCycle = UINT64_MAX;
    m_nextFlagCycle = UINT64_MAX;
    m_nextFdivCycle = UINT64_MAX;
    m_nextEfuCycle = UINT64_MAX;
    m_nextStoreCycle = UINT64_MAX;
    m_nextVfCycle = UINT64_MAX;
    m_nextViCycle = UINT64_MAX;
    m_nextAccCycle = UINT64_MAX;
    m_flagValid = 0u;
    m_efuValid = 0u;
    m_storeValid = 0u;
    m_vfValid = 0u;
    m_viValid = 0u;
    m_accValid = 0u;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    resetScheduler();
}



int32_t VU1Interpreter::readBranchVi(uint8_t reg) const
{
    if (reg == 0u)
        return 0;
    if (m_viBranchBackupValid &&
        m_viBranchBackupReg == reg)
    {
        return m_viBranchBackupValue;
    }
    return m_state.vi[reg];
}

void VU1Interpreter::recordViWriteForBranch(uint8_t reg, int32_t oldValue)
{
    if (reg == 0u)
        return;
    m_viBranchBackupValue = oldValue;
    m_viBranchBackupReg = reg;
    m_viBranchBackupValid = true;
}











void VU1Interpreter::queueFsset(uint16_t immediate)
{
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesStatus = false;
    }

    const uint32_t slot = static_cast<uint32_t>(std::countr_zero(
        static_cast<uint16_t>(~m_flagValid) & ((1u << kMaxFlagEntries) - 1u)));
    if (slot < kMaxFlagEntries)
    {
        FlagPipelineEntry &entry = m_flagPipeline[slot];
        m_flagValid = static_cast<uint16_t>(m_flagValid | (1u << slot));
        entry = {};
        entry.valid = true;
        entry.issueCycle = m_cycle;
        entry.readyCycle = m_cycle + kFmacLatency;
        m_nextCommitCycle = std::min(m_nextCommitCycle, entry.readyCycle);
        m_nextFlagCycle = std::min(m_nextFlagCycle, entry.readyCycle);
        entry.status = static_cast<uint32_t>(immediate) & 0xFC0u;
        entry.writesSticky = true;
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFFEu);
}

void VU1Interpreter::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    const uint32_t slot = static_cast<uint32_t>(std::countr_zero(
        static_cast<uint16_t>(~m_flagValid) & ((1u << kMaxFlagEntries) - 1u)));
    if (slot < kMaxFlagEntries)
    {
        FlagPipelineEntry &entry = m_flagPipeline[slot];
        m_flagValid = static_cast<uint16_t>(m_flagValid | (1u << slot));
        entry = {};
        entry.valid = true;
        entry.issueCycle = m_cycle;
        entry.readyCycle = m_cycle + kFmacLatency;
        m_nextCommitCycle = std::min(m_nextCommitCycle, entry.readyCycle);
        m_nextFlagCycle = std::min(m_nextFlagCycle, entry.readyCycle);
        entry.clip = m_workingClip;
        entry.writesClip = true;
        return;
    }
    reportReservedInstruction(true, 0xFFFFFFFDu);
}

void VU1Interpreter::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesClip = false;
    }
    const uint32_t slot = static_cast<uint32_t>(std::countr_zero(
        static_cast<uint16_t>(~m_flagValid) & ((1u << kMaxFlagEntries) - 1u)));
    if (slot < kMaxFlagEntries)
    {
        FlagPipelineEntry &entry = m_flagPipeline[slot];
        m_flagValid = static_cast<uint16_t>(m_flagValid | (1u << slot));
        entry = {};
        entry.valid = true;
        entry.issueCycle = m_cycle;
        entry.readyCycle = m_cycle + kFmacLatency;
        m_nextCommitCycle = std::min(m_nextCommitCycle, entry.readyCycle);
        m_nextFlagCycle = std::min(m_nextFlagCycle, entry.readyCycle);
        entry.clip = m_workingClip;
        entry.writesClip = true;
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFFAu);
}

void VU1Interpreter::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    m_nextCommitCycle = std::min(m_nextCommitCycle, m_fdiv.readyCycle);
    m_nextFdivCycle = std::min(m_nextFdivCycle, m_fdiv.readyCycle);
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
}

void VU1Interpreter::queueP(float value, uint32_t latency)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    const uint32_t slot = static_cast<uint32_t>(std::countr_zero(
        static_cast<uint8_t>(~m_efuValid) & 0x3u));
    if (slot < m_efu.size())
    {
        ScalarPipelineEntry &entry = m_efu[slot];
        m_efuValid = static_cast<uint8_t>(m_efuValid | (1u << slot));
        entry.valid = true;
        entry.readyCycle = m_cycle + latency;
        m_nextCommitCycle = std::min(m_nextCommitCycle, entry.readyCycle);
        m_nextEfuCycle = std::min(m_nextEfuCycle, entry.readyCycle);
        entry.value = value;
        // EFU throughput is one cycle shorter than result visibility.
        m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}





void VU1Interpreter::commitReadyPipelines()
{
    if (m_referenceScheduler)
    {
        commitReadyPipelinesReference();
        return;
    }
    if (m_nextCommitCycle > m_cycle)
        return;

    uint64_t nextCommit = UINT64_MAX;

    if (m_nextFlagCycle <= m_cycle)
    {
        uint64_t typeNext = UINT64_MAX;
        uint32_t mask = m_flagValid;
        while (mask != 0u)
        {
            const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
            mask &= mask - 1u;
            FlagPipelineEntry &entry = m_flagPipeline[index];
            if (entry.readyCycle > m_cycle)
            {
                typeNext = std::min(typeNext, entry.readyCycle);
                continue;
            }

            if (entry.writesMac)
                m_state.mac = entry.mac;
            if (entry.writesStatus)
            {
                const uint32_t current = entry.status & 0xFu;
                m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
            }
            if (entry.writesSticky)
                m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
            if (entry.writesClip)
                m_state.clip = entry.clip;
            entry = {};
            m_flagValid = static_cast<uint16_t>(m_flagValid & ~(1u << index));
        }
        m_nextFlagCycle = typeNext;
    }
    nextCommit = std::min(nextCommit, m_nextFlagCycle);

    if (m_fdiv.valid)
    {
        if (m_fdiv.readyCycle > m_cycle)
        {
            m_nextFdivCycle = m_fdiv.readyCycle;
        }
        else
        {
            m_state.q = m_fdiv.value;
            const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
            m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
            m_fdiv = {};
            m_nextFdivCycle = UINT64_MAX;
        }
    }
    else
    {
        m_nextFdivCycle = UINT64_MAX;
    }
    nextCommit = std::min(nextCommit, m_nextFdivCycle);

    if (m_nextEfuCycle <= m_cycle)
    {
        uint64_t typeNext = UINT64_MAX;
        uint32_t mask = m_efuValid;
        while (mask != 0u)
        {
            const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
            mask &= mask - 1u;
            ScalarPipelineEntry &entry = m_efu[index];
            if (entry.readyCycle > m_cycle)
            {
                typeNext = std::min(typeNext, entry.readyCycle);
                continue;
            }
            m_state.p = entry.value;
            entry = {};
            m_efuValid = static_cast<uint8_t>(m_efuValid & ~(1u << index));
        }
        m_nextEfuCycle = typeNext;
    }
    nextCommit = std::min(nextCommit, m_nextEfuCycle);

    if (m_nextStoreCycle <= m_cycle)
    {
        uint64_t typeNext = UINT64_MAX;
        uint32_t mask = m_storeValid;
        while (mask != 0u)
        {
            const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
            mask &= mask - 1u;
            PendingStore &store = m_storePipeline[index];
            if (store.readyCycle > m_cycle)
            {
                typeNext = std::min(typeNext, store.readyCycle);
                continue;
            }
            if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
            {
                uint32_t oldWords[4]{};
                std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
                for (uint32_t component = 0; component < 4u; ++component)
                {
                    if ((store.laneMask & laneForComponent(component)) != 0u)
                        oldWords[component] = store.words[component];
                }
                std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
            }
            store = {};
            m_storeValid = static_cast<uint16_t>(m_storeValid & ~(1u << index));
        }
        m_nextStoreCycle = typeNext;
    }
    nextCommit = std::min(nextCommit, m_nextStoreCycle);

    if (m_nextVfCycle <= m_cycle)
    {
        uint64_t typeNext = UINT64_MAX;
        uint32_t mask = m_vfValid;
        while (mask != 0u)
        {
            const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
            mask &= mask - 1u;
            PendingVfWrite &write = m_vfWritePipeline[index];
            if (write.readyCycle > m_cycle)
            {
                typeNext = std::min(typeNext, write.readyCycle);
                continue;
            }
            // PS2X(perf): the common case is a full-lane write that is still
            // the latest for its register -- copy the whole vector at once
            // instead of four lane/sequence tests and four scalar stores.
            // This loop was ~19% of the VU1 AOT benchmark.
            const std::array<uint64_t, 4> &latest = m_vfLatestWrite[write.reg];
            if (write.laneMask == 0x0Fu &&
                latest[0] == write.sequence && latest[1] == write.sequence &&
                latest[2] == write.sequence && latest[3] == write.sequence)
            {
                std::memcpy(m_state.vf[write.reg], write.value.data(), sizeof(write.value));
            }
            else
            {
                for (uint32_t component = 0; component < 4u; ++component)
                {
                    if ((write.laneMask & laneForComponent(component)) != 0u &&
                        m_vfLatestWrite[write.reg][component] == write.sequence)
                    {
                        m_state.vf[write.reg][component] = write.value[component];
                    }
                }
            }
            write = {};
            m_vfValid = static_cast<uint16_t>(m_vfValid & ~(1u << index));
        }
        m_nextVfCycle = typeNext;
    }
    nextCommit = std::min(nextCommit, m_nextVfCycle);

    if (m_nextViCycle <= m_cycle)
    {
        uint64_t typeNext = UINT64_MAX;
        uint32_t mask = m_viValid;
        while (mask != 0u)
        {
            const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
            mask &= mask - 1u;
            PendingViWrite &write = m_viWritePipeline[index];
            if (write.readyCycle > m_cycle)
            {
                typeNext = std::min(typeNext, write.readyCycle);
                continue;
            }
            if (m_viLatestWrite[write.reg] == write.sequence)
                m_state.vi[write.reg] = static_cast<int16_t>(write.value);
            write = {};
            m_viValid = static_cast<uint16_t>(m_viValid & ~(1u << index));
        }
        m_nextViCycle = typeNext;
    }
    nextCommit = std::min(nextCommit, m_nextViCycle);

    if (m_nextAccCycle <= m_cycle)
    {
        uint64_t typeNext = UINT64_MAX;
        uint32_t mask = m_accValid;
        while (mask != 0u)
        {
            const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
            mask &= mask - 1u;
            PendingAccWrite &write = m_accWritePipeline[index];
            if (write.readyCycle > m_cycle)
            {
                typeNext = std::min(typeNext, write.readyCycle);
                continue;
            }
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((write.laneMask & laneForComponent(component)) != 0u &&
                    m_accLatestWrite[component] == write.sequence)
                {
                    m_state.acc[component] = write.value[component];
                }
            }
            write = {};
            m_accValid = static_cast<uint16_t>(m_accValid & ~(1u << index));
        }
        m_nextAccCycle = typeNext;
    }
    nextCommit = std::min(nextCommit, m_nextAccCycle);

    m_nextCommitCycle = nextCommit;
}

void VU1Interpreter::commitReadyPipelinesReference()
{
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid || entry.readyCycle > m_cycle)
            continue;
        if (entry.writesMac)
            m_state.mac = entry.mac;
        if (entry.writesStatus)
        {
            const uint32_t current = entry.status & 0xFu;
            m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
        }
        if (entry.writesSticky)
            m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
        if (entry.writesClip)
            m_state.clip = entry.clip;
        entry = {};
    }

    if (m_fdiv.valid && m_fdiv.readyCycle <= m_cycle)
    {
        m_state.q = m_fdiv.value;
        const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
        m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
        m_fdiv = {};
    }

    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (entry.valid && entry.readyCycle <= m_cycle)
        {
            m_state.p = entry.value;
            entry = {};
        }
    }

    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid || store.readyCycle > m_cycle)
            continue;
        if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
        {
            uint32_t oldWords[4]{};
            std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((store.laneMask & laneForComponent(component)) != 0u)
                    oldWords[component] = store.words[component];
            }
            std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
        }
        store = {};
    }

    for (PendingVfWrite &write : m_vfWritePipeline)
    {
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_vfLatestWrite[write.reg][component] == write.sequence)
            {
                m_state.vf[write.reg][component] = write.value[component];
            }
        }
        write = {};
    }

    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        if (m_viLatestWrite[write.reg] == write.sequence)
            m_state.vi[write.reg] = static_cast<int16_t>(write.value);
        write = {};
    }

    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_accLatestWrite[component] == write.sequence)
            {
                m_state.acc[component] = write.value[component];
            }
        }
        write = {};
    }

    // The fast path owns the occupancy masks; rebuild them after the reference
    // pass so a mixed reference/new run cannot see stale slots.
    m_flagValid = 0u;
    for (uint32_t i = 0; i < kMaxFlagEntries; ++i)
        if (m_flagPipeline[i].valid)
            m_flagValid = static_cast<uint16_t>(m_flagValid | (1u << i));
    m_efuValid = 0u;
    for (uint32_t i = 0; i < m_efu.size(); ++i)
        if (m_efu[i].valid)
            m_efuValid = static_cast<uint8_t>(m_efuValid | (1u << i));
    m_storeValid = 0u;
    for (uint32_t i = 0; i < kMaxPendingStores; ++i)
        if (m_storePipeline[i].valid)
            m_storeValid = static_cast<uint16_t>(m_storeValid | (1u << i));
    m_vfValid = 0u;
    for (uint32_t i = 0; i < kMaxPendingVfWrites; ++i)
        if (m_vfWritePipeline[i].valid)
            m_vfValid = static_cast<uint16_t>(m_vfValid | (1u << i));
    m_viValid = 0u;
    for (uint32_t i = 0; i < kMaxPendingViWrites; ++i)
        if (m_viWritePipeline[i].valid)
            m_viValid = static_cast<uint16_t>(m_viValid | (1u << i));
    m_accValid = 0u;
    for (uint32_t i = 0; i < kMaxPendingAccWrites; ++i)
        if (m_accWritePipeline[i].valid)
            m_accValid = static_cast<uint16_t>(m_accValid | (1u << i));
}

void VU1Interpreter::progressXgkick()
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    ++m_xgkick.cycleCredit;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            // Not a VU instruction problem: the GIF packet is larger than the
            // transfer buffer. Name it accurately instead of reporting the
            // synthetic "reserved lower" sentinel.
            RUNTIME_ERROR("[vu1:xgkick] packet too large src=0x" << std::hex
                          << m_xgkick.sourceAddress << " copied=0x" << m_xgkick.copiedBytes
                          << std::dec << " (GIF kick aborted)\n");
            m_stopRequested = true;
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        // VU data memory is at most 16 KiB and usually a power of two; copying
        // one contiguous 16-byte qword (with a wrap split at most once) is much
        // cheaper than a per-byte modulo in the PATH1 transfer loop.
        const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes) % m_activeVuDataSize;
        if (source + 16u <= m_activeVuDataSize)
        {
            std::memcpy(m_xgkick.packet.data() + m_xgkick.copiedBytes,
                        m_activeVuData + source, 16u);
        }
        else
        {
            const uint32_t first = m_activeVuDataSize - source;
            std::memcpy(m_xgkick.packet.data() + m_xgkick.copiedBytes,
                        m_activeVuData + source, first);
            std::memcpy(m_xgkick.packet.data() + m_xgkick.copiedBytes + first,
                        m_activeVuData, 16u - first);
        }
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u || format == 3u) // FLG=3 ("disabled" in the GS manual) behaves exactly like IMAGE: NLOOP qwords of image data.
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                RUNTIME_ERROR("[vu1:xgkick] unknown GIF format " << format
                              << " src=0x" << std::hex << m_xgkick.sourceAddress
                              << std::dec << " (GIF kick aborted)\n");
                m_stopRequested = true;
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                RUNTIME_ERROR("[vu1:xgkick] malformed packet src=0x" << std::hex
                              << m_xgkick.sourceAddress << " qw=0x" << qwordOffset
                              << " tagBytes=" << std::dec << static_cast<unsigned long long>(tagBytes)
                              << " nloop=" << nloop << " flg=" << format
                              << " nreg=" << nreg << " (GIF kick aborted)\n");
                // PS2X(diag-vu1-xgkick-dump): at the failing tag, show the
                // VIF1 TOP/ITOP the program saw, every VI register, and the
                // qwords around the tag. The packet source is often a pointer
                // loaded from the data buffer, so both matter.
                if (vu1TraceEnabled())
                {
                    std::fprintf(stderr, "[vu1:xgkick] dump top=0x%x itop=0x%x pc=0x%x vi:",
                                 m_state.top, m_state.itop, m_state.pc);
                    for (uint32_t i = 1; i < 16u; ++i)
                        std::fprintf(stderr, " %u=0x%x", i, static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[i])));
                    std::fprintf(stderr, "\n");
                    const uint32_t tagByte = (m_xgkick.sourceAddress + qwordOffset) % m_activeVuDataSize;
                    for (uint32_t q = 0; q < 8u; ++q)
                    {
                        const uint32_t at = (tagByte + q * 16u) % m_activeVuDataSize;
                        uint64_t lo = 0, hi = 0;
                        std::memcpy(&lo, m_activeVuData + at, 8u);
                        std::memcpy(&hi, m_activeVuData + at + 8u, 8u);
                        std::fprintf(stderr, "[vu1:xgkick]   +%02u @0x%04x = %016llx %016llx\n",
                                     q, at, static_cast<unsigned long long>(lo),
                                     static_cast<unsigned long long>(hi));
                    }
                }
                m_stopRequested = true;
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    if (vu1TraceEnabled())
    {
        uint64_t tagLo = 0;
        uint64_t tagHi = 0;
        if (m_xgkick.totalBytes >= 16u)
        {
            std::memcpy(&tagLo, m_xgkick.packet.data(), sizeof(tagLo));
            std::memcpy(&tagHi, m_xgkick.packet.data() + 8, sizeof(tagHi));
        }
        std::fprintf(stderr, "[vu1:xgkick] submit bytes=%u qwords=%u tag=0x%016llx_%016llx\n",
                     m_xgkick.totalBytes, m_xgkick.totalBytes / 16u,
                     static_cast<unsigned long long>(tagLo), static_cast<unsigned long long>(tagHi));
    }
    if (VU1InventoryRecorder *inventory = VU1InventoryRecorder::instance())
        inventory->onXgkick(m_xgkick.totalBytes);
    if (m_xgkickCapture != nullptr)
    {
        m_xgkickCapture->emplace_back(m_xgkick.packet.data(),
                                      m_xgkick.packet.data() + m_xgkick.totalBytes);
    }
    else if (m_activeMemory)
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);
    else if (m_activeGs)
        m_activeGs->processGIFPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
    m_xgkick.active = false;
}

void VU1Interpreter::startXgkick(uint32_t qwordAddress)
{
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    if (vu1TraceEnabled())
    {
        std::fprintf(stderr, "[vu1:xgkick] start qword=0x%x byte=0x%x pc=0x%x top=0x%x itop=0x%x\n",
                     qwordAddress, sourceAddress, m_state.pc, m_state.top, m_state.itop);
    }
    // Do not value-initialize the 64 KiB packet buffer here: progressXgkick()
    // fills every byte it later submits.  Zeroing it was a 64 KiB memset per
    // XGKICK on the title path.
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.totalBytes = 0u;
    m_xgkick.copiedBytes = 0u;
    m_xgkick.currentTagEnd = 0u;
    m_xgkick.currentTagEop = false;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
}

void VU1Interpreter::advanceTo(uint64_t targetCycle)
{
    while (m_cycle < targetCycle)
    {
        if (!m_referenceScheduler && !m_xgkick.active)
        {
            const uint64_t next = std::min(targetCycle, m_nextCommitCycle);
            if (next > m_cycle)
            {
                m_cycle = next;
                m_state.cycles = m_cycle;
                continue;
            }
        }
        advanceOneCycle();
    }
    // Original advanceTo() committed at every intermediate cycle.  When the
    // jump lands exactly on a ready cycle (or on the cycle budget) make that
    // commit visible now instead of waiting for the next instruction.
    if (m_nextCommitCycle <= m_cycle)
        commitReadyPipelines();
}

bool VU1Interpreter::pipelinesPending() const
{
    return m_fdiv.valid || m_xgkick.active ||
           m_efuValid != 0u || m_flagValid != 0u || m_storeValid != 0u ||
           m_vfValid != 0u || m_viValid != 0u || m_accValid != 0u;
}

void VU1Interpreter::flushPipelines()
{
    while (pipelinesPending())
        advanceOneCycle();
}

uint64_t VU1Interpreter::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    uint64_t ready = m_cycle;
    const InstructionUsage *usages[2] = {
        &decoded.upperUsage,
        &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        if (!usage)
            continue;
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((access.lanes & laneForComponent(component)) != 0u)
                    ready = std::max(ready, m_vfReady[access.reg][component]);
            }
        }
        uint32_t viRead = usage->viRead;
        while (viRead != 0u)
        {
            const uint32_t reg = static_cast<uint32_t>(std::countr_zero(viRead));
            ready = std::max(ready, m_viReady[reg]);
            viRead &= viRead - 1u;
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((usage->accRead & laneForComponent(component)) != 0u)
                ready = std::max(ready, m_accReady[component]);
        }
    }

    if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (decoded.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (decoded.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}

void VU1Interpreter::markPairWrites(const DecodedInstructionPair &decoded)
{
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u &&
        decoded.suppressedLowerVf != lowerWrite.reg)
    {
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((lowerWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[lowerWrite.reg][component] = m_cycle + latency;
        }
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        const uint32_t latency = decoded.upperUsage.vfLatency != 0u
                                     ? decoded.upperUsage.vfLatency
                                     : decoded.upperUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((upperWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[upperWrite.reg][component] = m_cycle + latency;
        }
    }

    uint32_t viWrite = decoded.lowerUsage.viWrite;
    while (viWrite != 0u)
    {
        const uint32_t reg = static_cast<uint32_t>(std::countr_zero(viWrite));
        m_viReady[reg] = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
        viWrite &= viWrite - 1u;
    }
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((decoded.upperUsage.accWrite & laneForComponent(component)) != 0u)
            m_accReady[component] = m_cycle + kAccForwardLatency;
    }
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeUpperUsage(uint32_t upper) const
{
    InstructionUsage usage;
    usage.pipeline = PipelineFmac;
    usage.latency = kFmacLatency;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t dest = DEST(upper);
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t fd = FD(upper);

    if (op <= 0x2Fu)
    {
        addVfRead(usage, fs, dest);
        addVfWrite(usage, fd, dest);
        if (op <= 0x1Bu)
            addVfRead(usage, ft, laneForComponent(op & 3u));
        else if (op >= 0x28u)
            addVfRead(usage, ft, op == 0x2Eu ? 0xEu : dest);
        if (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
            op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if (op >= 0x3Cu)
    {
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        const bool writesAcc =
            special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) ||
            special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        if (writesAcc)
        {
            addVfRead(usage, fs, dest);
            if (special <= 0x1Bu)
                addVfRead(usage, ft, laneForComponent(special & 3u));
            else if ((special >= 0x28u && special <= 0x2Eu))
                addVfRead(usage, ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if ((special >= 0x08u && special <= 0x0Fu) ||
                special == 0x21u || special == 0x23u || special == 0x25u ||
                special == 0x27u || special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if (special >= 0x10u && special <= 0x17u)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Du)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Fu)
        {
            addVfRead(usage, fs, 0xEu);
            addVfRead(usage, ft, 0x1u);
            usage.writesClip = true;
        }
        else if (special != 0x2Fu && special != 0x30u)
        {
            usage.reserved = true;
        }
        return usage;
    }

    usage.reserved = true;
    return usage;
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeLowerUsage(uint32_t lower) const
{
    InstructionUsage usage;
    if (lower == 0u || lower == 0x8000033Cu)
        return usage;

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t vfT = FT(lower);
    const uint8_t vfS = FS(lower);
    const uint8_t viT = VIT(lower);
    const uint8_t viS = VIS(lower);
    const uint8_t viD = VID(lower);
    const uint8_t dest = DEST(lower);
    auto readVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    auto writeVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWrite |= static_cast<uint16_t>(1u << reg);
    };

    switch (opHi)
    {
    case 0x00:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        return usage;
    case 0x01:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viT);
        addVfRead(usage, vfS, dest);
        return usage;
    case 0x04:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x05:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x08:
    case 0x09:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x10:
    case 0x12:
    case 0x13:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(1u);
        return usage;
    case 0x11:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        usage.writesClip = true;
        return usage;
    case 0x14:
    case 0x16:
    case 0x17:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x15:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        return usage;
    case 0x18:
    case 0x1A:
    case 0x1B:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x1C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(viT);
        return usage;
    case 0x20:
        usage.pipeline = PipelineBranch;
        return usage;
    case 0x21:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x24:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x25:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x28:
    case 0x29:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x40:
        break;
    default:
        usage.reserved = true;
        return usage;
    }

    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        readVi(viT);
        writeVi(viD);
        return usage;
    }
    if (direct == 0x32u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    }
    if (direct < 0x3Cu)
    {
        usage.reserved = true;
        return usage;
    }

    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    switch (special)
    {
    case 0x30:
    case 0x31:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfRead(usage, vfS, special == 0x31u ? 0xFu : dest);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x34:
    case 0x36:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        usage.viLatency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x35:
    case 0x37:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viT);
        writeVi(viT);
        addVfRead(usage, vfS, dest);
        break;
    case 0x38:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x39:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3A:
        usage.pipeline = PipelineFdiv;
        usage.latency = 13u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3B:
        usage.pipeline = PipelineFdiv;
        usage.waitQ = true;
        break;
    case 0x3C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        writeVi(viT);
        break;
    case 0x3D:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x3E:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        break;
    case 0x3F:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        break;
    case 0x40:
    case 0x41:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x42:
    case 0x43:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x64:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x68:
    case 0x69:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        break;
    case 0x6C:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineXgkick;
        usage.latency = 2u;
        readVi(viS);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        switch (special)
        {
        case 0x70:
            usage.latency = 11u;
            break;
        case 0x71:
        case 0x72:
        case 0x77:
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7C:
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78:
        case 0x7A:
            usage.latency = 12u;
            break;
        case 0x79:
            usage.latency = 29u;
            break;
        case 0x7D:
            usage.latency = 44u;
            break;
        default:
            break;
        }
        if (special >= 0x70u && special <= 0x73u)
            addVfRead(usage, vfS, 0xEu);
        else if (special == 0x74u)
            addVfRead(usage, vfS, 0xCu);
        else if (special == 0x75u)
            addVfRead(usage, vfS, 0xAu);
        else if (special == 0x76u)
            addVfRead(usage, vfS, 0xFu);
        else
            addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x7B:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        usage.waitP = true;
        break;
    default:
        usage.reserved = true;
        break;
    }
    return usage;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
{
    DecodedInstructionPair decoded;
    std::memcpy(&decoded.lower, vuCode + pc, sizeof(decoded.lower));
    std::memcpy(&decoded.upper, vuCode + pc + sizeof(decoded.lower), sizeof(decoded.upper));
    decoded.iBit = (decoded.upper & 0x80000000u) != 0u;
    decoded.eBit = (decoded.upper & 0x40000000u) != 0u;
    decoded.mBit = (decoded.upper & 0x20000000u) != 0u;
    decoded.dBit = (decoded.upper & 0x10000000u) != 0u;
    decoded.tBit = (decoded.upper & 0x08000000u) != 0u;
    decoded.upperUsage = decodeUpperUsage(decoded.upper);
    if (!decoded.iBit)
        decoded.lowerUsage = decodeLowerUsage(decoded.lower);

    const uint8_t upperWriteReg = decoded.upperUsage.vfWrite.reg;
    if (upperWriteReg != 0u && (vfReadLanes(decoded.lowerUsage, upperWriteReg) != 0u || decoded.lowerUsage.vfWrite.reg == upperWriteReg))
    {
        decoded.upperVfShadowReg = upperWriteReg;
        if (decoded.lowerUsage.vfWrite.reg == upperWriteReg)
            decoded.suppressedLowerVf = upperWriteReg;
    }
    return decoded;
}

void VU1Interpreter::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
                                             const PS2Memory *memory, uint64_t generation)
{
    const uint32_t pairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    for (uint32_t i = 0; i < pairCount; ++i)
        m_decodedCodeCache[i] = decodeInstructionPair(vuCode, i * 8u);

    m_cachedVuCode = vuCode;
    m_cachedMemory = memory;
    m_cachedCodeSize = codeSize;
    m_cachedCodeGeneration = generation;
    m_decodedCodeCacheValid = true;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
        return decodeInstructionPair(vuCode, pc);

    const bool trackedVu1Code = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedVu1Code)
        return decodeInstructionPair(vuCode, pc);

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    const uint32_t pairIndex = pc / 8u;
    if (pairIndex >= kMaxDecodedPairs)
        return decodeInstructionPair(vuCode, pc);
    return m_decodedCodeCache[pairIndex];
}

void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t instruction)
{
    RUNTIME_ERROR(
        "[VU" << (m_unit == Unit::VU1 ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << m_cycle
              << " pc=0x" << std::hex << m_state.pc
              << " instruction=0x" << instruction
              << std::dec << '\n');
    m_stopRequested = true;
}

const ps2x::vu1::CompiledProgram *VU1Interpreter::findCompiledProgram(
    uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory)
{
    if (m_unit != Unit::VU1 || m_aotDisabled || !vu1AotEnabled())
        return nullptr;

    ensureCompiledProgramsRegistered();
    if (s_compiledProgramCount == 0u)
        return nullptr;

    const bool tracked = memory != nullptr && vuCode == memory->getVU1Code();
    const uint64_t generation = tracked ? memory->getVU1CodeGeneration() : 0u;
    if (m_aotProgramCacheValid &&
        m_aotProgramCode == vuCode &&
        m_aotProgramCodeSize == codeSize &&
        (!tracked || m_aotProgramGeneration == generation))
    {
        return m_aotProgram;
    }

    m_aotProgram = nullptr;
    m_aotProgramCode = vuCode;
    m_aotProgramCodeSize = codeSize;
    m_aotProgramGeneration = generation;
    m_aotProgramCacheValid = true;
    for (uint32_t index = 0; index < s_compiledProgramCount; ++index)
    {
        const ps2x::vu1::CompiledProgram *program = s_compiledPrograms[index];
        if (program == nullptr || program->run == nullptr)
            continue;
        if (program->extentOffset > codeSize ||
            program->extentSize > codeSize - program->extentOffset)
            continue;
        if (ps2x::vu1::hash(vuCode + program->extentOffset, program->extentSize) ==
            program->programHash)
        {
            m_aotProgram = program;
            break;
        }
    }
    return m_aotProgram;
}

ps2x::vu1::AotResult VU1Interpreter::aotStepPair(const DecodedInstructionPair &decoded)
{
    if (m_stopRequested)
        return ps2x::vu1::AotResult::StopRequested;
    if (m_cycle >= m_budgetEnd)
        return ps2x::vu1::AotResult::BudgetEnded;
    if (m_nextCommitCycle <= m_cycle)
        commitReadyPipelines();
    if (m_state.pc + 8u > m_activeCodeSize)
    {
        m_stopReason = "pc-out-of-range";
        return ps2x::vu1::AotResult::StopRequested;
    }

    if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
    {
        m_stopReason = "reserved";
        reportReservedInstruction(decoded.upperUsage.reserved,
                                  decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
        return ps2x::vu1::AotResult::StopRequested;
    }

    const uint64_t readyCycle = calculatePairReadyCycle(decoded);
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

    {
        // PS2X(diag-vu1-itrace): PS2X_VU1_ITRACE=<startpc hex>[:<count>]
        // logs every pair of the first <count> runs that begin at startpc.
        static const char *s_itrace = std::getenv("PS2X_VU1_ITRACE");
        static uint32_t s_itracePc = s_itrace ? static_cast<uint32_t>(std::strtoul(s_itrace, nullptr, 16)) : 0xFFFFFFFFu;
        static uint32_t s_itraceRuns = [] {
            const char *c = s_itrace ? std::strchr(s_itrace, ':') : nullptr;
            return c ? static_cast<uint32_t>(std::strtoul(c + 1, nullptr, 10)) : 1u;
        }();
        static uint64_t s_itraceRunCycle = ~0ull;
        if (s_itrace && m_traceStartPc == s_itracePc && s_itraceRuns != 0u)
        {
            if (s_itraceRunCycle == ~0ull)
                s_itraceRunCycle = m_budgetEnd;
            if (s_itraceRunCycle == m_budgetEnd)
            {
                std::fprintf(stderr, "[vu1:i] pc=0x%04x L=%08x U=%08x vi", m_state.pc, decoded.lower, decoded.upper);
                for (int r = 1; r < 16; ++r)
                    std::fprintf(stderr, " %x", static_cast<uint32_t>(m_state.vi[r]) & 0xFFFFu);
                std::fprintf(stderr, "\n");
            }
        }
    }

    uint8_t writtenVi = 0u;
    int32_t oldVi = 0;
    for (uint32_t reg = 1; reg < 16u; ++reg)
    {
        if ((decoded.lowerUsage.viWrite & (1u << reg)) != 0u)
        {
            writtenVi = static_cast<uint8_t>(reg);
            oldVi = m_state.vi[reg];
            break;
        }
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    const bool hasUpperWrite = upperWrite.reg != 0u;
    const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
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
    if (decoded.upperUsage.accWrite != 0u)
        std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

    if (decoded.iBit)
    {
        execUpper(decoded.upper);
        float immediate = 0.0f;
        std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
        m_state.i = normalizeOperand(immediate);
    }
    else if (decoded.upperVfShadowReg != 0u)
    {
        float oldVf[4]{};
        float upperVf[4]{};
        std::memcpy(oldVf,
                    m_state.vf[decoded.upperVfShadowReg],
                    sizeof(oldVf));
        execUpper(decoded.upper);
        std::memcpy(upperVf,
                    m_state.vf[decoded.upperVfShadowReg],
                    sizeof(upperVf));
        std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                    oldVf,
                    sizeof(oldVf));
        execLower(decoded.lower, m_activeVuData, m_activeVuDataSize, *m_activeGs, m_activeMemory, decoded.upper);
        std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                    upperVf,
                    sizeof(upperVf));
    }
    else
    {
        execUpper(decoded.upper);
        execLower(decoded.lower, m_activeVuData, m_activeVuDataSize, *m_activeGs, m_activeMemory, decoded.upper);
    }

    if (m_inventory != nullptr)
    {
        m_inventory->onPair(m_state.pc, decoded.lower, decoded.upper,
                            decoded.iBit, decoded.eBit, decoded.dBit,
                            decoded.tBit, decoded.mBit, m_state.branchPending,
                            m_state.branchTarget);
    }

    m_viBranchBackupValid = false;

    if (hasUpperWrite)
    {
        std::memcpy(newUpperVf, m_state.vf[upperWrite.reg], sizeof(newUpperVf));
        std::memcpy(m_state.vf[upperWrite.reg], oldUpperVf, sizeof(oldUpperVf));
        const uint32_t latency =
            decoded.upperUsage.vfLatency != 0u
                ? decoded.upperUsage.vfLatency
                : decoded.upperUsage.latency;
        queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
    }
    if (hasDistinctLowerWrite)
    {
        std::memcpy(newLowerVf, m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
        std::memcpy(m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(oldLowerVf));
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
    }
    if (decoded.upperUsage.accWrite != 0u)
    {
        std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
        std::memcpy(m_state.acc, oldAcc, sizeof(oldAcc));
        // ACC is forwarded to the next upper instruction. Its arithmetic
        // flags still use the normal four-cycle FMAC timeline.
        queueAccWrite(decoded.upperUsage.accWrite, newAcc,
                      kAccForwardLatency);
    }
    if (writtenVi != 0u)
    {
        const int32_t newVi = m_state.vi[writtenVi];
        m_state.vi[writtenVi] = oldVi;
        const uint32_t latency =
            decoded.lowerUsage.viLatency != 0u
                ? decoded.lowerUsage.viLatency
                : decoded.lowerUsage.latency;
        queueViWrite(writtenVi, newVi, latency);
    }

    markPairWrites(decoded);
    if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
        recordViWriteForBranch(writtenVi, oldVi);

    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    m_state.vi[0] = 0;

    uint32_t nextPc = m_state.pc + 8u;
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

    const bool dHalt = decoded.dBit && m_state.dBitEnabled;
    const bool tHalt = decoded.tBit && m_state.tBitEnabled;
    const bool haltBit = dHalt || tHalt;
    const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;

    bool programEnded = false;
    if (m_state.haltAfterDelaySlot)
    {
        m_state.stoppedByD = m_pendingHaltD;
        m_state.stoppedByT = m_pendingHaltT;
        programEnded = true;
    }
    else if (m_state.ebit)
        programEnded = true;
    else if (haltBit && !haltBranch)
    {
        m_state.stoppedByD = dHalt;
        m_state.stoppedByT = tHalt;
        programEnded = true;
    }
    else if (decoded.eBit)
        m_state.ebit = true;
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

void VU1Interpreter::runDifferential(uint8_t *vuCode, uint32_t codeSize,
                                     uint8_t *vuData, uint32_t dataSize,
                                     GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    runDifferentialImpl(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles, false);
}

void VU1Interpreter::runAotDifferential(uint8_t *vuCode, uint32_t codeSize,
                                        uint8_t *vuData, uint32_t dataSize,
                                        GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    runDifferentialImpl(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles, true);
}

void VU1Interpreter::runDifferentialImpl(uint8_t *vuCode, uint32_t codeSize,
                                         uint8_t *vuData, uint32_t dataSize,
                                         GS &gs, PS2Memory *memory, uint32_t maxCycles,
                                         bool candidateAot)
{
    std::call_once(s_diffRegistered, [] { std::atexit(reportVu1DiffTotals); });

    const VU1Interpreter saved = *this;
    const std::vector<uint8_t> initialData(vuData, vuData + dataSize);
    const uint32_t startPc = m_state.pc;

    std::vector<std::vector<uint8_t>> referencePackets;
    *this = saved;
    m_referenceScheduler = true;
    m_aotDisabled = true;
    m_xgkickCapture = &referencePackets;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    m_xgkickCapture = nullptr;
    m_referenceScheduler = false;
    const VU1State referenceState = m_state;
    const std::vector<uint8_t> referenceData(vuData, vuData + dataSize);

    std::vector<std::vector<uint8_t>> newPackets;
    *this = saved;
    std::memcpy(vuData, initialData.data(), dataSize);
    m_referenceScheduler = false;
    m_aotDisabled = !candidateAot;
    m_xgkickCapture = &newPackets;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    m_xgkickCapture = nullptr;
    m_aotDisabled = saved.m_aotDisabled;

    const uint64_t diffRuns = ++s_diffRuns;
    bool mismatch = false;
    if (std::memcmp(&referenceState, &m_state, sizeof(VU1State)) != 0)
    {
        std::fprintf(stderr, "[vu1:diff] mismatch state pc=0x%x", startPc);
        if (referenceState.pc != m_state.pc) std::fprintf(stderr, " pc %x/%x", referenceState.pc, m_state.pc);
        if (referenceState.cycles != m_state.cycles) std::fprintf(stderr, " cycles %llu/%llu", (unsigned long long)referenceState.cycles, (unsigned long long)m_state.cycles);
        if (referenceState.q != m_state.q) std::fprintf(stderr, " q %g/%g", referenceState.q, m_state.q);
        if (referenceState.p != m_state.p) std::fprintf(stderr, " p %g/%g", referenceState.p, m_state.p);
        if (referenceState.i != m_state.i) std::fprintf(stderr, " i %g/%g", referenceState.i, m_state.i);
        if (referenceState.r != m_state.r) std::fprintf(stderr, " r %x/%x", referenceState.r, m_state.r);
        if (referenceState.mac != m_state.mac) std::fprintf(stderr, " mac %x/%x", referenceState.mac, m_state.mac);
        if (referenceState.clip != m_state.clip) std::fprintf(stderr, " clip %x/%x", referenceState.clip, m_state.clip);
        if (referenceState.status != m_state.status) std::fprintf(stderr, " status %x/%x", referenceState.status, m_state.status);
        for (int reg = 0; reg < 32; ++reg)
            if (std::memcmp(referenceState.vf[reg], m_state.vf[reg], sizeof(float) * 4) != 0)
                std::fprintf(stderr, " vf%d=(%g,%g,%g,%g)/(%g,%g,%g,%g)", reg,
                             referenceState.vf[reg][0], referenceState.vf[reg][1], referenceState.vf[reg][2], referenceState.vf[reg][3],
                             m_state.vf[reg][0], m_state.vf[reg][1], m_state.vf[reg][2], m_state.vf[reg][3]);
        for (int reg = 0; reg < 16; ++reg)
            if (referenceState.vi[reg] != m_state.vi[reg])
                std::fprintf(stderr, " vi%d=%x/%x", reg, referenceState.vi[reg], m_state.vi[reg]);
        if (std::memcmp(referenceState.acc, m_state.acc, sizeof(referenceState.acc)) != 0)
            std::fprintf(stderr, " acc=(%g,%g,%g,%g)/(%g,%g,%g,%g)",
                         referenceState.acc[0], referenceState.acc[1], referenceState.acc[2], referenceState.acc[3],
                         m_state.acc[0], m_state.acc[1], m_state.acc[2], m_state.acc[3]);
        std::fprintf(stderr, "\n");
        mismatch = true;
    }
    if (referenceData != std::vector<uint8_t>(vuData, vuData + dataSize))
    {
        std::fprintf(stderr, "[vu1:diff] mismatch vu-data pc=0x%x bytes=%u\n", startPc, dataSize);
        mismatch = true;
    }
    if (referencePackets.size() != newPackets.size())
    {
        std::fprintf(stderr, "[vu1:diff] mismatch xgkick-count pc=0x%x ref=%zu new=%zu\n",
                     startPc, referencePackets.size(), newPackets.size());
        mismatch = true;
    }
    else
    {
        for (size_t i = 0; i < referencePackets.size(); ++i)
        {
            if (referencePackets[i] != newPackets[i])
            {
                std::fprintf(stderr, "[vu1:diff] mismatch xgkick-packet pc=0x%x index=%zu ref=%zu new=%zu\n",
                             startPc, i, referencePackets[i].size(), newPackets[i].size());
                mismatch = true;
                break;
            }
        }
    }

    if (mismatch)
    {
        ++s_diffMismatches;
    }
    else if (!s_diffPrintedMatch.exchange(true))
    {
        std::fprintf(stderr, "[vu1:diff] match pc=0x%x\n", startPc);
    }
    if ((diffRuns & 0xFFFu) == 0u)
    {
        // Also report current totals periodically: the frontend leaves through
        // std::_Exit(), so atexit() never runs on a normal game shutdown.
        reportVu1DiffTotals();
    }

    // Keep the game running on the new path's output: submit its packets once.
    for (const std::vector<uint8_t> &packet : newPackets)
    {
        if (m_activeMemory)
            m_activeMemory->submitGifPacket(GifPathId::Path1, packet.data(), static_cast<uint32_t>(packet.size()));
        else if (m_activeGs)
            m_activeGs->processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));
    }
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    resetScheduler();
    m_state.pc = startPC & microAddressMask();
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    // PS2X(diag-vu1-start-vi): show the VI state the VIF1 hand-off gave
    // the title render entries; the 0x140/0x150 loops read their pointers
    // from VI, so a stale value here shows up as a non-terminating loop.
    if (vu1TraceEnabled() && (m_state.pc == 0x140u || m_state.pc == 0x150u))
    {
        static int s_startViBudget = 120;
        if (s_startViBudget-- > 0)
        {
            std::fprintf(stderr, "[vu1:start] pc=0x%x top=0x%x itop=0x%x vi:", m_state.pc,
                         m_state.top, m_state.itop);
            for (uint32_t i = 1; i < 16u; ++i)
                std::fprintf(stderr, " %u=0x%x", i, static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[i])));
            std::fprintf(stderr, "\n");
        }
    }
    if (m_unit == Unit::VU1 && vu1DiffEnabled())
    {
        if (vu1DiffAotEnabled())
            runAotDifferential(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
        else
            runDifferential(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    }
    else
        run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    if (m_unit == Unit::VU1 && vu1DiffEnabled())
    {
        if (vu1DiffAotEnabled())
            runAotDifferential(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
        else
            runDifferential(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    }
    else
        run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeVuCode = vuCode;
    m_activeCodeSize = codeSize;
    m_activeGs = &gs;
    m_activeMemory = memory;
    m_budgetEnd = m_cycle + maxCycles;

    const int previousRoundingMode = std::fegetround();
    const bool useVuRounding = std::fesetround(FE_TOWARDZERO) == 0;
    const bool stats = vu1AotStatsEnabled();
    std::chrono::steady_clock::time_point statsStart{};
    if (stats)
        statsStart = std::chrono::steady_clock::now();
    bool compiledOnly = false;
    bool programEnded = false;
    m_traceActive = vu1TraceEnabled();
    m_traceInsns = 0;
    m_stopReason = "budget";
    m_traceStartPc = m_state.pc;
    VU1InventoryRecorder *inventory =
        (m_unit == Unit::VU1 && !vu1DiffEnabled()) ? VU1InventoryRecorder::instance() : nullptr;
    m_inventory = inventory;
    if (inventory != nullptr)
    {
        const uint64_t generation = memory != nullptr ? memory->getVU1CodeGeneration() : 0u;
        const uint64_t vsync = memory != nullptr
                                   ? memory->gs().vsyncTick.load(std::memory_order_relaxed)
                                   : 0u;
        inventory->beginRun(vuCode, codeSize, generation, m_state.pc, m_cycle, vsync);
    }
    const auto inventoryHostStart = std::chrono::steady_clock::now();
    if (m_traceActive)
    {
        std::fprintf(stderr, "[vu1:run] begin pc=0x%x top=0x%x itop=0x%x budget=%u\n",
                     m_state.pc, m_state.top, m_state.itop, maxCycles);
    }

    bool runInterpreter = true;
    const ps2x::vu1::CompiledProgram *compiled = findCompiledProgram(vuCode, codeSize, memory);
    if (compiled != nullptr)
    {
        bool entry = false;
        for (uint32_t index = 0; index < compiled->entryCount; ++index)
        {
            if (compiled->entries[index] == m_state.pc)
            {
                entry = true;
                break;
            }
        }
        if (entry)
        {
            logAotHit(compiled->programHash, m_state.pc);
            compiledOnly = true;
            const ps2x::vu1::AotResult result = compiled->run(*this);
            if (result == ps2x::vu1::AotResult::ProgramEnded)
            {
                programEnded = true;
                runInterpreter = false;
            }
            else if (result == ps2x::vu1::AotResult::Fallback)
            {
                compiledOnly = false;
                logAotMiss(compiled->programHash, m_state.pc);
            }
            else
            {
                if (result == ps2x::vu1::AotResult::BudgetEnded)
                    m_stopReason = "budget";
                else
                    m_stopReason = "stop";
                runInterpreter = false;
            }
        }
        else
        {
            logAotMiss(compiled->programHash, m_state.pc);
        }
    }
    else if (!m_aotDisabled && vu1AotEnabled())
    {
        logAotMiss(0u, m_state.pc);
    }

    while (runInterpreter && m_cycle < m_budgetEnd && !m_stopRequested)
    {
        const DecodedInstructionPair decoded =
            getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
        const ps2x::vu1::AotResult result = aotStepPair(decoded);
        if (result == ps2x::vu1::AotResult::ProgramEnded)
        {
            programEnded = true;
            break;
        }
        if (result != ps2x::vu1::AotResult::Continue)
            break;
    }

    if (programEnded)
    {
        flushPipelines();
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    if (m_traceActive)
    {
        std::fprintf(stderr, "[vu1:run] end pc=0x%x insns=%u cycles=%llu reason=%s xgkick=%s\n",
                     m_state.pc, m_traceInsns,
                     static_cast<unsigned long long>(m_cycle), m_stopReason,
                     m_xgkick.active ? "pending" : "idle");
    }
    m_state.cycles = m_cycle;
    if (inventory != nullptr)
    {
        const auto inventoryHostEnd = std::chrono::steady_clock::now();
        const uint64_t hostNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                inventoryHostEnd - inventoryHostStart)
                .count());
        inventory->endRun(m_state.pc, m_cycle, m_traceInsns, hostNs);
    }
    if (m_traceActive && m_activeVuData && m_activeVuDataSize >= 0x1e10u && m_state.pc == 0x200u)
    {
        uint64_t lo = 0, hi = 0;
        std::memcpy(&lo, m_activeVuData + 0x1e00u, 8u);
        std::memcpy(&hi, m_activeVuData + 0x1e08u, 8u);
        std::fprintf(stderr, "[vu1:dump] after pc=0x200 q1e0=%016llx %016llx\n",
                     static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));
    }
    m_inventory = nullptr;
    if (stats)
    {
        const uint64_t hostNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - statsStart)
                .count());
        addAotStats(compiledOnly, hostNs, m_traceInsns);
    }
    if (useVuRounding && previousRoundingMode != -1)
        std::fesetround(previousRoundingMode);
}
