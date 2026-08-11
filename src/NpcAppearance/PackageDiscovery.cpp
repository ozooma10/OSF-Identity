#include "NpcAppearance/Config.h"

#include "NpcAppearance/ConfigDetail.h"

#include <algorithm>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NpcAppearance
{
    using namespace Detail;

    namespace
    {
        // Anything at a manifest-less pack root that looks like a manifest the
        // runtime failed to recognize is a near-miss worth diagnosing instead of
        // silently treating the pack as convention-only.
        [[nodiscard]] bool IsSuspectRootFile(const std::filesystem::path& a_file)
        {
            const auto filename = FoldASCII(a_file.filename().string());
            if (filename == "package.json") {
                return false;
            }
            const auto extension = FoldASCII(a_file.extension().string());
            if (extension == ".json") {
                std::error_code ec;
                const auto presetPath = a_file.parent_path() /
                    (a_file.stem().string() + ".npc");
                if (std::filesystem::is_regular_file(presetPath, ec) && !ec) {
                    return false;
                }
            }
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
        Pack ScanPackDirectory(const std::filesystem::path& a_packDirectory)
        {
            DiscoveryResult result;
            const auto folderName = a_packageDirectory.filename().string();
            Pack pack;
            pack.id = folderName;
            pack.rootPath = a_packDirectory;

            std::vector<std::filesystem::path> pluginDirectories;

            std::filesystem::directory_iterator rootIt{ a_packDirectory, std::filesystem::directory_options::skip_permission_denied, ec };
            const std::filesystem::directory_iterator end;
            for (; !ec && rootIt != end; rootIt.increment(ec)) {
                if (rootIt->is_directory(ec) && !ec) {
                    pluginDirectories.push_back(rootIt->path());
                } else if (!ec && rootIt->is_regular_file(ec) && !ec) {
                    const auto extension = FoldASCII(rootIt->path().extension().string());
                    if (extension == ".npc") {
                        AddIssue(result, rootIt->path(), 0, "invalid_convention_layout", "preset files must be inside <OwningPlugin>/");
                    }
                } else if (!ec) {
                    AddIssue(result, rootIt->path(), 0, "invalid_convention_layout", "unsupported entry at the pack root");
                }
            }
            if (ec) {
                AddIssue(result, packageRoot, 0, "preset_scan_failed", ec.message());
                return;
            }
            std::ranges::sort(pluginDirectories, [](const auto& a_left, const auto& a_right) {
                return FoldASCII(a_left.filename().string()) <
                    FoldASCII(a_right.filename().string());
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
                    AddIssue(a_result, pluginDirectory, 0, "invalid_convention_plugin", "preset directory name must be an ESM, ESP, or ESL plugin filename");
                    continue;
                }

                std::vector<std::filesystem::path> files;
                std::filesystem::directory_iterator pluginIt{pluginDirectory, std::filesystem::directory_options::skip_permission_denied, ec };
                for (; !ec && pluginIt != end; pluginIt.increment(ec)) {
                    if (pluginIt->is_regular_file(ec) && !ec) {
                        files.push_back(pluginIt->path());
                    } else if (!ec && pluginIt->is_directory(ec) && !ec) {
                        AddIssue(a_result, pluginIt->path(), 0, "invalid_convention_layout", "nested preset directories are not supported");
                    }
                }
                if (ec) {
                    AddIssue(a_result, pluginDirectory, 0, "preset_scan_failed", ec.message());
                    ec.clear();
                    continue;
                }
                std::ranges::sort(files, [](const auto& a_left, const auto& a_right) {
                    return FoldASCII(a_left.filename().string()) < FoldASCII(a_right.filename().string());
                });
                std::unordered_map<std::string, std::filesystem::path> filesByName;
                for (const auto& file : files) {
                    const auto folded = FoldASCII(file.filename().string());
                    if (!filesByName.emplace(folded, file).second) {
                        AddIssue(a_result, file, 0, "duplicate_convention_filename", "case-insensitive duplicate filename is not supported");
                    }
                }

                for (const auto& file : files) {
                    if (FoldASCII(file.extension().string()) != ".npc") {
                        continue;
                    }
                    std::uint32_t localFormID = 0;
                    if (!ParseLocalFormID(file.stem().string(), localFormID)) {
                        AddIssue(a_result, file, 0, "invalid_convention_form_id", "preset filename must be a 1-8 digit plugin-local hexadecimal FormID no greater than 00FFFFFF and without a 0x prefix");
                        continue;
                    }

                    Assignment assignment;
                    assignment.target = Target{ plugin, localFormID };
                    const auto relative = pluginDirectory.filename() / file.filename();
                    std::string presetError;
                    if (!ResolvePresetPath(a_manifest.manifestPath, relative.generic_string(), a_requirePresetFiles, assignment.presetPath, presetError)) {
                        AddIssue(a_result, file, 0, "invalid_preset", presetError);
                        continue;
                    }
                    candidates.push_back({ std::move(assignment), file });
                }
            }

            for (auto& candidate : candidates) {
                pack.assignments.push_back(std::move(candidate.assignment));
            }

            result.packs.push_back(std::move(pack));

            return result;
        }
    }

    namespace Detail
    {
        void DiscoverConventionAssignments(
            PackageManifest& a_manifest,
            ManifestResult& a_result,
            const bool a_requirePresetFiles)
        {
            

            std::vector<std::filesystem::path> pluginDirectories;
            std::filesystem::directory_iterator rootIt{
                packageRoot, std::filesystem::directory_options::skip_permission_denied, ec };
            const std::filesystem::directory_iterator end;
            for (; !ec && rootIt != end; rootIt.increment(ec)) {
                if (rootIt->is_directory(ec) && !ec) {
                    pluginDirectories.push_back(rootIt->path());
                } else if (!ec && rootIt->is_regular_file(ec) && !ec) {
                    const auto extension = FoldASCII(rootIt->path().extension().string());
                    if (extension == ".npc" ||
                        (extension == ".json" &&
                         FoldASCII(rootIt->path().filename().string()) != "package.json")) {
                        AddIssue(a_result, rootIt->path(), 0, "invalid_convention_layout",
                                 "preset and metadata files must be inside <OwningPlugin>/");
                    }
                } else if (!ec) {
                    AddIssue(a_result, rootIt->path(), 0, "invalid_convention_layout",
                             "unsupported entry at the pack root");
                }
            }
            if (ec) {
                AddIssue(a_result, packageRoot, 0, "preset_scan_failed", ec.message());
                return;
            }
            std::ranges::sort(pluginDirectories, [](const auto& a_left, const auto& a_right) {
                return FoldASCII(a_left.filename().string()) <
                    FoldASCII(a_right.filename().string());
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
                                 "preset filename must be a 1-8 digit plugin-local hexadecimal FormID no greater than 00FFFFFF and without a 0x prefix");
                        continue;
                    }

                    Assignment assignment;
                    assignment.target = Target{ plugin, localFormID };
                    const auto relative = pluginDirectory.filename() / file.filename();
                    std::string presetError;
                    if (!ResolvePresetPath(a_manifest.manifestPath, relative.generic_string(),
                                           a_requirePresetFiles, assignment.presetPath,
                                           presetError)) {
                        AddIssue(a_result, file, 0, "invalid_preset", presetError);
                        continue;
                    }

                    Requirements presetRequirements;
                    const auto sidecarName = FoldASCII(
                        file.stem().string() + ".json");
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
                    if (!MergeRequirements(a_manifest.requirements, presetRequirements,
                                           assignment.requirements,
                                           plugin)) {
                        AddIssue(a_result, file, 0, "effective_requirements_invalid", "effective requirements are invalid");
                        continue;
                    }
                    candidates.push_back({ std::move(assignment), file });
                }

                for (const auto& file : files) {
                    if (FoldASCII(file.extension().string()) != ".json") {
                        continue;
                    }
                    const auto presetName = FoldASCII(
                        file.stem().string() + ".npc");
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
                             "pack contains more than one convention preset for target " + key);
                    continue;
                }
                a_manifest.assignments.push_back(std::move(candidate.assignment));
            }
        }
    }

    DiscoveryResult DiscoverPackages(const std::filesystem::path& a_packsRoot,
                                     const bool a_requirePresetFiles)
    {
        DiscoveryResult result;
        std::error_code ec;
        if (!std::filesystem::is_directory(a_packsRoot, ec) || ec) {
            result.issues.push_back({ a_packsRoot, 0, "package_root_missing", "packs root is missing or is not a directory" });
            return result;
        }
        std::vector<std::filesystem::path> packageDirectories;
        std::filesystem::directory_iterator it{ a_packsRoot,
            std::filesystem::directory_options::skip_permission_denied, ec };
        const std::filesystem::directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec) && !ec) {
                packageDirectories.push_back(it->path());
            } else if (!ec) {
                result.issues.push_back({ it->path(), 0, "stray_package_root_file",
                                          "files directly under the packs root are ignored; every pack must be its own folder" });
            }
        }
        if (ec) {
            result.issues.push_back({ a_packsRoot, 0, "package_scan_failed", ec.message() });
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
                                      "every pack whose folder name collides case-insensitively with '" +
                                          package.packageID + "' was rejected" });
            return true;
        });
        return result;
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
}
