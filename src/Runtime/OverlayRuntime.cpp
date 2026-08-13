#include "OverlayRuntime.h"

#include "NPCPresetApplicator.h"
#include "NPCSnapshot.h"
#include "RenderSourceNPC.h"
#include "RenderSourceRegistry.h"
#include "RuntimeSafety.h"

#include <Util/NativeMainThreadQueue.h>

namespace Runtime
{
    OverlayRuntime& GetOverlayRuntime()
    {
        static OverlayRuntime instance;
        return instance;
    }

    bool OverlayRuntime::Arm(AssignmentMap assignments)
    {
        if (assignments.empty()) {
            REX::WARN("[OverlayRuntime] Arm() called with empty assignments; overlay runtime remains disabled");
            return false;
        }
        if (!IsRuntimeOperational()) {
            REX::WARN("[OverlayRuntime] Arm() called after appearance injection was disabled; overlay runtime remains disabled");
            return false;
        }
        if (m_armed.load(std::memory_order_acquire)) {
            REX::WARN("[OverlayRuntime] Arm() called while already armed; immutable assignments were not replaced");
            return false;
        }
        if (!StartPreparationPump()) {
            REX::WARN("[OverlayRuntime] render-source preparation pump could not be started; overlay runtime remains disabled");
            return false;
        }

        std::unordered_map<RE::TESFormID, BaseState> bases;
        bases.reserve(assignments.size());
        for (auto& [baseID, assignment] : assignments) {
            bases.emplace(baseID, BaseState{ .assignment = std::move(assignment) });
        }

        {
            const std::scoped_lock lock{ m_stateMutex };
            if (m_armed.load(std::memory_order_relaxed)) {
                REX::WARN("[OverlayRuntime] concurrent Arm() call lost the one-shot publication race");
                return false;
            }
            m_bases = std::move(bases);
            m_armed.store(true, std::memory_order_release);
        }
        return true;
    }

    bool OverlayRuntime::IsArmed() const
    {
        return m_armed.load(std::memory_order_acquire) && IsRuntimeOperational();
    }

    void OverlayRuntime::OnReferenceSet3d(RE::TESObjectREFR* a_ref)
    {
        if (!IsArmed()) {
            return;
        }

        auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
        auto* base = actor ? actor->GetNPC() : nullptr;
        if (!base || FindRenderSource(base)) {
            return;
        }

        const auto baseID = base->GetFormID();
        const auto refID = actor->GetFormID();
        bool dispatch = false;
        bool recheckReadyState = false;
        {
            const std::scoped_lock lock{ m_stateMutex };
            const auto found = m_bases.find(baseID);
            if (found == m_bases.end() || found->second.status == BaseStatus::kDisabled) {
                return;
            }

            auto& state = found->second;
            if (state.status == BaseStatus::kReady) {
                recheckReadyState = true;
            } else {
                state.waitingRefs.insert(refID);
                if (state.status == BaseStatus::kDormant) {
                    state.status = BaseStatus::kPending;
                    m_hasPendingBases.store(true, std::memory_order_release);
                    dispatch = true;
                }
            }
        }

        if (recheckReadyState) {
            if (!FindRenderSource(base)) {
                KillRuntime("a base marked ready had no published render source");
            }
        } else if (dispatch) {
            TryDispatchPendingBase(baseID);
        }
    }

    bool OverlayRuntime::StartPreparationPump()
    {
        bool expected = false;
        if (!m_preparationPumpRegistered.compare_exchange_strong(expected, true)) {
            return true;
        }

        auto* taskInterface = SFSE::GetTaskInterface();
        if (!taskInterface) {
            m_preparationPumpRegistered.store(false);
            REX::ERROR("[OverlayRuntime] SFSE task interface unavailable");
            return false;
        }

        taskInterface->AddPermanentTask([this] {
            PumpPendingBases();
        });

        REX::DEBUG("[OverlayRuntime] pending-base preparation pump registered");
        return true;
    }

    void OverlayRuntime::PumpPendingBases()
    {
        if (!m_hasPendingBases.load(std::memory_order_acquire) || !IsArmed() || !Util::NativeMainThreadQueue::IsAvailable()) {
            return;
        }

        RE::TESFormID baseID = 0;
        {
            const std::scoped_lock lock{ m_stateMutex };
            for (const auto& [candidateID, state] : m_bases) {
                if (state.status == BaseStatus::kPending) {
                    baseID = candidateID;
                    break;
                }
            }
            if (baseID == 0) {
                UpdatePendingWorkFlagLocked();
            }
        }

        if (baseID != 0) {
            TryDispatchPendingBase(baseID);
        }
    }

