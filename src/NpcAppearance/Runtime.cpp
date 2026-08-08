#include "NpcAppearance/Runtime.h"

#include "NpcAppearance/Config.h"
#include "NpcAppearance/RuntimeDetail.h"
#include "pch.h"

#include "Util/NativeMainThreadQueue.h"
#include "Util/StarfieldRuntime.h"

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NpcAppearance
{
    namespace
    {
        using namespace Detail;

        // ==================================================================
        // Runtime arming state. Assignment maps are published by
        // validation-only scans and consumed only from the verified native
        // BSService queue drain. The Detail gates over this state are
        // defined at the bottom of this file.
        // ==================================================================
        std::atomic<bool>             g_runtimeOperational{ false };
        std::atomic<bool>             g_runtimeArmed{ false };
        std::atomic<bool>             g_mutationKilled{ false };
        std::atomic<bool>             g_saveLoadSinkRegistered{ false };
        std::mutex                    g_eventMutex;
        std::unordered_map<RE::TESFormID, SelectedAssignment> g_sceneAssignments;

        [[nodiscard]] std::filesystem::path GetThisDllDirectory()
        {
            HMODULE module = nullptr;
            const auto address = reinterpret_cast<LPCWSTR>(&GetThisDllDirectory);
            if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      address, &module) || !module) {
                return {};
            }
            std::wstring buffer(32768, L'\0');
            const auto length = ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0 || length >= buffer.size()) {
                return {};
            }
            buffer.resize(length);
            return std::filesystem::path{ buffer }.parent_path();
        }

        [[nodiscard]] std::filesystem::path DefaultPluginDirectory()
        {
            return GetThisDllDirectory() / L"OSFIdentity";
        }

        [[nodiscard]] std::filesystem::path DefaultPacksDirectory()
        {
            return DefaultPluginDirectory() / L"Packs";
        }

        // ==================================================================
        // Actor notification + refresh
        // Byte-gated calls into the engine that make a styled base visible
        // on the rendered actor.
        // ==================================================================
        using NotifyNpcAppearanceChanged = void (*)(RE::TESNPC*, std::uint32_t);
        using RefreshActorAppearance = void (*)(RE::Actor*, bool, std::uint32_t, bool);

        [[nodiscard]] bool ResolveNpcAppearanceChanged(
            RE::TESNPC* a_npc,
            NotifyNpcAppearanceChanged& a_notify) noexcept
        {
            a_notify = nullptr;
            std::uintptr_t vtable = 0;
            std::uintptr_t notifyAddress = 0;
            const auto expectedVtable =
                REL::Relocation<std::uintptr_t>{ kNpcPrimaryVtableID }.address();
            if (!a_npc ||
                !Util::SafeReadQword(
                    reinterpret_cast<std::uintptr_t>(a_npc), vtable) ||
                vtable != expectedVtable ||
                !Util::SafeReadQword(
                    vtable + 0x17 * sizeof(std::uintptr_t), notifyAddress) ||
                !HasExpectedBytes(notifyAddress, kNpcAppearanceChangedGate)) {
                return false;
            }
            a_notify = reinterpret_cast<NotifyNpcAppearanceChanged>(notifyAddress);
            return true;
        }

        [[nodiscard]] bool NotifyBaseAppearanceChanged(
            RE::TESNPC* a_npc,
            const std::uint32_t a_flag)
        {
            NotifyNpcAppearanceChanged notify = nullptr;
            if (!ResolveNpcAppearanceChanged(a_npc, notify)) {
                KillMutation("TESNPC appearance notification vtable/slot byte gate failed");
                return false;
            }
            notify(a_npc, a_flag);
            return true;
        }

        struct TargetActorResolution
        {
            RE::Actor*       actor{ nullptr };
            RE::TESFormID    actorRefID{ 0 };
            std::size_t      matches{ 0 };
            std::size_t      highActors{ 0 };
            bool             processListsValid{ false };
        };

        [[nodiscard]] TargetActorResolution ResolveTargetActor(RE::TESNPC* a_target)
        {
            TargetActorResolution result;
            auto* processLists = RE::ProcessLists::GetSingleton();
            if (!a_target || !processLists) {
                return result;
            }

            std::uintptr_t processListsVtable = 0;
            if (!Util::SafeReadQword(
                    reinterpret_cast<std::uintptr_t>(processLists),
                    processListsVtable) ||
                Util::ToRva(processListsVtable) != kProcessListsVtableRva) {
                return result;
            }
            result.processListsValid = true;
            result.highActors = processLists->highActorHandles.size();
            if (result.highActors > 0x4000) {
                result.processListsValid = false;
                return result;
            }

            for (auto& handle : processLists->highActorHandles) {
                if (!static_cast<bool>(handle)) {
                    continue;
                }
                const RE::NiPointer<RE::Actor> actorPointer = handle.get();
                auto* actor = actorPointer.get();
                std::uintptr_t actorVtable = 0;
                if (!actor ||
                    !Util::SafeReadQword(
                        reinterpret_cast<std::uintptr_t>(actor), actorVtable) ||
                    Util::ToRva(actorVtable) != kActorVtableRva ||
                    actor->GetNPC() != a_target) {
                    continue;
                }
                ++result.matches;
                if (!result.actor) {
                    result.actor = actor;
                    result.actorRefID = actor->GetFormID();
                }
            }
            return result;
        }

        [[nodiscard]] bool NotifyAndKick(
            RE::TESNPC* a_target,
            RE::Actor* a_actor,
            const RE::TESFormID a_actorRefID)
        {
            if (!MutationOperational() || !a_target || !a_actor ||
                a_actor->GetNPC() != a_target || a_actor->GetFormID() != a_actorRefID) {
                return false;
            }

            const auto refreshAddress =
                REL::Relocation<std::uintptr_t>{ kActorAppearanceRefreshID }.address();
            if (!HasExpectedBytes(refreshAddress, kActorAppearanceRefreshGate)) {
                KillMutation("actor appearance refresh byte gate failed");
                return false;
            }

            if (!NotifyBaseAppearanceChanged(a_target, 0x800) ||
                !NotifyBaseAppearanceChanged(a_target, 0x4000)) {
                return false;
            }
            reinterpret_cast<RefreshActorAppearance>(refreshAddress)(
                a_actor, false, 0x28, false);
            return true;
        }

        [[nodiscard]] bool HasLoaded3D(RE::Actor* a_actor)
        {
            std::uintptr_t loadedData = 0;
            std::uintptr_t root3D = 0;
            return a_actor &&
                   Util::SafeReadQword(reinterpret_cast<std::uintptr_t>(a_actor) + 0xB8,
                                       loadedData) &&
                   loadedData != 0 && Util::SafeReadQword(loadedData + 0x8, root3D) &&
                   root3D != 0;
        }

        // ==================================================================
        // Original-state capture for one transient overlay window. Nothing
        // is tracked between windows: the snapshot lives on the stack for
        // the duration of one drain task.
        // ==================================================================
        struct AppliedBaseState
        {
            RE::TESFormID       baseID{ 0 };
            SelectedAssignment assignment;
            OwnedVisualSnapshot originalVisual;
            NonVisualSnapshot   originalNonVisual;
            RE::TESNPC*         originalFaceNPC{ nullptr };
            std::uint32_t       originalActorFlags{ 0 };
        };

        std::atomic<std::uint64_t>                      g_loadReturnCount{ 0 };
        std::atomic<std::uint64_t>                      g_loadGeneration{ 0 };
        constexpr std::uint32_t                         kLoadSweepReadyMaxNativeFrames = 600;
        struct DeferredLoadSweepTask
        {
            std::uint64_t                    generation{ 0 };
            std::function<bool(std::uint32_t)> run;
            std::uint32_t                   attempts{ 0 };
            bool                            deferralLogged{ false };
        };
        std::mutex                                      g_deferredLoadSweepMutex;
        std::shared_ptr<DeferredLoadSweepTask>             g_deferredLoadSweepTask;
        std::shared_ptr<DeferredLoadSweepTask>             g_deferredLoadSweepInFlight;
        std::atomic<bool>                               g_deferredLoadSweepRetryScheduled{ false };
        constexpr std::uint32_t                         kLoadSweepRetryMaxWaits = 400;
        constexpr std::chrono::milliseconds             kLoadSweepRetryDelay{ 25 };

        [[nodiscard]] bool ExactOriginalState(
            RE::TESNPC* a_target,
            const AppliedBaseState& a_state)
        {
            return a_target &&
                SameExactVisualValues(a_target, a_state.originalVisual) &&
                a_target->faceNPC == a_state.originalFaceNPC &&
                a_target->actorData.actorBaseFlags.underlying() ==
                    a_state.originalActorFlags &&
                a_state.originalNonVisual == Snapshot(a_target);
        }

        [[nodiscard]] bool RestoreAppliedBaseState(
            const LineSink& a_out,
            RE::TESNPC* a_target,
            const AppliedBaseState& a_state)
        {
            const bool visualRestored = RestoreOwnedVisualSnapshot(
                a_out, a_target, a_state.originalVisual, a_state.originalFaceNPC);
            if (a_target) {
                a_target->actorData.actorBaseFlags =
                    static_cast<RE::ACTOR_BASE_DATA::Flag>(
                        a_state.originalActorFlags);
            }
            return visualRestored && ExactOriginalState(a_target, a_state);
        }

        [[nodiscard]] bool QueueOrRunNativeTask(
            std::function<void()> a_task,
            const std::string_view a_label,
            std::function<void()> a_onDrop = {})
        {
            const auto before = Util::NativeMainThreadQueue::GetDiagnostics();
            if (before.insideDrain) {
                try {
                    a_task();
                    return true;
                } catch (const std::exception& e) {
                    REX::CRITICAL(
                        "[NpcAppearance] native task '{}' threw '{}' inside the verified drain",
                        a_label, e.what());
                } catch (...) {
                    REX::CRITICAL(
                        "[NpcAppearance] native task '{}' threw inside the verified drain",
                        a_label);
                }
                return false;
            }

            const auto postResult = Util::NativeMainThreadQueue::Post(
                std::move(a_task), a_label, std::move(a_onDrop));
            if (postResult == Util::NativeMainThreadQueue::PostResult::kQueued) {
                return true;
            }
            if (postResult ==
                Util::NativeMainThreadQueue::PostResult::kQueueDisabled) {
                // The engine disables the queue around LoadGame; a refusal
                // here is an expected state every caller already handles
                // (deferral or retry at the next trigger).
                REX::WARN(
                    "[NpcAppearance] native task '{}' refused result=queue-disabled tid={}; caller falls back",
                    a_label, before.currentThreadID);
            } else {
                REX::CRITICAL(
                    "[NpcAppearance] native task '{}' post failed result={} tid={} drainOwnerTid={} queueEnabled={}",
                    a_label, Util::NativeMainThreadQueue::ToString(postResult),
                    before.currentThreadID, before.drainOwnerThreadID,
                    before.queueEnabled);
            }
            return false;
        }

        // ==================================================================
        // Overlay runtime (Mechanism B, probe-proven 2026-08-07; see
        // docs/OVERLAY_PROBE_FINDINGS.md). Per 3D build of a tracked actor:
        // apply preset to base -> notify -> refresh -> restore byte-exactly,
        // all within one verified drain task, so the serializable TESNPC is
        // never preset-mutated at rest. Triggered by ReferenceSet3d (fires
        // outside the drain; handler posts) plus a post-load sweep. A failed
        // in-window restore is the one hard failure: it kills mutation for
        // the process and the operator should reload rather than save over
        // that state.
        // ==================================================================
        constexpr std::chrono::milliseconds kOverlayReapplyCooldown{ 1000 };

        struct OverlayRuntimeState
        {
            std::unordered_set<RE::TESFormID> disabledBases;
            std::unordered_set<RE::TESFormID> inFlightRefs;
            std::unordered_map<RE::TESFormID,
                               std::chrono::steady_clock::time_point>
                          lastAppliedByRef;
        };
        std::mutex          g_overlayRuntimeMutex;
        OverlayRuntimeState g_overlayRuntime;
        std::atomic<bool>   g_overlaySinkRegistered{ false };

        // One transient window. Returns true when the actor rendered the
        // preset AND the base was proven byte-exact original before this
        // drain task returns. Must run inside the verified drain.
        [[nodiscard]] bool ApplyTransientOverlay(
            RE::TESNPC* a_target,
            RE::Actor* a_actor,
            const RE::TESFormID a_actorRefID,
            const SelectedAssignment& a_assignment)
        {
            const auto baseID = a_target->GetFormID();
            const LineSink out = [baseID](const std::string& a_text) {
                REX::INFO(
                    "[NpcAppearance] overlay base=0x{:08X}: {}",
                    baseID, a_text);
            };
            AppliedBaseState insurance{
                .baseID = baseID,
                .assignment = a_assignment,
                .originalVisual = CaptureOwnedVisualSnapshot(a_target),
                .originalNonVisual = Snapshot(a_target),
                .originalFaceNPC = a_target->faceNPC,
                .originalActorFlags =
                    a_target->actorData.actorBaseFlags.underlying(),
            };
            const auto started = std::chrono::steady_clock::now();
            const bool applied = SilentApplyPresetToBase(
                out, a_target, a_assignment.presetPath);
            bool notifiedKicked = false;
            if (applied) {
                notifiedKicked = NotifyAndKick(a_target, a_actor, a_actorRefID);
            }
            const bool restoredExact =
                RestoreAppliedBaseState(out, a_target, insurance);
            const auto elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();

            if (!restoredExact) {
                KillMutation(
                    "overlay window restore failed; base left non-original");
                REX::CRITICAL(
                    "[NpcAppearance] overlay base=0x{:08X} actor=0x{:08X} restore FAILED after applied={} notifiedKicked={}; mutation killed — reload rather than save over this state",
                    baseID, a_actorRefID, applied, notifiedKicked);
                return false;
            }
            if (!applied || !notifiedKicked) {
                {
                    const std::scoped_lock lock{ g_overlayRuntimeMutex };
                    g_overlayRuntime.disabledBases.insert(baseID);
                }
                REX::WARN(
                    "[NpcAppearance] overlay base=0x{:08X} actor=0x{:08X} applied={} notifiedKicked={}; rendering vanilla and disabling this base for the session",
                    baseID, a_actorRefID, applied, notifiedKicked);
                return false;
            }
            {
                const std::scoped_lock lock{ g_overlayRuntimeMutex };
                g_overlayRuntime.lastAppliedByRef[a_actorRefID] =
                    std::chrono::steady_clock::now();
            }
            REX::INFO(
                "[NpcAppearance] overlay base=0x{:08X} actor=0x{:08X} window CLOSED applied=true notifiedKicked=true restoredExact=true ms={:.3f}",
                baseID, a_actorRefID, elapsedMs);
            return true;
        }

        // Posted (or inline-in-drain) worker for one Set3d-triggered apply.
        void RunOverlayApplyTask(
            const RE::TESFormID a_refID,
            const RE::TESFormID a_baseID,
            const SelectedAssignment& a_assignment)
        {
            struct InFlightGuard
            {
                RE::TESFormID refID;
                ~InFlightGuard()
                {
                    const std::scoped_lock lock{ g_overlayRuntimeMutex };
                    g_overlayRuntime.inFlightRefs.erase(refID);
                }
            } guard{ a_refID };

            if (!MutationOperational()) {
                return;
            }
            {
                const std::scoped_lock lock{ g_overlayRuntimeMutex };
                if (g_overlayRuntime.disabledBases.contains(a_baseID)) {
                    return;
                }
            }
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_refID);
            auto* target = RE::TESForm::LookupByID<RE::TESNPC>(a_baseID);
            if (!actor || !target || actor->GetNPC() != target) {
                REX::WARN(
                    "[NpcAppearance] overlay apply skipped ref=0x{:08X} base=0x{:08X}; actor or base identity changed since post",
                    a_refID, a_baseID);
                return;
            }
            if (!HasLoaded3D(actor)) {
                return;
            }
            static_cast<void>(
                ApplyTransientOverlay(target, actor, a_refID, a_assignment));
        }

        // Set3d handler. Runs on arbitrary engine threads: reads only, then
        // posts. The inFlight guard also breaks the feedback loop from the
        // window's own refresh (refresh -> detach -> Set3d for the same ref);
        // the cooldown absorbs late deliveries after the task retires.
        void OnOverlaySet3d(RE::TESObjectREFR* a_ref) noexcept
        {
            try {
                if (!MutationOperational()) {
                    return;
                }
                auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
                auto* base = actor ? actor->GetNPC() : nullptr;
                if (!base) {
                    return;
                }
                const auto baseID = base->GetFormID();
                SelectedAssignment assignment;
                {
                    const std::scoped_lock lock{ g_eventMutex };
                    const auto found = g_sceneAssignments.find(baseID);
                    if (found == g_sceneAssignments.end()) {
                        return;
                    }
                    assignment = found->second;
                }
                const auto refID = actor->GetFormID();
                {
                    const std::scoped_lock lock{ g_overlayRuntimeMutex };
                    if (g_overlayRuntime.disabledBases.contains(baseID)) {
                        return;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    const auto last =
                        g_overlayRuntime.lastAppliedByRef.find(refID);
                    if (last != g_overlayRuntime.lastAppliedByRef.end() &&
                        now - last->second < kOverlayReapplyCooldown) {
                        return;
                    }
                    if (!g_overlayRuntime.inFlightRefs.insert(refID).second) {
                        return;
                    }
                }
                const bool queued = QueueOrRunNativeTask(
                    [refID, baseID, assignment = std::move(assignment)]() {
                        RunOverlayApplyTask(refID, baseID, assignment);
                    },
                    "NpcAppearance.OverlayApply",
                    [refID]() {
                        {
                            const std::scoped_lock lock{ g_overlayRuntimeMutex };
                            g_overlayRuntime.inFlightRefs.erase(refID);
                        }
                        REX::WARN(
                            "[NpcAppearance] overlay apply for ref=0x{:08X} dropped by the native queue; will retry at the next 3D build",
                            refID);
                    });
                if (!queued) {
                    const std::scoped_lock lock{ g_overlayRuntimeMutex };
                    g_overlayRuntime.inFlightRefs.erase(refID);
                }
            } catch (...) {
            }
        }

        class OverlaySet3dSink final :
            public RE::BSTEventSink<
                RE::RuntimeComponentDBFactory::ReferenceSet3d>
        {
        public:
            static OverlaySet3dSink& GetSingleton() noexcept
            {
                static OverlaySet3dSink singleton;
                return singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::RuntimeComponentDBFactory::ReferenceSet3d& a_event,
                RE::BSTEventSource<
                    RE::RuntimeComponentDBFactory::ReferenceSet3d>*) noexcept
                override
            {
                OnOverlaySet3d(a_event.ref.get());
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // Applies the overlay to every tracked actor that already has loaded
        // 3D. Backstop for actors whose Set3d fired while the native queue
        // was disabled (around LoadGame) or before arming. Must run inside
        // the verified drain.
        void RunOverlaySweep(const std::string_view a_reason)
        {
            if (!MutationOperational()) {
                return;
            }
            std::vector<std::pair<RE::TESFormID, SelectedAssignment>> winners;
            {
                const std::scoped_lock lock{ g_eventMutex };
                winners.reserve(g_sceneAssignments.size());
                for (const auto& entry : g_sceneAssignments) {
                    winners.push_back(entry);
                }
            }
            std::size_t appliedCount = 0;
            std::size_t skippedCount = 0;
            for (const auto& [baseID, assignment] : winners) {
                {
                    const std::scoped_lock lock{ g_overlayRuntimeMutex };
                    if (g_overlayRuntime.disabledBases.contains(baseID)) {
                        ++skippedCount;
                        continue;
                    }
                }
                auto* target = RE::TESForm::LookupByID<RE::TESNPC>(baseID);
                const auto resolution = ResolveTargetActor(target);
                if (!resolution.actor || !HasLoaded3D(resolution.actor)) {
                    ++skippedCount;
                    continue;
                }
                const auto refID = resolution.actorRefID;
                {
                    const std::scoped_lock lock{ g_overlayRuntimeMutex };
                    const auto now = std::chrono::steady_clock::now();
                    const auto last =
                        g_overlayRuntime.lastAppliedByRef.find(refID);
                    if ((last != g_overlayRuntime.lastAppliedByRef.end() &&
                         now - last->second < kOverlayReapplyCooldown) ||
                        !g_overlayRuntime.inFlightRefs.insert(refID).second) {
                        ++skippedCount;
                        continue;
                    }
                }
                RunOverlayApplyTask(refID, baseID, assignment);
                ++appliedCount;
                if (!MutationOperational()) {
                    break;
                }
            }
            REX::INFO(
                "[NpcAppearance] overlay sweep reason={} winners={} applied={} skipped={}",
                a_reason, winners.size(), appliedCount, skippedCount);
        }

        void PumpDeferredLoadSweep() noexcept;

        void ScheduleDeferredLoadSweepRetry() noexcept
        {
            bool expected = false;
            if (!g_deferredLoadSweepRetryScheduled.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                return;
            }

            try {
                // A retry scheduled directly from an SFSE task can be consumed
                // again by the same task drain, starving the main loop while
                // the native queue is disabled around LoadGame. Wait off-thread
                // for the verified queue gate, then enqueue one SFSE handoff.
                // The worker is demand-driven and bounded; it performs no
                // game-object work.
                std::thread([] {
                    try {
                        for (std::uint32_t wait = 1;
                             wait <= kLoadSweepRetryMaxWaits;
                             ++wait) {
                            std::this_thread::sleep_for(kLoadSweepRetryDelay);
                            const auto diagnostics =
                                Util::NativeMainThreadQueue::GetDiagnostics();
                            if (!diagnostics.queueEnabled ||
                                diagnostics.singleton == 0) {
                                continue;
                            }

                            const auto* tasks = SFSE::GetTaskInterface();
                            if (!tasks) {
                                g_deferredLoadSweepRetryScheduled.store(
                                    false, std::memory_order_release);
                                KillMutation(
                                    "SFSE task interface unavailable for deferred load retry");
                                REX::CRITICAL(
                                    "[NpcAppearance] deferred load retry could not acquire the SFSE task interface; pending work remains fail-closed");
                                return;
                            }
                            tasks->AddTask([] {
                                g_deferredLoadSweepRetryScheduled.store(
                                    false, std::memory_order_release);
                                PumpDeferredLoadSweep();
                            });
                            return;
                        }

                        g_deferredLoadSweepRetryScheduled.store(
                            false, std::memory_order_release);
                        KillMutation(
                            "native queue unavailable for deferred load retry");
                        REX::CRITICAL(
                            "[NpcAppearance] deferred load retry timed out after {} ms waiting for the native queue; pending work remains fail-closed",
                            kLoadSweepRetryMaxWaits *
                                static_cast<std::uint32_t>(kLoadSweepRetryDelay.count()));
                    } catch (const std::exception& e) {
                        g_deferredLoadSweepRetryScheduled.store(
                            false, std::memory_order_release);
                        KillMutation("deferred load retry worker threw");
                        REX::CRITICAL(
                            "[NpcAppearance] deferred load retry worker threw '{}'; pending work remains fail-closed",
                            e.what());
                    } catch (...) {
                        g_deferredLoadSweepRetryScheduled.store(
                            false, std::memory_order_release);
                        KillMutation("deferred load retry worker threw");
                        REX::CRITICAL(
                            "[NpcAppearance] deferred load retry worker threw; pending work remains fail-closed");
                    }
                }).detach();
            } catch (const std::exception& e) {
                g_deferredLoadSweepRetryScheduled.store(
                    false, std::memory_order_release);
                KillMutation("deferred load retry scheduling threw");
                REX::CRITICAL(
                    "[NpcAppearance] deferred load retry scheduling threw '{}'; pending work remains fail-closed",
                    e.what());
            } catch (...) {
                g_deferredLoadSweepRetryScheduled.store(
                    false, std::memory_order_release);
                KillMutation("deferred load retry scheduling threw");
                REX::CRITICAL(
                    "[NpcAppearance] deferred load retry scheduling threw; pending work remains fail-closed");
            }
        }

        void PumpDeferredLoadSweep() noexcept
        {
            std::shared_ptr<DeferredLoadSweepTask> pending;
            try {
                {
                    const std::scoped_lock lock{ g_deferredLoadSweepMutex };
                    if (!g_deferredLoadSweepTask || g_deferredLoadSweepInFlight) {
                        return;
                    }
                    pending = g_deferredLoadSweepTask;
                    g_deferredLoadSweepInFlight = pending;
                }

                auto execute = [pending] {
                    bool complete = true;
                    std::uint32_t attempt = 0;
                    try {
                        {
                            const std::scoped_lock lock{ g_deferredLoadSweepMutex };
                            attempt = ++pending->attempts;
                        }
                        complete = pending->run(attempt);
                        if (!complete &&
                            attempt >= kLoadSweepReadyMaxNativeFrames) {
                            KillMutation("load-return actor readiness timed out");
                            REX::CRITICAL(
                                "[NpcAppearance] LOAD-RETURN generation={} readiness TIMEOUT after {} verified native frames; no mutation",
                                pending->generation, attempt);
                            complete = true;
                        }
                    } catch (const std::exception& e) {
                        KillMutation("deferred load-return native task threw");
                        try {
                            REX::CRITICAL(
                                "[NpcAppearance] deferred LOAD-RETURN generation={} threw '{}' inside the verified drain",
                                pending->generation, e.what());
                        } catch (...) {
                        }
                    } catch (...) {
                        KillMutation("deferred load-return native task threw");
                        try {
                            REX::CRITICAL(
                                "[NpcAppearance] deferred LOAD-RETURN generation={} threw inside the verified drain",
                                pending->generation);
                        } catch (...) {
                        }
                    }
                    bool retry = false;
                    {
                        const std::scoped_lock lock{ g_deferredLoadSweepMutex };
                        if (complete && g_deferredLoadSweepTask == pending) {
                            g_deferredLoadSweepTask.reset();
                        }
                        retry = g_deferredLoadSweepTask != nullptr;
                        if (g_deferredLoadSweepInFlight == pending) {
                            g_deferredLoadSweepInFlight.reset();
                        }
                    }
                    if (retry) {
                        ScheduleDeferredLoadSweepRetry();
                    }
                };

                const auto diagnostics =
                    Util::NativeMainThreadQueue::GetDiagnostics();
                if (diagnostics.insideDrain) {
                    execute();
                    return;
                }

                const auto postResult = Util::NativeMainThreadQueue::Post(
                    std::move(execute), "NpcAppearance.LoadSweep",
                    [pending] {
                        bool retry = false;
                        {
                            const std::scoped_lock lock{ g_deferredLoadSweepMutex };
                            if (g_deferredLoadSweepInFlight == pending) {
                                g_deferredLoadSweepInFlight.reset();
                            }
                            retry = g_deferredLoadSweepTask != nullptr;
                        }
                        if (retry) {
                            ScheduleDeferredLoadSweepRetry();
                        }
                    });
                if (postResult ==
                    Util::NativeMainThreadQueue::PostResult::kQueued) {
                    if (pending->attempts == 0) {
                        REX::INFO(
                            "[NpcAppearance] LOAD-RETURN generation={} queued for verified native drain after queueDeferral={}",
                            pending->generation, pending->deferralLogged);
                    }
                    return;
                }

                bool logDeferral = false;
                {
                    const std::scoped_lock lock{ g_deferredLoadSweepMutex };
                    if (g_deferredLoadSweepInFlight == pending) {
                        g_deferredLoadSweepInFlight.reset();
                    }
                    if (g_deferredLoadSweepTask == pending &&
                        !pending->deferralLogged) {
                        pending->deferralLogged = true;
                        logDeferral = true;
                    }
                }
                if (logDeferral) {
                    REX::INFO(
                        "[NpcAppearance] LOAD-RETURN generation={} deferred until native queue is available result={} tid={} queueEnabled={} singleton=0x{:X}",
                        pending->generation,
                        Util::NativeMainThreadQueue::ToString(postResult),
                        diagnostics.currentThreadID, diagnostics.queueEnabled,
                        diagnostics.singleton);
                }
                ScheduleDeferredLoadSweepRetry();
            } catch (const std::exception& e) {
                {
                    const std::scoped_lock lock{ g_deferredLoadSweepMutex };
                    if (g_deferredLoadSweepInFlight == pending) {
                        g_deferredLoadSweepInFlight.reset();
                    }
                }
                KillMutation("deferred load-return scheduling threw");
                REX::CRITICAL(
                    "[NpcAppearance] deferred LOAD-RETURN scheduling threw '{}'; no mutation",
                    e.what());
            } catch (...) {
                {
                    const std::scoped_lock lock{ g_deferredLoadSweepMutex };
                    if (g_deferredLoadSweepInFlight == pending) {
                        g_deferredLoadSweepInFlight.reset();
                    }
                }
                KillMutation("deferred load-return scheduling threw");
                REX::CRITICAL(
                    "[NpcAppearance] deferred LOAD-RETURN scheduling threw; no mutation");
            }
        }

        // Load handling since Phase 4: no persistent apply, no bracket. The
        // deferred task waits for the blocking menus to close, then runs one
        // overlay sweep; ReferenceSet3d windows cover everything after that.
        void OnLoadGameReturnImpl() noexcept
        {
            if (!g_runtimeArmed.load(std::memory_order_acquire)) {
                return;
            }
            try {
                const auto loadGeneration =
                    g_loadGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
                const auto loadReturn =
                    g_loadReturnCount.fetch_add(1, std::memory_order_relaxed) + 1;

                auto pending = std::make_shared<DeferredLoadSweepTask>();
                pending->generation = loadGeneration;
                pending->run = [loadReturn, loadGeneration](
                                   const std::uint32_t attempt) {
                    try {
                        if (g_loadGeneration.load(std::memory_order_acquire) !=
                            loadGeneration) {
                            REX::WARN(
                                "[NpcAppearance] LOAD-RETURN return={} generation={} superseded before native execution",
                                loadReturn, loadGeneration);
                            return true;
                        }
                        auto* ui = RE::UI::GetSingleton();
                        const bool menusBlockMutation = !ui ||
                            ui->IsMenuOpen(RE::BSFixedString{ "MainMenu" }) ||
                            ui->IsMenuOpen(RE::BSFixedString{ "LoadingMenu" });
                        if (menusBlockMutation) {
                            if (attempt == 1 || (attempt % 60) == 0) {
                                REX::INFO(
                                    "[NpcAppearance] LOAD-RETURN return={} generation={} readiness WAIT attempt={} reason=blocking-menu",
                                    loadReturn, loadGeneration, attempt);
                            }
                            return false;
                        }
                        if (!MutationOperational()) {
                            REX::WARN(
                                "[NpcAppearance] LOAD-RETURN return={} generation={} mutation not operational; no overlay sweep",
                                loadReturn, loadGeneration);
                            return true;
                        }
                        RunOverlaySweep("load-return");
                        REX::INFO(
                            "[NpcAppearance] LOAD-RETURN done return={} generation={} tid={}",
                            loadReturn, loadGeneration, ::GetCurrentThreadId());
                        return true;
                    } catch (const std::exception& e) {
                        KillMutation("load-return native task threw");
                        REX::CRITICAL(
                            "[NpcAppearance] LOAD-RETURN native task threw '{}'; swallowed inside verified drain",
                            e.what());
                    } catch (...) {
                        KillMutation("load-return native task threw");
                        REX::CRITICAL(
                            "[NpcAppearance] LOAD-RETURN native task threw; swallowed inside verified drain");
                    }
                    return true;
                };
                std::optional<std::uint64_t> supersededGeneration;
                {
                    const std::scoped_lock lock{ g_deferredLoadSweepMutex };
                    if (g_deferredLoadSweepTask) {
                        supersededGeneration = g_deferredLoadSweepTask->generation;
                    }
                    g_deferredLoadSweepTask = std::move(pending);
                }
                if (supersededGeneration) {
                    REX::WARN(
                        "[NpcAppearance] LOAD-RETURN generation={} superseded pending generation={}; successor will run after the in-flight identity retires",
                        loadGeneration, *supersededGeneration);
                }
                PumpDeferredLoadSweep();
            } catch (const std::exception& e) {
                KillMutation("load-return callback scheduling threw");
                REX::CRITICAL(
                    "[NpcAppearance] LOAD-RETURN scheduling threw '{}'; no mutation",
                    e.what());
            } catch (...) {
                KillMutation("load-return callback scheduling threw");
                REX::CRITICAL(
                    "[NpcAppearance] LOAD-RETURN scheduling threw; no mutation");
            }
        }

        // Passive load-finished signal: BGSSaveLoadManager fires this for
        // every pump-driven save/load op. Known gaps (documented at the
        // event's RE notes): silent saves, new game, Unity/NG+ — there the
        // per-actor Set3d windows are the only styling path.
        class SaveLoadEventSink final :
            public RE::BSTEventSink<RE::SaveLoadEvent>
        {
        public:
            static SaveLoadEventSink& GetSingleton() noexcept
            {
                static SaveLoadEventSink singleton;
                return singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::SaveLoadEvent& a_event,
                RE::BSTEventSource<RE::SaveLoadEvent>*) noexcept override
            {
                if (a_event.status ==
                    RE::SaveLoadEvent::Status::kLoadSucceeded) {
                    OnLoadGameReturnImpl();
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // ==================================================================
        // Startup
        // Fail-closed arming sequence: mutation operational -> packs
        // directory -> validated winners -> Set3d sink -> overlay runtime.
        // ==================================================================
        void OnNpcAppearanceDataLoaded()
        {

            if (!MutationOperational()) {
                REX::CRITICAL(
                    "[NpcAppearance] startup mutation disabled; runtimeOperational={} mutationKilled={}",
                    g_runtimeOperational.load(std::memory_order_relaxed),
                    g_mutationKilled.load(std::memory_order_relaxed));
                return;
            }

            const auto packsRoot = DefaultPacksDirectory();
            std::error_code ec;
            const bool packsPresent =
                std::filesystem::is_directory(packsRoot, ec) && !ec;
            if (!packsPresent) {
                REX::INFO("[NpcAppearance] startup disabled: packs directory is absent ({})",
                          packsRoot.string());
                return;
            }

            const LineSink startupOut = [](const std::string& a_text) {
                REX::INFO("[NpcAppearance] startup: {}", a_text);
            };
            auto resolvedAssignments = RunScan(startupOut, packsRoot);

            const std::size_t assignments = resolvedAssignments.size();
            {
                const std::scoped_lock lock{ g_eventMutex };
                g_sceneAssignments = std::move(resolvedAssignments);
            }
            if (assignments == 0) {
                REX::WARN("[NpcAppearance] startup found no fully validated winning assignments; overlay runtime remains disabled");
                return;
            }

            // The Set3d trigger registers unconditionally; the handler
            // no-ops once mutation is killed. Without the source, only the
            // post-load sweep can style actors.
            if (!g_overlaySinkRegistered.load(std::memory_order_acquire)) {
                auto* set3dSource = RE::RuntimeComponentDBFactory::
                    ReferenceSet3d::GetEventSource();
                if (set3dSource) {
                    set3dSource->RegisterSink(
                        &OverlaySet3dSink::GetSingleton());
                    g_overlaySinkRegistered.store(
                        true, std::memory_order_release);
                } else {
                    REX::WARN(
                        "[NpcAppearance] ReferenceSet3d event source unavailable; only the post-load sweep can style actors this session");
                }
            }
            g_runtimeArmed.store(true, std::memory_order_release);
            REX::INFO(
                "[NpcAppearance] overlay runtime ARMED assignments={} set3dSinkRegistered={}; per-3D-build transient windows + post-load sweep",
                assignments,
                g_overlaySinkRegistered.load(std::memory_order_relaxed));
        }

    }

    namespace Detail
    {
        // The SaveLoadEvent sink is observer-only telemetry: it is not
        // load-bearing for mutation safety, so none of these gates consult
        // it. Correctness comes from the per-call byte gates, the verified
        // drain, and the overlay window's restore proof.
        bool MutationOperational() noexcept
        {
            return g_runtimeOperational.load(std::memory_order_acquire) &&
                !g_mutationKilled.load(std::memory_order_acquire);
        }

        bool RestoreOperational() noexcept
        {
            return g_runtimeOperational.load(std::memory_order_acquire);
        }

        void KillMutation(const std::string_view a_reason) noexcept
        {
            bool expected = false;
            if (g_mutationKilled.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                try {
                    REX::CRITICAL("[NpcAppearance] mutation killed for this process: {}", a_reason);
                } catch (...) {
                }
            }
        }

        bool RequireMutationOperational(
            const LineSink& a_out,
            const std::string_view a_operation)
        {
            if (MutationOperational()) {
                return true;
            }
            a_out(std::format(
                "{}: mutation disabled runtimeOperational={} mutationKilled={}; FAIL CLOSED",
                a_operation,
                g_runtimeOperational.load(std::memory_order_relaxed),
                g_mutationKilled.load(std::memory_order_relaxed)));
            return false;
        }

        bool RequireRestoreOperational(
            const LineSink& a_out,
            const std::string_view a_operation)
        {
            if (RestoreOperational()) {
                return true;
            }
            a_out(std::format(
                "{}: restore disabled because the runtime was never armed; FAIL CLOSED",
                a_operation));
            return false;
        }
    }

    void Initialize() noexcept
    {
        try {
            g_runtimeArmed.store(false, std::memory_order_release);
            // Observer-only: a missing event source is telemetry loss, not a
            // safety loss — the overlay runtime works without the sink.
            auto* saveLoadSource = RE::SaveLoadEvent::GetEventSource();
            if (saveLoadSource) {
                saveLoadSource->RegisterSink(&SaveLoadEventSink::GetSingleton());
                g_saveLoadSinkRegistered.store(true, std::memory_order_release);
            } else {
                REX::WARN(
                    "[NpcAppearance] SaveLoadEvent source unavailable; the post-load sweep is lost and styling relies on Set3d windows alone");
            }
            const bool deferredRetryAvailable = SFSE::GetTaskInterface() != nullptr;
            g_runtimeOperational.store(true, std::memory_order_release);
            if (!deferredRetryAvailable) {
                Detail::KillMutation(
                    "SFSE task interface unavailable for demand-driven load retries");
            }
            REX::INFO(
                "[NpcAppearance] save/load observer state saveLoadSink={} deferredRetryAvailable={} mutationKilled={} runtimeArmed={} callbacks=native-queue-shaped",
                g_saveLoadSinkRegistered.load(std::memory_order_relaxed), deferredRetryAvailable,
                g_mutationKilled.load(std::memory_order_relaxed),
                g_runtimeArmed.load(std::memory_order_relaxed));
            if (!QueueOrRunNativeTask(
                    [] { OnNpcAppearanceDataLoaded(); },
                    "NpcAppearance.StartupScan",
                    [] {
                        Detail::KillMutation("startup scan payload was dropped by the native queue");
                        REX::CRITICAL(
                            "[NpcAppearance] startup scan payload was dropped before verified native execution; mutation remains fail closed");
                    })) {
                Detail::KillMutation("startup scan could not enter the verified native queue");
            }
        } catch (const std::exception& e) {
            Detail::KillMutation("initialization threw");
            REX::CRITICAL(
                "[NpcAppearance] initialization threw '{}'; mutation stays fail closed",
                e.what());
        } catch (...) {
            Detail::KillMutation("initialization threw");
            REX::CRITICAL(
                "[NpcAppearance] initialization threw an unknown exception; mutation stays fail closed");
        }
    }

}
