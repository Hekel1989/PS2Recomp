// Share measurements for the multi-core plan (measurement only).
//
// Two independent, environment-gated captures:
//
//   PS2X_VU1_OVERLAP=1  per-frame VU1 kick/observation accounting.  A kick is
//                       an MSCAL/MSCALF/MSCNT (VIF1), an observation is the
//                       first EE access that needs VU1 to be finished
//                       (VIF1_STAT/register, VU1 memory, a VIF1 DMA or a
//                       PATH2/PATH3 GIF submission while a program is
//                       outstanding).  Consecutive kicks with no observation
//                       between them form one batch: the EE can queue the
//                       whole VIF1 DMA chain and keep running while a VU1
//                       thread executes the batch.  For every batch the
//                       capture records the EE host CPU work before the
//                       observation (elapsed time minus the batch's VU1 time,
//                       because the serial runtime spends that on the EE
//                       thread too) and min(total VU1 time, that work): the
//                       part of the batch a VU1 thread could have hidden.
//
//   PS2X_GS_SYNC=1      per-frame GS synchronisation/readback counts: FINISH,
//                       SIGNAL, CSR reads, guest VRAM downloads, local->host
//                       transfers and presents.
//
// Both are inert unless set: call sites gate on the inline enabled() checks,
// so a disabled build pays one predictable branch per observation point and
// never reads the clock.  The capture only uses the EE/game thread; at exit
// it prints one summary line per capture.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace ps2x::diag
{
    inline bool vu1OverlapEnabled()
    {
        static const bool enabled = std::getenv("PS2X_VU1_OVERLAP") != nullptr;
        return enabled;
    }

    // PS2X_VU1_OVERLAP=2 (or "hard") ignores PATH2/PATH3 GIF submissions as
    // observation points.  It measures the overlap available if the GS keeps
    // PATH1/PATH2/PATH3 ordered in a queue instead of making the EE wait.
    inline bool vu1OverlapIgnoreGif()
    {
        static const bool ignore = []
        {
            const char *value = std::getenv("PS2X_VU1_OVERLAP");
            if (value == nullptr)
                return false;
            return std::atoi(value) >= 2 || std::strcmp(value, "hard") == 0;
        }();
        return ignore;
    }

    inline bool gsSyncEnabled()
    {
        static const bool enabled = std::getenv("PS2X_GS_SYNC") != nullptr;
        return enabled;
    }

    // VU1 kick/observation accounting.  `vsync` is the current GS VBlank tick
    // and is only read by call sites when the capture is enabled.
    void vu1KickBegin(uint64_t vsync);
    void vu1KickEnd(uint64_t vsync);
    void vu1Observe(uint64_t vsync, const char *reason);

    // GS sync/readback event (`reason` is a short literal such as "finish").
    void gsEvent(uint64_t vsync, const char *reason);

    // Flush the current frame and print the whole-run totals.  Called from an
    // atexit handler installed on first use, so a timeout/SIGTERM still keeps
    // the completed frames.
    void flush();
} // namespace ps2x::diag
