// SUIKO(vu1-inventory): PS2X_VU1_INVENTORY=<dir>
//
// Captures the VU1 microprograms the game runs and aggregates, per 16 KiB
// micro-memory image and per start PC, call counts, executed pairs, VU1
// cycles, interpreter host time, dynamic opcode counts and feature usage.
// Everything is written under the requested directory; no code bytes are
// embedded in the JSON (images/payloads are separate .bin files).
#include "runtime/ps2_vu1_inventory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

namespace
{
    constexpr uint32_t kFnvOffset = 2166136261u;
    constexpr uint32_t kFnvPrime = 16777619u;

    uint32_t fnv1a32(const uint8_t *data, size_t size)
    {
        uint32_t hash = kFnvOffset;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= kFnvPrime;
        }
        return hash;
    }

    uint64_t nowMs()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    // Opcode counts are indexed by a small integer and only turned into
    // mnemonics when the JSON is written: onPair() runs hundreds of millions
    // of times, so it must not allocate or touch string maps.
    constexpr uint16_t kOpReserved = 0x3FFu;
    constexpr uint16_t kOpImm = 0x3FEu;
    constexpr uint16_t kOpNop = 0x3FDu;

    const char *const kUpperBase[0x30] = {
        "ADDx", "ADDy", "ADDz", "ADDw",
        "SUBx", "SUBy", "SUBz", "SUBw",
        "MADDx", "MADDy", "MADDz", "MADDw",
        "MSUBx", "MSUBy", "MSUBz", "MSUBw",
        "MAXx", "MAXy", "MAXz", "MAXw",
        "MINIx", "MINIy", "MINIz", "MINIw",
        "MULx", "MULy", "MULz", "MULw",
        "MULq", "MAXi", "MULi", "MINIi",
        "ADDq", "MADDq", "ADDi", "MADDi",
        "SUBq", "MSUBq", "SUBi", "MSUBi",
        "ADD", "MADD", "MUL", "MAX",
        "SUB", "MSUB", "OPMSUB", "MINI",
    };

    const char *const kLowerBase[0x30] = {
        "LQ", "SQ", "RESERVED", "RESERVED", "ILW", "ISW", "RESERVED", "RESERVED",
        "IADDIU", "ISUBIU", "RESERVED", "RESERVED", "RESERVED", "RESERVED", "RESERVED", "RESERVED",
        "FCEQ", "FCSET", "FCAND", "FCOR", "FSEQ", "FSSET", "FSAND", "FSOR",
        "FMEQ", "RESERVED", "FMAND", "FMOR", "FCGET", "RESERVED", "RESERVED", "RESERVED",
        "B", "BAL", "RESERVED", "RESERVED", "JR", "JALR", "RESERVED", "RESERVED",
        "IBEQ", "IBNE", "RESERVED", "RESERVED", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ",
    };

    const char *upperSpecialName(uint32_t special)
    {
        switch (special)
        {
        case 0x00: return "ADDAbcx";
        case 0x01: return "ADDAbcy";
        case 0x02: return "ADDAbcz";
        case 0x03: return "ADDAbcw";
        case 0x04: return "SUBAbcx";
        case 0x05: return "SUBAbcy";
        case 0x06: return "SUBAbcz";
        case 0x07: return "SUBAbcw";
        case 0x08: return "MADDAbcx";
        case 0x09: return "MADDAbcy";
        case 0x0A: return "MADDAbcz";
        case 0x0B: return "MADDAbcw";
        case 0x0C: return "MSUBAbcx";
        case 0x0D: return "MSUBAbcy";
        case 0x0E: return "MSUBAbcz";
        case 0x0F: return "MSUBAbcw";
        case 0x10: return "ITOF0";
        case 0x11: return "ITOF4";
        case 0x12: return "ITOF12";
        case 0x13: return "ITOF15";
        case 0x14: return "FTOI0";
        case 0x15: return "FTOI4";
        case 0x16: return "FTOI12";
        case 0x17: return "FTOI15";
        case 0x18: return "MULAbcx";
        case 0x19: return "MULAbcy";
        case 0x1A: return "MULAbcz";
        case 0x1B: return "MULAbcw";
        case 0x1C: return "MULAq";
        case 0x1D: return "ABS";
        case 0x1E: return "MULAi";
        case 0x1F: return "CLIP";
        case 0x20: return "ADDAq";
        case 0x21: return "MADDAq";
        case 0x22: return "ADDAi";
        case 0x23: return "MADDAi";
        case 0x24: return "SUBAq";
        case 0x25: return "MSUBAq";
        case 0x26: return "SUBAi";
        case 0x27: return "MSUBAi";
        case 0x28: return "ADDA";
        case 0x29: return "MADDA";
        case 0x2A: return "MULA";
        case 0x2C: return "SUBA";
        case 0x2D: return "MSUBA";
        case 0x2E: return "OPMULA";
        case 0x2F:
        case 0x30: return "NOP";
        default: return "RESERVED";
        }
    }

    const char *lowerDirectName(uint32_t funct)
    {
        switch (funct)
        {
        case 0x30: return "IADD";
        case 0x31: return "ISUB";
        case 0x32: return "IADDI";
        case 0x34: return "IAND";
        case 0x35: return "IOR";
        default: return "RESERVED";
        }
    }

    const char *lowerSpecialName(uint32_t special)
    {
        switch (special)
        {
        case 0x30: return "MOVE";
        case 0x31: return "MR32";
        case 0x34: return "LQI";
        case 0x35: return "SQI";
        case 0x36: return "LQD";
        case 0x37: return "SQD";
        case 0x38: return "DIV";
        case 0x39: return "SQRT";
        case 0x3A: return "RSQRT";
        case 0x3B: return "WAITQ";
        case 0x3C: return "MTIR";
        case 0x3D: return "MFIR";
        case 0x3E: return "ILWR";
        case 0x3F: return "ISWR";
        case 0x40: return "RNEXT";
        case 0x41: return "RGET";
        case 0x42: return "RINIT";
        case 0x43: return "RXOR";
        case 0x64: return "MFP";
        case 0x68: return "XTOP";
        case 0x69: return "XITOP";
        case 0x6C: return "XGKICK";
        case 0x70: return "ESADD";
        case 0x71: return "ERSADD";
        case 0x72: return "ELENG";
        case 0x73: return "ERLENG";
        case 0x74: return "EATANxy";
        case 0x75: return "EATANxz";
        case 0x76: return "ESUM";
        case 0x77: return "ERSQRT";
        case 0x78: return "ESQRT";
        case 0x79: return "ESIN";
        case 0x7A: return "ERCPR";
        case 0x7B: return "WAITP";
        case 0x7C: return "EATAN";
        case 0x7D: return "EEXP";
        default: return "RESERVED";
        }
    }

    uint16_t upperIndex(uint32_t instr)
    {
        const uint32_t op = instr & 0x3Fu;
        if (op <= 0x2Fu)
            return static_cast<uint16_t>(op);
        if (op >= 0x3Cu)
        {
            const uint32_t special = (instr & 3u) | ((instr >> 4) & 0x7Cu);
            if (special <= 0x30u)
                return static_cast<uint16_t>(0x100u + special);
        }
        return kOpReserved;
    }

    uint16_t lowerIndex(uint32_t instr)
    {
        if (instr == 0x00000000u || instr == 0x8000033Cu)
            return kOpNop;
        const uint32_t opHi = (instr >> 25) & 0x7Fu;
        if (opHi == 0x40u)
        {
            const uint32_t funct = instr & 0x3Fu;
            if (funct >= 0x3Cu && funct <= 0x3Fu)
            {
                const uint32_t special = (instr & 3u) | ((instr >> 4) & 0x7Cu);
                if (special < 0x7Eu)
                    return static_cast<uint16_t>(0x200u + special);
                return kOpReserved;
            }
            if (funct < 0x40u)
                return static_cast<uint16_t>(0x100u + funct);
            return kOpReserved;
        }
        if (opHi < 0x30u)
            return static_cast<uint16_t>(opHi);
        return kOpReserved;
    }

    const char *upperName(uint16_t index)
    {
        if (index < 0x30u)
            return kUpperBase[index];
        if (index >= 0x100u)
            return upperSpecialName(index - 0x100u);
        return "RESERVED";
    }

    const char *lowerName(uint16_t index)
    {
        if (index == kOpNop)
            return "NOP";
        if (index == kOpImm)
            return "(i-bit immediate)";
        if (index < 0x30u)
            return kLowerBase[index];
        if (index >= 0x100u && index < 0x140u)
            return lowerDirectName(index - 0x100u);
        if (index >= 0x200u && index < 0x300u)
            return lowerSpecialName(index - 0x200u);
        return "RESERVED";
    }

    int branchKind(uint32_t lower)
    {
        switch ((lower >> 25) & 0x7Fu)
        {
        case 0x20: return 0;
        case 0x21: return 1;
        case 0x24: return 2;
        case 0x25: return 3;
        case 0x28: return 4;
        case 0x29: return 5;
        case 0x2C: return 6;
        case 0x2D: return 7;
        case 0x2E: return 8;
        case 0x2F: return 9;
        default: return -1;
        }
    }

    const char *branchName(int kind)
    {
        static const char *const names[10] = {
            "B", "BAL", "JR", "JALR", "IBEQ", "IBNE", "IBLTZ", "IBGTZ",
            "IBLEZ", "IBGEZ"};
        return (kind >= 0 && kind < 10) ? names[kind] : "?";
    }

    void writePcBitmap(std::FILE *f, const std::bitset<2048> &bitmap)
    {
        std::fprintf(f, "[");
        bool first = true;
        for (uint32_t index = 0; index < 2048u; ++index)
        {
            if (!bitmap.test(index))
                continue;
            std::fprintf(f, "%s%u", first ? "" : ",", index * 8u);
            first = false;
        }
        std::fprintf(f, "]");
    }

    void writeOpArray(std::FILE *f, const std::array<uint64_t, 0x400> &values,
                      const char *(*nameFn)(uint16_t))
    {
        std::fprintf(f, "{");
        bool first = true;
        for (uint32_t index = 0; index < values.size(); ++index)
        {
            if (values[index] == 0u)
                continue;
            std::fprintf(f, "%s\"%s\":%llu", first ? "" : ",", nameFn(index),
                         static_cast<unsigned long long>(values[index]));
            first = false;
        }
        std::fprintf(f, "}");
    }
}

