#include "OverlayRuntime.h"

#include "FaceTextureCompositor.h"
#include "NPCPresetApplicator.h"
#include "NPCSnapshot.h"
#include "RenderSourceNPC.h"
#include "RenderSourceRegistry.h"
#include "RuntimeSafety.h"

#include <Util/NativeMainThreadQueue.h>

namespace Runtime
{
    namespace
    {
        RE::TESBoundObject *GetConfiguredLeveledBase(RE::TESObjectREFR *a_ref)
        {
            if (!a_ref || !a_ref->extraDataList)
            {
                return nullptr;
            }

            using func_t = RE::TESBoundObject *(*)(RE::ExtraDataList *);
            static REL::Relocation<func_t> func{REL::ID(44957)};
            return func(a_ref->extraDataList.get());
        }
    }

    OverlayRuntime &GetOverlayRuntime()
    {
        static OverlayRuntime instance;
        return instance;
    }

    bool OverlayRuntime::Arm(AssignmentMap assignments)
    {
        if (assignments.empty())
        {
            REX::WARN("[OverlayRuntime] Arm() called with empty assignments; overlay runtime remains disabled");
            return false;
        }
        if (!IsRuntimeOperational())
        {
            REX::WARN("[OverlayRuntime] Arm() called after appearance injection was disabled; overlay runtime remains disabled");
            return false;
        }
        if (m_armed.load(std::memory_order_acquire))
        {
            REX::WARN("[OverlayRuntime] Arm() called while already armed; immutable assignments were not replaced");
            return false;
        }
        if (!StartPreparationPump())
        {
            REX::WARN("[OverlayRuntime] render-source preparation pump could not be started; overlay runtime remains disabled");
            return false;
        }

        AssignmentMap configuredAssignments;
        configuredAssignments.reserve(assignments.size());
        std::unordered_map<RE::TESFormID, BaseState> bases;
        bases.reserve(assignments.size());
        for (auto &[baseID, assignment] : assignments)
        {
            configuredAssignments.emplace(baseID, assignment);
            bases.emplace(baseID, BaseState{.assignment = std::move(assignment), .configuredBaseID = baseID});
        }

        {
            const std::scoped_lock lock{m_stateMutex};
            if (m_armed.load(std::memory_order_relaxed))
            {
                REX::WARN("[OverlayRuntime] concurrent Arm() call lost the one-shot publication race");
                return false;
            }
            m_configuredAssignments = std::move(configuredAssignments);
            m_bases = std::move(bases);
            m_armed.store(true, std::memory_order_release);
        }
        return true;
    }

    bool OverlayRuntime::IsArmed() const
    {
        return m_armed.load(std::memory_order_acquire) && IsRuntimeOperational();
    }

    void OverlayRuntime::OnReferenceSet3d(RE::TESObjectREFR *a_ref)
    {
        if (!IsArmed())
        {
            return;
        }

        auto *actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
        auto *base = actor ? actor->GetNPC() : nullptr;
        if (!base)
        {
            return;
        }

        auto *ownedSource = FindOwnedRenderSource(base);
        const auto baseID = base->GetFormID();
        const auto refID = actor->GetFormID();
        auto *configuredBase = GetConfiguredLeveledBase(actor);
        const auto configuredBaseID = configuredBase ? configuredBase->GetFormID() : RE::TESFormID{0};
        const auto observedConfiguredBaseID = base->IsCreated() ? configuredBaseID : baseID;
        bool dispatch = false;
        bool invalidReadyState = false;
        bool inheritedAssignment = false;
        bool readyIdentityMismatch = false;
        bool inFlightIdentityMismatch = false;
        bool identityFallbackAlreadyPending = false;
        RE::TESFormID expectedConfiguredBaseID = 0;
        {
            const std::scoped_lock lock{m_stateMutex};
            auto found = m_bases.find(baseID);
            if (found == m_bases.end())
            {
                const auto configured = m_configuredAssignments.find(configuredBaseID);
                if (configured != m_configuredAssignments.end() && configuredBaseID != baseID && configured->second)
                {
                    found = m_bases.emplace(baseID, BaseState{.assignment = configured->second, .configuredBaseID = configuredBaseID}).first;
                    inheritedAssignment = true;
                }
            }

            if (found == m_bases.end() && ownedSource)
            {
                KillRuntime("a published render source had no owning base state");
                return;
            }
            if (found == m_bases.end() || found->second.status == BaseStatus::kDisabled)
            {
                return;
            }

            auto &state = found->second;
            expectedConfiguredBaseID = state.configuredBaseID;
            if (state.configuredBaseID == 0 || state.configuredBaseID != observedConfiguredBaseID)
            {
                if (state.status == BaseStatus::kReady && ownedSource)
                {
                    state.status = BaseStatus::kVanillaRefreshPending;
                    state.waitingRefs.clear();
                    state.waitingRefs.insert(refID);
                    state.assignment.reset();
                    m_hasDispatchableBases.store(true, std::memory_order_release);
                    readyIdentityMismatch = true;
                }
                else if (state.status == BaseStatus::kVanillaRefreshPending || state.status == BaseStatus::kVanillaRefreshQueued)
                {
                    state.waitingRefs.insert(refID);
                    identityFallbackAlreadyPending = true;
                }
                else
                {
                    inFlightIdentityMismatch = true;
                }
            }
            else if (state.status == BaseStatus::kReady)
            {
                invalidReadyState = !ownedSource;
            }
            else
            {
                state.waitingRefs.insert(refID);
                if (state.status == BaseStatus::kDormant)
                {
                    state.status = BaseStatus::kPending;
                    m_hasDispatchableBases.store(true, std::memory_order_release);
                    dispatch = true;
                }
            }
        }

        if (inheritedAssignment)
        {
            REX::DEBUG("[OverlayRuntime] configured leveled actor mapped: ref=0x{:08X} configuredBase=0x{:08X} runtimeBase=0x{:08X}",
                       refID, configuredBaseID, baseID);
        }
        if (identityFallbackAlreadyPending)
        {
            return;
        }
        if (inFlightIdentityMismatch)
        {
            REX::CRITICAL("[OverlayRuntime] runtime FormID identity changed before publication: runtimeBase=0x{:08X} expectedConfiguredBase=0x{:08X} observedConfiguredBase=0x{:08X} ref=0x{:08X}; disabling appearance injection",
                          baseID, expectedConfiguredBaseID, observedConfiguredBaseID, refID);
            KillRuntime("a runtime FormID changed configured-base identity during render-source preparation");
        }
        else if (readyIdentityMismatch)
        {
            if (!DeactivateRenderSource(base, ownedSource))
            {
                KillRuntime("a reused runtime FormID could not deactivate its stale render-source binding");
                return;
            }
            REX::WARN("[OverlayRuntime] runtime FormID reuse detected: runtimeBase=0x{:08X} expectedConfiguredBase=0x{:08X} observedConfiguredBase=0x{:08X} ref=0x{:08X}; stale render-source binding disabled and vanilla refresh queued",
                      baseID, expectedConfiguredBaseID, observedConfiguredBaseID, refID);
            TryDispatchVanillaRefresh(baseID);
        }
        else if (invalidReadyState)
        {
            KillRuntime("a base marked ready had no active FormID render-source binding");
        }
        else if (dispatch)
        {
            TryDispatchPendingBase(baseID);
        }
    }

