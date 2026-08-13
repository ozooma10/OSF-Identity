#include "RenderSourceRegistry.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>

namespace Runtime
{
    namespace
    {
        constexpr std::size_t kRegistryCapacity = 4096;
        constexpr std::size_t kRegistryMask = kRegistryCapacity - 1;
        constexpr std::uint8_t kPointerShift = 4;
        constexpr std::uint8_t kFoldShift = 33;
        constexpr std::uint64_t kHashMultiplier = 0xff51afd7ed558ccdULL;
        static_assert((kRegistryCapacity & kRegistryMask) == 0);

        struct RegistrySlot
        {
            std::atomic<RE::TESNPC*> canonical{ nullptr };
            std::atomic<RE::TESNPC*> source{ nullptr };
        };

        static_assert(std::is_standard_layout_v<RegistrySlot>);
        static_assert(std::atomic<RE::TESNPC*>::is_always_lock_free);
        static_assert(sizeof(std::atomic<RE::TESNPC*>) == sizeof(RE::TESNPC*));
        static_assert(offsetof(RegistrySlot, canonical) == 0);
        static_assert(offsetof(RegistrySlot, source) == sizeof(RE::TESNPC*));
        static_assert(sizeof(RegistrySlot) == 2 * sizeof(RE::TESNPC*));

        alignas(64) std::array<RegistrySlot, kRegistryCapacity> g_registry;
        std::mutex g_publishMutex;

        [[nodiscard]] std::size_t StartIndex(RE::TESNPC* a_canonical) noexcept
        {
            auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_canonical) >> kPointerShift);
            value ^= value >> kFoldShift;
            value *= kHashMultiplier;
            value ^= value >> kFoldShift;
            return static_cast<std::size_t>(value) & kRegistryMask;
        }
    }

    RenderSourceRegistryReadView GetRenderSourceRegistryReadView() noexcept
    {
        return RenderSourceRegistryReadView{
            .slots = g_registry.data(),
            .capacity = kRegistryCapacity,
            .slotSize = sizeof(RegistrySlot),
            .canonicalOffset = offsetof(RegistrySlot, canonical),
            .sourceOffset = offsetof(RegistrySlot, source),
            .hashMultiplier = kHashMultiplier,
            .pointerShift = kPointerShift,
            .foldShift = kFoldShift
        };
    }

    RE::TESNPC* ResolveRenderSource(RE::TESNPC* a_canonical) noexcept
    {
        if (!a_canonical) {
            return nullptr;
        }

        const auto start = StartIndex(a_canonical);
        for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
            const auto& slot = g_registry[(start + probe) & kRegistryMask];
            auto* canonical = slot.canonical.load(std::memory_order_acquire);
            if (canonical == a_canonical) {
                auto* source = slot.source.load(std::memory_order_relaxed);
                return source ? source : a_canonical;
            }
            if (!canonical) {
                return a_canonical;
            }
        }
        return a_canonical;
    }

    RE::TESNPC* FindRenderSource(RE::TESNPC* a_canonical) noexcept
    {
        auto* resolved = ResolveRenderSource(a_canonical);
        return resolved != a_canonical ? resolved : nullptr;
    }

    RenderSourcePublishResult PublishRenderSource(RE::TESNPC* a_canonical, RE::TESNPC* a_source) noexcept
    {
        if (!a_canonical || !a_source || a_canonical == a_source) {
            return {};
        }

        const std::scoped_lock lock{ g_publishMutex };
        const auto start = StartIndex(a_canonical);
        for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
            auto& slot = g_registry[(start + probe) & kRegistryMask];
            auto* canonical = slot.canonical.load(std::memory_order_acquire);
            if (canonical == a_canonical) {
                return { slot.source.load(std::memory_order_relaxed), false };
            }
            if (!canonical) {
                slot.source.store(a_source, std::memory_order_relaxed);
                slot.canonical.store(a_canonical, std::memory_order_release);
                return { a_source, true };
            }
        }

        REX::CRITICAL("[RenderSourceRegistry] immutable render-source registry is full (capacity={})", kRegistryCapacity);
        return {};
    }
}
