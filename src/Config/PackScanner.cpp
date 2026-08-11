#include <filesystem>
#include "./Config.h"
#include "./PackScanner.h"
#include "../Runtime/OverlayRuntime.h"
#include "../Util/String.h"

namespace Config
{

    namespace
    {
        struct Candidate
        {
            RE::TESFormID baseFormID{ 0 };
            std::shared_ptr<const PreparedAssignment> assignment;
        };

        enum class PluginTier
        {
            kFull,
            kMedium,
            kSmall
        };

        std::optional<RE::TESFormID> EncodeRuntimeFormID(const std::uint32_t a_localFormID, const PluginTier a_tier, const std::uint32_t a_index)
        {
            switch (a_tier) {
            case PluginTier::kSmall:
                if (a_localFormID > 0xFFF || a_index > 0xFFF) {
                    return std::nullopt;
                }
                return 0xFE000000u | (a_index << 12) | a_localFormID;
            case PluginTier::kMedium:
                if (a_localFormID > 0xFFFF || a_index > 0xFF) {
                    return std::nullopt;
                }
                return 0xFD000000u | (a_index << 16) | a_localFormID;
            case PluginTier::kFull:
                if (a_localFormID > 0xFFFFFF || a_index > 0xFC) {
                    return std::nullopt;
                }
                return (a_index << 24) | a_localFormID;
            }
            return std::nullopt;
        }

        struct LoadedPlugin
        {
            PluginTier tier;
            std::uint32_t index;
        };

        std::optional<LoadedPlugin> FindLoadedPlugin(const RE::TESDataHandler* a_handler, const std::string_view a_pluginName)
        {
            const auto targetName = Util::FoldASCII(a_pluginName);
            const auto find = [&](const auto& a_files, PluginTier a_tier) -> std::optional<LoadedPlugin> {
                std::uint32_t tierIndex = 0;
                for (const auto* file : a_files) {
                    if (file && Util::FoldASCII(Util::SafeText(file->fileName)) == targetName) {
                        return LoadedPlugin{ .tier = a_tier, .index = a_tier == PluginTier::kFull ? file->compileIndex : tierIndex };
                    }
                    tierIndex++;
                }
                return std::nullopt;
            };
            if(auto plugin = find(a_handler->compiledFileCollection.files, PluginTier::kFull)) {
                return plugin;
            }
            if(auto plugin = find(a_handler->compiledFileCollection.mediumFiles, PluginTier::kMedium)) {
                return plugin;
            }

            return find(a_handler->compiledFileCollection.smallFiles, PluginTier::kSmall);
        }

        RE::TESNPC* ResolveTarget(const Target& a_target, const RE::TESDataHandler* a_handler, RE::TESRace* a_humanRace)
        {
            const auto plugin = FindLoadedPlugin(a_handler, a_target.plugin);
            if (!plugin) {
                REX::WARN("[PackScanner] assignment skipped: target {}:{} plugin not loaded", a_target.plugin, a_target.localFormID);
                return nullptr;
            }

            const auto runtimeFormID = EncodeRuntimeFormID(a_target.localFormID, plugin->tier, plugin->index);
            if (!runtimeFormID) {
                REX::WARN("[PackScanner] assignment skipped: target {}:{} plugin {} formID out of range for plugin tier", a_target.plugin, a_target.localFormID, a_target.plugin);
                return nullptr;
            }

            auto* npc = RE::TESForm::LookupByID<RE::TESNPC>(*runtimeFormID);
            if (!npc || !npc->IsUnique() || npc->GetRace() != a_humanRace) {
                REX::WARN("[PackScanner] assignment skipped: target {}:{} plugin {} formID {} did not resolve to TESNPC", a_target.plugin, a_target.localFormID, a_target.plugin, *runtimeFormID);
                return nullptr;
            }

            return npc;
        }