    bool OverlayRuntime::StartPreparationPump()
    {
        bool expected = false;
        if (!m_preparationPumpRegistered.compare_exchange_strong(expected, true))
        {
            return true;
        }

        auto *taskInterface = SFSE::GetTaskInterface();
        if (!taskInterface)
        {
            m_preparationPumpRegistered.store(false);
            REX::ERROR("[OverlayRuntime] SFSE task interface unavailable");
            return false;
        }

        taskInterface->AddPermanentTask([this]
                                        { PumpPendingBases(); });

        REX::DEBUG("[OverlayRuntime] pending-base preparation pump registered");
        return true;
    }

    void OverlayRuntime::PumpPendingBases()
    {
        if (!m_hasDispatchableBases.load(std::memory_order_acquire) || !IsArmed() || !Util::NativeMainThreadQueue::IsAvailable())
        {
            return;
        }

        RE::TESFormID baseID = 0;
        bool pollComposite = false;
        bool pollActivation = false;
        bool refreshVanilla = false;
        bool invalidOwner = false;
        {
            const std::scoped_lock lock{m_stateMutex};
            if (m_preparationOwnerBaseID != 0)
            {
                const auto found = m_bases.find(m_preparationOwnerBaseID);
                if (found == m_bases.end())
                {
                    invalidOwner = true;
                }
                else
                {
                    const auto &state = found->second;
                    switch (state.status)
                    {
                    case BaseStatus::kPending:
                        baseID = m_preparationOwnerBaseID;
                        break;
                    case BaseStatus::kCompositePending:
                        baseID = m_preparationOwnerBaseID;
                        pollComposite = true;
                        break;
                    case BaseStatus::kCompositeFinalized:
                        baseID = m_preparationOwnerBaseID;
                        pollActivation = true;
                        break;
                    case BaseStatus::kQueued:
                    case BaseStatus::kCompositeQueued:
                    case BaseStatus::kCompositeActivationQueued:
                        break;
                    default:
                        invalidOwner = true;
                        break;
                    }
                }
            }
            else
            {
                for (const auto &[candidateID, state] : m_bases)
                {
                    if (state.status == BaseStatus::kVanillaRefreshPending)
                    {
                        baseID = candidateID;
                        refreshVanilla = true;
                        break;
                    }
                }
                if (baseID == 0)
                {
                    for (const auto &[candidateID, state] : m_bases)
                    {
                        if (state.status == BaseStatus::kPending)
                        {
                            baseID = candidateID;
                            break;
                        }
                    }
                }
            }
            if (baseID == 0)
            {
                UpdatePendingWorkFlagLocked();
            }
        }

        if (invalidOwner)
        {
            KillRuntime("the single-flight preparation owner crossed an invalid base-state transition");
            return;
        }

        if (baseID != 0)
        {
            if (refreshVanilla)
            {
                TryDispatchVanillaRefresh(baseID);
            }
            else if (pollActivation)
            {
                TryDispatchCompositeActivation(baseID);
            }
            else if (pollComposite)
            {
                TryDispatchCompositePoll(baseID);
            }
            else
            {
                TryDispatchPendingBase(baseID);
            }
        }
    }

