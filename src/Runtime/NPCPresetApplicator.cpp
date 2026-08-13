#include "NPCPresetApplicator.h"

#include "NPCSnapshot.h"
#include "RenderSourceNPC.h"
#include "RenderSourceRegistry.h"
#include "Util/String.h"

#include <vector>

namespace Runtime
{
    namespace
    {
        // TESNPC::AddChange(0x800) sets this actor-base bit before it enters
        // the registered-form change manager. FaceDB tests the bit to choose
        // runtime face generation over the baked-face path. Detached FormID-0
        // sources must reproduce only that object-local preparation step.
        constexpr std::uint32_t kRuntimeGeneratedFaceFlag = 0x8000U;

        bool SameText(const char *a_left, const std::string &a_right)
        {
            return ::_stricmp(Util::SafeText(a_left), a_right.c_str()) == 0;
        }

        bool SameText(const char *a_left, const char *a_right)
        {
            return ::_stricmp(Util::SafeText(a_left), Util::SafeText(a_right)) == 0;
        }

        void ApplyMorphs(RE::TESNPC *a_target, const Config::AppearancePreset &a_preset)
        {
            // Both supported producer contracts describe a complete appearance.
            // The detached source begins as an exact copy of the target only to obtain engine-owned storage; inherited target morph entries must not remain and blend with the preset.
            if (a_target->shapeBlendData)
            {
                a_target->shapeBlendData->clear();
            }
            if (a_target->facialBoneValues)
            {
                a_target->facialBoneValues->clear();
            }
            if (a_target->facialBoneGroupValues)
            {
                for (auto &region : *a_target->facialBoneGroupValues)
                {
                    if (region.value)
                    {
                        region.value->clear();
                    }
                }
            }

            a_target->morphWeight.thin = static_cast<float>(a_preset.morphWeights.x);
            a_target->morphWeight.muscular = static_cast<float>(a_preset.morphWeights.y);
            a_target->morphWeight.fat = static_cast<float>(a_preset.morphWeights.z);
            for (std::size_t i = 0; i < a_preset.bodyMorphRegionValues.size(); ++i)
            {
                a_target->SetBodyMorph(static_cast<std::uint32_t>(i), static_cast<float>(a_preset.bodyMorphRegionValues[i]));
            }
            for (const auto &morph : a_preset.facialMorphSliders)
            {
                const RE::BSFixedStringCS key{morph.name.c_str()};
                a_target->SetShapeBlend(key, static_cast<float>(morph.value));
            }
            for (const auto &region : a_preset.facialBoneRegions)
            {
                for (const auto &slider : region.sliders)
                {
                    if (slider.id != 0)
                    {
                        a_target->SetFacialBone(slider.id, static_cast<float>(slider.value));
                        continue;
                    }
                    const RE::BSFixedStringCS key{slider.groupName.c_str()};
                    a_target->EnsureFacialBoneGroup(region.regionID, key);
                    if (!a_target->facialBoneGroupValues)
                    {
                        continue;
                    }
                    const auto outer = a_target->facialBoneGroupValues->find(region.regionID);
                    if (outer == a_target->facialBoneGroupValues->end() || !outer->value)
                    {
                        continue;
                    }
                    for (auto &entry : *outer->value)
                    {
                        if (::_stricmp(Util::SafeText(entry.key.c_str()), slider.groupName.c_str()) == 0)
                        {
                            entry.value = static_cast<float>(slider.value);
                            break;
                        }
                    }
                }
            }
        }

