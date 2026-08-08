#include "NpcAppearance/Config.h"

#include "NpcAppearance/ConfigDetail.h"
#include "NpcAppearance/Json.h"

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
        using JsonValue = Json::Value;

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

            JsonValue root;
            Json::Reader reader{ a_json, kManifestJsonLimits };
            if (!reader.Parse(root)) {
                AddIssue(diagnostics, a_path, reader.ErrorOffset(),
                         "invalid_preset_metadata_json", reader.Error());
            } else if (root.kind != JsonValue::Kind::kObject) {
                AddIssue(diagnostics, a_path, root.offset, "wrong_type",
                         "preset metadata root must be an object");
            } else if (HasOnlyProperties(
                           root, { "$schema", "schemaVersion", "requires" },
                           diagnostics, a_path)) {
                if (const auto* schemaHint = root.Find("$schema");
                    schemaHint && schemaHint->kind != JsonValue::Kind::kString) {
                    AddIssue(diagnostics, a_path, schemaHint->offset, "wrong_type",
                             "property '$schema' has the wrong type");
                } else {
                    const auto* schema = Require(
                        root, "schemaVersion", JsonValue::Kind::kNumber,
                        diagnostics, a_path);
                    const auto* requirementsNode = root.Find("requires");
                    Requirements requirements;
                    if (schema) {
                        if (schema->integer != 1) {
                            AddIssue(diagnostics, a_path, schema->offset,
                                     "unsupported_preset_metadata_schema",
                                     "unsupported preset metadata schemaVersion " +
                                         std::to_string(schema->integer));
                        } else if (!requirementsNode || ParseRequirementsNode(
                                   *requirementsNode, requirements,
                                   diagnostics, a_path)) {
                            result.requirements = std::move(requirements);
                        }
                    }
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

        JsonValue root;
        Json::Reader reader{ a_json, kManifestJsonLimits };
        if (!reader.Parse(root)) {
            AddIssue(result, a_manifestPath, reader.ErrorOffset(), "invalid_json", reader.Error());
            return result;
        }
        if (root.kind != JsonValue::Kind::kObject) {
            AddIssue(result, a_manifestPath, root.offset, "wrong_type", "manifest root must be an object");
            return result;
        }
        if (!HasOnlyProperties(root, { "$schema", "schemaVersion", "priority",
                                       "requires", "assignments" },
                                result, a_manifestPath)) {
            return result;
        }
        if (const auto* schemaHint = root.Find("$schema");
            schemaHint && schemaHint->kind != JsonValue::Kind::kString) {
            AddIssue(result, a_manifestPath, schemaHint->offset, "wrong_type",
                     "property '$schema' has the wrong type");
            return result;
        }

        const auto* schema = Require(root, "schemaVersion", JsonValue::Kind::kNumber, result, a_manifestPath);
        const auto* priority = root.Find("priority");
        const auto* requirementsNode = root.Find("requires");
        const auto* assignments = root.Find("assignments");
        if (!schema) {
            return result;
        }
        if (priority && priority->kind != JsonValue::Kind::kNumber) {
            AddIssue(result, a_manifestPath, priority->offset, "wrong_type",
                     "property 'priority' has the wrong type");
            return result;
        }
        if (requirementsNode && requirementsNode->kind != JsonValue::Kind::kObject) {
            AddIssue(result, a_manifestPath, requirementsNode->offset, "wrong_type",
                     "property 'requires' has the wrong type");
            return result;
        }
        if (assignments && assignments->kind != JsonValue::Kind::kArray) {
            AddIssue(result, a_manifestPath, assignments->offset, "wrong_type",
                     "property 'assignments' must be an array");
            return result;
        }
        if (schema->integer != 1) {
            AddIssue(result, a_manifestPath, schema->offset, "unsupported_schema",
                     "unsupported schemaVersion " + std::to_string(schema->integer));
            return result;
        }
        if (priority &&
            (priority->integer < kMinPriority || priority->integer > kMaxPriority)) {
            AddIssue(result, a_manifestPath, priority->offset, "invalid_priority", "priority is outside the accepted range");
            return result;
        }
        if (assignments &&
            (assignments->array.empty() || assignments->array.size() > kMaxAssignments)) {
            AddIssue(result, a_manifestPath, assignments->offset, "invalid_assignment_count",
                     "assignments must contain 1-1024 entries");
            return result;
        }
        PackageManifest manifest;
        manifest.schemaVersion = 1;
        manifest.packageID = a_manifestPath.parent_path().filename().string();
        manifest.priority = priority ? static_cast<std::int32_t>(priority->integer) : 0;
        manifest.manifestPath = a_manifestPath;
        manifest.format = assignments ? PackageFormat::kExplicitAssignments :
                                        PackageFormat::kPluginFolderLocalFormID;

        if (requirementsNode) {
            if (!ParseRequirementsNode(
                    *requirementsNode, manifest.requirements, result, a_manifestPath)) {
                return result;
            }
        }

        if (assignments) {
            std::unordered_set<std::string> targets;
            for (const auto& rawAssignment : assignments->array) {
                if (rawAssignment.kind != JsonValue::Kind::kObject ||
                    !HasOnlyProperties(rawAssignment, { "target", "preset", "requires" },
                                       result, a_manifestPath)) {
                    if (rawAssignment.kind != JsonValue::Kind::kObject) {
                        AddIssue(result, a_manifestPath, rawAssignment.offset, "wrong_type",
                                 "assignment must be an object");
                    }
                    return result;
                }
                const auto* target = Require(rawAssignment, "target", JsonValue::Kind::kObject,
                                             result, a_manifestPath);
                const auto* preset = Require(rawAssignment, "preset", JsonValue::Kind::kString,
                                             result, a_manifestPath);
                if (!target || !preset ||
                    !HasOnlyProperties(*target, { "plugin", "localFormId" },
                                       result, a_manifestPath)) {
                    return result;
                }

                Assignment assignment;
                const auto* plugin = Require(
                    *target, "plugin", JsonValue::Kind::kString, result, a_manifestPath);
                const auto* localFormID = Require(
                    *target, "localFormId", JsonValue::Kind::kString, result, a_manifestPath);
                if (!plugin || !localFormID) {
                    return result;
                }
                if (!IsPluginName(plugin->string)) {
                    AddIssue(result, a_manifestPath, plugin->offset, "invalid_plugin",
                             "target plugin name is invalid");
                    return result;
                }
                std::uint32_t parsedLocalFormID = 0;
                if (!ParseLocalFormID(localFormID->string, parsedLocalFormID)) {
                    AddIssue(result, a_manifestPath, localFormID->offset,
                             "invalid_local_form_id",
                             "localFormId must be 1-8 hexadecimal digits no greater than 00FFFFFF and must not use a 0x prefix");
                    return result;
                }
                assignment.target = Target{ plugin->string, parsedLocalFormID };
                Requirements assignmentRequirements;
                if (const auto* assignmentRequirementsNode = rawAssignment.Find("requires")) {
                    if (!ParseRequirementsNode(*assignmentRequirementsNode,
                                               assignmentRequirements, result,
                                               a_manifestPath)) {
                        return result;
                    }
                }
                std::string requirementsError;
                if (!MergeRequirements(manifest.requirements, assignmentRequirements,
                                       assignment.requirements,
                                       requirementsError, plugin->string)) {
                    AddIssue(result, a_manifestPath, rawAssignment.offset,
                             "effective_requirements_invalid", requirementsError);
                    return result;
                }
                std::string presetError;
                if (!ResolvePresetPath(a_manifestPath, preset->string, a_requirePresetFiles,
                                       assignment.presetPath, presetError)) {
                    AddIssue(result, a_manifestPath, preset->offset, "invalid_preset", presetError);
                    return result;
                }
                const auto targetKey = assignment.target.CanonicalKey();
                if (!targets.insert(targetKey).second) {
                    AddIssue(result, a_manifestPath, target->offset, "duplicate_target",
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
