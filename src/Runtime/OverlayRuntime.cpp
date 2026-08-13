#include "OverlayRuntime.h"
#include "NPCPresetApplicator.h"
#include "NPCSnapshot.h"
#include "RenderSourceNPC.h"
#include "RenderSourceRegistry.h"
#include "RuntimeSafety.h"

#include <Util/NativeMainThreadQueue.h>

namespace Runtime
{
    constexpr std::chrono::milliseconds kOverlayReapplyCooldown{ 1000 };


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

        if (m_armed.load()) {
            REX::WARN("[OverlayRuntime] Arm() called while already armed; assignments are replaced");
        }

        if(!StartRetryPump()) {
            REX::WARN("[OverlayRuntime] retry pump could not be started; overlay runtime remains disabled");
            return false;
        }

        m_assignments = std::move(assignments);
        m_armed.store(true, std::memory_order_release);
        return true;
    }

    bool OverlayRuntime::IsArmed() const
    {
        return m_armed.load(std::memory_order_acquire) && IsRuntimeOperational();
    }

    std::shared_ptr<const Config::PreparedAssignment> Runtime::OverlayRuntime::FindAssignment(RE::TESFormID baseID) const
    {
        if (!IsArmed()) {
            return {};
        }
        const auto found = m_assignments.find(baseID);
        if (found == m_assignments.end()) {
            return {};
        }
        return found->second;
    }

    void Runtime::OverlayRuntime::OnReferenceSet3d(RE::TESObjectREFR * a_ref)
    {
        auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
        auto* base = actor ? actor->GetNPC() : nullptr;
        if(!base) {
            return;
        }

        const auto baseID = base->GetFormID();
        auto assignment = FindAssignment(baseID);
        if(!assignment) {
            return;
        }

        const auto refID = actor->GetFormID();

        if(!RememberPendingApply(baseID, refID, std::move(assignment))) {
            return;
        }

        TryDispatchPending(refID);
    }


    bool Runtime::OverlayRuntime::RememberPendingApply(RE::TESFormID baseID, RE::TESFormID refID, std::shared_ptr<const Config::PreparedAssignment> assignment)
    {
        if (!IsRuntimeOperational() || !assignment) {
            return false;
        }

        const std::scoped_lock lock{ m_stateMutex };

        if(m_disabledBases.contains(baseID)) {
            return false;
        }

        if(m_inFlightRefs.contains(refID)) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto last = m_lastAppliedByRef.find(refID);

        if(last != m_lastAppliedByRef.end() && now - last->second < kOverlayReapplyCooldown) {
            return false;
        }

        m_pendingApplies.insert_or_assign(refID, PendingApply{ baseID, std::move(assignment) });
        return true;
    }

    bool OverlayRuntime::ApplyRenderSource(RE::TESNPC *target, RE::Actor *actor, RE::TESFormID actorRefID, const Config::PreparedAssignment &assignment)
    {
        if (!IsRuntimeOperational() || !target || !actor || actor->GetNPC() != target || actor->GetFormID() != actorRefID) {
            return false;
        }

        const auto baseID = target->GetFormID();
        if (assignment.baseFormID != baseID) {
            DisableBase(baseID);
            REX::WARN( "[OverlayRuntime] prepared assignment no longer matches base=0x{:08X}; overlay disabled for that base", baseID);
            return false;
        }
        if (!assignment.preset.bodyMorphRegionValues.empty() && (!target->bodyMorphValues || target->bodyMorphValues->size() != assignment.preset.bodyMorphRegionValues.size())) {
            DisableBase(baseID);
            REX::WARN("[OverlayRuntime] base=0x{:08X} does not have the expected body-morph storage; detached render source was not created", baseID);
            return false;
        }

        const auto started = std::chrono::steady_clock::now();
        auto* source = FindRenderSource(target);
        if (!source) {
            auto* prepared = PrepareRenderSource(target, assignment.preset, assignment.dependencies);
            if (!prepared) {
                if (IsRuntimeOperational()) {
                    DisableBase(baseID);
                    REX::WARN("[OverlayRuntime] detached render source could not be prepared for base=0x{:08X}; rendering vanilla and disabling that base", baseID);
                }
                return false;
            }

            const auto published = PublishRenderSource(target, prepared);
            if (!published.source) {
                DestroyUnpublishedRenderSource(prepared);
                KillRuntime("the immutable render-source registry could not publish a prepared carrier");
                return false;
            }
            if (!published.adopted) {
                DestroyUnpublishedRenderSource(prepared);
            }
            source = published.source;
        }

        const auto canonicalState = CaptureOriginalNPCState(target);
        const auto canonicalStorage = CaptureVisualStorageState(target);
        bool actorRefreshed = false;
        try {
            actorRefreshed = RefreshAppearanceFromRenderSource(target, source, actor, actorRefID);
        } catch (const std::exception& error) {
            REX::ERROR("[OverlayRuntime] detached refresh for base=0x{:08X} ref=0x{:08X} threw: {}", baseID, actorRefID, error.what());
        } catch (...) {
            REX::ERROR("[OverlayRuntime] detached refresh for base=0x{:08X} ref=0x{:08X} threw an unknown exception", baseID, actorRefID);
        }

        const bool canonicalPreserved = CaptureOriginalNPCState(target) == canonicalState && CaptureVisualStorageState(target) == canonicalStorage;
        if (!canonicalPreserved) {
            KillRuntime("the canonical TESNPC changed during a detached appearance refresh");
            return false;
        }

        if (!actorRefreshed) {
            DisableBase(baseID);
            REX::WARN("[OverlayRuntime] detached refresh failed safely for base=0x{:08X} ref=0x{:08X}; rendering vanilla and disabling that base", baseID, actorRefID);
            return false;
        }

        RecordSuccessfulApply(actorRefID);
        REX::DEBUG("[OverlayRuntime] detached render-source refresh completed for base=0x{:08X} ref=0x{:08X} pack='{}' in {} us", baseID, actorRefID, assignment.packID, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
        return true;
    }

    void OverlayRuntime::DisableBase(RE::TESFormID baseID)
    {
        const std::scoped_lock lock{ m_stateMutex };
        m_disabledBases.insert(baseID);
    }

    void OverlayRuntime::RecordSuccessfulApply(RE::TESFormID actorRefId)
    {
        const std::scoped_lock lock{ m_stateMutex };
        m_lastAppliedByRef[actorRefId] = std::chrono::steady_clock::now();
    }

    bool OverlayRuntime::StartRetryPump()
    {
        bool expected = false;
        if (!m_retryPumpRegistered.compare_exchange_strong(expected, true)) {
            return true;
        }

        auto* taskInterface = SFSE::GetTaskInterface();
        if (!taskInterface) {
            m_retryPumpRegistered.store(false);
            REX::ERROR("[OverlayRuntime] SFSE task interface unavailable");
            return false;
        }

        taskInterface->AddPermanentTask([this] {
            PumpPendingApplies();
        });

        REX::DEBUG("[OverlayRuntime] pending-apply retry pump registered");
        return true;
    }

    void OverlayRuntime::PumpPendingApplies()
    {
        if (!IsArmed() || !Util::NativeMainThreadQueue::IsAvailable()) {
            return;
        }

        RE::TESFormID refID = 0;
        {
            const std::scoped_lock lock{ m_stateMutex };
            for (auto it = m_pendingApplies.begin(); it != m_pendingApplies.end(); it++) {
                if (!m_inFlightRefs.contains(it->first)) {
                    refID = it->first;
                    break;
                }
            }
        }

        if (refID != 0) {
            TryDispatchPending(refID);
        }
    }

    void OverlayRuntime::TryDispatchPending(RE::TESFormID refID)
    {
        if (!Util::NativeMainThreadQueue::IsAvailable()) {
            REX::DEBUG("[OverlayRuntime] overlay apply for ref=0x{:08X} pending while native queue is unavailable", refID);
            return;
        }

        PendingApply pending;
        {
            const std::scoped_lock lock{ m_stateMutex };

            const auto found = m_pendingApplies.find(refID);
            if (found == m_pendingApplies.end()) { return; }

            if (m_disabledBases.contains(found->second.baseID)) {
                m_pendingApplies.erase(found);
                return;
            }

            if (!m_inFlightRefs.insert(refID).second) {
                return;
            }

            pending = found->second;
        }

        const auto baseID = pending.baseID;
        auto assignment = std::move(pending.assignment);

        const auto result = Util::NativeMainThreadQueue::Post([this, refID, baseID, assignment = std::move(assignment)] {
            try {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(refID);
                auto* target = RE::TESForm::LookupByID<RE::TESNPC>(baseID);

                if (actor && target && actor->GetNPC() == target && HasLoaded3D(actor)) {
                    ApplyRenderSource(target, actor, refID, *assignment);
                } else {
                    REX::DEBUG("[OverlayRuntime] pending overlay for ref=0x{:08X} is no longer loaded or valid; discarding", refID);
                }
            } catch (const std::exception& error) {
                REX::CRITICAL("[OverlayRuntime] overlay apply for ref=0x{:08X} threw: {}", refID, error.what());
            } catch (...) {
                REX::CRITICAL("[OverlayRuntime] overlay apply for ref=0x{:08X} threw an unknown exception", refID);
            }

            CompletePendingApply(refID);
        },
        "OverlayRuntime.OverlayApply",
        [this, refID] {
            ReleaseInFlight(refID);

            REX::WARN("[OverlayRuntime] overlay apply for ref=0x{:08X} dropped by the native queue; retained for retry", refID);
        });

        if (result == Util::NativeMainThreadQueue::PostResult::kUnavailable) {
            ReleaseInFlight(refID);

            REX::DEBUG("[OverlayRuntime] native queue became unavailable for ref=0x{:08X}; apply remains pending", refID);
        } else {
            REX::DEBUG("[OverlayRuntime] overlay apply dispatch for ref=0x{:08X}: {}", refID, Util::NativeMainThreadQueue::ToString(result));
        }
    }

    void Runtime::OverlayRuntime::ReleaseInFlight(RE::TESFormID refID)
    {
        const std::scoped_lock lock{ m_stateMutex };
        m_inFlightRefs.erase(refID);
    }

    void OverlayRuntime::CompletePendingApply(RE::TESFormID refID)
    {
        const std::scoped_lock lock{ m_stateMutex };
        m_inFlightRefs.erase(refID);
        m_pendingApplies.erase(refID);
    }

    bool OverlayRuntime::HasLoaded3D(RE::Actor *actor)
    {
        if(!actor) { return false; }

        const auto loaded = actor->loadedData.LockRead();
        return *loaded && (*loaded)->data3D;
    }
}