VU1InventoryRecorder::VU1InventoryRecorder()
{
    const char *dir = std::getenv("PS2X_VU1_INVENTORY");
    if (dir == nullptr || dir[0] == '\0')
        return;

    m_dir = dir;
    std::error_code ec;
    std::filesystem::create_directories(m_dir, ec);
    if (ec)
    {
        std::fprintf(stderr, "[vu1:inventory] cannot create %s: %s\n", m_dir.c_str(),
                     ec.message().c_str());
        return;
    }
    m_active = true;
    m_lastFlushMs = nowMs();
    std::fprintf(stderr, "[vu1:inventory] recording into %s\n", m_dir.c_str());
    writeJson();
}

VU1InventoryRecorder::~VU1InventoryRecorder()
{
    flush();
}

VU1InventoryRecorder *VU1InventoryRecorder::instance()
{
    static VU1InventoryRecorder *s_instance = []() -> VU1InventoryRecorder *
    {
        if (std::getenv("PS2X_VU1_INVENTORY") == nullptr)
            return nullptr;
        static VU1InventoryRecorder recorder;
        return recorder.m_active ? &recorder : nullptr;
    }();
    return s_instance;
}

std::string VU1InventoryRecorder::outputPath(const char *name) const
{
    return (std::filesystem::path(m_dir) / name).string();
}

