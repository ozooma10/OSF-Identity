#pragma once

#include <functional>

namespace NpcAppearance::SaveLoadHooks
{
    struct Callbacks
    {
        // Return false to veto the engine SaveGame call.
        // Veto is available only when OSF Identity owns the validated hook pair directly.
        std::function<bool()> onSaveGameEntry;
        std::function<void()> onSaveGameReturn;
        std::function<void()> onLoadGameReturn;
    };

    [[nodiscard]] bool Install(const Callbacks& a_callbacks);
    [[nodiscard]] bool Operational() noexcept;
    [[nodiscard]] bool SupportsSaveVeto() noexcept;
}
