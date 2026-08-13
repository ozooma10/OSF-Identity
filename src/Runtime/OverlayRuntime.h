#pragma once

#include "Config/PreparedAssignment.h"

namespace Runtime
{
    using AssignmentMap = Config::PreparedAssignmentMap;

    class OverlayRuntime
    {
    public:
        bool Arm(AssignmentMap assignments);
        void OnReferenceSet3d(RE::TESObjectREFR* a_ref);
        bool IsArmed() const;

    private:
        enum class BaseStatus
        {
            kDormant,
            kPending,
            kQueued,
            kReady,
            kDisabled
        };

        struct BaseState
        {
            std::shared_ptr<const Config::PreparedAssignment> assignment;
            std::unordered_set<RE::TESFormID> waitingRefs;
            BaseStatus status{ BaseStatus::kDormant };
        };

        bool StartPreparationPump();
        void PumpPendingBases();
        void TryDispatchPendingBase(RE::TESFormID baseID);
        void PreparePendingBase(RE::TESFormID baseID, std::shared_ptr<const Config::PreparedAssignment> assignment);
        void RequeueBase(RE::TESFormID baseID);
        void DisableBaseBeforePublication(RE::TESFormID baseID);
        std::vector<RE::TESFormID> CompletePublication(RE::TESFormID baseID, const std::shared_ptr<const Config::PreparedAssignment>& assignment);
        void UpdatePendingWorkFlagLocked();

        bool RefreshWaitingReference(RE::TESNPC* target, RE::TESNPC* source, RE::TESFormID actorRefID, const Config::PreparedAssignment& assignment);
        static bool HasLoaded3D(RE::Actor* actor);

        std::unordered_map<RE::TESFormID, BaseState> m_bases;
        mutable std::mutex m_stateMutex;
        std::atomic<bool> m_armed{ false };
        std::atomic<bool> m_hasDispatchableBases{ false };
        std::atomic<bool> m_preparationPumpRegistered{ false };
    };

    OverlayRuntime& GetOverlayRuntime();
}
