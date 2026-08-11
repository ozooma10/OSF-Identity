#include "AssignmentSelection.h"

#include "Util/String.h"

#include <algorithm>
#include <map>

namespace Config
{
    namespace
    {
        [[nodiscard]] bool IdentityLess(const ResolvedAssignmentIdentity& a_left, const ResolvedAssignmentIdentity& a_right)
        {
            const auto leftPack = Util::FoldASCII(a_left.packID);
            const auto rightPack = Util::FoldASCII(a_right.packID);
            if (leftPack != rightPack) {
                return leftPack < rightPack;
            }
            if (a_left.packID != a_right.packID) {
                return a_left.packID < a_right.packID;
            }

            const auto leftPlugin = Util::FoldASCII(a_left.plugin);
            const auto rightPlugin = Util::FoldASCII(a_right.plugin);
            if (leftPlugin != rightPlugin) {
                return leftPlugin < rightPlugin;
            }
            if (a_left.plugin != a_right.plugin) {
                return a_left.plugin < a_right.plugin;
            }
            return a_left.localFormID < a_right.localFormID;
        }
    }

    AssignmentSelectionResult SelectAlphabeticalAssignments(const std::span<const ResolvedAssignmentIdentity> a_candidates)
    {
        std::map<std::uint32_t, std::vector<std::size_t>> groups;
        for (std::size_t i = 0; i < a_candidates.size(); ++i) {
            groups[a_candidates[i].baseFormID].push_back(i);
        }

        AssignmentSelectionResult result;
        result.winnerIndices.reserve(groups.size());
        result.winnerForCandidate.resize(a_candidates.size());

        for (auto& [baseFormID, indices] : groups) {
            static_cast<void>(baseFormID);
            std::ranges::sort(indices, [&](const auto a_left, const auto a_right) {
                const auto& left = a_candidates[a_left];
                const auto& right = a_candidates[a_right];
                if (IdentityLess(left, right)) {
                    return true;
                }
                if (IdentityLess(right, left)) {
                    return false;
                }
                return a_left < a_right;
            });

            const auto winner = indices.front();
            result.winnerIndices.push_back(winner);
            for (const auto index : indices) {
                result.winnerForCandidate[index] = winner;
            }
        }
        return result;
    }
}
