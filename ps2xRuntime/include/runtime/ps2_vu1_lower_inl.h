// SUIKO(vu1-aot-v2): compile-time lower-instruction execution.
//
// execLowerConstant<kPc, L>() mirrors VU1Interpreter::execLower() with the
// instruction word (and therefore every field, immediate and branch
// displacement) known at compile time.  Uncommon encodings fall back to the
// generic routine, which stays the single source of truth for their semantics.
#ifndef PS2_VU1_LOWER_INL_H
#define PS2_VU1_LOWER_INL_H

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace suiko::vu1::detail
{
    inline float vuEatan(float value)
    {
        constexpr float coefficients[] = {
            0.999999344348907f,
            -0.333298563957214f,
            0.199465364217758f,
            -0.13085337519646f,
            0.096420042216778f,
            -0.055909886956215f,
            0.021861229091883f,
            -0.004054057877511f};
        constexpr float quarterPi = 0.785398185253143f;

        const float squared = value * value;
        float polynomial = coefficients[7];
        for (int index = 6; index >= 0; --index)
            polynomial = coefficients[index] + squared * polynomial;
        return quarterPi + value * polynomial;
    }

    inline float vuEsin(float value)
    {
        constexpr float coefficients[] = {
            1.0f,
            -0.166666567325592f,
            0.008333025500178f,
            -0.000198074136279f,
            0.000002601886990f};

        const float squared = value * value;
        float polynomial = coefficients[4];
        for (int index = 3; index >= 0; --index)
            polynomial = coefficients[index] + squared * polynomial;
        return value * polynomial;
    }

    inline float vuEexp(float value)
    {
        constexpr float coefficients[] = {
            0.249998688697815f,
            0.031257584691048f,
            0.002591371303424f,
            0.000171562001924f,
            0.000005430199963f,
            0.000000690600018f};

        float polynomial = coefficients[5];
        for (int index = 4; index >= 0; --index)
            polynomial = coefficients[index] + value * polynomial;
        polynomial = 1.0f + value * polynomial;
        polynomial *= polynomial;
        polynomial *= polynomial;
        return polynomial != 0.0f ? 1.0f / polynomial : std::numeric_limits<float>::max();
    }

    inline bool vu1TraceEnabled()
    {
        static const bool s_enabled = std::getenv("PS2X_VU1_TRACE") != nullptr;
        return s_enabled;
    }
}

