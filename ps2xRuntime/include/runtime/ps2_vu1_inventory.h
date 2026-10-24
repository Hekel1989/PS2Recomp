#ifndef PS2_VU1_INVENTORY_H
#define PS2_VU1_INVENTORY_H

#include <array>
#include <bitset>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// PS2X(vu1-inventory): PS2X_VU1_INVENTORY=<dir>
//
// Observes the VU1 microprograms the game actually runs.  The recorder is
// created lazily on the first call to instance(); when the environment
// variable is unset instance() returns nullptr and every hot-path call site
// is a single predictable null check.
class VU1InventoryRecorder
{
public:
    static VU1InventoryRecorder *instance();

    // One MSCAL/MSCALF/MSCNT (or VU1 resume) run.
    void beginRun(const uint8_t *code, uint32_t codeSize, uint64_t generation,
                  uint32_t startPc, uint64_t startCycle, uint64_t vsync);
    void endRun(uint32_t endPc, uint64_t endCycle, uint64_t instructions,
                uint64_t hostNs);

    // One executed instruction pair.  branchTaken is the state of the branch
    // pipeline after execLower(); it is only meaningful for branch lower ops.
    void onPair(uint32_t pc, uint32_t lower, uint32_t upper, bool iBit,
                bool eBit, bool dBit, bool tBit, bool mBit, bool branchTaken,
                uint32_t branchTarget);

    void onStall(uint64_t cycles);
    void onXgkick(uint32_t bytes);

    // One VIF1 MPG command.  presentBytes may be less than declaredBytes when
    // the command ran past the end of the DMA chunk.  image/imageSize is the
    // whole micro memory after the copy (nullptr when unavailable).
    void onMpg(uint32_t dest, const uint8_t *payload, uint32_t presentBytes,
               uint32_t declaredBytes, const uint8_t *image, uint32_t imageSize,
               uint64_t vsync, uint32_t srcAddr, uint32_t srcOff,
               uint32_t srcKind, bool complete);

    void flush();

private:
    VU1InventoryRecorder();
    ~VU1InventoryRecorder();

    struct RunStats
    {
        uint64_t calls = 0;
        uint64_t instructions = 0;
        uint64_t cycles = 0;
        uint64_t hostNs = 0;
        uint64_t firstVsync = 0;
        uint64_t lastVsync = 0;
    };

    struct FeatureCounts
    {
        uint64_t iBit = 0;
        uint64_t eBit = 0;
        uint64_t dBit = 0;
        uint64_t tBit = 0;
        uint64_t mBit = 0;
        uint64_t clip = 0;
        uint64_t xgkickBytes = 0;
        uint64_t stallEvents = 0;
        uint64_t stallCycles = 0;
        uint64_t branchTaken[12] = {};
        uint64_t branchNotTaken[12] = {};
    };

    struct ImageStats
    {
        uint32_t hash = 0;
        uint32_t size = 0;
        std::vector<uint8_t> bytes;
        std::array<uint64_t, 0x400> upperOps{};
        std::array<uint64_t, 0x400> lowerOps{};
        std::map<uint32_t, RunStats> runs;
        std::bitset<2048> executedPcBitmap;
        std::map<uint32_t, std::set<uint32_t>> jrTargets;
        FeatureCounts features;
    };

    struct MpgEvent
    {
        uint32_t dest = 0;
        uint32_t declaredBytes = 0;
        uint32_t presentBytes = 0;
        uint32_t payloadHash = 0;
        uint32_t imageHash = 0;
        uint64_t vsync = 0;
        uint32_t srcAddr = 0;
        uint32_t srcOff = 0;
        uint32_t srcKind = 0;
        bool complete = true;
    };

    void registerImage(const uint8_t *image, uint32_t imageSize);
    void dumpImage(const ImageStats &image);
    void maybeFlush();
    void writeJson();
    std::string outputPath(const char *name) const;

    std::string m_dir;
    bool m_active = false;
    bool m_flushing = false;
    uint64_t m_runsSinceFlush = 0;
    uint64_t m_lastFlushMs = 0;

    const uint8_t *m_cachedCode = nullptr;
    uint32_t m_cachedSize = 0;
    uint64_t m_cachedGeneration = ~0ull;
    uint32_t m_cachedHash = 0;

    ImageStats *m_current = nullptr;
    uint32_t m_currentStartPc = 0;
    uint64_t m_currentStartCycle = 0;
    uint64_t m_currentVsync = 0;

    uint64_t m_vsyncFirst = 0;
    uint64_t m_vsyncLast = 0;

    std::map<uint32_t, ImageStats> m_images;
    std::map<uint32_t, std::vector<uint8_t>> m_mpgPayloads;
    std::vector<MpgEvent> m_mpgEvents;
    std::set<uint32_t> m_dumpedImages;
    std::set<uint32_t> m_dumpedMpgs;
};

#endif
