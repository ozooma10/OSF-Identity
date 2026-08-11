#include "NpcAppearance/Resolver.h"

#include "pch.h"

#include "Util/StarfieldRuntime.h"

#include "RE/B/BGSListForm.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>
#include <unordered_set>
#include "Resolver.h"

namespace NpcAppearance
{
    namespace
    {
        // Starfield 1.16.244 contracts. These offsets are compiler-derived from
        // the current headers and independently visible in IDs 68105/68803/68808.
        static_assert(offsetof(RE::BGSHeadPart, validRaces) == 0x130);
        static_assert(offsetof(RE::BGSHeadPart, type) == 0x148);
        static_assert(offsetof(RE::TESRace, chargenData) == 0x990);
        static_assert(offsetof(RE::TESRace, headparts) == 0x9D8);

        constexpr REL::ID kRaceGetBoneGroupNameID{ 68803 };
        constexpr REL::ID kRaceFindShapeDescriptorByNameID{ 68807 };
        constexpr REL::ID kRaceGetBoneRegionDescriptorID{ 68808 };
        constexpr REL::ID kFaceDbGetLayerValuesID{ 97404 };
        constexpr REL::ID kFaceDbHasModulationEntriesID{ 97406 };
        constexpr REL::ID kFaceDbMapLookupID{ 37347 };
        constexpr REL::ID kFaceDbMapEntriesID{ 884864 };
        constexpr REL::ID kFaceDbFindSimpleColorIndexID{ 37341 };
        constexpr REL::ID kFaceDbFindMappedColorIndexID{ 69610 };
        constexpr REL::ID kHairColorCategoryID{ 886309 };
        constexpr REL::ID kFacialHairColorCategoryID{ 886310 };
        constexpr REL::ID kTeethCategoryID{ 886313 };
        constexpr REL::ID kJewelryColorCategoryID{ 886314 };
        constexpr REL::ID kEyeColorCategoryID{ 886315 };

        constexpr std::array<std::uint8_t, 11> kRaceGetBoneGroupNameGate{
            0x48, 0x63, 0xC2,
            0x48, 0x8B, 0x84, 0xC1, 0x90, 0x09, 0x00, 0x00
        };
        constexpr std::array<std::uint8_t, 11> kRaceGetBoneRegionDescriptorGate{
            0x48, 0x89, 0x5C, 0x24, 0x08,
            0x57,
            0x48, 0x83, 0xEC, 0x20,
            0x48
        };
        constexpr std::array<std::uint8_t, 12> kRaceFindShapeDescriptorByNameGate{
            0x40, 0x56,
            0x41, 0x57,
            0x48, 0x83, 0xEC, 0x28,
            0x48, 0x63, 0xC2,
            0x4D
        };
        constexpr std::array<std::uint8_t, 12> kFaceDbGetLayerValuesGate{
            0x48, 0x89, 0x5C, 0x24, 0x08,
            0x48, 0x89, 0x74, 0x24, 0x18,
            0x48, 0x89
        };
        constexpr std::array<std::uint8_t, 12> kFaceDbHasModulationEntriesGate{
            0x48, 0x89, 0x5C, 0x24, 0x08,
            0x55, 0x56, 0x57,
            0x48, 0x83, 0xEC, 0x30
        };
        constexpr std::array<std::uint8_t, 12> kFaceDbMapLookupGate{
            0x48, 0x89, 0x5C, 0x24, 0x08,
            0x48, 0x89, 0x6C, 0x24, 0x10,
            0x48, 0x89
        };
        constexpr std::array<std::uint8_t, 12> kFaceDbFindSimpleColorIndexGate{
            0x48, 0x89, 0x5C, 0x24, 0x08,
            0x48, 0x89, 0x74, 0x24, 0x10,
            0x57, 0x48
        };
        constexpr std::array<std::uint8_t, 12> kFaceDbFindMappedColorIndexGate{
            0x48, 0x89, 0x5C, 0x24, 0x08,
            0x57,
            0x48, 0x83, 0xEC, 0x20,
            0x49, 0x8B
        };

