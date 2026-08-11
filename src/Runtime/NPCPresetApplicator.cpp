#include "NPCPresetApplicator.h"

#include "NPCSnapshot.h"
#include "Util/String.h"

#include <memory>
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

        bool IsRegisteredEmptyDonor(RE::TESNPC* a_donor, const RE::TESFormID a_formID)
        {
            if (!a_donor || a_formID == 0 || RE::TESForm::LookupByID<RE::TESNPC>(a_formID) != a_donor || a_donor->QRefCount() != 0 || a_donor->GetRace() != nullptr || a_donor->faceNPC != nullptr || a_donor->bodyMorphValues != nullptr ||
                a_donor->facialBoneValues != nullptr || a_donor->unk3E8 != nullptr || !a_donor->tintAVMData.empty() || a_donor->shapeBlendData != nullptr || a_donor->pronoun.underlying() != 0) {
                return false;
            }

            auto headParts = a_donor->headParts.Lock();
            return (*headParts).empty();
        }

        bool ReleaseDonor(std::unique_ptr<RE::TESNPC>& a_donor, const RE::TESFormID a_formID)
        {
            a_donor.reset();
            return a_formID != 0 && RE::TESForm::LookupByID<RE::TESNPC>(a_formID) == nullptr;
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

            a_target->skinToneIndex = static_cast<std::uint8_t>(a_preset.skinTone);
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
                a_target->morphWeight.fat != static_cast<float>(a_preset.morphWeights.z) || !a_target->bodyMorphValues || a_target->bodyMorphValues->size() != a_preset.bodyMorphRegionValues.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a_preset.bodyMorphRegionValues.size(); ++i) {
                if ((*a_target->bodyMorphValues)[static_cast<std::uint32_t>(i)] != static_cast<float>(a_preset.bodyMorphRegionValues[i])) {
                    return false;
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

            if (a_target->skinToneIndex != static_cast<std::uint8_t>(a_preset.skinTone) || !SameText(a_target->teeth.c_str(), a_preset.teethCustomization) || !SameText(a_target->jewelryColor.c_str(), a_preset.jewelryColor) ||
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

    PreparedAppearanceApplyResult ApplyPreparedAppearance(RE::TESNPC* a_target, const Config::AppearancePreset& a_preset, const Config::ResolvedAppearanceDependencies& a_dependencies)
    {
        PreparedAppearanceApplyResult result;
        if (!a_target || !a_dependencies.Complete() ||
            a_target->GetRace() != a_dependencies.race) {
            return result;
        }
        const auto original = CaptureOriginalNPCState(a_target);
        const auto originalStorage = CaptureVisualStorageState(a_target);
        std::unique_ptr<RE::TESNPC> donor{ RE::TESNPC::Create(false) };
        if (!donor) {
            return result;
        }

        result.donorReleased = false;
        const auto donorFormID = donor->GetFormID();
        if (!IsRegisteredEmptyDonor(donor.get(), donorFormID)) {
            result.donorReleased = ReleaseDonor(donor, donorFormID);
            return result;
        }

        try {
            donor->CopyAppearance(a_target, false);
            const bool donorCopiedExactly = SameExactVisualValues(donor.get(), a_target);
            const bool donorIndependent = HasIndependentVisualStorage(originalStorage, CaptureVisualStorageState(donor.get()));
            const bool sourcePreserved = CaptureNonVisualState(a_target) == original.nonVisual && a_target->faceNPC == original.faceNPC && a_target->actorData.actorBaseFlags.underlying() == original.actorFlags;
            if (donorCopiedExactly && donorIndependent && sourcePreserved) {
                ApplyMorphs(donor.get(), a_preset);
                ApplyVisuals(donor.get(), a_preset, a_dependencies);

                const bool donorValid = ValidateMorphs(donor.get(), a_preset) && ValidateVisuals(donor.get(), a_preset, a_dependencies);
                if (donorValid) {
                    a_target->morphWeight = donor->morphWeight;
                    for (std::size_t i = 0; i < a_preset.bodyMorphRegionValues.size(); ++i) {
                        a_target->SetBodyMorph(static_cast<std::uint32_t>(i), static_cast<float>(a_preset.bodyMorphRegionValues[i]));
                    }
                    a_target->skinToneIndex = donor->skinToneIndex;
                    a_target->CopyOwnedAppearance(donor.get(), false);
                    a_target->faceNPC = nullptr;

                    result.applied = ValidateMorphs(a_target, a_preset) && ValidateVisuals(a_target, a_preset, a_dependencies) && SameExactVisualValues(a_target, donor.get()) &&
                        HasIndependentVisualStorage(CaptureVisualStorageState(donor.get()), CaptureVisualStorageState(a_target)) && CaptureNonVisualState(a_target) == original.nonVisual &&
                        a_target->actorData.actorBaseFlags.underlying() == original.actorFlags;
                }
            }
        } catch (const std::exception& error) {
            REX::ERROR("[NPCPresetApplicator] prepared appearance apply threw before donor teardown: {}", error.what());
        } catch (...) {
            REX::ERROR("[NPCPresetApplicator] prepared appearance apply threw an unknown exception before donor teardown");
        }

        result.donorReleased = ReleaseDonor(donor, donorFormID);
        return result;
    }

    bool NotifyAndRefreshAppearance(RE::TESNPC* a_target, RE::Actor* a_actor, const RE::TESFormID a_actorRefID)
    {
        if (!a_target || !a_actor || a_actor->GetNPC() != a_target || a_actor->GetFormID() != a_actorRefID) {
            return false;
        }

        static_cast<void>(a_target->AddChange(0x800));
        static_cast<void>(a_target->AddChange(0x4000));
        a_actor->RefreshAppearance(false, 0x28, false);
        return true;
    }
}
