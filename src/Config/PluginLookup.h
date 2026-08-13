#pragma once

#include "RuntimeFormID.h"

#include <optional>
#include <string_view>

namespace Config
{
    struct LoadedPlugin
    {
        PluginTier tier;
        std::uint32_t index;
    };

    [[nodiscard]] std::optional<LoadedPlugin> FindLoadedPlugin(const RE::TESDataHandler* a_handler, std::string_view a_pluginName);
}