        bool CandidateLess(const Candidate& a_left, const Candidate& a_right)
        {
            const auto left = Util::FoldASCII(a_left.assignment->packID);
            const auto right = Util::FoldASCII(a_right.assignment->packID);
            if(right != left) {
                return left < right;
            }
            return a_left.assignment->packID < a_right.assignment->packID;
        }

        std::filesystem::path DefaultPacksDirectory()
        {
            return std::filesystem::path{ REX::FModule::GetCurrentModule().GetFileName() }.parent_path() / L"OSFIdentity" / L"Packs";
        }


        PreparedAssignmentMap RunScan(const std::filesystem::path& a_packsRoot)
        {
            const auto discovery = DiscoverPacks(a_packsRoot);

            for(const auto& issue : discovery.issues)
            {
                REX::WARN("[PackScanner] discovery issue: {}:{}: {} ({})", issue.path.string(), issue.offset, issue.code, issue.message);
            }

            std::vector<Candidate> candidates;

            const auto* handler = RE::TESDataHandler::GetSingleton();
            auto* humanRace = RE::TESForm::LookupByEditorID<RE::TESRace>(RE::BSFixedString{"HumanRace"});
            if(!handler || !humanRace) {
                REX::WARN("[PackScanner] assignment skipped: TESDataHandler or HumanRace not available");
                return {};
            }

            for (const auto& pack : discovery.packs) {
                for (const auto& assignment : pack.assignments) {
                    auto* npc = ResolveTarget(assignment.target, handler, humanRace);
                    if (!npc) {
                        REX::WARN("[PackScanner] assignment skipped: target {}:{} did not resolve to TESNPC", assignment.target.plugin, assignment.target.localFormID);
                        continue;
                    }

                    auto loaded = LoadCkPreset(assignment.presetPath);
                    if (!loaded.preset) {
                        for (const auto& issue : loaded.issues) {
                            REX::WARN("[PackScanner] preset issue: {}:{}: {} ({})", issue.path.string(), issue.offset, issue.code, issue.message);
                        }
                        continue;
                    }

                    auto resolvedDependencies = ResolveAppearanceDependencies(*loaded.preset, npc);
                    if (!resolvedDependencies.Complete()) {
                        for (const auto& issue : resolvedDependencies.issues) {
                            REX::WARN("[PackScanner] dependency issue: {}:{}: {} ({})", assignment.presetPath.string(), 0, issue.code, issue.message);
                        }
                        continue;
                    }

                    const auto baseFormID = npc->GetFormID();
                    std::shared_ptr<const PreparedAssignment> prepared = std::make_shared<const PreparedAssignment>(PreparedAssignment{
                        .target = assignment.target,
                        .baseFormID = baseFormID,
                        .packID = pack.id,
                        .presetPath = assignment.presetPath,
                        .preset = std::move(*loaded.preset),
                        .dependencies = std::move(resolvedDependencies)
                    });
                    candidates.push_back(Candidate{ baseFormID, prepared });
                }
            }

            std::ranges::sort(candidates, CandidateLess);

            PreparedAssignmentMap selected;
            for(auto& candidate : candidates)
            {
                const auto [winner, inserted] = selected.try_emplace(candidate.baseFormID, std::move(candidate.assignment));
                if (!inserted) {
                    REX::WARN("[PackScanner] assignment skipped: target {}:{} already has a higher-priority assignment from pack {}", winner->second->target.plugin, winner->second->target.localFormID, winner->second->packID);
                }
            }

            return selected;
        }
    }

    void ScanPacks()
    {
        const auto packsRoot = DefaultPacksDirectory();
        std::error_code ec;
        const bool packsPresent = std::filesystem::is_directory(packsRoot, ec) && !ec;
        if (!packsPresent) {
            REX::INFO("[PackScanner] startup disabled: packs directory is absent ({})", packsRoot.string());
            return;
        }

        auto resolvedAssignments = RunScan(packsRoot);
        if (resolvedAssignments.empty()) {
            REX::WARN("[PackScanner] startup found no valid assignments;");
            return;
        }

        Runtime::GetOverlayRuntime().Arm(std::move(resolvedAssignments));
    }
}