        // ID 97406 performs the same category -> BGSAVMData lookup inline.
        // Its two RIP-relative loads prove that ID 884864 is the table field
        // at map+0x18 and that the bucket-count field follows at map+0x20.
        constexpr std::ptrdiff_t kFaceDbMapOffsetFromEntries = -0x18;
        constexpr std::ptrdiff_t kHasEntriesBucketCountLoadOffset = 0x60;
        constexpr std::ptrdiff_t kHasEntriesTableLoadOffset = 0xAC;
        constexpr std::size_t kMaxAvmCatalogEntries = 1024;

        static_assert(sizeof(RE::BSScrapArray<RE::BSFixedString>) == 0x18);
        static_assert(sizeof(RE::AVMData::Entry) == 0x18);

        template <std::size_t N>
        [[nodiscard]] bool HasExpectedBytes(
            const std::uintptr_t a_address,
            const std::array<std::uint8_t, N>& a_expected)
        {
            return Util::IsReadableRange(a_address, a_expected.size()) &&
                   std::memcmp(reinterpret_cast<const void*>(a_address),
                               a_expected.data(), a_expected.size()) == 0;
        }

        void AddIssue(
            ResolvedAppearanceDependencies& a_result,
            std::string a_field,
            std::string a_value,
            std::string a_code,
            std::string a_message)
        {
            a_result.issues.push_back(DependencyIssue{
                std::move(a_field), std::move(a_value), std::move(a_code), std::move(a_message) });
        }

        [[nodiscard]] bool EqualEditorID(std::string_view a_lhs, const char* a_rhs)
        {
            if (!a_rhs || a_lhs.size() != std::strlen(a_rhs)) {
                return false;
            }
            return ::_strnicmp(a_lhs.data(), a_rhs, a_lhs.size()) == 0;
        }

        [[nodiscard]] std::uintptr_t ResolveRipRelativeTarget(
            const std::uintptr_t a_instruction,
            const std::size_t a_displacementOffset,
            const std::size_t a_instructionSize)
        {
            if (!Util::IsReadableRange(
                    a_instruction + a_displacementOffset, sizeof(std::int32_t))) {
                return 0;
            }
            std::int32_t displacement = 0;
            std::memcpy(&displacement,
                        reinterpret_cast<const void*>(a_instruction + a_displacementOffset),
                        sizeof(displacement));
            return a_instruction + a_instructionSize + displacement;
        }

        [[nodiscard]] bool RaceOffersHeadPart(
            const RE::TESRace* a_race,
            const RE::SEX a_sex,
            const RE::BGSHeadPart* a_part)
        {
            if (!a_race || !a_part || (a_sex != RE::SEX::kMale && a_sex != RE::SEX::kFemale)) {
                return false;
            }
            if (!a_part->validRaces) {
                return true;
            }
            const auto& validRaces = a_part->validRaces->arrayOfForms;
            return std::ranges::find(validRaces, static_cast<const RE::TESForm*>(a_race)) !=
                   validRaces.end();
        }

        [[nodiscard]] RE::BGSHeadPart* ResolveHeadPart(
            ResolvedAppearanceDependencies& a_result,
            const RE::TESRace* a_race,
            const RE::SEX a_sex,
            std::string_view a_field,
            std::string_view a_editorID,
            const std::optional<RE::BGSHeadPart::HeadPartType> a_expectedType,
            std::unordered_set<const RE::BGSHeadPart*>& a_seen)
        {
            auto* part = RE::TESForm::LookupByEditorID<RE::BGSHeadPart>(
                RE::BSFixedString{ a_editorID });
            if (!part) {
                AddIssue(a_result, std::string{ a_field }, std::string{ a_editorID },
                         "headpart_not_found", "EditorID did not resolve to BGSHeadPart");
                return nullptr;
            }
            if (!a_seen.insert(part).second) {
                AddIssue(a_result, std::string{ a_field }, std::string{ a_editorID },
                         "duplicate_headpart", "the same head-part form occurs more than once");
                return nullptr;
            }
            if (!RaceOffersHeadPart(a_race, a_sex, part)) {
                AddIssue(a_result, std::string{ a_field }, std::string{ a_editorID },
                         "headpart_not_valid_for_race",
                         "head-part's valid-races form list excludes the resolved race");
                return nullptr;
            }
            if (a_expectedType && part->type.get() != *a_expectedType) {
                AddIssue(a_result, std::string{ a_field }, std::string{ a_editorID },
                         "headpart_type_mismatch",
                         std::format("resolved type={} but CK positional slot requires type={}",
                                     part->type.underlying(),
                                     static_cast<std::uint32_t>(*a_expectedType)));
                return nullptr;
            }
            return part;
        }