void VU1InventoryRecorder::dumpImage(const ImageStats &image)
{
    if (m_dumpedImages.count(image.hash) != 0u || image.bytes.empty())
        return;
    m_dumpedImages.insert(image.hash);

    char name[64];
    std::snprintf(name, sizeof(name), "img_%08x.bin", image.hash);
    const std::string path = outputPath(name);
    const std::string tmp = path + ".tmp";
    if (std::FILE *f = std::fopen(tmp.c_str(), "wb"))
    {
        std::fwrite(image.bytes.data(), 1, image.bytes.size(), f);
        std::fclose(f);
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
    }
}

void VU1InventoryRecorder::registerImage(const uint8_t *image, uint32_t imageSize)
{
    if (image == nullptr || imageSize == 0u)
        return;
    const uint32_t hash = fnv1a32(image, imageSize);
    auto it = m_images.find(hash);
    if (it == m_images.end())
    {
        ImageStats stats;
        stats.hash = hash;
        stats.size = imageSize;
        stats.bytes.assign(image, image + imageSize);
        it = m_images.emplace(hash, std::move(stats)).first;
        dumpImage(it->second);
    }
    if (m_cachedCode == image && m_cachedSize == imageSize)
        m_cachedHash = hash;
}

void VU1InventoryRecorder::beginRun(const uint8_t *code, uint32_t codeSize,
                                    uint64_t generation, uint32_t startPc,
                                    uint64_t startCycle, uint64_t vsync)
{
    if (!m_active)
        return;

    if (code != m_cachedCode || codeSize != m_cachedSize ||
        generation != m_cachedGeneration)
    {
        m_cachedCode = code;
        m_cachedSize = codeSize;
        m_cachedGeneration = generation;
        m_cachedHash = code != nullptr ? fnv1a32(code, codeSize) : 0u;
        registerImage(code, codeSize);
    }

    auto it = m_images.find(m_cachedHash);
    if (it == m_images.end())
    {
        registerImage(code, codeSize);
        it = m_images.find(m_cachedHash);
    }
    m_current = it != m_images.end() ? &it->second : nullptr;
    m_currentStartPc = startPc;
    m_currentStartCycle = startCycle;
    m_currentVsync = vsync;
    if (m_vsyncFirst == 0u || vsync < m_vsyncFirst)
        m_vsyncFirst = vsync;
    if (vsync > m_vsyncLast)
        m_vsyncLast = vsync;
}

