// PS2X(vu1-aot-v2): upper-instruction execution visible to generated code.
//
// These are the exact functions from ps2_vu1_upper.cpp / ps2_vu1_core.cpp,
// moved into the header so a generated aotStepConstant<kPc, Pair>() call can
// inline them with the instruction word as a compile-time constant.  The
// interpreter's generic path calls the same definitions, so results cannot
// diverge.
#ifndef PS2_VU1_UPPER_INL_H
#define PS2_VU1_UPPER_INL_H

#include <cmath>
#include <cstring>
#include <limits>

// The recompiler's instructions.h defines FT/FS/FD as macros; a file that
// includes both (the tests do) must not have them expand inside the VU1
// field helpers.
#pragma push_macro("FT")
#pragma push_macro("FS")
#pragma push_macro("FD")
#undef FT
#undef FS
#undef FD
#include "../../src/lib/vu/ps2_vu1_detail.h"
#pragma pop_macro("FD")
#pragma pop_macro("FS")
#pragma pop_macro("FT")

#if defined(__GNUC__) || defined(__clang__)
#define PS2X_VU1_INLINE inline __attribute__((always_inline))
#else
#define PS2X_VU1_INLINE inline
#endif


namespace ps2x::vu1::detail
{
    inline int32_t vuFloatToInt(float value, float scale)
    {
        const double scaled = static_cast<double>(value) * static_cast<double>(scale);
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }

    // Fast equivalence test for the long-double FMAC result check.  Operands
    // are always normalized (no denormals, infinities or NaNs), so a result
    // whose magnitude is a normal float strictly below FLT_MAX can neither
    // overflow nor underflow: the IEEE result is in range, its sign matches
    // the exact result and every condition flag is zero.  Only the extreme
    // magnitudes need the exact long-double path.
    inline bool fmacResultNeedsExact(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t magnitude = bits & 0x7FFFFFFFu;
        return magnitude == 0u || magnitude < 0x00800000u || magnitude >= 0x7F7FFFFFu;
    }
}

PS2X_VU1_INLINE float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    return normalizeOperand(vf[bc & 3u]);
}

PS2X_VU1_INLINE float VU1Interpreter::normalizeResult(float value, uint32_t &laneFlags) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;

    laneFlags = sign != 0u ? 0x2u : 0u;
    if (magnitude == 0u)
    {
        laneFlags |= 0x1u;
    }
    else if (exponent == 0u)
    {
        laneFlags |= 0x5u;
        bits = sign;
    }
    else if (exponent == 0xFFu)
    {
        laneFlags |= 0x8u;
        bits = sign | 0x7F7FFFFFu;
    }

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

PS2X_VU1_INLINE void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    if (dest & 0x8u)
        dst[0] = result[0];
    if (dest & 0x4u)
        dst[1] = result[1];
    if (dest & 0x2u)
        dst[2] = result[2];
    if (dest & 0x1u)
        dst[3] = result[3];
}

PS2X_VU1_INLINE void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

inline uint8_t VU1Interpreter::normalizeFmacExactResult(float &value,
                                                        long double exactResult) const
{
    const bool negative = std::signbit(exactResult);
    const long double magnitude = std::fabs(exactResult);
    const long double maximum = static_cast<long double>(std::numeric_limits<float>::max());
    const long double minimum = static_cast<long double>(std::numeric_limits<float>::min());
    uint8_t flags = negative ? 0x2u : 0u;

    uint32_t bits = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0L)
    {
        flags |= 0x1u;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude > maximum)
    {
        flags |= 0x8u;
        bits |= 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude < minimum)
    {
        flags |= 0x5u;
        std::memcpy(&value, &bits, sizeof(value));
    }

    return flags;
}