        void ResolveHeadParts(
            ResolvedAppearanceDependencies& a_result,
            const AppearancePreset& a_preset,
            const RE::SEX a_sex)
        {
            constexpr auto kCkUniqueSlots =
                static_cast<std::size_t>(RE::BGSHeadPart::HeadPartType::kEyelashes) + 1;
            if (a_preset.uniqueHeadParts.size() != kCkUniqueSlots) {
                AddIssue(a_result, "UniqueHeadPartsA", std::to_string(a_preset.uniqueHeadParts.size()),
                         "headpart_slot_count",
                         std::format("CK 1.16.244 contract requires {} positional slots",
                                     kCkUniqueSlots));
            }

            std::unordered_set<const RE::BGSHeadPart*> seen;
            a_result.uniqueHeadParts.resize(a_preset.uniqueHeadParts.size(), nullptr);
            for (std::size_t i = 0; i < a_preset.uniqueHeadParts.size(); ++i) {
                const auto& editorID = a_preset.uniqueHeadParts[i];
                if (editorID.empty()) {
                    continue;
                }
                if (i >= kCkUniqueSlots) {
                    AddIssue(a_result, std::format("UniqueHeadPartsA[{}]", i), editorID,
                             "headpart_slot_out_of_range",
                             "no proven CK positional head-part type exists for this slot");
                    continue;
                }
                const auto expected = static_cast<RE::BGSHeadPart::HeadPartType>(i);
                a_result.uniqueHeadParts[i] = ResolveHeadPart(
                    a_result, a_result.race, a_sex,
                    std::format("UniqueHeadPartsA[{}]", i), editorID, expected, seen);
            }

            a_result.miscHeadParts.reserve(a_preset.miscHeadParts.size());
            for (std::size_t i = 0; i < a_preset.miscHeadParts.size(); ++i) {
                const auto& editorID = a_preset.miscHeadParts[i];
                auto* part = ResolveHeadPart(
                    a_result, a_result.race, a_sex,
                    std::format("MiscHeadPartsA[{}]", i), editorID, std::nullopt, seen);
                if (part) {
                    a_result.miscHeadParts.push_back(part);
                }
            }
        }

