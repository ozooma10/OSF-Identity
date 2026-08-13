#pragma once

#include "Config/Preset.h"
#include "Config/Resolver.h"

namespace Runtime
{
    RE::TESNPC* PrepareRenderSource(RE::TESNPC* a_target, const Config::AppearancePreset& a_preset, const Config::ResolvedAppearanceDependencies& a_dependencies);
    bool RefreshAppearanceFromRenderSource(RE::TESNPC* a_target, RE::TESNPC* a_source, RE::Actor* a_actor, RE::TESFormID a_actorRefID);
}
