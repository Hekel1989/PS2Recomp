// Share measurements: see runtime/ps2_diag_shares.h.
#include "runtime/ps2_diag_shares.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace suiko::diag
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr uint32_t kHistBuckets = 12u;
        // Upper edge (inclusive) of each wait/overlap bucket.  The last bucket
        // is "> 5000 us".
        constexpr uint64_t kHistEdgesUs[kHistBuckets] = {0u, 1u, 2u, 5u, 10u, 25u,
                                                         50u, 100u, 250u, 1000u, 5000u,
                                                         UINT64_MAX};

        enum VuReason : uint32_t
        {
            kVif1Stat = 0,
            kVu1MemRead,
            kVu1MemWrite,
            kVif1Reg,
            kVif1Dma,
            kGifPath,
            kGifImage,
            kVuReasonCount
        };

        const char *const kVuReasonNames[kVuReasonCount] = {
            "vif1stat", "vu1mem", "vu1memw", "vif1reg", "vif1dma", "gifpath",
            "gifimg"};

        enum GsReason : uint32_t
        {
            kFinish = 0,
            kSignal,
            kCsr,
            kDownload,
            kLocalToHost,
            kPresent,
            kReadVram,
            kGsReasonCount
        };

        const char *const kGsReasonNames[kGsReasonCount] = {
            "finish", "signal", "csr", "download", "localtohost", "present", "readvram"};

        struct VuFrame
        {
            uint64_t vsync = UINT64_MAX;
            uint64_t kicks = 0;
            uint64_t vu1Ns = 0;
            uint64_t observations = 0;
            uint64_t waitNs = 0;
            uint64_t overlapNs = 0;
            uint64_t maxWaitNs = 0;
            uint64_t full = 0;
            uint64_t zero = 0;
            uint64_t multi = 0;
            uint64_t maxBatch = 0;
            std::array<uint64_t, kHistBuckets> waitHist{};
            std::array<uint64_t, kHistBuckets> overlapHist{};
            std::array<uint64_t, kVuReasonCount> reasons{};
        };

        struct GsFrame
        {
            uint64_t vsync = UINT64_MAX;
            std::array<uint64_t, kGsReasonCount> reasons{};
        };

        struct State
        {
            VuFrame vu;
            GsFrame gs;
            VuFrame vuTotal;
            GsFrame gsTotal;
            uint64_t frames = 0;
            uint64_t gsFrames = 0;

            Clock::time_point kickStart{};
            Clock::time_point pendingSince{};
            uint64_t pendingVu1Ns = 0;
            uint64_t pendingKicks = 0;
            bool inKick = false;
            bool pending = false;
        };

        State &state()
        {
            static State s;
            return s;
        }

        uint32_t bucket(uint64_t ns)
        {
            const uint64_t us = ns / 1000u;
            for (uint32_t index = 0; index + 1u < kHistBuckets; ++index)
            {
                if (us <= kHistEdgesUs[index])
                    return index;
            }
            return kHistBuckets - 1u;
        }

        uint32_t vuReasonId(const char *reason)
        {
            if (reason != nullptr)
            {
                for (uint32_t index = 0; index < kVuReasonCount; ++index)
                {
                    if (std::strcmp(reason, kVuReasonNames[index]) == 0)
                        return index;
                }
            }
            return kVif1Stat;
        }

        uint32_t gsReasonId(const char *reason)
        {
            if (reason != nullptr)
            {
                for (uint32_t index = 0; index < kGsReasonCount; ++index)
                {
                    if (std::strcmp(reason, kGsReasonNames[index]) == 0)
                        return index;
                }
            }
            return kFinish;
        }

        void printHist(const std::array<uint64_t, kHistBuckets> &hist)
        {
            std::fprintf(stderr, "hist=");
            for (uint32_t index = 0; index < kHistBuckets; ++index)
                std::fprintf(stderr, "%s%llu", index == 0u ? "" : ",",
                             static_cast<unsigned long long>(hist[index]));
        }

        void printVuFrame(const char *prefix, const VuFrame &frame)
        {
            std::fprintf(stderr,
                         "%s vsync=%llu kicks=%llu vu1Us=%llu obs=%llu waitUs=%llu "
                         "ovUs=%llu full=%llu zero=%llu sup=%llu maxUs=%llu maxbatch=%llu ",
                         prefix,
                         static_cast<unsigned long long>(frame.vsync),
                         static_cast<unsigned long long>(frame.kicks),
                         static_cast<unsigned long long>(frame.vu1Ns / 1000u),
                         static_cast<unsigned long long>(frame.observations),
                         static_cast<unsigned long long>(frame.waitNs / 1000u),
                         static_cast<unsigned long long>(frame.overlapNs / 1000u),
                         static_cast<unsigned long long>(frame.full),
                         static_cast<unsigned long long>(frame.zero),
                         static_cast<unsigned long long>(frame.multi),
                         static_cast<unsigned long long>(frame.maxWaitNs / 1000u),
                         static_cast<unsigned long long>(frame.maxBatch));
            printHist(frame.waitHist);
            std::fprintf(stderr, " ");
            printHist(frame.overlapHist);
            std::fprintf(stderr, " reasons=");
            for (uint32_t index = 0; index < kVuReasonCount; ++index)
                std::fprintf(stderr, "%s%llu", index == 0u ? "" : ",",
                             static_cast<unsigned long long>(frame.reasons[index]));
            std::fprintf(stderr, "\n");
        }

        void printGsFrame(const char *prefix, const GsFrame &frame)
        {
            std::fprintf(stderr, "%s vsync=%llu", prefix,
                         static_cast<unsigned long long>(frame.vsync));
            for (uint32_t index = 0; index < kGsReasonCount; ++index)
                std::fprintf(stderr, " %s=%llu", kGsReasonNames[index],
                             static_cast<unsigned long long>(frame.reasons[index]));
            std::fprintf(stderr, "\n");
        }

        void resetVu(VuFrame &frame)
        {
            frame = VuFrame{};
        }

        void resetGs(GsFrame &frame)
        {
            frame = GsFrame{};
        }

        void flushVuFrame()
        {
            State &s = state();
            if (s.vu.vsync == UINT64_MAX)
                return;
            printVuFrame("[vu1-ov]", s.vu);
            s.vuTotal.kicks += s.vu.kicks;
            s.vuTotal.vu1Ns += s.vu.vu1Ns;
            s.vuTotal.observations += s.vu.observations;
            s.vuTotal.waitNs += s.vu.waitNs;
            s.vuTotal.overlapNs += s.vu.overlapNs;
            s.vuTotal.maxWaitNs = std::max(s.vuTotal.maxWaitNs, s.vu.maxWaitNs);
            s.vuTotal.full += s.vu.full;
            s.vuTotal.zero += s.vu.zero;
            s.vuTotal.multi += s.vu.multi;
            s.vuTotal.maxBatch = std::max(s.vuTotal.maxBatch, s.vu.maxBatch);
            for (uint32_t index = 0; index < kHistBuckets; ++index)
            {
                s.vuTotal.waitHist[index] += s.vu.waitHist[index];
                s.vuTotal.overlapHist[index] += s.vu.overlapHist[index];
            }
            for (uint32_t index = 0; index < kVuReasonCount; ++index)
                s.vuTotal.reasons[index] += s.vu.reasons[index];
            ++s.frames;
            resetVu(s.vu);
        }

        void flushGsFrame()
        {
            State &s = state();
            if (s.gs.vsync == UINT64_MAX)
                return;
            printGsFrame("[gs-sync]", s.gs);
            for (uint32_t index = 0; index < kGsReasonCount; ++index)
                s.gsTotal.reasons[index] += s.gs.reasons[index];
            ++s.gsFrames;
            resetGs(s.gs);
        }

        void atexitFlush()
        {
            flush();
        }

        void installAtexit()
        {
            static bool installed = false;
            if (!installed)
            {
                installed = true;
                std::atexit(&atexitFlush);
            }
        }
    } // namespace

    void vu1KickBegin(uint64_t vsync)
    {
        State &s = state();
        installAtexit();
        if (s.vu.vsync != vsync)
        {
            flushVuFrame();
            s.vu.vsync = vsync;
        }
        if (!s.pending)
        {
            // The batch starts at the first kick; the EE can already run while
            // this program and every following kick in the same batch execute.
            s.pending = true;
            s.pendingSince = Clock::now();
            s.pendingVu1Ns = 0;
            s.pendingKicks = 0;
        }
        s.inKick = true;
        s.kickStart = Clock::now();
    }

    void vu1KickEnd(uint64_t vsync)
    {
        State &s = state();
        if (!s.inKick)
            return;
        const auto now = Clock::now();
        const uint64_t durationNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - s.kickStart).count());
        if (s.vu.vsync != vsync)
        {
            flushVuFrame();
            s.vu.vsync = vsync;
        }
        s.inKick = false;
        ++s.vu.kicks;
        s.vu.vu1Ns += durationNs;
        s.pendingVu1Ns += durationNs;
        ++s.pendingKicks;
    }

    void vu1Observe(uint64_t vsync, const char *reason)
    {
        State &s = state();
        if (!s.pending)
            return;
        const auto now = Clock::now();
        const uint64_t elapsedNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - s.pendingSince).count());
        // The serial runtime spends the VU1 time on the EE thread too, so the
        // EE work available for overlap is elapsed minus the batch's VU1 time.
        const uint64_t eeWorkNs = elapsedNs > s.pendingVu1Ns ? elapsedNs - s.pendingVu1Ns : 0u;
        const uint64_t overlapNs = std::min(s.pendingVu1Ns, eeWorkNs);
        const uint64_t batch = s.pendingKicks;
        s.pending = false;
        if (s.vu.vsync != vsync)
        {
            flushVuFrame();
            s.vu.vsync = vsync;
        }
        ++s.vu.observations;
        ++s.vu.reasons[vuReasonId(reason)];
        s.vu.waitNs += eeWorkNs;
        s.vu.overlapNs += overlapNs;
        s.vu.maxWaitNs = std::max(s.vu.maxWaitNs, eeWorkNs);
        s.vu.maxBatch = std::max(s.vu.maxBatch, batch);
        if (batch > 1u)
            ++s.vu.multi;
        ++s.vu.waitHist[bucket(eeWorkNs)];
        ++s.vu.overlapHist[bucket(overlapNs)];
        if (eeWorkNs >= s.pendingVu1Ns)
            ++s.vu.full;
        if (eeWorkNs < 1000u)
            ++s.vu.zero;
    }

    void gsEvent(uint64_t vsync, const char *reason)
    {
        State &s = state();
        installAtexit();
        if (s.gs.vsync != vsync)
        {
            flushGsFrame();
            s.gs.vsync = vsync;
        }
        ++s.gs.reasons[gsReasonId(reason)];
    }

    void flush()
    {
        State &s = state();
        if (s.inKick)
        {
            const auto now = Clock::now();
            s.vu.vu1Ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - s.kickStart).count());
            ++s.vu.kicks;
            s.inKick = false;
        }
        flushVuFrame();
        flushGsFrame();

        if (s.frames > 0u)
        {
            std::fprintf(stderr,
                         "[vu1-ov-total] frames=%llu kicks=%llu vu1Ms=%llu obs=%llu "
                         "waitMs=%llu ovMs=%llu full=%llu zero=%llu multi=%llu maxUs=%llu "
                         "maxbatch=%llu ",
                         static_cast<unsigned long long>(s.frames),
                         static_cast<unsigned long long>(s.vuTotal.kicks),
                         static_cast<unsigned long long>(s.vuTotal.vu1Ns / 1000000u),
                         static_cast<unsigned long long>(s.vuTotal.observations),
                         static_cast<unsigned long long>(s.vuTotal.waitNs / 1000000u),
                         static_cast<unsigned long long>(s.vuTotal.overlapNs / 1000000u),
                         static_cast<unsigned long long>(s.vuTotal.full),
                         static_cast<unsigned long long>(s.vuTotal.zero),
                         static_cast<unsigned long long>(s.vuTotal.multi),
                         static_cast<unsigned long long>(s.vuTotal.maxWaitNs / 1000u),
                         static_cast<unsigned long long>(s.vuTotal.maxBatch));
            printHist(s.vuTotal.waitHist);
            std::fprintf(stderr, " ");
            printHist(s.vuTotal.overlapHist);
            std::fprintf(stderr, " reasons=");
            for (uint32_t index = 0; index < kVuReasonCount; ++index)
                std::fprintf(stderr, "%s%llu", index == 0u ? "" : ",",
                             static_cast<unsigned long long>(s.vuTotal.reasons[index]));
            std::fprintf(stderr, "\n");
        }
        if (s.gsFrames > 0u)
        {
            std::fprintf(stderr, "[gs-sync-total] frames=%llu",
                         static_cast<unsigned long long>(s.gsFrames));
            for (uint32_t index = 0; index < kGsReasonCount; ++index)
                std::fprintf(stderr, " %s=%llu", kGsReasonNames[index],
                             static_cast<unsigned long long>(s.gsTotal.reasons[index]));
            std::fprintf(stderr, "\n");
        }
        std::fflush(stderr);
    }
} // namespace suiko::diag
