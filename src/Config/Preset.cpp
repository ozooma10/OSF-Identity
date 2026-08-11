#include "Preset.h"
#include "JsonSchema.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>

namespace Config
{
    namespace
    {
        constexpr std::size_t kBodyMorphRegionCount = 5;
        constexpr std::int64_t kMaxRuntimeID = std::numeric_limits<std::uint32_t>::max();
        constexpr double kCharGenMenuTintScale = 128.0;
        constexpr double kRuntimeTintScale = 64.0;
        constexpr double kTintQuantizationTolerance = 1.0e-6;

        [[nodiscard]] bool IsSemanticString(const std::string_view a_value, const bool a_allowEmpty) noexcept
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
        };

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
                if (!a_validator.Index(item.RegionID, kMaxRuntimeID, "RegionID")) {
                    return false;
                }
                PresetBoneRegion decoded;
                decoded.regionID = static_cast<std::uint32_t>(item.RegionID);
                decoded.sliders.reserve(item.SlidersA.size());
                for (const auto& slider : item.SlidersA) {
                    if (!a_validator.Str(slider.GroupName, "slider group") || !a_validator.Index(slider.ID, kMaxRuntimeID, "slider ID") || !a_validator.Bounded(slider.Value, -1.0, 1.0, "bone slider value")) {
                        return false;
                    }
                    decoded.sliders.push_back(PresetBoneSlider{
                        .groupName = slider.GroupName,
                        .id = static_cast<std::uint32_t>(slider.ID),
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
                    !a_validator.Bounded(item.Intensity, 0.0, a_charGenMenu ? 0.5 : 1.0, "tint layer intensity")) {
                    return false;
                }
                PresetTintLayer decoded{
                    .name = item.Name,
                    .value = item.Value.Value,
                    .modulationValue = item.ModulationValue.Value,
                    .intensity = item.Intensity };
                if (a_charGenMenu) {
                    // CharGenMenu writes 1/128-quantized intensities that the runtime consumes on a 1/64 scale; 
                    const auto scaled = decoded.intensity * kCharGenMenuTintScale;
                    const auto packed = std::round(scaled);
                    if (std::abs(scaled - packed) > kTintQuantizationTolerance) {
                        return a_validator.Fail("invalid_char_gen_tint_intensity", "CharGenMenu tint intensity must be a 1/128-quantized value from 0 to 0.5");
                    }
                    decoded.intensity = packed / kRuntimeTintScale;
                }
                a_out.push_back(std::move(decoded));
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
            !validator.Str(document.TeethCustomization, "TeethCustomization") ||
            !validator.Index(document.SkinTone, 255, "SkinTone")) {
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
        preset.skinTone = static_cast<std::uint32_t>(document.SkinTone);

        // A CharGenMenu export carries no editor ID; that is what distinguishes it from a Creation Kit preset and it changes the tint contract below.
        const bool isCharGenMenu = preset.npcFormEditorID.empty();

        if (document.Sex == "Female") {
            preset.sex = PresetSex::kFemale;
        } else if (document.Sex == "Male") {
            preset.sex = PresetSex::kMale;
        } else {
            validator.Fail("unsupported_value", "Sex must be exactly 'Male' or 'Female'");
            return result;
        }

        if (document.BodyMorphRegionValuesA.size() != kBodyMorphRegionCount) {
            validator.Fail("count_out_of_range", "BodyMorphRegionValuesA must contain exactly five values for CK 1.16.244");
            return result;
        }
        preset.bodyMorphRegionValues.reserve(kBodyMorphRegionCount);
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

    PresetResult LoadCkPreset(const std::filesystem::path& a_path)
    {
        PresetResult result;
        auto extension = a_path.extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char a_ch) {
            return static_cast<char>(std::tolower(a_ch));
        });
        if (extension != ".npc") {
            result.issues.push_back({ a_path, 0, "invalid_extension", "preset path must use the .npc extension" });
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
        return ParseCkPreset(bytes, a_path);
    }
}
