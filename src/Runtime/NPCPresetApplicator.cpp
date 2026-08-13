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
        bool SameText(const char* a_left, const std::string& a_right)
        {
            return ::_stricmp(Util::SafeText(a_left), a_right.c_str()) == 0;
        }

        bool SameText(const char* a_left, const char* a_right)
        {
            return ::_stricmp(Util::SafeText(a_left), Util::SafeText(a_right)) == 0;
        }

        void ApplyMorphs(RE::TESNPC* a_target, const Config::AppearancePreset& a_preset)
        {
            a_target->morphWeight.thin = static_cast<float>(a_preset.morphWeights.x);
            a_target->morphWeight.muscular = static_cast<float>(a_preset.morphWeights.y);
            a_target->morphWeight.fat = static_cast<float>(a_preset.morphWeights.z);
            for (std::size_t i = 0; i < a_preset.bodyMorphRegionValues.size(); ++i) {
                a_target->SetBodyMorph(static_cast<std::uint32_t>(i), static_cast<float>(a_preset.bodyMorphRegionValues[i]));
            }
            for (const auto& morph : a_preset.facialMorphSliders) {
                const RE::BSFixedStringCS key{ morph.name.c_str() };
                a_target->SetShapeBlend(key, static_cast<float>(morph.value));
            }
            for (const auto& region : a_preset.facialBoneRegions) {
                for (const auto& slider : region.sliders) {
                    if (slider.id != 0) {
                        a_target->SetFacialBone(slider.id, static_cast<float>(slider.value));
                        continue;
                    }
                    const RE::BSFixedStringCS key{ slider.groupName.c_str() };
                    a_target->EnsureFacialBoneGroup(region.regionID, key);
                    if (!a_target->unk3E8) {
                        continue;
                    }
                    const auto outer = a_target->unk3E8->find(region.regionID);
                    if (outer == a_target->unk3E8->end() || !outer->value) {
                        continue;
                    }
                    for (auto& entry : *outer->value) {
                        if (::_stricmp(Util::SafeText(entry.key.c_str()), slider.groupName.c_str()) == 0) {
                            entry.value = static_cast<float>(slider.value);
                            break;
                        }
                    }
                }
            }
        }

        void ApplyVisuals(RE::TESNPC* a_target, const Config::AppearancePreset& a_preset, const Config::ResolvedAppearanceDependencies& a_dependencies)
        {
            std::vector<RE::BGSHeadPart*> originalHeadParts;
            {
                auto headParts = a_target->headParts.Lock();
                originalHeadParts.assign((*headParts).begin(), (*headParts).end());
            }
            for (std::size_t i = 1; i < a_dependencies.uniqueHeadParts.size(); ++i) {
                if (a_dependencies.uniqueHeadParts[i]) {
                    continue;
                }
                for (auto* part : originalHeadParts) {
                    if (part && static_cast<std::size_t>(part->type.get()) == i) {
                        a_target->RemoveHeadPart(part, false);
                    }
                }
            }
            for (auto* part : a_dependencies.uniqueHeadParts) {
                if (!part) {
                    continue;
                }
                bool present = false;
                {
                    auto headParts = a_target->headParts.Lock();
                    present = std::ranges::find(*headParts, part) != (*headParts).end();
                }
                if (!present) {
                    a_target->ChangeHeadPart(part);
                }
            }
            for (auto* part : a_dependencies.miscHeadParts) {
                bool present = false;
                {
                    auto headParts = a_target->headParts.Lock();
                    present = std::ranges::find(*headParts, part) != (*headParts).end();
                }
                if (!present) {
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
            for (const auto& avm : a_target->tintAVMData) {
                existingCategories.push_back(avm.category);
            }
            for (const auto& category : existingCategories) {
                const bool desired = std::ranges::any_of(a_dependencies.avmLayers, [&](const RE::AVMData& a_expected) {
                    return ::_stricmp(Util::SafeText(category.c_str()), Util::SafeText(a_expected.category.c_str())) == 0;
                });
                if (!desired) {
                    a_target->RemoveAVMData(category);
                }
            }
            for (const auto& avm : a_dependencies.avmLayers) {
                a_target->SetAVMData(avm);
            }
            a_target->faceNPC = nullptr;
        }

        bool ValidateMorphs(RE::TESNPC* a_target, const Config::AppearancePreset& a_preset)
        {
            if (a_target->morphWeight.thin != static_cast<float>(a_preset.morphWeights.x) || a_target->morphWeight.muscular != static_cast<float>(a_preset.morphWeights.y) || 
                a_target->morphWeight.fat != static_cast<float>(a_preset.morphWeights.z)) {
                return false;
            }

            if (!a_preset.bodyMorphRegionValues.empty()) {
                if (!a_target->bodyMorphValues || a_target->bodyMorphValues->size() != a_preset.bodyMorphRegionValues.size()) {
                    return false;
                }
                for (std::size_t i = 0; i < a_preset.bodyMorphRegionValues.size(); ++i) {
                    if ((*a_target->bodyMorphValues)[static_cast<std::uint32_t>(i)] != static_cast<float>(a_preset.bodyMorphRegionValues[i])) {
                        return false;
                    }
                }
            }
            for (const auto& morph : a_preset.facialMorphSliders) {
                float actual = 0.0F;
                bool found = false;
                if (a_target->shapeBlendData) {
                    for (const auto& entry : *a_target->shapeBlendData) {
                        if (::_stricmp(Util::SafeText(entry.key.c_str()), morph.name.c_str()) == 0) {
                            actual = entry.value;
                            found = true;
                            break;
                        }
                    }
                }
                const auto expected = static_cast<float>(morph.value);
                if (!((expected == 0.0F && (!found || actual == 0.0F)) || (found && actual == expected))) {
                    return false;
                }
            }
            for (const auto& region : a_preset.facialBoneRegions) {
                for (const auto& slider : region.sliders) {
                    bool found = false;
                    float value = 0.0F;
                    if (slider.id != 0 && a_target->facialBoneValues) {
                        const auto entry = a_target->facialBoneValues->find(slider.id);
                        if (entry != a_target->facialBoneValues->end()) {
                            found = true;
                            value = entry->value;
                        }
                    } else if (slider.id == 0 && a_target->unk3E8) {
                        const auto outer = a_target->unk3E8->find(region.regionID);
                        if (outer != a_target->unk3E8->end() && outer->value) {
                            for (const auto& entry : *outer->value) {
                                if (::_stricmp(Util::SafeText(entry.key.c_str()), slider.groupName.c_str()) == 0) {
                                    found = true;
                                    value = entry.value;
                                    break;
                                }
                            }
                        }
                    }
                    const auto expected = static_cast<float>(slider.value);
                    if ((found && value != expected) || (!found && expected != 0.0F)) {
                        return false;
                    }
                }
            }
            return true;
        }

        bool ValidateVisuals(RE::TESNPC* a_target, const Config::AppearancePreset& a_preset, const Config::ResolvedAppearanceDependencies& a_dependencies)
        {
            std::vector<RE::BGSHeadPart*> headParts;
            {
                auto locked = a_target->headParts.Lock();
                headParts.assign((*locked).begin(), (*locked).end());
            }
            for (std::size_t i = 0; i < a_dependencies.uniqueHeadParts.size(); ++i) {
                const auto expected = a_dependencies.uniqueHeadParts[i];
                const auto actual = std::ranges::find_if(headParts, [&](const RE::BGSHeadPart* a_part) {
                    return a_part && static_cast<std::size_t>(a_part->type.get()) == i;
                });
                if (i == static_cast<std::size_t>(RE::BGSHeadPart::HeadPartType::kMisc)) {
                    if (expected && std::ranges::find(headParts, expected) == headParts.end()) {
                        return false;
                    }
                } else if (expected ? actual == headParts.end() || *actual != expected : actual != headParts.end()) {
                    return false;
                }
            }
            for (auto* part : a_dependencies.miscHeadParts) {
                if (std::ranges::find(headParts, part) == headParts.end()) {
                    return false;
                }
            }

            if (a_target->skinToneIndex != a_preset.skinTone || !SameText(a_target->teeth.c_str(), a_preset.teethCustomization) || !SameText(a_target->jewelryColor.c_str(), a_preset.jewelryColor) ||
                !SameText(a_target->eyeColor.c_str(), a_preset.eyeColor) || !SameText(a_target->hairColor.c_str(), a_preset.hairColor) || !SameText(a_target->facialColor.c_str(), a_preset.facialHairColor) ||
                !SameText(a_target->eyebrowColor.c_str(), a_preset.browHairColor) || a_target->tintAVMData.size() != a_dependencies.avmLayers.size()) {
                return false;
            }
            for (const auto& expected : a_dependencies.avmLayers) {
                const auto actual = std::ranges::find_if(a_target->tintAVMData, [&](const RE::AVMData& a_entry) {
                    return ::_stricmp(Util::SafeText(a_entry.category.c_str()), Util::SafeText(expected.category.c_str())) == 0;
                });
                if (actual == a_target->tintAVMData.end() || actual->type != expected.type || !SameText(actual->unk10.name.c_str(), expected.unk10.name.c_str()) || 
                    !SameText(actual->unk10.texturePath.c_str(), expected.unk10.texturePath.c_str()) || actual->unk10.color != expected.unk10.color || actual->unk10.intensity != expected.unk10.intensity) {
                    return false;
                }
            }
            return a_target->faceNPC == nullptr;
        }
    }

    RE::TESNPC* PrepareRenderSource(RE::TESNPC* a_target, const Config::AppearancePreset& a_preset, const Config::ResolvedAppearanceDependencies& a_dependencies)
    {
        if (!a_target || !a_dependencies.Complete() || a_target->GetRace() != a_dependencies.race) {
            return nullptr;
        }

        const auto canonicalState = CaptureOriginalNPCState(a_target);
        const auto canonicalStorage = CaptureVisualStorageState(a_target);
        auto* source = CreateRenderSourceNPC();
        if (!source) {
            return nullptr;
        }

        try {
            // copy set of non-owned fields FaceDB consumes from TESNPC argument, then let engine deep-copy all owned appearance containers. The canonical source is only ever passed as a read source.
            source->actorData = a_target->actorData;
            source->formRace = a_target->formRace;
            source->formSkin = a_target->formSkin;
            source->originalRace = a_target->originalRace;
            source->height = a_target->height;
            source->heightMax = a_target->heightMax;
            source->pronoun = a_target->pronoun;
            source->CopyAppearance(a_target, false);
            source->faceNPC = nullptr;

            const bool copiedExactly = SameExactVisualValues(source, a_target);
            const bool independent = HasIndependentVisualStorage(canonicalStorage, CaptureVisualStorageState(source));
            const bool canonicalPreservedAfterCopy = CaptureOriginalNPCState(a_target) == canonicalState && CaptureVisualStorageState(a_target) == canonicalStorage;
            if (!copiedExactly || !independent || !canonicalPreservedAfterCopy) {
                REX::WARN("[NPCPresetApplicator] detached baseline copy failed exact={} independent={} canonicalPreserved={}", copiedExactly, independent, canonicalPreservedAfterCopy);
                DestroyUnpublishedRenderSource(source);
                return nullptr;
            }

            ApplyMorphs(source, a_preset);
            ApplyVisuals(source, a_preset, a_dependencies);

            // AddChange(0x800) sets this internal rebuild bit. Set it only on the unregistered carrier; never notify or dirty the canonical form.
            source->actorData.actorBaseFlags = static_cast<RE::ACTOR_BASE_DATA::Flag>(source->actorData.actorBaseFlags.underlying() | 0x8000U);

            const bool presetValid = ValidateMorphs(source, a_preset) && ValidateVisuals(source, a_preset, a_dependencies);
            const bool sourceInvariant = source->GetFormID() == 0 && source->QRefCount() == 0 && source->GetRace() == a_dependencies.race && (source->actorData.actorBaseFlags.underlying() & 0x8000U) != 0;
            const bool canonicalPreserved = CaptureOriginalNPCState(a_target) == canonicalState && CaptureVisualStorageState(a_target) == canonicalStorage;
            if (!presetValid || !sourceInvariant || !canonicalPreserved) {
                REX::WARN("[NPCPresetApplicator] detached preset validation failed presetValid={} sourceInvariant={} canonicalPreserved={}", presetValid, sourceInvariant, canonicalPreserved);
                DestroyUnpublishedRenderSource(source);
                return nullptr;
            }

            return source;
        } catch (const std::exception& error) {
            REX::ERROR("[NPCPresetApplicator] detached render-source preparation threw: {}", error.what());
        } catch (...) {
            REX::ERROR("[NPCPresetApplicator] detached render-source preparation threw an unknown exception");
        }

        DestroyUnpublishedRenderSource(source);
        return nullptr;
    }

    bool RefreshAppearanceFromRenderSource(RE::TESNPC* a_target, RE::TESNPC* a_source, RE::Actor* a_actor, const RE::TESFormID a_actorRefID)
    {
        if (!a_target || !a_source || !a_actor || a_source->GetFormID() != 0 || FindRenderSource(a_target) != a_source ||
            a_actor->GetNPC() != a_target || a_actor->GetFormID() != a_actorRefID) {
            return false;
        }

        a_actor->RefreshAppearance(false, 0x28, false);
        return a_actor->GetNPC() == a_target && a_actor->GetFormID() == a_actorRefID;
    }
}