template <uint32_t U>
inline bool VU1Interpreter::calculateFmacExactResult(uint32_t component,
                                                     long double &result) const
{
    const uint32_t upper = upperWord<U>();
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu
                                ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                                : 0xFFu;
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);

    const auto operand = [this](float value)
    {
        return static_cast<long double>(normalizeOperand(value));
    };
    const auto vs = [&](uint32_t lane)
    {
        return operand(m_state.vf[fs][lane]);
    };
    const auto vt = [&](uint32_t lane)
    {
        return operand(m_state.vf[ft][lane]);
    };
    const auto acc = [&](uint32_t lane)
    {
        return operand(m_state.acc[lane]);
    };

    const long double q = operand(m_state.q);
    const long double i = operand(m_state.i);

    if (op < 0x3Cu)
    {
        if (op <= 0x03u)
            result = vs(component) + vt(op & 3u);
        else if (op <= 0x07u)
            result = vs(component) - vt(op & 3u);
        else if (op <= 0x0Bu)
            result = acc(component) + vs(component) * vt(op & 3u);
        else if (op <= 0x0Fu)
            result = acc(component) - vs(component) * vt(op & 3u);
        else if (op >= 0x18u && op <= 0x1Bu)
            result = vs(component) * vt(op & 3u);
        else
        {
            switch (op)
            {
            case 0x1Cu:
                result = vs(component) * q;
                break;
            case 0x1Eu:
                result = vs(component) * i;
                break;
            case 0x20u:
                result = vs(component) + q;
                break;
            case 0x21u:
                result = acc(component) + vs(component) * q;
                break;
            case 0x22u:
                result = vs(component) + i;
                break;
            case 0x23u:
                result = acc(component) + vs(component) * i;
                break;
            case 0x24u:
                result = vs(component) - q;
                break;
            case 0x25u:
                result = acc(component) - vs(component) * q;
                break;
            case 0x26u:
                result = vs(component) - i;
                break;
            case 0x27u:
                result = acc(component) - vs(component) * i;
                break;
            case 0x28u:
                result = vs(component) + vt(component);
                break;
            case 0x29u:
                result = acc(component) + vs(component) * vt(component);
                break;
            case 0x2Au:
                result = vs(component) * vt(component);
                break;
            case 0x2Cu:
                result = vs(component) - vt(component);
                break;
            case 0x2Du:
                result = acc(component) - vs(component) * vt(component);
                break;
            case 0x2Eu:
            {
                static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
                static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
                result = component == 3u
                             ? 0.0L
                             : acc(component) - vs(left[component]) * vt(right[component]);
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

    if (special <= 0x03u)
        result = vs(component) + vt(special & 3u);
    else if (special <= 0x07u)
        result = vs(component) - vt(special & 3u);
    else if (special <= 0x0Bu)
        result = acc(component) + vs(component) * vt(special & 3u);
    else if (special <= 0x0Fu)
        result = acc(component) - vs(component) * vt(special & 3u);
    else if (special >= 0x18u && special <= 0x1Bu)
        result = vs(component) * vt(special & 3u);
    else
    {
        switch (special)
        {
        case 0x1Cu:
            result = vs(component) * q;
            break;
        case 0x1Eu:
            result = vs(component) * i;
            break;
        case 0x20u:
            result = vs(component) + q;
            break;
        case 0x21u:
            result = acc(component) + vs(component) * q;
            break;
        case 0x22u:
            result = vs(component) + i;
            break;
        case 0x23u:
            result = acc(component) + vs(component) * i;
            break;
        case 0x24u:
            result = vs(component) - q;
            break;
        case 0x25u:
            result = acc(component) - vs(component) * q;
            break;
        case 0x26u:
            result = vs(component) - i;
            break;
        case 0x27u:
            result = acc(component) - vs(component) * i;
            break;
        case 0x28u:
            result = vs(component) + vt(component);
            break;
        case 0x29u:
            result = acc(component) + vs(component) * vt(component);
            break;
        case 0x2Au:
            result = vs(component) * vt(component);
            break;
        case 0x2Cu:
            result = vs(component) - vt(component);
            break;
        case 0x2Du:
            result = acc(component) - vs(component) * vt(component);
            break;
        case 0x2Eu:
        {
            static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            result = component == 3u
                         ? 0.0L
                         : vs(left[component]) * vt(right[component]);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

template <uint32_t U>
PS2X_VU1_INLINE void VU1Interpreter::normalizeFmacResult(float *result, uint8_t dest,
                                                uint8_t laneFlags[4])
{
    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & lane(component)) == 0u)
            continue;

        if (ps2x::vu1::detail::fmacResultNeedsExact(result[component]))
        {
            long double exactResult = 0.0L;
            if (calculateFmacExactResult<U>(component, exactResult))
            {
                laneFlags[component] = normalizeFmacExactResult(result[component], exactResult);
                continue;
            }
        }

        uint32_t bits = 0;
        std::memcpy(&bits, &result[component], sizeof(bits));
        laneFlags[component] = static_cast<uint8_t>((bits >> 31) != 0u ? 0x2u : 0x0u);
    }
}

template <uint32_t U>
PS2X_VU1_INLINE uint32_t VU1Interpreter::calculateFmacProductSticky(uint8_t dest) const
{
    uint32_t extraSticky = 0u;
    const uint32_t upper = upperWord<U>();
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : 0xFFu;
    const bool productSum =
        (op >= 0x08u && op <= 0x0Fu) ||
        op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
        op == 0x29u || op == 0x2Du || op == 0x2Eu ||
        (special >= 0x08u && special <= 0x0Fu) ||
        special == 0x21u || special == 0x23u || special == 0x25u ||
        special == 0x27u || special == 0x29u || special == 0x2Du;
    if (!productSum)
        return 0u;

    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & lane(component)) == 0u)
            continue;
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        const uint8_t leftComponent = op == 0x2Eu ? crossLeft[component] : static_cast<uint8_t>(component);
        const float left = normalizeOperand(m_state.vf[fs][leftComponent]);
        float right = 0.0f;
        if ((op >= 0x08u && op <= 0x0Fu) || (special >= 0x08u && special <= 0x0Fu))
        {
            right = normalizeOperand(m_state.vf[ft][(op >= 0x08u && op <= 0x0Fu ? op : special) & 3u]);
        }
        else if (op == 0x21u || op == 0x25u || special == 0x21u || special == 0x25u)
        {
            right = normalizeOperand(m_state.q);
        }
        else if (op == 0x23u || op == 0x27u || special == 0x23u || special == 0x27u)
        {
            right = normalizeOperand(m_state.i);
        }
        else if (op == 0x2Eu)
        {
            right = normalizeOperand(m_state.vf[ft][crossRight[component]]);
        }
        else
        {
            right = normalizeOperand(m_state.vf[ft][component]);
        }

        float product = left * right;
        uint8_t productFlags = 0u;
        if (ps2x::vu1::detail::fmacResultNeedsExact(product))
        {
            const long double exactProduct = static_cast<long double>(left) * static_cast<long double>(right);
            productFlags = normalizeFmacExactResult(product, exactProduct);
        }
        else
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &product, sizeof(bits));
            productFlags = static_cast<uint8_t>((bits >> 31) != 0u ? 0x2u : 0x0u);
        }
        // Product-sum instructions report Z/S/U/O from the add/subtract result
        // as current flags, while every product condition accumulates into the
        // corresponding sticky flag.
        extraSticky |= productFlags & 0xFu;
    }
    return extraSticky;
}

