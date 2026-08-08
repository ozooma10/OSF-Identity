#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NpcAppearance
{
    inline constexpr std::uintmax_t kMaxPresetBytes = 32 * 1024 * 1024; // 32 MiB

    enum class PresetSex
    {
        kMale,
        kFemale
    };

    struct PresetMorphWeights
    {
        double x{ 0.0 };
        double y{ 0.0 };
        double z{ 0.0 };

        bool operator==(const PresetMorphWeights&) const = default;
    };

    struct PresetNamedMorph
    {
        std::string name;
        double value{ 0.0 };

        bool operator==(const PresetNamedMorph&) const = default;
    };

    struct PresetBoneSlider
    {
        std::string groupName;
        std::uint32_t id{ 0 };
        double value{ 0.0 };

        bool operator==(const PresetBoneSlider&) const = default;
    };

    struct PresetBoneRegion
    {
        std::uint32_t regionID{ 0 };
        std::vector<PresetBoneSlider> sliders;

        bool operator==(const PresetBoneRegion&) const = default;
    };

    struct PresetTintLayer
    {
        std::string name;
        std::string value;
        std::string modulationValue;
        double intensity{ 0.0 };

        bool operator==(const PresetTintLayer&) const = default;
    };

    struct AppearancePreset
    {
        std::string npcFormEditorID;
        std::string raceFormID;
        PresetSex sex{ PresetSex::kFemale };
        std::uint32_t skinTone{ 0 };
        std::string browHairColor;
        std::string eyeColor;
        std::string facialHairColor;
        std::string hairColor;
        std::string jewelryColor;
        std::string teethCustomization;
        PresetMorphWeights morphWeights;
        std::vector<double> bodyMorphRegionValues;
        std::vector<std::string> miscHeadParts;
        std::vector<std::string> uniqueHeadParts;
        std::vector<PresetNamedMorph> facialMorphSliders;
        std::vector<PresetBoneRegion> facialBoneRegions;
        std::vector<PresetTintLayer> postBlendLayers;

        bool operator==(const AppearancePreset&) const = default;
    };

    struct PresetIssue
    {
        std::filesystem::path path;
        std::size_t offset{ 0 };
        std::string code;
        std::string message;
    };

    struct PresetResult
    {
        std::optional<AppearancePreset> preset;
        std::vector<PresetIssue> issues;

        [[nodiscard]] bool HasFatalError() const noexcept { return !preset.has_value(); }
    };

    [[nodiscard]] PresetResult ParseCkPreset(
        std::string_view a_json,
        const std::filesystem::path& a_path = {});

    [[nodiscard]] PresetResult LoadCkPreset(const std::filesystem::path& a_path);
}
