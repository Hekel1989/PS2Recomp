#include "iop_vblank.h"

#include "../core/iop_cpu.h"
#include "../iop_emulator_const.h"
#include "../core/iop_kernel.h"

namespace ps2x::iop::detail
{
    IopVblank::IopVblank(IopKernel &kernel) noexcept
        : m_kernel(kernel)
    {
    }

    bool IopVblank::dispatchImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle)
    {
        switch (ordinal)
        {
        case 4: // WaitVblankStart
        case 5: // WaitVblankEnd
        case 6: // WaitVblank
        case 7: // WaitNonVblank
        {
            const bool waitForEnd = ordinal == 5u || ordinal == 7u;
            const uint64_t phase = waitForEnd ? kVblankEndPhaseCycles : 0u;
            const uint64_t fieldStart = currentCycle - (currentCycle % kVblankPeriodCycles);
            uint64_t wakeCycle = fieldStart + phase;
            if (wakeCycle <= currentCycle)
                wakeCycle += kVblankPeriodCycles;
            // PS2X(iop-vblank-phase): on hardware the IOP and the EE share one
            // VBlank, so an IOP thread woken by it runs before the EE code of
            // the new frame.  The IOP's own cycle grid had an arbitrary phase
            // against the EE's VBlank: an EE command written late in one frame
            // and the next one written early in the following frame could both
            // land between two IOP polls.  SDSTR3's CD-lock thread then lost an
            // unlock (the book freeze) and IOPMAIN read its sound table before
            // SDSTR3 had filled it (the black screen after the book;
            // docs/bringup.md).  With the EE announcing its VBlank edges, wake
            // exactly there; the grid stays for runs without an EE.
            if (m_eeSync)
                wakeCycle = nextEeEdge(waitForEnd ? m_nextEnd : m_nextStart, currentCycle);
            m_kernel.delayCurrentUntil(wakeCycle, cpu);
            m_kernel.markCurrentVblankWait(waitForEnd ? 2u : 1u);
            cpu.gpr[2] = 0u;
            return true;
        }
        case 8: // RegisterVblankHandler
        case 9: // ReleaseVblankHandler
            // Callback delivery is not required by the scheduler wait ABI yet.
            cpu.gpr[2] = 0u;
            return true;
        default:
            return false;
        }
    }

    uint64_t IopVblank::nextEeEdge(uint64_t edge, uint64_t currentCycle) const
    {
        // A thread can wait again before the EE has announced the following
        // VBlank (the IOP runs up to the EE's time, then the EE handles the
        // event): predict whole periods ahead.
        uint64_t wake = edge;
        while (wake <= currentCycle && m_period != 0u)
            wake += m_period;
        return wake > currentCycle ? wake : currentCycle + 1u;
    }

    void IopVblank::setEeVblank(uint64_t nextStart, uint64_t nextEnd, uint64_t period)
    {
        // Threads already waiting were scheduled on a prediction; after an EE
        // stall the chain is re-anchored, so move them to the announced edge.
        if (m_eeSync && nextStart != m_nextStart)
            m_kernel.retargetVblankWaiters(1u, nextStart);
        if (m_eeSync && nextEnd != m_nextEnd)
            m_kernel.retargetVblankWaiters(2u, nextEnd);
        m_eeSync = true;
        m_nextStart = nextStart;
        m_nextEnd = nextEnd;
        m_period = period;
    }
}
