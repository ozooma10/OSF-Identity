#include "NpcAppearance/RuntimeDetail.h"

#include "NpcAppearance/Config.h"
#include "NpcAppearance/Preset.h"
#include "NpcAppearance/Resolver.h"
#include "pch.h"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NpcAppearance
{
    namespace
    {
        using namespace Detail;

        [[nodiscard]] std::filesystem::path DefaultDataDirectory()
        {
            std::wstring buffer(32768, L'\0');
            const auto length =
                ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0 || length >= buffer.size()) {
                return {};
            }
            buffer.resize(length);
            return std::filesystem::path{ buffer }.parent_path() / L"Data";
        }

        struct LoadedPlugin
        {
            RE::TESFile* file{ nullptr };
            PluginTier tier{ PluginTier::kFull };
            std::uint32_t index{ 0 };
        };

        [[nodiscard]] std::optional<LoadedPlugin> FindLoadedPlugin(
            const std::string_view a_name)
        {
            const auto* handler = RE::TESDataHandler::GetSingleton();
            if (!handler) {
                return std::nullopt;
            }
            const std::string needle{ a_name };
            const auto find = [&](const auto& a_files, const PluginTier a_tier)
                -> std::optional<LoadedPlugin> {
                std::uint32_t tierIndex = 0;
                for (auto* file : a_files) {
                    if (file && ::_stricmp(file->fileName, needle.c_str()) == 0) {
                        return LoadedPlugin{
                            file,
                            a_tier,
                            a_tier == PluginTier::kFull ? file->compileIndex : tierIndex
                        };
                    }
                    ++tierIndex;
                }
                return std::nullopt;
            };
            if (auto found = find(
                    handler->compiledFileCollection.files, PluginTier::kFull)) {
                return found;
            }
            if (auto found = find(
                    handler->compiledFileCollection.mediumFiles, PluginTier::kMedium)) {
                return found;
            }
            return find(handler->compiledFileCollection.smallFiles, PluginTier::kSmall);
        }

        [[nodiscard]] std::optional<RE::TESFormID> ResolveRuntimeFormID(
            const Target& a_target)
        {
            const auto plugin = FindLoadedPlugin(a_target.plugin);
            if (!plugin) {
                return std::nullopt;
            }
            return EncodeRuntimeFormID(
                a_target.localFormID, plugin->tier, plugin->index);
        }

        [[nodiscard]] RE::TESNPC* ResolveEligibleTarget(const LineSink& a_out, const Target& a_target)
        {
            const auto runtimeFormID = ResolveRuntimeFormID(a_target);
            if (!runtimeFormID) {
                a_out(std::format(
                    "resolve {}: target plugin absent, tier index invalid, or local FormID exceeds its tier",
                    a_target.CanonicalKey()));
                return nullptr;
            }
            auto* npc = RE::TESForm::LookupByID<RE::TESNPC>(*runtimeFormID);
            if (!npc) {
                a_out(std::format("resolve {} -> 0x{:08X}: not found or not TESNPC",
                                  a_target.CanonicalKey(), *runtimeFormID));
                return nullptr;
            }
            if (!npc->IsUnique()) {
                a_out(std::format("resolve {}: rejected (base 0x{:08X} is not unique)",
                                  a_target.CanonicalKey(), npc->GetFormID()));
                return nullptr;
            }
            auto* humanRace = RE::TESForm::LookupByEditorID<RE::TESRace>(RE::BSFixedString{ "HumanRace" });
            if (!humanRace || npc->GetRace() != humanRace) {
                a_out(std::format("resolve {}: rejected (race={} expected HumanRace={})",
                                  a_target.CanonicalKey(), static_cast<void*>(npc->GetRace()),
                                  static_cast<void*>(humanRace)));
                return nullptr;
            }
            a_out(std::format("resolve {}: base=0x{:08X} editorID='{}' ptr={} unique=1 race=HumanRace",
                              a_target.CanonicalKey(), npc->GetFormID(), SafeText(npc->GetFormEditorID()),
                              static_cast<void*>(npc)));
            return npc;
        }

        void ReportDependencyResolution(
            const LineSink& a_out,
            const ResolvedAppearanceDependencies& a_result)
        {
            a_out(std::format(
                "refs: forms={} bones={} shapes={} colors={} avm={} complete={} race={} uniqueSlots={} misc={} issues={}",
                a_result.formReferencesComplete, a_result.boneReferencesComplete,
                a_result.shapeReferencesComplete, a_result.colorReferencesComplete,
                a_result.avmReferencesComplete,
                a_result.Complete(),
                static_cast<void*>(a_result.race), a_result.uniqueHeadParts.size(),
                a_result.miscHeadParts.size(),
                a_result.issues.size()));
            for (const auto& issue : a_result.issues) {
                a_out(std::format("  dependency issue code={} field={} value='{}': {}",
                                  issue.code, issue.field, issue.value, issue.message));
            }
        }
    }

    namespace Detail
    {
        std::unordered_map<RE::TESFormID, SelectedAssignment> RunScan(
            const LineSink& a_out, const std::filesystem::path& a_packsRoot)
        {
            a_out(std::format("scan packsRoot={}", a_packsRoot.string()));
            auto discovery = DiscoverPackages(a_packsRoot, true);
            for (const auto& issue : discovery.issues) {
                a_out(std::format("pack issue code={} path={} @{}: {}",
                                  issue.code, issue.path.string(), issue.offset, issue.message));
            }

            std::size_t decodedPresets = 0;
            std::size_t implicitPacks = 0;
            std::size_t validPacks = 0;
            std::vector<ResolvedAssignment> validatedCandidates;
            for (const auto& package : discovery.packages) {
                if (package.implicitManifest) {
                    ++implicitPacks;
                }
                a_out(std::format("pack '{}' discovery={} priority={} root={} assignments={}",
                                  package.packageID,
                                  package.implicitManifest ? "implicit" : "manifest",
                                  package.priority, package.PackageRoot().string(),
                                  package.assignments.size()));
                bool packageRequirementsComplete = true;
                for (const auto& plugin : package.requirements.plugins) {
                    if (!FindLoadedPlugin(plugin)) {
                        a_out(std::format("pack '{}' rejected: required plugin '{}' is not loaded",
                                          package.packageID, plugin));
                        packageRequirementsComplete = false;
                    }
                }
                const auto packageAssets =
                    CheckRequiredAssets(package.requirements, DefaultDataDirectory());
                if (!packageAssets.Complete()) {
                    packageRequirementsComplete = false;
                    for (const auto& asset : packageAssets.missing) {
                        a_out(std::format("pack '{}' rejected: required Data asset '{}' is missing or is not a regular file",
                                          package.packageID, asset.generic_string()));
                    }
                }
                if (!packageRequirementsComplete) {
                    continue;
                }

                struct TargetResolution
                {
                    const Assignment* assignment{ nullptr };
                    RE::TESNPC* npc{ nullptr };
                };
                std::vector<TargetResolution> targetResolutions;
                targetResolutions.reserve(package.assignments.size());
                std::unordered_map<RE::TESFormID, std::string> resolvedTargets;
                bool duplicateResolvedTarget = false;
                for (const auto& assignment : package.assignments) {
                    auto* npc = ResolveEligibleTarget(a_out, assignment.target);
                    if (npc) {
                        const auto [found, inserted] = resolvedTargets.emplace(
                            npc->GetFormID(), assignment.target.CanonicalKey());
                        if (!inserted) {
                            duplicateResolvedTarget = true;
                            a_out(std::format(
                                "pack '{}' rejected: code=package_rejected_duplicate_resolved_target targets {} and {} both resolve to base 0x{:08X}",
                                package.packageID, found->second,
                                assignment.target.CanonicalKey(), npc->GetFormID()));
                        }
                    }
                    targetResolutions.push_back({ &assignment, npc });
                }
                if (duplicateResolvedTarget) {
                    continue;
                }

                bool packageHasValidCandidate = false;
                for (const auto& resolution : targetResolutions) {
                    const auto& assignment = *resolution.assignment;
                    bool assignmentRequirementsComplete = true;
                    for (const auto& plugin : assignment.requirements.plugins) {
                        if (!FindLoadedPlugin(plugin)) {
                            a_out(std::format("candidate pack='{}' target={} rejected: required plugin '{}' is not loaded",
                                              package.packageID,
                                              assignment.target.CanonicalKey(), plugin));
                            assignmentRequirementsComplete = false;
                        }
                    }
                    const auto assignmentAssets =
                        CheckRequiredAssets(assignment.requirements, DefaultDataDirectory());
                    if (!assignmentAssets.Complete()) {
                        assignmentRequirementsComplete = false;
                        for (const auto& asset : assignmentAssets.missing) {
                            a_out(std::format("candidate pack='{}' target={} rejected: required Data asset '{}' is missing or is not a regular file",
                                              package.packageID,
                                              assignment.target.CanonicalKey(),
                                              asset.generic_string()));
                        }
                    }
                    if (!assignmentRequirementsComplete) {
                        continue;
                    }

                    auto* npc = resolution.npc;
                    if (!npc) {
                        a_out(std::format("candidate pack='{}' target={} rejected: target is absent or ineligible",
                                          package.packageID,
                                          assignment.target.CanonicalKey()));
                        continue;
                    }

                    const auto decoded = LoadCkPreset(assignment.presetPath);
                    if (!decoded.preset) {
                        a_out(std::format("candidate pack='{}' target={} rejected: preset '{}' does not satisfy a supported 1.16.244 producer contract",
                                          package.packageID,
                                          assignment.target.CanonicalKey(),
                                          assignment.presetPath.string()));
                        for (const auto& issue : decoded.issues) {
                            a_out(std::format("  preset issue code={} path={} @0x{:X}: {}",
                                              issue.code, issue.path.string(), issue.offset,
                                              issue.message));
                        }
                        continue;
                    }
                    ++decodedPresets;

                    const auto resolved = ResolveAppearanceDependencies(*decoded.preset, npc);
                    ReportDependencyResolution(a_out, resolved);
                    if (!resolved.Complete()) {
                        a_out(std::format("candidate pack='{}' target={} rejected: preset appearance references are incomplete",
                                          package.packageID,
                                          assignment.target.CanonicalKey()));
                        continue;
                    }
                    validatedCandidates.push_back({
                        npc->GetFormID(),
                        SelectedAssignment{
                            assignment.target,
                            assignment.presetPath,
                            package.packageID,
                            package.priority
                        }
                    });
                    packageHasValidCandidate = true;
                }
                if (packageHasValidCandidate) {
                    ++validPacks;
                }
            }

            const auto selection = SelectResolvedAssignments(validatedCandidates);
            for (const auto& decision : selection.decisions) {
                a_out(std::format("conflict target={} pack='{}' priority={} result={} reason={}",
                                  decision.targetKey, decision.packageID, decision.priority,
                                  decision.winner ? "winner" : "loser", decision.reason));
            }

            std::unordered_map<RE::TESFormID, SelectedAssignment> resolvedAssignments;
            for (const auto& resolvedWinner : selection.winners) {
                const auto& assignment = resolvedWinner.assignment;
                auto* npc = ResolveEligibleTarget(a_out, assignment.target);
                if (npc && npc->GetFormID() == resolvedWinner.baseFormID) {
                    resolvedAssignments.emplace(resolvedWinner.baseFormID, assignment);
                } else if (npc) {
                    a_out(std::format(
                        "  winner pack='{}' target={} changed resolved base from 0x{:08X} to 0x{:08X}; rejected fail-closed",
                        assignment.packageID, assignment.target.CanonicalKey(),
                        resolvedWinner.baseFormID, npc->GetFormID()));
                }
                a_out(std::format("  winner pack='{}' priority={} target={} preset={} targetResolved={}",
                                  assignment.packageID, assignment.priority,
                                  assignment.target.CanonicalKey(), assignment.presetPath.string(),
                                  static_cast<void*>(npc)));
            }
            a_out(std::format("scan: discoveredPacks={} implicitPacks={} validPacks={} decodedPresets={} validCandidates={} winners={} resolvedTargets={}; validation only, owned population/application gate prevents mutation",
                              discovery.packages.size(), implicitPacks, validPacks,
                              decodedPresets, validatedCandidates.size(), selection.winners.size(),
                              resolvedAssignments.size()));
            return resolvedAssignments;
        }
    }
}
