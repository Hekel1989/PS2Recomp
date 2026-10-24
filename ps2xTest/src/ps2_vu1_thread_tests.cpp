// VU1 thread equivalence test (runtime/ps2_vu1_thread.h, docs/vu1-thread.md).
// Also runs as tests/vu1/vu1_thread_test.cpp in the Suikoden Tactics recomp.
//
// Random VIF1 DMA streams (MPG, UNPACK, MSCAL with an XGKICK program, DIRECT,
// MSKPATH3) are interleaved with game-thread GIF-front work (PATH3 packets
// with and without drains, IMAGE packets, GS privileged-register writes) and
// observation points (VIF1/GIF_STAT reads, VU1 memory reads and writes).  The
// same seeded sequence runs on the serial path, on the VU1 thread with random
// game-thread timing, and in verify mode; the GS-bound stream (every packet
// the arbiter hands on and every privileged-register notification) and the
// final VIF1/VU1 state must be identical.

#include "MiniTest.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_gs_host_hooks.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_thread.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using Mode = ps2x::Vu1Thread::Mode;

    constexpr uint32_t kStreamAddr = 0x00200000u; // reused by every kick
    constexpr uint32_t kVif1Chcr = 0x10009000u;
    constexpr uint32_t kVif1Madr = 0x10009010u;
    constexpr uint32_t kVif1Qwc = 0x10009020u;

    std::vector<std::string> *g_log = nullptr;

    uint64_t hashBytes(const uint8_t *data, uint32_t size)
    {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < size; ++i)
        {
            h ^= data[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    // --- VIF1 stream builder ------------------------------------------------
    struct Stream
    {
        std::vector<uint32_t> words;

        void cmd(uint32_t opcode, uint32_t num, uint32_t imm)
        {
            words.push_back((opcode << 24) | (num << 16) | (imm & 0xFFFFu));
        }
        // Pad with VIF NOPs so the next command's payload starts at a multiple
        // of `align` bytes.
        void alignPayload(uint32_t align)
        {
            while (((words.size() + 1u) * 4u) % align != 0u)
                words.push_back(0u);
        }
        void data64(uint64_t value)
        {
            words.push_back(static_cast<uint32_t>(value));
            words.push_back(static_cast<uint32_t>(value >> 32));
        }
    };

    uint64_t gifTag(uint32_t nloop, bool eop, uint32_t flg, uint32_t nreg)
    {
        return static_cast<uint64_t>(nloop & 0x7FFFu) | (eop ? (1ull << 15) : 0ull) |
               (static_cast<uint64_t>(flg & 3u) << 58) | (static_cast<uint64_t>(nreg & 0xFu) << 60);
    }

    // XGKICK vi00 (packet at VU1 data qword 0) in an E-bit pair, then a NOP pair.
    void appendProgram(Stream &s)
    {
        s.alignPayload(8u);
        s.cmd(0x4A, 2u, 0u); // MPG 2 instructions at 0
        s.data64((static_cast<uint64_t>(0x400002FFu) << 32) | 0x800006FCu);
        s.data64((static_cast<uint64_t>(0x000002FFu) << 32) | 0x8000033Cu);
    }

    // UNPACK V4-32 an A+D GIF packet (1 tag + nloop qwords) to VU1 qword 0.
    void appendPacketUnpack(Stream &s, std::mt19937_64 &rng)
    {
        const uint32_t nloop = 1u + static_cast<uint32_t>(rng() % 3u);
        s.cmd(0x01, 0u, 0x0101u); // STCYCL cl=1 wl=1
        s.cmd(0x6C, 1u + nloop, 0u);
        s.data64(gifTag(nloop, true, 0u, 1u));
        s.data64(0xEull); // REGS = A+D
        for (uint32_t i = 0; i < nloop; ++i)
        {
            s.data64(rng());
            s.data64(rng() % 0x60u); // register address
        }
    }

    void appendDirect(Stream &s, std::mt19937_64 &rng, bool image)
    {
        const uint32_t nloop = 1u + static_cast<uint32_t>(rng() % 2u);
        s.alignPayload(16u);
        s.cmd(0x50, 0u, 1u + nloop);
        s.data64(gifTag(nloop, true, image ? 2u : 0u, image ? 0u : 1u));
        s.data64(image ? 0ull : 0xEull);
        for (uint32_t i = 0; i < nloop; ++i)
        {
            s.data64(rng());
            s.data64(image ? rng() : rng() % 0x60u);
        }
    }

    Stream randomStream(std::mt19937_64 &rng, bool first)
    {
        Stream s;
        if (first || rng() % 8u == 0u)
            appendProgram(s);
        const uint32_t parts = 1u + static_cast<uint32_t>(rng() % 4u);
        for (uint32_t p = 0; p < parts; ++p)
        {
            switch (rng() % 6u)
            {
            case 0:
            case 1:
            case 2:
                appendPacketUnpack(s, rng);
                s.cmd(0x14, 0u, 0u); // MSCAL 0
                break;
            case 3:
                appendDirect(s, rng, rng() % 3u == 0u);
                break;
            case 4:
                s.cmd(0x06, 0u, (rng() & 1u) ? 0x8000u : 0u); // MSKPATH3
                break;
            default:
                s.cmd(0x11, 0u, 0u); // FLUSH
                break;
            }
        }
        while ((s.words.size() * 4u) % 16u != 0u)
            s.words.push_back(0u);
        return s;
    }

    std::vector<uint8_t> randomPath3(std::mt19937_64 &rng)
    {
        const bool image = rng() % 3u == 0u;
        const uint32_t nloop = 1u + static_cast<uint32_t>(rng() % 3u);
        std::vector<uint8_t> packet(16u * (1u + nloop));
        const uint64_t tag[2] = {gifTag(nloop, true, image ? 2u : 0u, image ? 0u : 1u), image ? 0ull : 0xEull};
        std::memcpy(packet.data(), tag, 16u);
        for (uint32_t i = 0; i < nloop; ++i)
        {
            const uint64_t q[2] = {rng(), image ? rng() : rng() % 0x60u};
            std::memcpy(packet.data() + 16u * (1u + i), q, 16u);
        }
        return packet;
    }

    // --- one run ------------------------------------------------------------
    struct Result
    {
        std::vector<std::string> log;
        std::vector<uint8_t> vu1Code;
        std::vector<uint8_t> vu1Data;
        VIFRegisters vif1{};
        bool path3Masked = false;
        bool verifyFailed = false;
        uint64_t verifiedPackets = 0u;
    };

    Result runSequence(uint64_t seed, Mode mode, bool jitter)
    {
        Result result;
        g_log = &result.log;

        auto memory = std::make_unique<PS2Memory>();
        memory->initialize();
        GifArbiter arbiter;
        GS gs;
        VU1Interpreter vu1{VU1Interpreter::Unit::VU1};
        vu1.reset();

        arbiter.setProcessPacketFn(
            [&](GifPathId path, const uint8_t *data, uint32_t size, bool continuation)
            {
                if (ps2x::Vu1Thread *thread = memory->vu1Thread())
                    thread->checkPacket(path, data, size, continuation);
                char line[64];
                std::snprintf(line, sizeof(line), "p%u c%d %016llx", static_cast<unsigned>(path), continuation ? 1 : 0,
                              static_cast<unsigned long long>(hashBytes(data, size)));
                g_log->emplace_back(line);
            });
        memory->setGifArbiter(&arbiter);
        memory->setVu1MscalCallback(
            [&](uint32_t startPC, uint32_t top, uint32_t itop)
            {
                vu1.execute(memory->getVU1Code(), PS2_VU1_CODE_SIZE, memory->getVU1Data(), PS2_VU1_DATA_SIZE, gs,
                            memory.get(), startPC, top, itop, 65536);
            });
        memory->setVu1MscntCallback(
            [&](uint32_t top, uint32_t itop)
            {
                vu1.resume(memory->getVU1Code(), PS2_VU1_CODE_SIZE, memory->getVU1Data(), PS2_VU1_DATA_SIZE, gs,
                           memory.get(), top, itop, 65536);
            });

        std::unique_ptr<ps2x::Vu1Thread> thread;
        if (mode != Mode::Serial)
        {
            thread = std::make_unique<ps2x::Vu1Thread>(*memory, vu1, gs, mode, [] { return 0u; });
            memory->setVu1Thread(thread.get());
        }

        std::mt19937_64 rng(seed);
        std::mt19937_64 timing(seed ^ 0x5DEECE66Dull ^ static_cast<uint64_t>(
                                                         std::chrono::steady_clock::now().time_since_epoch().count()));
        bool first = true;
        uint64_t vsyncTick = 0u;
        for (int event = 0; event < 400; ++event)
        {
            if (jitter && timing() % 4u == 0u)
            {
                const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(timing() % 60u);
                while (std::chrono::steady_clock::now() < until)
                {
                }
            }
            const uint32_t kind = static_cast<uint32_t>(rng() % 16u);
            if (kind < 7u)
            {
                const Stream s = randomStream(rng, first);
                first = false;
                std::memcpy(memory->getRDRAM() + kStreamAddr, s.words.data(), s.words.size() * 4u);
                memory->writeIORegister(kVif1Madr, kStreamAddr);
                memory->writeIORegister(kVif1Qwc, static_cast<uint32_t>(s.words.size() / 4u));
                memory->writeIORegister(kVif1Chcr, 0x101u);
                // The game may reuse the buffer at once: the kick must have
                // copied it.
                std::memset(memory->getRDRAM() + kStreamAddr, 0xCD, s.words.size() * 4u);
            }
            else if (kind < 11u)
            {
                const std::vector<uint8_t> packet = randomPath3(rng);
                memory->submitGifPacket(GifPathId::Path3, packet.data(), static_cast<uint32_t>(packet.size()),
                                        rng() % 2u == 0u);
            }
            else if (kind == 11u)
            {
                memory->write64(0x12000070u, rng()); // DISPFB1
            }
            else if (kind == 12u)
            {
                (void)memory->read32(rng() % 2u ? 0x10003C00u : 0x10003020u); // VIF1_STAT / GIF_STAT
            }
            else if (kind == 13u)
            {
                (void)memory->read32(0x1100C000u + static_cast<uint32_t>(rng() % 0x400u) * 4u);
            }
            else if (kind == 14u)
            {
                memory->write32(0x1100C000u + 0x100u * 16u + static_cast<uint32_t>(rng() % 64u) * 4u,
                                static_cast<uint32_t>(rng()));
            }
            else
            {
                // VBlank as the EE scheduler does it: wait for the previous
                // present, tick, then present (queued behind outstanding work
                // on the VU1 thread) with this VBlank's registers.
                if (thread)
                    thread->beforeVblank();
                ++vsyncTick;
                const auto regs = ps2x::Vu1Thread::captureVsyncRegs(memory->gs());
                if (thread)
                    thread->vblankPresent(vsyncTick, regs);
                else
                    Ps2GsHostHooks::hooks().vsyncRegs(vsyncTick, regs);
            }
        }
        memory->vu1Sync(ps2x::Vu1SyncReason::VBlank);
        if (thread)
        {
            result.verifyFailed = thread->verifyFailed();
            result.verifiedPackets = thread->verifiedPackets();
            memory->setVu1Thread(nullptr);
            thread.reset();
        }
        result.vu1Code.assign(memory->getVU1Code(), memory->getVU1Code() + PS2_VU1_CODE_SIZE);
        result.vu1Data.assign(memory->getVU1Data(), memory->getVU1Data() + PS2_VU1_DATA_SIZE);
        result.vif1 = memory->vif1_regs;
        result.path3Masked = memory->isPath3Masked();
        g_log = nullptr;
        return result;
    }

    int compare(const char *name, uint64_t seed, const Result &a, const Result &b)
    {
        int failures = 0;
        if (a.log != b.log)
        {
            size_t i = 0;
            while (i < a.log.size() && i < b.log.size() && a.log[i] == b.log[i])
                ++i;
            std::fprintf(stderr, "[vu1-thread-test] FAIL %s seed=%llu: GS stream differs at entry %zu (%zu vs %zu entries): %s vs %s\n",
                         name, static_cast<unsigned long long>(seed), i, a.log.size(), b.log.size(),
                         i < a.log.size() ? a.log[i].c_str() : "<end>", i < b.log.size() ? b.log[i].c_str() : "<end>");
            ++failures;
        }
        if (a.vu1Code != b.vu1Code || a.vu1Data != b.vu1Data)
        {
            std::fprintf(stderr, "[vu1-thread-test] FAIL %s seed=%llu: VU1 memory differs\n", name,
                         static_cast<unsigned long long>(seed));
            ++failures;
        }
        if (std::memcmp(&a.vif1, &b.vif1, sizeof(a.vif1)) != 0 || a.path3Masked != b.path3Masked)
        {
            std::fprintf(stderr, "[vu1-thread-test] FAIL %s seed=%llu: VIF1/PATH3 state differs\n", name,
                         static_cast<unsigned long long>(seed));
            ++failures;
        }
        return failures;
    }
} // namespace

void register_ps2_vu1_thread_tests()
{
    MiniTest::Case("PS2VU1Thread", [](TestCase &tc)
    {
        tc.Run("serial, threaded and verify modes give the same GS stream and VU1 state", [](TestCase &t)
        {
            const auto previousHooks = Ps2GsHostHooks::hooks();
            Ps2GsHostHooks::Hooks hooks;
            hooks.privRegWrite = [](uint32_t offset, uint64_t value)
            {
                if (g_log == nullptr)
                    return;
                char line[64];
                std::snprintf(line, sizeof(line), "r%04x=%016llx", offset, static_cast<unsigned long long>(value));
                g_log->emplace_back(line);
            };
            hooks.vsyncRegs = [](uint64_t tick, const Ps2GsHostHooks::Hooks::VsyncRegs &regs)
            {
                if (g_log == nullptr)
                    return;
                char line[96];
                std::snprintf(line, sizeof(line), "v%llu csr=%llx dispfb1=%016llx", static_cast<unsigned long long>(tick),
                              static_cast<unsigned long long>(regs.csr), static_cast<unsigned long long>(regs.dispfb1));
                g_log->emplace_back(line);
            };
            Ps2GsHostHooks::setHooks(std::move(hooks));

            int failures = 0;
            size_t entries = 0;
            uint64_t verified = 0;
            constexpr uint64_t kSeeds = 20u;
            for (uint64_t seed = 1; seed <= kSeeds; ++seed)
            {
                const Result serial = runSequence(seed, Mode::Serial, false);
                const Result threaded = runSequence(seed, Mode::Threaded, true);
                const Result verify = runSequence(seed, Mode::Verify, true);
                failures += compare("threaded", seed, serial, threaded);
                failures += compare("verify", seed, serial, verify);
                if (verify.verifyFailed)
                {
                    std::fprintf(stderr, "[vu1-thread-test] FAIL seed=%llu: verify mode reported a mismatch\n",
                                 static_cast<unsigned long long>(seed));
                    ++failures;
                }
                entries += serial.log.size();
                verified += verify.verifiedPackets;
            }
            std::printf("[vu1-thread-test] %llu seeds, %zu GS-stream entries, %llu packets verified in-process, %d failure(s)\n",
                        static_cast<unsigned long long>(kSeeds), entries, static_cast<unsigned long long>(verified), failures);
            Ps2GsHostHooks::setHooks(previousHooks);
            t.IsTrue(failures == 0, "VU1 thread must match the serial path");
            t.IsTrue(verified > 0u, "verify mode must compare packets");
        });
    });
}
