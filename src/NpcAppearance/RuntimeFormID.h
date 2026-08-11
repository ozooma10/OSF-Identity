#pragma once

#include <cstdint>
#include <optional>

namespace NpcAppearance
{
    enum class PluginTier
    {
        kFull,
        kMedium,
        kSmall
    };

    [[nodiscard]] std::optional<std::uint32_t> EncodeRuntimeFormID(
        std::uint32_t a_localFormID,
        PluginTier a_tier,
        std::uint32_t a_index) noexcept;
}