        void ResolveBoneReferences(
            ResolvedAppearanceDependencies& a_result,
            const AppearancePreset& a_preset,
            const RE::SEX a_sex)
        {
            using GetGroupName = const RE::BSFixedStringCS* (*)(
                RE::TESRace*, std::int32_t, std::uint32_t);
            using GetRegionDescriptor = void* (*)(
                RE::TESRace*, std::int32_t, std::uint32_t);
            using FindShapeDescriptorByName = void* (*)(
                RE::TESRace*, std::int32_t, const char*);

            REL::Relocation<GetGroupName> getGroupName{ kRaceGetBoneGroupNameID };
            REL::Relocation<FindShapeDescriptorByName> findShape{
                kRaceFindShapeDescriptorByNameID };
            REL::Relocation<GetRegionDescriptor> getRegion{ kRaceGetBoneRegionDescriptorID };
            if (!HasExpectedBytes(getGroupName.address(), kRaceGetBoneGroupNameGate) ||
                !HasExpectedBytes(getRegion.address(), kRaceGetBoneRegionDescriptorGate) ||
                !HasExpectedBytes(findShape.address(), kRaceFindShapeDescriptorByNameGate)) {
                AddIssue(a_result, "FacialBoneRegionDataA", {}, "chargen_accessor_contract_mismatch",
                         "Address Library IDs 68803/68807/68808 do not match the 1.16.244 byte contract");
                return;
            }

            std::unordered_set<std::string> groupNames;
            constexpr std::uint32_t kMaxCatalogNames = 128;
            for (std::uint32_t i = 0; i < kMaxCatalogNames; ++i) {
                const auto* name = getGroupName(a_result.race, static_cast<std::int32_t>(a_sex), i);
                if (!name) {
                    break;
                }
                groupNames.emplace(name->c_str());
            }

            bool shapeOK = true;
            for (const auto& morph : a_preset.facialMorphSliders) {
                const auto separator = morph.name.rfind('_');
                if (separator == std::string::npos || separator == 0 ||
                    separator + 1 >= morph.name.size()) {
                    shapeOK = false;
                    AddIssue(a_result, "FacialMorphSliderDataA.Name", morph.name,
                             "facial_shape_name_invalid",
                             "name is not a descriptor/group composite");
                    continue;
                }
                const auto descriptorName = morph.name.substr(0, separator);
                const auto groupName = morph.name.substr(separator + 1);
                if (!findShape(a_result.race, static_cast<std::int32_t>(a_sex),
                               descriptorName.c_str())) {
                    shapeOK = false;
                    AddIssue(a_result, "FacialMorphSliderDataA.Name", morph.name,
                             "facial_shape_descriptor_not_found",
                             std::format("race/sex descriptor '{}' was not found", descriptorName));
                    continue;
                }
                if (!groupNames.contains(groupName)) {
                    shapeOK = false;
                    AddIssue(a_result, "FacialMorphSliderDataA.Name", morph.name,
                             "facial_shape_group_not_found",
                             std::format("race/sex group '{}' was not found", groupName));
                    continue;
                }
            }
            a_result.shapeReferencesComplete = shapeOK;

            bool ok = true;
            std::unordered_set<std::uint32_t> checkedDescriptors;
            for (const auto& region : a_preset.facialBoneRegions) {
                // CK RegionID is hierarchy/grouping metadata. Live 1.16.244 proof
                // shows all Sarah RegionIDs are absent from the numeric descriptor
                // map while every nonzero child slider ID resolves. The preset
                // decoder validates these group IDs structurally; only child IDs
                // are engine descriptor references.
                for (const auto& slider : region.sliders) {
                    if (!slider.groupName.empty()) {
                        if (!groupNames.contains(slider.groupName)) {
                            ok = false;
                            AddIssue(a_result, "FacialBoneRegionDataA.SlidersA.GroupName",
                                     slider.groupName, "bone_group_not_found",
                                     "name is absent from the race/sex chargen group-name catalog");
                        }
                    }
                    if (slider.id != 0 && checkedDescriptors.insert(slider.id).second) {
                        if (!getRegion(a_result.race, static_cast<std::int32_t>(a_sex), slider.id)) {
                            ok = false;
                            AddIssue(a_result, "FacialBoneRegionDataA.SlidersA.ID",
                                     std::to_string(slider.id), "bone_slider_not_found",
                                     "ID is absent from the race/sex chargen descriptor map");
                        }
                    }
                }
            }
            a_result.boneReferencesComplete = ok;
        }

        [[nodiscard]] bool ValidateFaceDbMapContract(
            const std::uintptr_t a_hasEntriesAddress,
            const std::uintptr_t a_mapEntriesAddress)
        {
            const auto bucketLoad =
                a_hasEntriesAddress + kHasEntriesBucketCountLoadOffset;
            const auto tableLoad = a_hasEntriesAddress + kHasEntriesTableLoadOffset;
            constexpr std::array<std::uint8_t, 3> kBucketLoadGate{ 0x48, 0x8B, 0x35 };
            constexpr std::array<std::uint8_t, 3> kTableLoadGate{ 0x48, 0x8B, 0x0D };
            return HasExpectedBytes(bucketLoad, kBucketLoadGate) &&
                   HasExpectedBytes(tableLoad, kTableLoadGate) &&
                   ResolveRipRelativeTarget(bucketLoad, 3, 7) == a_mapEntriesAddress + 8 &&
                   ResolveRipRelativeTarget(tableLoad, 3, 7) == a_mapEntriesAddress;
        }

