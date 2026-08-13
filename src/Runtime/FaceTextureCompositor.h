#pragma once

namespace Runtime
{
    struct FaceTextureComposite;

    [[nodiscard]] bool PreflightFaceTextureCompositorContract() noexcept;
    [[nodiscard]] bool NeedsFaceTextureComposite(const RE::TESNPC* a_source) noexcept;

    // The output is process-lifetime once submission starts. Render-graph work and FaceDB publication retain resources beyond the initiating task.
    [[nodiscard]] FaceTextureComposite* CreateFaceTextureComposite() noexcept;
    void DestroyUnstartedFaceTextureComposite(FaceTextureComposite* a_composite) noexcept;

    [[nodiscard]] bool StartFaceTextureComposite(FaceTextureComposite* a_composite, RE::TESNPC* a_canonical, RE::TESNPC* a_source) noexcept;
    [[nodiscard]] bool IsFaceTextureCompositeReady(FaceTextureComposite* a_composite) noexcept;
    [[nodiscard]] bool FinalizeFaceTextureComposite(FaceTextureComposite* a_composite) noexcept;
}
