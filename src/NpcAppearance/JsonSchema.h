#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>

// Wire shapes of the package manifest, the preset-metadata sidecar, and the CK
// preset. Glaze reflects these directly, so everything structural -- malformed
// JSON, unknown keys, missing keys, wrong types, trailing data -- is the
// library's job and surfaces as one formatted error naming the line, column and
// offending token. Anything left over is a domain rule and lives in the parser.
//
// These types need external linkage: Glaze derives member names by reflection
// and cannot see into an anonymous namespace. Member names match their JSON
// keys exactly; the few keys that are not valid C++ identifiers ("$schema") or
// are keywords ("requires") are mapped through glz::meta below.
namespace NpcAppearance::Schema
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
        // Read as signed so an out-of-range value is caught by the range check
        // instead of silently wrapping into an unsigned type.
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

    struct TintLayer
    {
        double Intensity{ 0.0 };
        ValueWrapper ModulationValue;
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

    // Reject unknown keys (Glaze's default), require every non-optional member,
    // and refuse anything but whitespace after the root value.
    struct ParseOpts : glz::opts
    {
        bool validate_trailing_whitespace = true;
    };

    consteval ParseOpts MakeParseOpts()
    {
        ParseOpts opts{};
        opts.error_on_missing_keys = true;
        // ParseCkPreset and ParsePackageManifest take a std::string_view, which
        // carries no guarantee of a terminator, so Glaze must stay inside the
        // view's bounds rather than scanning for a NUL.
        opts.null_terminated = false;
        return opts;
    }

    inline constexpr auto kParseOpts = MakeParseOpts();

    // Glaze already distinguishes an unknown key from a missing one from a type
    // mismatch, so keep reporting the issue codes the rest of the mod (and its
    // diagnostics output) already speaks instead of flattening everything to
    // "invalid_json". a_fallback names the syntax-error code, which differs
    // between the manifest and the preset-metadata sidecar.
    // Glaze reads `null` into an optional member as "absent" and reports no
    // error, which would make `"requires": null` silently drop a pack's
    // dependency gate rather than fail. Neither the manifest nor the sidecar
    // schema has a nullable field anywhere, so a null is always an authoring
    // mistake and the whole document is rejected. Only these two small config
    // files need the check -- CkPreset declares no optional members, so a null
    // there is already a type error.
    [[nodiscard]] inline bool ContainsNull(const glz::generic& a_node)
    {
        if (a_node.is_null()) {
            return true;
        }
        if (a_node.is_object()) {
            for (const auto& [name, child] : a_node.get_object()) {
                if (ContainsNull(child)) {
                    return true;
                }
            }
            return false;
        }
        if (a_node.is_array()) {
            for (const auto& child : a_node.get_array()) {
                if (ContainsNull(child)) {
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] inline bool HasNullValue(const std::string_view a_json)
    {
        glz::generic document;
        if (glz::read<kParseOpts>(document, a_json)) {
            return false; // Already rejected by the typed read; report that error.
        }
        return ContainsNull(document);
    }

    [[nodiscard]] inline const char* IssueCodeFor(const glz::error_ctx& a_error,
                                                  const std::string_view a_json,
                                                  const char* a_fallback) noexcept
    {
        switch (a_error.ec) {
        case glz::error_code::unknown_key:
            return "unknown_property";
        case glz::error_code::missing_key:
            return "missing_property";
        // Glaze reports expected_quote both for a scalar where the schema
        // declares a string and for a document that simply ran out; only the
        // truncation stops at the end of the input.
        case glz::error_code::expected_quote:
            return a_error.count >= a_json.size() ? a_fallback : "wrong_type";
        // A brace or bracket is only "expected" when a scalar turned up where
        // the schema declares an object or an array.
        case glz::error_code::parse_number_failure:
        case glz::error_code::expected_brace:
        case glz::error_code::expected_bracket:
            return "wrong_type";
        default:
            return a_fallback;
        }
    }
}

template <>
struct glz::meta<NpcAppearance::Schema::Assignment>
{
    using T = NpcAppearance::Schema::Assignment;
    static constexpr auto value = object(
        "target", &T::target,
        "preset", &T::preset,
        "requires", &T::requirements);
};

template <>
struct glz::meta<NpcAppearance::Schema::Manifest>
{
    using T = NpcAppearance::Schema::Manifest;
    static constexpr auto value = object(
        "$schema", &T::schemaHint,
        "schemaVersion", &T::schemaVersion,
        "priority", &T::priority,
        "requires", &T::requirements,
        "assignments", &T::assignments);
};

template <>
struct glz::meta<NpcAppearance::Schema::PresetMetadata>
{
    using T = NpcAppearance::Schema::PresetMetadata;
    static constexpr auto value = object(
        "$schema", &T::schemaHint,
        "schemaVersion", &T::schemaVersion,
        "requires", &T::requirements);
};
