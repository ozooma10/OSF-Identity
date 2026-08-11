#include "Config.h"
#include "ConfigDetail.h"
#include "Util/String.h"

#include <algorithm>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Config
{
    namespace
    {
        bool PathLess(const std::filesystem::path& a_left, const std::filesystem::path& a_right)
        {
            const auto left = Util::FoldASCII(a_left.filename().string());
            const auto right = Util::FoldASCII(a_right.filename().string());
            return left != right ? left < right : a_left.filename().string() < a_right.filename().string();
        }

        std::string TargetKey(const Target& a_target)
        {
            return Util::FoldASCII(a_target.plugin) + ":" + std::to_string(a_target.localFormID);
        }

        DiscoveryResult ScanPackDirectory(const std::filesystem::path& a_packDirectory)
        {
            DiscoveryResult result;
            Pack pack{
                .id = a_packDirectory.filename().string(),
                .rootPath = a_packDirectory
            };

            std::vector<std::filesystem::path> pluginDirectories;
            std::error_code ec;
            std::filesystem::directory_iterator rootIt{ a_packDirectory, std::filesystem::directory_options::skip_permission_denied, ec };
            const std::filesystem::directory_iterator end;
            for (; !ec && rootIt != end; rootIt.increment(ec)) {
                if (rootIt->is_directory(ec) && !ec) {
                    pluginDirectories.push_back(rootIt->path());
                } else if (!ec && rootIt->is_regular_file(ec) && !ec) {
                    const auto filename = Util::FoldASCII(rootIt->path().filename().string());
                    const auto extension = Util::FoldASCII(rootIt->path().extension().string());
                    if (extension == ".npc") {
                        Detail::AddIssue(result, rootIt->path(), 0, "invalid_convention_layout", "preset files must be inside <OwningPlugin>/");
                    }
                } else if (!ec) {
                    Detail::AddIssue(result, rootIt->path(), 0, "invalid_convention_layout", "unsupported entry at the pack root");
                }
            }
            if (ec) {
                Detail::AddIssue(result, a_packDirectory, 0, "pack_scan_failed", ec.message());
                return result;
            }
            std::ranges::sort(pluginDirectories, PathLess);

            struct Candidate
            {
                Assignment assignment;
                std::filesystem::path sourcePath;
            };
            std::vector<Candidate> candidates;

            for (const auto& pluginDirectory : pluginDirectories) {
                const auto plugin = pluginDirectory.filename().string();
                if (!Detail::IsPluginName(plugin)) {
                    Detail::AddIssue(result, pluginDirectory, 0, "invalid_convention_plugin", "preset directory name must be an ESM, ESP, or ESL plugin filename");
                    continue;
                }

                std::vector<std::filesystem::path> files;
                std::filesystem::directory_iterator pluginIt{ pluginDirectory, std::filesystem::directory_options::skip_permission_denied, ec };
                for (; !ec && pluginIt != end; pluginIt.increment(ec)) {
                    if (pluginIt->is_regular_file(ec) && !ec) {
                        files.push_back(pluginIt->path());
                    } else if (!ec && pluginIt->is_directory(ec) && !ec) {
                        Detail::AddIssue(result, pluginIt->path(), 0, "invalid_convention_layout", "nested preset directories are not supported");
                    } else if (!ec) {
                        Detail::AddIssue(result, pluginIt->path(), 0, "invalid_convention_layout", "unsupported entry inside an owning-plugin directory");
                    }
                }
                if (ec) {
                    Detail::AddIssue(result, pluginDirectory, 0, "preset_scan_failed", ec.message());
                    ec.clear();
                    continue;
                }
                std::ranges::sort(files, PathLess);

                std::unordered_map<std::string, std::size_t> filenameCounts;
                for (const auto& file : files) {
                    ++filenameCounts[Util::FoldASCII(file.filename().string())];
                }

                for (const auto& file : files) {
                    const auto foldedFilename = Util::FoldASCII(file.filename().string());
                    if (Util::FoldASCII(file.extension().string()) != ".npc") {
                        continue;
                    }
                    if (filenameCounts[foldedFilename] != 1) {
                        Detail::AddIssue(result, file, 0, "duplicate_convention_filename", "case-insensitive duplicate filename is not supported");
                        continue;
                    }

                    std::uint32_t localFormID = 0;
                    if (!Detail::ParseLocalFormID(file.stem().string(), localFormID)) {
                        Detail::AddIssue(result, file, 0, "invalid_convention_form_id", "preset filename must be a 1-8 digit plugin-local hexadecimal FormID no greater than 00FFFFFF and without a 0x prefix");
                        continue;
                    }

                    Assignment assignment;
                    assignment.target = Target{ plugin, localFormID };
                    const auto relative = pluginDirectory.filename() / file.filename();
                    std::string presetError;
                    if (!Detail::ResolvePresetPath(pack.rootPath, relative.generic_string(), true, assignment.presetPath, presetError)) {
                        Detail::AddIssue( result, file, 0, "invalid_preset", presetError);
                        continue;
                    }
                    candidates.push_back({ std::move(assignment), file });
                }
            }

            std::unordered_map<std::string, std::size_t> targetCounts;
            for (const auto& candidate : candidates) {
                ++targetCounts[TargetKey(candidate.assignment.target)];
            }
            for (auto& candidate : candidates) {
                if (targetCounts[TargetKey(candidate.assignment.target)] != 1) {
                    Detail::AddIssue(result, candidate.sourcePath, 0, "duplicate_target", "pack contains more than one preset for the same canonical target");
                    continue;
                }
                pack.assignments.push_back(std::move(candidate.assignment));
            }

            result.packs.push_back(std::move(pack));
            return result;
        }
    }

    DiscoveryResult DiscoverPacks(const std::filesystem::path& a_packsRoot)
    {
        DiscoveryResult result;
        std::error_code ec;
        if (!std::filesystem::is_directory(a_packsRoot, ec) || ec) {
            result.issues.push_back({a_packsRoot, 0, "package_root_missing", "packs root is missing or is not a directory"});
            return result;
        }

        std::vector<std::filesystem::path> packDirectories;
        std::filesystem::directory_iterator it{a_packsRoot, std::filesystem::directory_options::skip_permission_denied, ec };
        const std::filesystem::directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec) && !ec) {
                packDirectories.push_back(it->path());
            } else if (!ec) {
                result.issues.push_back({it->path(), 0, "stray_package_root_file", "files directly under the packs root are ignored; every pack must be its own folder"});
            }
        }
        if (ec) {
            result.issues.push_back({a_packsRoot, 0, "package_scan_failed", ec.message()});
            return result;
        }

        std::ranges::sort(packDirectories, [](const auto& a_left, const auto& a_right) {
            const auto left = Util::FoldASCII(a_left.generic_string());
            const auto right = Util::FoldASCII(a_right.generic_string());
            return left != right ? left < right : a_left.generic_string() < a_right.generic_string();
        });

        for (const auto& packDirectory : packDirectories) {
            auto scanned = ScanPackDirectory(packDirectory);
            result.issues.insert(result.issues.end(), std::make_move_iterator(scanned.issues.begin()), std::make_move_iterator(scanned.issues.end()));
            result.packs.insert(result.packs.end(), std::make_move_iterator(scanned.packs.begin()), std::make_move_iterator(scanned.packs.end()));
        }

        return result;
    }
}