        void ApplyVisuals(RE::TESNPC *a_target, const Config::AppearancePreset &a_preset, const Config::ResolvedAppearanceDependencies &a_dependencies)
        {
            std::vector<RE::BGSHeadPart *> originalHeadParts;
            {
                auto headParts = a_target->headParts.Lock();
                originalHeadParts.assign((*headParts).begin(), (*headParts).end());
            }
            for (std::size_t i = 1; i < a_dependencies.uniqueHeadParts.size(); ++i)
            {
                auto *expected = a_dependencies.uniqueHeadParts[i];
                for (auto *part : originalHeadParts)
                {
                    if (part && part != expected && static_cast<std::size_t>(part->type.get()) == i)
                    {
                        a_target->RemoveHeadPart(part, false);
                    }
                }
            }
            for (auto *part : a_dependencies.uniqueHeadParts)
            {
                if (!part)
                {
                    continue;
                }
                bool present = false;
                {
                    auto headParts = a_target->headParts.Lock();
                    present = std::ranges::find(*headParts, part) != (*headParts).end();
                }
                if (!present)
                {
                    a_target->ChangeHeadPart(part);
                }
            }
            for (auto *part : a_dependencies.miscHeadParts)
            {
                bool present = false;
                {
                    auto headParts = a_target->headParts.Lock();
                    present = std::ranges::find(*headParts, part) != (*headParts).end();
                }
                if (!present)
                {
                    a_target->ChangeHeadPart(part);
                }
            }

            a_target->skinToneIndex = a_preset.skinTone;
            a_target->teeth = a_preset.teethCustomization;
            a_target->jewelryColor = a_preset.jewelryColor;
            a_target->eyeColor = a_preset.eyeColor;
            a_target->hairColor = a_preset.hairColor;
            a_target->facialColor = a_preset.facialHairColor;
            a_target->eyebrowColor = a_preset.browHairColor;

            std::vector<RE::BSFixedString> existingCategories;
            existingCategories.reserve(a_target->tintAVMData.size());
            for (const auto &avm : a_target->tintAVMData)
            {
                existingCategories.push_back(avm.category);
            }
            for (const auto &category : existingCategories)
            {
                const bool desired = std::ranges::any_of(a_dependencies.avmLayers, [&](const RE::AVMData &a_expected)
                                                         { return ::_stricmp(Util::SafeText(category.c_str()), Util::SafeText(a_expected.category.c_str())) == 0; });
                if (!desired)
                {
                    a_target->RemoveAVMData(category);
                }
            }
            for (const auto &avm : a_dependencies.avmLayers)
            {
                a_target->SetAVMData(avm);
            }
            a_target->faceNPC = nullptr;
        }

