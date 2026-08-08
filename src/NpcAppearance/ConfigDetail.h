#pragma once

#include "NpcAppearance/Config.h"
#include "NpcAppearance/Json.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Shared internals of the package-configuration pipeline, split across
// ConfigDetail.cpp (validation primitives), ManifestParser.cpp (JSON schema
// walks), PackageDiscovery.cpp (filesystem scanning), and Selection.cpp
// (conflict resolution). Nothing here is part of the plugin's public surface;
// Config.h is.
namespace NpcAppearance::Detail
{
    // Manifest and preset-metadata JSON is integer-only and bounded by the
    // package limits declared in Config.h.
    inline constexpr Json::ReaderLimits kManifestJsonLimits{
        .maxArrayElements = kMaxAssignments,
        .maxObjectProperties = 128,
        .maxTotalNodes = 32768,
        .integersOnly = true,
    };

    [[nodiscard]] std::string FoldASCII(std::string_view a_text);

    void AddIssue(ManifestResult& a_result, const std::filesystem::path& a_path,
                  std::size_t a_offset, std::string a_code, std::string a_message);

    [[nodiscard]] bool HasOnlyProperties(const Json::Value& a_object,
                                         std::initializer_list<std::string_view> a_allowed,
                                         ManifestResult& a_result,
                                         const std::filesystem::path& a_path);

    [[nodiscard]] const Json::Value* Require(const Json::Value& a_object, std::string_view a_name,
                                             Json::Value::Kind a_kind, ManifestResult& a_result,
                                             const std::filesystem::path& a_path);

    [[nodiscard]] bool IsPluginName(std::string_view a_name);

    [[nodiscard]] bool ParseLocalFormID(std::string_view a_text, std::uint32_t& a_out);

    [[nodiscard]] bool IsWithin(const std::filesystem::path& a_root,
                                const std::filesystem::path& a_candidate);

    [[nodiscard]] bool ValidateRelativePath(const std::string& a_text,
                                            std::filesystem::path& a_out,
                                            std::string& a_error);

    [[nodiscard]] bool ResolvePresetPath(const std::filesystem::path& a_manifestPath,
                                         const std::string& a_text,
                                         bool a_requireFile,
                                         std::filesystem::path& a_out,
                                         std::string& a_error);

    [[nodiscard]] bool ParseRequirementsNode(const Json::Value& a_node,
                                             Requirements& a_requirements,
                                             ManifestResult& a_result,
                                             const std::filesystem::path& a_path);

    [[nodiscard]] bool MergeRequirements(const Requirements& a_package,
                                         const Requirements& a_assignment,
                                         Requirements& a_out,
                                         std::string& a_error,
                                         std::string_view a_implicitPlugin = {});

    struct PresetMetadataResult
    {
        std::optional<Requirements> requirements;
        std::vector<ManifestIssue> issues;
    };

    // Defined in ManifestParser.cpp; consumed by the convention-pack scan.
    [[nodiscard]] PresetMetadataResult LoadPresetMetadata(
        const std::filesystem::path& a_path,
        const std::filesystem::path& a_packageRoot);

    // Defined in PackageDiscovery.cpp; consumed by ParsePackageManifest when a
    // manifest declares no explicit assignments.
    void DiscoverConventionAssignments(PackageManifest& a_manifest,
                                       ManifestResult& a_result,
                                       bool a_requirePresetFiles);
}
