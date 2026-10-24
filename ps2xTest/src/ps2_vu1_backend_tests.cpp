// VU1 backend hook (runtime/ps2_vu1_backend.h): VU1 memory moves into the
// backend's buffers, kicks reach the backend with the VIF1 state, and its
// XGKICK packets go to the GIF as PATH1.

#include "MiniTest.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_backend.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    uint8_t g_micro[PS2_VU1_CODE_SIZE];
    uint8_t g_data[PS2_VU1_DATA_SIZE];

    ps2x::Vu1Backend::RunArgs g_lastArgs;
    uint32_t g_runs = 0;

    ps2x::Vu1Backend::RunResult fakeRun(const ps2x::Vu1Backend::RunArgs &args)
    {
        g_lastArgs = args;
        ++g_runs;
        // One A+D packet with a single register write, as an XGKICK would send.
        const uint64_t packet[4] = {0x1000000000008001ull, 0xEull, 0x1234ull, 0x0Full};
        args.sink(args.user, reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
        ps2x::Vu1Backend::RunResult result;
        result.finished = true;
        result.stoppedByT = (args.fbrst & 0x800u) != 0u;
        return result;
    }

    void fakeReset() {}
} // namespace

void register_ps2_vu1_backend_tests()
{
    MiniTest::Case("PS2VU1Backend", [](TestCase &tc)
    {
        tc.Run("useVu1Storage keeps VU1 memory and survives initialize", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "memory initializes");
            mem.getVU1Code()[0x10] = 0xAB;
            mem.getVU1Data()[0x20] = 0xCD;
            const uint64_t generation = mem.getVU1CodeGeneration();
            mem.useVu1Storage(g_micro, g_data);
            t.IsTrue(mem.getVU1Code() == g_micro && mem.getVU1Data() == g_data, "VU1 memory is the host's");
            t.Equals(static_cast<uint32_t>(g_micro[0x10]), 0xABu, "micro contents are moved");
            t.Equals(static_cast<uint32_t>(g_data[0x20]), 0xCDu, "data contents are moved");
            t.IsTrue(mem.getVU1CodeGeneration() != generation, "moving micro memory counts as a code change");
            t.IsTrue(mem.initialize(), "memory re-initializes");
            t.IsTrue(mem.getVU1Code() == g_micro && mem.getVU1Data() == g_data,
                     "initialize keeps using the host's buffers");
        });

        tc.Run("kicks reach the backend and its packets go to PATH1", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "memory initializes");
            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            std::vector<std::vector<uint8_t>> path1;
            GifArbiter arbiter([&](GifPathId path, const uint8_t *data, uint32_t size, bool)
                               {
                                   if (path == GifPathId::Path1)
                                       path1.emplace_back(data, data + size);
                               });
            mem.setGifArbiter(&arbiter);
            mem.useVu1Storage(g_micro, g_data);

            ps2x::Vu1Backend backend;
            backend.name = "test";
            backend.micro = g_micro;
            backend.data = g_data;
            backend.reset = fakeReset;
            backend.run = fakeRun;
            VU1Interpreter vu1;
            ps2x::Vu1BackendRunner runner(backend, mem, vu1, gs, ps2x::Vu1BackendRunner::Mode::On);

            g_runs = 0;
            const ps2x::Vu1BackendRunner::Stop stop = runner.run({false, 0x40u, 0x12u, 0x34u, 0x800u});
            t.Equals(g_runs, 1u, "one backend run per kick");
            t.Equals(g_lastArgs.startPc, 0x40u, "start pc is forwarded");
            t.IsTrue(!g_lastArgs.resume, "MSCAL is not a resume");
            t.Equals(g_lastArgs.top, 0x12u, "TOP is forwarded");
            t.Equals(g_lastArgs.itop, 0x34u, "ITOP is forwarded");
            t.Equals(g_lastArgs.fbrst, 0x800u, "FBRST is forwarded");
            t.IsTrue(g_lastArgs.microGeneration == mem.getVU1CodeGeneration(), "micro generation is forwarded");
            t.IsTrue(stop.byT && !stop.byD, "stop bits come back");
            t.Equals(path1.size(), static_cast<size_t>(1u), "the packet reaches the GIF as PATH1");

            (void)runner.run({true, 0u, 0u, 0u, 0u});
            t.IsTrue(g_lastArgs.resume, "MSCNT is a resume");
            mem.setGifArbiter(nullptr);
        });
    });
}
