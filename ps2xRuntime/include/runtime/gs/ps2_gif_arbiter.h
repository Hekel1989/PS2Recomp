#ifndef PS2_GIF_ARBITER_H
#define PS2_GIF_ARBITER_H

#include <cstdint>
#include <functional>
#include <vector>

enum class GifPathId : uint8_t
{
    Path1 = 1,
    Path2 = 2,
    Path3 = 3,
};

struct GifArbiterPacket
{
    GifPathId pathId;
    bool path2DirectHl = false;
    bool path3Image = false;
    // SUIKO(gs-phase5b): the packet continues a VIF1 DIRECT payload that was
    // split at a DMA-chunk boundary; its first qword is a GIFtag the runtime
    // synthesized for its own frontend (see ps2_vif1_interpreter.cpp).
    bool directContinuation = false;
    std::vector<uint8_t> data;
};

class GifArbiter
{
public:
    // SUIKO(gs-phase5): the callback receives the packet's GifPathId so the
    // consumer can route it to the right GS path (see ps2_gs_host_hooks.h).
    // SUIKO(gs-phase5b): plus the packet's directContinuation flag.
    using ProcessPacketFn = std::function<void(GifPathId, const uint8_t *, uint32_t, bool)>;

    GifArbiter() = default;
    explicit GifArbiter(ProcessPacketFn processFn);

    void setProcessPacketFn(ProcessPacketFn fn) { m_processFn = std::move(fn); }

    void submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl = false,
                bool directContinuation = false);

    void drain();
    bool empty() const { return m_queue.empty(); }

private:
    ProcessPacketFn m_processFn;
    std::vector<GifArbiterPacket> m_queue;

    static bool isImagePacket(const uint8_t *data, uint32_t sizeBytes);
    static uint8_t pathPriority(GifPathId id);
};

#endif
