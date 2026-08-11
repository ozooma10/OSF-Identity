#pragma once

#include "Config/Preset.h"
#include "Config/Resolver.h"

namespace Runtime
{
    struct PreparedAppearanceApplyResult
    {
        bool applied{ false };
        bool donorReleased{ true };
    };

    PreparedAppearanceApplyResult ApplyPreparedAppearance(RE::TESNPC* a_target, const Config::AppearancePreset& a_preset, const Config::ResolvedAppearanceDependencies& a_dependencies);

    [[nodiscard]] bool NotifyAndRefreshAppearance(RE::TESNPC* a_target, RE::Actor* a_actor, RE::TESFormID a_actorRefID);
}
