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

        RE::TESNPC* ResolveTarget(const Target& a_target)
        {
            const auto* handler = RE::TESDataHandler::GetSingleton();
            if (!handler) {
                return nullptr;
            }

            const auto targetPlugin = Util::FoldASCII(a_target.plugin);
            const auto find = [&](const auto& a_files, const PluginTier a_tier) -> RE::TESNPC* {
                std::uint32_t tierIndex = 0;
                for (const auto* file : a_files) {
                    if (file && Util::FoldASCII(Util::SafeText(file->fileName)) == targetPlugin) {
                        const auto index = a_tier == PluginTier::kFull ? file->compileIndex : tierIndex;
                        const auto runtimeFormID = EncodeRuntimeFormID(a_target.localFormID, a_tier, index);
                        return runtimeFormID ? RE::TESForm::LookupByID<RE::TESNPC>(*runtimeFormID) : nullptr;
                    }
                    tierIndex++;
                }
                return nullptr;
            };

            if (auto* target = find(handler->compiledFileCollection.files, PluginTier::kFull)) {
                return target;
            }
            if (auto* target = find(handler->compiledFileCollection.mediumFiles, PluginTier::kMedium)) {
                return target;
            }
            return find(handler->compiledFileCollection.smallFiles, PluginTier::kSmall);
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

            for (const auto& pack : discovery.packs) {
                for (const auto& assignment : pack.assignments) {
                    auto* npc = ResolveTarget(assignment.target);
                    if (!npc) {
                        REX::WARN("[PackScanner] assignment skipped: target {}:{} did not resolve to TESNPC", assignment.target.plugin, assignment.target.localFormID);
                        continue;
                    }

                    const auto loaded = LoadCkPreset(assignment.presetPath);
                    if (!loaded.preset) {
                        for (const auto& issue : loaded.issues) {
                            REX::WARN("[PackScanner] preset issue: {}:{}: {} ({})", issue.path.string(), issue.offset, issue.code, issue.message);
                        }
                        continue;
                    }

                    const auto resolvedDependencies = ResolveAppearanceDependencies(*loaded.preset, npc);
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
