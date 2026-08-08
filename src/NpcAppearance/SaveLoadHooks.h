#pragma once

#include <functional>

namespace NpcAppearance::SaveLoadHooks
{
    // Observers only: the engine SaveGame/LoadGame gateways always run and
    // no callback can block or alter serialization.
    struct Callbacks
    {
        std::function<void()> onLoadGameReturn;
    };

    [[nodiscard]] bool Install(const Callbacks& a_callbacks);
    [[nodiscard]] bool Operational() noexcept;
}
