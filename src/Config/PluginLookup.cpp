#include "PluginLookup.h"

#include "Util/String.h"

namespace Config
{
    std::optional<LoadedPlugin> FindLoadedPlugin(const RE::TESDataHandler* a_handler, const std::string_view a_pluginName)
    {
        if (!a_handler) {
            return std::nullopt;
        }

        const auto targetName = Util::FoldASCII(a_pluginName);
        const auto find = [&](const auto& a_files, const PluginTier a_tier) -> std::optional<LoadedPlugin> {
            std::uint32_t tierIndex = 0;
            for (const auto* file : a_files) {
                if (file && Util::FoldASCII(Util::SafeText(file->fileName)) == targetName) {
                    return LoadedPlugin{
                        .tier = a_tier,
                        .index = a_tier == PluginTier::kFull ? file->compileIndex : tierIndex };
                }
                ++tierIndex;
            }
            return std::nullopt;
        };

        if (auto plugin = find(a_handler->compiledFileCollection.files, PluginTier::kFull)) {
            return plugin;
        }
        if (auto plugin = find(a_handler->compiledFileCollection.mediumFiles, PluginTier::kMedium)) {
            return plugin;
        }
        return find(a_handler->compiledFileCollection.smallFiles, PluginTier::kSmall);
    }
}