void VU1InventoryRecorder::endRun(uint32_t endPc, uint64_t endCycle,
                                  uint64_t instructions, uint64_t hostNs)
{
    (void)endPc;
    if (!m_active || m_current == nullptr)
        return;

    RunStats &run = m_current->runs[m_currentStartPc];
    ++run.calls;
    run.instructions += instructions;
    run.cycles += endCycle > m_currentStartCycle ? endCycle - m_currentStartCycle : 0u;
    run.hostNs += hostNs;
    if (run.firstVsync == 0u)
        run.firstVsync = m_currentVsync;
    run.lastVsync = m_currentVsync;

    m_current = nullptr;
    ++m_runsSinceFlush;
    maybeFlush();
}

void VU1InventoryRecorder::onPair(uint32_t pc, uint32_t lower, uint32_t upper,
                                  bool iBit, bool eBit, bool dBit, bool tBit,
                                  bool mBit, bool branchTaken,
                                  uint32_t branchTarget)
{
    if (!m_active || m_current == nullptr)
        return;

    ImageStats &image = *m_current;
    const uint32_t pcIndex = pc / 8u;
    if (pcIndex < 2048u)
        image.executedPcBitmap.set(pcIndex);
    if (iBit)
        ++image.features.iBit;
    if (eBit)
        ++image.features.eBit;
    if (dBit)
        ++image.features.dBit;
    if (tBit)
        ++image.features.tBit;
    if (mBit)
        ++image.features.mBit;

    const uint16_t upperOp = upperIndex(upper);
    ++image.upperOps[upperOp];
    if (upperOp == 0x11Fu) // CLIP
        ++image.features.clip;

    if (iBit)
    {
        ++image.lowerOps[kOpImm];
        return;
    }

    ++image.lowerOps[lowerIndex(lower)];

    const int kind = branchKind(lower);
    if (kind >= 0)
    {
        if (branchTaken)
            ++image.features.branchTaken[kind];
        else
            ++image.features.branchNotTaken[kind];
        const uint32_t opHi = (lower >> 25) & 0x7Fu;
        if ((opHi == 0x24u || opHi == 0x25u) && branchTaken)
            image.jrTargets[pc].insert(branchTarget);
    }
}

void VU1InventoryRecorder::onStall(uint64_t cycles)
{
    if (!m_active || m_current == nullptr)
        return;
    ++m_current->features.stallEvents;
    m_current->features.stallCycles += cycles;
}

void VU1InventoryRecorder::onXgkick(uint32_t bytes)
{
    if (!m_active || m_current == nullptr)
        return;
    m_current->features.xgkickBytes += bytes;
}