PS2X_VU1_INLINE void VU1Interpreter::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
                                            uint32_t extraSticky)
{
    if (dest == 0u)
        return;

    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t laneBit = lane(component);
        if ((dest & laneBit) == 0u)
            continue;

        const uint32_t flags = laneFlags[component];
        if ((flags & 0x1u) != 0u)
            mac |= laneBit;
        if ((flags & 0x2u) != 0u)
            mac |= static_cast<uint32_t>(laneBit) << 4;
        if ((flags & 0x4u) != 0u)
            mac |= static_cast<uint32_t>(laneBit) << 8;
        if ((flags & 0x8u) != 0u)
            mac |= static_cast<uint32_t>(laneBit) << 12;
        status |= flags;
    }

    const uint32_t slot = static_cast<uint32_t>(std::countr_zero(
        static_cast<uint16_t>(~m_flagValid) & ((1u << kMaxFlagEntries) - 1u)));
    if (slot >= kMaxFlagEntries)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }
    FlagPipelineEntry *entry = &m_flagPipeline[slot];
    m_flagValid = static_cast<uint16_t>(m_flagValid | (1u << slot));

    *entry = {};
    entry->valid = true;
    entry->issueCycle = m_cycle;
    entry->readyCycle = m_cycle + kFmacLatency;
    m_nextCommitCycle = std::min(m_nextCommitCycle, entry->readyCycle);
    m_nextFlagCycle = std::min(m_nextFlagCycle, entry->readyCycle);
    entry->mac = mac;
    entry->status = status;
    entry->extraSticky = extraSticky;
    entry->writesMac = true;
    entry->writesStatus = true;
}

