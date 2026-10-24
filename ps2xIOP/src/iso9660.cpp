#include "ps2x/iop/iso9660.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace ps2x::iop::iso9660
{
    namespace
    {
        // Upper bound on the sectors scanned inside one directory record area.
        constexpr uint64_t kMaxDirectorySectors = 8192u;

        uint32_t readLe32At(const uint8_t *source)
        {
            return static_cast<uint32_t>(source[0]) |
                   (static_cast<uint32_t>(source[1]) << 8u) |
                   (static_cast<uint32_t>(source[2]) << 16u) |
                   (static_cast<uint32_t>(source[3]) << 24u);
        }

        uint64_t alignSectors(uint64_t bytes)
        {
            return (bytes + kSectorSize - 1u) / kSectorSize;
        }

        bool readSector(const ImageReader &read, uint64_t lsn, std::array<uint8_t, kSectorSize> &sector)
        {
            return read(sector.data(), lsn * kSectorSize, kSectorSize);
        }

        bool findInDirectory(const ImageReader &read, uint64_t dirLsn, uint64_t dirBytes,
                             const std::string &wanted, Entry &found)
        {
            std::array<uint8_t, kSectorSize> sector;
            const uint64_t sectors = std::min<uint64_t>(alignSectors(dirBytes), kMaxDirectorySectors);
            for (uint64_t sectorIndex = 0u; sectorIndex < sectors; ++sectorIndex)
            {
                if (!readSector(read, dirLsn + sectorIndex, sector))
                    return false;
                size_t offset = 0u;
                while (offset + 34u <= kSectorSize)
                {
                    const uint8_t recordLength = sector[offset];
                    if (recordLength == 0u)
                        break;  // zero padding until the next sector
                    if (recordLength < 34u || offset + recordLength > kSectorSize)
                        break;
                    const uint8_t nameLength = sector[offset + 32u];
                    if (nameLength > 1u && offset + 33u + nameLength <= offset + recordLength)
                    {
                        const std::string_view raw(
                            reinterpret_cast<const char *>(sector.data() + offset + 33u), nameLength);
                        if (normalizeName(raw) == wanted)
                        {
                            found.name = normalizeName(raw);
                            found.byteOffset = static_cast<uint64_t>(readLe32At(sector.data() + offset + 2u)) * kSectorSize;
                            found.sizeBytes = readLe32At(sector.data() + offset + 10u);
                            found.isDirectory = (sector[offset + 25u] & 0x02u) != 0u;
                            return true;
                        }
                    }
                    offset += recordLength;
                }
            }
            return false;
        }
    }

    std::string normalizeName(std::string_view raw)
    {
        // ISO9660 names on PS2 discs look like "SD.BIN;1". Drop the version
        // suffix and any trailing dot, then upper-case for comparison.
        const size_t version = raw.find(';');
        std::string name(raw.substr(0, version));
        while (!name.empty() && name.back() == '.')
            name.pop_back();
        for (char &c : name)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return name;
    }

    std::vector<std::string> splitPath(std::string_view path)
    {
        // Drop the device prefix ("cdrom0:") if present.
        if (const size_t colon = path.find(':'); colon != std::string_view::npos)
            path.remove_prefix(colon + 1u);
        std::vector<std::string> components;
        size_t begin = 0u;
        while (begin < path.size())
        {
            const size_t end = path.find_first_of("\\/", begin);
            const size_t length = (end == std::string_view::npos) ? path.size() - begin : end - begin;
            const std::string_view component(path.data() + begin, length);
            begin = (end == std::string_view::npos) ? path.size() + 1u : end + 1u;
            if (!component.empty())
                components.push_back(normalizeName(component));
        }
        return components;
    }

    bool lookup(const ImageReader &read, std::string_view path, Entry &entry)
    {
        const std::vector<std::string> components = splitPath(path);
        if (components.empty())
            return false;

        std::array<uint8_t, kSectorSize> sector;
        if (!readSector(read, kPrimaryVolumeDescriptorLsn, sector) ||
            sector[0u] != 1u || std::memcmp(sector.data() + 1u, "CD001", 5u) != 0)
            return false;

        uint64_t dirLsn = readLe32At(sector.data() + 158u);  // root directory record
        uint64_t dirSize = readLe32At(sector.data() + 166u);
        for (size_t index = 0u; index < components.size(); ++index)
        {
            Entry found;
            if (!findInDirectory(read, dirLsn, dirSize, components[index], found))
                return false;
            if (index + 1u < components.size())
            {
                if (!found.isDirectory)
                    return false;
                dirLsn = found.byteOffset / kSectorSize;
                dirSize = found.sizeBytes;
                continue;
            }
            entry = std::move(found);
            return true;
        }
        return false;
    }
}
