#include "ConfigDetail.h"

#include "Preset.h"
#include "Util/String.h"

#include <charconv>
#include <system_error>
#include <utility>

namespace Config::Detail
{
    namespace
    {
        [[nodiscard]] bool PathComponentEquals(const std::filesystem::path& a_left, const std::filesystem::path& a_right)
        {
#ifdef _WIN32
            return _wcsicmp(a_left.c_str(), a_right.c_str()) == 0;
#else
            return a_left == a_right;
#endif
        }

        [[nodiscard]] bool ContainsParentTraversal(std::string_view a_text) noexcept
        {
            while (true) {
                const auto separator = a_text.find_first_of("/\\");
                if (a_text.substr(0, separator) == "..") {
                    return true;
                }
                if (separator == std::string_view::npos) {
                    return false;
                }
                a_text.remove_prefix(separator + 1);
            }
        }
    }

    void AddIssue(DiscoveryResult& a_result, const std::filesystem::path& a_path, const std::size_t a_offset, std::string a_code, std::string a_message)
    {
        a_result.issues.push_back({ a_path, a_offset, std::move(a_code), std::move(a_message) });
    }

    bool IsPluginName(const std::string_view a_name)
    {
        if (a_name.size() <= 4 || a_name.size() > 260 || a_name.contains('/') || a_name.contains('\\') ||
            a_name.contains(':')) {
            return false;
        }
        const auto folded = Util::FoldASCII(a_name);
        return folded.ends_with(".esm") || folded.ends_with(".esp") || folded.ends_with(".esl");
    }

    bool ParseLocalFormID(const std::string_view a_text, std::uint32_t& a_out)
    {
        if (a_text.empty() || a_text.size() > 8) {
            return false;
        }
        std::uint32_t value = 0;
        const auto [ptr, ec] = std::from_chars( a_text.data(), a_text.data() + a_text.size(), value, 16);
        if (ec != std::errc{} || ptr != a_text.data() + a_text.size() || value > 0x00FFFFFF) {
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

    bool ValidateRelativePath(const std::string& a_text, std::filesystem::path& a_out, std::string& a_error)
    {
        if (a_text.empty()) {
            a_error = "path is empty";
            return false;
        }
        // Reject Windows-style roots (drive letters, leading separators) explicitly so validation behaves identically on every host
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

    bool ResolvePresetPath(const std::filesystem::path& a_packPath, const std::string& a_text, const bool a_requireFile, std::filesystem::path& a_out, std::string& a_error)
    {
        std::filesystem::path relative;
        if (!ValidateRelativePath(a_text, relative, a_error)) {
            return false;
        }
        if (Util::FoldASCII(relative.extension().string()) != ".npc") {
            a_error = "preset path must use the .npc extension";
            return false;
        }

        std::error_code ec;
        const auto root = std::filesystem::absolute(a_packPath, ec).lexically_normal();
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
            const auto size = std::filesystem::file_size(canonicalCandidate, ec);
            if (ec) {
                a_error = "could not determine preset size: " + ec.message();
                return false;
            }
            if (size == 0 || size > kMaxPresetBytes) {
                a_error = "preset file size is outside the accepted range";
                return false;
            }
            candidate = canonicalCandidate;
        }
        a_out = std::move(candidate);
        return true;
    }
}
