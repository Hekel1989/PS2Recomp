#pragma once

// SUIKO(gs-phase4): optional host hooks that let the embedding application
// attach an external GS backend (see docs/gs-port-plan.md phase 4). The runtime
// only calls these; it never requires them, and they must not contain any
// PCSX2 types -- plain integers only, so this header stays submodule-clean.
//
// Threading: both hooks fire on the game thread (the EE interpreter and the
// scheduler event loop share it), once per guest register write / VBlankStart.
// Register them before PS2Runtime::run() starts the game thread.

#include <cstdint>
#include <functional>
#include <utility>

namespace Ps2GsHostHooks
{
    struct Hooks
    {
        // Called after each guest write to a GS privileged register (the
        // 0x12000000 page). offset is the 64-bit-aligned register offset
        // (0x0000..0x00E0, CSR 0x1000, IMR 0x1010, BUSDIR 0x1040,
        // SIGLBLID 0x1080); value is the full 64-bit register contents after
        // PS2Recomp semantics (CSR write-one-to-clear already applied).
        std::function<void(uint32_t offset, uint64_t value)> privRegWrite;

        // Called once per VBlankStart, after the CSR FIELD bit update.
        // tick is the scheduler's vsync counter (1-based).
        std::function<void(uint64_t tick)> vsync;
    };

    inline Hooks &mutableHooks()
    {
        static Hooks hooks;
        return hooks;
    }

    inline const Hooks &hooks()
    {
        return mutableHooks();
    }

    inline void setHooks(Hooks hooks)
    {
        mutableHooks() = std::move(hooks);
    }
} // namespace Ps2GsHostHooks
