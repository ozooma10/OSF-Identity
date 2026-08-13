#include "RenderSourceRegistry.h"

#include "RuntimeSafety.h"

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
        constexpr std::size_t kMaxPublishedSources = kRegistryCapacity / 2;
        constexpr std::size_t kRegistryMask = kRegistryCapacity - 1;
        constexpr std::uint8_t kFoldShift = 33;
        constexpr std::uint64_t kHashMultiplier = 0xff51afd7ed558ccdULL;
        static_assert((kRegistryCapacity & kRegistryMask) == 0);
        static_assert(kMaxPublishedSources < kRegistryCapacity);

        struct RegistrySlot
        {
            std::atomic<std::uint64_t> formID{ 0 };
            std::atomic<RE::TESNPC*> source{ nullptr };
            std::atomic<std::uint64_t> active{ 0 };
            std::atomic<RE::TESNPC*> faceTextureIdentity{ nullptr };
        };

        struct SourceIndexSlot
        {
            std::atomic<RE::TESNPC*> source{ nullptr };
            RegistrySlot* registrySlot{ nullptr };
        };

        static_assert(std::is_standard_layout_v<RegistrySlot>);
        static_assert(std::atomic<RE::TESNPC*>::is_always_lock_free);
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
        static_assert(sizeof(std::atomic<RE::TESNPC*>) == sizeof(RE::TESNPC*));
        static_assert(offsetof(RegistrySlot, formID) == 0);
        static_assert(offsetof(RegistrySlot, source) == sizeof(RE::TESNPC*));
        static_assert(offsetof(RegistrySlot, active) == 2 * sizeof(RE::TESNPC*));
        static_assert(offsetof(RegistrySlot, faceTextureIdentity) == 3 * sizeof(RE::TESNPC*));
        static_assert(sizeof(RegistrySlot) == 4 * sizeof(RE::TESNPC*));
        static_assert(std::is_standard_layout_v<SourceIndexSlot>);
        static_assert(offsetof(SourceIndexSlot, source) == 0);
        static_assert(offsetof(SourceIndexSlot, registrySlot) == sizeof(RE::TESNPC*));
        static_assert(sizeof(SourceIndexSlot) == 2 * sizeof(RE::TESNPC*));

        alignas(64) std::array<RegistrySlot, kRegistryCapacity> g_registry;
        alignas(64) std::array<SourceIndexSlot, kRegistryCapacity> g_sourceIndex;
        std::mutex g_publishMutex;
        std::size_t g_publishedSourceCount{ 0 };  // Guarded by g_publishMutex.

        [[nodiscard]] std::size_t StartIndex(std::uint64_t a_key) noexcept
        {
            a_key *= kHashMultiplier;
            a_key ^= a_key >> kFoldShift;
            return static_cast<std::size_t>(a_key) & kRegistryMask;
        }

        [[nodiscard]] std::size_t SourceStartIndex(const RE::TESNPC* a_source) noexcept
        {
            return StartIndex(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_source)));
        }

        [[nodiscard]] RegistrySlot* FindSlot(const RE::TESFormID a_formID) noexcept
        {
            if (a_formID == 0) {
                return nullptr;
            }

            const auto start = StartIndex(static_cast<std::uint64_t>(a_formID));
            for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
                auto& slot = g_registry[(start + probe) & kRegistryMask];
                const auto key = slot.formID.load(std::memory_order_acquire);
                if (key == a_formID) {
                    return &slot;
                }
                if (key == 0) {
                    return nullptr;
                }
            }
            return nullptr;
        }

        [[nodiscard]] RegistrySlot* FindSourceRegistrySlot(const RE::TESNPC* a_source) noexcept
        {
            if (!a_source) {
                return nullptr;
            }

            const auto start = SourceStartIndex(a_source);
            for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
                const auto& slot = g_sourceIndex[(start + probe) & kRegistryMask];
                const auto key = slot.source.load(std::memory_order_acquire);
                if (key == a_source) {
                    return slot.registrySlot;
                }
                if (!key) {
                    return nullptr;
                }
            }
            return nullptr;
        }
    }

    RenderSourceRegistryReadView GetRenderSourceRegistryReadView() noexcept
    {
        return RenderSourceRegistryReadView{
            .runtimeOperational = RuntimeOperationalFlagAddress(),
            .slots = g_registry.data(),
            .capacity = kRegistryCapacity,
            .slotSize = sizeof(RegistrySlot),
            .formIDOffset = offsetof(RegistrySlot, formID),
            .sourceOffset = offsetof(RegistrySlot, source),
            .activeOffset = offsetof(RegistrySlot, active),
            .hashMultiplier = kHashMultiplier,
            .foldShift = kFoldShift
        };
    }

    std::size_t MaxSupportedRenderSources() noexcept
    {
        return kMaxPublishedSources;
    }

    RE::TESNPC* ResolveRenderSource(RE::TESNPC* a_canonical) noexcept
    {
        if (!a_canonical) {
            return nullptr;
        }
        if (!IsRuntimeOperational()) {
            return a_canonical;
        }

        auto* slot = FindSlot(a_canonical->GetFormID());
        if (!slot || slot->active.load(std::memory_order_acquire) == 0) {
            return a_canonical;
        }

        auto* source = slot->source.load(std::memory_order_relaxed);
        return source ? source : a_canonical;
    }

    RE::TESNPC* FindOwnedRenderSource(RE::TESNPC* a_canonical) noexcept
    {
        if (!a_canonical || !IsRuntimeOperational()) {
            return nullptr;
        }

        auto* slot = FindSlot(a_canonical->GetFormID());
        return slot ? slot->source.load(std::memory_order_relaxed) : nullptr;
    }

    RE::TESFormID ResolveRuntimeFormIDForRenderSource(RE::TESNPC* a_source) noexcept
    {
        if (!a_source) {
            return 0;
        }
        if (!IsRuntimeOperational()) {
            return a_source->GetFormID();
        }

        auto* slot = FindSourceRegistrySlot(a_source);
        if (slot) {
            const auto formID = slot->formID.load(std::memory_order_relaxed);
            if (formID != 0) {
                return static_cast<RE::TESFormID>(formID);
            }
        }
        return a_source->GetFormID();
    }

    RE::TESNPC* ResolveFaceTextureIdentityForRenderSource(RE::TESNPC* a_source) noexcept
    {
        if (!a_source || !IsRuntimeOperational()) {
            return a_source;
        }

        auto* slot = FindSourceRegistrySlot(a_source);
        if (slot) {
            auto* identity = slot->faceTextureIdentity.load(std::memory_order_relaxed);
            return identity ? identity : a_source;
        }
        return a_source;
    }

    RenderSourcePublishResult PublishRenderSource(RE::TESNPC* a_canonical, RE::TESNPC* a_source, RE::TESNPC* a_faceTextureIdentity) noexcept
    {
        if (!a_canonical || !a_source || !a_faceTextureIdentity || a_canonical == a_source) {
            return {};
        }

        const auto formID = a_canonical->GetFormID();
        if (formID == 0) {
            return {};
        }

        const std::scoped_lock lock{ g_publishMutex };
        const auto start = StartIndex(static_cast<std::uint64_t>(formID));
        RegistrySlot* publishSlot = nullptr;
        for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
            auto& slot = g_registry[(start + probe) & kRegistryMask];
            const auto key = slot.formID.load(std::memory_order_acquire);
            if (key == formID) {
                return { slot.source.load(std::memory_order_relaxed), false };
            }
            if (key == 0) {
                publishSlot = &slot;
                break;
            }
        }

        if (g_publishedSourceCount >= kMaxPublishedSources) {
            REX::CRITICAL("[RenderSourceRegistry] published render-source limit reached (published={}, limit={})", g_publishedSourceCount, kMaxPublishedSources);
            return {};
        }

        if (!publishSlot) {
            REX::CRITICAL("[RenderSourceRegistry] render-source registry is full (capacity={})", kRegistryCapacity);
            return {};
        }

        const auto sourceStart = SourceStartIndex(a_source);
        SourceIndexSlot* sourcePublishSlot = nullptr;
        for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
            auto& slot = g_sourceIndex[(sourceStart + probe) & kRegistryMask];
            const auto key = slot.source.load(std::memory_order_acquire);
            if (key == a_source) {
                REX::CRITICAL("[RenderSourceRegistry] render source is already indexed to another runtime FormID (source={})", static_cast<const void*>(a_source));
                return {};
            }
            if (!key) {
                sourcePublishSlot = &slot;
                break;
            }
        }

        if (!sourcePublishSlot) {
            REX::CRITICAL("[RenderSourceRegistry] render-source pointer index is full (capacity={})", kRegistryCapacity);
            return {};
        }

        publishSlot->source.store(a_source, std::memory_order_relaxed);
        publishSlot->active.store(0, std::memory_order_relaxed);
        publishSlot->faceTextureIdentity.store(a_faceTextureIdentity, std::memory_order_relaxed);
        publishSlot->formID.store(formID, std::memory_order_release);

        sourcePublishSlot->registrySlot = publishSlot;
        sourcePublishSlot->source.store(a_source, std::memory_order_release);
        ++g_publishedSourceCount;
        return { a_source, true };
    }

    bool ActivateRenderSource(RE::TESNPC* a_canonical, RE::TESNPC* a_source) noexcept
    {
        if (!a_canonical || !a_source || a_canonical == a_source) {
            return false;
        }

        auto* slot = FindSlot(a_canonical->GetFormID());
        if (!slot || slot->source.load(std::memory_order_relaxed) != a_source) {
            return false;
        }

        slot->active.store(1, std::memory_order_release);
        return true;
    }

    bool DeactivateRenderSource(RE::TESNPC* a_canonical, RE::TESNPC* a_source) noexcept
    {
        if (!a_canonical || !a_source || a_canonical == a_source) {
            return false;
        }

        auto* slot = FindSlot(a_canonical->GetFormID());
        if (!slot || slot->source.load(std::memory_order_relaxed) != a_source) {
            return false;
        }

        std::uint64_t expected = 1;
        return slot->active.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }
}
