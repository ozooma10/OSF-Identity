#include "OverlayRuntime.h"
#include "NPCRestorePoint.h"
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
        if(assignments.empty()) {
            REX::WARN("[OverlayRuntime] Arm() called with empty assignments; overlay runtime remains disabled");
            return false;
        }

        if (m_armed.load()) {
            REX::WARN("[OverlayRuntime] Arm() called while already armed; assignments are replaced");
        }
        m_assignments = std::move(assignments);
        m_armed.store(true);
        return true;
    }

    std::shared_ptr<const Config::PreparedAssignment> Runtime::OverlayRuntime::FindAssignment(RE::TESFormID baseID) const
    {
        if (!m_armed.load()) {
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
        if(!target || !actor || actor->GetNPC() != target || actor->GetFormID() != actorRefID) {
            return false;
        }

        const auto baseID = target->GetFormID();

        return false;
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
