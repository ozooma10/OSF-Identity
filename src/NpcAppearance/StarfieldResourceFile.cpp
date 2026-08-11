#include "NpcAppearance/StarfieldResourceFile.h"

#include "pch.h"

#include <algorithm>
#include <cwchar>
#include <optional>
#include <ranges>
#include <string>
#include <system_error>

namespace NpcAppearance
{
    namespace
    {
        [[nodiscard]] bool PathComponentEquals(
            const std::filesystem::path& a_left,
            const std::filesystem::path& a_right)
        {
#ifdef _WIN32
            return _wcsicmp(a_left.c_str(), a_right.c_str()) == 0;
#else
            return a_left == a_right;
#endif
        }

        [[nodiscard]] bool IsWithin(
            const std::filesystem::path& a_root,
            const std::filesystem::path& a_candidate)
        {
            auto rootIt = a_root.begin();
            auto candidateIt = a_candidate.begin();
            for (; rootIt != a_root.end(); ++rootIt, ++candidateIt) {
                if (candidateIt == a_candidate.end() ||
                    !PathComponentEquals(*rootIt, *candidateIt)) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] std::optional<std::string> NormalizeResourcePath(
            const std::filesystem::path& a_path)
        {
            const auto normalized = a_path.lexically_normal();
            if (normalized.empty() || normalized.is_absolute() ||
                normalized.has_root_name() || normalized.has_root_directory()) {
                return std::nullopt;
            }
            for (const auto& component : normalized) {
                if (component == "..") {
                    return std::nullopt;
                }
            }
            auto resourcePath = normalized.generic_string();
            std::ranges::replace(resourcePath, '/', '\\');
            return resourcePath;
        }

        [[nodiscard]] std::optional<std::string> PresetResourcePath(
            const std::filesystem::path& a_packsRoot,
            const std::filesystem::path& a_presetPath)
        {
            std::error_code ec;
            const auto root =
                std::filesystem::absolute(a_packsRoot, ec).lexically_normal();
            if (ec) {
                return std::nullopt;
            }
            const auto preset =
                std::filesystem::absolute(a_presetPath, ec).lexically_normal();
            if (ec || !IsWithin(root, preset)) {
                return std::nullopt;
            }

            auto rootIt = root.begin();
            auto presetIt = preset.begin();
            while (rootIt != root.end()) {
                ++rootIt;
                ++presetIt;
            }
            std::filesystem::path relative;
            for (; presetIt != preset.end(); ++presetIt) {
                relative /= *presetIt;
            }
            return NormalizeResourcePath(
                std::filesystem::path{ "SFSE/Plugins/OSFIdentity/Packs" } / relative);
        }
    }

    bool StarfieldResourceExists(const std::filesystem::path& a_dataRelativePath)
    {
        const auto path = NormalizeResourcePath(a_dataRelativePath);
        if (!path) {
            return false;
        }
        const RE::BSResourceNiBinaryStream stream{ *path };
        return stream.Good();
    }

    PresetResult LoadStarfieldCkPreset(
        const std::filesystem::path& a_packsRoot,
        const std::filesystem::path& a_presetPath)
    {
        PresetResult result;
        const auto resourcePath = PresetResourcePath(a_packsRoot, a_presetPath);
        if (!resourcePath) {
            result.issues.push_back({ a_presetPath, 0, "invalid_resource_path",
                                      "preset path is outside the configured packs root" });
            return result;
        }

        RE::BSResourceNiBinaryStream stream{ *resourcePath };
        if (!stream.Good()) {
            result.issues.push_back({ a_presetPath, 0, "resource_missing",
                                      "preset is unavailable through loose files and loaded archives" });
            return result;
        }
        const auto size = stream.GetSize();
        if (size == 0 || size > kMaxPresetBytes) {
            result.issues.push_back({ a_presetPath, 0, "invalid_size",
                                      "preset is empty or exceeds the 32 MiB safety limit" });
            return result;
        }

        std::string bytes(size, '\0');
        if (stream.DoRead(bytes.data(), bytes.size()) != bytes.size()) {
            result.issues.push_back({ a_presetPath, 0, "read_failed",
                                      "could not read the complete preset resource" });
            return result;
        }
        return ParseCkPreset(bytes, a_presetPath);
    }
}
