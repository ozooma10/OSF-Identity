#include "NpcAppearance/Config.h"

#include "NpcAppearance/ConfigDetail.h"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace NpcAppearance
{
    using Detail::FoldASCII;

    SelectionResult SelectAssignments(const std::vector<PackageManifest>& a_packages)
    {
        struct Candidate
        {
            const PackageManifest* package;
            const Assignment* assignment;
        };
        std::map<std::string, std::vector<Candidate>> groups;
        for (const auto& package : a_packages) {
            for (const auto& assignment : package.assignments) {
                groups[assignment.target.CanonicalKey()].push_back({ &package, &assignment });
            }
        }

        SelectionResult result;
        for (auto& [targetKey, candidates] : groups) {
            std::ranges::sort(candidates, [](const Candidate& a_left, const Candidate& a_right) {
                if (a_left.package->priority != a_right.package->priority) {
                    return a_left.package->priority > a_right.package->priority;
                }
                const auto leftID = FoldASCII(a_left.package->packageID);
                const auto rightID = FoldASCII(a_right.package->packageID);
                return leftID != rightID ? leftID < rightID :
                    a_left.package->packageID < a_right.package->packageID;
            });
            const auto& winner = candidates.front();
            result.winners.push_back({ winner.assignment->target, winner.assignment->presetPath,
                                       winner.assignment->requirements,
                                       winner.package->packageID, winner.package->priority });
            for (const auto& candidate : candidates) {
                const bool won = std::addressof(candidate) == std::addressof(candidates.front());
                result.decisions.push_back({ targetKey, candidate.package->packageID,
                                             candidate.package->priority, won,
                                             won ? "winner" : "shadowed_by_" + winner.package->packageID });
            }
        }
        return result;
    }

    ResolvedSelectionResult SelectResolvedAssignments(
        const std::vector<ResolvedAssignment>& a_candidates)
    {
        using PackageBaseKey = std::pair<std::string, std::uint32_t>;
        std::map<PackageBaseKey, std::size_t> packageBaseCounts;
        std::map<std::string, std::string> packageNames;
        for (const auto& candidate : a_candidates) {
            const auto foldedPackage = FoldASCII(candidate.assignment.packageID);
            packageNames.try_emplace(foldedPackage, candidate.assignment.packageID);
            ++packageBaseCounts[{ foldedPackage, candidate.baseFormID }];
        }

        std::set<std::string> rejectedFoldedPackages;
        for (const auto& [key, count] : packageBaseCounts) {
            if (count > 1) {
                rejectedFoldedPackages.insert(key.first);
            }
        }

        std::map<std::uint32_t, std::vector<const ResolvedAssignment*>> groups;
        ResolvedSelectionResult result;
        for (const auto& rejected : rejectedFoldedPackages) {
            result.rejectedPackages.push_back(packageNames.at(rejected));
        }
        for (const auto& candidate : a_candidates) {
            const auto foldedPackage = FoldASCII(candidate.assignment.packageID);
            if (rejectedFoldedPackages.contains(foldedPackage)) {
                result.decisions.push_back({
                    std::format("base:{:08x}", candidate.baseFormID),
                    candidate.assignment.packageID,
                    candidate.assignment.priority,
                    false,
                    "package_rejected_duplicate_resolved_target"
                });
                continue;
            }
            groups[candidate.baseFormID].push_back(&candidate);
        }

        for (auto& [baseFormID, candidates] : groups) {
            std::ranges::sort(candidates, [](const auto* a_left, const auto* a_right) {
                if (a_left->assignment.priority != a_right->assignment.priority) {
                    return a_left->assignment.priority > a_right->assignment.priority;
                }
                const auto leftID = FoldASCII(a_left->assignment.packageID);
                const auto rightID = FoldASCII(a_right->assignment.packageID);
                return leftID != rightID ? leftID < rightID :
                    a_left->assignment.packageID < a_right->assignment.packageID;
            });
            const auto* winner = candidates.front();
            result.winners.push_back(*winner);
            for (const auto* candidate : candidates) {
                const bool won = candidate == winner;
                result.decisions.push_back({
                    std::format("base:{:08x}", baseFormID),
                    candidate->assignment.packageID,
                    candidate->assignment.priority,
                    won,
                    won ? "winner" :
                          "shadowed_by_" + winner->assignment.packageID
                });
            }
        }
        return result;
    }
}
