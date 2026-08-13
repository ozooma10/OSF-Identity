#pragma once

#include <cstddef>
#include <cstdint>

namespace Runtime
{
    struct RenderSourceRegistryReadView
    {
        const void* runtimeOperational{ nullptr };
        const void* slots{ nullptr };
        std::size_t capacity{ 0 };
        std::size_t slotSize{ 0 };
        std::size_t canonicalOffset{ 0 };
        std::size_t sourceOffset{ 0 };
        std::uint64_t hashMultiplier{ 0 };
        std::uint8_t pointerShift{ 0 };
        std::uint8_t foldShift{ 0 };
    };

    struct RenderSourcePublishResult
    {
        RE::TESNPC* source{ nullptr };
        bool adopted{ false };
    };

    // Installation-time description used by generated x86-64 read thunks.
    // The registry storage and its published canonical keys are process-lifetime stable.
    [[nodiscard]] RenderSourceRegistryReadView GetRenderSourceRegistryReadView() noexcept;

    // Reference implementation for native callers. Generated engine thunks mirror this lookup directly and must remain behaviorally identical to it.
    [[nodiscard]] RE::TESNPC* ResolveRenderSource(RE::TESNPC* a_canonical) noexcept;

    [[nodiscard]] RE::TESNPC* FindRenderSource(RE::TESNPC* a_canonical) noexcept;

    [[nodiscard]] RenderSourcePublishResult PublishRenderSource(RE::TESNPC* a_canonical, RE::TESNPC* a_source) noexcept;
}
