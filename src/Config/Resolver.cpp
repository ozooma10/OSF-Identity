#include "Resolver.h"

#include "PluginLookup.h"
#include "RuntimeFormID.h"

#include <format>

namespace Config
{
    namespace
    {
        void AddIssue(ResolvedAppearanceDependencies& a_result, std::string a_field, std::string a_value, std::string a_code, std::string a_message)
        {
            a_result.issues.push_back(DependencyIssue{std::move(a_field), std::move(a_value), std::move(a_code), std::move(a_message) });
        }

        bool RaceOffersHeadPart(const RE::TESRace* a_race, const RE::SEX a_sex, const RE::BGSHeadPart* a_part)
        {
            if (!a_race || !a_part || (a_sex != RE::SEX::kMale && a_sex != RE::SEX::kFemale)) {
                return false;
            }
            if (!a_part->validRaces) {
                return true;
            }
            const auto& validRaces = a_part->validRaces->arrayOfForms;
            return std::ranges::find(validRaces, static_cast<const RE::TESForm*>(a_race)) != validRaces.end();
        }

        std::string DescribeFormReference(const PresetFormReference& a_reference)
        {
            return std::format("{}|{:X}", a_reference.plugin, a_reference.localFormID);
        }

        std::optional<RE::TESFormID> ResolveFormID(ResolvedAppearanceDependencies& a_result, const std::string_view a_field, const PresetFormReference& a_reference)
        {
            const auto plugin = FindLoadedPlugin(RE::TESDataHandler::GetSingleton(), a_reference.plugin);
            if (!plugin) {
                AddIssue(a_result, std::string{ a_field }, a_reference.plugin, "plugin_not_loaded", "referenced plugin is not loaded");
                return std::nullopt;
            }
            const auto runtimeFormID = EncodeRuntimeFormID(a_reference.localFormID, plugin->tier, plugin->index);
            if (!runtimeFormID) {
                AddIssue(a_result, std::string{ a_field }, DescribeFormReference(a_reference), "form_id_out_of_range", "local FormID does not fit the loaded plugin tier");
                return std::nullopt;
            }
            return runtimeFormID;
        }

        RE::BGSHeadPart* ValidateHeadPart(ResolvedAppearanceDependencies& a_result, const RE::TESRace* a_race, const RE::SEX a_sex,
            const std::string_view a_field, const std::string_view a_value, RE::BGSHeadPart* a_part,
            const std::optional<RE::BGSHeadPart::HeadPartType> a_expectedType, std::unordered_set<const RE::BGSHeadPart*>& a_seen)
        {
            if (!a_part) {
                AddIssue(a_result, std::string{ a_field }, std::string{ a_value }, "headpart_not_found", "form did not resolve to BGSHeadPart");
                return nullptr;
            }
            if (!a_seen.insert(a_part).second) {
                AddIssue(a_result, std::string{ a_field }, std::string{ a_value }, "duplicate_headpart", "the same head-part form occurs more than once");
                return nullptr;
            }
            if (!RaceOffersHeadPart(a_race, a_sex, a_part)) {
                AddIssue(a_result, std::string{ a_field }, std::string{ a_value }, "headpart_not_valid_for_race", "head-part's valid-races form list excludes the resolved race");
                return nullptr;
            }
            if (a_expectedType && a_part->type.get() != *a_expectedType) {
                AddIssue(a_result, std::string{ a_field }, std::string{ a_value }, "headpart_type_mismatch", std::format("resolved type={} but CK positional slot requires type={}", a_part->type.underlying(), static_cast<std::uint32_t>(*a_expectedType)));
                return nullptr;
            }
            return a_part;
        }