        bool ValidateMorphs(RE::TESNPC *a_target, const Config::AppearancePreset &a_preset)
        {
            if (a_target->morphWeight.thin != static_cast<float>(a_preset.morphWeights.x) || a_target->morphWeight.muscular != static_cast<float>(a_preset.morphWeights.y) ||
                a_target->morphWeight.fat != static_cast<float>(a_preset.morphWeights.z))
            {
                REX::WARN("[NPCPresetApplicator] detached preset morph validation failed field='MorphWeight' actual=({},{},{}) expected=({},{},{})",
                          a_target->morphWeight.thin, a_target->morphWeight.muscular, a_target->morphWeight.fat,
                          a_preset.morphWeights.x, a_preset.morphWeights.y, a_preset.morphWeights.z);
                return false;
            }

            if (!a_preset.bodyMorphRegionValues.empty())
            {
                if (!a_target->bodyMorphValues || a_target->bodyMorphValues->size() != a_preset.bodyMorphRegionValues.size())
                {
                    REX::WARN("[NPCPresetApplicator] detached preset morph validation failed field='BodyMorphRegionValuesA' actualCount={} expectedCount={}",
                              a_target->bodyMorphValues ? a_target->bodyMorphValues->size() : 0, a_preset.bodyMorphRegionValues.size());
                    return false;
                }
                for (std::size_t i = 0; i < a_preset.bodyMorphRegionValues.size(); ++i)
                {
                    if ((*a_target->bodyMorphValues)[static_cast<std::uint32_t>(i)] != static_cast<float>(a_preset.bodyMorphRegionValues[i]))
                    {
                        REX::WARN("[NPCPresetApplicator] detached preset morph validation failed field='BodyMorphRegionValuesA[{}]' actual={} expected={}",
                                  i, (*a_target->bodyMorphValues)[static_cast<std::uint32_t>(i)], a_preset.bodyMorphRegionValues[i]);
                        return false;
                    }
                }
            }
            for (const auto &morph : a_preset.facialMorphSliders)
            {
                float actual = 0.0F;
                bool found = false;
                if (a_target->shapeBlendData)
                {
                    for (const auto &entry : *a_target->shapeBlendData)
                    {
                        if (::_stricmp(Util::SafeText(entry.key.c_str()), morph.name.c_str()) == 0)
                        {
                            actual = entry.value;
                            found = true;
                            break;
                        }
                    }
                }
                const auto expected = static_cast<float>(morph.value);
                if (!((expected == 0.0F && (!found || actual == 0.0F)) || (found && actual == expected)))
                {
                    REX::WARN("[NPCPresetApplicator] detached preset morph validation failed field='FacialMorphSliderDataA' name='{}' found={} actual={} expected={}",
                              morph.name, found, actual, expected);
                    return false;
                }
            }
            for (const auto &region : a_preset.facialBoneRegions)
            {
                for (const auto &slider : region.sliders)
                {
                    bool found = false;
                    float value = 0.0F;
                    if (slider.id != 0 && a_target->facialBoneValues)
                    {
                        const auto entry = a_target->facialBoneValues->find(slider.id);
                        if (entry != a_target->facialBoneValues->end())
                        {
                            found = true;
                            value = entry->value;
                        }
                    }
                    else if (slider.id == 0 && a_target->facialBoneGroupValues)
                    {
                        const auto outer = a_target->facialBoneGroupValues->find(region.regionID);
                        if (outer != a_target->facialBoneGroupValues->end() && outer->value)
                        {
                            for (const auto &entry : *outer->value)
                            {
                                if (::_stricmp(Util::SafeText(entry.key.c_str()), slider.groupName.c_str()) == 0)
                                {
                                    found = true;
                                    value = entry.value;
                                    break;
                                }
                            }
                        }
                    }
                    const auto expected = static_cast<float>(slider.value);
                    if ((found && value != expected) || (!found && expected != 0.0F))
                    {
                        REX::WARN("[NPCPresetApplicator] detached preset morph validation failed field='FacialBoneRegionDataA' region={} sliderID={} group='{}' found={} actual={} expected={}",
                                  region.regionID, slider.id, slider.groupName, found, value, expected);
                        return false;
                    }
                }
            }

            if (a_target->shapeBlendData)
            {
                for (const auto &actual : *a_target->shapeBlendData)
                {
                    const auto expected = std::ranges::find_if(a_preset.facialMorphSliders, [&](const Config::PresetNamedMorph &a_morph)
                                                               { return ::_stricmp(Util::SafeText(actual.key.c_str()), a_morph.name.c_str()) == 0; });
                    if (expected == a_preset.facialMorphSliders.end())
                    {
                        REX::WARN("[NPCPresetApplicator] detached preset morph validation failed field='FacialMorphSliderDataA' unexpectedName='{}'", Util::SafeText(actual.key.c_str()));
                        return false;
                    }
                }
            }
            if (a_target->facialBoneValues)
            {
                for (const auto &actual : *a_target->facialBoneValues)
                {
                    const bool expected = std::ranges::any_of(a_preset.facialBoneRegions, [&](const Config::PresetBoneRegion &a_region)
                                                              { return std::ranges::any_of(a_region.sliders, [&](const Config::PresetBoneSlider &a_slider)
                                                                                           { return a_slider.id != 0 && a_slider.id == actual.key; }); });
                    if (!expected)
                    {
                        REX::WARN("[NPCPresetApplicator] detached preset morph validation failed field='FacialBoneRegionDataA' unexpectedSliderID={}", actual.key);
                        return false;
                    }
                }
            }
            if (a_target->facialBoneGroupValues)
            {
                for (const auto &actualRegion : *a_target->facialBoneGroupValues)
                {
                    if (!actualRegion.value)
                    {
                        continue;
                    }
                    for (const auto &actualSlider : *actualRegion.value)
                    {
                        const auto expectedRegion = std::ranges::find(a_preset.facialBoneRegions, actualRegion.key, &Config::PresetBoneRegion::regionID);
                        if (expectedRegion == a_preset.facialBoneRegions.end())
                        {
                            REX::WARN("[NPCPresetApplicator] detached preset morph validation failed field='FacialBoneRegionDataA' unexpectedRegion={}", actualRegion.key);
                            return false;
                        }
                        const auto expectedSlider = std::ranges::find_if(expectedRegion->sliders, [&](const Config::PresetBoneSlider &a_slider)
                                                                         { return a_slider.id == 0 && ::_stricmp(Util::SafeText(actualSlider.key.c_str()), a_slider.groupName.c_str()) == 0; });
                        if (expectedSlider == expectedRegion->sliders.end())
                        {
                            REX::WARN("[NPCPresetApplicator] detached preset morph validation failed field='FacialBoneRegionDataA' region={} unexpectedGroup='{}'",
                                      actualRegion.key, Util::SafeText(actualSlider.key.c_str()));
                            return false;
                        }
                    }
                }
            }
            return true;
        }

