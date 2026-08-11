#include "Config/Config.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string_view>

namespace
{
    std::size_t g_failed = 0;

    void Check(const bool a_condition, const char* a_name)
    {
        std::cout << (a_condition ? "PASS " : "FAIL ") << a_name << '\n';
        if (!a_condition) {
            ++g_failed;
        }
    }

    void Write(const std::filesystem::path& a_path,
               const std::string_view a_text = "fixture")
    {
        std::filesystem::create_directories(a_path.parent_path());
        std::ofstream stream{ a_path, std::ios::binary };
        stream.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
        if (!stream) {
            throw std::runtime_error{ "could not write test fixture" };
        }
    }

    void CreateDirectory(const std::filesystem::path& a_path)
    {
        std::error_code ec;
        std::filesystem::create_directories(a_path, ec);
        if (ec) {
            throw std::runtime_error{ "could not create test directory: " +
                                      ec.message() };
        }
    }

    [[nodiscard]] bool HasIssue(const Config::DiscoveryResult& a_result,
                                const std::string_view a_code)
    {
        return std::ranges::any_of(a_result.issues, [&](const auto& a_issue) {
            return a_issue.code == a_code;
        });
    }

    [[nodiscard]] std::size_t CountIssues(
        const Config::DiscoveryResult& a_result,
        const std::string_view a_code)
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            a_result.issues, [&](const auto& a_issue) {
                return a_issue.code == a_code;
            }));
    }

    [[nodiscard]] const Config::Pack* FindPack(
        const Config::DiscoveryResult& a_result,
        const std::string_view a_id)
    {
        const auto found = std::ranges::find_if(
            a_result.packs, [&](const auto& a_pack) {
                return a_pack.id == a_id;
            });
        return found == a_result.packs.end() ? nullptr : &*found;
    }
}