        RE::BGSHeadPart* ResolveCkHeadPart(ResolvedAppearanceDependencies& a_result, const RE::TESRace* a_race, const RE::SEX a_sex, const std::string_view a_field,
            const std::string_view a_editorID, const std::optional<RE::BGSHeadPart::HeadPartType> a_expectedType, std::unordered_set<const RE::BGSHeadPart*>& a_seen)
        {
            auto* part = RE::TESForm::LookupByEditorID<RE::BGSHeadPart>(RE::BSFixedString{ a_editorID });
            return ValidateHeadPart(a_result, a_race, a_sex, a_field, a_editorID, part, a_expectedType, a_seen);
        }

        void ResolveCkHeadParts(ResolvedAppearanceDependencies& a_result, const AppearancePreset& a_preset, const RE::SEX a_sex)
        {
            constexpr auto kCkUniqueSlots = static_cast<std::size_t>(RE::BGSHeadPart::HeadPartType::kEyelashes) + 1;
            if (a_preset.uniqueHeadParts.size() != kCkUniqueSlots) {
                AddIssue(a_result, "UniqueHeadPartsA", std::to_string(a_preset.uniqueHeadParts.size()), "headpart_slot_count", std::format("CK 1.16.244 contract requires {} positional slots", kCkUniqueSlots));
            }

            std::unordered_set<const RE::BGSHeadPart*> seen;
            a_result.uniqueHeadParts.resize(a_preset.uniqueHeadParts.size(), nullptr);
            for (std::size_t i = 0; i < a_preset.uniqueHeadParts.size(); ++i) {
                const auto& editorID = a_preset.uniqueHeadParts[i];
                if (editorID.empty()) {
                    continue;
                }
                if (i >= kCkUniqueSlots) {
                    AddIssue(a_result, std::format("UniqueHeadPartsA[{}]", i), editorID, "headpart_slot_out_of_range", "no proven CK positional head-part type exists for this slot");
                    continue;
                }
                const auto expected = static_cast<RE::BGSHeadPart::HeadPartType>(i);
                a_result.uniqueHeadParts[i] = ResolveCkHeadPart(a_result, a_result.race, a_sex, std::format("UniqueHeadPartsA[{}]", i), editorID, expected, seen);
            }

            a_result.miscHeadParts.reserve(a_preset.miscHeadParts.size());
            for (std::size_t i = 0; i < a_preset.miscHeadParts.size(); ++i) {
                const auto& editorID = a_preset.miscHeadParts[i];
                auto* part = ResolveCkHeadPart(a_result, a_result.race, a_sex, std::format("MiscHeadPartsA[{}]", i), editorID, std::nullopt, seen);
                if (part) {
                    a_result.miscHeadParts.push_back(part);
                }
            }
        }

        void ResolveCharGenHeadParts(ResolvedAppearanceDependencies& a_result, const AppearancePreset& a_preset, const RE::SEX a_sex)
        {
            constexpr auto kUniqueSlots = static_cast<std::size_t>(RE::BGSHeadPart::HeadPartType::kEyelashes) + 1;
            std::unordered_set<const RE::BGSHeadPart*> seen;
            a_result.uniqueHeadParts.resize(kUniqueSlots, nullptr);

            for (std::size_t i = 0; i < a_preset.headPartForms.size(); ++i) {
                const auto field = std::format("HeadParts[{}]", i);
                const auto& reference = a_preset.headPartForms[i];
                const auto runtimeFormID = ResolveFormID(a_result, field, reference);
                if (!runtimeFormID) {
                    continue;
                }
                const auto value = DescribeFormReference(reference);
                auto* part = ValidateHeadPart( a_result, a_result.race, a_sex, field, value, RE::TESForm::LookupByID<RE::BGSHeadPart>(*runtimeFormID), std::nullopt, seen);
                if (!part) {
                    continue;
                }

                const auto type = static_cast<std::size_t>(part->type.get());
                if (type >= kUniqueSlots) {
                    AddIssue(a_result, field, value, "headpart_type_unsupported", "resolved head-part type is outside the supported human appearance slots");
                    continue;
                }
                if (part->type.get() == RE::BGSHeadPart::HeadPartType::kMisc) {
                    a_result.miscHeadParts.push_back(part);
                    continue;
                }
                if (a_result.uniqueHeadParts[type]) {
                    AddIssue(a_result, field, value, "duplicate_headpart_type", "more than one non-misc head part resolves to the same appearance slot");
                    continue;
                }
                a_result.uniqueHeadParts[type] = part;
            }
        }

