#pragma once

#include <glaze/glaze.hpp>

// schema for json files (package manifest, preset metadata, and CK preset)
// member names match JSON keys exactly
namespace Config::Schema
{
    struct Requirements
    {
        std::optional<std::vector<std::string>> plugins;
        std::optional<std::vector<std::string>> assets;
    };

    struct Target
    {
        std::string plugin;
        std::string localFormId;
    };

    struct Assignment
    {
        Target target;
        std::string preset;
        std::optional<Requirements> requirements;
    };

    struct Manifest
    {
        std::optional<std::string> schemaHint;
        std::int64_t schemaVersion{ 0 };
        std::optional<std::int64_t> priority;
        std::optional<Requirements> requirements;
        std::optional<std::vector<Assignment>> assignments;
    };

    struct PresetMetadata
    {
        std::optional<std::string> schemaHint;
        std::int64_t schemaVersion{ 0 };
        std::optional<Requirements> requirements;
    };

    struct Weights
    {
        double x{ 0.0 };
        double y{ 0.0 };
        double z{ 0.0 };
    };

    struct NamedMorph
    {
        std::string Name;
        double Value{ 0.0 };
    };

    struct BoneSlider
    {
        std::string GroupName;
        // Read as signed so an out-of-range value is caught by the range check instead of silently wrapping into an unsigned type.
        std::int64_t ID{ 0 };
        double Value{ 0.0 };
    };

    struct BoneRegion
    {
        std::int64_t RegionID{ 0 };
        std::vector<BoneSlider> SlidersA;
    };

    struct ValueWrapper
    {
        std::string Value;
    };

    struct CustomColor
    {
        // Read as signed so invalid negative and overflowing channel values are rejected explicitly before conversion to the engine's byte channels.
        std::int64_t Blue{ 0 };
        std::int64_t Green{ 0 };
        std::int64_t Red{ 0 };
        std::int64_t Rough{ 0 };
    };

    struct Modulation
    {
        std::string Value;
        std::optional<CustomColor> CustomColorValue;
    };

    struct TintLayer
    {
        double Intensity{ 0.0 };
        Modulation ModulationValue;
        std::string Name;
        ValueWrapper Value;
    };

    struct PostBlend
    {
        std::vector<TintLayer> LayersA;
    };

    struct CkPreset
    {
        std::vector<double> BodyMorphRegionValuesA;
        std::string BrowHairColor;
        std::string EyeColor;
        std::vector<BoneRegion> FacialBoneRegionDataA;
        std::string FacialHairColor;
        std::vector<NamedMorph> FacialMorphSliderDataA;
        std::string HairColor;
        std::string JewelryColor;
        std::vector<std::string> MiscHeadPartsA;
        Weights MorphWeights;
        std::string NPCFormEditorID;
        PostBlend PostBlendFaceCustomization;
        std::string RaceFormID;
        std::string Sex;
        std::int64_t SkinTone{ 0 };
        std::string TeethCustomization;
        std::vector<std::string> UniqueHeadPartsA;
    };

    // Reject unknown keys (Glaze's default), require every non-optional member, and refuse anything but whitespace after the root value.
    struct ParseOpts : glz::opts
    {
        bool validate_trailing_whitespace = true;
    };

    consteval ParseOpts MakeParseOpts()
    {
        ParseOpts opts{};
        opts.error_on_missing_keys = true;
        // ParseCkPreset and ParsePackageManifest take a std::string_view, which may not be null-terminated.
        opts.null_terminated = false;
        return opts;
    }

    inline constexpr auto kParseOpts = MakeParseOpts();
}

template <>
struct glz::meta<Config::Schema::Assignment>
{
    using T = Config::Schema::Assignment;
    static constexpr auto value = object(
        "target", &T::target,
        "preset", &T::preset,
        "requires", &T::requirements);
};

template <>
struct glz::meta<Config::Schema::Manifest>
{
    using T = Config::Schema::Manifest;
    static constexpr auto value = object(
        "$schema", &T::schemaHint,
        "schemaVersion", &T::schemaVersion,
        "priority", &T::priority,
        "requires", &T::requirements,
        "assignments", &T::assignments);
};

template <>
struct glz::meta<Config::Schema::PresetMetadata>
{
    using T = Config::Schema::PresetMetadata;
    static constexpr auto value = object(
        "$schema", &T::schemaHint,
        "schemaVersion", &T::schemaVersion,
        "requires", &T::requirements);
};