int main()
{
    const auto root = std::filesystem::absolute("tmp/config-tests");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    if (ec) {
        std::cerr << "could not reset test root: " << ec.message() << '\n';
        return 1;
    }

    const auto missing = Config::DiscoverPacks(root / "missing");
    Check(missing.packs.empty() &&
              HasIssue(missing, "package_root_missing"),
          "missing packs root is diagnosed");

    const auto validRoot = root / "valid";
    Write(validRoot / "Bravo Pack" / "Starfield.esm" / "00005983.npc");
    Write(validRoot / "alpha pack" / "Example.esl" / "A.NPC");
    Write(validRoot / "Middle Pack" / "Example.esp" / "00FFFFFF.npc");
    Write(validRoot / "alpha pack" / "package.json", "{}");
    Write(validRoot / "alpha pack" / "Example.esl" / "A.json", "{}");
    Write(validRoot / "alpha pack" / "Example.esl" / "README.md", "notes");

    const auto valid = Config::DiscoverPacks(validRoot);
    Check(valid.packs.size() == 3 && valid.issues.empty(),
          "valid ESM ESP and ESL filesystem packs are discovered");
    Check(valid.packs.size() == 3 &&
              valid.packs[0].id == "alpha pack" &&
              valid.packs[1].id == "Bravo Pack" &&
              valid.packs[2].id == "Middle Pack",
          "packs use deterministic case-insensitive folder order");

    const auto* alpha = FindPack(valid, "alpha pack");
    Check(alpha && alpha->rootPath == validRoot / "alpha pack" &&
              alpha->assignments.size() == 1 &&
              alpha->assignments[0].target.plugin == "Example.esl" &&
              alpha->assignments[0].target.localFormID == 0xAu &&
              alpha->assignments[0].presetPath.is_absolute(),
          "pack identity and plugin-local target come from the filesystem");
    Check(alpha && alpha->assignments.size() == 1,
          "unsupported JSON and unrelated files do not become assignments");

    const auto layoutRoot = root / "invalid-layout";
    Write(layoutRoot / "loose.txt", "stray");
    Write(layoutRoot / "Layout Pack" / "5983.npc");
    Write(layoutRoot / "Layout Pack" / "not-a-plugin" / "5983.npc");
    Write(layoutRoot / "Layout Pack" / "Starfield.esm" / "Nested" /
          "5984.npc");

    const auto layout = Config::DiscoverPacks(layoutRoot);
    const auto* layoutPack = FindPack(layout, "Layout Pack");
    Check(layoutPack && layoutPack->assignments.empty(),
          "invalid pack layouts produce no assignments");
    Check(HasIssue(layout, "stray_package_root_file") &&
              HasIssue(layout, "invalid_convention_plugin") &&
              HasIssue(layout, "invalid_convention_layout"),
          "root files invalid plugin folders and nesting are diagnosed");

    const auto formRoot = root / "invalid-form-ids";
    Write(formRoot / "Form Pack" / "Starfield.esm" / "0x5983.npc");
    Write(formRoot / "Form Pack" / "Starfield.esm" / "01000000.npc");
    Write(formRoot / "Form Pack" / "Starfield.esm" / "G.npc");
    Write(formRoot / "Form Pack" / "Starfield.esm" / "-1.npc");
    Write(formRoot / "Form Pack" / "Starfield.esm" / "5983.txt");

    const auto invalidForms = Config::DiscoverPacks(formRoot);
    const auto* formPack = FindPack(invalidForms, "Form Pack");
    Check(formPack && formPack->assignments.empty(),
          "invalid plugin-local FormIDs produce no assignments");
    Check(CountIssues(invalidForms, "invalid_convention_form_id") == 4,
          "prefix overflow signs and non-hex FormIDs are rejected");

    const auto emptyRoot = root / "empty-preset";
    Write(emptyRoot / "Empty Pack" / "Starfield.esm" / "5983.npc", "");
    const auto emptyPreset = Config::DiscoverPacks(emptyRoot);
    const auto* emptyPack = FindPack(emptyPreset, "Empty Pack");
    Check(emptyPack && emptyPack->assignments.empty() &&
              HasIssue(emptyPreset, "invalid_preset"),
          "empty preset is rejected without rejecting its pack");

    const auto duplicateTargetRoot = root / "duplicate-target";
    Write(duplicateTargetRoot / "Duplicate Pack" / "Starfield.esm" /
          "5983.npc");
    Write(duplicateTargetRoot / "Duplicate Pack" / "Starfield.esm" /
          "00005983.npc");
    const auto duplicateTarget = Config::DiscoverPacks(duplicateTargetRoot);
    const auto* duplicatePack = FindPack(duplicateTarget, "Duplicate Pack");
    Check(duplicatePack && duplicatePack->assignments.empty() &&
              CountIssues(duplicateTarget, "duplicate_target") == 2,
          "equivalent flexible-width FormIDs are rejected as duplicate targets");

#ifndef _WIN32
    const auto duplicateFilenameRoot = root / "duplicate-filename";
    Write(duplicateFilenameRoot / "Filename Pack" / "Starfield.esm" /
          "5983.npc");
    Write(duplicateFilenameRoot / "Filename Pack" / "Starfield.esm" /
          "5983.NPC");
    const auto duplicateFilename =
        Config::DiscoverPacks(duplicateFilenameRoot);
    const auto* filenamePack = FindPack(duplicateFilename, "Filename Pack");
    Check(filenamePack && filenamePack->assignments.empty() &&
              HasIssue(duplicateFilename, "duplicate_convention_filename"),
          "case-insensitive duplicate filenames are rejected");
#endif

    const auto independentRoot = root / "independent-packs";
    Write(independentRoot / "Pack A" / "Starfield.esm" / "5983.npc");
    Write(independentRoot / "Pack B" / "Starfield.esm" / "00005983.npc");
    CreateDirectory(independentRoot / "Empty Pack");
    const auto independent = Config::DiscoverPacks(independentRoot);
    Check(independent.packs.size() == 3 && independent.issues.empty(),
          "different packs may target the same NPC and empty packs remain inert");
    Check(FindPack(independent, "Pack A") &&
              FindPack(independent, "Pack A")->assignments.size() == 1 &&
              FindPack(independent, "Pack B") &&
              FindPack(independent, "Pack B")->assignments.size() == 1,
          "duplicate-target isolation is per pack");

    std::filesystem::remove_all(root, ec);
    std::cout << "RESULT failed=" << g_failed << '\n';
    return g_failed == 0 ? 0 : 1;
}