        void ResolveHeadParts(ResolvedAppearanceDependencies& a_result, const AppearancePreset& a_preset, const RE::SEX a_sex)
        {
            if (a_preset.sourceFormat == PresetSourceFormat::kCharGenJson) {
                ResolveCharGenHeadParts(a_result, a_preset, a_sex);
            } else {
                ResolveCkHeadParts(a_result, a_preset, a_sex);
            }
        }

        void ResolveBoneReferences(ResolvedAppearanceDependencies& a_result, const AppearancePreset& a_preset, const RE::SEX a_sex)
        {
            std::unordered_set<std::string> groupNames;
            constexpr std::uint32_t kMaxCatalogNames = 128;
            for (std::uint32_t i = 0; i < kMaxCatalogNames; ++i) {
                const auto* name = a_result.race->GetBoneGroupName(a_sex, i);
                if (!name) {
                    break;
                }
                groupNames.emplace(name->c_str());
            }

            bool shapeOK = true;
            for (const auto& morph : a_preset.facialMorphSliders) {
                const auto separator = morph.name.rfind('_');
                if (separator == std::string::npos || separator == 0 || separator + 1 >= morph.name.size()) {
                    shapeOK = false;
                    AddIssue(a_result, "FacialMorphSliderDataA.Name", morph.name, "facial_shape_name_invalid", "name is not a descriptor/group composite");
                    continue;
                }
                const auto descriptorName = morph.name.substr(0, separator);
                const auto groupName = morph.name.substr(separator + 1);
                // Chargen does not sex-lock archetype faces, so a real export can carry descriptors of either sex (e.g. 'male_as_md1' on a Female preset); accept whichever catalog has it.
                const auto otherSex = a_sex == RE::SEX::kMale ? RE::SEX::kFemale : RE::SEX::kMale;
                if (!a_result.race->FindShapeDescriptorByName(a_sex, descriptorName.c_str()) && !a_result.race->FindShapeDescriptorByName(otherSex, descriptorName.c_str())) {
                    shapeOK = false;
                    AddIssue(a_result, "FacialMorphSliderDataA.Name", morph.name, "facial_shape_descriptor_not_found", std::format("race/sex descriptor '{}' was not found", descriptorName));
                    continue;
                }
                if (!groupNames.contains(groupName)) {
                    shapeOK = false;
                    AddIssue(a_result, "FacialMorphSliderDataA.Name", morph.name, "facial_shape_group_not_found", std::format("race/sex group '{}' was not found", groupName));
                    continue;
                }
            }
            a_result.shapeReferencesComplete = shapeOK;

            bool ok = true;
            std::unordered_set<std::uint32_t> checkedDescriptors;
            for (const auto& region : a_preset.facialBoneRegions) {
                for (const auto& slider : region.sliders) {
                    if (!slider.groupName.empty()) {
                        if (!groupNames.contains(slider.groupName)) {
                            ok = false;
                            AddIssue(a_result, "FacialBoneRegionDataA.SlidersA.GroupName", slider.groupName, "bone_group_not_found", "name is absent from the race/sex chargen group-name catalog");
                        }
                    }
                    if (slider.id != 0 && checkedDescriptors.insert(slider.id).second) {
                        if (!a_result.race->GetBoneRegionDescriptor(a_sex, slider.id)) {
                            ok = false;
                            AddIssue(a_result, "FacialBoneRegionDataA.SlidersA.ID", std::to_string(slider.id), "bone_slider_not_found", "ID is absent from the race/sex chargen descriptor map");
                        }
                    }
                }
            }
            a_result.boneReferencesComplete = ok;
        }

