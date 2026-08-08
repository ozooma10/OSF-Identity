#pragma once

#include "NpcAppearance/Preset.h"

#include <cstddef>
#include <string>
#include <vector>

namespace RE
{
    class BGSHeadPart;
    class TESNPC;
    class TESRace;
}

namespace NpcAppearance
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
        bool formReferencesComplete{ false };
        bool boneReferencesComplete{ false };
        bool shapeReferencesComplete{ false };
        bool avmReferencesComplete{ false };
        bool colorReferencesComplete{ false };
        std::vector<DependencyIssue> issues;

        [[nodiscard]] bool Complete() const noexcept
        {
            return formReferencesComplete && boneReferencesComplete && shapeReferencesComplete &&
                   colorReferencesComplete && avmReferencesComplete && issues.empty();
        }
    };

    // Read-only, fail-closed resolution for decoded CK references. This does not
    // construct a donor, intern preset strings, or change either NPC.
    [[nodiscard]] ResolvedAppearanceDependencies ResolveAppearanceDependencies(
        const AppearancePreset& a_preset,
        RE::TESNPC* a_target);
}
