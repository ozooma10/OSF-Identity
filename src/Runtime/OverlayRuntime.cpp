#include "OverlayRuntime.h"
#include "MutationSafety.h"
#include "NPCRestorePoint.h"
#include "NPCPresetApplicator.h"

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
        if (!IsMutationOperational()) {
            REX::WARN("[OverlayRuntime] Arm() called after mutation was disabled; overlay runtime remains disabled");
            return false;
        }

        if (m_armed.load()) {
            REX::WARN("[OverlayRuntime] Arm() called while already armed; assignments are replaced");
        }
        m_assignments = std::move(assignments);
        m_armed.store(true);
        return true;
    }

    bool OverlayRuntime::IsArmed() const
    {
        return m_armed.load(std::memory_order_acquire) && IsMutationOperational();
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
        if (!TryReserveApply(baseID, refID)) {
            return;
        }

        const auto result = Util::NativeMainThreadQueue::Post(
            [this, refID, baseID, assignment = std::move(assignment)] {
                try {
                    auto* actor = RE::TESForm::LookupByID<RE::Actor>(refID);
                    auto* target = RE::TESForm::LookupByID<RE::TESNPC>(baseID);
                    if (actor && target && actor->GetNPC() == target) {
                        static_cast<void>(ApplyTransientOverlay(target, actor, refID, *assignment));
                    }
                } catch (const std::exception& error) {
                    REX::CRITICAL("[OverlayRuntime] overlay apply for ref=0x{:08X} threw: {}", refID, error.what());
                } catch (...) {
                    REX::CRITICAL("[OverlayRuntime] overlay apply for ref=0x{:08X} threw an unknown exception", refID);
                }
                ReleaseInFlight(refID);
            },
            "OverlayRuntime.OverlayApply",
            [this, refID]() {
                ReleaseInFlight(refID);
                REX::WARN("[OverlayRuntime] overlay apply for ref=0x{:08X} dropped by the native queue; will retry at the next 3D build", refID);
            }
        );

        if(result == Util::NativeMainThreadQueue::PostResult::kUnavailable) {
            ReleaseInFlight(refID);
            REX::WARN("[OverlayRuntime] native queue unavailable for overlay apply for ref=0x{:08X}; will retry at the next 3D build", refID);
        }
    }


    bool Runtime::OverlayRuntime::TryReserveApply(RE::TESFormID baseID, RE::TESFormID refID)
    {
        if (!IsMutationOperational()) {
            return false;
        }

        const std::scoped_lock lock{ m_stateMutex };

        if(m_disabledBases.contains(baseID)) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto last = m_lastAppliedByRef.find(refID);

        if(last != m_lastAppliedByRef.end() && now - last->second < kOverlayReapplyCooldown) {
            return false;
        }

        return m_inFlightRefs.insert(refID).second;
    }

    void Runtime::OverlayRuntime::ReleaseInFlight(RE::TESFormID refID)
    {
        const std::scoped_lock lock{ m_stateMutex };
        m_inFlightRefs.erase(refID);
    }

    bool OverlayRuntime::ApplyTransientOverlay(RE::TESNPC *target, RE::Actor *actor, RE::TESFormID actorRefID, const Config::PreparedAssignment &assignment)
    {
        if (!IsMutationOperational() || !target || !actor || actor->GetNPC() != target || actor->GetFormID() != actorRefID) {
            return false;
        }

        const auto baseID = target->GetFormID();
        if (assignment.baseFormID != baseID) {
            DisableBase(baseID);
            REX::WARN( "[OverlayRuntime] prepared assignment no longer matches base=0x{:08X}; overlay disabled for that base", baseID);
            return false;
        }
        if (!target->bodyMorphValues || target->bodyMorphValues->size() != assignment.preset.bodyMorphRegionValues.size()) {
            DisableBase(baseID);
            REX::WARN("[OverlayRuntime] base=0x{:08X} does not have canonical body-morph storage; overlay disabled before mutation", baseID);
            return false;
        }

        const auto started = std::chrono::steady_clock::now();
        auto restorePoint = NPCRestorePoint::Capture(target);
        if (!restorePoint) {
            if (IsMutationOperational()) {
                DisableBase(baseID);
                REX::WARN("[OverlayRuntime] could not capture an exact restore point for base=0x{:08X}; overlay disabled for that base", baseID);
            }
            return false;
        }

        const auto& originalState = restorePoint->OriginalState();
        bool appearanceApplied = false;
        bool presetDonorReleased = true;
        bool actorRefreshed = false;
        try {
            const auto applyResult = ApplyPreparedAppearance(target, assignment.preset, assignment.dependencies, originalState);
            appearanceApplied = applyResult.applied;
            presetDonorReleased = applyResult.donorReleased;
            if (appearanceApplied) {
                actorRefreshed = NotifyAndRefreshAppearance(target, actor, actorRefID);
            }
        } catch (const std::exception& error) {
            REX::ERROR("[OverlayRuntime] transient apply for base=0x{:08X} ref=0x{:08X} threw before restore: {}", baseID, actorRefID, error.what());
        } catch (...) {
            REX::ERROR("[OverlayRuntime] transient apply for base=0x{:08X} ref=0x{:08X} threw an unknown exception before restore", baseID, actorRefID);
        }

        bool restored = false;
        try {
            restored = restorePoint->RestoreExact(target);
        } catch (const std::exception& error) {
            REX::CRITICAL("[OverlayRuntime] exact restore for base=0x{:08X} ref=0x{:08X} threw: {}", baseID, actorRefID, error.what());
        } catch (...) {
            REX::CRITICAL("[OverlayRuntime] exact restore for base=0x{:08X} ref=0x{:08X} threw an unknown exception", baseID, actorRefID);
        }

        bool restorePointReleased = false;
        try {
            restorePointReleased = restorePoint->ReleaseAndVerify();
        } catch (const std::exception& error) {
            REX::CRITICAL("[OverlayRuntime] restore-point teardown for base=0x{:08X} ref=0x{:08X} threw: {}", baseID, actorRefID, error.what());
        } catch (...) {
            REX::CRITICAL("[OverlayRuntime] restore-point teardown for base=0x{:08X} ref=0x{:08X} threw an unknown exception", baseID, actorRefID);
        }

        if (!restored || !restorePointReleased || !presetDonorReleased) {
            KillMutation(!restored ? "an in-window restore failed" : !restorePointReleased ? "a restore donor did not unregister" : "a preset donor did not unregister");
            return false;
        }

        if (!appearanceApplied || !actorRefreshed) {
            DisableBase(baseID);
            REX::WARN("[OverlayRuntime] transient overlay failed safely for base=0x{:08X} ref=0x{:08X} applied={} refreshed={}; rendering vanilla and disabling that base", baseID, actorRefID, appearanceApplied, actorRefreshed);
            return false;
        }

        RecordSuccessfulApply(actorRefID);
        REX::DEBUG("[OverlayRuntime] transient overlay completed for base=0x{:08X} ref=0x{:08X} pack='{}' in {} us", baseID, actorRefID, assignment.packID, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
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
}
