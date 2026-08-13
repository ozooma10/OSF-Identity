#include "RenderSourceNPC.h"

#include "RuntimeSafety.h"

#include <Util/NativeMainThreadQueue.h>

namespace Runtime
{
    namespace
    {
        void DestroyRejectedConstructedNPC(RE::TESNPC* a_source) noexcept
        {
            const auto references = a_source->QRefCount();
            if (references != 0) {
                REX::CRITICAL("[RenderSourceNPC] rejected detached TESNPC has {} external references; leaking it rather than risking a use-after-free", references);
                return;
            }

            const auto formID = a_source->GetFormID();
            delete a_source;

            if (formID != 0 && RE::TESForm::LookupByID<RE::TESNPC>(formID) != nullptr) {
                KillRuntime("a rejected detached TESNPC did not unregister during destruction");
            }
        }
    }

    RE::TESNPC* CreateRenderSourceNPC() noexcept
    {
        if (!IsRuntimeOperational()) {
            return nullptr;
        }

        const auto queueState = Util::NativeMainThreadQueue::SnapshotState();
        if (!queueState.insideDrain || queueState.currentThreadID != queueState.drainOwnerThreadID) {
            REX::CRITICAL("[RenderSourceNPC] detached TESNPC construction refused outside the native main-thread queue drain (currentTid={} drainOwnerTid={})", queueState.currentThreadID, queueState.drainOwnerThreadID);
            KillRuntime("detached TESNPC construction was requested outside the native main-thread queue");
            return nullptr;
        }

        auto* source = RE::TESNPC::CreateUnregistered();
        if (!source) {
            REX::ERROR("[RenderSourceNPC] engine allocation for detached TESNPC returned null");
            return nullptr;
        }

        const auto formID = source->GetFormID();
        const auto references = source->QRefCount();
        if (formID != 0 || references != 0) {
            REX::CRITICAL("[RenderSourceNPC] unregistered TESNPC violated construction invariants formID=0x{:08X} refs={}; disabling appearance injection", formID, references);
            DestroyRejectedConstructedNPC(source);
            KillRuntime("an unregistered TESNPC acquired a FormID or external reference during construction");
            return nullptr;
        }
        return source;
    }

    void DestroyUnpublishedRenderSource(RE::TESNPC* a_source) noexcept
    {
        if (!a_source) {
            return;
        }
        const auto formID = a_source->GetFormID();
        const auto references = a_source->QRefCount();
        if (formID != 0 || references != 0) {
            REX::CRITICAL("[RenderSourceNPC] refusing to destroy an unpublished render source with formID=0x{:08X} refs={}", formID, references);
            KillRuntime("an unpublished render source acquired a FormID or external reference");
            return;
        }
        delete a_source;
    }
}
