#include "RuntimeFormID.h"

namespace Config
{
    std::optional<std::uint32_t> EncodeRuntimeFormID(const std::uint32_t a_localFormID, const PluginTier a_tier, const std::uint32_t a_index) noexcept
    {
        switch (a_tier) {
        case PluginTier::kSmall:
            if (a_localFormID > 0x00000FFF || a_index > 0xFFF) { return std::nullopt; }
            return 0xFE000000u | (a_index << 12) | a_localFormID;
        case PluginTier::kMedium:
            if (a_localFormID > 0x0000FFFF || a_index > 0xFF) { return std::nullopt; }
            return 0xFD000000u | (a_index << 16) | a_localFormID;
        case PluginTier::kFull:
            if (a_localFormID > 0x00FFFFFF || a_index > 0xFC) { return std::nullopt; }
            return (a_index << 24) | a_localFormID;
        }
        return std::nullopt;
    }
}