void VU1InventoryRecorder::onMpg(uint32_t dest, const uint8_t *payload,
                                 uint32_t presentBytes, uint32_t declaredBytes,
                                 const uint8_t *image, uint32_t imageSize,
                                 uint64_t vsync, uint32_t srcAddr,
                                 uint32_t srcOff, uint32_t srcKind, bool complete)
{
    if (!m_active)
        return;

    MpgEvent event;
    event.dest = dest;
    event.declaredBytes = declaredBytes;
    event.presentBytes = presentBytes;
    event.payloadHash = presentBytes != 0u ? fnv1a32(payload, presentBytes)
                                           : kFnvOffset;
    event.imageHash = (image != nullptr && imageSize != 0u) ? fnv1a32(image, imageSize) : 0u;
    event.vsync = vsync;
    event.srcAddr = srcAddr;
    event.srcOff = srcOff;
    event.srcKind = srcKind;
    event.complete = complete;
    m_mpgEvents.push_back(event);

    if (presentBytes != 0u && m_dumpedMpgs.count(event.payloadHash) == 0u)
    {
        m_dumpedMpgs.insert(event.payloadHash);
        char name[64];
        std::snprintf(name, sizeof(name), "mpg_%08x.bin", event.payloadHash);
        const std::string path = outputPath(name);
        const std::string tmp = path + ".tmp";
        if (std::FILE *f = std::fopen(tmp.c_str(), "wb"))
        {
            std::fwrite(payload, 1, presentBytes, f);
            std::fclose(f);
            std::error_code ec;
            std::filesystem::rename(tmp, path, ec);
        }
    }
    if (presentBytes != 0u)
        m_mpgPayloads[event.payloadHash] = std::vector<uint8_t>(payload, payload + presentBytes);

    if (image != nullptr && imageSize != 0u)
        registerImage(image, imageSize);
    maybeFlush();
}

void VU1InventoryRecorder::maybeFlush()
{
    if (!m_active || m_flushing)
        return;
    const uint64_t now = nowMs();
    if (m_runsSinceFlush < 256u && now - m_lastFlushMs < 2000u)
        return;
    writeJson();
}

void VU1InventoryRecorder::flush()
{
    if (!m_active)
        return;
    writeJson();
}

