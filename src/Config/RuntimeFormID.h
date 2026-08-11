#pragma once

#include <cstdint>
#include <optional>

namespace Config
{
    enum class PluginTier
    {
        kFull,
        kMedium,
        kSmall
    };

    std::optional<std::uint32_t> EncodeRuntimeFormID(std::uint32_t a_localFormID, PluginTier a_tier, std::uint32_t a_index) noexcept;
}