template <uint32_t kPc, uint32_t L>
SUIKO_VU1_INLINE void VU1Interpreter::execLowerConstant(uint8_t *vuData, uint32_t dataSize,
                                                       GS &gs, PS2Memory *memory, uint32_t upperInstr)
{
    (void)upperInstr;
    if constexpr (L == 0x00000000u || L == 0x8000033Cu) // NOP
        return;

    constexpr uint8_t opHi = static_cast<uint8_t>((L >> 25) & 0x7Fu);
    const uint32_t pcMask = microAddressMask();

    if constexpr (opHi == 0x00u) // LQ
    {
        constexpr uint8_t it = FT(L);
        constexpr uint8_t is = VIS(L);
        constexpr uint8_t dest = static_cast<uint8_t>((L >> 21) & 0xF);
        constexpr int16_t imm = IMM11(L);
        const uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is] + imm)) * 16u & (dataSize - 1);
        if (suiko::vu1::detail::vu1TraceEnabled() && (addr == 0x1e00u || addr == 0x3e00u || addr == 0x5e00u))
            std::fprintf(stderr, "[vu1:load] LQ pc=0x%x addr=0x%x vf=%u mask=0x%x\n",
                         kPc, addr, it, dest);
        if (addr + 16 <= dataSize)
        {
            float tmp[4];
            std::memcpy(tmp, vuData + addr, 16);
            applyDest(m_state.vf[it], tmp, dest);
        }
        return;
    }
    else if constexpr (opHi == 0x01u) // SQ
    {
        constexpr uint8_t is = FS(L);
        constexpr uint8_t it = VIT(L);
        constexpr uint8_t dest = static_cast<uint8_t>((L >> 21) & 0xF);
        constexpr int16_t imm = IMM11(L);
        const uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[it] + imm)) * 16u & (dataSize - 1);
        if (suiko::vu1::detail::vu1TraceEnabled() && (addr == 0x1e00u || addr == 0x3e00u || addr == 0x5e00u))
            std::fprintf(stderr, "[vu1:store] SQ pc=0x%x addr=0x%x vf=%u mask=0x%x\n",
                         kPc, addr, is, dest);
        if (addr + 16 <= dataSize)
        {
            uint32_t words[4]{};
            std::memcpy(words, m_state.vf[is], sizeof(words));
            queueStore(addr, words, dest);
        }
        return;
    }
    else if constexpr (opHi == 0x04u) // ILW
    {
        constexpr uint8_t it = VIT(L);
        constexpr uint8_t is = VIS(L);
        constexpr uint8_t dest = static_cast<uint8_t>((L >> 21) & 0xF);
        constexpr int16_t imm = IMM11(L);
        const uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is] + imm)) * 16u & (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            constexpr int comp = (dest & 0x8) ? 0 : (dest & 0x4) ? 1 : (dest & 0x2) ? 2 : 3;
            uint32_t v;
            std::memcpy(&v, vuData + addr + comp * 4, 4);
            if (it != 0)
                m_state.vi[it] = (int32_t)(int16_t)(v & 0xFFFF);
        }
        return;
    }
    else if constexpr (opHi == 0x05u) // ISW
    {
        constexpr uint8_t it = VIT(L);
        constexpr uint8_t is = VIS(L);
        constexpr uint8_t dest = static_cast<uint8_t>((L >> 21) & 0xF);
        constexpr int16_t imm = IMM11(L);
        const uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is] + imm)) * 16u & (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            const uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[it] & 0xFFFF));
            const uint32_t words[4] = {val, val, val, val};
            queueStore(addr, words, dest);
        }
        return;
    }
    else if constexpr (opHi == 0x08u) // IADDIU
    {
        constexpr uint8_t it = VIT(L);
        constexpr uint8_t is = VIS(L);
        constexpr int16_t imm = (int16_t)(L & 0x7FF) | ((L >> 10) & 0x7800);
        if (it != 0)
            m_state.vi[it] = (int16_t)(m_state.vi[is] + imm);
        return;
    }
    else if constexpr (opHi == 0x09u) // ISUBIU
    {
        constexpr uint8_t it = VIT(L);
        constexpr uint8_t is = VIS(L);
        constexpr int16_t imm = (int16_t)(L & 0x7FF) | ((L >> 10) & 0x7800);
        if (it != 0)
            m_state.vi[it] = (int16_t)(m_state.vi[is] - imm);
        return;
    }
    else if constexpr (opHi == 0x10u) // FCEQ
    {
        constexpr uint32_t imm24 = L & 0xFFFFFF;
        m_state.vi[1] = ((m_state.clip & 0xFFFFFF) == imm24) ? 1 : 0;
        return;
    }
    else if constexpr (opHi == 0x11u) // FCSET
    {
        queueFcset(L & 0xFFFFFFu);
        return;
    }
    else if constexpr (opHi == 0x12u) // FCAND
    {
        constexpr uint32_t imm24 = L & 0xFFFFFF;
        m_state.vi[1] = ((m_state.clip & imm24) != 0) ? 1 : 0;
        return;
    }
    else if constexpr (opHi == 0x13u) // FCOR
    {
        constexpr uint32_t imm24 = L & 0xFFFFFF;
        m_state.vi[1] = ((m_state.clip | imm24) == 0xFFFFFF) ? 1 : 0;
        return;
    }
    else if constexpr (opHi == 0x14u) // FSEQ
    {
        constexpr uint8_t it = VIT(L);
        constexpr uint16_t imm12 = static_cast<uint16_t>((((L >> 21) & 0x1u) << 11) | (L & 0x7FFu));
        if (it != 0)
            m_state.vi[it] = ((m_state.status & 0xFFFu) == imm12) ? 1 : 0;
        return;
    }
    else if constexpr (opHi == 0x15u) // FSSET
    {
        constexpr uint16_t imm12 = static_cast<uint16_t>((((L >> 21) & 0x1u) << 11) | (L & 0x7FFu));
        queueFsset(imm12);
        return;
    }
    else if constexpr (opHi == 0x16u) // FSAND
    {
        constexpr uint8_t it = VIT(L);
        constexpr uint16_t imm12 = static_cast<uint16_t>((((L >> 21) & 0x1u) << 11) | (L & 0x7FFu));
        if (it != 0)
            m_state.vi[it] = static_cast<int32_t>((m_state.status & 0xFFFu) & imm12);
        return;
    }
    else if constexpr (opHi == 0x17u) // FSOR
    {
        constexpr uint8_t it = VIT(L);
        constexpr uint16_t imm12 = static_cast<uint16_t>((((L >> 21) & 0x1u) << 11) | (L & 0x7FFu));
        if (it != 0)
            m_state.vi[it] = static_cast<int32_t>((m_state.status & 0xFFFu) | imm12);
        return;
    }
    else if constexpr (opHi == 0x18u) // FMEQ
    {
        constexpr uint8_t it = VIT(L);
        constexpr uint8_t is = VIS(L);
        if (it != 0)
            m_state.vi[it] = ((m_state.mac & 0xFFFF) == (uint32_t)(uint16_t)m_state.vi[is]) ? 1 : 0;
        return;
    }
    else if constexpr (opHi == 0x1Au) // FMAND
    {
        constexpr uint8_t it = VIT(L);
        constexpr uint8_t is = VIS(L);
        if (it != 0)
            m_state.vi[it] = (int32_t)(m_state.mac & (uint32_t)(uint16_t)m_state.vi[is]);
        return;
    }
    else if constexpr (opHi == 0x1Bu) // FMOR
    {
        constexpr uint8_t it = VIT(L);
        constexpr uint8_t is = VIS(L);
        if (it != 0)
            m_state.vi[it] = (int32_t)(m_state.mac | (uint32_t)(uint16_t)m_state.vi[is]);
        return;
    }
    else if constexpr (opHi == 0x1Cu) // FCGET
    {
        constexpr uint8_t it = VIT(L);
        if (it != 0)
            m_state.vi[it] = static_cast<int32_t>(m_state.clip & 0x0FFFu);
        return;
    }
    else if constexpr (opHi == 0x20u) // B
    {
        constexpr int16_t imm = IMM11(L);
        const uint32_t target = (kPc + 8 + imm * 8) & pcMask;
        m_state.branchPending = true;
        m_state.branchTarget = target;
        m_state.branchDelay = 1;
        return;
    }
    else if constexpr (opHi == 0x21u) // BAL
    {
        constexpr uint8_t it = VIT(L);
        constexpr int16_t imm = IMM11(L);
        const uint32_t target = (kPc + 8 + imm * 8) & pcMask;
        if (it != 0)
            m_state.vi[it] = (int32_t)((kPc + 16) / 8);
        m_state.branchPending = true;
        m_state.branchTarget = target;
        m_state.branchDelay = 1;
        return;
    }
    else if constexpr (opHi == 0x24u) // JR
    {
        constexpr uint8_t is = VIS(L);
        const uint32_t target = ((uint32_t)(uint16_t)m_state.vi[is] * 8u) & pcMask;
        m_state.branchPending = true;
        m_state.branchTarget = target;
        m_state.branchDelay = 1;
        return;
    }
    else if constexpr (opHi == 0x25u) // JALR
    {
        constexpr uint8_t it = VIT(L);
        constexpr uint8_t is = VIS(L);
        const uint32_t target = ((uint32_t)(uint16_t)m_state.vi[is] * 8u) & pcMask;
        if (it != 0)
            m_state.vi[it] = (int32_t)((kPc + 16) / 8);
        m_state.branchPending = true;
        m_state.branchTarget = target;
        m_state.branchDelay = 1;
        return;
    }
    else if constexpr (opHi >= 0x28u && opHi <= 0x2Fu) // IB*
    {
        constexpr uint8_t it = VIT(L);
        constexpr uint8_t is = VIS(L);
        constexpr int16_t imm = IMM11(L);
        bool take = false;
        if constexpr (opHi == 0x28u)
            take = (int16_t)readBranchVi(is) == (int16_t)readBranchVi(it);
        else if constexpr (opHi == 0x29u)
            take = (int16_t)readBranchVi(is) != (int16_t)readBranchVi(it);
        else if constexpr (opHi == 0x2Cu)
            take = (int16_t)readBranchVi(is) < 0;
        else if constexpr (opHi == 0x2Du)
            take = (int16_t)readBranchVi(is) > 0;
        else if constexpr (opHi == 0x2Eu)
            take = (int16_t)readBranchVi(is) <= 0;
        else if constexpr (opHi == 0x2Fu)
            take = (int16_t)readBranchVi(is) >= 0;
        if (take)
        {
            const uint32_t target = (kPc + 8 + imm * 8) & pcMask;
            m_state.branchPending = true;
            m_state.branchTarget = target;
            m_state.branchDelay = 1;
        }
        return;
    }
    else if constexpr (opHi == 0x40u)
    {
        constexpr uint8_t funct = L & 0x3Fu;
        constexpr uint8_t vfT = FT(L);
        constexpr uint8_t vfS = FS(L);
        constexpr uint8_t viT = VIT(L);
        constexpr uint8_t viS = VIS(L);
        constexpr uint8_t viD = VID(L);
        constexpr uint8_t dest = static_cast<uint8_t>((L >> 21) & 0xF);

        if constexpr (funct == 0x30u) // IADD
        {
            if (viD != 0)
                m_state.vi[viD] = (int16_t)(m_state.vi[viS] + m_state.vi[viT]);
            return;
        }
        else if constexpr (funct == 0x31u) // ISUB
        {
            if (viD != 0)
                m_state.vi[viD] = (int16_t)(m_state.vi[viS] - m_state.vi[viT]);
            return;
        }
        else if constexpr (funct == 0x32u) // IADDI
        {
            constexpr int16_t imm5 = (int16_t)((int32_t)((L >> 6) & 0x1F) << 27 >> 27);
            if (viT != 0)
                m_state.vi[viT] = (int16_t)(m_state.vi[viS] + imm5);
            return;
        }
        else if constexpr (funct == 0x34u) // IAND
        {
            if (viD != 0)
                m_state.vi[viD] = m_state.vi[viS] & m_state.vi[viT];
            return;
        }
        else if constexpr (funct == 0x35u) // IOR
        {
            if (viD != 0)
                m_state.vi[viD] = m_state.vi[viS] | m_state.vi[viT];
            return;
        }
        else if constexpr (funct >= 0x3Cu)
        {
            constexpr uint8_t funct2 = static_cast<uint8_t>((L & 0x3u) | ((L >> 4) & 0x7Cu));
            if constexpr (funct2 == 0x30u) // MOVE
            {
                float tmp[4];
                std::memcpy(tmp, m_state.vf[vfS], 16);
                applyDest(m_state.vf[vfT], tmp, dest);
                return;
            }
            else if constexpr (funct2 == 0x31u) // MR32
            {
                float tmp[4] = {m_state.vf[vfS][1], m_state.vf[vfS][2], m_state.vf[vfS][3], m_state.vf[vfS][0]};
                applyDest(m_state.vf[vfT], tmp, dest);
                return;
            }
            else if constexpr (funct2 == 0x34u) // LQI
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[viS]) * 16u & (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    applyDest(m_state.vf[vfT], tmp, dest);
                }
                if (viS != 0)
                    m_state.vi[viS] = (int16_t)(m_state.vi[viS] + 1);
                return;
            }
            else if constexpr (funct2 == 0x35u) // SQI
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[viT]) * 16u & (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    uint32_t words[4]{};
                    std::memcpy(words, m_state.vf[vfS], sizeof(words));
                    queueStore(addr, words, dest);
                }
                if (viT != 0)
                    m_state.vi[viT] = (int16_t)(m_state.vi[viT] + 1);
                return;
            }
            else if constexpr (funct2 == 0x36u) // LQD
            {
                if (viS != 0)
                    m_state.vi[viS] = (int16_t)(m_state.vi[viS] - 1);
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[viS]) * 16u & (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    applyDest(m_state.vf[vfT], tmp, dest);
                }
                return;
            }
            else if constexpr (funct2 == 0x37u) // SQD
            {
                if (viT != 0)
                    m_state.vi[viT] = (int16_t)(m_state.vi[viT] - 1);
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[viT]) * 16u & (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    uint32_t words[4]{};
                    std::memcpy(words, m_state.vf[vfS], sizeof(words));
                    queueStore(addr, words, dest);
                }
                return;
            }
            else if constexpr (funct2 == 0x38u) // DIV
            {
                constexpr int fsf = (L >> 21) & 0x3;
                constexpr int ftf = (L >> 23) & 0x3;
                const float num = normalizeOperand(m_state.vf[vfS][fsf]);
                const float den = normalizeOperand(m_state.vf[vfT][ftf]);
                uint32_t statusDi = 0u;
                float result = 0.0f;
                if (den == 0.0f)
                {
                    statusDi = num == 0.0f ? 0x10u : 0x20u;
                    result = std::signbit(num) != std::signbit(den)
                                 ? -std::numeric_limits<float>::max()
                                 : std::numeric_limits<float>::max();
                }
                else
                {
                    result = num / den;
                }
                uint32_t ignoredFlags = 0u;
                result = normalizeResult(result, ignoredFlags);
                queueQ(result, 7u, statusDi);
                return;
            }
            else if constexpr (funct2 == 0x39u) // SQRT
            {
                constexpr int ftf = (L >> 23) & 0x3;
                const float val = normalizeOperand(m_state.vf[vfT][ftf]);
                queueQ(std::sqrt(std::fabs(val)), 7u, val < 0.0f ? 0x10u : 0u);
                return;
            }
            else if constexpr (funct2 == 0x3Au) // RSQRT
            {
                constexpr int fsf = (L >> 21) & 0x3;
                constexpr int ftf = (L >> 23) & 0x3;
                const float num = normalizeOperand(m_state.vf[vfS][fsf]);
                const float radicand = normalizeOperand(m_state.vf[vfT][ftf]);
                const float den = std::sqrt(std::fabs(radicand));
                uint32_t statusDi = radicand < 0.0f ? 0x10u : 0u;
                float result = 0.0f;
                if (den != 0.0f)
                    result = num / den;
                else
                {
                    statusDi = num == 0.0f ? 0x10u : 0x20u;
                    result = std::signbit(num)
                                 ? -std::numeric_limits<float>::max()
                                 : std::numeric_limits<float>::max();
                }
                uint32_t ignoredFlags = 0u;
                result = normalizeResult(result, ignoredFlags);
                queueQ(result, 13u, statusDi);
                return;
            }
            else if constexpr (funct2 == 0x3Bu) // WAITQ
            {
                return;
            }
            else if constexpr (funct2 == 0x3Cu) // MTIR
            {
                constexpr uint32_t comp = (L >> 21) & 0x3u;
                uint32_t fval;
                std::memcpy(&fval, &m_state.vf[vfS][comp], 4);
                if (viT != 0)
                    m_state.vi[viT] = (int32_t)(int16_t)(fval & 0xFFFF);
                return;
            }
            else if constexpr (funct2 == 0x3Du) // MFIR
            {
                float result[4];
                const int32_t val = (int32_t)(int16_t)(m_state.vi[viS] & 0xFFFF);
                std::memcpy(&result[0], &val, 4);
                result[1] = result[0];
                result[2] = result[0];
                result[3] = result[0];
                applyDest(m_state.vf[vfT], result, dest);
                return;
            }
            else if constexpr (funct2 == 0x3Eu) // ILWR
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[viS]) * 16u & (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    constexpr int comp = (dest & 0x8) ? 0 : (dest & 0x4) ? 1 : (dest & 0x2) ? 2 : 3;
                    uint32_t v;
                    std::memcpy(&v, vuData + addr + comp * 4, 4);
                    if (viT != 0)
                        m_state.vi[viT] = (int32_t)(int16_t)(v & 0xFFFF);
                }
                return;
            }
            else if constexpr (funct2 == 0x3Fu) // ISWR
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[viS]) * 16u & (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    const uint32_t val =
                        static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[viT] & 0xFFFF));
                    const uint32_t words[4] = {val, val, val, val};
                    queueStore(addr, words, dest);
                }
                return;
            }
            else if constexpr (funct2 == 0x40u) // RNEXT
            {
                const uint32_t x = (m_state.r >> 4) & 1u;
                const uint32_t y = (m_state.r >> 22) & 1u;
                m_state.r = ((m_state.r << 1) ^ x ^ y) & 0x007FFFFFu;
                m_state.r |= 0x3F800000u;
                float value = 0.0f;
                std::memcpy(&value, &m_state.r, sizeof(value));
                const float result[4] = {value, value, value, value};
                applyDest(m_state.vf[vfT], result, dest);
                return;
            }
            else if constexpr (funct2 == 0x41u) // RGET
            {
                float value = 0.0f;
                std::memcpy(&value, &m_state.r, sizeof(value));
                const float result[4] = {value, value, value, value};
                applyDest(m_state.vf[vfT], result, dest);
                return;
            }
            else if constexpr (funct2 == 0x42u) // RINIT
            {
                constexpr uint32_t component = (L >> 21) & 3u;
                uint32_t bits = 0u;
                std::memcpy(&bits, &m_state.vf[vfS][component], sizeof(bits));
                m_state.r = 0x3F800000u | (bits & 0x007FFFFFu);
                return;
            }
            else if constexpr (funct2 == 0x43u) // RXOR
            {
                constexpr uint32_t component = (L >> 21) & 3u;
                uint32_t bits = 0u;
                std::memcpy(&bits, &m_state.vf[vfS][component], sizeof(bits));
                m_state.r = 0x3F800000u | ((m_state.r ^ bits) & 0x007FFFFFu);
                return;
            }
            else if constexpr (funct2 == 0x64u) // MFP
            {
                float result[4] = {m_state.p, m_state.p, m_state.p, m_state.p};
                applyDest(m_state.vf[vfT], result, dest);
                return;
            }
            else if constexpr (funct2 == 0x68u) // XTOP
            {
                if (viT != 0)
                    m_state.vi[viT] = (int32_t)(m_state.top & 0x3FFu);
                return;
            }
            else if constexpr (funct2 == 0x69u) // XITOP
            {
                if (viT != 0)
                    m_state.vi[viT] = (int32_t)(m_state.itop & 0x3FFu);
                return;
            }
            else if constexpr (funct2 == 0x6Cu) // XGKICK
            {
                startXgkick(static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[viS])));
                return;
            }
            else if constexpr (funct2 == 0x70u) // ESADD
            {
                const float x = normalizeOperand(m_state.vf[vfS][0]);
                const float y = normalizeOperand(m_state.vf[vfS][1]);
                const float z = normalizeOperand(m_state.vf[vfS][2]);
                queueP(x * x + y * y + z * z, 11u);
                return;
            }
            else if constexpr (funct2 == 0x71u) // ERSADD
            {
                const float x = normalizeOperand(m_state.vf[vfS][0]);
                const float y = normalizeOperand(m_state.vf[vfS][1]);
                const float z = normalizeOperand(m_state.vf[vfS][2]);
                const float sum = x * x + y * y + z * z;
                queueP(sum != 0.0f ? 1.0f / sum : sum, 18u);
                return;
            }
            else if constexpr (funct2 == 0x72u) // ELENG
            {
                const float x = normalizeOperand(m_state.vf[vfS][0]);
                const float y = normalizeOperand(m_state.vf[vfS][1]);
                const float z = normalizeOperand(m_state.vf[vfS][2]);
                queueP(std::sqrt(x * x + y * y + z * z), 18u);
                return;
            }
            else if constexpr (funct2 == 0x73u) // ERLENG
            {
                const float x = normalizeOperand(m_state.vf[vfS][0]);
                const float y = normalizeOperand(m_state.vf[vfS][1]);
                const float z = normalizeOperand(m_state.vf[vfS][2]);
                const float len = std::sqrt(x * x + y * y + z * z);
                queueP(len != 0.0f ? 1.0f / len : len, 24u);
                return;
            }
            else if constexpr (funct2 == 0x74u) // EATANxy
            {
                const float x = normalizeOperand(m_state.vf[vfS][0]);
                const float y = normalizeOperand(m_state.vf[vfS][1]);
                queueP(x != 0.0f ? suiko::vu1::detail::vuEatan(y / x) : 0.0f, 54u);
                return;
            }
            else if constexpr (funct2 == 0x75u) // EATANxz
            {
                const float x = normalizeOperand(m_state.vf[vfS][0]);
                const float z = normalizeOperand(m_state.vf[vfS][2]);
                queueP(x != 0.0f ? suiko::vu1::detail::vuEatan(z / x) : 0.0f, 54u);
                return;
            }
            else if constexpr (funct2 == 0x76u) // ESUM
            {
                float sum = 0.0f;
                for (uint32_t component = 0; component < 4u; ++component)
                    sum += normalizeOperand(m_state.vf[vfS][component]);
                queueP(sum, 12u);
                return;
            }
            else if constexpr (funct2 == 0x77u) // ERSQRT
            {
                constexpr uint32_t component = (L >> 21) & 3u;
                const float value = normalizeOperand(m_state.vf[vfS][component]);
                float result = value;
                if (result >= 0.0f)
                {
                    result = std::sqrt(result);
                    if (result != 0.0f)
                        result = 1.0f / result;
                }
                queueP(result, 18u);
                return;
            }
            else if constexpr (funct2 == 0x78u) // ESQRT
            {
                constexpr uint32_t component = (L >> 21) & 3u;
                const float value = normalizeOperand(m_state.vf[vfS][component]);
                queueP(value >= 0.0f ? std::sqrt(value) : value, 12u);
                return;
            }
            else if constexpr (funct2 == 0x79u) // ESIN
            {
                constexpr uint32_t component = (L >> 21) & 3u;
                const float value = normalizeOperand(m_state.vf[vfS][component]);
                queueP(suiko::vu1::detail::vuEsin(value), 29u);
                return;
            }
            else if constexpr (funct2 == 0x7Au) // ERCPR
            {
                constexpr uint32_t component = (L >> 21) & 3u;
                const float value = normalizeOperand(m_state.vf[vfS][component]);
                queueP(value != 0.0f ? 1.0f / value : value, 12u);
                return;
            }
            else if constexpr (funct2 == 0x7Bu) // WAITP
            {
                return;
            }
            else if constexpr (funct2 == 0x7Cu) // EATAN
            {
                constexpr uint32_t component = (L >> 21) & 3u;
                queueP(suiko::vu1::detail::vuEatan(normalizeOperand(m_state.vf[vfS][component])), 54u);
                return;
            }
            else if constexpr (funct2 == 0x7Du) // EEXP
            {
                constexpr uint32_t component = (L >> 21) & 3u;
                queueP(suiko::vu1::detail::vuEexp(normalizeOperand(m_state.vf[vfS][component])), 44u);
                return;
            }
            else
            {
                execLower(L, vuData, dataSize, gs, memory, upperInstr);
                return;
            }
        }
        else
        {
            execLower(L, vuData, dataSize, gs, memory, upperInstr);
            return;
        }
    }
    else
    {
        execLower(L, vuData, dataSize, gs, memory, upperInstr);
        return;
    }
}

#endif
