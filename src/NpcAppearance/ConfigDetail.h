#pragma once

#include "NpcAppearance/Config.h"
#include "NpcAppearance/JsonSchema.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Shared internals of the package-configuration pipeline
// ConfigDetail.cpp (validation primitives), ManifestParser.cpp (JSON schema walks),
// PackageDiscovery.cpp (filesystem scanning), and Selection.cpp (conflict resolution).
namespace NpcAppearance
{
    inline constexpr std::size_t kMaxManifestBytes = 1024 * 1024; // 1 MiB
    inline constexpr std::size_t kMaxAssignments = 1024;
    inline constexpr std::size_t kMaxPackages = 1024;
    inline constexpr std::size_t kMaxRequirements = 256;
    inline constexpr std::int32_t kMinPriority = -1'000'000;
    inline constexpr std::int32_t kMaxPriority = 1'000'000;

    // Parser entry points behind DiscoverPackages; exposed here for the
    // host-side test suites.
    [[nodiscard]] ManifestResult ParsePackageManifest(
        std::string_view a_json,
        const std::filesystem::path& a_manifestPath,
        bool a_requirePresetFiles);

    [[nodiscard]] ManifestResult LoadPackageManifest(
        const std::filesystem::path& a_manifestPath,
        bool a_requirePresetFiles = true);
}

namespace NpcAppearance::Detail
{
    [[nodiscard]] std::string FoldASCII(std::string_view a_text);

    void AddIssue(ManifestResult& a_result, const std::filesystem::path& a_path,
                  std::size_t a_offset, std::string a_code, std::string a_message);

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

    [[nodiscard]] bool ParseRequirements(const Schema::Requirements& a_node,
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
