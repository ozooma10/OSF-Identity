#pragma once

#include "Config.h"
#include "Preset.h"
#include "Resolver.h"

#include <memory>
#include <unordered_map>

namespace Config
{
    struct PreparedAssignment
    {
        Target target;
        RE::TESFormID baseFormID {0};
        std::string packID;
        std::filesystem::path presetPath;
        AppearancePreset preset;
        ResolvedAppearanceDependencies dependencies;
    };

    using PreparedAssignmentMap = std::unordered_map<RE::TESFormID, std::shared_ptr<const PreparedAssignment>>;
}
