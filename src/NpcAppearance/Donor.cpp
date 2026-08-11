#include "NpcAppearance/RuntimeDetail.h"

#include "NpcAppearance/Preset.h"
#include "NpcAppearance/Resolver.h"
#include "pch.h"

#include "Util/StarfieldRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NpcAppearance
{
    namespace
    {
        using namespace Detail;

        using ResolveFaceDbEntry = bool (*)(
            std::uint32_t, const RE::BSFixedString*, const RE::BSFixedString*,
            RE::AVMData::Entry*);

        struct MaterializedAvmLayer
        {
            RE::AVMData data;
            std::string modulationValue;
        };

        [[nodiscard]] bool MaterializeAvmLayers(
            const LineSink& a_out,
            const AppearancePreset& a_preset,
            ResolveFaceDbEntry a_resolveEntry,
            std::vector<MaterializedAvmLayer>& a_outLayers)
        {
            a_outLayers.clear();
            a_outLayers.reserve(a_preset.postBlendLayers.size());
            for (const auto& layer : a_preset.postBlendLayers) {
                MaterializedAvmLayer materialized;
                materialized.data.category = RE::BSFixedString{ layer.name };
                materialized.modulationValue = layer.modulationValue;
                const RE::BSFixedString value{ layer.value };

                std::uint32_t matchedStore = 0;
                for (std::uint32_t store = 1; store <= 2; ++store) {
                    RE::AVMData::Entry candidate;
                    if (a_resolveEntry(store, &materialized.data.category,
                                       &value, &candidate)) {
                        if (matchedStore != 0) {
                            a_out(std::format(
                                "donorvisual: AVM layer '{}' value '{}' resolves in multiple primary FaceDB stores",
                                layer.name, layer.value));
                            return false;
                        }
                        matchedStore = store;
                        materialized.data.unk10 = candidate;
                    }
                }
                if (matchedStore == 0) {
                    a_out(std::format(
                        "donorvisual: AVM layer '{}' value '{}' did not materialize from primary FaceDB stores",
                        layer.name, layer.value));
                    return false;
                }
                materialized.data.type = static_cast<RE::AVMData::Type>(matchedStore);

                if (!layer.modulationValue.empty()) {
                    const RE::BSFixedString modulationValue{ layer.modulationValue };
                    RE::AVMData::Entry modulation;
                    if (!a_resolveEntry(3, &materialized.data.category,
                                        &modulationValue, &modulation)) {
                        a_out(std::format(
                            "donorvisual: AVM layer '{}' modulation '{}' did not materialize from FaceDB store 3",
                            layer.name, layer.modulationValue));
                        return false;
                    }
                    materialized.data.unk10.color = modulation.color;
                }

                materialized.data.unk10.intensity = static_cast<std::uint32_t>(
                    std::floor(std::clamp(layer.intensity, 0.0, 1.0) * 64.0));
                a_outLayers.push_back(std::move(materialized));
            }
            return true;
        }

        using SetShapeBlend = void (*)(
            RE::TESNPC*, const RE::BSFixedStringCS*, float);
        using SetBodyMorph = void (*)(RE::TESNPC*, std::uint32_t, float);
        using SetFacialBone = void (*)(RE::TESNPC*, std::uint32_t, float);
        using EnsureFacialBoneGroup = void (*)(
            RE::TESNPC*, std::uint32_t, const RE::BSFixedStringCS*);
        using RemoveHeadPart = void (*)(RE::TESNPC*, RE::BGSHeadPart*, bool);
        using ChangeHeadPart = void (*)(RE::TESNPC*, RE::BGSHeadPart*);
        using SetAvmData = void (*)(RE::TESNPC*, const RE::AVMData*);
        using RemoveAvmData = void (*)(RE::TESNPC*, const RE::BSFixedString*);
        using OwnedVisualCopy = void (*)(RE::TESNPC*, RE::TESNPC*, bool);
        using DestroyNpc = RE::TESNPC* (*)(RE::TESNPC*, std::uint32_t);
        using CreateNpc = RE::TESNPC* (*)(void*, bool);
        using CopyNpcAppearance = void (*)(RE::TESNPC*, RE::TESNPC*, bool);

        // ==================================================================
        // Preset -> donor population
        // Writes decoded preset data onto a temporary donor NPC through the
        // game's own setters; the donor is later copied onto the target and
        // destroyed.
        // ==================================================================
        void PopulatePresetMorphs(
            RE::TESNPC* a_donor,
            const AppearancePreset& a_preset,
            SetShapeBlend a_setShape,
            SetBodyMorph a_setBody,
            SetFacialBone a_setBone,
            EnsureFacialBoneGroup a_ensureBoneGroup)
        {
            a_donor->morphWeight.thin = static_cast<float>(a_preset.morphWeights.x);
            a_donor->morphWeight.muscular = static_cast<float>(a_preset.morphWeights.y);
            a_donor->morphWeight.fat = static_cast<float>(a_preset.morphWeights.z);
            for (std::size_t i = 0; i < a_preset.bodyMorphRegionValues.size(); ++i) {
                a_setBody(a_donor, static_cast<std::uint32_t>(i),
                          static_cast<float>(a_preset.bodyMorphRegionValues[i]));
            }
            for (const auto& morph : a_preset.facialMorphSliders) {
                const RE::BSFixedStringCS key{ morph.name.c_str() };
                a_setShape(a_donor, &key, static_cast<float>(morph.value));
            }
            for (const auto& region : a_preset.facialBoneRegions) {
                for (const auto& slider : region.sliders) {
                    if (slider.id != 0) {
                        a_setBone(a_donor, slider.id, static_cast<float>(slider.value));
                        continue;
                    }
                    const RE::BSFixedStringCS key{ slider.groupName.c_str() };
                    a_ensureBoneGroup(a_donor, region.regionID, &key);
                    if (!a_donor->unk3E8) {
                        continue;
                    }
                    const auto outer = a_donor->unk3E8->find(region.regionID);
                    if (outer == a_donor->unk3E8->end() || !outer->value) {
                        continue;
                    }
                    for (auto& entry : *outer->value) {
                        if (::_stricmp(SafeText(entry.key.c_str()),
                                       slider.groupName.c_str()) == 0) {
                            entry.value = static_cast<float>(slider.value);
                            break;
                        }
                    }
                }
            }
        }

        void PopulatePresetVisuals(
            RE::TESNPC* a_donor,
            const AppearancePreset& a_preset,
            const ResolvedAppearanceDependencies& a_resolved,
            const std::vector<MaterializedAvmLayer>& a_expectedAvms,
            RemoveHeadPart a_removeHeadPart,
            ChangeHeadPart a_changeHeadPart,
            SetAvmData a_setAvmData,
            RemoveAvmData a_removeAvmData)
        {
            std::vector<RE::BGSHeadPart*> donorHeadParts;
            {
                auto headParts = a_donor->headParts.Lock();
                donorHeadParts.assign((*headParts).begin(), (*headParts).end());
            }
            for (std::size_t i = 1; i < a_resolved.uniqueHeadParts.size(); ++i) {
                if (a_resolved.uniqueHeadParts[i]) {
                    continue;
                }
                for (auto* part : donorHeadParts) {
                    if (part && static_cast<std::size_t>(part->type.get()) == i) {
                        a_removeHeadPart(a_donor, part, false);
                    }
                }
            }
            for (auto* part : a_resolved.uniqueHeadParts) {
                if (!part) {
                    continue;
                }
                bool present = false;
                {
                    auto headParts = a_donor->headParts.Lock();
                    present = std::ranges::find(*headParts, part) != (*headParts).end();
                }
                if (!present) {
                    a_changeHeadPart(a_donor, part);
                }
            }
            for (auto* part : a_resolved.miscHeadParts) {
                bool present = false;
                {
                    auto headParts = a_donor->headParts.Lock();
                    present = std::ranges::find(*headParts, part) != (*headParts).end();
                }
                if (!present) {
                    a_changeHeadPart(a_donor, part);
                }
            }

            a_donor->skinToneIndex = static_cast<std::uint8_t>(a_preset.skinTone);
            a_donor->teeth = a_preset.teethCustomization;
            a_donor->jewelryColor = a_preset.jewelryColor;
            a_donor->eyeColor = a_preset.eyeColor;
            a_donor->hairColor = a_preset.hairColor;
            a_donor->facialColor = a_preset.facialHairColor;
            a_donor->eyebrowColor = a_preset.browHairColor;

            std::vector<RE::BSFixedString> existingAvmCategories;
            existingAvmCategories.reserve(a_donor->tintAVMData.size());
            for (const auto& avm : a_donor->tintAVMData) {
                existingAvmCategories.push_back(avm.category);
            }
            for (const auto& category : existingAvmCategories) {
                const bool desired = std::ranges::any_of(
                    a_expectedAvms, [&](const MaterializedAvmLayer& a_expected) {
                        return ::_stricmp(SafeText(category.c_str()),
                                          SafeText(a_expected.data.category.c_str())) == 0;
                    });
                if (!desired) {
                    a_removeAvmData(a_donor, &category);
                }
            }
            for (const auto& expected : a_expectedAvms) {
                a_setAvmData(a_donor, &expected.data);
            }
        }

        [[nodiscard]] bool ValidateDonorVisualPopulation(
            const LineSink& a_out,
            RE::TESNPC* a_donor,
            const AppearancePreset& a_preset,
            const ResolvedAppearanceDependencies& a_resolved,
            const std::vector<MaterializedAvmLayer>& a_expectedAvms)
        {
            std::size_t checked = 0;
            std::size_t failed = 0;
            std::size_t headPartFailed = 0;
            std::size_t colorFailed = 0;
            std::size_t avmFailed = 0;
            const auto check = [&](const bool a_condition, const std::string& a_label,
                                   std::size_t& a_categoryFailures) {
                ++checked;
                if (!a_condition) {
                    ++failed;
                    ++a_categoryFailures;
                    if (failed <= 12) {
                        a_out(std::format("donorvisual mismatch: {}", a_label));
                    }
                }
            };

            std::vector<RE::BGSHeadPart*> donorHeadParts;
            {
                auto headParts = a_donor->headParts.Lock();
                donorHeadParts.assign((*headParts).begin(), (*headParts).end());
            }
            for (std::size_t i = 0; i < a_resolved.uniqueHeadParts.size(); ++i) {
                const auto expected = a_resolved.uniqueHeadParts[i];
                const auto actual = std::ranges::find_if(
                    donorHeadParts, [&](const RE::BGSHeadPart* a_part) {
                        return a_part && static_cast<std::size_t>(a_part->type.get()) == i;
                    });
                if (i == static_cast<std::size_t>(RE::BGSHeadPart::HeadPartType::kMisc)) {
                    if (expected) {
                        check(std::ranges::find(donorHeadParts, expected) != donorHeadParts.end(),
                              std::format("UniqueHeadPartsA[{}]", i), headPartFailed);
                    }
                } else {
                    check(expected ? actual != donorHeadParts.end() && *actual == expected :
                                     actual == donorHeadParts.end(),
                          std::format("UniqueHeadPartsA[{}]", i), headPartFailed);
                }
            }
            for (std::size_t i = 0; i < a_resolved.miscHeadParts.size(); ++i) {
                check(std::ranges::find(donorHeadParts, a_resolved.miscHeadParts[i]) !=
                          donorHeadParts.end(),
                      std::format("MiscHeadPartsA[{}]", i), headPartFailed);
            }

            check(a_donor->skinToneIndex == static_cast<std::uint8_t>(a_preset.skinTone),
                  "SkinTone", colorFailed);
            check(::_stricmp(SafeText(a_donor->teeth.c_str()),
                             a_preset.teethCustomization.c_str()) == 0,
                  "TeethCustomization", colorFailed);
            check(::_stricmp(SafeText(a_donor->jewelryColor.c_str()),
                             a_preset.jewelryColor.c_str()) == 0,
                  "JewelryColor", colorFailed);
            check(::_stricmp(SafeText(a_donor->eyeColor.c_str()),
                             a_preset.eyeColor.c_str()) == 0,
                  "EyeColor", colorFailed);
            check(::_stricmp(SafeText(a_donor->hairColor.c_str()),
                             a_preset.hairColor.c_str()) == 0,
                  "HairColor", colorFailed);
            check(::_stricmp(SafeText(a_donor->facialColor.c_str()),
                             a_preset.facialHairColor.c_str()) == 0,
                  "FacialHairColor", colorFailed);
            check(::_stricmp(SafeText(a_donor->eyebrowColor.c_str()),
                             a_preset.browHairColor.c_str()) == 0,
                  "BrowHairColor", colorFailed);

            check(a_donor->tintAVMData.size() == a_expectedAvms.size(),
                  "PostBlendFaceCustomization.LayersA size", avmFailed);
            for (const auto& expected : a_expectedAvms) {
                const auto actual = std::ranges::find_if(
                    a_donor->tintAVMData, [&](const RE::AVMData& a_avm) {
                        return ::_stricmp(SafeText(a_avm.category.c_str()),
                                          SafeText(expected.data.category.c_str())) == 0;
                    });
                const auto prefix = std::format("AVM['{}']", expected.data.category.c_str());
                check(actual != a_donor->tintAVMData.end(), prefix + " present", avmFailed);
                if (actual == a_donor->tintAVMData.end()) {
                    continue;
                }
                check(actual->type == expected.data.type, prefix + " type", avmFailed);
                check(::_stricmp(SafeText(actual->unk10.name.c_str()),
                                 SafeText(expected.data.unk10.name.c_str())) == 0,
                      prefix + " value", avmFailed);
                check(::_stricmp(SafeText(actual->unk10.texturePath.c_str()),
                                 SafeText(expected.data.unk10.texturePath.c_str())) == 0,
                      prefix + " texture", avmFailed);
                check(actual->unk10.color == expected.data.unk10.color,
                      prefix + " color", avmFailed);
                check(actual->unk10.intensity == expected.data.unk10.intensity,
                      prefix + " intensity", avmFailed);
            }
            a_out(std::format(
                "donorvisual: validated={} failed={} headPartFailed={} colorFailed={} avmFailed={}",
                checked, failed, headPartFailed, colorFailed, avmFailed));
            return failed == 0;
        }

        [[nodiscard]] bool ValidateDonorMorphPopulation(
            const LineSink& a_out,
            RE::TESNPC* a_donor,
            const AppearancePreset& a_preset)
        {
            std::size_t checked = 0;
            std::size_t failed = 0;
            std::size_t shapeFailed = 0;
            std::size_t boneIDFailed = 0;
            std::size_t boneGroupFailed = 0;
            const auto check = [&](const bool a_condition, const std::string& a_label) {
                ++checked;
                if (!a_condition) {
                    ++failed;
                    if (failed <= 10) {
                        a_out(std::format("donormorph mismatch: {}", a_label));
                    }
                }
            };
            check(a_donor->morphWeight.thin == static_cast<float>(a_preset.morphWeights.x),
                  "MorphWeight.x/thin");
            check(a_donor->morphWeight.muscular == static_cast<float>(a_preset.morphWeights.y),
                  "MorphWeight.y/muscular");
            check(a_donor->morphWeight.fat == static_cast<float>(a_preset.morphWeights.z),
                  "MorphWeight.z/fat");

            check(a_donor->unk3D8 &&
                      a_donor->unk3D8->size() == a_preset.bodyMorphRegionValues.size(),
                  "BodyMorphRegionValuesA size");
            if (a_donor->unk3D8 &&
                a_donor->unk3D8->size() == a_preset.bodyMorphRegionValues.size()) {
                for (std::size_t i = 0; i < a_preset.bodyMorphRegionValues.size(); ++i) {
                    check((*a_donor->unk3D8)[static_cast<std::uint32_t>(i)] ==
                              static_cast<float>(a_preset.bodyMorphRegionValues[i]),
                          std::format("BodyMorphRegionValuesA[{}]", i));
                }
            }

            for (const auto& morph : a_preset.facialMorphSliders) {
                float actual = 0.0F;
                bool found = false;
                if (a_donor->shapeBlendData) {
                    for (const auto& entry : *a_donor->shapeBlendData) {
                        if (::_stricmp(SafeText(entry.key.c_str()), morph.name.c_str()) == 0) {
                            actual = entry.value;
                            found = true;
                            break;
                        }
                    }
                }
                const auto expected = static_cast<float>(morph.value);
                const bool valid =
                    (expected == 0.0F && (!found || actual == 0.0F)) ||
                    (found && actual == expected);
                shapeFailed += !valid;
                check(valid,
                      std::format("FacialMorphSliderDataA['{}'] expected={:.9g} actual={:.9g} found={}",
                                  morph.name, expected, actual, found));
            }

            for (const auto& region : a_preset.facialBoneRegions) {
                for (const auto& slider : region.sliders) {
                    float actual = 0.0F;
                    bool found = false;
                    if (slider.id != 0 && a_donor->unk3E0) {
                        const auto it = a_donor->unk3E0->find(slider.id);
                        if (it != a_donor->unk3E0->end()) {
                            actual = it->value;
                            found = true;
                        }
                    } else if (slider.id == 0 && !slider.groupName.empty() &&
                               a_donor->unk3E8) {
                        const auto outer = a_donor->unk3E8->find(region.regionID);
                        if (outer != a_donor->unk3E8->end() && outer->value) {
                            for (const auto& entry : *outer->value) {
                                if (::_stricmp(SafeText(entry.key.c_str()),
                                               slider.groupName.c_str()) == 0) {
                                    actual = entry.value;
                                    found = true;
                                    break;
                                }
                            }
                        }
                    }
                    const auto expected = static_cast<float>(slider.value);
                    const bool valid =
                        (expected == 0.0F && (!found || actual == 0.0F)) ||
                        (found && actual == expected);
                    if (!valid) {
                        slider.id != 0 ? ++boneIDFailed : ++boneGroupFailed;
                    }
                    check(valid,
                          std::format("FacialBoneRegionDataA[{}].{} expected={:.9g} actual={:.9g} found={}",
                                      region.regionID,
                                      slider.id != 0 ? std::to_string(slider.id) : slider.groupName,
                                      expected, actual, found));
                }
            }
            a_out(std::format(
                "donormorph: validated={} failed={} shapeFailed={} boneIDFailed={} boneGroupFailed={}",
                checked, failed, shapeFailed, boneIDFailed, boneGroupFailed));
            if (shapeFailed != 0 && a_donor->shapeBlendData) {
                std::size_t emitted = 0;
                for (const auto& entry : *a_donor->shapeBlendData) {
                    if (emitted++ >= 16) {
                        break;
                    }
                    a_out(std::format("donormorph shape sample: '{}'={:.9g}",
                                      SafeText(entry.key.c_str()), entry.value));
                }
            }
            return failed == 0;
        }

        struct NpcDonorDeleter
        {
            DestroyNpc destroy{ nullptr };

            void operator()(RE::TESNPC* a_donor) const noexcept
            {
                if (a_donor && destroy) {
                    static_cast<void>(destroy(a_donor, 1));
                }
            }
        };

        using ScopedNpcDonor = std::unique_ptr<RE::TESNPC, NpcDonorDeleter>;

        struct SnapshotDonorFunctions
        {
            std::uintptr_t          factoryAddress{ 0 };
            std::uintptr_t          npcVtable{ 0 };
            CreateNpc               create{ nullptr };
            DestroyNpc              destroy{ nullptr };
            SetShapeBlend           setShape{ nullptr };
            SetBodyMorph            setBody{ nullptr };
            SetFacialBone           setBone{ nullptr };
            EnsureFacialBoneGroup   ensureBoneGroup{ nullptr };
            ChangeHeadPart          changeHeadPart{ nullptr };
            SetAvmData              setAvmData{ nullptr };
            OwnedVisualCopy         ownedCopy{ nullptr };
        };

        struct BuiltSnapshotDonor
        {
            ScopedNpcDonor donor;
            RE::TESFormID  formID{ 0 };
        };

        [[nodiscard]] std::optional<SnapshotDonorFunctions>
        ResolveSnapshotDonorFunctions(const LineSink& a_out)
        {
            const auto factoryAddress =
                REL::Relocation<std::uintptr_t>{ kNpcFactorySingletonID }.address();
            const auto factoryVtable =
                REL::Relocation<std::uintptr_t>{ kNpcFactoryVtableID }.address();
            const auto createAddress =
                REL::Relocation<std::uintptr_t>{ kNpcFactoryCreateID }.address();
            const auto npcVtable =
                REL::Relocation<std::uintptr_t>{ kNpcPrimaryVtableID }.address();
            const auto destructorAddress =
                REL::Relocation<std::uintptr_t>{ kNpcScalarDeletingDestructorID }.address();
            const auto shapeAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetShapeBlendID }.address();
            const auto bodyAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetBodyMorphID }.address();
            const auto boneAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetBoneValueID }.address();
            const auto boneGroupAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetBoneGroupValueID }.address();
            const auto changeHeadAddress =
                REL::Relocation<std::uintptr_t>{ kNpcChangeHeadPartID }.address();
            const auto setAvmAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetAvmDataID }.address();
            const auto ownedCopyAddress = kNpcOwnedVisualCopyID.address();

            if (!Util::IsReadableRange(factoryAddress, sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryAddress) != factoryVtable ||
                !Util::IsReadableRange(
                    factoryVtable + sizeof(std::uintptr_t), sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(
                    factoryVtable + sizeof(std::uintptr_t)) != createAddress ||
                !HasExpectedBytes(createAddress, kNpcFactoryCreateGate) ||
                !HasExpectedBytes(destructorAddress, kNpcDestructorGate) ||
                !HasExpectedBytes(shapeAddress, kNpcSetShapeBlendGate) ||
                !HasExpectedBytes(bodyAddress, kNpcSetBodyMorphGate) ||
                !HasExpectedBytes(boneAddress, kNpcSetBoneValueGate) ||
                !HasExpectedBytes(boneGroupAddress, kNpcSetBoneGroupValueGate) ||
                !HasExpectedBytes(changeHeadAddress, kNpcChangeHeadPartGate) ||
                !HasExpectedBytes(setAvmAddress, kNpcSetAvmDataGate) ||
                !HasExpectedBytes(ownedCopyAddress, kNpcOwnedVisualCopyGate)) {
                KillMutation("owned snapshot restore byte gate failed");
                a_out("ownedrestore: donor/setter/copy/destructor contract mismatch; FAIL CLOSED");
                return std::nullopt;
            }

            return SnapshotDonorFunctions{
                .factoryAddress = factoryAddress,
                .npcVtable = npcVtable,
                .create = reinterpret_cast<CreateNpc>(createAddress),
                .destroy = reinterpret_cast<DestroyNpc>(destructorAddress),
                .setShape = reinterpret_cast<SetShapeBlend>(shapeAddress),
                .setBody = reinterpret_cast<SetBodyMorph>(bodyAddress),
                .setBone = reinterpret_cast<SetFacialBone>(boneAddress),
                .ensureBoneGroup =
                    reinterpret_cast<EnsureFacialBoneGroup>(boneGroupAddress),
                .changeHeadPart = reinterpret_cast<ChangeHeadPart>(changeHeadAddress),
                .setAvmData = reinterpret_cast<SetAvmData>(setAvmAddress),
                .ownedCopy = reinterpret_cast<OwnedVisualCopy>(ownedCopyAddress),
            };
        }

        [[nodiscard]] std::optional<BuiltSnapshotDonor> BuildOwnedVisualSnapshotDonor(
            const LineSink& a_out,
            const OwnedVisualSnapshot& a_snapshot,
            const SnapshotDonorFunctions& a_functions)
        {
            auto* donor = a_functions.create(
                reinterpret_cast<void*>(a_functions.factoryAddress), false);
            if (!donor) {
                a_out("ownedrestore: Create(false) returned null; no target mutation");
                return std::nullopt;
            }

            const auto donorFormID = donor->GetFormID();
            ScopedNpcDonor donorOwner{ donor, NpcDonorDeleter{ a_functions.destroy } };
            std::size_t headPartCount = 0;
            {
                auto headParts = donor->headParts.Lock();
                headPartCount = (*headParts).size();
            }
            const bool initialized =
                *reinterpret_cast<const std::uintptr_t*>(donor) == a_functions.npcVtable &&
                donorFormID != 0 &&
                RE::TESForm::LookupByID<RE::TESNPC>(donorFormID) == donor &&
                donor->QRefCount() == 0 && donor->GetRace() == nullptr &&
                donor->faceNPC == nullptr && headPartCount == 0 &&
                donor->unk3D8 == nullptr && donor->unk3E0 == nullptr &&
                donor->unk3E8 == nullptr && donor->tintAVMData.empty() &&
                donor->shapeBlendData == nullptr && donor->pronoun.underlying() == 0;
            if (!initialized) {
                a_out("ownedrestore: donor failed registered-empty invariants; no target mutation");
                return std::nullopt;
            }

            donor->morphWeight.thin = a_snapshot.thin;
            donor->morphWeight.muscular = a_snapshot.muscular;
            donor->morphWeight.fat = a_snapshot.fat;
            if (a_snapshot.hasBodyMorphRegions) {
                for (std::size_t i = 0; i < a_snapshot.bodyMorphRegions.size(); ++i) {
                    a_functions.setBody(
                        donor, static_cast<std::uint32_t>(i),
                        a_snapshot.bodyMorphRegions[i]);
                }
            }
            if (a_snapshot.hasBoneValues) {
                for (const auto& [key, value] : a_snapshot.boneValues) {
                    a_functions.setBone(donor, key, value);
                }
            }
            if (a_snapshot.hasBoneRegions) {
                for (const auto& region : a_snapshot.boneRegions) {
                    if (!region.hasValues) {
                        continue;
                    }
                    for (const auto& [key, value] : region.values) {
                        const RE::BSFixedStringCS fixedKey{ key.c_str() };
                        a_functions.ensureBoneGroup(donor, region.regionID, &fixedKey);
                        if (!donor->unk3E8) {
                            continue;
                        }
                        const auto outer = donor->unk3E8->find(region.regionID);
                        if (outer == donor->unk3E8->end() || !outer->value) {
                            continue;
                        }
                        for (auto& entry : *outer->value) {
                            if (std::string_view{ SafeText(entry.key.c_str()) } == key) {
                                entry.value = value;
                                break;
                            }
                        }
                    }
                }
            }
            if (a_snapshot.hasShapeBlends) {
                for (const auto& [key, value] : a_snapshot.shapeBlends) {
                    const RE::BSFixedStringCS fixedKey{ key.c_str() };
                    a_functions.setShape(donor, &fixedKey, value);
                }
            }
            for (const auto formID : a_snapshot.headPartFormIDs) {
                auto* part = RE::TESForm::LookupByID<RE::BGSHeadPart>(formID);
                if (!part) {
                    a_out(std::format(
                        "ownedrestore: headpart 0x{:08X} did not resolve; no target mutation",
                        formID));
                    return std::nullopt;
                }
                a_functions.changeHeadPart(donor, part);
            }

            donor->skinToneIndex = a_snapshot.skinToneIndex;
            donor->pronoun = static_cast<RE::TESNPC::PRONOUN_TYPE>(a_snapshot.pronoun);
            donor->teeth = a_snapshot.teeth;
            donor->jewelryColor = a_snapshot.jewelryColor;
            donor->eyeColor = a_snapshot.eyeColor;
            donor->hairColor = a_snapshot.hairColor;
            donor->facialColor = a_snapshot.facialColor;
            donor->eyebrowColor = a_snapshot.eyebrowColor;
            for (const auto& avm : a_snapshot.avms) {
                RE::AVMData materialized{};
                materialized.type = avm.type;
                materialized.category = RE::BSFixedString{ avm.category.c_str() };
                materialized.unk10.name = RE::BSFixedString{ avm.name.c_str() };
                materialized.unk10.texturePath =
                    RE::BSFixedString{ avm.texturePath.c_str() };
                materialized.unk10.color = avm.color;
                materialized.unk10.intensity = avm.intensity;
                a_functions.setAvmData(donor, &materialized);
            }

            if (!SameExactVisualValues(donor, a_snapshot)) {
                a_out("ownedrestore: constructed donor did not exactly match snapshot; no target mutation");
                return std::nullopt;
            }
            return BuiltSnapshotDonor{
                .donor = std::move(donorOwner),
                .formID = donorFormID,
            };
        }
    }

    namespace Detail
    {
        bool RestoreOwnedVisualSnapshot(
            const LineSink& a_out,
            RE::TESNPC* a_target,
            const OwnedVisualSnapshot& a_snapshot,
            RE::TESNPC* a_originalFaceNPC)
        {
            if (!RequireRestoreOperational(a_out, "ownedrestore") || !a_target) {
                if (!a_target) {
                    a_out("ownedrestore: target is null; no mutation");
                }
                return false;
            }

            const auto functions = ResolveSnapshotDonorFunctions(a_out);
            if (!functions) {
                return false;
            }
            auto built = BuildOwnedVisualSnapshotDonor(a_out, a_snapshot, *functions);
            if (!built) {
                return false;
            }

            a_target->morphWeight.thin = a_snapshot.thin;
            a_target->morphWeight.muscular = a_snapshot.muscular;
            a_target->morphWeight.fat = a_snapshot.fat;
            if (a_snapshot.hasBodyMorphRegions) {
                for (std::size_t i = 0; i < a_snapshot.bodyMorphRegions.size(); ++i) {
                    functions->setBody(
                        a_target, static_cast<std::uint32_t>(i),
                        a_snapshot.bodyMorphRegions[i]);
                }
            }
            a_target->skinToneIndex = a_snapshot.skinToneIndex;
            functions->ownedCopy(a_target, built->donor.get(), false);
            a_target->faceNPC = a_originalFaceNPC;
            const bool targetExact = SameExactVisualValues(a_target, a_snapshot) &&
                a_target->faceNPC == a_originalFaceNPC;

            const auto donorFormID = built->formID;
            built->donor.reset();
            const bool donorUnregistered =
                RE::TESForm::LookupByID<RE::TESNPC>(donorFormID) == nullptr;
            a_out(std::format(
                "ownedrestore: donorExact=true targetExact={} donorUnregistered={}",
                targetExact, donorUnregistered));
            return targetExact && donorUnregistered;
        }

        bool SilentApplyPresetToBase(
            const LineSink& a_out,
            RE::TESNPC* a_target,
            const std::filesystem::path& a_path)
        {
            if (!RequireMutationOperational(a_out, "silentapply") || !a_target) {
                if (!a_target) {
                    a_out("silentapply: target is null; no mutation");
                }
                return false;
            }

            const auto decoded = LoadCkPreset(a_path);
            if (!decoded.preset) {
                a_out(std::format(
                    "silentapply: preset rejected path={} issues={}",
                    a_path.string(), decoded.issues.size()));
                return false;
            }
            const auto resolved = ResolveAppearanceDependencies(*decoded.preset, a_target);
            if (!resolved.Complete()) {
                a_out("silentapply: dependency resolution incomplete; no mutation");
                return false;
            }

            const auto factoryAddress =
                REL::Relocation<std::uintptr_t>{ kNpcFactorySingletonID }.address();
            const auto factoryVtable =
                REL::Relocation<std::uintptr_t>{ kNpcFactoryVtableID }.address();
            const auto createAddress =
                REL::Relocation<std::uintptr_t>{ kNpcFactoryCreateID }.address();
            const auto npcVtable =
                REL::Relocation<std::uintptr_t>{ kNpcPrimaryVtableID }.address();
            const auto destructorAddress =
                REL::Relocation<std::uintptr_t>{ kNpcScalarDeletingDestructorID }.address();
            const auto copyAddress =
                REL::Relocation<std::uintptr_t>{ kNpcCopyAppearanceID }.address();
            const auto shapeAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetShapeBlendID }.address();
            const auto bodyAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetBodyMorphID }.address();
            const auto boneAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetBoneValueID }.address();
            const auto boneGroupAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetBoneGroupValueID }.address();
            const auto removeHeadAddress =
                REL::Relocation<std::uintptr_t>{ kNpcRemoveHeadPartID }.address();
            const auto changeHeadAddress =
                REL::Relocation<std::uintptr_t>{ kNpcChangeHeadPartID }.address();
            const auto resolveEntryAddress =
                REL::Relocation<std::uintptr_t>{ kFaceDbResolveEntryID }.address();
            const auto setAvmAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetAvmDataID }.address();
            const auto removeAvmAddress =
                REL::Relocation<std::uintptr_t>{ kNpcRemoveAvmDataID }.address();
            const auto ownedCopyAddress = kNpcOwnedVisualCopyID.address();

            if (!Util::IsReadableRange(factoryAddress, sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryAddress) != factoryVtable ||
                !Util::IsReadableRange(
                    factoryVtable + sizeof(std::uintptr_t), sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(
                    factoryVtable + sizeof(std::uintptr_t)) != createAddress ||
                !HasExpectedBytes(createAddress, kNpcFactoryCreateGate) ||
                !HasExpectedBytes(destructorAddress, kNpcDestructorGate) ||
                !HasExpectedBytes(copyAddress, kNpcCopyAppearanceGate) ||
                !HasExpectedBytes(shapeAddress, kNpcSetShapeBlendGate) ||
                !HasExpectedBytes(bodyAddress, kNpcSetBodyMorphGate) ||
                !HasExpectedBytes(boneAddress, kNpcSetBoneValueGate) ||
                !HasExpectedBytes(boneGroupAddress, kNpcSetBoneGroupValueGate) ||
                !HasExpectedBytes(removeHeadAddress, kNpcRemoveHeadPartGate) ||
                !HasExpectedBytes(changeHeadAddress, kNpcChangeHeadPartGate) ||
                !HasExpectedBytes(resolveEntryAddress, kFaceDbResolveEntryGate) ||
                !HasExpectedBytes(setAvmAddress, kNpcSetAvmDataGate) ||
                !HasExpectedBytes(removeAvmAddress, kNpcRemoveAvmDataGate) ||
                !HasExpectedBytes(ownedCopyAddress, kNpcOwnedVisualCopyGate)) {
                KillMutation("silent preset apply byte gate failed");
                a_out("silentapply: population/copy/destructor contract mismatch; FAIL CLOSED");
                return false;
            }

            const auto resolveEntry =
                reinterpret_cast<ResolveFaceDbEntry>(resolveEntryAddress);
            std::vector<MaterializedAvmLayer> expectedAvms;
            if (!MaterializeAvmLayers(
                    a_out, *decoded.preset, resolveEntry, expectedAvms)) {
                a_out("silentapply: AVM materialization incomplete; no mutation");
                return false;
            }

            const auto create = reinterpret_cast<CreateNpc>(createAddress);
            const auto copy = reinterpret_cast<CopyNpcAppearance>(copyAddress);
            const auto ownedCopy = reinterpret_cast<OwnedVisualCopy>(ownedCopyAddress);
            const auto setShape = reinterpret_cast<SetShapeBlend>(shapeAddress);
            const auto setBody = reinterpret_cast<SetBodyMorph>(bodyAddress);
            const auto setBone = reinterpret_cast<SetFacialBone>(boneAddress);
            const auto ensureBoneGroup =
                reinterpret_cast<EnsureFacialBoneGroup>(boneGroupAddress);
            const auto removeHeadPart =
                reinterpret_cast<RemoveHeadPart>(removeHeadAddress);
            const auto changeHeadPart =
                reinterpret_cast<ChangeHeadPart>(changeHeadAddress);
            const auto setAvmData = reinterpret_cast<SetAvmData>(setAvmAddress);
            const auto removeAvmData =
                reinterpret_cast<RemoveAvmData>(removeAvmAddress);
            const auto destroy = reinterpret_cast<DestroyNpc>(destructorAddress);

            const auto targetNonVisualBefore = Snapshot(a_target);
            const auto targetVisualBefore = SnapshotVisualSeed(a_target);
            const auto originalActorFlags =
                a_target->actorData.actorBaseFlags.underlying();
            auto* const originalFaceNPC = a_target->faceNPC;

            ScopedNpcDonor backupDonor{
                create(reinterpret_cast<void*>(factoryAddress), false),
                NpcDonorDeleter{ destroy }
            };
            ScopedNpcDonor presetDonor{
                create(reinterpret_cast<void*>(factoryAddress), false),
                NpcDonorDeleter{ destroy }
            };
            if (!backupDonor || !presetDonor) {
                a_out("silentapply: failed to create the donor pair; no mutation");
                return false;
            }
            const auto backupFormID = backupDonor->GetFormID();
            const auto presetFormID = presetDonor->GetFormID();
            const auto initialized = [&](RE::TESNPC* a_donor, RE::TESFormID a_formID) {
                return *reinterpret_cast<const std::uintptr_t*>(a_donor) == npcVtable &&
                    a_formID != 0 &&
                    RE::TESForm::LookupByID<RE::TESNPC>(a_formID) == a_donor &&
                    a_donor->QRefCount() == 0 && a_donor->unk3D8 == nullptr &&
                    a_donor->unk3E0 == nullptr && a_donor->unk3E8 == nullptr &&
                    a_donor->shapeBlendData == nullptr && a_donor->tintAVMData.empty();
            };
            if (!initialized(backupDonor.get(), backupFormID) ||
                !initialized(presetDonor.get(), presetFormID)) {
                a_out("silentapply: donor pair failed registered-empty invariants; no mutation");
                return false;
            }

            copy(backupDonor.get(), a_target, false);
            copy(presetDonor.get(), a_target, false);
            PopulatePresetMorphs(
                presetDonor.get(), *decoded.preset, setShape, setBody,
                setBone, ensureBoneGroup);
            PopulatePresetVisuals(
                presetDonor.get(), *decoded.preset, resolved, expectedAvms,
                removeHeadPart, changeHeadPart, setAvmData, removeAvmData);

            const bool backupExact =
                SameExactVisualValues(backupDonor.get(), a_target);
            const bool backupIndependent = HasIndependentVisualStorage(
                targetVisualBefore, SnapshotVisualSeed(backupDonor.get()));
            const bool presetMorphsValid = ValidateDonorMorphPopulation(
                a_out, presetDonor.get(), *decoded.preset);
            const bool presetVisualsValid = ValidateDonorVisualPopulation(
                a_out, presetDonor.get(), *decoded.preset, resolved, expectedAvms);
            const bool bodyCompatible = backupDonor->unk3D8 &&
                backupDonor->unk3D8->size() ==
                    decoded.preset->bodyMorphRegionValues.size();
            if (!backupExact || !backupIndependent || !presetMorphsValid ||
                !presetVisualsValid || !bodyCompatible ||
                targetNonVisualBefore != Snapshot(a_target) ||
                targetVisualBefore != SnapshotVisualSeed(a_target)) {
                a_out(std::format(
                    "silentapply: preflight failed backupExact={} backupIndependent={} presetMorphsValid={} presetVisualsValid={} bodyCompatible={}; no mutation",
                    backupExact, backupIndependent, presetMorphsValid,
                    presetVisualsValid, bodyCompatible));
                return false;
            }

            presetDonor->faceNPC = nullptr;
            a_target->morphWeight.thin =
                static_cast<float>(decoded.preset->morphWeights.x);
            a_target->morphWeight.muscular =
                static_cast<float>(decoded.preset->morphWeights.y);
            a_target->morphWeight.fat =
                static_cast<float>(decoded.preset->morphWeights.z);
            for (std::size_t i = 0;
                 i < decoded.preset->bodyMorphRegionValues.size(); ++i) {
                setBody(
                    a_target, static_cast<std::uint32_t>(i),
                    static_cast<float>(decoded.preset->bodyMorphRegionValues[i]));
            }
            a_target->skinToneIndex =
                static_cast<std::uint8_t>(decoded.preset->skinTone);
            ownedCopy(a_target, presetDonor.get(), false);

            const bool targetMorphsValid = ValidateDonorMorphPopulation(
                a_out, a_target, *decoded.preset);
            const bool targetVisualsValid = ValidateDonorVisualPopulation(
                a_out, a_target, *decoded.preset, resolved, expectedAvms);
            const bool targetMatchesPreset =
                SameExactVisualValues(a_target, presetDonor.get());
            const bool targetStorageIndependent = HasIndependentVisualStorage(
                SnapshotVisualSeed(presetDonor.get()), SnapshotVisualSeed(a_target));
            const bool targetNonVisualPreserved =
                targetNonVisualBefore == Snapshot(a_target);
            const bool applied = targetMorphsValid && targetVisualsValid &&
                targetMatchesPreset && targetStorageIndependent &&
                targetNonVisualPreserved && a_target->faceNPC == nullptr;

            if (!applied) {
                a_target->morphWeight = backupDonor->morphWeight;
                for (std::uint32_t i = 0; i < backupDonor->unk3D8->size(); ++i) {
                    setBody(a_target, i, (*backupDonor->unk3D8)[i]);
                }
                a_target->skinToneIndex = backupDonor->skinToneIndex;
                ownedCopy(a_target, backupDonor.get(), false);
                a_target->faceNPC = originalFaceNPC;
                a_target->actorData.actorBaseFlags =
                    static_cast<RE::ACTOR_BASE_DATA::Flag>(originalActorFlags);
                const bool rolledBack =
                    SameExactVisualValues(a_target, backupDonor.get()) &&
                    a_target->faceNPC == originalFaceNPC &&
                    targetNonVisualBefore == Snapshot(a_target);
                a_out(std::format(
                    "silentapply: post-validate failed morphs={} visuals={} matchesPreset={} storageIndependent={} nonVisualPreserved={} faceNPCCleared={}; rolledBack={}",
                    targetMorphsValid, targetVisualsValid, targetMatchesPreset,
                    targetStorageIndependent, targetNonVisualPreserved,
                    a_target->faceNPC == nullptr, rolledBack));
                if (!rolledBack) {
                    KillMutation("silent preset apply rollback failed");
                }
            }

            presetDonor.reset();
            backupDonor.reset();
            const bool donorsUnregistered =
                RE::TESForm::LookupByID<RE::TESNPC>(presetFormID) == nullptr &&
                RE::TESForm::LookupByID<RE::TESNPC>(backupFormID) == nullptr;
            a_out(std::format(
                "silentapply: applied={} morphsValid={} visualsValid={} matchesPreset={} storageIndependent={} nonVisualPreserved={} donorsUnregistered={} faceNPCCleared={}",
                applied, targetMorphsValid, targetVisualsValid, targetMatchesPreset,
                targetStorageIndependent, targetNonVisualPreserved,
                donorsUnregistered, applied && a_target->faceNPC == nullptr));
            if (!donorsUnregistered) {
                KillMutation("silent preset donor teardown failed");
            }
            return applied && donorsUnregistered;
        }
    }
}
