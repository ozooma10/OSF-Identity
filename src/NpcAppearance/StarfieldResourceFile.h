#pragma once

#include "NpcAppearance/Preset.h"

#include <filesystem>

namespace NpcAppearance
{
    // Resolves a known Data-relative path through Starfield's mounted resource
    // system, preserving the engine's loose-file-over-archive precedence.
    [[nodiscard]] bool StarfieldResourceExists(
        const std::filesystem::path& a_dataRelativePath);

    // Maps a validated preset path below the loose-discovered pack root to its
    // fixed Data-relative resource path, then reads it through Starfield's VFS.
    [[nodiscard]] PresetResult LoadStarfieldCkPreset(
        const std::filesystem::path& a_packsRoot,
        const std::filesystem::path& a_presetPath);
}