    void OverlayRuntime::TryDispatchPendingBase(const RE::TESFormID baseID)
    {
        if (!Util::NativeMainThreadQueue::IsAvailable()) {
            REX::DEBUG("[OverlayRuntime] render source for base=0x{:08X} pending while native queue is unavailable", baseID);
            return;
        }

        std::shared_ptr<const Config::PreparedAssignment> assignment;
        {
            const std::scoped_lock lock{ m_stateMutex };
            const auto found = m_bases.find(baseID);
            if (found == m_bases.end() || found->second.status != BaseStatus::kPending || !found->second.assignment) {
                return;
            }
            found->second.status = BaseStatus::kQueued;
            assignment = found->second.assignment;
            UpdatePendingWorkFlagLocked();
        }

        const auto result = Util::NativeMainThreadQueue::Post([this, baseID, assignment = std::move(assignment)] {
            try {
                PreparePendingBase(baseID, assignment);
            } catch (const std::exception& error) {
                auto* target = RE::TESForm::LookupByID<RE::TESNPC>(baseID);
                if (target && FindRenderSource(target)) {
                    KillRuntime("render-source preparation threw after immutable publication");
                } else {
                    DisableBaseBeforePublication(baseID);
                }
                REX::CRITICAL("[OverlayRuntime] render-source preparation for base=0x{:08X} threw: {}", baseID, error.what());
            } catch (...) {
                auto* target = RE::TESForm::LookupByID<RE::TESNPC>(baseID);
                if (target && FindRenderSource(target)) {
                    KillRuntime("render-source preparation threw after immutable publication");
                } else {
                    DisableBaseBeforePublication(baseID);
                }
                REX::CRITICAL("[OverlayRuntime] render-source preparation for base=0x{:08X} threw an unknown exception", baseID);
            }
        },
        "OverlayRuntime.PrepareRenderSource",
        [this, baseID] {
            RequeueBase(baseID);
            REX::WARN("[OverlayRuntime] render-source preparation for base=0x{:08X} was dropped by the native queue; retained for retry", baseID);
        });

        if (result == Util::NativeMainThreadQueue::PostResult::kUnavailable) {
            RequeueBase(baseID);
            REX::DEBUG("[OverlayRuntime] native queue became unavailable for base=0x{:08X}; preparation remains pending", baseID);
        } else {
            REX::DEBUG("[OverlayRuntime] render-source preparation dispatch for base=0x{:08X}: {}", baseID, Util::NativeMainThreadQueue::ToString(result));
        }
    }

    void OverlayRuntime::PreparePendingBase(const RE::TESFormID baseID, const std::shared_ptr<const Config::PreparedAssignment> assignment)
    {
        if (!IsRuntimeOperational() || !assignment) {
            DisableBaseBeforePublication(baseID);
            return;
        }

        auto* target = RE::TESForm::LookupByID<RE::TESNPC>(baseID);
        if (!target || assignment->baseFormID != baseID) {
            DisableBaseBeforePublication(baseID);
            REX::WARN("[OverlayRuntime] prepared assignment no longer resolves to base=0x{:08X}; rendering vanilla and disabling that base", baseID);
            return;
        }
        if (!assignment->preset.bodyMorphRegionValues.empty() &&
            (!target->bodyMorphValues || target->bodyMorphValues->size() != assignment->preset.bodyMorphRegionValues.size())) {
            DisableBaseBeforePublication(baseID);
            REX::WARN("[OverlayRuntime] base=0x{:08X} does not have the expected body-morph storage; rendering vanilla and disabling that base", baseID);
            return;
        }

        const auto started = std::chrono::steady_clock::now();
        auto* source = FindRenderSource(target);
        if (!source) {
            auto* prepared = PrepareRenderSource(target, assignment->preset, assignment->dependencies);
            if (!prepared) {
                DisableBaseBeforePublication(baseID);
                if (IsRuntimeOperational()) {
                    REX::WARN("[OverlayRuntime] detached render source could not be prepared for base=0x{:08X}; rendering vanilla and disabling that base", baseID);
                }
                return;
            }

            const auto published = PublishRenderSource(target, prepared);
            if (!published.source) {
                DestroyUnpublishedRenderSource(prepared);
                DisableBaseBeforePublication(baseID);
                KillRuntime("the immutable render-source registry could not publish a prepared carrier");
                return;
            }
            if (!published.adopted) {
                DestroyUnpublishedRenderSource(prepared);
            }
            source = published.source;
        }

        auto waitingRefs = CompletePublication(baseID);
        REX::DEBUG("[OverlayRuntime] detached render source published for base=0x{:08X} pack='{}' waitingRefs={} in {} us",
            baseID, assignment->packID, waitingRefs.size(), std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());

        for (const auto refID : waitingRefs) {
            if (!IsRuntimeOperational()) {
                break;
            }
            RefreshWaitingReference(target, source, refID, *assignment);
        }
    }