        bool ValidateVisuals(RE::TESNPC *a_target, const Config::AppearancePreset &a_preset, const Config::ResolvedAppearanceDependencies &a_dependencies)
        {
            std::vector<RE::BGSHeadPart *> headParts;
            {
                auto locked = a_target->headParts.Lock();
                headParts.assign((*locked).begin(), (*locked).end());
            }
            for (std::size_t i = 0; i < a_dependencies.uniqueHeadParts.size(); ++i)
            {
                const auto expected = a_dependencies.uniqueHeadParts[i];
                const auto actual = std::ranges::find_if(headParts, [&](const RE::BGSHeadPart *a_part)
                                                         { return a_part && static_cast<std::size_t>(a_part->type.get()) == i; });
                if (i == static_cast<std::size_t>(RE::BGSHeadPart::HeadPartType::kMisc))
                {
                    if (expected && std::ranges::find(headParts, expected) == headParts.end())
                    {
                        REX::WARN("[NPCPresetApplicator] detached preset visual validation failed field='UniqueHeadPartsA[{}]' expected=0x{:08X} missing", i, expected->GetFormID());
                        return false;
                    }
                }
                else if (expected ? actual == headParts.end() || *actual != expected : actual != headParts.end())
                {
                    REX::WARN("[NPCPresetApplicator] detached preset visual validation failed field='UniqueHeadPartsA[{}]' actual=0x{:08X} expected=0x{:08X}",
                              i,
                              actual != headParts.end() && *actual ? (*actual)->GetFormID() : 0,
                              expected ? expected->GetFormID() : 0);
                    return false;
                }
            }
            for (auto *part : a_dependencies.miscHeadParts)
            {
                if (std::ranges::find(headParts, part) == headParts.end())
                {
                    REX::WARN("[NPCPresetApplicator] detached preset visual validation failed field='MiscHeadPartsA' expected=0x{:08X} missing", part ? part->GetFormID() : 0);
                    return false;
                }
            }

            if (a_target->skinToneIndex != a_preset.skinTone)
            {
                REX::WARN("[NPCPresetApplicator] detached preset visual validation failed field='SkinTone' actual={} expected={}", a_target->skinToneIndex, a_preset.skinTone);
                return false;
            }
            const std::array colorFields{
                std::tuple{"TeethCustomization", Util::SafeText(a_target->teeth.c_str()), a_preset.teethCustomization.c_str()},
                std::tuple{"JewelryColor", Util::SafeText(a_target->jewelryColor.c_str()), a_preset.jewelryColor.c_str()},
                std::tuple{"EyeColor", Util::SafeText(a_target->eyeColor.c_str()), a_preset.eyeColor.c_str()},
                std::tuple{"HairColor", Util::SafeText(a_target->hairColor.c_str()), a_preset.hairColor.c_str()},
                std::tuple{"FacialHairColor", Util::SafeText(a_target->facialColor.c_str()), a_preset.facialHairColor.c_str()},
                std::tuple{"BrowHairColor", Util::SafeText(a_target->eyebrowColor.c_str()), a_preset.browHairColor.c_str()}};
            for (const auto &[field, actual, expected] : colorFields)
            {
                if (!SameText(actual, expected))
                {
                    REX::WARN("[NPCPresetApplicator] detached preset visual validation failed field='{}' actual='{}' expected='{}'", field, actual, expected);
                    return false;
                }
            }
            if (a_target->tintAVMData.size() != a_dependencies.avmLayers.size())
            {
                REX::WARN("[NPCPresetApplicator] detached preset visual validation failed field='PostBlendFaceCustomization' actualCount={} expectedCount={}",
                          a_target->tintAVMData.size(), a_dependencies.avmLayers.size());
                return false;
            }
            for (const auto &expected : a_dependencies.avmLayers)
            {
                const auto actual = std::ranges::find_if(a_target->tintAVMData, [&](const RE::AVMData &a_entry)
                                                         { return ::_stricmp(Util::SafeText(a_entry.category.c_str()), Util::SafeText(expected.category.c_str())) == 0; });
                if (actual == a_target->tintAVMData.end() || actual->type != expected.type || !SameText(actual->unk10.name.c_str(), expected.unk10.name.c_str()) ||
                    !SameText(actual->unk10.texturePath.c_str(), expected.unk10.texturePath.c_str()) || actual->unk10.color != expected.unk10.color || actual->unk10.intensity != expected.unk10.intensity)
                {
                    REX::WARN("[NPCPresetApplicator] detached preset visual validation failed field='PostBlendFaceCustomization' category='{}' found={} actualType={} expectedType={} actualValue='{}' expectedValue='{}' actualTexture='{}' expectedTexture='{}' actualColor={} expectedColor={} actualIntensity={} expectedIntensity={}",
                              Util::SafeText(expected.category.c_str()),
                              actual != a_target->tintAVMData.end(),
                              actual != a_target->tintAVMData.end() ? static_cast<std::uint32_t>(actual->type) : 0,
                              static_cast<std::uint32_t>(expected.type),
                              actual != a_target->tintAVMData.end() ? Util::SafeText(actual->unk10.name.c_str()) : "",
                              Util::SafeText(expected.unk10.name.c_str()),
                              actual != a_target->tintAVMData.end() ? Util::SafeText(actual->unk10.texturePath.c_str()) : "",
                              Util::SafeText(expected.unk10.texturePath.c_str()),
                              actual != a_target->tintAVMData.end() ? actual->unk10.color.ToInt() : 0,
                              expected.unk10.color.ToInt(),
                              actual != a_target->tintAVMData.end() ? actual->unk10.intensity : 0,
                              expected.unk10.intensity);
                    return false;
                }
            }
            if (a_target->faceNPC)
            {
                REX::WARN("[NPCPresetApplicator] detached preset visual validation failed field='faceNPC' actual=0x{:08X} expected=0x00000000", a_target->faceNPC->GetFormID());
                return false;
            }
            return true;
        }
    }