template <uint32_t U>
PS2X_VU1_INLINE void VU1Interpreter::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult<U>(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky<U>(dest));
    applyDest(dst, result, dest);
}

template <uint32_t U>
PS2X_VU1_INLINE void VU1Interpreter::applyFmacDestAcc(float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult<U>(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky<U>(dest));
    applyDestAcc(result, dest);
}

inline void VU1Interpreter::execUpper(uint32_t instr)
{
    m_currentUpperInstruction = instr;
    uint8_t dest = DEST(instr);
    uint8_t ft = FT(instr);
    uint8_t fs = FS(instr);
    uint8_t fd = FD(instr);
    uint8_t op = instr & 0x3F;

    float *vd = m_state.vf[fd];
    float normalizedVs[4];
    float normalizedVt[4];
    float normalizedAcc[4];
    for (uint32_t component = 0; component < 4u; ++component)
    {
        normalizedVs[component] = normalizeOperand(m_state.vf[fs][component]);
        normalizedVt[component] = normalizeOperand(m_state.vf[ft][component]);
        normalizedAcc[component] = normalizeOperand(m_state.acc[component]);
    }
    const float *vs = normalizedVs;
    const float *vt = normalizedVt;
    const float *acc = normalizedAcc;
    const float q = normalizeOperand(m_state.q);
    const float i = normalizeOperand(m_state.i);
    float result[4];

    // Upper opcode decoding (bits 5:0 of upper word)
    switch (op)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03: // ADDbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07: // SUBbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B: // MADDbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x0F: // MSUBbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13: // MAXbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > bc) ? vs[c] : bc;
        applyDest(vd, result, dest);
        return;
    }
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17: // MINIbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < bc) ? vs[c] : bc;
        applyDest(vd, result, dest);
        return;
    }
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B: // MULbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x1C: // MULq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x1D: // MAXi
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > i) ? vs[c] : i;
        applyDest(vd, result, dest);
        return;
    case 0x1E: // MULi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x1F: // MINIi
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < i) ? vs[c] : i;
        applyDest(vd, result, dest);
        return;
    case 0x20: // ADDq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x21: // MADDq
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x22: // ADDi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x23: // MADDi
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x24: // SUBq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x25: // MSUBq
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x26: // SUBi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x27: // MSUBi
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x28: // ADD
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x29: // MADD
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x2A: // MUL
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x2B: // MAX
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > vt[c]) ? vs[c] : vt[c];
        applyDest(vd, result, dest);
        return;
    case 0x2C: // SUB
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x2D: // MSUB
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x2E: // OPMSUB
        result[0] = acc[0] - vs[1] * vt[2];
        result[1] = acc[1] - vs[2] * vt[0];
        result[2] = acc[2] - vs[0] * vt[1];
        result[3] = 0.0f;
        applyFmacDest(vd, result, dest);
        return;
    case 0x2F: // MINI
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < vt[c]) ? vs[c] : vt[c];
        applyDest(vd, result, dest);
        return;

    // Upper special group (low op 0x3C..0x3F).
    // Like lower1 special, the real selector is not just bits 5:0.  Dobie decodes:
    //   op = (instr & 0x3) | ((instr >> 4) & 0x7C)
    // Several instructions in this group also use FT as the destination, not FD.
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
    {
        const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
        float *vtDest = m_state.vf[ft];

        switch (specialOp)
        {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03: // ADDAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07: // SUBAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B: // MADDAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F: // MSUBAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x10: // ITOF0
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x11: // ITOF4
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 16.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x12: // ITOF12
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 4096.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x13: // ITOF15
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 32768.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x14: // FTOI0
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = ps2x::vu1::detail::vuFloatToInt(vs[c], 1.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x15: // FTOI4
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = ps2x::vu1::detail::vuFloatToInt(vs[c], 16.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x16: // FTOI12
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = ps2x::vu1::detail::vuFloatToInt(vs[c], 4096.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x17: // FTOI15
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = ps2x::vu1::detail::vuFloatToInt(vs[c], 32768.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x1B: // MULAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x1C: // MULAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x1D: // ABS
            for (int c = 0; c < 4; c++)
                result[c] = std::fabs(vs[c]);
            applyDest(vtDest, result, dest);
            return;
        case 0x1E: // MULAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x1F: // CLIP
        {
            uint32_t wBits = 0u;
            std::memcpy(&wBits, &m_state.vf[ft][3], sizeof(wBits));
            const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;

            const auto exceedsClipPlane = [limit](float value, uint32_t signMask)
            {
                uint32_t bits = 0u;
                std::memcpy(&bits, &value, sizeof(bits));
                bits ^= signMask;
                int32_t orderedBits = 0;
                std::memcpy(&orderedBits, &bits, sizeof(orderedBits));
                return orderedBits > limit;
            };

            uint32_t flags = 0u;
            if (exceedsClipPlane(m_state.vf[fs][0], 0x00000000u))
                flags |= 0x01u;
            if (exceedsClipPlane(m_state.vf[fs][0], 0x80000000u))
                flags |= 0x02u;
            if (exceedsClipPlane(m_state.vf[fs][1], 0x00000000u))
                flags |= 0x04u;
            if (exceedsClipPlane(m_state.vf[fs][1], 0x80000000u))
                flags |= 0x08u;
            if (exceedsClipPlane(m_state.vf[fs][2], 0x00000000u))
                flags |= 0x10u;
            if (exceedsClipPlane(m_state.vf[fs][2], 0x80000000u))
                flags |= 0x20u;
            queueClip(flags);
            return;
        }
        case 0x20: // ADDAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x21: // MADDAq
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x22: // ADDAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x23: // MADDAi
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x24: // SUBAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x25: // MSUBAq
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x26: // SUBAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x27: // MSUBAi
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x28: // ADDA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x29: // MADDA
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x2A: // MULA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x2C: // SUBA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x2D: // MSUBA
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x2E: // OPMULA
            result[0] = vs[1] * vt[2];
            result[1] = vs[2] * vt[0];
            result[2] = vs[0] * vt[1];
            result[3] = 0.0f;
            applyFmacDestAcc(result, dest);
            return;
        case 0x2F:
        case 0x30: // NOP
            return;
        default:
            reportReservedInstruction(true, instr);
            return;
        }
    }

    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    default:
        reportReservedInstruction(true, instr);
        return;
    }
}


// ---------------------------------------------------------------------------
// Compile-time upper-op execution (v2)
// ---------------------------------------------------------------------------
template <uint32_t U, uint32_t Op, bool WriteAcc, bool CrossNoAcc>
PS2X_VU1_INLINE void VU1Interpreter::execUpperArith()
{
    constexpr uint8_t dest = DEST(U);
    constexpr uint8_t ft = FT(U);
    constexpr uint8_t fs = FS(U);
    constexpr uint8_t fd = FD(U);

    float normalizedVs[4]{};
    float normalizedVt[4]{};
    float normalizedAcc[4]{};
    float q = 0.0f;
    float i = 0.0f;

    for (uint32_t c = 0; c < 4u; ++c)
        normalizedVs[c] = normalizeOperand(m_state.vf[fs][c]);

    constexpr bool needsVt = Op < 0x1Cu || Op >= 0x28u;
    constexpr bool needsAcc = (Op >= 0x08u && Op <= 0x0Fu) ||
                              Op == 0x21u || Op == 0x23u || Op == 0x25u ||
                              Op == 0x27u || Op == 0x29u || Op == 0x2Du ||
                              Op == 0x2Eu;
    constexpr bool needsQ = Op == 0x1Cu || Op == 0x20u || Op == 0x21u ||
                            Op == 0x24u || Op == 0x25u;
    constexpr bool needsI = Op == 0x1Du || Op == 0x1Fu || Op == 0x1Eu ||
                            Op == 0x22u || Op == 0x23u ||
                            Op == 0x26u || Op == 0x27u;

    if constexpr (needsVt)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            normalizedVt[c] = normalizeOperand(m_state.vf[ft][c]);
    }
    if constexpr (needsAcc)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            normalizedAcc[c] = normalizeOperand(m_state.acc[c]);
    }
    if constexpr (needsQ)
        q = normalizeOperand(m_state.q);
    if constexpr (needsI)
        i = normalizeOperand(m_state.i);

    const float *vs = normalizedVs;
    const float *vt = normalizedVt;
    const float *acc = normalizedAcc;
    float result[4];

    if constexpr (CrossNoAcc)
    {
        result[0] = vs[1] * vt[2];
        result[1] = vs[2] * vt[0];
        result[2] = vs[0] * vt[1];
        result[3] = 0.0f;
    }
    else if constexpr (Op <= 0x03u)
    {
        const float bc = vt[Op & 3u];
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = vs[c] + bc;
    }
    else if constexpr (Op <= 0x07u)
    {
        const float bc = vt[Op & 3u];
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = vs[c] - bc;
    }
    else if constexpr (Op <= 0x0Bu)
    {
        const float bc = vt[Op & 3u];
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = acc[c] + vs[c] * bc;
    }
    else if constexpr (Op <= 0x0Fu)
    {
        const float bc = vt[Op & 3u];
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = acc[c] - vs[c] * bc;
    }
    else if constexpr (Op <= 0x13u)
    {
        const float bc = vt[Op & 3u];
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = (vs[c] > bc) ? vs[c] : bc;
    }
    else if constexpr (Op <= 0x17u)
    {
        const float bc = vt[Op & 3u];
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = (vs[c] < bc) ? vs[c] : bc;
    }
    else if constexpr (Op <= 0x1Bu)
    {
        const float bc = vt[Op & 3u];
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = vs[c] * bc;
    }
    else if constexpr (Op == 0x1Cu)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = vs[c] * q;
    }
    else if constexpr (Op == 0x1Du)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = (vs[c] > i) ? vs[c] : i;
    }
    else if constexpr (Op == 0x1Eu)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = vs[c] * i;
    }
    else if constexpr (Op == 0x1Fu)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = (vs[c] < i) ? vs[c] : i;
    }
    else if constexpr (Op == 0x20u)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = vs[c] + q;
    }
    else if constexpr (Op == 0x21u)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = acc[c] + vs[c] * q;
    }
    else if constexpr (Op == 0x22u)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = vs[c] + i;
    }
    else if constexpr (Op == 0x23u)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = acc[c] + vs[c] * i;
    }
    else if constexpr (Op == 0x24u)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = vs[c] - q;
    }
    else if constexpr (Op == 0x25u)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = acc[c] - vs[c] * q;
    }
    else if constexpr (Op == 0x26u)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = vs[c] - i;
    }
    else if constexpr (Op == 0x27u)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = acc[c] - vs[c] * i;
    }
    else if constexpr (Op == 0x28u)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = vs[c] + vt[c];
    }
    else if constexpr (Op == 0x29u)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = acc[c] + vs[c] * vt[c];
    }
    else if constexpr (Op == 0x2Au)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = vs[c] * vt[c];
    }
    else if constexpr (Op == 0x2Bu)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = (vs[c] > vt[c]) ? vs[c] : vt[c];
    }
    else if constexpr (Op == 0x2Cu)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = vs[c] - vt[c];
    }
    else if constexpr (Op == 0x2Du)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = acc[c] - vs[c] * vt[c];
    }
    else if constexpr (Op == 0x2Eu)
    {
        result[0] = acc[0] - vs[1] * vt[2];
        result[1] = acc[1] - vs[2] * vt[0];
        result[2] = acc[2] - vs[0] * vt[1];
        result[3] = 0.0f;
    }
    else // Op == 0x2F, MINI
    {
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = (vs[c] < vt[c]) ? vs[c] : vt[c];
    }

    constexpr bool noFlags = (Op >= 0x10u && Op <= 0x17u) ||
                             Op == 0x1Du || Op == 0x1Fu ||
                             Op == 0x2Bu || Op == 0x2Fu;
    if constexpr (WriteAcc)
    {
        if constexpr (noFlags)
            applyDestAcc(result, dest);
        else
            applyFmacDestAcc<U>(result, dest);
    }
    else
    {
        if constexpr (noFlags)
            applyDest(m_state.vf[fd], result, dest);
        else
            applyFmacDest<U>(m_state.vf[fd], result, dest);
    }
}