    void OverlayRuntime::TryDispatchPendingBase(const RE::TESFormID baseID)
    {
        std::shared_ptr<const Config::PreparedAssignment> assignment;
        bool acquiredOwner = false;
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (found == m_bases.end() || found->second.status != BaseStatus::kPending || !found->second.assignment)
            {
                return;
            }
            if (m_preparationOwnerBaseID != 0 && m_preparationOwnerBaseID != baseID)
            {
                UpdatePendingWorkFlagLocked();
                return;
            }
            if (m_preparationOwnerBaseID == 0)
            {
                m_preparationOwnerBaseID = baseID;
                acquiredOwner = true;
            }
            found->second.status = BaseStatus::kQueued;
            assignment = found->second.assignment;
            UpdatePendingWorkFlagLocked();
        }

        if (acquiredOwner)
        {
            REX::DEBUG("[OverlayRuntime] single-flight preparation ownership acquired for base=0x{:08X}", baseID);
        }

        const auto result = Util::NativeMainThreadQueue::Post([this, baseID, assignment = std::move(assignment)]
                                                              {
            try {
                PreparePendingBase(baseID, assignment);
            } catch (const std::exception& error) {
                auto* target = RE::TESForm::LookupByID<RE::TESNPC>(baseID);
                if (target && FindOwnedRenderSource(target)) {
                    KillRuntime("render-source preparation threw after immutable publication");
                } else {
                    DisableBaseBeforePublication(baseID);
                }
                REX::CRITICAL("[OverlayRuntime] render-source preparation for base=0x{:08X} threw: {}", baseID, error.what());
            } catch (...) {
                auto* target = RE::TESForm::LookupByID<RE::TESNPC>(baseID);
                if (target && FindOwnedRenderSource(target)) {
                    KillRuntime("render-source preparation threw after immutable publication");
                } else {
                    DisableBaseBeforePublication(baseID);
                }
                REX::CRITICAL("[OverlayRuntime] render-source preparation for base=0x{:08X} threw an unknown exception", baseID);
            } },
                                                              "OverlayRuntime.PrepareRenderSource",
                                                              [this, baseID]
                                                              {
                                                                  RequeueBase(baseID);
                                                                  REX::WARN("[OverlayRuntime] render-source preparation for base=0x{:08X} was dropped by the native queue; retained for retry", baseID);
                                                              });

