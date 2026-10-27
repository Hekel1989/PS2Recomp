#pragma once

#include <cstdint>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;

    class IopVblank
    {
    public:
        explicit IopVblank(IopKernel &kernel) noexcept;

        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);
        // The EE announces its next VBlank start/end in IOP cycles at every
        // VBlank start; from then on WaitVblank* wakes exactly there.
        void setEeVblank(uint64_t nextStart, uint64_t nextEnd, uint64_t period);

    private:
        [[nodiscard]] uint64_t nextEeEdge(uint64_t edge, uint64_t currentCycle) const;

        IopKernel &m_kernel;
        bool m_eeSync = false;
        uint64_t m_nextStart = 0;
        uint64_t m_nextEnd = 0;
        uint64_t m_period = 0;
    };
}
