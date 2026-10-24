#ifndef PS2_VU1_THREAD_H
#define PS2_VU1_THREAD_H

// VIF1 + VU1 on their own host thread, exactly equivalent to the serial
// runtime (design: docs/vu1-thread.md).
//
// The game (EE) thread still walks and completes every VIF1 DMA; it hands the
// copied bytes to the worker, which runs the VIF1 interpreter (and through
// MSCAL/MSCNT, VU1) over them.  Everything the worker would have done to the
// GIF front (PATH1/PATH2 submissions, arbiter drains, MSKPATH3) is recorded
// instead of executed and replayed by the game thread in the serial order, so
// the GIF arbiter, the PATH3 mask and the GS are only ever touched by the game
// thread.  Game-thread GIF-front operations go straight through while nothing
// is outstanding and are queued behind the outstanding VIF1 work otherwise.
// Every point where the game could observe worker-owned state calls sync().
//
// PS2X_VU1_THREAD=0 keeps the serial path (no object is created);
// PS2X_VU1_THREAD=verify adds an in-process serial shadow and compares every
// GS-bound packet and, at each fence, the VIF1/VU1 state.

#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_gs_host_hooks.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class GS;
class PS2Memory;
struct GSRegisters;
class VU1Interpreter;

namespace ps2x
{
    enum class Vu1SyncReason : uint8_t
    {
        VBlank,
        Vif1Register,
        VuMemory,
        GsRegister,
        GifStatus,
        GifFastPath,
        GsDirect,
        DebugBits,
        Diagnostics,
        Shutdown,
        Count
    };

    class Vu1Thread
    {
    public:
        enum class Mode : uint8_t
        {
            Serial,
            Threaded,
            Verify,
        };

        // PS2X_VU1_THREAD: unset/1 = threaded, 0 = serial, verify = threaded +
        // shadow.  Diagnostics that are not thread-safe force serial.
        static Mode modeFromEnvironment();

        // Returns the current guest context's VU0 FBRST (CTC2 vi28); the kick
        // reads DE1/TE1 from it exactly where the serial MSCAL callback did.
        using FbrstProvider = std::function<uint32_t()>;

        Vu1Thread(PS2Memory &memory, VU1Interpreter &vu1, GS &gs, Mode mode, FbrstProvider fbrst);
        ~Vu1Thread();

        Vu1Thread(const Vu1Thread &) = delete;
        Vu1Thread &operator=(const Vu1Thread &) = delete;

        Mode mode() const { return m_mode; }
        bool threaded() const { return !m_serialForGood; }
        static bool onWorker();

        // --- VIF1 kicks (game thread) ---------------------------------------
        // beginKick() returns false when this kick must run inline on the
        // serial path (it has already fenced in that case).
        bool beginKick();
        void addChunk(const uint8_t *data, uint32_t sizeBytes, uint32_t srcAddr, uint32_t srcKind);
        void endKick();
        // FBRST of the work item being processed (worker only).
        uint32_t workerFbrst() const;

        // --- GIF-front routing ----------------------------------------------
        // Each returns true when the operation was recorded or queued and the
        // caller must not execute it now.
        bool routeSubmit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drain, bool path2DirectHl,
                         bool directContinuation);
        bool routeDrain();
        bool routePath3Mask(bool masked);
        bool routePrivReg(uint32_t regOff, uint64_t value);

        // --- Fences ---------------------------------------------------------
        // Waits for every queued work item and replays the whole log.  A no-op
        // on the worker and while the log is already being replayed.
        void sync(Vu1SyncReason reason);
        // Replays the completed prefix of the log without waiting.
        void pump();
        // GIF_STAT read: its only worker-dependent bit is M3P (the PATH3 mask).
        // Fences only when a queued VIF1 stream may contain MSKPATH3.
        void beforeGifStatusRead();

