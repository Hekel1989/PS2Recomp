#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    class IopMemory;

    enum class IopImageLoadError : uint8_t
    {
        None,
        InvalidElf,
        ArenaExhausted,
        MalformedImage,
    };

    struct IopImageLoadResult
    {
        IopImageLoadError error = IopImageLoadError::MalformedImage;
        uint32_t base = 0;
        uint32_t size = 0;
        uint32_t entry = 0;
        uint32_t gp = 0;
        bool relocationsComplete = true;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return error == IopImageLoadError::None;
        }
    };

    class IopModuleLoader
    {
    public:
        [[nodiscard]] static bool readWholeHostFile(IopHost &host, std::string_view guestPath, std::vector<uint8_t> &bytes);
        [[nodiscard]] static bool readElfFromGuest(IopHost &host, uint32_t guestAddress, std::vector<uint8_t> &bytes);
        // Places the image via the IopMemory allocator (fixed link address when
        // possible, otherwise the lowest free block) and registers it as an
        // allocation so the sysmem free list keeps off it.
        [[nodiscard]] static IopImageLoadResult load(std::span<const uint8_t> image, IopMemory &memory);
    };
}
