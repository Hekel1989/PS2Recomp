#pragma once

// PS2X(gs-phase4): optional host hooks that let the embedding application
// attach an external GS backend. The runtime
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

        // PS2X(gs-phase5): Called for each complete tagged GIF packet as the
        // GIF arbiter hands it to the GS frontend (drain), before the runtime's
        // own frontend parses it. path is the effective GIF path in PCSX2's
        // Transfer<n> slot semantics: 1 = PATH1 (VU1 XGKICK), 2 = PATH2 (GIF
        // DMA), 3 = PATH3 (VIF1 DIRECT/DIRECTHL). Note PS2Recomp models VIF1
        // DIRECT/DIRECTHL as Path2 and GIF DMA as Path3, so the arbiter's
        // GifPathId is remapped to the hardware path here. data points at
        // qwords * 16 bytes starting with the GIFtag and is only valid for the
        // duration of the call. Fires on the game thread.
        //
        // PS2X(gs-phase5b): continuation marks a DIRECT continuation: the
        // packet's first qword is a GIFtag the runtime synthesized for its own
        // frontend because the original DIRECT payload was split at a DMA-chunk
        // boundary (see m_vif1PendingPath2ImageQwc). The raw continuation data
        // (qwords - 1 qwords after the synthesized tag) completes the packet
        // the same path delivered earlier; an external GS that keeps per-path
        // transfer state should skip the synthesized tag and feed the rest as
        // continuation of that open packet instead of parsing it as a new tag.
        std::function<void(uint32_t path, const uint8_t *data, uint32_t qwords, bool continuation)> gifPacket;

        // PS2X(gs-phase5b): Called when the runtime's own GS frontend consumes
        // GIF qwords, with the same path numbering as gifPacket. Fires at every
        // consumption site: the arbiter drain, the native GIF fast paths, and
        // the sceGsPutDrawEnv register-pair path. The count is the number of
        // GIF qwords the hardware would have delivered on that path (fast-path
        // chains count their assembled packet; a DIRECT continuation counts
        // without its synthesized GIFtag). Lets a host cross-check that
        // everything the runtime frontend saw also reached its external GS.
        std::function<void(uint32_t path, uint32_t qwords)> gifConsume;

        // PS2X(gs-single-raster): Bring the runtime's CPU-side VRAM copy up to
        // date from the external GS before the guest reads it (local-to-host
        // IMAGE transfer or GS::ReadVram).  With a hardware GS attached the CPU
        // rasterizer is redundant for display; it is only needed for guest
        // readbacks, so this hook makes those reads authoritative.
        std::function<void()> syncLocalMemory;
    };

    // PS2X(gs-single-raster): an embedding frontend that renders every packet
    // itself can switch off the redundant CPU rasterizer.  Drawing is still
    // parsed (register/transfer state stays correct); only pixel generation is
    // skipped.  Enabled by default so standalone PS2Recomp behaviour is
    // unchanged.
    inline bool &cpuRasterEnabledStorage()
    {
        static bool enabled = true;
        return enabled;
    }

    inline void setCpuRasterEnabled(bool enabled)
    {
        cpuRasterEnabledStorage() = enabled;
    }

    inline bool cpuRasterEnabled()
    {
        return cpuRasterEnabledStorage();
    }

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
