#pragma once

#include <functional>

namespace NpcAppearance::SaveLoadHooks
{
    struct Callbacks
    {
        std::function<void()> onSaveGameEntry;
        std::function<void()> onSaveGameReturn;
        std::function<void()> onLoadGameReturn;
    };

    [[nodiscard]] bool Install(const Callbacks& a_callbacks);
    [[nodiscard]] bool Operational() noexcept;
}