    RE::TESNPC *PrepareRenderSource(
        RE::TESNPC *a_target,
        const Config::AppearancePreset &a_preset,
        const Config::ResolvedAppearanceDependencies &a_dependencies,
        const bool a_generatedLeveledBase)
    {
        if (!a_target)
        {
            REX::WARN("[NPCPresetApplicator] detached render-source preparation received a null target");
            return nullptr;
        }
        if (!a_dependencies.Complete() || !a_dependencies.race)
        {
            REX::ERROR("[NPCPresetApplicator] detached render-source preparation received incomplete resolved dependencies for base=0x{:08X}", a_target->GetFormID());
            return nullptr;
        }

        auto *targetRace = a_target->GetRace();
        auto *inheritedRace = a_target->faceNPC ? a_target->faceNPC->GetRace() : nullptr;
        if (targetRace != a_dependencies.race)
        {
            const bool missingGeneratedRace = a_generatedLeveledBase && !targetRace && (!inheritedRace || inheritedRace == a_dependencies.race);
            if (!missingGeneratedRace)
            {
                REX::WARN("[NPCPresetApplicator] base=0x{:08X} race mismatch target=0x{:08X} inherited=0x{:08X} expected=0x{:08X}; detached source rejected",
                          a_target->GetFormID(),
                          targetRace ? targetRace->GetFormID() : 0,
                          inheritedRace ? inheritedRace->GetFormID() : 0,
                          a_dependencies.race->GetFormID());
                return nullptr;
            }

            REX::DEBUG("[NPCPresetApplicator] generated leveled base=0x{:08X} has no materialized race; using configured race=0x{:08X} inheritedRace=0x{:08X}",
                       a_target->GetFormID(),
                       a_dependencies.race->GetFormID(),
                       inheritedRace ? inheritedRace->GetFormID() : 0);
        }

        const auto canonicalState = CaptureOriginalNPCState(a_target);
        const auto canonicalStorage = CaptureVisualStorageState(a_target);
        auto *source = CreateRenderSourceNPC();
        if (!source)
        {
            return nullptr;
        }

        try
        {
            // copy set of non-owned fields FaceDB consumes from TESNPC argument, then let engine deep-copy all owned appearance containers. The canonical source is only ever passed as a read source.
            source->actorData = a_target->actorData;
            source->formRace = targetRace ? targetRace : a_dependencies.race;
            source->formSkin = a_target->formSkin;
            source->originalRace = source->formRace;
            source->height = a_target->height;
            source->heightMax = a_target->heightMax;
            source->pronoun = a_target->pronoun;
            source->CopyAppearance(a_target, false);
            source->formRace = targetRace ? targetRace : a_dependencies.race;
            // A detached carrier has no entry in the engine's pointer-keyed
            // alternate-race head-part cache. Make it a self-contained NPC so
            // native appearance builders enumerate its owned headParts array.
            source->originalRace = source->formRace;
            source->faceNPC = nullptr;

            const bool copiedExactly = SameExactVisualValues(source, a_target);
            const bool independent = HasIndependentVisualStorage(canonicalStorage, CaptureVisualStorageState(source));
            const bool canonicalPreservedAfterCopy = CaptureOriginalNPCState(a_target) == canonicalState && CaptureVisualStorageState(a_target) == canonicalStorage;
            if (!copiedExactly || !independent || !canonicalPreservedAfterCopy)
            {
                REX::WARN("[NPCPresetApplicator] detached baseline copy failed exact={} independent={} canonicalPreserved={}", copiedExactly, independent, canonicalPreservedAfterCopy);
                DestroyUnpublishedRenderSource(source);
                return nullptr;
            }

            ApplyMorphs(source, a_preset);
            ApplyVisuals(source, a_preset, a_dependencies);

            source->actorData.actorBaseFlags = static_cast<RE::ACTOR_BASE_DATA::Flag>(
                source->actorData.actorBaseFlags.underlying() | kRuntimeGeneratedFaceFlag);

            const bool morphsValid = ValidateMorphs(source, a_preset);
            const bool visualsValid = morphsValid && ValidateVisuals(source, a_preset, a_dependencies);
            const bool presetValid = morphsValid && visualsValid;
            const bool sourceInvariant = source->GetFormID() == 0 && source->QRefCount() == 0 && source->GetRace() == a_dependencies.race &&
                                         source->originalRace == source->formRace &&
                                         source->actorData.actorBaseFlags.underlying() == (canonicalState.actorFlags | kRuntimeGeneratedFaceFlag);
            const bool canonicalPreserved = CaptureOriginalNPCState(a_target) == canonicalState && CaptureVisualStorageState(a_target) == canonicalStorage;
            if (!presetValid || !sourceInvariant || !canonicalPreserved)
            {
                REX::WARN("[NPCPresetApplicator] detached preset validation failed morphsValid={} visualsValid={} sourceInvariant={} canonicalPreserved={}",
                          morphsValid, visualsValid, sourceInvariant, canonicalPreserved);
                DestroyUnpublishedRenderSource(source);
                return nullptr;
            }

            return source;
        }
        catch (const std::exception &error)
        {
            REX::ERROR("[NPCPresetApplicator] detached render-source preparation threw: {}", error.what());
        }
        catch (...)
        {
            REX::ERROR("[NPCPresetApplicator] detached render-source preparation threw an unknown exception");
        }

        DestroyUnpublishedRenderSource(source);
        return nullptr;
    }

    bool RefreshAppearanceFromRenderSource(RE::TESNPC *a_target, RE::TESNPC *a_source, RE::Actor *a_actor, const RE::TESFormID a_actorRefID)
    {
        if (!a_target || !a_source || !a_actor || a_source->GetFormID() != 0 || FindOwnedRenderSource(a_target) != a_source ||
            a_actor->GetNPC() != a_target || a_actor->GetFormID() != a_actorRefID)
        {
            return false;
        }

        a_actor->RefreshAppearance(false, 0x28, false);
        return a_actor->GetNPC() == a_target && a_actor->GetFormID() == a_actorRefID;
    }
}
