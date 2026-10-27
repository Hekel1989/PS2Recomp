#pragma once

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace ps2x::time
{
    // PS2X(deterministic-mode): PS2X_DETERMINISTIC=1 makes emulated time
    // independent of the host clock.  Inline so the lightweight VFS tests do
    // not have to link the scheduler; the function-local static is read once
    // per process and every caller then pays one predictable branch.
    inline bool deterministicMode() noexcept
    {
        static const bool enabled = []()
        {
            const char *value = std::getenv("PS2X_DETERMINISTIC");
            return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    // Fixed instant used for every guest-visible host clock read under the
    // switch: 2000-01-01 00:00:00 UTC (946684800).
    inline constexpr std::time_t kDeterministicTimeT = 946684800;

    // Guest-visible wall-clock time.  Normally std::time(nullptr); under the
    // switch, kDeterministicTimeT.
    std::time_t wallTimeNow() noexcept;

    // Break a time_t into local time.  Normally the host time zone; under the
    // switch, UTC, so the host's time-zone setting cannot change guest state.
    std::tm localTime(std::time_t value) noexcept;

    // Report host-only work that ran on the EE executor thread and must not
    // advance the guest's emulated clock: PNG/VRAM snapshot encoding, debug
    // dumps, and similar diagnostics.  The EE scheduler subtracts this time
    // from its wall-clock pacing and repays it only as fast as guest execution
    // allows (see EeScheduler::sampleHostPacing).  Safe to call from any
    // thread; the accounting is per-thread and only the executor's portion is
    // observed.
    void noteHostStall(std::chrono::nanoseconds duration) noexcept;

    // Report time the executor thread spent blocked on work the serial
    // runtime would have run on this thread itself (the VU1 thread's fences,
    // runtime/ps2_vu1_thread.h).  It advances the guest clock like a guest
    // wait instead of counting as OS descheduling, so a long fence cannot
    // push the game into slow-motion debt.  Executor thread only.
    void noteEmulatedWait(std::chrono::nanoseconds duration) noexcept;

    // Developer A/B switch for the short-descheduling repayment rule:
    // PS2X_EE_DESCHED_REPAY=0 restores the old behaviour (all excluded time,
    // including OS descheduling, goes through the slow-motion debt) on the
    // same binary.  Env only; never a config file or menu setting.
    bool descheduleRepayEnabled() noexcept;
}
