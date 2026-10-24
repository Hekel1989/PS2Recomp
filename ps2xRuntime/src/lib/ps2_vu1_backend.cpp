#include "runtime/ps2_vu1_backend.h"

#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2x
{
    namespace
    {
        const Vu1Backend *s_backend = nullptr;

        // A VU1 program that has not reached its E-bit by then is stopped,
        // as the interpreter's budget does (runtime MSCAL callback).
        constexpr uint32_t kKickCycles = 65536u;

        void submitPath1(void *user, const uint8_t *data, uint32_t bytes)
        {
            static_cast<PS2Memory *>(user)->submitGifPacket(GifPathId::Path1, data, bytes);
        }

        void capturePacket(void *user, const uint8_t *data, uint32_t bytes)
        {
            static_cast<std::vector<std::vector<uint8_t>> *>(user)->emplace_back(data, data + bytes);
        }

        void toBackend(const VU1State &state, Vu1BackendRegisters &out)
        {
            std::memcpy(out.vf, state.vf, sizeof(out.vf));
            for (uint32_t i = 0; i < 16u; ++i)
                out.vi[i] = static_cast<uint16_t>(state.vi[i]);
            std::memcpy(out.acc, state.acc, sizeof(out.acc));
            out.q = state.q;
            out.p = state.p;
            out.i = state.i;
            out.r = state.r;
            out.status = state.status;
            out.mac = state.mac;
            out.clip = state.clip;
        }

        uint32_t bitsOf(float value)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            return bits;
        }

        // Distance in units in the last place between two float bit patterns
        // (sign-magnitude mapped onto a monotonic integer line).
        uint64_t ulpDistance(uint32_t a, uint32_t b)
        {
            auto ordered = [](uint32_t x) -> int64_t
            {
                return (x & 0x80000000u) ? -static_cast<int64_t>(x & 0x7FFFFFFFu) : static_cast<int64_t>(x);
            };
            const int64_t d = ordered(a) - ordered(b);
            return static_cast<uint64_t>(d < 0 ? -d : d);
        }
    } // namespace

    void setVu1Backend(const Vu1Backend *backend)
    {
        s_backend = backend;
    }

    const Vu1Backend *vu1Backend()
    {
        return s_backend;
    }

    Vu1BackendRunner::Mode Vu1BackendRunner::modeFromEnvironment()
    {
        const char *value = std::getenv("PS2X_VU1_JIT");
        if (!value || !*value || std::strcmp(value, "1") == 0)
            return Mode::On;
        if (std::strcmp(value, "diff") == 0)
            return Mode::Diff;
        return Mode::Off;
    }

    struct Vu1BackendRunner::DiffStats
    {
        std::vector<std::vector<uint8_t>> jitPackets;
        std::vector<std::vector<uint8_t>> refPackets;
        std::vector<uint8_t> dataBefore;
        std::vector<uint8_t> dataJit;

        uint64_t kicks = 0;
        uint64_t identicalKicks = 0;
        uint64_t packets = 0;
        uint64_t identicalPackets = 0;
        uint64_t countMismatch = 0;
        uint64_t sizeMismatch = 0;
        uint64_t words = 0;
        uint64_t diffWords = 0;
        // ulp buckets for differing words: 1, 2-15, 16-255, 256+.
        uint64_t ulp[4] = {};
        uint64_t dataDiffWords = 0;
        uint64_t vfDiffLanes = 0;
        uint64_t viDiffs = 0;
        uint32_t logged = 0;
        uint32_t structuralLogged = 0;
        uint32_t viLogged = 0;
        uint32_t tpcLogged = 0;
        std::vector<std::vector<uint8_t>> prevJitPackets;
        std::vector<std::vector<uint8_t>> prevRefPackets;
        bool prevResume = false;
        bool dumped = false;
        uint32_t pcIn = 0, prevPcIn = 0;
        std::vector<uint8_t> microBefore;
        Kick prevKick{};
        Vu1BackendRegisters prevRegsIn{};
        std::vector<uint8_t> prevDataIn;
        std::vector<uint8_t> prevMicroIn;
        uint32_t prevStart = 0, prevJitTpc = 0, prevRefPc = 0;
        uint64_t lastReportVsync = 0;
    };

    Vu1BackendRunner::Vu1BackendRunner(const Vu1Backend &backend, PS2Memory &memory, VU1Interpreter &vu1, GS &gs,
                                       Mode mode)
        : m_backend(backend), m_memory(memory), m_vu1(vu1), m_gs(gs), m_mode(mode)
    {
        std::fprintf(stderr, "[vu1-jit] VU1 runs on %s%s\n", backend.name,
                     mode == Mode::Diff ? " (diff: interpreter authoritative)" : "");
        if (mode == Mode::Diff)
            m_diff = new DiffStats();
    }

    Vu1BackendRunner::~Vu1BackendRunner()
    {
        if (m_diff)
        {
            report(true);
            delete m_diff;
        }
    }

    Vu1BackendRunner::Stop Vu1BackendRunner::run(const Kick &kick)
    {
        if (m_mode == Mode::Diff)
            return runDiff(kick);

        Vu1Backend::RunArgs args;
        args.startPc = kick.startPc;
        args.resume = kick.resume;
        args.top = kick.top;
        args.itop = kick.itop;
        args.fbrst = kick.fbrst;
        args.cycles = kKickCycles;
        args.microGeneration = m_memory.getVU1CodeGeneration();
        args.sink = submitPath1;
        args.user = &m_memory;
        const Vu1Backend::RunResult result = m_backend.run(args);
        return Stop{result.stoppedByD, result.stoppedByT};
    }

    Vu1BackendRunner::Stop Vu1BackendRunner::runDiff(const Kick &kick)
    {
        DiffStats &d = *m_diff;
        uint8_t *const data = m_memory.getVU1Data();

        // Same starting point for both: the interpreter's registers.
        Vu1BackendRegisters regs{};
        toBackend(m_vu1.state(), regs);
        m_backend.writeRegisters(regs);
        d.dataBefore.assign(data, data + PS2_VU1_DATA_SIZE);
        d.microBefore.assign(m_memory.getVU1Code(), m_memory.getVU1Code() + PS2_VU1_CODE_SIZE);
        d.pcIn = m_vu1.state().pc;

        d.jitPackets.clear();
        Vu1Backend::RunArgs args;
        args.startPc = kick.startPc;
        args.resume = kick.resume;
        args.top = kick.top;
        args.itop = kick.itop;
        args.fbrst = kick.fbrst;
        args.cycles = kKickCycles;
        args.microGeneration = m_memory.getVU1CodeGeneration();
        args.sink = capturePacket;
        args.user = &d.jitPackets;
        // MSCNT continues at the backend's own TPC: both runs of the previous
        // kick started at the same pc and ended at the same E-bit.
        const Vu1Backend::RunResult jitResult = m_backend.run(args);
        Vu1BackendRegisters jitRegs{};
        m_backend.readRegisters(jitRegs);
        d.dataJit.assign(data, data + PS2_VU1_DATA_SIZE);
        std::memcpy(data, d.dataBefore.data(), PS2_VU1_DATA_SIZE);

        // The reference run: its packets go to the GIF exactly as without diff.
        d.refPackets.clear();
        m_vu1.setXgkickCapture(&d.refPackets);
        m_vu1.state().dBitEnabled = (kick.fbrst & (1u << 10)) != 0u;
        m_vu1.state().tBitEnabled = (kick.fbrst & (1u << 11)) != 0u;
        if (kick.resume)
            m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, m_gs, &m_memory,
                         kick.top, kick.itop, kKickCycles);
        else
            m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, m_gs, &m_memory,
                          kick.startPc, kick.top, kick.itop, kKickCycles);
        m_vu1.setXgkickCapture(nullptr);
        if (d.tpcLogged < 6u)
        {
            ++d.tpcLogged;
            std::fprintf(stderr, "[vu1-jit-diff] kick %llu %s start=0x%x end tpc jit=0x%x ref=0x%x finished=%d\n",
                         static_cast<unsigned long long>(d.kicks + 1u), kick.resume ? "MSCNT" : "MSCAL",
                         kick.startPc, jitResult.tpc, m_vu1.state().pc, jitResult.finished ? 1 : 0);
        }
        for (const std::vector<uint8_t> &packet : d.refPackets)
            m_memory.submitGifPacket(GifPathId::Path1, packet.data(), static_cast<uint32_t>(packet.size()));

        // Compare.
        ++d.kicks;
        bool identical = d.jitPackets.size() == d.refPackets.size();
        if (!identical)
            ++d.countMismatch;
        bool structural = !identical;
        for (size_t k = 0; !structural && k < d.refPackets.size(); ++k)
            structural = d.refPackets[k].size() != d.jitPackets[k].size();
        const VU1State &refState = m_vu1.state();
        bool viDiffer = false;
        for (uint32_t r = 1; r < 16u; ++r)
            viDiffer = viDiffer || static_cast<uint16_t>(refState.vi[r]) != jitRegs.vi[r];
        if ((structural && d.structuralLogged < 16u) || (!structural && viDiffer && d.viLogged < 3u))
        {
            if (structural)
                ++d.structuralLogged;
            else
                ++d.viLogged;
            std::fprintf(stderr, "[vu1-jit-diff] kick %llu %s pc=0x%x top=0x%x itop=0x%x packets ref=%zu jit=%zu sizes ref:",
                         static_cast<unsigned long long>(d.kicks), kick.resume ? "MSCNT" : "MSCAL", kick.startPc,
                         kick.top, kick.itop, d.refPackets.size(), d.jitPackets.size());
            for (size_t k = 0; k < d.refPackets.size() && k < 12u; ++k)
                std::fprintf(stderr, " %zu", d.refPackets[k].size());
            std::fprintf(stderr, " jit:");
            for (size_t k = 0; k < d.jitPackets.size() && k < 12u; ++k)
                std::fprintf(stderr, " %zu", d.jitPackets[k].size());
            std::fprintf(stderr, " vi(ref/jit):");
            for (uint32_t r = 1; r < 16u; ++r)
                if (static_cast<uint16_t>(refState.vi[r]) != jitRegs.vi[r])
                    std::fprintf(stderr, " vi%u=%04x/%04x", r, static_cast<uint16_t>(refState.vi[r]), jitRegs.vi[r]);
            std::fprintf(stderr, " startVi:");
            for (uint32_t r = 1; r < 16u; ++r)
                std::fprintf(stderr, " %04x", regs.vi[r]);
            std::fprintf(stderr, "\n");
            auto head = [](const char *who, size_t k, const std::vector<uint8_t> &packet)
            {
                uint64_t q[4] = {};
                std::memcpy(q, packet.data(), std::min<size_t>(packet.size(), sizeof(q)));
                std::fprintf(stderr, "[vu1-jit-diff]   %s[%zu] %zu bytes %016llx %016llx | %016llx %016llx\n", who, k,
                             packet.size(), static_cast<unsigned long long>(q[0]), static_cast<unsigned long long>(q[1]),
                             static_cast<unsigned long long>(q[2]), static_cast<unsigned long long>(q[3]));
            };
            if (const char *dumpPath = std::getenv("PS2X_VU1_JIT_DIFF_DUMP"); dumpPath && structural && !d.dumped)
            {
                // Both kicks' inputs, for a local replay (tests/vu1/vu1_jit_test.cpp).
                d.dumped = true;
                if (std::FILE *f = std::fopen(dumpPath, "wb"))
                {
                    auto putKick = [&](const Kick &k, uint32_t pcIn, const Vu1BackendRegisters &r,
                                       const std::vector<uint8_t> &dat, const std::vector<uint8_t> &mic)
                    {
                        const uint32_t header[7] = {0x4B4A5556u, k.resume ? 1u : 0u, k.startPc, k.top, k.itop, k.fbrst,
                                                    pcIn};
                        std::fwrite(header, sizeof(header), 1, f);
                        std::fwrite(&r, sizeof(r), 1, f);
                        std::fwrite(dat.data(), 1, PS2_VU1_DATA_SIZE, f);
                        std::fwrite(mic.data(), 1, PS2_VU1_CODE_SIZE, f);
                    };
                    putKick(d.prevKick, d.prevPcIn, d.prevRegsIn, d.prevDataIn, d.prevMicroIn);
                    putKick(kick, d.pcIn, regs, d.dataBefore, d.microBefore);
                    std::fclose(f);
                    std::fprintf(stderr, "[vu1-jit-diff] wrote %s\n", dumpPath);
                }
            }
            if (d.structuralLogged <= 2u)
            {
                for (size_t k = 0; k < d.prevRefPackets.size(); ++k)
                    head("prevRef", k, d.prevRefPackets[k]);
                for (size_t k = 0; k < d.prevJitPackets.size(); ++k)
                    head("prevJit", k, d.prevJitPackets[k]);
                for (size_t k = 0; k < d.refPackets.size(); ++k)
                    head("ref", k, d.refPackets[k]);
                for (size_t k = 0; k < d.jitPackets.size(); ++k)
                    head("jit", k, d.jitPackets[k]);
                std::fprintf(stderr, "[vu1-jit-diff]   prev kick %s start=0x%x jit tpc=0x%x ref pc=0x%x\n",
                             d.prevResume ? "MSCNT" : "MSCAL", d.prevStart, d.prevJitTpc, d.prevRefPc);
            }
        }
        const size_t common = std::min(d.jitPackets.size(), d.refPackets.size());
        d.packets += d.refPackets.size();
        for (size_t k = 0; k < common; ++k)
        {
            const std::vector<uint8_t> &a = d.refPackets[k];
            const std::vector<uint8_t> &b = d.jitPackets[k];
            if (a.size() != b.size())
            {
                ++d.sizeMismatch;
                identical = false;
                continue;
            }
            uint64_t diffs = 0;
            for (size_t w = 0; w + 4u <= a.size(); w += 4u)
            {
                uint32_t x, y;
                std::memcpy(&x, a.data() + w, 4);
                std::memcpy(&y, b.data() + w, 4);
                ++d.words;
                if (x == y)
                    continue;
                ++diffs;
                const uint64_t u = ulpDistance(x, y);
                ++d.ulp[u <= 1u ? 0 : u < 16u ? 1 : u < 256u ? 2 : 3];
                if (d.logged < 8u)
                {
                    ++d.logged;
                    std::fprintf(stderr,
                                 "[vu1-jit-diff] kick %llu pc=0x%x packet %zu qw %zu.%zu ref=%08x jit=%08x ulp=%llu\n",
                                 static_cast<unsigned long long>(d.kicks), kick.startPc, k, w / 16u, (w % 16u) / 4u,
                                 x, y, static_cast<unsigned long long>(u));
                }
            }
            d.diffWords += diffs;
            if (diffs == 0)
                ++d.identicalPackets;
            else
                identical = false;
        }
        for (uint32_t w = 0; w < PS2_VU1_DATA_SIZE; w += 4u)
        {
            if (std::memcmp(d.dataJit.data() + w, data + w, 4) != 0)
            {
                ++d.dataDiffWords;
                identical = false;
            }
        }
        const VU1State &ref = m_vu1.state();
        for (uint32_t r = 0; r < 32u; ++r)
            for (uint32_t c = 0; c < 4u; ++c)
                if (bitsOf(ref.vf[r][c]) != bitsOf(jitRegs.vf[r][c]))
                    ++d.vfDiffLanes;
        for (uint32_t r = 1; r < 16u; ++r)
            if (static_cast<uint16_t>(ref.vi[r]) != jitRegs.vi[r])
                ++d.viDiffs;
        if (identical)
            ++d.identicalKicks;
        d.prevRefPackets.swap(d.refPackets);
        d.prevJitPackets.swap(d.jitPackets);
        d.prevKick = kick;
        d.prevPcIn = d.pcIn;
        d.prevRegsIn = regs;
        d.prevDataIn.swap(d.dataBefore);
        d.prevMicroIn.swap(d.microBefore);
        d.prevResume = kick.resume;
        d.prevStart = kick.startPc;
        d.prevJitTpc = jitResult.tpc;
        d.prevRefPc = m_vu1.state().pc;
        report(false);
        return Stop{m_vu1.state().stoppedByD, m_vu1.state().stoppedByT};
    }

    void Vu1BackendRunner::report(bool force)
    {
        DiffStats &d = *m_diff;
        const uint64_t vsync = m_memory.gs().vsyncTick.load(std::memory_order_relaxed);
        if (!force && vsync < d.lastReportVsync + 300u)
            return;
        d.lastReportVsync = vsync;
        std::fprintf(stderr,
                     "[vu1-jit-diff] vsync=%llu kicks=%llu identical=%llu packets=%llu identical=%llu "
                     "countMismatch=%llu sizeMismatch=%llu words=%llu diffWords=%llu "
                     "ulp(1/<16/<256/more)=%llu/%llu/%llu/%llu vuDataDiffWords=%llu vfDiffLanes=%llu viDiffs=%llu\n",
                     static_cast<unsigned long long>(vsync), static_cast<unsigned long long>(d.kicks),
                     static_cast<unsigned long long>(d.identicalKicks), static_cast<unsigned long long>(d.packets),
                     static_cast<unsigned long long>(d.identicalPackets),
                     static_cast<unsigned long long>(d.countMismatch), static_cast<unsigned long long>(d.sizeMismatch),
                     static_cast<unsigned long long>(d.words), static_cast<unsigned long long>(d.diffWords),
                     static_cast<unsigned long long>(d.ulp[0]), static_cast<unsigned long long>(d.ulp[1]),
                     static_cast<unsigned long long>(d.ulp[2]), static_cast<unsigned long long>(d.ulp[3]),
                     static_cast<unsigned long long>(d.dataDiffWords), static_cast<unsigned long long>(d.vfDiffLanes),
                     static_cast<unsigned long long>(d.viDiffs));
    }
} // namespace ps2x
