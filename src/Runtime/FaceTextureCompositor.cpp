#include "FaceTextureCompositor.h"

#include "RenderSourceRegistry.h"
#include "RuntimeSafety.h"

#include <Util/NativeMainThreadQueue.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <new>

namespace Runtime
{
    namespace
    {
        constexpr std::size_t kOutputSize = 0x48;
        constexpr std::size_t kRequestCountOffset = 0x38;

        struct ContractSite
        {
            std::string_view name;
            REL::ID functionID;
            std::ptrdiff_t offset;
            std::array<std::uint8_t, 25> expected;
            std::size_t size;
        };

        // 1.16.244: the original CharGen state machine proves the submission argument order, output layout, readiness/finalization entry points, and output cleanup routine used below.
        constexpr std::array<ContractSite, 6> kContractSites{
            ContractSite{ "CharGen.submitCall", REL::ID(69553), 0x1CB, { 0x45, 0x33, 0xC9, 0x4C, 0x8D, 0x05, 0x3B, 0x71, 0x17, 0x05, 0x48, 0x8B, 0x97, 0x28, 0x5B, 0x00, 0x00, 0x49, 0x8B, 0xCA, 0xE8, 0xBC, 0x33, 0x00, 0x00 }, 25 },
            ContractSite{ "CharGen.outputConstructorCall", REL::ID(69538), 0xA7, { 0x48, 0x8D, 0x8F, 0x90, 0x5B, 0x00, 0x00, 0xE8, 0x6D, 0xD2, 0x00, 0x00 }, 12 },
            ContractSite{ "FaceTextureComposite.ready", REL::ID(69633), 0x0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x48, 0x8B, 0x41, 0x30, 0x41, 0xB3 }, 16 },
            ContractSite{ "FaceTextureComposite.finalize", REL::ID(69634), 0x0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x81, 0xEC, 0xB0, 0x00 }, 16 },
            ContractSite{ "FaceTextureComposite.construct", REL::ID(69637), 0x0, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x56 }, 16 },
            ContractSite{ "FaceTextureComposite.destroyRequests", REL::ID(40952), 0x0, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x79, 0x30, 0x00, 0x48, 0x8B }, 16 }
        };

        using ConstructOutput = void (*)(void*);
        using SubmitComposite = void (*)(RE::TESNPC*, void*, void*, bool);
        using CompositeReady = bool (*)(void*);
        using FinalizeComposite = void (*)(void*);
        using DestroyRequests = void (*)(void*);

        [[nodiscard]] bool InsideNativeDrain(const std::string_view a_operation) noexcept
        {
            const auto queueState = Util::NativeMainThreadQueue::SnapshotState();
            if (queueState.insideDrain && queueState.currentThreadID == queueState.drainOwnerThreadID) {
                return true;
            }

            REX::CRITICAL("[FaceTextureCompositor] {} refused outside the native main-thread queue drain (currentTid={} drainOwnerTid={})",
                a_operation, queueState.currentThreadID, queueState.drainOwnerThreadID);
            KillRuntime("face-texture composition was requested outside the native main-thread queue");
            return false;
        }

        [[nodiscard]] std::uint32_t RequestCount(const FaceTextureComposite* a_composite) noexcept;
    }

    struct alignas(16) FaceTextureComposite
    {
        std::array<std::byte, kOutputSize> storage{};
        bool submitted{ false };
        bool finalized{ false };
    };

    static_assert(alignof(FaceTextureComposite) >= alignof(void*));

    namespace
    {
        std::uint32_t RequestCount(const FaceTextureComposite* a_composite) noexcept
        {
            std::uint32_t count = 0;
            if (a_composite) {
                std::memcpy(&count, a_composite->storage.data() + kRequestCountOffset, sizeof(count));
            }
            return count;
        }
    }

    bool PreflightFaceTextureCompositorContract() noexcept
    {
        bool valid = true;
        for (const auto& site : kContractSites) {
            const REL::Relocation<std::uintptr_t> location{ site.functionID, site.offset };
            if (std::memcmp(reinterpret_cast<const void*>(location.address()), site.expected.data(), site.size) != 0) {
                REX::CRITICAL("[FaceTextureCompositor] byte gate failed at '{}' (Address Library ID {} + 0x{:X})",
                    site.name, site.functionID.id(), site.offset);
                valid = false;
            }
        }
        return valid;
    }

    bool NeedsFaceTextureComposite(const RE::TESNPC* a_source) noexcept
    {
        return a_source && !a_source->tintAVMData.empty();
    }

    FaceTextureComposite* CreateFaceTextureComposite() noexcept
    {
        if (!IsRuntimeOperational() || !InsideNativeDrain("output construction")) {
            return nullptr;
        }

        auto* composite = new (std::nothrow) FaceTextureComposite{};
        if (!composite) {
            REX::ERROR("[FaceTextureCompositor] could not allocate the 0x{:X}-byte output carrier", kOutputSize);
            return nullptr;
        }

        static REL::Relocation<ConstructOutput> construct{ REL::ID(69637) };
        construct(composite->storage.data());
        if (RequestCount(composite) != 0) {
            REX::CRITICAL("[FaceTextureCompositor] newly constructed output carrier was not empty");
            delete composite;
            KillRuntime("the engine face-texture output constructor violated its empty-state contract");
            return nullptr;
        }
        return composite;
    }

    void DestroyUnstartedFaceTextureComposite(FaceTextureComposite* a_composite) noexcept
    {
        if (!a_composite) {
            return;
        }
        if (a_composite->submitted) {
            REX::CRITICAL("[FaceTextureCompositor] refusing to destroy a submitted output carrier");
            KillRuntime("submitted face-texture output ownership was released");
            return;
        }
        if (!InsideNativeDrain("unstarted output destruction")) {
            return;
        }

        static REL::Relocation<DestroyRequests> destroyRequests{ REL::ID(40952) };
        destroyRequests(a_composite->storage.data() + 8);
        delete a_composite;
    }

    bool StartFaceTextureComposite(FaceTextureComposite* a_composite, RE::TESNPC* a_canonical, RE::TESNPC* a_source) noexcept
    {
        if (!a_composite || a_composite->submitted || a_composite->finalized || !a_canonical || !a_source ||
            a_source->GetFormID() != 0 || ResolveCanonicalForRenderSource(a_source) != a_canonical ||
            !NeedsFaceTextureComposite(a_source) || !IsRuntimeOperational() || !InsideNativeDrain("submission")) {
            return false;
        }

        static REL::Relocation<SubmitComposite> submit{ REL::ID(69597) };
        static REL::Relocation<std::uintptr_t> faceTextureNames{ REL::ID(910759) };
        submit(a_source, a_composite->storage.data(), reinterpret_cast<void*>(faceTextureNames.address()), false);

        const auto requestCount = RequestCount(a_composite);
        if (requestCount == 0) {
            REX::ERROR("[FaceTextureCompositor] engine submission produced no face-texture requests for base=0x{:08X}", a_canonical->GetFormID());
            return false;
        }

        a_composite->submitted = true;
        REX::DEBUG("[FaceTextureCompositor] submitted {} generated face-texture requests for base=0x{:08X} layers={}",
            requestCount, a_canonical->GetFormID(), a_source->tintAVMData.size());
        return true;
    }

    bool IsFaceTextureCompositeReady(FaceTextureComposite* a_composite) noexcept
    {
        if (!a_composite || !a_composite->submitted || a_composite->finalized || !IsRuntimeOperational() || !InsideNativeDrain("readiness poll")) {
            return false;
        }

        static REL::Relocation<CompositeReady> ready{ REL::ID(69633) };
        return ready(a_composite->storage.data());
    }

    bool FinalizeFaceTextureComposite(FaceTextureComposite* a_composite) noexcept
    {
        if (!a_composite || !a_composite->submitted || a_composite->finalized || !IsRuntimeOperational() || !InsideNativeDrain("finalization")) {
            return false;
        }

        static REL::Relocation<FinalizeComposite> finalize{ REL::ID(69634) };
        finalize(a_composite->storage.data());
        a_composite->finalized = true;
        return true;
    }
}