        [[nodiscard]] bool ResolveAvmModulation(
            ResolvedAppearanceDependencies& a_result,
            const PresetTintLayer& a_layer,
            const RE::BSFixedString& a_category,
            const std::uintptr_t a_mapAddress,
            const auto a_mapLookup)
        {
            if (a_layer.modulationValue.empty()) {
                return true;
            }

            void* rawData = nullptr;
            if (!a_mapLookup(reinterpret_cast<void*>(a_mapAddress), &a_category, &rawData) ||
                !rawData ||
                !Util::IsReadableRange(reinterpret_cast<std::uintptr_t>(rawData), 0x70)) {
                AddIssue(a_result, "PostBlendFaceCustomization.LayersA.ModulationValue",
                         a_layer.modulationValue, "avm_modulation_catalog_not_found",
                         std::format("layer '{}' has no readable FaceDB modulation catalog",
                                     a_layer.name));
                return false;
            }

            const auto* form = reinterpret_cast<const RE::TESForm*>(rawData);
            if (form->GetFormType() != RE::FormType::kAVMD) {
                AddIssue(a_result, "PostBlendFaceCustomization.LayersA.ModulationValue",
                         a_layer.modulationValue, "avm_modulation_catalog_type_mismatch",
                         std::format("layer '{}' resolved to form type {} instead of AVMD",
                                     a_layer.name,
                                     static_cast<std::uint32_t>(form->GetFormType())));
                return false;
            }

            const auto dataAddress = reinterpret_cast<std::uintptr_t>(rawData);
            std::uintptr_t beginAddress = 0;
            std::uintptr_t endAddress = 0;
            if (!Util::SafeReadQword(dataAddress + 0x60, beginAddress) ||
                !Util::SafeReadQword(dataAddress + 0x68, endAddress) ||
                endAddress < beginAddress ||
                (endAddress - beginAddress) % sizeof(RE::AVMData::Entry) != 0) {
                AddIssue(a_result, "PostBlendFaceCustomization.LayersA.ModulationValue",
                         a_layer.modulationValue, "avm_modulation_catalog_invalid",
                         std::format("layer '{}' AVMD entry span is malformed", a_layer.name));
                return false;
            }

            const auto count =
                (endAddress - beginAddress) / sizeof(RE::AVMData::Entry);
            if (count == 0 || count > kMaxAvmCatalogEntries ||
                !Util::IsReadableRange(beginAddress, count * sizeof(RE::AVMData::Entry))) {
                AddIssue(a_result, "PostBlendFaceCustomization.LayersA.ModulationValue",
                         a_layer.modulationValue, "avm_modulation_catalog_invalid",
                         std::format("layer '{}' AVMD entry count {} is outside the proven range",
                                     a_layer.name, count));
                return false;
            }

            const auto* entries =
                reinterpret_cast<const RE::AVMData::Entry*>(beginAddress);
            const auto found = std::ranges::any_of(
                std::span{ entries, count }, [&](const RE::AVMData::Entry& a_entry) {
                    return EqualEditorID(a_layer.modulationValue, a_entry.name.c_str());
                });
            if (!found) {
                AddIssue(a_result, "PostBlendFaceCustomization.LayersA.ModulationValue",
                         a_layer.modulationValue, "avm_modulation_not_found",
                         std::format("name is absent from layer '{}' AVMD entries",
                                     a_layer.name));
                return false;
            }
            return true;
        }

        void ResolveAvmReferences(
            ResolvedAppearanceDependencies& a_result,
            const AppearancePreset& a_preset)
        {
            using GetLayerValues = void (*)(
                std::uint8_t, const RE::BSFixedString*, bool,
                RE::BSScrapArray<RE::BSFixedString>*);
            using MapLookup = bool (*)(
                void*, const RE::BSFixedString*, void**);

            REL::Relocation<GetLayerValues> getLayerValues{ kFaceDbGetLayerValuesID };
            REL::Relocation<MapLookup> mapLookup{ kFaceDbMapLookupID };
            const auto hasEntriesAddress =
                REL::Relocation<std::uintptr_t>{ kFaceDbHasModulationEntriesID }.address();
            const auto mapEntriesAddress =
                REL::Relocation<std::uintptr_t>{ kFaceDbMapEntriesID }.address();
            if (!HasExpectedBytes(getLayerValues.address(), kFaceDbGetLayerValuesGate) ||
                !HasExpectedBytes(mapLookup.address(), kFaceDbMapLookupGate) ||
                !HasExpectedBytes(hasEntriesAddress, kFaceDbHasModulationEntriesGate) ||
                !ValidateFaceDbMapContract(hasEntriesAddress, mapEntriesAddress)) {
                AddIssue(a_result, "PostBlendFaceCustomization.LayersA", {},
                         "avm_catalog_contract_mismatch",
                         "Address Library IDs 97404/97406/37347/884864 do not match the 1.16.244 FaceDB contract");
                return;
            }

            const auto mapAddress = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(mapEntriesAddress) +
                kFaceDbMapOffsetFromEntries);
            if (!Util::IsReadableRange(mapAddress, 0x28)) {
                AddIssue(a_result, "PostBlendFaceCustomization.LayersA", {},
                         "avm_catalog_map_unreadable",
                         "the FaceDB category map is not readable");
                return;
            }

