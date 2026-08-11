#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Config
{
    struct ResolvedAssignmentIdentity
    {
        std::uint32_t baseFormID{ 0 };
        std::string packID;
        std::string plugin;
        std::uint32_t localFormID{ 0 };
    };

    struct AssignmentSelectionResult
    {
        std::vector<std::size_t> winnerIndices;
        std::vector<std::size_t> winnerForCandidate;
    };

    AssignmentSelectionResult SelectAlphabeticalAssignments(std::span<const ResolvedAssignmentIdentity> a_candidates);
}
