#pragma once

#include <chrono>

namespace ps2x::time
{
    // Report host-only work that ran on the EE executor thread and must not
    // advance the guest's emulated clock: PNG/VRAM snapshot encoding, debug
    // dumps, and similar diagnostics.  The EE scheduler subtracts this time
    // from its wall-clock pacing and repays it only as fast as guest execution
    // allows (see EeScheduler::sampleHostPacing).  Safe to call from any
    // thread; the accounting is per-thread and only the executor's portion is
    // observed.
    void noteHostStall(std::chrono::nanoseconds duration) noexcept;
}
