#include "runtime/ps2_vu1_thread.h"

#include "ThreadNaming.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

extern "C" void ps2xProfRegisterThread(const char *name) __attribute__((weak));

namespace ps2x
{
    namespace
    {
        thread_local bool t_onWorker = false;
        thread_local void *t_work = nullptr;

        // Spin before sleeping: a VIF1 kick arrives every ~50-100 us during a
        // 3D frame, so a short spin avoids a futex wake per kick.
        constexpr int kWorkerSpins = 4000;
        constexpr int kWaitSpins = 4000;

        inline void cpuRelax()
        {
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }

        uint64_t nowNs()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
        }

        uint64_t hashBytes(const uint8_t *data, uint32_t size)
        {
            uint64_t h = 0x243F6A8885A308D3ull ^ size;
            uint32_t i = 0;
            for (; i + 8u <= size; i += 8u)
            {
                uint64_t word = 0;
                std::memcpy(&word, data + i, sizeof(word));
                h ^= word;
                h *= 0x9E3779B97F4A7C15ull;
                h ^= h >> 29;
            }
            for (; i < size; ++i)
            {
                h ^= data[i];
                h *= 0x100000001B3ull;
            }
            return h;
        }

        const char *reasonName(Vu1SyncReason reason)
        {
            switch (reason)
            {
            case Vu1SyncReason::VBlank: return "vblank";
            case Vu1SyncReason::Vif1Register: return "vif1reg";
            case Vu1SyncReason::VuMemory: return "vumem";
            case Vu1SyncReason::GsRegister: return "gsreg";
            case Vu1SyncReason::GifStatus: return "gifstat";
            case Vu1SyncReason::GifFastPath: return "giffast";
            case Vu1SyncReason::GsDirect: return "gsdirect";
            case Vu1SyncReason::DebugBits: return "debugbits";
            case Vu1SyncReason::Diagnostics: return "diag";
            case Vu1SyncReason::Shutdown: return "shutdown";
            case Vu1SyncReason::Count: break;
            }
            return "?";
        }

