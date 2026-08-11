#pragma once

#include "./Preset.h"

namespace Config
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

        bool Complete() const noexcept
        {
            return formReferencesComplete && boneReferencesComplete && shapeReferencesComplete && colorReferencesComplete && avmReferencesComplete && issues.empty();
        }
    };

    ResolvedAppearanceDependencies ResolveAppearanceDependencies(const AppearancePreset& a_preset, RE::TESNPC* a_target);
}