        if (result == Util::NativeMainThreadQueue::PostResult::kUnavailable)
        {
            RequeueBase(baseID);
            REX::DEBUG("[OverlayRuntime] native queue became unavailable for base=0x{:08X}; preparation remains pending", baseID);
        }
        else
        {
            REX::DEBUG("[OverlayRuntime] render-source preparation dispatch for base=0x{:08X}: {}", baseID, Util::NativeMainThreadQueue::ToString(result));
        }
    }

    void OverlayRuntime::PreparePendingBase(const RE::TESFormID baseID, const std::shared_ptr<const Config::PreparedAssignment> assignment)
    {
        if (!IsRuntimeOperational() || !assignment)
        {
            DisableBaseBeforePublication(baseID);
            return;
        }

        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (m_preparationOwnerBaseID != baseID || found == m_bases.end() || found->second.status != BaseStatus::kQueued || found->second.assignment != assignment)
            {
                KillRuntime("render-source preparation started without single-flight ownership");
                return;
            }
        }

        auto *target = RE::TESForm::LookupByID<RE::TESNPC>(baseID);
        if (!target)
        {
            DisableBaseBeforePublication(baseID);
            REX::WARN("[OverlayRuntime] runtime base=0x{:08X} for configured base=0x{:08X} no longer resolves; rendering vanilla and disabling that runtime base", baseID, assignment->baseFormID);
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        auto *source = FindOwnedRenderSource(target);
        FaceTextureComposite *composite = nullptr;
        if (!source)
        {
            source = PrepareRenderSource(target, assignment->preset, assignment->dependencies, baseID != assignment->baseFormID);
            if (!source)
            {
                DisableBaseBeforePublication(baseID);
                if (IsRuntimeOperational())
                {
                    REX::WARN("[OverlayRuntime] detached render source could not be prepared for base=0x{:08X}; rendering vanilla and disabling that base", baseID);
                }
                return;
            }

            const auto capturedStructure = CaptureRenderSourceStructure(source);
            if (!capturedStructure)
            {
                DestroyUnpublishedRenderSource(source);
                DisableBaseBeforePublication(baseID);
                REX::WARN("[OverlayRuntime] detached render source failed structural validation for base=0x{:08X}; rendering vanilla and disabling that base", baseID);
                return;
            }
            const auto preparedStructure = std::make_shared<const RenderSourceStructureState>(std::move(*capturedStructure));

            if (NeedsFaceTextureComposite(source))
            {
                composite = CreateFaceTextureComposite();
                if (!composite)
                {
                    DestroyUnpublishedRenderSource(source);
                    DisableBaseBeforePublication(baseID);
                    return;
                }
            }

            auto *faceTextureIdentity = RE::TESForm::LookupByID<RE::TESNPC>(assignment->baseFormID);
            if (!faceTextureIdentity)
            {
                DestroyUnstartedFaceTextureComposite(composite);
                DestroyUnpublishedRenderSource(source);
                DisableBaseBeforePublication(baseID);
                KillRuntime("the configured NPC could not provide a stable face-texture identity");
                return;
            }

            const auto published = PublishRenderSource(target, source, faceTextureIdentity);
            if (!published.source)
            {
                DestroyUnstartedFaceTextureComposite(composite);
                DestroyUnpublishedRenderSource(source);
                DisableBaseBeforePublication(baseID);
                KillRuntime("the immutable render-source registry could not publish a prepared carrier");
                return;
            }
            if (!published.adopted)
            {
                DestroyUnpublishedRenderSource(source);
                KillRuntime("single-flight publication unexpectedly found an existing render source");
                return;
            }
            source = published.source;

            bool invalidStructurePublication = false;
            {
                const std::scoped_lock lock{m_stateMutex};
                const auto found = m_bases.find(baseID);
                if (m_preparationOwnerBaseID != baseID || found == m_bases.end() || found->second.status != BaseStatus::kQueued ||
                    found->second.assignment != assignment || found->second.renderSourceStructure)
                {
                    invalidStructurePublication = true;
                }
                else
                {
                    found->second.renderSourceStructure = preparedStructure;
                }
            }
            if (invalidStructurePublication)
            {
                KillRuntime("render-source structure publication crossed an invalid base-state transition");
                return;
            }
        }
        else if (!ValidateRenderSourceForActivation(baseID, source))
        {
            KillRuntime("staged render-source structure was invalid before preparation resumed");
            return;
        }

        if (NeedsFaceTextureComposite(source))
        {
            if (!composite)
            {
                composite = CreateFaceTextureComposite();
                if (!composite)
                {
                    KillRuntime("a face-texture output could not be created after staged publication");
                    return;
                }
            }

            if (!StartFaceTextureComposite(composite, target, source))
            {
                DestroyUnstartedFaceTextureComposite(composite);
                KillRuntime("the engine rejected face-texture composite submission after staged publication");
                return;
            }

            bool invalidTransition = false;
            {
                const std::scoped_lock lock{m_stateMutex};
                const auto found = m_bases.find(baseID);
                if (m_preparationOwnerBaseID != baseID || found == m_bases.end() || found->second.status != BaseStatus::kQueued || found->second.assignment != assignment || found->second.faceTextureComposite)
                {
                    invalidTransition = true;
                }
                else
                {
                    found->second.faceTextureComposite = composite;
                    found->second.compositionStarted = std::chrono::steady_clock::now();
                    found->second.status = BaseStatus::kCompositePending;
                }
                UpdatePendingWorkFlagLocked();
            }
            if (invalidTransition)
            {
                KillRuntime("face-texture submission crossed an invalid base-state transition");
                return;
            }

            REX::DEBUG("[OverlayRuntime] detached render source staged and face-texture composition started for runtimeBase=0x{:08X} configuredBase=0x{:08X} pack='{}' in {} us",
                       baseID, assignment->baseFormID, assignment->packID, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
            return;
        }

        if (!ValidateRenderSourceForActivation(baseID, source))
        {
            KillRuntime("a textureless render source changed after structural publication");
            return;
        }
        if (!ActivateRenderSource(target, source))
        {
            KillRuntime("a textureless render source could not be activated after staged publication");
            return;
        }
        auto waitingRefs = CompletePublication(baseID, assignment, target, source);
        REX::DEBUG("[OverlayRuntime] detached render source activated without post-blend layers for runtimeBase=0x{:08X} configuredBase=0x{:08X} pack='{}' waitingRefs={} in {} us",
                   baseID, assignment->baseFormID, assignment->packID, waitingRefs.size(), std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());

        for (const auto refID : waitingRefs)
        {
            if (!IsRuntimeOperational())
            {
                break;
            }
            RefreshWaitingReference(target, source, refID, *assignment);
        }
        if (IsRuntimeOperational())
        {
            ReleasePreparationOwner(baseID);
        }
    }

    void OverlayRuntime::TryDispatchCompositePoll(const RE::TESFormID baseID)
    {
        std::shared_ptr<const Config::PreparedAssignment> assignment;
        FaceTextureComposite *composite = nullptr;
        bool invalidOwner = false;
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (found == m_bases.end() || found->second.status != BaseStatus::kCompositePending || !found->second.assignment || !found->second.faceTextureComposite)
            {
                return;
            }
            if (m_preparationOwnerBaseID != baseID)
            {
                invalidOwner = true;
            }
            else
            {
                found->second.status = BaseStatus::kCompositeQueued;
                assignment = found->second.assignment;
                composite = found->second.faceTextureComposite;
            }
            UpdatePendingWorkFlagLocked();
        }
        if (invalidOwner)
        {
            KillRuntime("face-texture polling was dispatched without single-flight ownership");
            return;
        }

        const auto result = Util::NativeMainThreadQueue::Post([this, baseID, assignment = std::move(assignment), composite]
                                                              { PollPendingComposite(baseID, assignment, composite); },
                                                              "OverlayRuntime.PollFaceTextureComposite",
                                                              [this, baseID]
                                                              {
                                                                  RequeueComposite(baseID);
                                                                  REX::WARN("[OverlayRuntime] face-texture readiness poll for base=0x{:08X} was dropped by the native queue; retained for retry", baseID);
                                                              });

        if (result == Util::NativeMainThreadQueue::PostResult::kUnavailable)
        {
            RequeueComposite(baseID);
        }
    }

    void OverlayRuntime::PollPendingComposite(
        const RE::TESFormID baseID,
        const std::shared_ptr<const Config::PreparedAssignment> assignment,
        FaceTextureComposite *composite)
    {
        auto *target = RE::TESForm::LookupByID<RE::TESNPC>(baseID);
        auto *source = target ? FindOwnedRenderSource(target) : nullptr;
        if (!target || !source || !assignment || !composite)
        {
            KillRuntime("face-texture readiness polling lost its published source or ownership state");
            return;
        }

        std::chrono::steady_clock::time_point started;
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (m_preparationOwnerBaseID != baseID || found == m_bases.end() || found->second.status != BaseStatus::kCompositeQueued || found->second.assignment != assignment || found->second.faceTextureComposite != composite)
            {
                KillRuntime("face-texture readiness polling crossed an invalid base-state transition");
                return;
            }
            started = found->second.compositionStarted;
        }

        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(30))
        {
            REX::CRITICAL("[OverlayRuntime] face-texture composition timed out for base=0x{:08X}; disabling appearance injection", baseID);
            KillRuntime("face-texture composition did not complete within 30 seconds");
            return;
        }
        if (!IsFaceTextureCompositeReady(composite))
        {
            RequeueComposite(baseID);
            return;
        }
        if (!FinalizeFaceTextureComposite(composite))
        {
            KillRuntime("face-texture composite finalization failed after staged publication");
            return;
        }

        bool invalidTransition = false;
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (m_preparationOwnerBaseID != baseID || found == m_bases.end() || found->second.status != BaseStatus::kCompositeQueued || found->second.assignment != assignment || found->second.faceTextureComposite != composite)
            {
                invalidTransition = true;
            }
            else
            {
                found->second.status = BaseStatus::kCompositeFinalized;
            }
            UpdatePendingWorkFlagLocked();
        }
        if (invalidTransition)
        {
            KillRuntime("face-texture finalization crossed an invalid base-state transition");
            return;
        }

        REX::DEBUG("[OverlayRuntime] face-texture composition finalized for runtimeBase=0x{:08X} configuredBase=0x{:08X} pack='{}'; waiting for the engine publication barrier",
                   baseID, assignment->baseFormID, assignment->packID);
    }

    void OverlayRuntime::RequeueBase(const RE::TESFormID baseID)
    {
        bool invalidOwner = false;
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (found != m_bases.end() && found->second.status == BaseStatus::kQueued)
            {
                if (m_preparationOwnerBaseID != baseID)
                {
                    invalidOwner = true;
                }
                else
                {
                    found->second.status = BaseStatus::kPending;
                }
            }
            UpdatePendingWorkFlagLocked();
        }
        if (invalidOwner)
        {
            KillRuntime("render-source preparation was requeued without single-flight ownership");
        }
    }

    void OverlayRuntime::RequeueComposite(const RE::TESFormID baseID)
    {
        bool invalidOwner = false;
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (found != m_bases.end() && found->second.status == BaseStatus::kCompositeQueued)
            {
                if (m_preparationOwnerBaseID != baseID)
                {
                    invalidOwner = true;
                }
                else
                {
                    found->second.status = BaseStatus::kCompositePending;
                }
            }
            UpdatePendingWorkFlagLocked();
        }
        if (invalidOwner)
        {
            KillRuntime("face-texture polling was requeued without single-flight ownership");
        }
    }

    void OverlayRuntime::TryDispatchCompositeActivation(const RE::TESFormID baseID)
    {
        std::shared_ptr<const Config::PreparedAssignment> assignment;
        FaceTextureComposite *composite = nullptr;
        bool invalidOwner = false;
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (found == m_bases.end() || found->second.status != BaseStatus::kCompositeFinalized || !found->second.assignment || !found->second.faceTextureComposite)
            {
                return;
            }
            if (m_preparationOwnerBaseID != baseID)
            {
                invalidOwner = true;
            }
            else
            {
                found->second.status = BaseStatus::kCompositeActivationQueued;
                assignment = found->second.assignment;
                composite = found->second.faceTextureComposite;
            }
            UpdatePendingWorkFlagLocked();
        }
        if (invalidOwner)
        {
            KillRuntime("face-texture activation was dispatched without single-flight ownership");
            return;
        }

        const auto result = Util::NativeMainThreadQueue::Post([this, baseID, assignment = std::move(assignment), composite]
                                                              { PollCompositeActivation(baseID, assignment, composite); },
                                                              "OverlayRuntime.PollFaceTexturePublication",
                                                              [this, baseID]
                                                              {
                                                                  RequeueCompositeActivation(baseID);
                                                                  REX::WARN("[OverlayRuntime] face-texture publication poll for base=0x{:08X} was dropped by the native queue; retained for retry", baseID);
                                                              });

        if (result == Util::NativeMainThreadQueue::PostResult::kUnavailable)
        {
            RequeueCompositeActivation(baseID);
        }
    }

    void OverlayRuntime::PollCompositeActivation(
        const RE::TESFormID baseID,
        const std::shared_ptr<const Config::PreparedAssignment> assignment,
        FaceTextureComposite *composite)
    {
        auto *target = RE::TESForm::LookupByID<RE::TESNPC>(baseID);
        auto *source = target ? FindOwnedRenderSource(target) : nullptr;
        if (!target || !source || !assignment || !composite)
        {
            KillRuntime("face-texture publication polling lost its staged source or ownership state");
            return;
        }

        bool armBarrier = false;
        std::chrono::steady_clock::time_point started;
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (m_preparationOwnerBaseID != baseID || found == m_bases.end() || found->second.status != BaseStatus::kCompositeActivationQueued || found->second.assignment != assignment || found->second.faceTextureComposite != composite)
            {
                KillRuntime("face-texture publication polling crossed an invalid base-state transition");
                return;
            }
            started = found->second.compositionStarted;
            if (!found->second.publicationBarrierArmed)
            {
                found->second.publicationBarrierArmed = true;
                found->second.status = BaseStatus::kCompositeFinalized;
                armBarrier = true;
            }
            UpdatePendingWorkFlagLocked();
        }

        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(30))
        {
            REX::CRITICAL("[OverlayRuntime] face-texture publication timed out for base=0x{:08X}; disabling appearance injection", baseID);
            KillRuntime("face-texture publication barrier did not clear within 30 seconds");
            return;
        }
        if (armBarrier)
        {
            REX::DEBUG("[OverlayRuntime] face-texture publication barrier armed for base=0x{:08X}; deferring activation to the next native queue drain", baseID);
            return;
        }
        if (!IsFaceTexturePublicationIdle())
        {
            RequeueCompositeActivation(baseID);
            return;
        }
        if (!ValidateRenderSourceForActivation(baseID, source))
        {
            KillRuntime("a render source changed during face-texture composition");
            return;
        }
        if (!ActivateRenderSource(target, source))
        {
            KillRuntime("a render source could not be activated after the face-texture publication barrier");
            return;
        }

        auto waitingRefs = CompletePublication(baseID, assignment, target, source);
        REX::DEBUG("[OverlayRuntime] face-texture publication barrier cleared and render source activated for runtimeBase=0x{:08X} configuredBase=0x{:08X} pack='{}' waitingRefs={} in {} ms",
                   baseID, assignment->baseFormID, assignment->packID, waitingRefs.size(), std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());

        for (const auto refID : waitingRefs)
        {
            if (!IsRuntimeOperational())
            {
                break;
            }
            RefreshWaitingReference(target, source, refID, *assignment);
        }
        if (IsRuntimeOperational())
        {
            ReleasePreparationOwner(baseID);
        }
    }

    void OverlayRuntime::RequeueCompositeActivation(const RE::TESFormID baseID)
    {
        bool invalidOwner = false;
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (found != m_bases.end() && found->second.status == BaseStatus::kCompositeActivationQueued)
            {
                if (m_preparationOwnerBaseID != baseID)
                {
                    invalidOwner = true;
                }
                else
                {
                    found->second.status = BaseStatus::kCompositeFinalized;
                }
            }
            UpdatePendingWorkFlagLocked();
        }
        if (invalidOwner)
        {
            KillRuntime("face-texture activation was requeued without single-flight ownership");
        }
    }

    void OverlayRuntime::TryDispatchVanillaRefresh(const RE::TESFormID baseID)
    {
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (found == m_bases.end() || found->second.status != BaseStatus::kVanillaRefreshPending)
            {
                return;
            }
            if (m_preparationOwnerBaseID != 0)
            {
                UpdatePendingWorkFlagLocked();
                return;
            }
            found->second.status = BaseStatus::kVanillaRefreshQueued;
            UpdatePendingWorkFlagLocked();
        }

        const auto result = Util::NativeMainThreadQueue::Post([this, baseID]
                                                              { RefreshVanillaAfterIdentityMismatch(baseID); },
                                                              "OverlayRuntime.RefreshVanillaAfterIdentityMismatch",
                                                              [this, baseID]
                                                              {
                                                                  RequeueVanillaRefresh(baseID);
                                                                  REX::WARN("[OverlayRuntime] vanilla refresh for reused runtime base=0x{:08X} was dropped by the native queue; retained for retry", baseID);
                                                              });

        if (result == Util::NativeMainThreadQueue::PostResult::kUnavailable)
        {
            RequeueVanillaRefresh(baseID);
            REX::DEBUG("[OverlayRuntime] native queue unavailable for reused runtime base=0x{:08X}; vanilla refresh remains pending", baseID);
        }
    }

    void OverlayRuntime::RefreshVanillaAfterIdentityMismatch(const RE::TESFormID baseID)
    {
        std::vector<RE::TESFormID> waitingRefs;
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (found == m_bases.end() || found->second.status != BaseStatus::kVanillaRefreshQueued)
            {
                KillRuntime("a vanilla fallback refresh crossed an invalid base-state transition");
                return;
            }
            waitingRefs.assign(found->second.waitingRefs.begin(), found->second.waitingRefs.end());
            found->second.waitingRefs.clear();
            found->second.status = BaseStatus::kDisabled;
            UpdatePendingWorkFlagLocked();
        }

        auto *target = RE::TESForm::LookupByID<RE::TESNPC>(baseID);
        for (const auto refID : waitingRefs)
        {
            auto *actor = RE::TESForm::LookupByID<RE::Actor>(refID);
            if (!target || !actor || actor->GetNPC() != target || !HasLoaded3D(actor))
            {
                REX::DEBUG("[OverlayRuntime] vanilla fallback skipped for reused runtime base=0x{:08X} ref=0x{:08X}; reference is no longer loaded or valid", baseID, refID);
                continue;
            }

            const auto canonicalState = CaptureOriginalNPCState(target);
            const auto canonicalStorage = CaptureVisualStorageState(target);
            try
            {
                actor->RefreshAppearance(false, 0x28, false);
            }
            catch (const std::exception &error)
            {
                REX::ERROR("[OverlayRuntime] vanilla fallback refresh for reused runtime base=0x{:08X} ref=0x{:08X} threw: {}", baseID, refID, error.what());
            }
            catch (...)
            {
                REX::ERROR("[OverlayRuntime] vanilla fallback refresh for reused runtime base=0x{:08X} ref=0x{:08X} threw an unknown exception", baseID, refID);
            }

            if (CaptureOriginalNPCState(target) != canonicalState || CaptureVisualStorageState(target) != canonicalStorage)
            {
                KillRuntime("the canonical TESNPC changed during a vanilla fallback refresh");
                return;
            }
            REX::DEBUG("[OverlayRuntime] vanilla fallback refresh completed for reused runtime base=0x{:08X} ref=0x{:08X}", baseID, refID);
        }
    }

    void OverlayRuntime::RequeueVanillaRefresh(const RE::TESFormID baseID)
    {
        const std::scoped_lock lock{m_stateMutex};
        const auto found = m_bases.find(baseID);
        if (found != m_bases.end() && found->second.status == BaseStatus::kVanillaRefreshQueued)
        {
            found->second.status = BaseStatus::kVanillaRefreshPending;
        }
        UpdatePendingWorkFlagLocked();
    }

    void OverlayRuntime::DisableBaseBeforePublication(const RE::TESFormID baseID)
    {
        bool invalidTransition = false;
        bool releasedOwner = false;
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (m_preparationOwnerBaseID != baseID || found == m_bases.end() || found->second.status == BaseStatus::kReady)
            {
                invalidTransition = true;
            }
            else
            {
                found->second.status = BaseStatus::kDisabled;
                found->second.waitingRefs.clear();
                found->second.assignment.reset();
                m_preparationOwnerBaseID = 0;
                releasedOwner = true;
            }
            UpdatePendingWorkFlagLocked();
        }
        if (invalidTransition)
        {
            KillRuntime("pre-publication disablement crossed an invalid base-state transition");
        }
        else if (releasedOwner)
        {
            REX::DEBUG("[OverlayRuntime] single-flight preparation ownership released after disabling base=0x{:08X}", baseID);
        }
    }

    bool OverlayRuntime::ValidateRenderSourceForActivation(const RE::TESFormID baseID, RE::TESNPC *source)
    {
        std::shared_ptr<const RenderSourceStructureState> expected;
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (found != m_bases.end())
            {
                expected = found->second.renderSourceStructure;
            }
        }

        if (!source || !expected || expected->source != source || source->GetFormID() != 0 || source->QRefCount() == 0)
        {
            REX::CRITICAL("[OverlayRuntime] detached render-source activation invariant failed: base=0x{:08X} source={} snapshot={} formID=0x{:08X} references={}",
                          baseID,
                          static_cast<const void*>(source),
                          static_cast<bool>(expected),
                          source ? source->GetFormID() : 0,
                          source ? source->QRefCount() : 0);
            return false;
        }
        if (!MatchesRenderSourceStructure(source, *expected))
        {
            REX::CRITICAL("[OverlayRuntime] detached render-source structure changed before activation: base=0x{:08X} source={}",
                          baseID, static_cast<const void*>(source));
            return false;
        }
        return true;
    }

    std::vector<RE::TESFormID> OverlayRuntime::CompletePublication(
        const RE::TESFormID baseID,
        const std::shared_ptr<const Config::PreparedAssignment> &assignment,
        RE::TESNPC *canonical,
        RE::TESNPC *source)
    {
        std::vector<RE::TESFormID> waitingRefs;
        bool invalidTransition = false;
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            const auto expectedStatus = found != m_bases.end() && found->second.faceTextureComposite ? BaseStatus::kCompositeActivationQueued : BaseStatus::kQueued;
            if (m_preparationOwnerBaseID != baseID || found == m_bases.end() || found->second.status != expectedStatus || found->second.assignment != assignment ||
                found->second.configuredBaseID != assignment->baseFormID ||
                !canonical || !source || FindOwnedRenderSource(canonical) != source)
            {
                invalidTransition = true;
            }
            else
            {
                auto &state = found->second;
                waitingRefs.assign(state.waitingRefs.begin(), state.waitingRefs.end());
                state.waitingRefs.clear();
                state.assignment.reset();
                state.status = BaseStatus::kReady;
            }
            UpdatePendingWorkFlagLocked();
        }
        if (invalidTransition)
        {
            KillRuntime("immutable publication crossed an invalid base-state transition");
        }
        return waitingRefs;
    }

    void OverlayRuntime::ReleasePreparationOwner(const RE::TESFormID baseID)
    {
        bool invalidTransition = false;
        {
            const std::scoped_lock lock{m_stateMutex};
            const auto found = m_bases.find(baseID);
            if (m_preparationOwnerBaseID != baseID || found == m_bases.end() || found->second.status != BaseStatus::kReady)
            {
                invalidTransition = true;
            }
            else
            {
                m_preparationOwnerBaseID = 0;
            }
            UpdatePendingWorkFlagLocked();
        }
        if (invalidTransition)
        {
            KillRuntime("single-flight preparation ownership was released from an invalid base state");
            return;
        }

        REX::DEBUG("[OverlayRuntime] single-flight preparation ownership released after activating base=0x{:08X}", baseID);
    }

    void OverlayRuntime::UpdatePendingWorkFlagLocked()
    {
        bool hasPending = false;
        if (m_preparationOwnerBaseID != 0)
        {
            const auto found = m_bases.find(m_preparationOwnerBaseID);
            if (found != m_bases.end())
            {
                hasPending = found->second.status == BaseStatus::kPending || found->second.status == BaseStatus::kCompositePending ||
                             found->second.status == BaseStatus::kCompositeFinalized;
            }
        }
        else
        {
            hasPending = std::ranges::any_of(m_bases, [](const auto &entry)
                                             { return entry.second.status == BaseStatus::kPending || entry.second.status == BaseStatus::kVanillaRefreshPending; });
        }
        m_hasDispatchableBases.store(hasPending, std::memory_order_release);
    }

    bool OverlayRuntime::RefreshWaitingReference(RE::TESNPC *target, RE::TESNPC *source, const RE::TESFormID actorRefID, const Config::PreparedAssignment &assignment)
    {
        auto *actor = RE::TESForm::LookupByID<RE::Actor>(actorRefID);
        if (!actor || actor->GetNPC() != target || !HasLoaded3D(actor))
        {
            REX::DEBUG("[OverlayRuntime] waiting ref=0x{:08X} for base=0x{:08X} is no longer loaded or valid; catch-up refresh skipped", actorRefID, target->GetFormID());
            return false;
        }

        const auto canonicalState = CaptureOriginalNPCState(target);
        const auto canonicalStorage = CaptureVisualStorageState(target);
        bool refreshed = false;
        try
        {
            refreshed = RefreshAppearanceFromRenderSource(target, source, actor, actorRefID);
        }
        catch (const std::exception &error)
        {
            REX::ERROR("[OverlayRuntime] detached catch-up refresh for base=0x{:08X} ref=0x{:08X} threw: {}", target->GetFormID(), actorRefID, error.what());
        }
        catch (...)
        {
            REX::ERROR("[OverlayRuntime] detached catch-up refresh for base=0x{:08X} ref=0x{:08X} threw an unknown exception", target->GetFormID(), actorRefID);
        }

        const bool canonicalPreserved = CaptureOriginalNPCState(target) == canonicalState && CaptureVisualStorageState(target) == canonicalStorage;
        if (!canonicalPreserved)
        {
            KillRuntime("the canonical TESNPC changed during a detached appearance refresh");
            return false;
        }
        if (!refreshed)
        {
            REX::WARN("[OverlayRuntime] catch-up refresh failed for published base=0x{:08X} ref=0x{:08X}; future 3D builds can still use the published source", target->GetFormID(), actorRefID);
            return false;
        }

        REX::DEBUG("[OverlayRuntime] detached catch-up refresh completed for base=0x{:08X} ref=0x{:08X} pack='{}'", target->GetFormID(), actorRefID, assignment.packID);
        return true;
    }

    bool OverlayRuntime::HasLoaded3D(RE::Actor *actor)
    {
        if (!actor)
        {
            return false;
        }

        const auto loaded = actor->loadedData.LockRead();
        return *loaded && (*loaded)->data3D;
    }
}