        bool ResolveAvmModulation(ResolvedAppearanceDependencies& a_result, const PresetTintLayer& a_layer, const RE::BSFixedString& a_category)
        {
            if (a_layer.customColor || a_layer.modulationValue.empty()) {
                return true;
            }

            const auto* catalog = RE::BSFaceDB::FindCategoryData(a_category);
            if (!catalog) {
                AddIssue(a_result, "PostBlendFaceCustomization.LayersA.ModulationValue", a_layer.modulationValue, "avm_modulation_catalog_not_found", std::format("layer '{}' has no FaceDB modulation catalog", a_layer.name));
                return false;
            }

            const auto entries = catalog->GetEntries();
            if (entries.empty()) {
                AddIssue(a_result, "PostBlendFaceCustomization.LayersA.ModulationValue", a_layer.modulationValue, "avm_modulation_catalog_empty", std::format("layer '{}' AVMD catalog has no entries", a_layer.name));
                return false;
            }

            const auto found = std::ranges::any_of(entries, [&](const RE::AVMData::Entry& a_entry) {
                return a_entry.name == std::string_view{ a_layer.modulationValue };
            });
            if (!found) {
                AddIssue(a_result, "PostBlendFaceCustomization.LayersA.ModulationValue", a_layer.modulationValue, "avm_modulation_not_found", std::format("name is absent from layer '{}' AVMD entries", a_layer.name));
                return false;
            }
            return true;
        }

        bool UsesComplexAvmGroup(const std::string_view a_category)
        {
            return a_category == "Scars" || a_category == "Accents1" || a_category == "Accents2" || a_category == "ColorlessAccents1" || a_category == "ColorlessAccents2";
        }

