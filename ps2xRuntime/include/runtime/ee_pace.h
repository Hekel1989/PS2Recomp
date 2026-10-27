#pragma once

#include <algorithm>
#include <cstdint>

namespace ps2x::time
{
    // Emulated-time pacing arithmetic, kept out of EeScheduler so it can be
    // unit-tested with synthetic intervals.  The scheduler feeds one sample per
    // ~2 ms of host wall time; see EeScheduler::sampleHostPacing().
    //
    // A sample separates three kinds of excluded host time:
    //  - reported host-only work (noteHostStall: snapshots, dumps, MPEG decode);
    //  - OS descheduling outside a guest wait (the executor thread was runnable
    //    but not on a CPU);
    //  - guest waits (DelayThread/WaitVSync/semaphore), which are emulated time.
    //
    // Short descheduling (up to one VBlank period) is repaid immediately: it is
    // ordinary scheduler jitter, not a freeze, and leaving it out makes the
    // guest clock run ~0.5 % slow.  Longer descheduling joins reported host
    // stalls in the debt, which is repaid only out of surplus guest CPU time:
    // a one-off multi-hundred-ms freeze must not turn into a burst of due
    // VBlanks/timers.
    constexpr uint64_t kPaceMaxDebtNs = 2000000000ull;   // 2 s
    constexpr uint64_t kPaceMaxShortDeschedNs = 16000000ull; // one VBlank
    constexpr uint64_t kPaceCatchUpNumerator = 2ull;
    constexpr uint64_t kPaceCatchUpDenominator = 1ull;

    struct PaceSample
    {
        uint64_t wallNs = 0;
        uint64_t cpuNs = 0;
        uint64_t stallNs = 0;
        uint64_t waitNs = 0;
        bool movieActive = false;
        bool repayShortDesched = true;
    };

    struct PaceStep
    {
        uint64_t descheduledNs = 0;
        uint64_t shortDeschedNs = 0;
        uint64_t guestCpuNs = 0;
        uint64_t baseNs = 0;
        uint64_t repayNs = 0;
        uint64_t advanceNs = 0;
        uint64_t debtNs = 0;
    };

    inline PaceStep stepPace(const PaceSample &sample, uint64_t debtNs) noexcept
    {
        PaceStep step{};
        const uint64_t notOnCpu = sample.wallNs > sample.cpuNs
                                      ? sample.wallNs - sample.cpuNs
                                      : 0u;
        step.descheduledNs = notOnCpu > sample.waitNs
                                 ? notOnCpu - sample.waitNs
                                 : 0u;

        // Excluded time is everything the guest clock should not advance by
        // while it happens: reported host work plus descheduling.
        const uint64_t excluded = std::min(sample.wallNs, sample.stallNs + step.descheduledNs);
        step.baseNs = sample.wallNs - excluded;

        // Short descheduling is repaid during an MPEG movie too.  The sound
        // device consumes 48 kHz of wall time; leaving the descheduling out
        // ran the guest clock (and so SPU2 output) 1-2 % slow for the whole
        // movie, and the output queue drained into a short silence every
        // ~10 s.  One VBlank of catch-up is well inside the game's 24 KiB
        // (128 ms) IOP audio ring.  Long stalls are still discarded below.
        if (!sample.repayShortDesched)
        {
            step.shortDeschedNs = 0u;
        }
        else
        {
            step.shortDeschedNs = std::min(step.descheduledNs, kPaceMaxShortDeschedNs);
        }

        const uint64_t hostCpu = std::min(sample.cpuNs, sample.stallNs);
        step.guestCpuNs = sample.cpuNs - hostCpu;

        // The short part is paid immediately; only the remainder competes for
        // the slow-motion catch-up budget.
        uint64_t debtAdd = excluded - std::min(excluded, step.shortDeschedNs);
        if (debtAdd >= kPaceMaxDebtNs - debtNs)
        {
            debtNs = kPaceMaxDebtNs;
        }
        else
        {
            debtNs += debtAdd;
        }
        if (sample.movieActive)
        {
            debtNs = 0u;
        }

        const uint64_t allowance =
            (step.guestCpuNs / kPaceCatchUpDenominator) * kPaceCatchUpNumerator +
            (step.guestCpuNs % kPaceCatchUpDenominator) * kPaceCatchUpNumerator / kPaceCatchUpDenominator;
        step.repayNs = allowance > step.baseNs ? allowance - step.baseNs : 0u;
        step.repayNs = std::min(step.repayNs, debtNs);
        step.debtNs = debtNs - step.repayNs;
        step.advanceNs = step.baseNs + step.repayNs + step.shortDeschedNs;
        return step;
    }
}