        // --- Verification hooks (no-ops unless Mode::Verify) ----------------
        void checkPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool directContinuation);
        void checkPrivReg(uint32_t regOff, uint64_t value);
        void mirrorVuWrite(const uint8_t *vuMem, uint32_t offset, const void *data, uint32_t size);
        void mirrorVif1Register(uint32_t address, uint32_t value);

        // --- Present (game thread, VBlank) ----------------------------------
        // The present is one more operation in the ordered log, so the game
        // thread does not wait at VBlank for the frame's VU1 work; the image
        // is the same, it is only presented when that work has been replayed.
        // beforeVblank() keeps the worker at most one frame behind: it waits
        // until the previous VBlank's present has run.
        using VsyncRegs = Ps2GsHostHooks::Hooks::VsyncRegs;
        static VsyncRegs captureVsyncRegs(const GSRegisters &regs);
        void beforeVblank();
        void vblankPresent(uint64_t tick, const VsyncRegs &regs);

        void onVsync(uint64_t vsyncTick);
        bool verifyFailed() const { return m_verifyFailed; }
        uint64_t verifiedPackets() const { return m_verifyPackets; }

    private:
        enum class OpKind : uint8_t
        {
            Submit,
            Drain,
            Path3Mask,
            PrivReg,
            Vsync,
        };

        struct Op
        {
            OpKind kind = OpKind::Submit;
            GifPathId path = GifPathId::Path1;
            bool drain = false;
            bool directHl = false;
            bool continuation = false;
            bool masked = false;
            uint32_t regOff = 0u;
            uint64_t value = 0u;
            std::vector<uint8_t> data;
            VsyncRegs vsync{};
        };

        struct Chunk
        {
            uint32_t offset = 0u;
            uint32_t size = 0u;
            uint32_t srcAddr = 0u;
            uint32_t srcKind = 0u;
        };

        struct Work
        {
            std::vector<uint8_t> bytes;
            std::vector<Chunk> chunks;
            uint32_t fbrst = 0u;
            uint64_t sequence = 0u;
            bool mayChangeMask = false;
            std::vector<Op> ops;
            size_t opCount = 0u; // ops[0..opCount) are live; the rest are reusable
            std::atomic<bool> done{false};
        };

        struct LogEntry
        {
            Work *work = nullptr; // marker when set
            Op op;
        };

        struct Expected
        {
            OpKind kind = OpKind::Submit;
            GifPathId path = GifPathId::Path1;
            bool continuation = false;
            uint32_t regOff = 0u;
            uint64_t value = 0u;
            uint32_t size = 0u;
            uint64_t hash = 0u;
            std::vector<uint8_t> head;
        };

        void workerMain();
        Work *acquireWork();
        void releaseWork(Work *work);
        Op &appendWorkerOp(Work &work);
        void replay(const Op &op);
        void replayLog(bool wait, Vu1SyncReason reason);
        bool replayFront(bool wait, Vu1SyncReason reason);
        void present(uint64_t tick, VsyncRegs regs);
        void waitFor(Work &work, Vu1SyncReason reason);
        void stopWorker();

        void shadowKick(const Work &work);
        void compareState(Vu1SyncReason reason);
        void verifyFailure(const char *what);
        void pushExpected(Expected &&expected);
        bool popExpected(Expected &out);

        PS2Memory &m_memory;
        VU1Interpreter &m_vu1;
        GS &m_gs;
        const Mode m_mode;
        FbrstProvider m_fbrst;
        bool m_serialForGood = false;

        // Game-thread state.
        std::deque<LogEntry> m_log;
        Work *m_building = nullptr;
        bool m_replaying = false;
        uint32_t m_pendingVsyncs = 0u;
        uint32_t m_outstandingMaskWork = 0u;
        uint64_t m_replayed = 0u;
        uint64_t m_nextSequence = 1u;
        std::vector<Work *> m_freeWork;

        // Shared with the worker.
        std::mutex m_queueMutex;
        std::condition_variable m_queueCv;
        std::deque<Work *> m_queue;
        bool m_stop = false;
        std::atomic<bool> m_workerSleeping{false};
        std::atomic<uint64_t> m_completed{0u};
        std::mutex m_doneMutex;
        std::condition_variable m_doneCv;
        std::atomic<bool> m_eeWaiting{false};
        std::thread m_worker;
        uint32_t m_creatorMxcsr = 0x1F80u;
        uint16_t m_creatorFpuCw = 0x037Fu;

        // Verification shadow (Mode::Verify).
        struct Shadow;
        std::unique_ptr<Shadow> m_shadow;
        std::deque<Expected> m_expected;
        bool m_verifyFailed = false;
        uint64_t m_verifyPackets = 0u;
        uint64_t m_verifyFences = 0u;

        // PS2X_VU1_THREAD_STATS.
        struct Stats
        {
            uint64_t kicks = 0u;
            uint64_t bytes = 0u;
            uint64_t ops = 0u;
            uint64_t deferredEeOps = 0u;
            uint64_t fences[static_cast<size_t>(Vu1SyncReason::Count)]{};
            uint64_t waits[static_cast<size_t>(Vu1SyncReason::Count)]{};
            uint64_t waitNs[static_cast<size_t>(Vu1SyncReason::Count)]{};
            uint64_t maxLog = 0u;
            uint64_t deferredPresents = 0u;
        };
        bool m_statsEnabled = false;
        Stats m_stats;
        uint64_t m_statsLastVsync = 0u;
    };
} // namespace ps2x

#endif
