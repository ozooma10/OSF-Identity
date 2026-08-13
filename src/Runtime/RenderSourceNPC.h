#pragma once

namespace Runtime
{
    RE::TESNPC* CreateRenderSourceNPC() noexcept;

    void DestroyUnpublishedRenderSource(RE::TESNPC* a_source) noexcept;
}
