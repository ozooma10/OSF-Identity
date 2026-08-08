#include "NpcAppearance/Config.h"

#include "NpcAppearance/ConfigDetail.h"
#include "NpcAppearance/JsonSchema.h"

#include <fstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace NpcAppearance
{
    using namespace Detail;

    namespace
    {
        [[nodiscard]] PresetMetadataResult ParsePresetMetadata(
            const std::string_view a_json,
            const std::filesystem::path& a_path) try
        {
            PresetMetadataResult result;
            ManifestResult diagnostics;
            if (a_json.size() > kMaxManifestBytes) {
                AddIssue(diagnostics, a_path, 0, "preset_metadata_too_large",
                         "preset metadata exceeds 1 MiB safety limit");
                result.issues = std::move(diagnostics.issues);
                return result;
            }

            Schema::PresetMetadata document;
            if (const auto ec = glz::read<Schema::kParseOpts>(document, a_json); ec) {
                AddIssue(diagnostics, a_path, ec.count,
                         Schema::IssueCodeFor(ec, a_json, "invalid_preset_metadata_json"),
                         glz::format_error(ec, a_json));
            } else if (Schema::HasNullValue(a_json)) {
                AddIssue(diagnostics, a_path, 0, "wrong_type",
                         "null is not a valid value anywhere in preset metadata; omit the property instead");
            } else if (document.schemaVersion != 1) {
                AddIssue(diagnostics, a_path, 0, "unsupported_preset_metadata_schema",
                         "unsupported preset metadata schemaVersion " +
                             std::to_string(document.schemaVersion));
            } else {
                Requirements requirements;
                if (!document.requirements ||
                    ParseRequirements(*document.requirements, requirements, diagnostics, a_path)) {
                    result.requirements = std::move(requirements);
                }
            }
            result.issues = std::move(diagnostics.issues);
            return result;
        }
        catch (const std::exception& e)
        {
            PresetMetadataResult result;
            try {
                result.issues.push_back({
                    a_path, 0, "preset_metadata_parser_exception",
                    "preset metadata parser exception: " + std::string{ e.what() } });
            } catch (...) {
            }
            return result;
        }
        catch (...)
        {
            PresetMetadataResult result;
            try {
                result.issues.push_back({
                    a_path, 0, "preset_metadata_parser_exception",
                    "preset metadata parser unknown exception" });
            } catch (...) {
            }
            return result;
        }
    }

    namespace Detail
    {
        PresetMetadataResult LoadPresetMetadata(
            const std::filesystem::path& a_path,
            const std::filesystem::path& a_packageRoot)
        {
            PresetMetadataResult result;
            std::error_code ec;
            const auto canonicalRoot = std::filesystem::weakly_canonical(a_packageRoot, ec);
            if (ec) {
                result.issues.push_back({ a_path, 0, "preset_metadata_read_failed",
                    "could not canonicalize pack directory: " + ec.message() });
                return result;
            }
            const auto canonicalPath = std::filesystem::weakly_canonical(a_path, ec);
            if (ec || !IsWithin(canonicalRoot, canonicalPath)) {
                result.issues.push_back({ a_path, 0, "preset_metadata_path_escape",
                    "preset metadata resolves outside the pack directory" });
                return result;
            }
            const auto size = std::filesystem::file_size(canonicalPath, ec);
            if (ec || size > kMaxManifestBytes) {
                result.issues.push_back({ a_path, 0, "preset_metadata_read_failed",
                    ec ? "could not stat preset metadata: " + ec.message() :
                         "preset metadata exceeds 1 MiB safety limit" });
                return result;
            }
            std::ifstream stream{ canonicalPath, std::ios::binary };
            std::string text(static_cast<std::size_t>(size), '\0');
            if (!stream.is_open() ||
                (size != 0 && !stream.read(text.data(), static_cast<std::streamsize>(text.size())))) {
                result.issues.push_back({ a_path, 0, "preset_metadata_read_failed",
                    "could not read complete preset metadata" });
                return result;
            }
            return ParsePresetMetadata(text, canonicalPath);
        }
    }

    ManifestResult ParsePackageManifest(const std::string_view a_json,
                                        const std::filesystem::path& a_manifestPath,
                                        const bool a_requirePresetFiles) try
    {
        ManifestResult result;
        if (a_json.size() > kMaxManifestBytes) {
            AddIssue(result, a_manifestPath, 0, "manifest_too_large", "manifest exceeds 1 MiB safety limit");
            return result;
        }

        Schema::Manifest document;
        if (const auto ec = glz::read<Schema::kParseOpts>(document, a_json); ec) {
            AddIssue(result, a_manifestPath, ec.count, Schema::IssueCodeFor(ec, a_json, "invalid_json"),
                     glz::format_error(ec, a_json));
            return result;
        }
        if (Schema::HasNullValue(a_json)) {
            AddIssue(result, a_manifestPath, 0, "wrong_type",
                     "null is not a valid value anywhere in a manifest; omit the property instead");
            return result;
        }
        if (document.schemaVersion != 1) {
            AddIssue(result, a_manifestPath, 0, "unsupported_schema",
                     "unsupported schemaVersion " + std::to_string(document.schemaVersion));
            return result;
        }
        const auto priority = document.priority.value_or(0);
        if (priority < kMinPriority || priority > kMaxPriority) {
            AddIssue(result, a_manifestPath, 0, "invalid_priority",
                     "priority is outside the accepted range");
            return result;
        }
        if (document.assignments &&
            (document.assignments->empty() || document.assignments->size() > kMaxAssignments)) {
            AddIssue(result, a_manifestPath, 0, "invalid_assignment_count",
                     "assignments must contain 1-1024 entries");
            return result;
        }
        PackageManifest manifest;
        manifest.schemaVersion = 1;
        manifest.packageID = a_manifestPath.parent_path().filename().string();
        manifest.priority = static_cast<std::int32_t>(priority);
        manifest.manifestPath = a_manifestPath;
        manifest.format = document.assignments ? PackageFormat::kExplicitAssignments :
                                                 PackageFormat::kPluginFolderLocalFormID;

        if (document.requirements &&
            !ParseRequirements(*document.requirements, manifest.requirements, result,
                               a_manifestPath)) {
            return result;
        }

        if (document.assignments) {
            std::unordered_set<std::string> targets;
            for (const auto& rawAssignment : *document.assignments) {
                if (!IsPluginName(rawAssignment.target.plugin)) {
                    AddIssue(result, a_manifestPath, 0, "invalid_plugin",
                             "target plugin name is invalid");
                    return result;
                }
                std::uint32_t parsedLocalFormID = 0;
                if (!ParseLocalFormID(rawAssignment.target.localFormId, parsedLocalFormID)) {
                    AddIssue(result, a_manifestPath, 0, "invalid_local_form_id",
                             "localFormId must be 1-8 hexadecimal digits no greater than 00FFFFFF and must not use a 0x prefix");
                    return result;
                }
                Assignment assignment;
                assignment.target = Target{ rawAssignment.target.plugin, parsedLocalFormID };
                Requirements assignmentRequirements;
                if (rawAssignment.requirements &&
                    !ParseRequirements(*rawAssignment.requirements, assignmentRequirements,
                                       result, a_manifestPath)) {
                    return result;
                }
                std::string requirementsError;
                if (!MergeRequirements(manifest.requirements, assignmentRequirements,
                                       assignment.requirements, requirementsError,
                                       rawAssignment.target.plugin)) {
                    AddIssue(result, a_manifestPath, 0, "effective_requirements_invalid",
                             requirementsError);
                    return result;
                }
                std::string presetError;
                if (!ResolvePresetPath(a_manifestPath, rawAssignment.preset, a_requirePresetFiles,
                                       assignment.presetPath, presetError)) {
                    AddIssue(result, a_manifestPath, 0, "invalid_preset", presetError);
                    return result;
                }
                const auto targetKey = assignment.target.CanonicalKey();
                if (!targets.insert(targetKey).second) {
                    AddIssue(result, a_manifestPath, 0, "duplicate_target",
                             "pack contains more than one assignment for target " + targetKey);
                    return result;
                }
                manifest.assignments.push_back(std::move(assignment));
            }
        } else {
            DiscoverConventionAssignments(manifest, result, a_requirePresetFiles);
        }

        result.manifest = std::move(manifest);
        return result;
    }
    catch (const std::exception& e)
    {
        ManifestResult result;
        try {
            AddIssue(result, a_manifestPath, 0, "manifest_parser_exception",
                     "manifest parser exception: " + std::string{ e.what() });
        } catch (...) {
        }
        return result;
    }
    catch (...)
    {
        ManifestResult result;
        try {
            AddIssue(result, a_manifestPath, 0, "manifest_parser_exception",
                     "manifest parser unknown exception");
        } catch (...) {
        }
        return result;
    }

    ManifestResult LoadPackageManifest(const std::filesystem::path& a_manifestPath,
                                       const bool a_requirePresetFiles)
    {
        ManifestResult result;
        std::error_code ec;
        const auto size = std::filesystem::file_size(a_manifestPath, ec);
        if (ec || size > kMaxManifestBytes) {
            AddIssue(result, a_manifestPath, 0, "manifest_read_failed",
                     ec ? "could not stat manifest: " + ec.message() : "manifest exceeds 1 MiB safety limit");
            return result;
        }
        std::ifstream stream{ a_manifestPath, std::ios::binary };
        std::string text(static_cast<std::size_t>(size), '\0');
        if (!stream.is_open() || (size != 0 && !stream.read(text.data(), static_cast<std::streamsize>(text.size())))) {
            AddIssue(result, a_manifestPath, 0, "manifest_read_failed", "could not read complete manifest");
            return result;
        }
        return ParsePackageManifest(text, a_manifestPath, a_requirePresetFiles);
    }
}
