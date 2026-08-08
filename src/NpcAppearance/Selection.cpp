#include "NpcAppearance/Config.h"

#include "NpcAppearance/ConfigDetail.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace NpcAppearance
{
    using Detail::FoldASCII;

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