            bool ok = true;
            for (const auto& layer : a_preset.postBlendLayers) {
                RE::BSFixedString category{ layer.name };
                RE::BSScrapArray<RE::BSFixedString> values;
                getLayerValues(static_cast<std::uint8_t>(a_preset.skinTone),
                               &category, true, &values);
                if (values.size() == 0 || values.size() > kMaxAvmCatalogEntries ||
                    !Util::IsReadableRange(
                        reinterpret_cast<std::uintptr_t>(values.data()),
                        values.size() * sizeof(RE::BSFixedString))) {
                    ok = false;
                    AddIssue(a_result, "PostBlendFaceCustomization.LayersA.Name",
                             layer.name, "avm_layer_not_found_for_skin_tone",
                             std::format("FaceDB returned no valid value catalog for skin tone {}",
                                         a_preset.skinTone));
                    continue;
                }

                const auto valueFound = std::ranges::any_of(
                    values, [&](const RE::BSFixedString& a_value) {
                        return EqualEditorID(layer.value, a_value.c_str());
                    });
                if (!valueFound) {
                    ok = false;
                    AddIssue(a_result, "PostBlendFaceCustomization.LayersA.Value",
                             layer.value, "avm_value_not_found",
                             std::format("value is absent from layer '{}' for skin tone {}",
                                         layer.name, a_preset.skinTone));
                }

                if (!ResolveAvmModulation(a_result, layer, category,
                                          mapAddress, mapLookup.get())) {
                    ok = false;
                }
            }
            a_result.avmReferencesComplete = ok;
        }

