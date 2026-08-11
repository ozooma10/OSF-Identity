#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Filesystem pack contract: discover <Pack>/<OwningPlugin>/<LocalFormID>.npc assignments.
namespace Config
{
    struct Target
    {
        std::string plugin;
        std::uint32_t localFormID;
    };

    struct Assignment
    {
        Target target;
        std::filesystem::path presetPath;
    };

    struct Pack
    {
        std::string id;
        std::filesystem::path rootPath;
        std::vector<Assignment> assignments;
    };

    struct ConfigIssue
    {
        std::filesystem::path path;
        std::size_t offset{ 0 };
        std::string code;
        std::string message;
    };

    struct DiscoveryResult
    {
        std::vector<Pack> packs;
        std::vector<ConfigIssue> issues;
    };

    // Runtime handoff type. Selection and engine FormID resolution populate this later.
    struct SelectedAssignment
    {
        Target target;
        std::filesystem::path presetPath;
        std::string packID;
    };

    [[nodiscard]] DiscoveryResult DiscoverPacks(const std::filesystem::path& a_packsRoot);
}
