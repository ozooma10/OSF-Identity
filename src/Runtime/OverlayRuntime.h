#pragma once

#include "Config/PreparedAssignment.h"
#include <chrono>

namespace Runtime
{
    struct FaceTextureComposite;

    using AssignmentMap = Config::PreparedAssignmentMap;

    class OverlayRuntime
    {
    public:
        bool Arm(AssignmentMap assignments);
        void OnReferenceSet3d(RE::TESObjectREFR *a_ref);
        bool IsArmed() const;

    private:
        enum class BaseStatus
        {
            kDormant,
            kPending,
            kQueued,
            kCompositePending,
            kCompositeQueued,
            kCompositeFinalized,
            kCompositeActivationQueued,
            kVanillaRefreshPending,
            kVanillaRefreshQueued,
            kReady,
            kDisabled
        };

        struct BaseState
        {
            std::shared_ptr<const Config::PreparedAssignment> assignment;
            RE::TESFormID configuredBaseID{0};
            std::unordered_set<RE::TESFormID> waitingRefs;
            FaceTextureComposite *faceTextureComposite{nullptr};
            std::chrono::steady_clock::time_point compositionStarted{};
            bool publicationBarrierArmed{false};
            BaseStatus status{BaseStatus::kDormant};
        };

        bool StartPreparationPump();
        void PumpPendingBases();
        void TryDispatchPendingBase(RE::TESFormID baseID);
        void PreparePendingBase(RE::TESFormID baseID, std::shared_ptr<const Config::PreparedAssignment> assignment);
        void RequeueBase(RE::TESFormID baseID);
        void TryDispatchCompositePoll(RE::TESFormID baseID);
        void PollPendingComposite(RE::TESFormID baseID, std::shared_ptr<const Config::PreparedAssignment> assignment, FaceTextureComposite *composite);
        void RequeueComposite(RE::TESFormID baseID);
        void TryDispatchCompositeActivation(RE::TESFormID baseID);
        void PollCompositeActivation(RE::TESFormID baseID, std::shared_ptr<const Config::PreparedAssignment> assignment, FaceTextureComposite *composite);
        void RequeueCompositeActivation(RE::TESFormID baseID);
        void TryDispatchVanillaRefresh(RE::TESFormID baseID);
        void RefreshVanillaAfterIdentityMismatch(RE::TESFormID baseID);
        void RequeueVanillaRefresh(RE::TESFormID baseID);
        void DisableBaseBeforePublication(RE::TESFormID baseID);
        std::vector<RE::TESFormID> CompletePublication(RE::TESFormID baseID, const std::shared_ptr<const Config::PreparedAssignment> &assignment, RE::TESNPC *canonical, RE::TESNPC *source);
        void ReleasePreparationOwner(RE::TESFormID baseID);
        void UpdatePendingWorkFlagLocked();

        bool RefreshWaitingReference(RE::TESNPC *target, RE::TESNPC *source, RE::TESFormID actorRefID, const Config::PreparedAssignment &assignment);
        static bool HasLoaded3D(RE::Actor *actor);

        AssignmentMap m_configuredAssignments;
        std::unordered_map<RE::TESFormID, BaseState> m_bases;
        RE::TESFormID m_preparationOwnerBaseID{0};
        mutable std::mutex m_stateMutex;
        std::atomic<bool> m_armed{false};
        std::atomic<bool> m_hasDispatchableBases{false};
        std::atomic<bool> m_preparationPumpRegistered{false};
    };

    OverlayRuntime &GetOverlayRuntime();
}
