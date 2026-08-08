#include "NpcAppearance/ConfigDetail.h"

#include <algorithm>
#include <charconv>
#include <cwchar>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace NpcAppearance::Detail
{
    namespace
    {
        [[nodiscard]] char LowerASCII(const char a_ch) noexcept
        {
            return a_ch >= 'A' && a_ch <= 'Z' ? static_cast<char>(a_ch - 'A' + 'a') : a_ch;
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

        [[nodiscard]] bool ContainsParentTraversal(const std::string_view a_text) noexcept
        {
            std::size_t componentStart = 0;
            for (std::size_t i = 0; i <= a_text.size(); ++i) {
                if (i != a_text.size() && a_text[i] != '/' && a_text[i] != '\\') {
                    continue;
                }
                if (a_text.substr(componentStart, i - componentStart) == "..") {
                    return true;
                }
                componentStart = i + 1;
            }
            return false;
        }

        [[nodiscard]] bool ParseStringArray(const Json::Value& a_value, const std::string_view a_property,
                                            const bool a_plugins, std::vector<std::string>& a_strings,
                                            std::vector<std::filesystem::path>& a_paths,
                                            ManifestResult& a_result, const std::filesystem::path& a_path)
        {
            if (a_value.kind != Json::Value::Kind::kArray) {
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
                if (item.kind != Json::Value::Kind::kString) {
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
    }

    std::string FoldASCII(const std::string_view a_text)
    {
        std::string folded;
        folded.reserve(a_text.size());
        for (const char ch : a_text) {
            folded.push_back(LowerASCII(ch));
        }
        return folded;
    }

    void AddIssue(ManifestResult& a_result, const std::filesystem::path& a_path,
                  const std::size_t a_offset, std::string a_code, std::string a_message)
    {
        a_result.issues.push_back({ a_path, a_offset, std::move(a_code), std::move(a_message) });
    }

    bool HasOnlyProperties(const Json::Value& a_object,
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

    const Json::Value* Require(const Json::Value& a_object, const std::string_view a_name,
                               const Json::Value::Kind a_kind, ManifestResult& a_result,
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

    bool IsPluginName(const std::string_view a_name)
    {
        if (a_name.size() <= 4 || a_name.size() > 260 || a_name.contains('/') || a_name.contains('\\') ||
            a_name.contains(':')) {
            return false;
        }
        const auto folded = FoldASCII(a_name);
        return folded.ends_with(".esm") || folded.ends_with(".esp") || folded.ends_with(".esl");
    }

    bool ParseLocalFormID(const std::string_view a_text, std::uint32_t& a_out)
    {
        if (a_text.empty() || a_text.size() > 8) {
            return false;
        }
        std::uint32_t value = 0;
        const auto [ptr, ec] = std::from_chars(
            a_text.data(), a_text.data() + a_text.size(), value, 16);
        if (ec != std::errc{} || ptr != a_text.data() + a_text.size() ||
            value > 0x00FFFFFF) {
            return false;
        }
        a_out = value;
        return true;
    }

    bool IsWithin(const std::filesystem::path& a_root, const std::filesystem::path& a_candidate)
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

    bool ValidateRelativePath(const std::string& a_text,
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
        if (ContainsParentTraversal(a_text)) {
            a_error = "path contains parent traversal";
            return false;
        }
        const std::filesystem::path relative{ a_text };
        if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
            a_error = "path must be relative";
            return false;
        }
        a_out = relative.lexically_normal();
        return true;
    }

    bool ResolvePresetPath(const std::filesystem::path& a_manifestPath,
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
            a_error = "could not resolve pack directory: " + ec.message();
            return false;
        }
        auto candidate = (root / relative).lexically_normal();
        if (!IsWithin(root, candidate)) {
            a_error = "preset path escapes the pack directory";
            return false;
        }
        if (a_requireFile) {
            const auto canonicalRoot = std::filesystem::weakly_canonical(root, ec);
            if (ec) {
                a_error = "could not canonicalize pack directory: " + ec.message();
                return false;
            }
            const auto canonicalCandidate = std::filesystem::weakly_canonical(candidate, ec);
            if (ec || !IsWithin(canonicalRoot, canonicalCandidate)) {
                a_error = "preset path resolves outside the pack directory";
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

    bool ParseRequirementsNode(const Json::Value& a_node,
                               Requirements& a_requirements,
                               ManifestResult& a_result,
                               const std::filesystem::path& a_path)
    {
        if (a_node.kind != Json::Value::Kind::kObject) {
            AddIssue(a_result, a_path, a_node.offset, "wrong_type",
                     "requirements must be an object");
            return false;
        }
        if (!HasOnlyProperties(a_node, { "plugins", "assets" }, a_result, a_path)) {
            return false;
        }
        if (const auto* plugins = a_node.Find("plugins");
            plugins && !ParseStringArray(*plugins, "plugins", true,
                                         a_requirements.plugins,
                                         a_requirements.assets,
                                         a_result, a_path)) {
            return false;
        }
        if (const auto* assets = a_node.Find("assets");
            assets && !ParseStringArray(*assets, "assets", false,
                                        a_requirements.plugins,
                                        a_requirements.assets,
                                        a_result, a_path)) {
            return false;
        }
        return true;
    }

    bool MergeRequirements(const Requirements& a_package,
                           const Requirements& a_assignment,
                           Requirements& a_out,
                           std::string& a_error,
                           const std::string_view a_implicitPlugin)
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
        if (!a_implicitPlugin.empty() && !addPlugin(a_implicitPlugin)) {
            return false;
        }
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
}