template <uint32_t U, uint32_t Special>
PS2X_VU1_INLINE void VU1Interpreter::execUpperSpecial()
{
    constexpr uint8_t ft = FT(U);
    constexpr uint8_t fs = FS(U);
    constexpr uint8_t dest = DEST(U);
    constexpr uint8_t fd = FD(U);
    (void)fd;

    if constexpr (Special == 0x10u || Special == 0x11u ||
                  Special == 0x12u || Special == 0x13u)
    {
        float normalizedVs[4];
        float result[4];
        constexpr float scale = Special == 0x10u   ? 1.0f
                                : Special == 0x11u ? 16.0f
                                : Special == 0x12u ? 4096.0f
                                                   : 32768.0f;
        for (uint32_t c = 0; c < 4u; ++c)
        {
            normalizedVs[c] = normalizeOperand(m_state.vf[fs][c]);
            int32_t iv;
            std::memcpy(&iv, &m_state.vf[fs][c], 4);
            result[c] = static_cast<float>(iv) / scale;
        }
        applyDest(m_state.vf[ft], result, dest);
    }
    else if constexpr (Special >= 0x14u && Special <= 0x17u)
    {
        float result[4];
        constexpr float scale = Special == 0x14u   ? 1.0f
                                : Special == 0x15u ? 16.0f
                                : Special == 0x16u ? 4096.0f
                                                   : 32768.0f;
        for (uint32_t c = 0; c < 4u; ++c)
        {
            const int32_t iv = ps2x::vu1::detail::vuFloatToInt(
                normalizeOperand(m_state.vf[fs][c]), scale);
            std::memcpy(&result[c], &iv, 4);
        }
        applyDest(m_state.vf[ft], result, dest);
    }
    else if constexpr (Special == 0x1Du)
    {
        float result[4];
        for (uint32_t c = 0; c < 4u; ++c)
            result[c] = std::fabs(normalizeOperand(m_state.vf[fs][c]));
        applyDest(m_state.vf[ft], result, dest);
    }
    else if constexpr (Special == 0x1Fu)
    {
        uint32_t wBits = 0u;
        std::memcpy(&wBits, &m_state.vf[ft][3], sizeof(wBits));
        const int32_t limit = (wBits & 0x7F800000u) != 0u
                                  ? static_cast<int32_t>(wBits & 0x7FFFFFFFu)
                                  : 0x007FFFFF;
        const auto exceedsClipPlane = [limit](float value, uint32_t signMask)
        {
            uint32_t bits = 0u;
            std::memcpy(&bits, &value, sizeof(bits));
            bits ^= signMask;
            int32_t orderedBits = 0;
            std::memcpy(&orderedBits, &bits, sizeof(orderedBits));
            return orderedBits > limit;
        };
        uint32_t flags = 0u;
        if (exceedsClipPlane(m_state.vf[fs][0], 0x00000000u))
            flags |= 0x01u;
        if (exceedsClipPlane(m_state.vf[fs][0], 0x80000000u))
            flags |= 0x02u;
        if (exceedsClipPlane(m_state.vf[fs][1], 0x00000000u))
            flags |= 0x04u;
        if (exceedsClipPlane(m_state.vf[fs][1], 0x80000000u))
            flags |= 0x08u;
        if (exceedsClipPlane(m_state.vf[fs][2], 0x00000000u))
            flags |= 0x10u;
        if (exceedsClipPlane(m_state.vf[fs][2], 0x80000000u))
            flags |= 0x20u;
        queueClip(flags);
    }
    else if constexpr (Special == 0x2Fu || Special == 0x30u)
    {
        return;
    }
    else if constexpr (Special == 0x2Eu)
    {
        execUpperArith<U, 0x2Eu, true, true>();
    }
    else if constexpr (Special <= 0x0Fu || Special == 0x1Cu ||
                       Special == 0x1Eu || (Special >= 0x18u && Special <= 0x1Bu) ||
                       (Special >= 0x20u && Special <= 0x2Du))
    {
        execUpperArith<U, Special, true, false>();
    }
    else
    {
        execUpper(U);
    }
}

template <uint32_t U>
PS2X_VU1_INLINE void VU1Interpreter::execUpperConstant()
{
    constexpr uint8_t op = static_cast<uint8_t>(U & 0x3Fu);
    constexpr uint8_t special = static_cast<uint8_t>((U & 3u) | ((U >> 4) & 0x7Cu));
    if constexpr (op < 0x3Cu)
    {
        execUpperArith<U, op, false, false>();
    }
    else
    {
        execUpperSpecial<U, special>();
    }
}

#endif