        void ResolveAvmReferences(ResolvedAppearanceDependencies& a_result, const AppearancePreset& a_preset)
        {
            if (a_preset.postBlendLayers.empty()) {
                a_result.avmReferencesComplete = true;
                return;
            }

            bool ok = true;
            a_result.avmLayers.reserve(a_preset.postBlendLayers.size());
            for (const auto& layer : a_preset.postBlendLayers) {
                RE::BSFixedString category{ layer.name };
                if (layer.materialType || layer.texturePath) {
                    if (!layer.materialType || !layer.texturePath || !layer.customColor ||
                        (*layer.materialType != 1 && *layer.materialType != 2)) {
                        ok = false;
                        AddIssue(a_result, "HeadPartData", layer.name, "direct_avm_incomplete", "CharGen JSON AVM material data is incomplete or unsupported");
                        continue;
                    }

                    RE::AVMData materialized{};
                    materialized.type = static_cast<RE::AVMData::Type>(*layer.materialType);
                    materialized.category = category;
                    materialized.unk10.name = RE::BSFixedString{ layer.value };
                    materialized.unk10.texturePath = RE::BSFixedString{ *layer.texturePath };
                    materialized.unk10.color = RE::Color{ layer.customColor->red, layer.customColor->green, layer.customColor->blue, layer.customColor->rough };
                    materialized.unk10.intensity = layer.packedIntensity;
                    a_result.avmLayers.push_back(std::move(materialized));
                    continue;
                }

                // The skin-tone value catalog only exists for chargen-authorable layers (those with a bare SimpleGroup_/Chargen_ AVMD). 
                // Complex-group layers such as Accents1 have no catalog yet are valid on NPC records, so an empty catalog is advisory; 
                RE::BSScrapArray<RE::BSFixedString> values;
                RE::BSFaceDB::GetLayerValues(a_preset.skinTone, category, true, values);
                if (!values.empty()) {
                    const auto valueFound = std::ranges::any_of(values, [&](const RE::BSFixedString& a_value) {
                        return a_value == std::string_view{ layer.value };
                    });
                    if (!valueFound) {
                        ok = false;
                        AddIssue(a_result, "PostBlendFaceCustomization.LayersA.Value", layer.value, "avm_value_not_found", std::format("value is absent from layer '{}' for skin tone {}", layer.name, a_preset.skinTone));
                    }
                }

                if (!ResolveAvmModulation(a_result, layer, category)) {
                    ok = false;
                }

                RE::AVMData materialized{};
                materialized.category = category;
                const RE::BSFixedString value{ layer.value };
                auto valueCategory = category;
                auto type = RE::AVMData::Type::kSimpleGroup;
                if (UsesComplexAvmGroup(layer.name)) {
                    RE::BSScrapArray<RE::BSFixedString> groups;
                    RE::BSFaceDB::GetCategoryValues(2, category, groups);
                    if (!groups.empty()) {
                        valueCategory = groups[0];
                        type = RE::AVMData::Type::kComplexGroup;
                    }
                }

                if (!RE::BSFaceDB::ResolveEntry(1, valueCategory, value, materialized.unk10)) {
                    ok = false;
                    AddIssue(a_result, "PostBlendFaceCustomization.LayersA.Value", layer.value, "avm_value_materialization_failed", std::format("value could not be materialized for layer '{}' through FaceDB {} group '{}'", layer.name, type == RE::AVMData::Type::kComplexGroup ? "complex" : "simple", valueCategory.c_str()));
                    continue;
                }
                materialized.type = type;

                if (layer.customColor) {
                    materialized.unk10.color = RE::Color{
                        layer.customColor->red,
                        layer.customColor->green,
                        layer.customColor->blue,
                        layer.customColor->rough };
                } else if (!layer.modulationValue.empty()) {
                    const RE::BSFixedString modulationValue{ layer.modulationValue };
                    RE::AVMData::Entry modulation{};
                    if (!RE::BSFaceDB::ResolveEntry(3, category, modulationValue, modulation)) {
                        ok = false;
                        AddIssue(a_result, "PostBlendFaceCustomization.LayersA.ModulationValue", layer.modulationValue, "avm_modulation_materialization_failed", std::format("modulation could not be materialized for layer '{}'", layer.name));
                        continue;
                    }
                    materialized.unk10.color = modulation.color;
                }
                materialized.unk10.intensity = layer.packedIntensity;
                a_result.avmLayers.push_back(std::move(materialized));
            }
            a_result.avmReferencesComplete = ok && a_result.avmLayers.size() == a_preset.postBlendLayers.size();
        }

        void ResolveColorReferences(ResolvedAppearanceDependencies& a_result, const AppearancePreset& a_preset)
        {
            const auto& hairCategory = RE::BSFaceDB::GetHairColorCategory();
            const auto& facialHairCategory = RE::BSFaceDB::GetFacialHairColorCategory();
            const auto& teethCategory = RE::BSFaceDB::GetTeethCategory();
            const auto& jewelryCategory = RE::BSFaceDB::GetJewelryColorCategory();
            const auto& eyeCategory = RE::BSFaceDB::GetEyeColorCategory();

            bool ok = true;
            const auto checkSimple = [&](const std::string_view a_field, const std::string& a_atom, const std::uint32_t a_storeIndex, const RE::BSFixedString& a_category) {
                if (a_atom.empty()) {
                    return;
                }
                RE::BSFixedString candidate{ a_atom };
                if (RE::BSFaceDB::FindSimpleColorIndex(a_storeIndex, a_category, candidate) == 0xFFFFFFFF) {
                    ok = false;
                    AddIssue(a_result, std::string{ a_field }, a_atom, "color_catalog_atom_not_found", "name is absent from the engine FaceDB AVMD catalog");
                }
            };
            const auto checkMapped = [&](const std::string_view a_field, const std::string& a_atom, const RE::BGSHeadPart* a_part, const RE::BSFixedString* a_primary, const bool a_useHeadPartMask) {
                if (a_atom.empty()) {
                    return;
                }
                if (!a_part) {
                    ok = false;
                    AddIssue(a_result, std::string{ a_field }, a_atom, "color_headpart_unresolved", "the color-bearing headpart did not resolve");
                    return;
                }
                RE::BSFixedString candidate{ a_atom };
                const auto& first = a_useHeadPartMask ? a_part->colorMapping : *a_primary;
                const auto& second = a_useHeadPartMask ? a_part->mask : a_part->colorMapping;
                if (RE::BSFaceDB::FindMappedColorIndex(first, second, candidate) == 0xFFFFFFFF) {
                    ok = false;
                    AddIssue(a_result, std::string{ a_field }, a_atom, "color_catalog_atom_not_found", "name is absent from the selected headpart's FaceDB AVMD palette");
                }
            };

            const auto partAt = [&](const std::size_t a_index) -> const RE::BGSHeadPart* {
                return a_index < a_result.uniqueHeadParts.size() ? a_result.uniqueHeadParts[a_index] : nullptr;
            };
            checkMapped("HairColor", a_preset.hairColor, partAt(3), std::addressof(hairCategory), false);
            checkMapped("FacialHairColor", a_preset.facialHairColor, partAt(4), std::addressof(facialHairCategory), false);
            checkMapped("BrowHairColor", a_preset.browHairColor, partAt(6), nullptr, true);
            checkSimple("EyeColor", a_preset.eyeColor, 1, eyeCategory);
            checkSimple("JewelryColor", a_preset.jewelryColor, 3, jewelryCategory);
            checkSimple("TeethCustomization", a_preset.teethCustomization, 1, teethCategory);

            a_result.colorReferencesComplete = ok;
        }
    }