        void ResolveColorReferences(
            ResolvedAppearanceDependencies& a_result,
            const AppearancePreset& a_preset)
        {
            using FindSimpleColorIndex = std::uint32_t (*)(
                std::uint32_t, const RE::BSFixedString*, const RE::BSFixedString*);
            using FindMappedColorIndex = std::uint32_t (*)(
                const RE::BSFixedString*, const RE::BSFixedString*,
                const RE::BSFixedString*);

            REL::Relocation<FindSimpleColorIndex> findSimple{
                kFaceDbFindSimpleColorIndexID };
            REL::Relocation<FindMappedColorIndex> findMapped{
                kFaceDbFindMappedColorIndexID };
            if (!HasExpectedBytes(findSimple.address(), kFaceDbFindSimpleColorIndexGate) ||
                !HasExpectedBytes(findMapped.address(), kFaceDbFindMappedColorIndexGate)) {
                AddIssue(a_result, "ColorAndTeethCatalogs", {},
                         "color_catalog_contract_mismatch",
                         "Address Library IDs 37341/69610 do not match the 1.16.244 FaceDB contract");
                return;
            }

            const auto category = [&](const REL::ID a_id) {
                const auto address =
                    REL::Relocation<std::uintptr_t>{ a_id }.address();
                return Util::IsReadableRange(address, sizeof(RE::BSFixedString))
                    ? reinterpret_cast<const RE::BSFixedString*>(address)
                    : nullptr;
            };
            const auto* hairCategory = category(kHairColorCategoryID);
            const auto* facialHairCategory = category(kFacialHairColorCategoryID);
            const auto* teethCategory = category(kTeethCategoryID);
            const auto* jewelryCategory = category(kJewelryColorCategoryID);
            const auto* eyeCategory = category(kEyeColorCategoryID);
            if (!hairCategory || !facialHairCategory || !teethCategory ||
                !jewelryCategory || !eyeCategory) {
                AddIssue(a_result, "ColorAndTeethCatalogs", {},
                         "color_catalog_category_unreadable",
                         "one or more FaceDB category BSFixedString globals are unreadable");
                return;
            }

            bool ok = true;
            const auto checkSimple = [&](const std::string_view a_field,
                                         const std::string& a_atom,
                                         const std::uint32_t a_storeIndex,
                                         const RE::BSFixedString* a_category) {
                if (a_atom.empty()) {
                    return;
                }
                RE::BSFixedString candidate{ a_atom };
                if (findSimple(a_storeIndex, a_category, &candidate) == 0xFFFFFFFF) {
                    ok = false;
                    AddIssue(a_result, std::string{ a_field }, a_atom,
                             "color_catalog_atom_not_found",
                             "name is absent from the engine FaceDB AVMD catalog");
                }
            };
            const auto checkMapped = [&](const std::string_view a_field,
                                         const std::string& a_atom,
                                         const RE::BGSHeadPart* a_part,
                                         const RE::BSFixedString* a_primary,
                                         const bool a_useHeadPartMask) {
                if (a_atom.empty()) {
                    return;
                }
                if (!a_part) {
                    ok = false;
                    AddIssue(a_result, std::string{ a_field }, a_atom,
                             "color_headpart_unresolved",
                             "the color-bearing headpart did not resolve");
                    return;
                }
                RE::BSFixedString candidate{ a_atom };
                const auto* first = a_useHeadPartMask
                    ? std::addressof(a_part->colorMapping)
                    : a_primary;
                const auto* second = a_useHeadPartMask
                    ? std::addressof(a_part->mask)
                    : std::addressof(a_part->colorMapping);
                if (findMapped(first, second, &candidate) == 0xFFFFFFFF) {
                    ok = false;
                    AddIssue(a_result, std::string{ a_field }, a_atom,
                             "color_catalog_atom_not_found",
                             "name is absent from the selected headpart's FaceDB AVMD palette");
                }
            };

            const auto partAt = [&](const std::size_t a_index) -> const RE::BGSHeadPart* {
                return a_index < a_result.uniqueHeadParts.size()
                    ? a_result.uniqueHeadParts[a_index]
                    : nullptr;
            };
            checkMapped("HairColor", a_preset.hairColor, partAt(3), hairCategory, false);
            checkMapped("FacialHairColor", a_preset.facialHairColor, partAt(4),
                        facialHairCategory, false);
            checkMapped("BrowHairColor", a_preset.browHairColor, partAt(6), nullptr, true);
            checkSimple("EyeColor", a_preset.eyeColor, 1, eyeCategory);
            checkSimple("JewelryColor", a_preset.jewelryColor, 3, jewelryCategory);
            checkSimple("TeethCustomization", a_preset.teethCustomization, 1, teethCategory);

            a_result.colorReferencesComplete = ok;
        }
    }

    ResolvedAppearanceDependencies ResolveAppearanceDependencies(
        const AppearancePreset& a_preset,
        RE::TESNPC* a_target)
    {
        ResolvedAppearanceDependencies result;
        if (!a_target) {
            AddIssue(result, "target", {}, "target_null", "target TESNPC is null");
            return result;
        }

        // The package's plugin/local tuple is the sole target identity.
        // NPCFormEditorID remains producer metadata and never participates in
        // target lookup or equality.
        bool formsOK = true;
        result.race = RE::TESForm::LookupByEditorID<RE::TESRace>(
            RE::BSFixedString{ a_preset.raceFormID });
        if (!result.race) {
            AddIssue(result, "RaceFormID", a_preset.raceFormID, "race_not_found",
                     "EditorID did not resolve to TESRace");
            return result;
        }
        if (a_target->GetRace() != result.race) {
            formsOK = false;
            AddIssue(result, "RaceFormID", a_preset.raceFormID, "target_race_mismatch",
                     "preset race differs from the target race; race transformation is out of scope");
        }

        const auto presetSex =
            a_preset.sex == PresetSex::kFemale ? RE::SEX::kFemale : RE::SEX::kMale;
        if (a_target->GetSex() != presetSex) {
            formsOK = false;
            AddIssue(result, "Sex",
                     a_preset.sex == PresetSex::kFemale ? "Female" : "Male",
                     "target_sex_mismatch",
                     "preset sex differs from the target; sex transformation is out of scope");
        }

        const auto issuesBeforeHeadParts = result.issues.size();
        ResolveHeadParts(result, a_preset, presetSex);
        formsOK = formsOK && result.issues.size() == issuesBeforeHeadParts;
        result.formReferencesComplete = formsOK;

        ResolveBoneReferences(result, a_preset, presetSex);
        ResolveAvmReferences(result, a_preset);
        ResolveColorReferences(result, a_preset);
        return result;
    }
}