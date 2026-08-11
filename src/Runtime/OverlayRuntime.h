#pragma once

#include "Config/PreparedAssignment.h"

namespace Runtime
{
    using AssignmentMap = Config::PreparedAssignmentMap;
    
    class OverlayRuntime
    {
    public:
        bool Arm(AssignmentMap assignments);

        std::shared_ptr<const Config::PreparedAssignment> FindAssignment(RE::TESFormID baseID) const;

        void OnReferenceSet3d(RE::TESObjectREFR* a_ref);

        bool IsArmed() const
        {
            return m_armed.load();
        }

    private:
        AssignmentMap m_assignments;
        std::atomic<bool> m_armed{ false };


    private:
        bool TryReserveApply(RE::TESFormID baseID, RE::TESFormID refID);
        void ReleaseInFlight(RE::TESFormID refID);

        bool ApplyTransientOverlay(RE::TESNPC* target, RE::Actor* actor, RE::TESFormID actorRefID, const Config::PreparedAssignment& assignment);
        void DisableBase(RE::TESFormID baseID);
        void RecordSuccessfulApply(RE::TESFormID actorRefId);


        mutable std::mutex m_stateMutex;

        //If npc overlay failed to apply, just disable instead of repeatedly trying to apply it
        std::unordered_set<RE::TESFormID> m_disabledBases;

        // Track refs that are currently being processed to avoid duplicate work
        std::unordered_set<RE::TESFormID> m_inFlightRefs;

        // Track the last time an overlay was applied to a given ref to avoid reapplying too frequently
        std::unordered_map<RE::TESFormID, std::chrono::steady_clock::time_point> m_lastAppliedByRef;
    };

    OverlayRuntime& GetOverlayRuntime();
}
