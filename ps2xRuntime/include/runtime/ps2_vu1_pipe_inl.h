// PS2X(vu1-aot-v2): hot pipeline enqueue helpers.
//
// Moved out of ps2_vu1_core.cpp so generated aotStepConstant<kPc, Pair>()
// instantiations can inline them with constant register/lane/latency values.
#ifndef PS2_VU1_PIPE_INL_H
#define PS2_VU1_PIPE_INL_H

#include <algorithm>
#include <bit>

PS2X_VU1_INLINE void VU1Interpreter::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    const uint32_t slot = static_cast<uint32_t>(std::countr_zero(
        static_cast<uint16_t>(~m_storeValid) & ((1u << kMaxPendingStores) - 1u)));
    if (slot < kMaxPendingStores)
    {
        PendingStore &store = m_storePipeline[slot];
        m_storeValid = static_cast<uint16_t>(m_storeValid | (1u << slot));
        store.valid = true;
        store.readyCycle = m_cycle + 1u;
        m_nextCommitCycle = std::min(m_nextCommitCycle, store.readyCycle);
        m_nextStoreCycle = std::min(m_nextStoreCycle, store.readyCycle);
        store.address = address;
        store.laneMask = laneMask;
        std::copy(words, words + 4, store.words.begin());
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFFCu);
}

PS2X_VU1_INLINE void VU1Interpreter::queueVfWrite(uint8_t reg, uint8_t laneMask,
                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    const uint32_t slot = static_cast<uint32_t>(std::countr_zero(
        static_cast<uint16_t>(~m_vfValid) & ((1u << kMaxPendingVfWrites) - 1u)));
    if (slot < kMaxPendingVfWrites)
    {
        PendingVfWrite &write = m_vfWritePipeline[slot];
        m_vfValid = static_cast<uint16_t>(m_vfValid | (1u << slot));
        write = {};
        write.valid = true;
        write.readyCycle = m_cycle + latency;
        m_nextCommitCycle = std::min(m_nextCommitCycle, write.readyCycle);
        m_nextVfCycle = std::min(m_nextVfCycle, write.readyCycle);
        write.sequence = ++m_nextWriteSequence;
        write.reg = reg;
        write.laneMask = laneMask;
        std::copy(value, value + 4, write.value.begin());
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((laneMask & lane(component)) != 0u)
                m_vfLatestWrite[reg][component] = write.sequence;
        }
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFF7u);
}

PS2X_VU1_INLINE void VU1Interpreter::queueViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    const uint32_t slot = static_cast<uint32_t>(std::countr_zero(
        static_cast<uint16_t>(~m_viValid) & ((1u << kMaxPendingViWrites) - 1u)));
    if (slot < kMaxPendingViWrites)
    {
        PendingViWrite &write = m_viWritePipeline[slot];
        m_viValid = static_cast<uint16_t>(m_viValid | (1u << slot));
        write = {};
        write.valid = true;
        write.readyCycle = m_cycle + latency;
        m_nextCommitCycle = std::min(m_nextCommitCycle, write.readyCycle);
        m_nextViCycle = std::min(m_nextViCycle, write.readyCycle);
        write.sequence = ++m_nextWriteSequence;
        write.reg = reg;
        write.value = value;
        m_viLatestWrite[reg] = write.sequence;
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFF6u);
}

PS2X_VU1_INLINE void VU1Interpreter::queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    const uint32_t slot = static_cast<uint32_t>(std::countr_zero(
        static_cast<uint16_t>(~m_accValid) & ((1u << kMaxPendingAccWrites) - 1u)));
    if (slot < kMaxPendingAccWrites)
    {
        PendingAccWrite &write = m_accWritePipeline[slot];
        m_accValid = static_cast<uint16_t>(m_accValid | (1u << slot));
        write = {};
        write.valid = true;
        write.readyCycle = m_cycle + latency;
        m_nextCommitCycle = std::min(m_nextCommitCycle, write.readyCycle);
        m_nextAccCycle = std::min(m_nextAccCycle, write.readyCycle);
        write.sequence = ++m_nextWriteSequence;
        write.laneMask = laneMask;
        std::copy(value, value + 4, write.value.begin());
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((laneMask & lane(component)) != 0u)
                m_accLatestWrite[component] = write.sequence;
        }
        return;
    }
    reportReservedInstruction(true, 0xFFFFFFF5u);
}

#endif