    ResolvedAppearanceDependencies ResolveAppearanceDependencies(const AppearancePreset& a_preset, RE::TESNPC* a_target)
    {
        ResolvedAppearanceDependencies result;
        if (!a_target) {
            AddIssue(result, "target", {}, "target_null", "target TESNPC is null");
            return result;
        }

        bool formsOK = true;
        for (const auto& plugin : a_preset.requiredPlugins) {
            if (!FindLoadedPlugin(RE::TESDataHandler::GetSingleton(), plugin)) {
                formsOK = false;
                AddIssue(result, "Dependencies", plugin, "plugin_not_loaded", "required CharGen JSON plugin is not loaded");
            }
        }

        std::string raceField = "RaceFormID";
        std::string raceValue = a_preset.raceFormID;
        if (a_preset.sourceFormat == PresetSourceFormat::kCharGenJson) {
            raceField = "Race";
            if (!a_preset.raceForm) {
                AddIssue(result, raceField, {}, "race_reference_missing", "CharGen JSON preset has no decoded race form reference");
                return result;
            }
            raceValue = DescribeFormReference(*a_preset.raceForm);
            const auto runtimeFormID = ResolveFormID(result, raceField, *a_preset.raceForm);
            if (runtimeFormID) {
                result.race = RE::TESForm::LookupByID<RE::TESRace>(*runtimeFormID);
            }
        } else {
            result.race = RE::TESForm::LookupByEditorID<RE::TESRace>(RE::BSFixedString{ a_preset.raceFormID });
        }
        if (!result.race) {
            AddIssue(result, raceField, raceValue, "race_not_found", "form did not resolve to TESRace");
            return result;
        }
        if (a_target->GetRace() != result.race) {
            formsOK = false;
            AddIssue(result, raceField, raceValue, "target_race_mismatch", "preset race differs from the target race; race transformation is out of scope");
        }

        const auto presetSex = a_preset.sex == PresetSex::kFemale ? RE::SEX::kFemale : RE::SEX::kMale;
        if (a_target->GetSex() != presetSex) {
            formsOK = false;
            AddIssue(result, a_preset.sourceFormat == PresetSourceFormat::kCharGenJson ? "Gender" : "Sex", a_preset.sex == PresetSex::kFemale ? "Female" : "Male", "target_sex_mismatch", "preset sex differs from the target; sex transformation is out of scope");
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
