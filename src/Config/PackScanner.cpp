#include <filesystem>
#include "./Config.h"
#include "./PackScanner.h"
#include "../Runtime/OverlayRuntime.h"

namespace Config
{

    namespace
    {
        struct Candidate
        {
            RE::TESFormID baseFormID{ 0 };
            std::shared_ptr<const PreparedAssignment> assignment;
        };

        RE::TESNPC* ResolveTarget(const Target& a_target)
        {
            return nullptr;
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
                    const auto* npc = ResolveTarget(assignment.target);
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