void VU1InventoryRecorder::writeJson()
{
    if (!m_active || m_flushing)
        return;
    m_flushing = true;
    m_runsSinceFlush = 0;
    m_lastFlushMs = nowMs();

    const std::string path = outputPath("inventory.json");
    const std::string tmp = path + ".tmp";
    std::FILE *f = std::fopen(tmp.c_str(), "w");
    if (f == nullptr)
    {
        m_flushing = false;
        return;
    }

    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"format\": \"vu1-inventory/1\",\n");
    std::fprintf(f, "  \"generated\": %llu,\n",
                 static_cast<unsigned long long>(std::time(nullptr)));
    std::fprintf(f, "  \"vsync_first\": %llu,\n",
                 static_cast<unsigned long long>(m_vsyncFirst));
    std::fprintf(f, "  \"vsync_last\": %llu,\n",
                 static_cast<unsigned long long>(m_vsyncLast));
    std::fprintf(f, "  \"images\": {\n");
    bool firstImage = true;
    for (const auto &entry : m_images)
    {
        const ImageStats &image = entry.second;
        std::fprintf(f, "%s    \"%08x\": {\n", firstImage ? "" : ",\n", image.hash);
        firstImage = false;
        std::fprintf(f, "      \"hash\": \"0x%08x\",\n", image.hash);
        std::fprintf(f, "      \"size\": %u,\n", image.size);
        std::fprintf(f, "      \"executed_pcs\": ");
        writePcBitmap(f, image.executedPcBitmap);
        std::fprintf(f, ",\n      \"upper_ops\": ");
        writeOpArray(f, image.upperOps, upperName);
        std::fprintf(f, ",\n      \"lower_ops\": ");
        writeOpArray(f, image.lowerOps, lowerName);
        std::fprintf(f, ",\n      \"features\": {\n");
        std::fprintf(f, "        \"i_bit\": %llu,\n",
                     static_cast<unsigned long long>(image.features.iBit));
        std::fprintf(f, "        \"e_bit\": %llu,\n",
                     static_cast<unsigned long long>(image.features.eBit));
        std::fprintf(f, "        \"d_bit\": %llu,\n",
                     static_cast<unsigned long long>(image.features.dBit));
        std::fprintf(f, "        \"t_bit\": %llu,\n",
                     static_cast<unsigned long long>(image.features.tBit));
        std::fprintf(f, "        \"m_bit\": %llu,\n",
                     static_cast<unsigned long long>(image.features.mBit));
        std::fprintf(f, "        \"clip\": %llu,\n",
                     static_cast<unsigned long long>(image.features.clip));
        std::fprintf(f, "        \"xgkick_bytes\": %llu,\n",
                     static_cast<unsigned long long>(image.features.xgkickBytes));
        std::fprintf(f, "        \"stall_events\": %llu,\n",
                     static_cast<unsigned long long>(image.features.stallEvents));
        std::fprintf(f, "        \"stall_cycles\": %llu,\n",
                     static_cast<unsigned long long>(image.features.stallCycles));
        std::fprintf(f, "        \"branches\": {");
        bool firstBranch = true;
        for (int kind = 0; kind < 10; ++kind)
        {
            if (image.features.branchTaken[kind] == 0u &&
                image.features.branchNotTaken[kind] == 0u)
                continue;
            std::fprintf(f, "%s\"%s\":{\"taken\":%llu,\"not_taken\":%llu}",
                         firstBranch ? "" : ",", branchName(kind),
                         static_cast<unsigned long long>(image.features.branchTaken[kind]),
                         static_cast<unsigned long long>(image.features.branchNotTaken[kind]));
            firstBranch = false;
        }
        std::fprintf(f, "}\n      },\n");
        std::fprintf(f, "      \"jr_targets\": {");
        bool firstJr = true;
        for (const auto &jr : image.jrTargets)
        {
            std::fprintf(f, "%s\"%u\":", firstJr ? "" : ",", jr.first);
            std::fprintf(f, "[");
            bool firstTarget = true;
            for (uint32_t target : jr.second)
            {
                std::fprintf(f, "%s%u", firstTarget ? "" : ",", target);
                firstTarget = false;
            }
            std::fprintf(f, "]");
            firstJr = false;
        }
        std::fprintf(f, "},\n      \"runs\": [");
        bool firstRun = true;
        for (const auto &runEntry : image.runs)
        {
            const RunStats &run = runEntry.second;
            std::fprintf(f,
                         "%s\n        {\"start_pc\":%u,\"calls\":%llu,"
                         "\"instructions\":%llu,\"cycles\":%llu,"
                         "\"host_ns\":%llu,\"first_vsync\":%llu,"
                         "\"last_vsync\":%llu}",
                         firstRun ? "" : ",", runEntry.first,
                         static_cast<unsigned long long>(run.calls),
                         static_cast<unsigned long long>(run.instructions),
                         static_cast<unsigned long long>(run.cycles),
                         static_cast<unsigned long long>(run.hostNs),
                         static_cast<unsigned long long>(run.firstVsync),
                         static_cast<unsigned long long>(run.lastVsync));
            firstRun = false;
        }
        std::fprintf(f, "%s]\n    }", firstRun ? "" : "\n      ");
    }
    std::fprintf(f, "\n  },\n");
    std::fprintf(f, "  \"mpg_events\": [");
    bool firstMpg = true;
    for (const MpgEvent &event : m_mpgEvents)
    {
        std::fprintf(f,
                     "%s\n    {\"dest\":%u,\"declared_bytes\":%u,"
                     "\"present_bytes\":%u,\"hash\":\"0x%08x\","
                     "\"image_hash\":\"0x%08x\","
                     "\"vsync\":%llu,\"src_addr\":%u,\"src_off\":%u,"
                     "\"src_kind\":%u,\"complete\":%s}",
                     firstMpg ? "" : ",", event.dest, event.declaredBytes,
                     event.presentBytes, event.payloadHash, event.imageHash,
                     static_cast<unsigned long long>(event.vsync),
                     event.srcAddr, event.srcOff, event.srcKind,
                     event.complete ? "true" : "false");
        firstMpg = false;
    }
    std::fprintf(f, "\n  ]\n}\n");
    std::fclose(f);

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    m_flushing = false;
}
