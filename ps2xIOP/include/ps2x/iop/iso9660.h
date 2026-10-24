#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::iso9660
{
    inline constexpr uint64_t kSectorSize = 2048u;
    inline constexpr uint64_t kPrimaryVolumeDescriptorLsn = 16u;

    // Reads byteCount bytes at byteOffset inside a raw disc image.
    // Returns false when the range could not be read in full.
    using ImageReader = std::function<bool(void *destination, uint64_t byteOffset, size_t byteCount)>;

    struct Entry
    {
        std::string name;        // Normalized leaf name, e.g. "SD.BIN"
        uint64_t byteOffset = 0; // Data start inside the image (LSN * kSectorSize)
        uint64_t sizeBytes = 0;
        bool isDirectory = false;
    };

    // Normalizes one ISO9660 name or path component: drops the ";version" suffix
    // and any trailing dot, then upper-cases for comparison.
    [[nodiscard]] std::string normalizeName(std::string_view raw);

    // Splits a PS2-style path ("cdrom0:\DIR\FILE.EXT;1") into normalized
    // components, dropping the device prefix and empty components.
    [[nodiscard]] std::vector<std::string> splitPath(std::string_view path);

    // Looks up a path in the ISO 9660 tree of a raw disc image (PVD at LSN 16,
    // directory records walked component by component, case-insensitive).
    bool lookup(const ImageReader &read, std::string_view path, Entry &entry);
}
