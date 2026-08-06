#include "NpcAppearance/Config.h"

#include "NpcAppearance/Json.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cwchar>
#include <fstream>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NpcAppearance
{
    namespace
    {
        [[nodiscard]] char LowerASCII(const char a_ch) noexcept
        {
            return a_ch >= 'A' && a_ch <= 'Z' ? static_cast<char>(a_ch - 'A' + 'a') : a_ch;
        }

        [[nodiscard]] std::string FoldASCII(const std::string_view a_text)
        {
            std::string folded;
            folded.reserve(a_text.size());
            for (const char ch : a_text) {
                folded.push_back(LowerASCII(ch));
            }
            return folded;
        }

        using JsonValue = Json::Value;

        // Manifest and preset-metadata JSON is integer-only and bounded by the
        // package limits declared in Config.h.
        constexpr Json::ReaderLimits kManifestJsonLimits{
            .maxArrayElements = kMaxAssignments,
            .integersOnly = true,
        };

        void AddIssue(ManifestResult& a_result, const std::filesystem::path& a_path,
                      const std::size_t a_offset, std::string a_code, std::string a_message)
        {
            a_result.issues.push_back({ a_path, a_offset, std::move(a_code), std::move(a_message) });
        }

        [[nodiscard]] bool HasOnlyProperties(const JsonValue& a_object,
                                             const std::initializer_list<std::string_view> a_allowed,
                                             ManifestResult& a_result,
                                             const std::filesystem::path& a_path)
        {
            for (const auto& [name, value] : a_object.object) {
                if (std::ranges::find(a_allowed, name) == a_allowed.end()) {
                    AddIssue(a_result, a_path, value.offset, "unknown_property",
                             "unknown property '" + name + "'");
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] const JsonValue* Require(const JsonValue& a_object, const std::string_view a_name,
                                               const JsonValue::Kind a_kind, ManifestResult& a_result,
                                               const std::filesystem::path& a_path)
        {
            const auto* value = a_object.Find(a_name);
            if (!value) {
                AddIssue(a_result, a_path, a_object.offset, "missing_property",
                         "missing property '" + std::string(a_name) + "'");
                return nullptr;
            }
            if (value->kind != a_kind) {
                AddIssue(a_result, a_path, value->offset, "wrong_type",
                         "property '" + std::string(a_name) + "' has the wrong type");
                return nullptr;
            }
            return value;
        }

        [[nodiscard]] bool IsPluginName(const std::string_view a_name)
        {
            if (a_name.empty() || a_name.size() > 260 || a_name.contains('/') || a_name.contains('\\') ||
                a_name.contains(':')) {
                return false;
            }
            const auto folded = FoldASCII(a_name);
            return folded.ends_with(".esm") || folded.ends_with(".esp") || folded.ends_with(".esl");
        }

        [[nodiscard]] bool IsPackageID(const std::string_view a_id)
        {
            if (a_id.size() < 3 || a_id.size() > 128 || !std::isalnum(static_cast<unsigned char>(a_id.front()))) {
                return false;
            }
            return std::ranges::all_of(a_id, [](const unsigned char a_ch) {
                return (a_ch >= 'a' && a_ch <= 'z') || (a_ch >= '0' && a_ch <= '9') ||
                       a_ch == '.' || a_ch == '_' || a_ch == '-';
            });
        }

        [[nodiscard]] bool ParseLocalFormID(const std::string_view a_text, std::uint32_t& a_out)
        {
            if (a_text.empty() || a_text.size() > 8 || a_text.starts_with("0x") || a_text.starts_with("0X")) {
                return false;
            }
            std::uint32_t value = 0;
            const auto [ptr, ec] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), value, 16);
            if (ec != std::errc{} || ptr != a_text.data() + a_text.size() || value > 0x00FFFFFF) {
                return false;
            }
            a_out = value;
            return true;
        }

        [[nodiscard]] bool PathComponentEquals(const std::filesystem::path& a_left,
                                               const std::filesystem::path& a_right)
        {
#ifdef _WIN32
            return _wcsicmp(a_left.c_str(), a_right.c_str()) == 0;
#else
            return a_left == a_right;
#endif
        }

        [[nodiscard]] bool IsWithin(const std::filesystem::path& a_root,
                                    const std::filesystem::path& a_candidate)
        {
            auto rootIt = a_root.begin();
            auto candidateIt = a_candidate.begin();
            for (; rootIt != a_root.end(); ++rootIt, ++candidateIt) {
                if (candidateIt == a_candidate.end() || !PathComponentEquals(*rootIt, *candidateIt)) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool ValidateRelativePath(const std::string& a_text,
                                                std::filesystem::path& a_out,
                                                std::string& a_error)
        {
            if (a_text.empty()) {
                a_error = "path is empty";
                return false;
            }
            // Reject Windows-style roots (drive letters, leading separators)
            // explicitly so validation behaves identically on every host and
            // the host test suite is portable; std::filesystem only recognizes
            // them on Windows.
            if (a_text.contains(':') || a_text.front() == '/' || a_text.front() == '\\') {
                a_error = "path must be relative";
                return false;
            }
            const std::filesystem::path relative{ a_text };
            if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
                a_error = "path must be relative";
                return false;
            }
            for (const auto& component : relative) {
                if (component == "..") {
                    a_error = "path contains parent traversal";
                    return false;
                }
            }
            a_out = relative.lexically_normal();
            return true;
        }

        [[nodiscard]] bool ResolvePresetPath(const std::filesystem::path& a_manifestPath,
                                             const std::string& a_text,
                                             const bool a_requireFile,
                                             std::filesystem::path& a_out,
                                             std::string& a_error)
        {
            std::filesystem::path relative;
            if (!ValidateRelativePath(a_text, relative, a_error)) {
                return false;
            }
            if (FoldASCII(relative.extension().string()) != ".npc") {
                a_error = "preset path must use the .npc extension";
                return false;
            }

            std::error_code ec;
            const auto root = std::filesystem::absolute(a_manifestPath.parent_path(), ec).lexically_normal();
            if (ec) {
                a_error = "could not resolve package directory: " + ec.message();
                return false;
            }
            auto candidate = (root / relative).lexically_normal();
            if (!IsWithin(root, candidate)) {
                a_error = "preset path escapes the package directory";
                return false;
            }
            if (a_requireFile) {
                const auto canonicalRoot = std::filesystem::weakly_canonical(root, ec);
                if (ec) {
                    a_error = "could not canonicalize package directory: " + ec.message();
                    return false;
                }
                const auto canonicalCandidate = std::filesystem::weakly_canonical(candidate, ec);
                if (ec || !IsWithin(canonicalRoot, canonicalCandidate)) {
                    a_error = "preset path resolves outside the package directory";
                    return false;
                }
                if (!std::filesystem::is_regular_file(canonicalCandidate, ec) || ec) {
                    a_error = "preset file is missing or is not a regular file";
                    return false;
                }
                const auto size = std::filesystem::file_size(canonicalCandidate, ec);
                if (ec || size == 0 || size > kMaxPresetBytes) {
                    a_error = "preset file size is outside the accepted range";
                    return false;
                }
                candidate = canonicalCandidate;
            }
            a_out = std::move(candidate);
            return true;
        }

        [[nodiscard]] bool ParseStringArray(const JsonValue& a_value, const std::string_view a_property,
                                             const bool a_plugins, std::vector<std::string>& a_strings,
                                             std::vector<std::filesystem::path>& a_paths,
                                             ManifestResult& a_result, const std::filesystem::path& a_path)
        {
            if (a_value.kind != JsonValue::Kind::kArray) {
                AddIssue(a_result, a_path, a_value.offset, "wrong_type",
                         "property '" + std::string(a_property) + "' must be an array");
                return false;
            }
            if (a_value.array.size() > kMaxRequirements) {
                AddIssue(a_result, a_path, a_value.offset, "limit_exceeded",
                         "property '" + std::string(a_property) + "' exceeds the requirement limit");
                return false;
            }
            std::unordered_set<std::string> seen;
            for (const auto& item : a_value.array) {
                if (item.kind != JsonValue::Kind::kString) {
                    AddIssue(a_result, a_path, item.offset, "wrong_type",
                             "property '" + std::string(a_property) + "' must contain strings");
                    return false;
                }
                if (a_plugins) {
                    if (!IsPluginName(item.string)) {
                        AddIssue(a_result, a_path, item.offset, "invalid_plugin", "invalid required plugin name");
                        return false;
                    }
                    if (!seen.insert(FoldASCII(item.string)).second) {
                        AddIssue(a_result, a_path, item.offset, "duplicate_requirement", "duplicate required plugin");
                        return false;
                    }
                    a_strings.push_back(item.string);
                } else {
                    std::filesystem::path relative;
                    std::string error;
                    if (!ValidateRelativePath(item.string, relative, error)) {
                        AddIssue(a_result, a_path, item.offset, "invalid_asset_path", error);
                        return false;
                    }
                    if (!seen.insert(FoldASCII(relative.generic_string())).second) {
                        AddIssue(a_result, a_path, item.offset, "duplicate_requirement", "duplicate required asset");
                        return false;
                    }
                    a_paths.push_back(std::move(relative));
                }
            }
            return true;
        }

        [[nodiscard]] bool ParseRequirementsNode(
            const JsonValue& a_node,
            Requirements& a_requirements,
            ManifestResult& a_result,
            const std::filesystem::path& a_path)
        {
            if (a_node.kind != JsonValue::Kind::kObject) {
                AddIssue(a_result, a_path, a_node.offset, "wrong_type",
                         "requirements must be an object");
                return false;
            }
            if (!HasOnlyProperties(a_node, { "plugins", "assets" }, a_result, a_path)) {
                return false;
            }
            const auto* plugins = Require(
                a_node, "plugins", JsonValue::Kind::kArray, a_result, a_path);
            const auto* assets = Require(
                a_node, "assets", JsonValue::Kind::kArray, a_result, a_path);
            return plugins && assets &&
                ParseStringArray(*plugins, "plugins", true, a_requirements.plugins,
                                 a_requirements.assets, a_result, a_path) &&
                ParseStringArray(*assets, "assets", false, a_requirements.plugins,
                                 a_requirements.assets, a_result, a_path);
        }

        [[nodiscard]] bool MergeRequirements(
            const Requirements& a_package,
            const Requirements& a_assignment,
            const std::string_view a_targetPlugin,
            Requirements& a_out,
            std::string& a_error)
        {
            std::unordered_set<std::string> plugins;
            const auto addPlugin = [&](const std::string_view a_plugin) {
                const auto folded = FoldASCII(a_plugin);
                if (plugins.insert(folded).second) {
                    if (a_out.plugins.size() >= kMaxRequirements) {
                        a_error = "effective plugin requirements exceed the safety limit";
                        return false;
                    }
                    a_out.plugins.emplace_back(a_plugin);
                }
                return true;
            };
            for (const auto& plugin : a_package.plugins) {
                if (!addPlugin(plugin)) return false;
            }
            for (const auto& plugin : a_assignment.plugins) {
                if (!addPlugin(plugin)) return false;
            }
            if (!addPlugin(a_targetPlugin)) return false;

            std::unordered_set<std::string> assets;
            const auto addAsset = [&](const std::filesystem::path& a_asset) {
                const auto folded = FoldASCII(a_asset.generic_string());
                if (assets.insert(folded).second) {
                    if (a_out.assets.size() >= kMaxRequirements) {
                        a_error = "effective asset requirements exceed the safety limit";
                        return false;
                    }
                    a_out.assets.push_back(a_asset);
                }
                return true;
            };
            for (const auto& asset : a_package.assets) {
                if (!addAsset(asset)) return false;
            }
            for (const auto& asset : a_assignment.assets) {
                if (!addAsset(asset)) return false;
            }
            return true;
        }

        struct PresetMetadataResult
        {
            std::optional<Requirements> requirements;
            std::vector<ManifestIssue> issues;
        };

        [[nodiscard]] PresetMetadataResult ParsePresetMetadata(
            const std::string_view a_json,
            const std::filesystem::path& a_path)
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
                    const auto* requirementsNode = Require(
                        root, "requires", JsonValue::Kind::kObject,
                        diagnostics, a_path);
                    Requirements requirements;
                    if (schema && requirementsNode) {
                        if (schema->integer != 1) {
                            AddIssue(diagnostics, a_path, schema->offset,
                                     "unsupported_preset_metadata_schema",
                                     "unsupported preset metadata schemaVersion " +
                                         std::to_string(schema->integer));
                        } else if (ParseRequirementsNode(
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

        [[nodiscard]] PresetMetadataResult LoadPresetMetadata(
            const std::filesystem::path& a_path,
            const std::filesystem::path& a_packageRoot)
        {
            PresetMetadataResult result;
            std::error_code ec;
            const auto canonicalRoot = std::filesystem::weakly_canonical(a_packageRoot, ec);
            if (ec) {
                result.issues.push_back({ a_path, 0, "preset_metadata_read_failed",
                    "could not canonicalize package directory: " + ec.message() });
                return result;
            }
            const auto canonicalPath = std::filesystem::weakly_canonical(a_path, ec);
            if (ec || !IsWithin(canonicalRoot, canonicalPath)) {
                result.issues.push_back({ a_path, 0, "preset_metadata_path_escape",
                    "preset metadata resolves outside the package directory" });
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

        [[nodiscard]] bool EndsWithFolded(
            const std::string_view a_text,
            const std::string_view a_suffix)
        {
            const auto text = FoldASCII(a_text);
            const auto suffix = FoldASCII(a_suffix);
            return text.ends_with(suffix);
        }

        void DiscoverConventionAssignments(
            PackageManifest& a_manifest,
            ManifestResult& a_result,
            const bool a_requirePresetFiles)
        {
            const auto packageRoot = a_manifest.manifestPath.parent_path();
            const auto presetsRoot = packageRoot / "Presets";
            std::error_code ec;
            if (!std::filesystem::is_directory(presetsRoot, ec) || ec) {
                if (a_requirePresetFiles) {
                    AddIssue(a_result, presetsRoot, 0, "preset_root_missing",
                             "convention package Presets directory is missing");
                }
                return;
            }

            std::vector<std::filesystem::path> pluginDirectories;
            std::filesystem::directory_iterator rootIt{
                presetsRoot, std::filesystem::directory_options::skip_permission_denied, ec };
            const std::filesystem::directory_iterator end;
            for (; !ec && rootIt != end; rootIt.increment(ec)) {
                if (rootIt->is_directory(ec) && !ec) {
                    pluginDirectories.push_back(rootIt->path());
                } else if (!ec && rootIt->is_regular_file(ec) && !ec &&
                           (FoldASCII(rootIt->path().extension().string()) == ".npc" ||
                            EndsWithFolded(rootIt->path().filename().string(), ".identity.json"))) {
                    AddIssue(a_result, rootIt->path(), 0, "invalid_convention_layout",
                             "preset and metadata files must be inside Presets/<OwningPlugin>");
                }
            }
            if (ec) {
                AddIssue(a_result, presetsRoot, 0, "preset_scan_failed", ec.message());
                return;
            }
            std::ranges::sort(pluginDirectories, [](const auto& a_left, const auto& a_right) {
                return FoldASCII(a_left.generic_string()) < FoldASCII(a_right.generic_string());
            });

            struct Candidate
            {
                Assignment assignment;
                std::filesystem::path sourcePath;
            };
            std::vector<Candidate> candidates;
            for (const auto& pluginDirectory : pluginDirectories) {
                const auto plugin = pluginDirectory.filename().string();
                if (!IsPluginName(plugin)) {
                    AddIssue(a_result, pluginDirectory, 0, "invalid_convention_plugin",
                             "preset directory name must be an ESM, ESP, or ESL plugin filename");
                    continue;
                }

                std::vector<std::filesystem::path> files;
                std::filesystem::directory_iterator pluginIt{
                    pluginDirectory,
                    std::filesystem::directory_options::skip_permission_denied, ec };
                for (; !ec && pluginIt != end; pluginIt.increment(ec)) {
                    if (pluginIt->is_regular_file(ec) && !ec) {
                        files.push_back(pluginIt->path());
                    } else if (!ec && pluginIt->is_directory(ec) && !ec) {
                        AddIssue(a_result, pluginIt->path(), 0, "invalid_convention_layout",
                                 "nested preset directories are not supported");
                    }
                }
                if (ec) {
                    AddIssue(a_result, pluginDirectory, 0, "preset_scan_failed", ec.message());
                    ec.clear();
                    continue;
                }
                std::ranges::sort(files, [](const auto& a_left, const auto& a_right) {
                    return FoldASCII(a_left.filename().string()) <
                        FoldASCII(a_right.filename().string());
                });
                std::unordered_map<std::string, std::filesystem::path> filesByName;
                for (const auto& file : files) {
                    const auto folded = FoldASCII(file.filename().string());
                    if (!filesByName.emplace(folded, file).second) {
                        AddIssue(a_result, file, 0, "duplicate_convention_filename",
                                 "case-insensitive duplicate filename is not supported");
                    }
                }

                for (const auto& file : files) {
                    if (FoldASCII(file.extension().string()) != ".npc") {
                        continue;
                    }
                    std::uint32_t localFormID = 0;
                    if (!ParseLocalFormID(file.stem().string(), localFormID)) {
                        AddIssue(a_result, file, 0, "invalid_convention_form_id",
                                 "preset filename must be a plugin-local hexadecimal FormID no greater than 00FFFFFF");
                        continue;
                    }

                    Assignment assignment;
                    assignment.target = Target{ plugin, localFormID };
                    const auto relative = std::filesystem::path{ "Presets" } /
                        pluginDirectory.filename() / file.filename();
                    std::string presetError;
                    if (!ResolvePresetPath(a_manifest.manifestPath, relative.generic_string(),
                                           a_requirePresetFiles, assignment.presetPath,
                                           presetError)) {
                        AddIssue(a_result, file, 0, "invalid_preset", presetError);
                        continue;
                    }

                    Requirements presetRequirements;
                    const auto sidecarName = FoldASCII(
                        file.stem().string() + ".identity.json");
                    if (const auto sidecar = filesByName.find(sidecarName);
                        sidecar != filesByName.end()) {
                        auto metadata = LoadPresetMetadata(sidecar->second, packageRoot);
                        a_result.issues.insert(
                            a_result.issues.end(),
                            std::make_move_iterator(metadata.issues.begin()),
                            std::make_move_iterator(metadata.issues.end()));
                        if (!metadata.requirements) {
                            continue;
                        }
                        presetRequirements = std::move(*metadata.requirements);
                    }
                    std::string requirementsError;
                    if (!MergeRequirements(a_manifest.requirements, presetRequirements,
                                           plugin, assignment.requirements,
                                           requirementsError)) {
                        AddIssue(a_result, file, 0, "effective_requirements_invalid",
                                 requirementsError);
                        continue;
                    }
                    if (candidates.size() >= kMaxAssignments) {
                        AddIssue(a_result, file, 0, "assignment_limit_exceeded",
                                 "convention preset count exceeds the 1024-assignment safety limit");
                        return;
                    }
                    candidates.push_back({ std::move(assignment), file });
                }

                for (const auto& file : files) {
                    const auto filename = file.filename().string();
                    if (!EndsWithFolded(filename, ".identity.json")) {
                        continue;
                    }
                    const auto stemLength = filename.size() - std::string_view{ ".identity.json" }.size();
                    const auto presetName = FoldASCII(
                        filename.substr(0, stemLength) + ".npc");
                    if (!filesByName.contains(presetName)) {
                        AddIssue(a_result, file, 0, "orphan_preset_metadata",
                                 "preset metadata has no adjacent .npc file");
                    }
                }
            }

            std::unordered_map<std::string, std::size_t> targetCounts;
            for (const auto& candidate : candidates) {
                ++targetCounts[candidate.assignment.target.CanonicalKey()];
            }
            for (auto& candidate : candidates) {
                const auto key = candidate.assignment.target.CanonicalKey();
                if (targetCounts[key] != 1) {
                    AddIssue(a_result, candidate.sourcePath, 0, "duplicate_target",
                             "package contains more than one convention preset for target " + key);
                    continue;
                }
                a_manifest.assignments.push_back(std::move(candidate.assignment));
            }
        }

        // A manifest-less package may only contain Presets/. Anything at its root that
        // looks like a manifest the runtime failed to recognize is a near-miss worth
        // diagnosing instead of silently treating the package as convention-only.
        [[nodiscard]] bool IsSuspectRootFile(const std::filesystem::path& a_file)
        {
            const auto filename = FoldASCII(a_file.filename().string());
            if (filename == "package.json") {
                return false;
            }
            const auto extension = FoldASCII(a_file.extension().string());
            return extension == ".json" || extension == ".jsonc" ||
                filename.starts_with("package.");
        }

        [[nodiscard]] bool FindNestedManifest(
            const std::filesystem::path& a_packageDirectory,
            std::filesystem::path& a_out)
        {
            std::error_code ec;
            std::filesystem::recursive_directory_iterator it{
                a_packageDirectory, std::filesystem::directory_options::skip_permission_denied, ec };
            const std::filesystem::recursive_directory_iterator end;
            for (; !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec) && !ec &&
                    FoldASCII(it->path().filename().string()) == "package.json") {
                    a_out = it->path();
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] ManifestResult BuildImplicitPackage(
            const std::filesystem::path& a_packageDirectory,
            const bool a_requirePresetFiles)
        {
            ManifestResult result;
            const auto folderName = a_packageDirectory.filename().string();
            const auto packageID = FoldASCII(folderName);
            if (!IsPackageID(packageID)) {
                AddIssue(result, a_packageDirectory, 0, "invalid_package_folder_name",
                         "package folder name '" + folderName +
                             "' cannot be used as a packageId; rename the folder to 3-128 ASCII "
                             "letters, digits, '.', '_' or '-' starting with a letter or digit, or "
                             "add a package.json");
                return result;
            }

            PackageManifest manifest;
            manifest.schemaVersion = 1;
            manifest.packageID = packageID;
            manifest.priority = 0;
            manifest.manifestPath = a_packageDirectory / "package.json";
            manifest.format = PackageFormat::kPluginFolderLocalFormID;
            manifest.implicitManifest = true;
            DiscoverConventionAssignments(manifest, result, a_requirePresetFiles);
            result.manifest = std::move(manifest);
            return result;
        }

        [[nodiscard]] ManifestResult LoadPackageDirectory(
            const std::filesystem::path& a_packageDirectory,
            const bool a_requirePresetFiles)
        {
            ManifestResult result;
            std::optional<std::filesystem::path> manifestPath;
            std::vector<std::filesystem::path> suspects;
            std::error_code ec;
            std::filesystem::directory_iterator it{
                a_packageDirectory, std::filesystem::directory_options::skip_permission_denied, ec };
            const std::filesystem::directory_iterator end;
            for (; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec) || ec) {
                    continue;
                }
                if (FoldASCII(it->path().filename().string()) == "package.json") {
                    manifestPath = it->path();
                } else if (IsSuspectRootFile(it->path())) {
                    suspects.push_back(it->path());
                }
            }
            if (ec) {
                AddIssue(result, a_packageDirectory, 0, "package_scan_failed", ec.message());
                return result;
            }

            if (manifestPath) {
                return LoadPackageManifest(*manifestPath, a_requirePresetFiles);
            }
            if (!suspects.empty()) {
                std::ranges::sort(suspects, [](const auto& a_left, const auto& a_right) {
                    return FoldASCII(a_left.filename().string()) <
                        FoldASCII(a_right.filename().string());
                });
                AddIssue(result, suspects.front(), 0, "suspect_package_root_file",
                         "package has no package.json; rename '" +
                             suspects.front().filename().string() +
                             "' to 'package.json' or remove it, because a manifest-less package may "
                             "only contain Presets/");
                return result;
            }
            if (std::filesystem::path nested; FindNestedManifest(a_packageDirectory, nested)) {
                AddIssue(result, nested, 0, "manifest_not_at_package_root",
                         "package.json must sit at the package root; move it to '" +
                             (a_packageDirectory / "package.json").string() + "'");
                return result;
            }
            return BuildImplicitPackage(a_packageDirectory, a_requirePresetFiles);
        }
    }

    std::string Target::CanonicalKey() const
    {
        return std::format("{}:{:08x}", FoldASCII(plugin), localFormID);
    }

    bool IsLocalFormIDValidForTier(
        const std::uint32_t a_localFormID,
        const PluginTier a_tier) noexcept
    {
        switch (a_tier) {
        case PluginTier::kSmall: return a_localFormID <= 0xFFF;
        case PluginTier::kMedium: return a_localFormID <= 0xFFFF;
        case PluginTier::kFull: return a_localFormID <= 0x00FFFFFF;
        }
        return false;
    }

    ManifestResult ParsePackageManifest(const std::string_view a_json,
                                        const std::filesystem::path& a_manifestPath,
                                        const bool a_requirePresetFiles)
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
        if (!HasOnlyProperties(root, { "$schema", "schemaVersion", "packageId", "priority",
                                       "requires", "assignments", "presetConvention" },
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
        const auto* packageID = Require(root, "packageId", JsonValue::Kind::kString, result, a_manifestPath);
        const auto* priority = Require(root, "priority", JsonValue::Kind::kNumber, result, a_manifestPath);
        const auto* requirementsNode = Require(root, "requires", JsonValue::Kind::kObject, result, a_manifestPath);
        const auto* assignments = root.Find("assignments");
        const auto* convention = root.Find("presetConvention");
        if (!schema || !packageID || !priority || !requirementsNode) {
            return result;
        }
        if ((assignments == nullptr) == (convention == nullptr)) {
            AddIssue(result, a_manifestPath, root.offset, "package_format_choice",
                     "package must contain exactly one of 'assignments' or 'presetConvention'");
            return result;
        }
        if (assignments && assignments->kind != JsonValue::Kind::kArray) {
            AddIssue(result, a_manifestPath, assignments->offset, "wrong_type",
                     "property 'assignments' must be an array");
            return result;
        }
        if (convention && convention->kind != JsonValue::Kind::kString) {
            AddIssue(result, a_manifestPath, convention->offset, "wrong_type",
                     "property 'presetConvention' must be a string");
            return result;
        }
        if (schema->integer != 1) {
            AddIssue(result, a_manifestPath, schema->offset, "unsupported_schema",
                     "unsupported schemaVersion " + std::to_string(schema->integer));
            return result;
        }
        if (!IsPackageID(packageID->string)) {
            AddIssue(result, a_manifestPath, packageID->offset, "invalid_package_id",
                     "packageId must be 3-128 lowercase ASCII letters, digits, '.', '_' or '-'");
            return result;
        }
        if (priority->integer < kMinPriority || priority->integer > kMaxPriority) {
            AddIssue(result, a_manifestPath, priority->offset, "invalid_priority", "priority is outside the accepted range");
            return result;
        }
        if (assignments &&
            (assignments->array.empty() || assignments->array.size() > kMaxAssignments)) {
            AddIssue(result, a_manifestPath, assignments->offset, "invalid_assignment_count",
                     "assignments must contain 1-1024 entries");
            return result;
        }
        if (convention && convention->string != kPluginFolderLocalFormIDConvention) {
            AddIssue(result, a_manifestPath, convention->offset, "unsupported_preset_convention",
                     "presetConvention must be 'pluginFolderLocalFormId' in schema version 1");
            return result;
        }

        PackageManifest manifest;
        manifest.schemaVersion = 1;
        manifest.packageID = packageID->string;
        manifest.priority = static_cast<std::int32_t>(priority->integer);
        manifest.manifestPath = a_manifestPath;
        manifest.format = convention ? PackageFormat::kPluginFolderLocalFormID :
                                       PackageFormat::kExplicitAssignments;

        if (!ParseRequirementsNode(
                *requirementsNode, manifest.requirements, result, a_manifestPath)) {
            return result;
        }

        if (assignments) {
            std::unordered_set<std::string> targets;
            for (const auto& rawAssignment : assignments->array) {
                if (rawAssignment.kind != JsonValue::Kind::kObject ||
                    !HasOnlyProperties(rawAssignment, { "target", "preset", "scope", "requires" },
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
                const auto* scope = Require(rawAssignment, "scope", JsonValue::Kind::kString,
                                            result, a_manifestPath);
                if (!target || !preset || !scope ||
                    !HasOnlyProperties(*target, { "plugin", "localFormId" },
                                       result, a_manifestPath)) {
                    return result;
                }
                const auto* plugin = Require(*target, "plugin", JsonValue::Kind::kString,
                                             result, a_manifestPath);
                const auto* localFormID = Require(*target, "localFormId", JsonValue::Kind::kString,
                                                  result, a_manifestPath);
                if (!plugin || !localFormID) {
                    return result;
                }
                if (!IsPluginName(plugin->string)) {
                    AddIssue(result, a_manifestPath, plugin->offset, "invalid_plugin",
                             "target plugin name is invalid");
                    return result;
                }
                Assignment assignment;
                assignment.target.plugin = plugin->string;
                if (!ParseLocalFormID(localFormID->string, assignment.target.localFormID)) {
                    AddIssue(result, a_manifestPath, localFormID->offset, "invalid_local_form_id",
                             "localFormId must be 1-8 hexadecimal digits no greater than 00FFFFFF");
                    return result;
                }
                if (scope->string != "faceAndBody") {
                    AddIssue(result, a_manifestPath, scope->offset, "unsupported_scope",
                             "scope must be 'faceAndBody' in schema version 1");
                    return result;
                }
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
                                       plugin->string, assignment.requirements,
                                       requirementsError)) {
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
                             "package contains more than one assignment for target " + targetKey);
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

    DiscoveryResult DiscoverPackages(const std::filesystem::path& a_packagesRoot,
                                     const bool a_requirePresetFiles)
    {
        DiscoveryResult result;
        std::error_code ec;
        if (!std::filesystem::is_directory(a_packagesRoot, ec) || ec) {
            result.issues.push_back({ a_packagesRoot, 0, "package_root_missing", "package root is missing or is not a directory" });
            return result;
        }
        std::vector<std::filesystem::path> packageDirectories;
        std::filesystem::directory_iterator it{ a_packagesRoot,
            std::filesystem::directory_options::skip_permission_denied, ec };
        const std::filesystem::directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec) && !ec) {
                packageDirectories.push_back(it->path());
                if (packageDirectories.size() > kMaxPackages) {
                    result.issues.push_back({ a_packagesRoot, 0, "package_limit_exceeded", "package count exceeds safety limit" });
                    return result;
                }
            } else if (!ec) {
                result.issues.push_back({ it->path(), 0, "stray_package_root_file",
                                          "files directly under the package root are ignored; every package must be its own folder" });
            }
        }
        if (ec) {
            result.issues.push_back({ a_packagesRoot, 0, "package_scan_failed", ec.message() });
            return result;
        }
        std::ranges::sort(packageDirectories, [](const auto& a_left, const auto& a_right) {
            return FoldASCII(a_left.generic_string()) < FoldASCII(a_right.generic_string());
        });
        for (const auto& packageDirectory : packageDirectories) {
            auto parsed = LoadPackageDirectory(packageDirectory, a_requirePresetFiles);
            result.issues.insert(result.issues.end(),
                                 std::make_move_iterator(parsed.issues.begin()),
                                 std::make_move_iterator(parsed.issues.end()));
            if (parsed.manifest) {
                result.packages.push_back(std::move(*parsed.manifest));
            }
        }

        std::unordered_map<std::string, std::size_t> counts;
        for (const auto& package : result.packages) {
            ++counts[FoldASCII(package.packageID)];
        }
        std::erase_if(result.packages, [&](const auto& package) {
            if (counts[FoldASCII(package.packageID)] == 1) {
                return false;
            }
            result.issues.push_back({ package.DiagnosticPath(), 0, "duplicate_package_id",
                                      "every package using duplicate packageId '" + package.packageID + "' was rejected" });
            return true;
        });
        return result;
    }

    AssetRequirementResult CheckRequiredAssets(
        const PackageManifest& a_package,
        const std::filesystem::path& a_dataRoot)
    {
        return CheckRequiredAssets(a_package.requirements, a_dataRoot);
    }

    AssetRequirementResult CheckRequiredAssets(
        const Requirements& a_requirements,
        const std::filesystem::path& a_dataRoot)
    {
        AssetRequirementResult result;
        if (a_dataRoot.empty()) {
            result.missing = a_requirements.assets;
            return result;
        }
        for (const auto& relative : a_requirements.assets) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(a_dataRoot / relative, ec) || ec) {
                result.missing.push_back(relative);
            }
        }
        return result;
    }

    SelectionResult SelectAssignments(const std::vector<PackageManifest>& a_packages)
    {
        struct Candidate
        {
            const PackageManifest* package;
            const Assignment* assignment;
        };
        std::map<std::string, std::vector<Candidate>> groups;
        for (const auto& package : a_packages) {
            for (const auto& assignment : package.assignments) {
                groups[assignment.target.CanonicalKey()].push_back({ &package, &assignment });
            }
        }

        SelectionResult result;
        for (auto& [targetKey, candidates] : groups) {
            std::ranges::sort(candidates, [](const Candidate& a_left, const Candidate& a_right) {
                if (a_left.package->priority != a_right.package->priority) {
                    return a_left.package->priority > a_right.package->priority;
                }
                return a_left.package->packageID < a_right.package->packageID;
            });
            const auto& winner = candidates.front();
            result.winners.push_back({ winner.assignment->target, winner.assignment->presetPath,
                                       winner.assignment->requirements,
                                       winner.package->packageID, winner.package->priority });
            for (const auto& candidate : candidates) {
                const bool won = std::addressof(candidate) == std::addressof(candidates.front());
                result.decisions.push_back({ targetKey, candidate.package->packageID,
                                             candidate.package->priority, won,
                                             won ? "winner" : "shadowed_by_" + winner.package->packageID });
            }
        }
        return result;
    }
}
