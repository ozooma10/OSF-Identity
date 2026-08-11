#pragma once

// Pack config pipeline: discover packs on disk, check their requirements, and pick a deterministic winner per resolved base form. 
namespace NpcAppearance
{
    struct Target
    {
        std::string plugin;
        std::uint32_t localFormID{ 0 };

        [[nodiscard]] std::string CanonicalKey() const;
    };

    struct Requirements
    {
        std::vector<std::string> plugins;
        std::vector<std::filesystem::path> assets;
    };

    struct Assignment
    {
        Target target;
        std::filesystem::path presetPath;
        Requirements requirements;
    };

    struct PackageManifest
    {
        std::string packageID;
        std::int32_t priority{ 0 };
        Requirements requirements;
        std::vector<Assignment> assignments;
        std::filesystem::path manifestPath;
        bool implicitManifest{ false };

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

    struct ResolvedAssignment
    {
        std::uint32_t baseFormID{ 0 };
        SelectedAssignment assignment;
    };

    struct ResolvedSelectionResult
    {
        std::vector<ResolvedAssignment> winners;
        std::vector<ConflictDecision> decisions;
        std::vector<std::string> rejectedPackages;
    };

    [[nodiscard]] DiscoveryResult DiscoverPackages(
        const std::filesystem::path& a_packsRoot,
        bool a_requirePresetFiles = true);

    [[nodiscard]] AssetRequirementResult CheckRequiredAssets(
        const Requirements& a_requirements,
        const std::filesystem::path& a_dataRoot);

    [[nodiscard]] ResolvedSelectionResult SelectResolvedAssignments(
        const std::vector<ResolvedAssignment>& a_candidates);
}