        bool envSet(const char *name)
        {
            const char *value = std::getenv(name);
            return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }
    } // namespace

    struct Vu1Thread::Shadow
    {
        PS2Memory memory;
        GifArbiter arbiter;
        VU1Interpreter vu1{VU1Interpreter::Unit::VU1};
        uint32_t fbrst = 0u;
    };

    Vu1Thread::Mode Vu1Thread::modeFromEnvironment()
    {
        Mode mode = Mode::Threaded;
        if (const char *value = std::getenv("PS2X_VU1_THREAD"); value != nullptr)
        {
            if (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0 ||
                std::strcmp(value, "serial") == 0 || std::strcmp(value, "false") == 0)
                mode = Mode::Serial;
            else if (std::strcmp(value, "verify") == 0)
                mode = Mode::Verify;
        }
        // These captures keep plain (non-atomic) global state that the worker
        // and the game thread would both touch.
        if (mode != Mode::Serial && (envSet("PS2X_VU1_OVERLAP") || envSet("PS2X_VU1_INVENTORY")))
        {
            std::fprintf(stderr, "[vu1-thread] serial: PS2X_VU1_OVERLAP/PS2X_VU1_INVENTORY need the serial path\n");
            mode = Mode::Serial;
        }
        return mode;
    }

    Vu1Thread::Vu1Thread(PS2Memory &memory, VU1Interpreter &vu1, GS &gs, Mode mode, FbrstProvider fbrst)
        : m_memory(memory), m_vu1(vu1), m_gs(gs), m_mode(mode), m_fbrst(std::move(fbrst))
    {
        m_statsEnabled = envSet("PS2X_VU1_THREAD_STATS");
        m_predictor = std::make_unique<PS2Memory>();
        m_predictor->initializeVif1Predictor(m_memory);
#if defined(__x86_64__) || defined(_M_X64)
        m_creatorMxcsr = _mm_getcsr();
#if defined(__GNUC__) || defined(__clang__)
        __asm__ volatile("fnstcw %0" : "=m"(m_creatorFpuCw));
#endif
#endif
        if (m_mode == Mode::Verify)
        {
            m_shadow = std::make_unique<Shadow>();
            Shadow &shadow = *m_shadow;
            shadow.memory.initializeVif1Shadow(m_memory);
            shadow.vu1.reset();
            shadow.arbiter.setProcessPacketFn(
                [this](GifPathId pathId, const uint8_t *data, uint32_t size, bool continuation)
                {
                    Expected expected;
                    expected.kind = OpKind::Submit;
                    expected.path = pathId;
                    expected.continuation = continuation;
                    expected.size = size;
                    expected.hash = hashBytes(data, size);
                    expected.head.assign(data, data + std::min<uint32_t>(size, 64u));
                    pushExpected(std::move(expected));
                });
            shadow.memory.setGifArbiter(&shadow.arbiter);
            shadow.memory.setVu1MscalCallback(
                [this](uint32_t startPC, uint32_t top, uint32_t itop)
                {
                    Shadow &s = *m_shadow;
                    s.vu1.state().dBitEnabled = (s.fbrst & (1u << 10)) != 0u;
                    s.vu1.state().tBitEnabled = (s.fbrst & (1u << 11)) != 0u;
                    s.vu1.execute(s.memory.getVU1Code(), PS2_VU1_CODE_SIZE, s.memory.getVU1Data(),
                                  PS2_VU1_DATA_SIZE, m_gs, &s.memory, startPC, top, itop, 65536);
                });
            shadow.memory.setVu1MscntCallback(
                [this](uint32_t top, uint32_t itop)
                {
                    Shadow &s = *m_shadow;
                    s.vu1.state().dBitEnabled = (s.fbrst & (1u << 10)) != 0u;
                    s.vu1.state().tBitEnabled = (s.fbrst & (1u << 11)) != 0u;
                    s.vu1.resume(s.memory.getVU1Code(), PS2_VU1_CODE_SIZE, s.memory.getVU1Data(),
                                 PS2_VU1_DATA_SIZE, m_gs, &s.memory, top, itop, 65536);
                });
        }
        // PS2Memory's constructor clears the process-wide scratchpad pointer;
        // the real memory still owns it.
        ps2SetScratchpadHostPtr(m_memory.getScratchpad());
        std::fprintf(stderr, "[vu1-thread] mode=%s\n", m_mode == Mode::Verify ? "verify" : "threaded");
        m_worker = std::thread([this] { workerMain(); });
    }

    Vu1Thread::~Vu1Thread()
    {
        sync(Vu1SyncReason::Shutdown);
        stopWorker();
        if (m_shadow)
        {
            std::fprintf(stderr, "[vu1-thread] verify %s: packets=%" PRIu64 " fences=%" PRIu64 "\n",
                         m_verifyFailed ? "FAILED" : "ok", m_verifyPackets, m_verifyFences);
        }
        for (Work *work : m_freeWork)
            delete work;
    }

    bool Vu1Thread::onWorker()
    {
        return t_onWorker;
    }

    void Vu1Thread::stopWorker()
    {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_stop = true;
        }
        m_queueCv.notify_all();
        if (m_worker.joinable())
            m_worker.join();
    }

    void Vu1Thread::workerMain()
    {
        ThreadNaming::SetCurrentThreadName("ps2x-vu1");
        if (ps2xProfRegisterThread != nullptr)
            ps2xProfRegisterThread("VU1");
#if defined(__x86_64__) || defined(_M_X64)
        _mm_setcsr(m_creatorMxcsr);
#if defined(__GNUC__) || defined(__clang__)
        __asm__ volatile("fldcw %0" : : "m"(m_creatorFpuCw));
#endif
#endif
        t_onWorker = true;

        for (;;)
        {
            Work *work = nullptr;
            // Spin on the push counter, not the mutex: a contended glibc mutex
            // puts the game thread to sleep instead of spinning.
            for (int spin = 0; spin < kWorkerSpins && m_pushed.load(std::memory_order_acquire) == m_taken; ++spin)
                cpuRelax();
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                if (m_queue.empty() && !m_stop)
                {
                    m_workerSleeping.store(true);
                    m_queueCv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                    m_workerSleeping.store(false);
                }
                if (m_queue.empty())
                    break; // stopping
                work = m_queue.front();
                m_queue.pop_front();
                ++m_taken;
            }

            t_work = work;
            try
            {
                for (const Chunk &chunk : work->chunks)
                {
                    m_memory.m_vif1ChunkSrcAddr = chunk.srcAddr;
                    m_memory.m_vif1ChunkSrcKind = chunk.srcKind;
                    m_memory.processVIF1Data(work->bytes.data() + chunk.offset, chunk.size);
                }
            }
            catch (const std::exception &e)
            {
                std::fprintf(stderr, "[vu1-thread] exception in VIF1 work %" PRIu64 ": %s\n", work->sequence,
                             e.what());
            }
            catch (...)
            {
                std::fprintf(stderr, "[vu1-thread] unknown exception in VIF1 work %" PRIu64 "\n", work->sequence);
            }
            m_memory.m_vif1ChunkSrcAddr = 0u;
            m_memory.m_vif1ChunkSrcKind = 0u;
            t_work = nullptr;

            work->done.store(true);
            m_completed.fetch_add(1u);
            if (m_eeWaiting.load())
            {
                std::lock_guard<std::mutex> lock(m_doneMutex);
                m_doneCv.notify_all();
            }
        }
        t_onWorker = false;
    }

    Vu1Thread::Work *Vu1Thread::acquireWork()
    {
        Work *work = nullptr;
        if (!m_freeWork.empty())
        {
            work = m_freeWork.back();
            m_freeWork.pop_back();
        }
        else
        {
            work = new Work();
        }
        work->bytes.clear();
        work->chunks.clear();
        work->opCount = 0u;
        work->fbrst = 0u;
        work->done.store(false, std::memory_order_relaxed);
        return work;
    }

    void Vu1Thread::releaseWork(Work *work)
    {
        // Keep the vectors' capacity; a frame's kicks reuse the same buffers.
        for (size_t i = 0; i < work->opCount; ++i)
            work->ops[i].data.clear();
        work->opCount = 0u;
        if (work->bytes.capacity() > (4u << 20))
            std::vector<uint8_t>().swap(work->bytes);
        m_freeWork.push_back(work);
    }

    Vu1Thread::Op &Vu1Thread::appendWorkerOp(Work &work)
    {
        if (work.opCount == work.ops.size())
            work.ops.emplace_back();
        Op &op = work.ops[work.opCount++];
        op.data.clear();
        return op;
    }

    uint32_t Vu1Thread::workerFbrst() const
    {
        const Work *work = static_cast<const Work *>(t_work);
        return work != nullptr ? work->fbrst : 0u;
    }

    bool Vu1Thread::beginKick()
    {
        if (m_serialForGood)
            return false;
        const uint32_t fbrst = m_fbrst ? m_fbrst() : 0u;
        // FBRST.DE1/TE1 let VU1 stop on D/T bits and write VPU-STAT, which the
        // game reads inline.  Retail games leave them clear; if one sets them,
        // run that kick and every later one on the serial path.
        if ((fbrst & 0x0C00u) != 0u)
        {
            sync(Vu1SyncReason::DebugBits);
            m_serialForGood = true;
            if (m_shadow)
            {
                std::fprintf(stderr, "[vu1-thread] verify stopped: FBRST DE1/TE1 set, serial from now on\n");
                m_shadow.reset();
            }
            std::fprintf(stderr, "[vu1-thread] FBRST=0x%x sets DE1/TE1: VU1 runs serially from now on\n", fbrst);
            return false;
        }
        m_building = acquireWork();
        m_building->fbrst = fbrst;
        return true;
    }

    void Vu1Thread::addChunk(const uint8_t *data, uint32_t sizeBytes, uint32_t srcAddr, uint32_t srcKind)
    {
        if (m_building == nullptr || data == nullptr || sizeBytes == 0u)
            return;
        Chunk chunk;
        chunk.offset = static_cast<uint32_t>(m_building->bytes.size());
        chunk.size = sizeBytes;
        chunk.srcAddr = srcAddr;
        chunk.srcKind = srcKind;
        m_building->bytes.insert(m_building->bytes.end(), data, data + sizeBytes);
        m_building->chunks.push_back(chunk);
    }

    void Vu1Thread::endKick()
    {
        Work *work = m_building;
        m_building = nullptr;
        if (work == nullptr)
            return;
        if (work->chunks.empty())
        {
            releaseWork(work);
            return;
        }
        work->sequence = m_nextSequence++;
        for (const Chunk &chunk : work->chunks)
            m_predictor->processVIF1Data(work->bytes.data() + chunk.offset, chunk.size);
        if (m_shadow)
            shadowKick(*work);

        LogEntry entry;
        entry.work = work;
        m_log.push_back(std::move(entry));
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_queue.push_back(work);
            m_pushed.fetch_add(1u, std::memory_order_release);
        }
        if (m_workerSleeping.load())
            m_queueCv.notify_one();

        if (m_statsEnabled)
        {
            ++m_stats.kicks;
            m_stats.bytes += work->bytes.size();
            if (m_log.size() > m_stats.maxLog)
                m_stats.maxLog = m_log.size();
        }
        pump();
    }

    bool Vu1Thread::routeSubmit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drain,
                                bool path2DirectHl, bool directContinuation)
    {
        if (t_onWorker)
        {
            Work *work = static_cast<Work *>(t_work);
            if (work == nullptr)
                return false;
            Op &op = appendWorkerOp(*work);
            op.kind = OpKind::Submit;
            op.path = pathId;
            op.drain = drain;
            op.directHl = path2DirectHl;
            op.continuation = directContinuation;
            op.data.assign(data, data + sizeBytes);
            return true;
        }
        if (m_replaying)
            return false;
        if (m_shadow)
            m_shadow->memory.submitGifPacket(pathId, data, sizeBytes, drain, path2DirectHl, directContinuation);
        if (!m_log.empty())
            pump();
        if (m_log.empty())
            return false;
        LogEntry entry;
        entry.op.kind = OpKind::Submit;
        entry.op.path = pathId;
        entry.op.drain = drain;
        entry.op.directHl = path2DirectHl;
        entry.op.continuation = directContinuation;
        entry.op.data.assign(data, data + sizeBytes);
        m_log.push_back(std::move(entry));
        if (m_statsEnabled)
            ++m_stats.deferredEeOps;
        return true;
    }

    bool Vu1Thread::routeDrain()
    {
        if (t_onWorker || m_replaying)
            return false;
        if (m_shadow && m_shadow->memory.m_gifArbiter != nullptr)
            m_shadow->memory.m_gifArbiter->drain();
        if (!m_log.empty())
            pump();
        if (m_log.empty())
            return false;
        LogEntry entry;
        entry.op.kind = OpKind::Drain;
        m_log.push_back(std::move(entry));
        if (m_statsEnabled)
            ++m_stats.deferredEeOps;
        return true;
    }

    bool Vu1Thread::routePath3Mask(bool masked)
    {
        if (!t_onWorker)
            return false;
        Work *work = static_cast<Work *>(t_work);
        if (work == nullptr)
            return false;
        Op &op = appendWorkerOp(*work);
        op.kind = OpKind::Path3Mask;
        op.masked = masked;
        return true;
    }

    bool Vu1Thread::routePrivReg(uint32_t regOff, uint64_t value)
    {
        if (t_onWorker || m_replaying)
            return false;
        if (m_shadow)
        {
            Expected expected;
            expected.kind = OpKind::PrivReg;
            expected.regOff = regOff;
            expected.value = value;
            pushExpected(std::move(expected));
        }
        if (!m_log.empty())
            pump();
        if (m_log.empty())
            return false;
        LogEntry entry;
        entry.op.kind = OpKind::PrivReg;
        entry.op.regOff = regOff;
        entry.op.value = value;
        m_log.push_back(std::move(entry));
        if (m_statsEnabled)
            ++m_stats.deferredEeOps;
        return true;
    }

    void Vu1Thread::replay(const Op &op)
    {
        // PS2X_VU1_THREAD_FAULT=<n>: drop the n-th replayed packet submission,
        // to show that PS2X_VU1_THREAD=verify catches a broken GS stream.  (A
        // dropped drain is often legitimately invisible: the next drain emits
        // the same packets in the same order.)
        static const uint64_t s_faultAt = []
        {
            const char *value = std::getenv("PS2X_VU1_THREAD_FAULT");
            return value != nullptr ? std::strtoull(value, nullptr, 10) : 0ull;
        }();
        if (s_faultAt != 0u && op.kind == OpKind::Submit && ++m_replayed == s_faultAt)
        {
            std::fprintf(stderr, "[vu1-thread] fault injection: dropped replayed op %" PRIu64 " (kind %u)\n",
                         m_replayed, static_cast<unsigned>(op.kind));
            return;
        }
        switch (op.kind)
        {
        case OpKind::Submit:
            m_memory.submitGifPacketDirect(op.path, op.data.data(), static_cast<uint32_t>(op.data.size()), op.drain,
                                           op.directHl, op.continuation);
            break;
        case OpKind::Drain:
            m_memory.drainGifArbiterDirect();
            break;
        case OpKind::Path3Mask:
            m_memory.setPath3MaskDirect(op.masked);
            break;
        case OpKind::PrivReg:
            m_memory.notifyGsPrivRegWriteDirect(op.regOff, op.value);
            break;
        case OpKind::Vsync:
            if (m_pendingVsyncs > 0u)
                --m_pendingVsyncs;
            present(op.value, op.vsync);
            break;
        }
    }

    void Vu1Thread::waitFor(Work &work, Vu1SyncReason reason)
    {
        const uint64_t start = nowNs();
        bool done = false;
        for (int spin = 0; spin < kWaitSpins; ++spin)
        {
            if (work.done.load())
            {
                done = true;
                break;
            }
            cpuRelax();
        }
        if (!done)
        {
            std::unique_lock<std::mutex> lock(m_doneMutex);
            m_eeWaiting.store(true);
            m_doneCv.wait(lock, [&work] { return work.done.load(); });
            m_eeWaiting.store(false);
        }
        const uint64_t waited = nowNs() - start;
        if (m_waitObserver)
            m_waitObserver(waited);
        if (m_statsEnabled)
        {
            const size_t index = static_cast<size_t>(reason);
            ++m_stats.waits[index];
            m_stats.waitNs[index] += waited;
        }
    }

    bool Vu1Thread::replayFront(bool wait, Vu1SyncReason reason)
    {
        LogEntry &entry = m_log.front();
        if (entry.work != nullptr)
        {
            Work *work = entry.work;
            if (!work->done.load(std::memory_order_acquire))
            {
                if (!wait)
                    return false;
                waitFor(*work, reason);
            }
            for (size_t i = 0; i < work->opCount; ++i)
                replay(work->ops[i]);
            if (m_statsEnabled)
                m_stats.ops += work->opCount;
            releaseWork(work);
        }
        else
        {
            replay(entry.op);
        }
        m_log.pop_front();
        return true;
    }

    void Vu1Thread::replayLog(bool wait, Vu1SyncReason reason)
    {
        if (m_replaying)
            return;
        m_replaying = true;
        while (!m_log.empty() && replayFront(wait, reason))
        {
        }
        m_replaying = false;
    }

    Vu1Thread::VsyncRegs Vu1Thread::captureVsyncRegs(const GSRegisters &r)
    {
        VsyncRegs regs{};
        regs.pmode = r.pmode;
        regs.smode1 = r.smode1;
        regs.smode2 = r.smode2;
        regs.srfsh = r.srfsh;
        regs.synch1 = r.synch1;
        regs.synch2 = r.synch2;
        regs.syncv = r.syncv;
        regs.dispfb1 = r.dispfb1;
        regs.display1 = r.display1;
        regs.dispfb2 = r.dispfb2;
        regs.display2 = r.display2;
        regs.extbuf = r.extbuf;
        regs.extdata = r.extdata;
        regs.extwrite = r.extwrite;
        regs.bgcolor = r.bgcolor;
        regs.csr = r.csr.load(std::memory_order_acquire);
        regs.imr = r.imr;
        regs.busdir = r.busdir;
        regs.siglblid = r.siglblid;
        return regs;
    }

    void Vu1Thread::beforeVblank()
    {
        if (t_onWorker || m_replaying || m_pendingVsyncs == 0u)
            return;
        if (m_statsEnabled)
            ++m_stats.fences[static_cast<size_t>(Vu1SyncReason::VBlank)];
        m_replaying = true;
        while (m_pendingVsyncs > 0u && !m_log.empty() && replayFront(true, Vu1SyncReason::VBlank))
        {
        }
        m_replaying = false;
    }

    void Vu1Thread::vblankPresent(uint64_t tick, const VsyncRegs &regs)
    {
        if (m_shadow)
        {
            Expected expected;
            expected.kind = OpKind::Vsync;
            expected.value = tick;
            pushExpected(std::move(expected));
        }
        if (!m_log.empty())
            pump();
        if (m_log.empty() || m_replaying)
        {
            present(tick, regs);
            return;
        }
        LogEntry entry;
        entry.op.kind = OpKind::Vsync;
        entry.op.value = tick;
        entry.op.vsync = regs;
        m_log.push_back(std::move(entry));
        ++m_pendingVsyncs;
        if (m_statsEnabled)
            ++m_stats.deferredPresents;
    }

    void Vu1Thread::present(uint64_t tick, VsyncRegs regs)
    {
        // CSR and SIGLBLID are what the replayed SIGNAL/FINISH/LABEL packets
        // left; nothing else may change them before this present, because
        // every game-thread CSR/SIGLBLID access fences.
        const GSRegisters &live = m_memory.gs();
        regs.csr = live.csr.load(std::memory_order_acquire);
        regs.siglblid = live.siglblid;
        if (m_shadow && !m_verifyFailed)
        {
            Expected expected;
            if (!popExpected(expected) || expected.kind != OpKind::Vsync || expected.value != tick)
            {
                char what[160];
                std::snprintf(what, sizeof(what), "present vsync=%" PRIu64 " vs shadow kind=%u value=%" PRIu64, tick,
                              static_cast<unsigned>(expected.kind), expected.value);
                verifyFailure(what);
            }
        }
        if (const auto &hooks = Ps2GsHostHooks::hooks(); hooks.vsyncRegs)
            hooks.vsyncRegs(tick, regs);
    }

    void Vu1Thread::pump()
    {
        if (!m_log.empty() && !t_onWorker)
            replayLog(false, Vu1SyncReason::Count);
    }

    bool Vu1Thread::path3MaskForRead()
    {
        if (m_serialForGood)
            return m_memory.m_path3Masked;
        const bool masked = m_predictor->m_path3Masked;
        if (m_shadow && !m_verifyFailed && m_shadow->memory.m_path3Masked != masked)
            verifyFailure("predicted PATH3 mask at a GIF_STAT read");
        return masked;
    }

    void Vu1Thread::checkPredictor(Vu1SyncReason reason)
    {
        // After a full fence the real VIF1 state is the serial one; the parse
        // predictor must agree.  Cheap enough to check at every fence.
        if (!m_log.empty() || m_replaying || m_serialForGood)
            return;
        const PS2Memory &p = *m_predictor;
        const PS2Memory &r = m_memory;
        if (std::memcmp(&p.vif1_regs, &r.vif1_regs, sizeof(r.vif1_regs)) == 0 &&
            p.m_path3Masked == r.m_path3Masked &&
            p.m_vif1PendingPath2ImageQwc == r.m_vif1PendingPath2ImageQwc &&
            p.m_vif1PendingPath2DirectHl == r.m_vif1PendingPath2DirectHl &&
            p.m_vif1PendingPath2FromNextDirect == r.m_vif1PendingPath2FromNextDirect)
            return;
        if (!m_predictorDriftLogged)
        {
            m_predictorDriftLogged = true;
            std::fprintf(stderr, "[vu1-thread] VIF1 parse predictor drifted from the real state at a %s fence\n",
                         reasonName(reason));
        }
        if (m_shadow)
            verifyFailure("VIF1 parse predictor drifted");
        m_predictor->vif1_regs = r.vif1_regs;
        m_predictor->m_path3Masked = r.m_path3Masked;
        m_predictor->m_vif1PendingPath2ImageQwc = r.m_vif1PendingPath2ImageQwc;
        m_predictor->m_vif1PendingPath2DirectHl = r.m_vif1PendingPath2DirectHl;
        m_predictor->m_vif1PendingPath2FromNextDirect = r.m_vif1PendingPath2FromNextDirect;
    }

    void Vu1Thread::sync(Vu1SyncReason reason)
    {
        if (t_onWorker || m_replaying)
            return;
        if (m_statsEnabled)
            ++m_stats.fences[static_cast<size_t>(reason)];
        if (!m_log.empty())
            replayLog(true, reason);
        checkPredictor(reason);
        if (m_shadow)
            compareState(reason);
    }

    void Vu1Thread::onVsync(uint64_t vsyncTick)
    {
        // The frontend leaves through _Exit(), so the destructor's summary
        // never prints on a normal shutdown; report verification progress here.
        if (m_shadow && vsyncTick % 600u == 0u)
        {
            std::fprintf(stderr, "[vu1-thread] verify %s at vsync=%" PRIu64 ": packets=%" PRIu64 " fences=%" PRIu64 "\n",
                         m_verifyFailed ? "FAILED" : "ok", vsyncTick, m_verifyPackets, m_verifyFences);
        }
        if (!m_statsEnabled || vsyncTick < m_statsLastVsync + 60u)
            return;
        m_statsLastVsync = vsyncTick;
        std::fprintf(stderr,
                     "[vu1-thread] vsync=%" PRIu64 " kicks=%" PRIu64 " kb=%" PRIu64 " ops=%" PRIu64
                     " deferred-ee=%" PRIu64 " max-log=%" PRIu64 " late-presents=%" PRIu64,
                     vsyncTick, m_stats.kicks, m_stats.bytes / 1024u, m_stats.ops, m_stats.deferredEeOps,
                     m_stats.maxLog, m_stats.deferredPresents);
        for (size_t i = 0; i < static_cast<size_t>(Vu1SyncReason::Count); ++i)
        {
            if (m_stats.fences[i] == 0u)
                continue;
            std::fprintf(stderr, " %s=%" PRIu64 "/%" PRIu64 "w/%.2fms", reasonName(static_cast<Vu1SyncReason>(i)),
                         m_stats.fences[i], m_stats.waits[i], static_cast<double>(m_stats.waitNs[i]) / 1e6);
        }
        std::fprintf(stderr, "\n");
        m_stats = Stats{};
    }

    // --- verification ------------------------------------------------------

    void Vu1Thread::shadowKick(const Work &work)
    {
        Shadow &shadow = *m_shadow;
        shadow.fbrst = work.fbrst;
        for (const Chunk &chunk : work.chunks)
        {
            shadow.memory.m_vif1ChunkSrcAddr = chunk.srcAddr;
            shadow.memory.m_vif1ChunkSrcKind = chunk.srcKind;
            shadow.memory.processVIF1Data(work.bytes.data() + chunk.offset, chunk.size);
        }
        shadow.memory.m_vif1ChunkSrcAddr = 0u;
        shadow.memory.m_vif1ChunkSrcKind = 0u;
    }

    void Vu1Thread::pushExpected(Expected &&expected)
    {
        if (!m_verifyFailed)
            m_expected.push_back(std::move(expected));
    }

    bool Vu1Thread::popExpected(Expected &out)
    {
        if (m_expected.empty())
            return false;
        out = std::move(m_expected.front());
        m_expected.pop_front();
        return true;
    }

    void Vu1Thread::verifyFailure(const char *what)
    {
        if (m_verifyFailed)
            return;
        m_verifyFailed = true;
        m_expected.clear();
        std::fprintf(stderr, "[vu1-thread] VERIFY MISMATCH after %" PRIu64 " packets, %" PRIu64 " fences: %s\n",
                     m_verifyPackets, m_verifyFences, what);
    }

    void Vu1Thread::checkPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool directContinuation)
    {
        if (!m_shadow || m_verifyFailed)
            return;
        Expected expected;
        if (!popExpected(expected))
        {
            char what[160];
            std::snprintf(what, sizeof(what), "real packet p%u size=%u has no shadow packet",
                          static_cast<unsigned>(pathId), sizeBytes);
            verifyFailure(what);
            return;
        }
        const uint64_t hash = hashBytes(data, sizeBytes);
        if (expected.kind != OpKind::Submit || expected.path != pathId || expected.size != sizeBytes ||
            expected.continuation != directContinuation || expected.hash != hash)
        {
            char what[512];
            int n = std::snprintf(what, sizeof(what),
                                  "packet #%" PRIu64 " real p%u size=%u cont=%d, shadow kind=%u p%u size=%u cont=%d"
                                  " (privreg off=0x%x) hash %016" PRIx64 " vs %016" PRIx64 "; real head ",
                                  m_verifyPackets, static_cast<unsigned>(pathId), sizeBytes,
                                  directContinuation ? 1 : 0, static_cast<unsigned>(expected.kind),
                                  static_cast<unsigned>(expected.path), expected.size,
                                  expected.continuation ? 1 : 0, expected.regOff, hash, expected.hash);
            for (uint32_t i = 0; i < std::min<uint32_t>(sizeBytes, 32u) && n > 0 && n + 3 < (int)sizeof(what); ++i)
                n += std::snprintf(what + n, sizeof(what) - n, "%02x", data[i]);
            verifyFailure(what);
            return;
        }
        ++m_verifyPackets;
    }

    void Vu1Thread::checkPrivReg(uint32_t regOff, uint64_t value)
    {
        if (!m_shadow || m_verifyFailed)
            return;
        Expected expected;
        if (!popExpected(expected) || expected.kind != OpKind::PrivReg || expected.regOff != regOff ||
            expected.value != value)
        {
            char what[200];
            std::snprintf(what, sizeof(what), "privreg real off=0x%x value=0x%016" PRIx64 " vs shadow kind=%u off=0x%x",
                          regOff, value, static_cast<unsigned>(expected.kind), expected.regOff);
            verifyFailure(what);
        }
    }

    void Vu1Thread::mirrorVuWrite(const uint8_t *vuMem, uint32_t offset, const void *data, uint32_t size)
    {
        if (!m_shadow)
            return;
        PS2Memory &shadow = m_shadow->memory;
        if (vuMem == m_memory.getVU1Code())
        {
            std::memcpy(shadow.getVU1Code() + offset, data, size);
            shadow.markVU1CodeModified();
        }
        else if (vuMem == m_memory.getVU1Data())
        {
            std::memcpy(shadow.getVU1Data() + offset, data, size);
        }
    }

    void Vu1Thread::mirrorVif1Register(uint32_t address, uint32_t value)
    {
        m_predictor->writeIORegister(address, value);
        if (m_shadow)
            m_shadow->memory.writeIORegister(address, value);
    }

    void Vu1Thread::compareState(Vu1SyncReason reason)
    {
        if (!m_shadow || m_verifyFailed || m_replaying)
            return;
        ++m_verifyFences;
        const PS2Memory &real = m_memory;
        const PS2Memory &shadow = m_shadow->memory;
        char what[256];
        auto fail = [&](const char *field)
        {
            std::snprintf(what, sizeof(what), "state %s differs at %s fence", field, reasonName(reason));
            verifyFailure(what);
        };
        if (!m_expected.empty())
        {
            std::snprintf(what, sizeof(what), "%zu shadow GS operation(s) never reached the real GS at %s fence",
                          m_expected.size(), reasonName(reason));
            verifyFailure(what);
            return;
        }
        if (std::memcmp(&real.vif1_regs, &shadow.vif1_regs, sizeof(real.vif1_regs)) != 0)
            return fail("vif1_regs");
        if (real.m_vif1PendingPath2ImageQwc != shadow.m_vif1PendingPath2ImageQwc ||
            real.m_vif1PendingPath2DirectHl != shadow.m_vif1PendingPath2DirectHl ||
            real.m_vif1PendingPath2FromNextDirect != shadow.m_vif1PendingPath2FromNextDirect)
            return fail("vif1 pending PATH2");
        if (real.m_path3Masked != shadow.m_path3Masked || real.m_path3MaskedFifo != shadow.m_path3MaskedFifo)
            return fail("PATH3 mask");
        if (std::memcmp(real.getVU1Code(), shadow.getVU1Code(), PS2_VU1_CODE_SIZE) != 0)
            return fail("VU1 code memory");
        if (std::memcmp(real.getVU1Data(), shadow.getVU1Data(), PS2_VU1_DATA_SIZE) != 0)
            return fail("VU1 data memory");
        const VU1State &a = m_vu1.state();
        const VU1State &b = m_shadow->vu1.state();
        if (std::memcmp(a.vf, b.vf, sizeof(a.vf)) != 0 || std::memcmp(a.vi, b.vi, sizeof(a.vi)) != 0 ||
            std::memcmp(a.acc, b.acc, sizeof(a.acc)) != 0)
            return fail("VU1 VF/VI/ACC");
        if (std::memcmp(&a.q, &b.q, sizeof(a.q)) != 0 || std::memcmp(&a.p, &b.p, sizeof(a.p)) != 0 ||
            std::memcmp(&a.i, &b.i, sizeof(a.i)) != 0 || a.r != b.r || a.pc != b.pc || a.mac != b.mac ||
            a.clip != b.clip || a.status != b.status || a.cycles != b.cycles || a.ebit != b.ebit ||
            a.stoppedByD != b.stoppedByD || a.stoppedByT != b.stoppedByT || a.top != b.top || a.itop != b.itop)
            return fail("VU1 scalar state");
    }
} // namespace ps2x
