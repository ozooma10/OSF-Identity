#pragma once

#include "./Config.h"

#include <string_view>

namespace Config::Detail
{
    void AddIssue(DiscoveryResult& a_result, const std::filesystem::path& a_path, std::size_t a_offset, std::string a_code, std::string a_message);

    [[nodiscard]] bool IsPluginName(std::string_view a_name);

    [[nodiscard]] bool ParseLocalFormID(std::string_view a_text, std::uint32_t& a_out);

    [[nodiscard]] bool IsWithin(const std::filesystem::path& a_root, const std::filesystem::path& a_candidate);

    [[nodiscard]] bool ValidateRelativePath(const std::string& a_text, std::filesystem::path& a_out, std::string& a_error);

    bool ResolvePresetPath(const std::filesystem::path& a_packPath, const std::string& a_text, bool a_requireFile, std::filesystem::path& a_out, std::string& a_error);
}
