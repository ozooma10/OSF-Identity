#pragma once

#include <cstddef>
#include <cstdint>

namespace Runtime
{
    struct RenderSourceRegistryReadView
    {
        const void *runtimeOperational{nullptr};
        const void *slots{nullptr};
        std::size_t capacity{0};
        std::size_t slotSize{0};
        std::size_t formIDOffset{0};
        std::size_t sourceOffset{0};
        std::size_t activeOffset{0};
        std::uint64_t hashMultiplier{0};
        std::uint8_t foldShift{0};
    };

    struct RenderSourcePublishResult
    {
        RE::TESNPC *source{nullptr};
        bool adopted{false};
    };

    // Installation-time description used by generated x86-64 read thunks.
    // The registry storage and its published runtime FormID keys are process-lifetime stable.
    [[nodiscard]] RenderSourceRegistryReadView GetRenderSourceRegistryReadView() noexcept;

    // Keeps the immutable hot-path table at or below its supported load factor.
    [[nodiscard]] std::size_t MaxSupportedRenderSources() noexcept;

    // Reference implementation for native callers. Generated engine thunks mirror this lookup directly and must remain behaviorally identical to it.
    // Staged sources remain invisible here until ActivateRenderSource publishes them to world rendering.
    [[nodiscard]] RE::TESNPC *ResolveRenderSource(RE::TESNPC *a_canonical) noexcept;

    // Finds a source owned by the registry whether it is staged or active.
    [[nodiscard]] RE::TESNPC *FindOwnedRenderSource(RE::TESNPC *a_canonical) noexcept;

    // Runtime FormID reverse lookup used by the compositor and FaceDB selection path.
    [[nodiscard]] RE::TESFormID ResolveRuntimeFormIDForRenderSource(RE::TESNPC *a_source) noexcept;

    // Stable configured NPC identity used only by FaceDB texture naming. A dynamic leveled base has no plugin owner and cannot serve as a persistent generated-texture key.
    [[nodiscard]] RE::TESNPC *ResolveFaceTextureIdentityForRenderSource(RE::TESNPC *a_source) noexcept;

    [[nodiscard]] RenderSourcePublishResult PublishRenderSource(RE::TESNPC *a_canonical, RE::TESNPC *a_source, RE::TESNPC *a_faceTextureIdentity) noexcept;
    [[nodiscard]] bool ActivateRenderSource(RE::TESNPC *a_canonical, RE::TESNPC *a_source) noexcept;
}
