#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Probe::NpcAppearance
{
    inline constexpr std::size_t kMaxManifestBytes = 1024 * 1024;
    inline constexpr std::size_t kMaxAssignments = 1024;
    inline constexpr std::size_t kMaxPackages = 1024;
    inline constexpr std::size_t kMaxRequirements = 256;
    inline constexpr std::uintmax_t kMaxPresetBytes = 32 * 1024 * 1024;
    inline constexpr std::int32_t kMinPriority = -1'000'000;
    inline constexpr std::int32_t kMaxPriority = 1'000'000;
    inline constexpr std::string_view kPluginFolderLocalFormIDConvention =
        "pluginFolderLocalFormId";

    struct Target
    {
        std::string plugin;
        std::uint32_t localFormID{ 0 };

        [[nodiscard]] std::string CanonicalKey() const;
    };

    enum class PluginTier
    {
        kFull,
        kMedium,
        kSmall
    };

    [[nodiscard]] bool IsLocalFormIDValidForTier(
        std::uint32_t a_localFormID,
        PluginTier a_tier) noexcept;

    struct Requirements
    {
        std::vector<std::string> plugins;
        std::vector<std::filesystem::path> assets;
    };

    enum class AppearanceScope
    {
        kFaceAndBody
    };

    struct Assignment
    {
        Target target;
        std::filesystem::path presetPath;
        Requirements requirements;
        AppearanceScope scope{ AppearanceScope::kFaceAndBody };
    };

    enum class PackageFormat
    {
        kExplicitAssignments,
        kPluginFolderLocalFormID
    };

    struct PackageManifest
    {
        std::uint32_t schemaVersion{ 0 };
        std::string packageID;
        std::int32_t priority{ 0 };
        Requirements requirements;
        std::vector<Assignment> assignments;
        std::filesystem::path manifestPath;
        PackageFormat format{ PackageFormat::kExplicitAssignments };
        bool implicitManifest{ false };

        // Package root directory. For an implicit package `manifestPath` names the
        // manifest the author never wrote, so it is only ever an anchor.
        [[nodiscard]] std::filesystem::path PackageRoot() const
        {
            return manifestPath.parent_path();
        }

        [[nodiscard]] std::filesystem::path DiagnosticPath() const
        {
            return implicitManifest ? manifestPath.parent_path() : manifestPath;
        }
    };

    struct ManifestIssue
    {
        std::filesystem::path path;
        std::size_t offset{ 0 };
        std::string code;
        std::string message;
    };

    struct ManifestResult
    {
        std::optional<PackageManifest> manifest;
        std::vector<ManifestIssue> issues;

        [[nodiscard]] bool HasFatalError() const noexcept { return !manifest.has_value(); }
    };

    struct DiscoveryResult
    {
        std::vector<PackageManifest> packages;
        std::vector<ManifestIssue> issues;
    };

    struct AssetRequirementResult
    {
        std::vector<std::filesystem::path> missing;

        [[nodiscard]] bool Complete() const noexcept { return missing.empty(); }
    };

    struct SelectedAssignment
    {
        Target target;
        std::filesystem::path presetPath;
        Requirements requirements;
        std::string packageID;
        std::int32_t priority{ 0 };
    };

    struct ConflictDecision
    {
        std::string targetKey;
        std::string packageID;
        std::int32_t priority{ 0 };
        bool winner{ false };
        std::string reason;
    };

    struct SelectionResult
    {
        std::vector<SelectedAssignment> winners;
        std::vector<ConflictDecision> decisions;
    };

    [[nodiscard]] ManifestResult ParsePackageManifest(
        std::string_view a_json,
        const std::filesystem::path& a_manifestPath,
        bool a_requirePresetFiles);

    [[nodiscard]] ManifestResult LoadPackageManifest(
        const std::filesystem::path& a_manifestPath,
        bool a_requirePresetFiles = true);

    [[nodiscard]] DiscoveryResult DiscoverPackages(
        const std::filesystem::path& a_packagesRoot,
        bool a_requirePresetFiles = true);

    [[nodiscard]] AssetRequirementResult CheckRequiredAssets(
        const PackageManifest& a_package,
        const std::filesystem::path& a_dataRoot);

    [[nodiscard]] AssetRequirementResult CheckRequiredAssets(
        const Requirements& a_requirements,
        const std::filesystem::path& a_dataRoot);

    [[nodiscard]] SelectionResult SelectAssignments(
        const std::vector<PackageManifest>& a_packages);
}
