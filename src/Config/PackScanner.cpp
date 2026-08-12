#include <filesystem>
#include "./AssignmentSelection.h"
#include "./Config.h"
#include "./PackScanner.h"
#include "./RuntimeFormID.h"
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

    }

    std::filesystem::path DefaultPacksDirectory()
    {
        return std::filesystem::path{ REX::FModule::GetExecutingModule().GetFileName() }.parent_path() / L"Data" / L"SFSE" / L"Plugins" / L"OSFIdentity" / L"Packs";
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

        std::vector<ResolvedAssignmentIdentity> identities;
        identities.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            identities.push_back(ResolvedAssignmentIdentity{
                .baseFormID = candidate.baseFormID,
                .packID = candidate.assignment->packID,
                .plugin = candidate.assignment->target.plugin,
                .localFormID = candidate.assignment->target.localFormID
            });
        }

        const auto selection = SelectAlphabeticalAssignments(identities);

        PreparedAssignmentMap selected;
        for (const auto winnerIndex : selection.winnerIndices) {
            const auto& winner = candidates[winnerIndex];
            selected.emplace(winner.baseFormID, winner.assignment);
        }
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto winnerIndex = selection.winnerForCandidate[i];
            if (i == winnerIndex) {
                continue;
            }
            const auto& loser = *candidates[i].assignment;
            const auto& winner = *candidates[winnerIndex].assignment;
            REX::WARN("[PackScanner] assignment skipped: base=0x{:08X} pack '{}' target {}:{:08X} is shadowed by alphabetically earlier pack '{}'", candidates[i].baseFormID, loser.packID, loser.target.plugin, loser.target.localFormID, winner.packID);
        }

        return selected;
    }
}
