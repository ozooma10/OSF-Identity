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

        static_assert(std::is_standard_layout_v<RegistrySlot>);
        static_assert(std::atomic<RE::TESNPC*>::is_always_lock_free);
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
        static_assert(sizeof(std::atomic<RE::TESNPC*>) == sizeof(RE::TESNPC*));
        static_assert(offsetof(RegistrySlot, formID) == 0);
        static_assert(offsetof(RegistrySlot, source) == sizeof(RE::TESNPC*));
        static_assert(offsetof(RegistrySlot, active) == 2 * sizeof(RE::TESNPC*));
        static_assert(offsetof(RegistrySlot, faceTextureIdentity) == 3 * sizeof(RE::TESNPC*));
        static_assert(sizeof(RegistrySlot) == 4 * sizeof(RE::TESNPC*));

        alignas(64) std::array<RegistrySlot, kRegistryCapacity> g_registry;
        std::mutex g_publishMutex;

        [[nodiscard]] std::size_t StartIndex(const RE::TESFormID a_formID) noexcept
        {
            auto value = static_cast<std::uint64_t>(a_formID);
            value *= kHashMultiplier;
            value ^= value >> kFoldShift;
            return static_cast<std::size_t>(value) & kRegistryMask;
        }

        [[nodiscard]] RegistrySlot* FindSlot(const RE::TESFormID a_formID) noexcept
        {
            if (a_formID == 0) {
                return nullptr;
            }

            const auto start = StartIndex(a_formID);
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

        //@TODO: THIS IS GARBAGE. PROBABLY OPTIMIZE BUT THIS IS JUST TEX CREATE PATH SO PROB FINE FOR NOW
        for (const auto& slot : g_registry) {
            if (slot.source.load(std::memory_order_acquire) == a_source) {
                const auto formID = slot.formID.load(std::memory_order_acquire);
                if (formID != 0) {
                    return static_cast<RE::TESFormID>(formID);
                }
            }
        }
        return a_source->GetFormID();
    }

    RE::TESNPC* ResolveFaceTextureIdentityForRenderSource(RE::TESNPC* a_source) noexcept
    {
        if (!a_source || !IsRuntimeOperational()) {
            return a_source;
        }

        //@TODO: THIS IS GARBAGE. PROBABLY OPTIMIZE BUT THIS IS JUST TEX CREATE PATH SO PROB FINE FOR NOW
        for (const auto& slot : g_registry) {
            if (slot.source.load(std::memory_order_acquire) == a_source) {
                auto* identity = slot.faceTextureIdentity.load(std::memory_order_acquire);
                return identity ? identity : a_source;
            }
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
        const auto start = StartIndex(formID);
        for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
            auto& slot = g_registry[(start + probe) & kRegistryMask];
            const auto key = slot.formID.load(std::memory_order_acquire);
            if (key == formID) {
                return { slot.source.load(std::memory_order_relaxed), false };
            }
            if (key == 0) {
                slot.source.store(a_source, std::memory_order_relaxed);
                slot.active.store(0, std::memory_order_relaxed);
                slot.faceTextureIdentity.store(a_faceTextureIdentity, std::memory_order_relaxed);
                slot.formID.store(formID, std::memory_order_release);
                return { a_source, true };
            }
        }

        REX::CRITICAL("[RenderSourceRegistry] render-source registry is full (capacity={})", kRegistryCapacity);
        return {};
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
}
