#pragma once

#include "Probe/NpcAppearancePreset.h"

#include <cstddef>
#include <string>
#include <vector>

namespace RE
{
    class BGSHeadPart;
    class TESNPC;
    class TESRace;
}

namespace Probe::NpcAppearance
{
    struct DependencyIssue
    {
        std::string field;
        std::string value;
        std::string code;
        std::string message;
    };

    struct ResolvedAppearanceDependencies
    {
        RE::TESRace* race{ nullptr };
        std::vector<RE::BGSHeadPart*> miscHeadParts;
        std::vector<RE::BGSHeadPart*> uniqueHeadParts;
        std::size_t validatedBoneRegionGroups{ 0 };
        std::size_t resolvedBoneSliderIDs{ 0 };
        std::size_t resolvedBoneGroupNames{ 0 };
        std::size_t resolvedFacialShapeNames{ 0 };
        std::size_t resolvedAvmLayerNames{ 0 };
        std::size_t resolvedAvmValues{ 0 };
        std::size_t resolvedAvmModulations{ 0 };
        std::size_t resolvedColorAndTeethAtoms{ 0 };
        bool formReferencesComplete{ false };
        bool boneReferencesComplete{ false };
        bool shapeReferencesComplete{ false };
        bool avmReferencesComplete{ false };
        bool colorReferencesComplete{ false };
        bool stringCatalogsComplete{ false };
        std::vector<DependencyIssue> issues;

        [[nodiscard]] bool Complete() const noexcept
        {
            return formReferencesComplete && boneReferencesComplete && shapeReferencesComplete &&
                   stringCatalogsComplete && issues.empty();
        }
    };

    // Read-only, fail-closed resolution for decoded CK references. This does not
    // construct a donor, intern preset strings, or change either NPC.
    [[nodiscard]] ResolvedAppearanceDependencies ResolveAppearanceDependencies(
        const AppearancePreset& a_preset,
        RE::TESNPC* a_target);
}
