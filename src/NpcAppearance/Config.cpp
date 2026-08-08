#include "NpcAppearance/Config.h"

#include "NpcAppearance/ConfigDetail.h"

#include <format>

namespace NpcAppearance
{
    std::string Target::CanonicalKey() const
    {
        return std::format("{}:{:08x}", Detail::FoldASCII(plugin), localFormID);
    }

    bool IsLocalFormIDValidForTier(
        const std::uint32_t a_localFormID,
        const PluginTier a_tier) noexcept
    {
        switch (a_tier) {
        case PluginTier::kSmall: return a_localFormID <= 0x00000FFF;
        case PluginTier::kMedium: return a_localFormID <= 0x0000FFFF;
        case PluginTier::kFull: return a_localFormID <= 0x00FFFFFF;
        }
        return false;
    }

    std::optional<std::uint32_t> EncodeRuntimeFormID(
        const std::uint32_t a_localFormID,
        const PluginTier a_tier,
        const std::uint32_t a_index) noexcept
    {
        if (!IsLocalFormIDValidForTier(a_localFormID, a_tier)) {
            return std::nullopt;
        }
        switch (a_tier) {
        case PluginTier::kSmall:
            if (a_index > 0xFFF) return std::nullopt;
            return 0xFE000000u | (a_index << 12) | a_localFormID;
        case PluginTier::kMedium:
            if (a_index > 0xFF) return std::nullopt;
            return 0xFD000000u | (a_index << 16) | a_localFormID;
        case PluginTier::kFull:
            if (a_index > 0xFC) return std::nullopt;
            return (a_index << 24) | a_localFormID;
        }
        return std::nullopt;
    }
}
