#pragma once

#include "Config/Preset.h"
#include "Config/Resolver.h"
#include "NPCSnapshot.h"

namespace Runtime
{
    struct PreparedAppearanceApplyResult
    {
        bool applied{ false };
        bool donorReleased{ true };
    };

    PreparedAppearanceApplyResult ApplyPreparedAppearance(RE::TESNPC* a_target, const Config::AppearancePreset& a_preset, const Config::ResolvedAppearanceDependencies& a_dependencies, const OriginalNPCState& a_original);
    bool NotifyAndRefreshAppearance(RE::TESNPC* a_target, RE::Actor* a_actor, RE::TESFormID a_actorRefID);
}