    void OverlayRuntime::RequeueBase(const RE::TESFormID baseID)
    {
        const std::scoped_lock lock{ m_stateMutex };
        const auto found = m_bases.find(baseID);
        if (found != m_bases.end() && found->second.status == BaseStatus::kQueued) {
            found->second.status = BaseStatus::kPending;
        }
        UpdatePendingWorkFlagLocked();
    }

    void OverlayRuntime::DisableBaseBeforePublication(const RE::TESFormID baseID)
    {
        const std::scoped_lock lock{ m_stateMutex };
        const auto found = m_bases.find(baseID);
        if (found != m_bases.end()) {
            found->second.status = BaseStatus::kDisabled;
            found->second.waitingRefs.clear();
            found->second.assignment.reset();
        }
        UpdatePendingWorkFlagLocked();
    }

    std::vector<RE::TESFormID> OverlayRuntime::CompletePublication(const RE::TESFormID baseID)
    {
        std::vector<RE::TESFormID> waitingRefs;
        const std::scoped_lock lock{ m_stateMutex };
        const auto found = m_bases.find(baseID);
        if (found == m_bases.end()) {
            KillRuntime("a published render source had no owning base state");
            return waitingRefs;
        }

        auto& state = found->second;
        waitingRefs.assign(state.waitingRefs.begin(), state.waitingRefs.end());
        state.waitingRefs.clear();
        state.assignment.reset();
        state.status = BaseStatus::kReady;
        UpdatePendingWorkFlagLocked();
        return waitingRefs;
    }

    void OverlayRuntime::UpdatePendingWorkFlagLocked()
    {
        const auto hasPending = std::ranges::any_of(m_bases, [](const auto& entry) {
            return entry.second.status == BaseStatus::kPending || entry.second.status == BaseStatus::kQueued;
        });
        m_hasPendingBases.store(hasPending, std::memory_order_release);
    }

    bool OverlayRuntime::RefreshWaitingReference(RE::TESNPC* target, RE::TESNPC* source, const RE::TESFormID actorRefID, const Config::PreparedAssignment& assignment)
    {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorRefID);
        if (!actor || actor->GetNPC() != target || !HasLoaded3D(actor)) {
            REX::DEBUG("[OverlayRuntime] waiting ref=0x{:08X} for base=0x{:08X} is no longer loaded or valid; catch-up refresh skipped", actorRefID, target->GetFormID());
            return false;
        }

        const auto canonicalState = CaptureOriginalNPCState(target);
        const auto canonicalStorage = CaptureVisualStorageState(target);
        bool refreshed = false;
        try {
            refreshed = RefreshAppearanceFromRenderSource(target, source, actor, actorRefID);
        } catch (const std::exception& error) {
            REX::ERROR("[OverlayRuntime] detached catch-up refresh for base=0x{:08X} ref=0x{:08X} threw: {}", target->GetFormID(), actorRefID, error.what());
        } catch (...) {
            REX::ERROR("[OverlayRuntime] detached catch-up refresh for base=0x{:08X} ref=0x{:08X} threw an unknown exception", target->GetFormID(), actorRefID);
        }

        const bool canonicalPreserved = CaptureOriginalNPCState(target) == canonicalState && CaptureVisualStorageState(target) == canonicalStorage;
        if (!canonicalPreserved) {
            KillRuntime("the canonical TESNPC changed during a detached appearance refresh");
            return false;
        }
        if (!refreshed) {
            REX::WARN("[OverlayRuntime] catch-up refresh failed for published base=0x{:08X} ref=0x{:08X}; future 3D builds can still use the published source", target->GetFormID(), actorRefID);
            return false;
        }

        REX::DEBUG("[OverlayRuntime] detached catch-up refresh completed for base=0x{:08X} ref=0x{:08X} pack='{}'", target->GetFormID(), actorRefID, assignment.packID);
        return true;
    }

    bool OverlayRuntime::HasLoaded3D(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        const auto loaded = actor->loadedData.LockRead();
        return *loaded && (*loaded)->data3D;
    }
}
