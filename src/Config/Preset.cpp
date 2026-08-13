#include "Preset.h"
#include "ConfigDetail.h"
#include "JsonSchema.h"
#include "Util/String.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <format>
#include <limits>
#include <ranges>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace Config
{
    namespace
    {
        constexpr std::size_t kBodyMorphRegionCount = 5;
        constexpr std::int64_t kMinSerializedRuntimeID = std::numeric_limits<std::int32_t>::min();
        constexpr std::int64_t kMaxRuntimeID = std::numeric_limits<std::uint32_t>::max();
        constexpr double kCkTintScale = 64.0;
        constexpr double kCharGenMenuTintScale = 128.0;
        constexpr double kTintQuantizationTolerance = 1.0e-6;

        bool IsSemanticString(const std::string_view a_value, const bool a_allowEmpty) noexcept
        {
            if (!a_allowEmpty && a_value.empty()) {
                return false;
            }
            return std::ranges::all_of(a_value, [](const unsigned char a_ch) {
                return a_ch >= 0x20 && a_ch != 0x7F;
            });
        }

        struct Validator
        {
            PresetResult& result;
            const std::filesystem::path& path;

            bool Fail(std::string a_code, std::string a_message)
            {
                result.issues.push_back({ path, 0, std::move(a_code), std::move(a_message) });
                return false;
            }

            bool Str(const std::string& a_value, const std::string_view a_context, const bool a_allowEmpty = true)
            {
                return IsSemanticString(a_value, a_allowEmpty) || Fail("invalid_string", std::string(a_context) + " contains an invalid control character");
            }

            bool Bounded(const double a_value, const double a_min, const double a_max, const std::string_view a_context)
            {
                return (std::isfinite(a_value) && a_value >= a_min && a_value <= a_max) || Fail("number_out_of_range", std::string(a_context) + " is outside the accepted range");
            }

            bool Index(const std::int64_t a_value, const std::int64_t a_max, const std::string_view a_context)
            {
                return (a_value >= 0 && a_value <= a_max) || Fail("invalid_integer", std::string(a_context) + " must be a bounded non-negative integer");
            }

            bool RuntimeID(const std::int64_t a_value, const std::string_view a_context)
            {
                return (a_value >= kMinSerializedRuntimeID && a_value <= kMaxRuntimeID) || Fail("invalid_integer", std::string(a_context) + " must fit a serialized 32-bit runtime ID");
            }
        };

        std::uint32_t DecodeRuntimeID(const std::int64_t a_value) noexcept
        {
            return a_value < 0 ? static_cast<std::uint32_t>(static_cast<std::int32_t>(a_value)) : static_cast<std::uint32_t>(a_value);
        }

        bool ParseDecimalID(const std::string_view a_text, std::uint32_t& a_out)
        {
            if (a_text.empty()) {
                return false;
            }
            std::uint32_t value = 0;
            const auto [end, error] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), value, 10);
            if (error != std::errc{} || end != a_text.data() + a_text.size()) {
                return false;
            }
            a_out = value;
            return true;
        }

        bool ReadFormReference(const std::string& a_text, PresetFormReference& a_out, Validator& a_validator, const std::string_view a_context)
        {
            const auto separator = a_text.find('|');
            if (separator == std::string::npos || separator == 0 || separator + 1 == a_text.size() ||
                a_text.find('|', separator + 1) != std::string::npos) {
                return a_validator.Fail("invalid_form_reference", std::string(a_context) + " must use Plugin.esm|LocalFormID");
            }

            const auto plugin = std::string_view{ a_text }.substr(0, separator);
            const auto localText = std::string_view{ a_text }.substr(separator + 1);
            std::uint32_t localFormID = 0;
            if (!Detail::IsPluginName(plugin) || !Detail::ParseLocalFormID(localText, localFormID)) {
                return a_validator.Fail("invalid_form_reference", std::string(a_context) + " must use a valid plugin name and hexadecimal local FormID");
            }

            a_out = PresetFormReference{ .plugin = std::string{ plugin }, .localFormID = localFormID };
            return true;
        }

        bool ReadStringArray(const std::vector<std::string>& a_items, std::vector<std::string>& a_out, Validator& a_validator, const std::string_view a_context)
        {
            for (const auto& item : a_items) {
                if (!a_validator.Str(item, a_context)) {
                    return false;
                }
            }
            a_out = a_items;
            return true;
        }

        bool ReadNamedMorphs(const std::vector<Schema::NamedMorph>& a_items, std::vector<PresetNamedMorph>& a_out, Validator& a_validator)
        {
            a_out.reserve(a_items.size());
            for (const auto& item : a_items) {
                if (!a_validator.Str(item.Name, "facial morph name", false) ||
                    !a_validator.Bounded(item.Value, 0.0, 1.0, "facial morph value")) {
                    return false;
                }
                a_out.push_back(PresetNamedMorph{ .name = item.Name, .value = item.Value });
            }
            return true;
        }

        bool ReadBoneRegions(const std::vector<Schema::BoneRegion>& a_items, std::vector<PresetBoneRegion>& a_out, Validator& a_validator)
        {
            a_out.reserve(a_items.size());
            for (const auto& item : a_items) {
                if (!a_validator.RuntimeID(item.RegionID, "RegionID")) {
                    return false;
                }
                PresetBoneRegion decoded;
                decoded.regionID = DecodeRuntimeID(item.RegionID);
                decoded.sliders.reserve(item.SlidersA.size());
                for (const auto& slider : item.SlidersA) {
                    if (!a_validator.Str(slider.GroupName, "slider group") || !a_validator.RuntimeID(slider.ID, "slider ID") || !a_validator.Bounded(slider.Value, -1.0, 1.0, "bone slider value")) {
                        return false;
                    }
                    decoded.sliders.push_back(PresetBoneSlider{
                        .groupName = slider.GroupName,
                        .id = DecodeRuntimeID(slider.ID),
                        .value = slider.Value });
                }
                a_out.push_back(std::move(decoded));
            }
            return true;
        }

        bool ReadTintLayers(const std::vector<Schema::TintLayer>& a_items, std::vector<PresetTintLayer>& a_out, Validator& a_validator, const bool a_charGenMenu)
        {
            a_out.reserve(a_items.size());
            for (const auto& item : a_items) {
                if (!a_validator.Str(item.Name, "tint layer name", false) ||
                    !a_validator.Str(item.ModulationValue.Value, "tint layer ModulationValue") ||
                    !a_validator.Str(item.Value.Value, "tint layer Value") ||
                    !a_validator.Bounded(item.Intensity, 0.0, 1.0, "tint layer intensity")) {
                    return false;
                }

                PresetTintLayer decoded{
                    .name = item.Name,
                    .value = item.Value.Value,
                    .modulationValue = item.ModulationValue.Value,
                    .customColor = std::nullopt,
                    .materialType = std::nullopt,
                    .texturePath = std::nullopt };

                if (item.ModulationValue.CustomColorValue) {
                    const auto& color = *item.ModulationValue.CustomColorValue;
                    if (!a_validator.Index(color.Red, 255, "CustomColorValue.Red") ||
                        !a_validator.Index(color.Green, 255, "CustomColorValue.Green") ||
                        !a_validator.Index(color.Blue, 255, "CustomColorValue.Blue") ||
                        !a_validator.Index(color.Rough, 255, "CustomColorValue.Rough")) {
                        return false;
                    }
                    decoded.customColor = PresetCustomColor{
                        .red = static_cast<std::uint8_t>(color.Red),
                        .green = static_cast<std::uint8_t>(color.Green),
                        .blue = static_cast<std::uint8_t>(color.Blue),
                        .rough = static_cast<std::uint8_t>(color.Rough) };
                }

                if (a_charGenMenu) {
                    // CharGenMenu serializes the engine's packed intensity as a 1/128-quantized number, including the full packed 0..128 range.
                    const auto scaled = item.Intensity * kCharGenMenuTintScale;
                    const auto packed = std::round(scaled);
                    if (std::abs(scaled - packed) > kTintQuantizationTolerance) {
                        return a_validator.Fail("invalid_char_gen_tint_intensity", "CharGenMenu tint intensity must be a 1/128-quantized value from 0 to 1");
                    }
                    decoded.packedIntensity = static_cast<std::uint32_t>(packed);
                } else {
                    decoded.packedIntensity = static_cast<std::uint32_t>(std::floor(item.Intensity * kCkTintScale));
                }
                a_out.push_back(std::move(decoded));
            }
            return true;
        }

        bool ReadCharGenTintLayers(const std::vector<Schema::CharGenHeadPartData>& a_items, std::vector<PresetTintLayer>& a_out, Validator& a_validator)
        {
            a_out.reserve(a_items.size());
            for (const auto& item : a_items) {
                if (!a_validator.Str(item.Group, "HeadPartData.Group", false) || !a_validator.Str(item.Name, "HeadPartData.Name", false) || !a_validator.Str(item.Texture, "HeadPartData.Texture", false) || 
                !a_validator.Bounded(item.Intensity, 0.0, 1.0, "HeadPartData.Intensity") || item.Type < 1 || item.Type > 2) {
                    if (item.Type < 1 || item.Type > 2) {
                        a_validator.Fail("unsupported_value", "HeadPartData.Type must be 1 (simple) or 2 (complex)");
                    }
                    return false;
                }
                if (!a_validator.Index(item.Color.r, 255, "HeadPartData.Color.r") || !a_validator.Index(item.Color.g, 255, "HeadPartData.Color.g") || !a_validator.Index(item.Color.b, 255, "HeadPartData.Color.b") ||
                    !a_validator.Index(item.Color.a, 255, "HeadPartData.Color.a") || !a_validator.Index(item.unk04, kMaxRuntimeID, "HeadPartData.unk04")) {
                    return false;
                }

                const auto scaled = item.Intensity * kCharGenMenuTintScale;
                const auto packed = std::round(scaled);
                if (std::abs(scaled - packed) > kTintQuantizationTolerance) {
                    return a_validator.Fail("invalid_char_gen_tint_intensity", "HeadPartData.Intensity must be a 1/128-quantized value from 0 to 1");
                }

                a_out.push_back(PresetTintLayer{
                    .name = item.Group,
                    .value = item.Name,
                    .modulationValue = {},
                    .customColor = PresetCustomColor{
                        .red = static_cast<std::uint8_t>(item.Color.r),
                        .green = static_cast<std::uint8_t>(item.Color.g),
                        .blue = static_cast<std::uint8_t>(item.Color.b),
                        .rough = static_cast<std::uint8_t>(item.Color.a) },
                    .materialType = static_cast<std::uint32_t>(item.Type),
                    .texturePath = item.Texture,
                    .packedIntensity = static_cast<std::uint32_t>(packed) });
            }
            return true;
        }
    }

    PresetResult ParseCkPreset(const std::string_view a_json, const std::filesystem::path& a_path) try
    {
        PresetResult result;
        if (a_json.empty() || a_json.size() > kMaxPresetBytes) {
            result.issues.push_back({ a_path, 0, "invalid_size", "preset is empty or exceeds the 32 MiB safety limit" });
            return result;
        }

        Schema::CkPreset document;
        if (const auto ec = glz::read<Schema::kParseOpts>(document, a_json); ec) {
            result.issues.push_back({ a_path, ec.count, "invalid_json", glz::format_error(ec, a_json) });
            return result;
        }

        Validator validator{ result, a_path };
        AppearancePreset preset;

        if (!validator.Str(document.BrowHairColor, "BrowHairColor") ||
            !validator.Str(document.EyeColor, "EyeColor") ||
            !validator.Str(document.FacialHairColor, "FacialHairColor") ||
            !validator.Str(document.HairColor, "HairColor") ||
            !validator.Str(document.JewelryColor, "JewelryColor") ||
            !validator.Str(document.NPCFormEditorID, "NPCFormEditorID") ||
            !validator.Str(document.RaceFormID, "RaceFormID", false) ||
            !validator.Str(document.TeethCustomization, "TeethCustomization")) {
            return result;
        }

        // CharGenMenu's public producer contract serializes this as uint32_t even though the current TESNPC field is one byte. Real exports can therefore contain unrelated padding in the upper 24 bits.
        const bool isCharGenMenu = document.NPCFormEditorID.empty();
        if (!validator.Index(document.SkinTone, isCharGenMenu ? kMaxRuntimeID : 255, "SkinTone")) {
            return result;
        }

        preset.browHairColor = document.BrowHairColor;
        preset.eyeColor = document.EyeColor;
        preset.facialHairColor = document.FacialHairColor;
        preset.hairColor = document.HairColor;
        preset.jewelryColor = document.JewelryColor;
        preset.npcFormEditorID = document.NPCFormEditorID;
        preset.raceFormID = document.RaceFormID;
        preset.teethCustomization = document.TeethCustomization;
        preset.skinTone = static_cast<std::uint8_t>(static_cast<std::uint32_t>(document.SkinTone));

        if (document.Sex == "Female") {
            preset.sex = PresetSex::kFemale;
        } else if (document.Sex == "Male") {
            preset.sex = PresetSex::kMale;
        } else {
            validator.Fail("unsupported_value", "Sex must be exactly 'Male' or 'Female'");
            return result;
        }

        if (!document.BodyMorphRegionValuesA.empty() && document.BodyMorphRegionValuesA.size() != kBodyMorphRegionCount) {
            validator.Fail("count_out_of_range", "BodyMorphRegionValuesA must be empty or contain exactly five values for CK 1.16.244");
            return result;
        }
        preset.bodyMorphRegionValues.reserve(document.BodyMorphRegionValuesA.size());
        for (const auto value : document.BodyMorphRegionValuesA) {
            if (!validator.Bounded(value, 0.0, 1.0, "body morph region value")) {
                return result;
            }
            preset.bodyMorphRegionValues.push_back(value);
        }

        if (!validator.Bounded(document.MorphWeights.x, 0.0, 1.0, "MorphWeights.x") ||
            !validator.Bounded(document.MorphWeights.y, 0.0, 1.0, "MorphWeights.y") ||
            !validator.Bounded(document.MorphWeights.z, 0.0, 1.0, "MorphWeights.z")) {
            return result;
        }
        preset.morphWeights = PresetMorphWeights{ .x = document.MorphWeights.x,
                                                  .y = document.MorphWeights.y,
                                                  .z = document.MorphWeights.z };

        if (!ReadStringArray(document.MiscHeadPartsA, preset.miscHeadParts, validator, "MiscHeadPartsA") ||
            !ReadStringArray(document.UniqueHeadPartsA, preset.uniqueHeadParts, validator,  "UniqueHeadPartsA") ||
            !ReadNamedMorphs(document.FacialMorphSliderDataA, preset.facialMorphSliders, validator) ||
            !ReadBoneRegions(document.FacialBoneRegionDataA, preset.facialBoneRegions, validator) ||
            !ReadTintLayers(document.PostBlendFaceCustomization.LayersA, preset.postBlendLayers, validator, isCharGenMenu)) {
            return result;
        }

        result.preset = std::move(preset);
        return result;
    }
    catch (const std::exception& e)
    {
        PresetResult result;
        try {
            result.issues.push_back({ a_path, 0, "parser_exception", "preset parser exception: " + std::string{ e.what() } });
        } catch (...) {
        }
        return result;
    }
    catch (...)
    {
        PresetResult result;
        try {
            result.issues.push_back({ a_path, 0, "parser_exception", "preset parser unknown exception" });
        } catch (...) {
        }
        return result;
    }

    PresetResult ParseCharGenJsonPreset(const std::string_view a_json, const std::filesystem::path& a_path) try
    {
        PresetResult result;
        if (a_json.empty() || a_json.size() > kMaxPresetBytes) {
            result.issues.push_back({ a_path, 0, "invalid_size", "preset is empty or exceeds the 32 MiB safety limit" });
            return result;
        }

        Schema::CharGenJsonPreset document;
        if (const auto ec = glz::read<Schema::kParseOpts>(document, a_json); ec) {
            result.issues.push_back({ a_path, ec.count, "invalid_json", glz::format_error(ec, a_json) });
            return result;
        }

        Validator validator{ result, a_path };
        if (document.Version != 2) {
            validator.Fail("unsupported_version", "CharGen JSON Version must be exactly 2");
            return result;
        }
        if (!validator.Str(document.Name, "Name") ||
            !validator.Str(document.EyeColor, "EyeColor") ||
            !validator.Str(document.EyebrowColor, "EyebrowColor") ||
            !validator.Str(document.FacialHairColor, "FacialHairColor") ||
            !validator.Str(document.HairColor, "HairColor") ||
            !validator.Str(document.JewelryColor, "JewelryColor") ||
            !validator.Str(document.Teeth, "Teeth") ||
            !validator.Str(document.unk470, "unk470") ||
            !validator.Index(document.Pronoun, 255, "Pronoun") ||
            !validator.Index(document.SkinTone, 255, "SkinTone")) {
            return result;
        }

        AppearancePreset preset;
        preset.sourceFormat = PresetSourceFormat::kCharGenJson;
        preset.skinTone = static_cast<std::uint8_t>(document.SkinTone);
        preset.browHairColor = document.EyebrowColor;
        preset.eyeColor = document.EyeColor;
        preset.facialHairColor = document.FacialHairColor;
        preset.hairColor = document.HairColor;
        preset.jewelryColor = document.JewelryColor;
        preset.teethCustomization = document.Teeth;

        if (document.Gender == "Female") {
            preset.sex = PresetSex::kFemale;
        } else if (document.Gender == "Male") {
            preset.sex = PresetSex::kMale;
        } else {
            validator.Fail("unsupported_value", "Gender must be exactly 'Male' or 'Female'");
            return result;
        }

        if (document.Dependencies.empty()) {
            validator.Fail("missing_dependency", "Dependencies must contain every plugin referenced by the preset");
            return result;
        }
        std::unordered_set<std::string> dependencyKeys;
        preset.requiredPlugins.reserve(document.Dependencies.size());
        for (const auto& plugin : document.Dependencies) {
            if (!Detail::IsPluginName(plugin)) {
                validator.Fail("invalid_plugin_name", "Dependencies contains an invalid plugin filename");
                return result;
            }
            if (!dependencyKeys.insert(Util::FoldASCII(plugin)).second) {
                validator.Fail("duplicate_dependency", "Dependencies contains the same plugin more than once");
                return result;
            }
            preset.requiredPlugins.push_back(plugin);
        }

        PresetFormReference race;
        if (!ReadFormReference(document.Race, race, validator, "Race")) {
            return result;
        }
        if (!dependencyKeys.contains(Util::FoldASCII(race.plugin))) {
            validator.Fail("missing_dependency", "Race references a plugin absent from Dependencies");
            return result;
        }
        preset.raceForm = std::move(race);

        std::unordered_set<std::string> headPartKeys;
        preset.headPartForms.reserve(document.HeadParts.size());
        for (std::size_t i = 0; i < document.HeadParts.size(); ++i) {
            PresetFormReference reference;
            if (!ReadFormReference(document.HeadParts[i], reference, validator, std::format("HeadParts[{}]", i))) {
                return result;
            }
            if (!dependencyKeys.contains(Util::FoldASCII(reference.plugin))) {
                validator.Fail("missing_dependency", std::format("HeadParts[{}] references a plugin absent from Dependencies", i));
                return result;
            }
            const auto key = Util::FoldASCII(reference.plugin) + ":" + std::to_string(reference.localFormID);
            if (!headPartKeys.insert(key).second) {
                validator.Fail("duplicate_headpart", "HeadParts contains the same form more than once");
                return result;
            }
            preset.headPartForms.push_back(std::move(reference));
        }

        if (document.BodyMorphRegionValues && !document.BodyMorphRegionValues->empty() && document.BodyMorphRegionValues->size() != kBodyMorphRegionCount) {
            validator.Fail("count_out_of_range", "BodyMorphRegionValues must be empty or contain exactly five values");
            return result;
        }
        if (document.BodyMorphRegionValues) {
            preset.bodyMorphRegionValues.reserve(document.BodyMorphRegionValues->size());
            for (const auto value : *document.BodyMorphRegionValues) {
                if (!validator.Bounded(value, 0.0, 1.0, "body morph region value")) {
                    return result;
                }
                preset.bodyMorphRegionValues.push_back(value);
            }
        }

        if (!validator.Bounded(document.Weight.Thin, 0.0, 1.0, "Weight.Thin") ||
            !validator.Bounded(document.Weight.Muscular, 0.0, 1.0, "Weight.Muscular") ||
            !validator.Bounded(document.Weight.Heavy, 0.0, 1.0, "Weight.Heavy")) {
            return result;
        }
        preset.morphWeights = PresetMorphWeights{
            .x = document.Weight.Thin,
            .y = document.Weight.Muscular,
            .z = document.Weight.Heavy };

        preset.facialMorphSliders.reserve(document.ShapeBlendData.size());
        for (const auto& [name, value] : document.ShapeBlendData) {
            if (!validator.Str(name, "ShapeBlendData name", false) ||
                !validator.Bounded(value, 0.0, 1.0, "ShapeBlendData value")) {
                return result;
            }
            preset.facialMorphSliders.push_back(PresetNamedMorph{ .name = name, .value = value });
        }

        preset.facialBoneRegions.reserve(document.Morphs.size() + (document.AdditionalSliders.empty() ? 0u : 1u));
        for (const auto& [regionText, sliders] : document.Morphs) {
            std::uint32_t regionID = 0;
            if (!ParseDecimalID(regionText, regionID)) {
                validator.Fail("invalid_integer", "Morphs region keys must be unsigned decimal 32-bit IDs");
                return result;
            }
            PresetBoneRegion region{ .regionID = regionID, .sliders = {} };
            region.sliders.reserve(sliders.size());
            for (const auto& [groupName, value] : sliders) {
                if (!validator.Str(groupName, "Morphs group name", false) ||
                    !validator.Bounded(value, -1.0, 1.0, "Morphs value")) {
                    return result;
                }
                region.sliders.push_back(PresetBoneSlider{ .groupName = groupName, .id = 0, .value = value });
            }
            preset.facialBoneRegions.push_back(std::move(region));
        }

        if (!document.AdditionalSliders.empty()) {
            PresetBoneRegion directSliders;
            directSliders.sliders.reserve(document.AdditionalSliders.size());
            for (const auto& [idText, value] : document.AdditionalSliders) {
                std::uint32_t id = 0;
                if (!ParseDecimalID(idText, id) || id == 0) {
                    validator.Fail("invalid_integer", "AdditionalSliders keys must be non-zero unsigned decimal 32-bit IDs");
                    return result;
                }
                if (!validator.Bounded(value, -1.0, 1.0, "AdditionalSliders value")) {
                    return result;
                }
                directSliders.sliders.push_back(PresetBoneSlider{ .groupName = {}, .id = id, .value = value });
            }
            preset.facialBoneRegions.push_back(std::move(directSliders));
        }

        if (!ReadCharGenTintLayers(document.HeadPartData, preset.postBlendLayers, validator)) {
            return result;
        }

        result.preset = std::move(preset);
        return result;
    }
    catch (const std::exception& e)
    {
        PresetResult result;
        try {
            result.issues.push_back({ a_path, 0, "parser_exception", "preset parser exception: " + std::string{ e.what() } });
        } catch (...) {
        }
        return result;
    }
    catch (...)
    {
        PresetResult result;
        try {
            result.issues.push_back({ a_path, 0, "parser_exception", "preset parser unknown exception" });
        } catch (...) {
        }
        return result;
    }

    PresetResult LoadPreset(const std::filesystem::path& a_path)
    {
        PresetResult result;
        const auto sourceFormat = Detail::PresetFormatFromExtension(a_path);
        if (!sourceFormat) {
            result.issues.push_back({ a_path, 0, "invalid_extension", "preset path must use the .npc or .json extension" });
            return result;
        }

        std::error_code ec;
        const auto size = std::filesystem::file_size(a_path, ec);
        if (ec || size == 0 || size > kMaxPresetBytes) {
            result.issues.push_back({ a_path, 0, "invalid_size", ec ? "could not determine preset size: " + ec.message() : "preset is empty or exceeds the 32 MiB safety limit" });
            return result;
        }

        std::string bytes(static_cast<std::size_t>(size), '\0');
        std::ifstream stream{ a_path, std::ios::binary };
        if (!stream.is_open() || !stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
            result.issues.push_back({ a_path, 0, "read_failed", "could not read the complete preset file" });
            return result;
        }
        return *sourceFormat == PresetSourceFormat::kCkNpc ? ParseCkPreset(bytes, a_path) : ParseCharGenJsonPreset(bytes, a_path);
    }
}
