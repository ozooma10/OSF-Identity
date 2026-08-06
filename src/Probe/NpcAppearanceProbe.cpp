#include "Probe/NpcAppearanceProbe.h"

#include "Probe/NpcAppearanceConfig.h"
#include "Probe/NpcAppearancePreset.h"
#include "Probe/NpcAppearanceResolver.h"
#include "pch.h"

#include "Util/NativeMainThreadQueue.h"
#include "Util/StarfieldRuntime.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Probe::NpcAppearance
{
    namespace
    {
        constexpr REL::ID kActorCopyAppearanceWorkerID{ 97401 };
        constexpr REL::ID kNpcFactorySingletonID{ 824718 };
        constexpr REL::ID kNpcFactoryVtableID{ 420871 };
        constexpr REL::ID kNpcFactoryCreateID{ 68242 };
        constexpr REL::ID kNpcPrimaryVtableID{ 420893 };
        constexpr REL::ID kNpcScalarDeletingDestructorID{ 68093 };
        constexpr REL::ID kNpcCopyAppearanceID{ 68122 };
        constexpr REL::ID kNpcSetShapeBlendID{ 68207 };
        constexpr REL::ID kNpcSetBodyMorphID{ 68208 };
        constexpr REL::ID kNpcSetBoneValueID{ 68210 };
        constexpr REL::ID kNpcSetBoneGroupValueID{ 68212 };
        constexpr REL::ID kNpcRemoveHeadPartID{ 68188 };
        constexpr REL::ID kNpcChangeHeadPartID{ 68189 };
        constexpr REL::ID kFaceDbResolveEntryID{ 37340 };
        constexpr REL::ID kNpcSetAvmDataID{ 68087 };
        constexpr REL::ID kNpcRemoveAvmDataID{ 68088 };
        constexpr REL::ID kActorAppearanceRefreshID{ 101307 };
        constexpr std::uint32_t kAppearanceRefreshDirtyActorFlag = 0x00008000;
        constexpr std::uint32_t kTargetHoldSeconds = 12;
        constexpr REL::Offset kNpcOwnedVisualCopyOffset{ 0xCD56E0 };
        constexpr std::array<std::uint8_t, 17> kActorCopyAppearanceGate{
            0x48, 0x85, 0xD2,
            0x0F, 0x84, 0x94, 0x00, 0x00, 0x00,
            0x48, 0x89, 0x5C, 0x24, 0x08,
            0x48, 0x89, 0x6C
        };
        constexpr std::array<std::uint8_t, 16> kNpcFactoryCreateGate{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
            0xEC, 0x30, 0x8B, 0xDA, 0xB9, 0x58, 0x04, 0x00
        };
        constexpr std::array<std::uint8_t, 16> kNpcDestructorGate{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
            0xEC, 0x20, 0x8B, 0xDA, 0x48, 0x8B, 0xF9, 0xE8
        };
        constexpr std::array<std::uint8_t, 16> kNpcCopyAppearanceGate{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
            0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57
        };
        constexpr std::array<std::uint8_t, 16> kNpcSetShapeBlendGate{
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48,
            0x89, 0x68, 0x20, 0xC5, 0xFA, 0x11, 0x50, 0x18
        };
        constexpr std::array<std::uint8_t, 16> kNpcSetBodyMorphGate{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
            0x24, 0x10, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83
        };
        constexpr std::array<std::uint8_t, 16> kNpcSetBoneValueGate{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x89, 0x54, 0x24,
            0x10, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30
        };
        constexpr std::array<std::uint8_t, 16> kNpcSetBoneGroupValueGate{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0xC5, 0xFA, 0x11,
            0x5C, 0x24, 0x20, 0x89, 0x54, 0x24, 0x10, 0x55
        };
        constexpr std::array<std::uint8_t, 16> kNpcRemoveHeadPartGate{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
            0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57
        };
        constexpr std::array<std::uint8_t, 16> kNpcChangeHeadPartGate{
            0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x54,
            0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41
        };
        constexpr std::array<std::uint8_t, 16> kFaceDbResolveEntryGate{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
            0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x8B
        };
        constexpr std::array<std::uint8_t, 16> kNpcSetAvmDataGate{
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83,
            0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9
        };
        constexpr std::array<std::uint8_t, 16> kNpcRemoveAvmDataGate{
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
            0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48
        };
        constexpr std::array<std::uint8_t, 16> kNpcOwnedVisualCopyGate{
            0x44, 0x88, 0x44, 0x24, 0x18, 0x53, 0x56, 0x57,
            0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
        };
        constexpr std::array<std::uint8_t, 16> kActorAppearanceRefreshGate{
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
            0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57
        };

        std::mutex                    g_eventMutex;
        std::unordered_set<RE::TESFormID> g_targetBaseIDs;
        std::unordered_set<RE::TESFormID> g_loadedTargetRefs;
        std::atomic<bool>             g_eventRegistered{ false };
        std::atomic<std::uint64_t>    g_eventCount{ 0 };
        std::atomic<std::uint64_t>    g_matchingLoadCount{ 0 };
        std::atomic<std::uint64_t>    g_matchingUnloadCount{ 0 };
        std::atomic<std::uint64_t>    g_debouncedLoadCount{ 0 };
        std::atomic<std::uint64_t>    g_queuedApplyCount{ 0 };
        std::atomic<std::uint64_t>    g_ranApplyCount{ 0 };

        std::unordered_set<RE::TESFormID> g_sceneTargetRefs;
        std::unordered_map<RE::TESFormID, SelectedAssignment> g_sceneAssignments;
        constexpr std::uint32_t kSceneStableNativeFrames = 30;
        struct PendingSceneApply
        {
            RE::TESFormID refID{ 0 };
            RE::TESFormID baseID{ 0 };
            std::uint32_t  eventThreadID{ 0 };
            std::uint64_t  sequence{ 0 };
            std::uint32_t  stableNativeFrames{ 0 };
            bool           loadingDeferralLogged{ false };
            bool           invalidStateLogged{ false };
        };
        std::optional<PendingSceneApply> g_pendingSceneApply;
        std::uint64_t                    g_nextSceneSequence{ 0 };
        struct SceneSet3dSuppression
        {
            std::uint32_t count{ 0 };
            std::uint64_t deadlineMs{ 0 };
        };
        std::unordered_map<RE::TESFormID, SceneSet3dSuppression> g_sceneSet3dSuppressions;
        std::atomic<bool>             g_sceneRegistered{ false };
        std::atomic<bool>             g_sceneDispatchObserveArmed{ false };
        std::atomic<bool>             g_sceneAutoTrialArmed{ false };
        std::atomic<bool>             g_scenePersistentEnabled{ false };
        std::atomic<bool>             g_startupPackagesPresent{ false };
        std::atomic<bool>             g_startupPersistentArmed{ false };
        std::atomic<std::uint64_t>    g_sceneSet3dCount{ 0 };
        std::atomic<std::uint64_t>    g_sceneDetachCount{ 0 };
        std::atomic<std::uint64_t>    g_sceneMatchingSet3dCount{ 0 };
        std::atomic<std::uint64_t>    g_sceneMatchingDetachCount{ 0 };
        std::atomic<std::uint64_t>    g_sceneDebouncedCount{ 0 };
        std::atomic<std::uint64_t>    g_sceneQueuedApplyCount{ 0 };
        std::atomic<std::uint64_t>    g_sceneRanApplyCount{ 0 };
        std::atomic<std::uint64_t>    g_sceneAutoTrialAttemptCount{ 0 };
        std::atomic<std::uint64_t>    g_sceneAutoTrialApplyCount{ 0 };
        std::atomic<std::uint64_t>    g_scenePersistentAttemptCount{ 0 };
        std::atomic<std::uint64_t>    g_scenePersistentApplyCount{ 0 };
        std::atomic<std::uint64_t>    g_scenePersistentRemovalCount{ 0 };
        std::atomic<std::uint64_t>    g_sceneSuppressedSet3dCount{ 0 };
        std::atomic<std::uint64_t>    g_sceneSuppressedDetachCount{ 0 };
        std::atomic<std::uint64_t>    g_sceneNativeFrameCount{ 0 };
        std::atomic<std::uint64_t>    g_sceneNativeLoadingDeferralCount{ 0 };
        std::atomic<std::uint64_t>    g_sceneNativeReadyCount{ 0 };
        std::atomic<std::uint64_t>    g_sceneNativeObservePassCount{ 0 };
        std::atomic<std::uint32_t>    g_sceneNativeThreadID{ 0 };
        std::atomic<bool>             g_sceneNativeTaskInFlight{ false };
        std::atomic<bool>             g_sceneNativePostFailureLogged{ false };

        enum class TargetTrialMode
        {
            kImmediate,
            kHold,
            kRenderLatch,
            kOwnedSnapshotLatch,
            kPersistentLatch
        };

        void RunTargetTrial(const LineSink& a_out, const std::vector<std::string>& a_args,
                            TargetTrialMode a_mode, bool* a_completed = nullptr);
        [[nodiscard]] bool TargetHoldActive();
        [[nodiscard]] bool ForgetPersistentState(RE::TESFormID a_refID);

        void SuppressNextSceneSet3d(const RE::TESFormID a_refID)
        {
            constexpr std::uint64_t kSuppressionWindowMs = 2000;
            const auto now = ::GetTickCount64();
            const std::scoped_lock lock{ g_eventMutex };
            auto& suppression = g_sceneSet3dSuppressions[a_refID];
            if (now > suppression.deadlineMs) {
                suppression.count = 0;
            }
            ++suppression.count;
            suppression.deadlineMs = now + kSuppressionWindowMs;
        }

        class ObjectLoadedSink : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
        {
        public:
            static ObjectLoadedSink& GetSingleton() noexcept
            {
                static ObjectLoadedSink singleton;
                return singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESObjectLoadedEvent& a_event,
                RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
            {
                g_eventCount.fetch_add(1, std::memory_order_relaxed);

                if (!a_event.loaded) {
                    bool wasTarget = false;
                    {
                        const std::scoped_lock lock{ g_eventMutex };
                        wasTarget = g_loadedTargetRefs.erase(a_event.formID) != 0;
                    }
                    if (wasTarget) {
                        g_matchingUnloadCount.fetch_add(1, std::memory_order_relaxed);
                        REX::INFO("[NpcAppearance] TESObjectLoadedEvent ref=0x{:08X} loaded=0; per-reference state cleared tid={}",
                                  a_event.formID, ::GetCurrentThreadId());
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }

                auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_event.formID);
                auto* base = actor ? actor->GetNPC() : nullptr;
                if (!base) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                bool matchingBase = false;
                bool firstForGeneration = false;
                {
                    const std::scoped_lock lock{ g_eventMutex };
                    matchingBase = g_targetBaseIDs.contains(base->GetFormID());
                    if (matchingBase) {
                        firstForGeneration = g_loadedTargetRefs.insert(a_event.formID).second;
                    }
                }
                if (!matchingBase) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!firstForGeneration) {
                    g_debouncedLoadCount.fetch_add(1, std::memory_order_relaxed);
                    return RE::BSEventNotifyControl::kContinue;
                }

                g_matchingLoadCount.fetch_add(1, std::memory_order_relaxed);
                const auto refID = a_event.formID;
                const auto baseID = base->GetFormID();
                const auto eventTid = ::GetCurrentThreadId();
                if (const auto* tasks = SFSE::GetTaskInterface()) {
                    g_queuedApplyCount.fetch_add(1, std::memory_order_relaxed);
                    tasks->AddTask([refID, baseID, eventTid] {
                        auto* queuedActor = RE::TESForm::LookupByID<RE::Actor>(refID);
                        auto* queuedBase = queuedActor ? queuedActor->GetNPC() : nullptr;
                        if (!queuedBase || queuedBase->GetFormID() != baseID) {
                            REX::INFO("[NpcAppearance] queued apply ref=0x{:08X} skipped (unloaded/rebound) eventTid={} taskTid={}",
                                      refID, eventTid, ::GetCurrentThreadId());
                            return;
                        }
                        g_ranApplyCount.fetch_add(1, std::memory_order_relaxed);
                        REX::INFO("[NpcAppearance] queued apply ref=0x{:08X} base=0x{:08X} reached game task; mutation disabled eventTid={} taskTid={}",
                                  refID, baseID, eventTid, ::GetCurrentThreadId());
                    });
                } else {
                    REX::WARN("[NpcAppearance] TESObjectLoadedEvent matched ref=0x{:08X}, but SFSE TaskInterface is unavailable",
                              refID);
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        struct NonVisualSnapshot
        {
            std::string         editorID;
            std::string         name;
            std::uint32_t       actorFlagsExceptSex{ 0 };
            std::uint16_t       level{ 0 };
            std::uint16_t       calcLevelMin{ 0 };
            std::uint16_t       calcLevelMax{ 0 };
            std::uint16_t       baseDisposition{ 0 };
            std::uint16_t       templateUseFlags{ 0 };
            std::uint8_t        pronoun{ 0 };
            std::size_t         factionCount{ 0 };
            const void*         factionData{ nullptr };
            std::size_t         inventoryCount{ 0 };
            const void*         inventoryData{ nullptr };
            RE::TESRace*        race{ nullptr };
            RE::TESRace*        originalRace{ nullptr };
            RE::TESClass*       npcClass{ nullptr };
            RE::BGSVoiceType*   voiceType{ nullptr };
            RE::TESCombatStyle* combatStyle{ nullptr };
            RE::BGSOutfit*      defaultOutfit{ nullptr };
            RE::BGSOutfit*      sleepOutfit{ nullptr };
            RE::TESFaction*     crimeFaction{ nullptr };
            std::array<std::byte, sizeof(RE::AIDATA_GAME)> aiData{};

            [[nodiscard]] bool operator==(const NonVisualSnapshot&) const = default;
        };

        class ReferenceSet3dSink :
            public RE::BSTEventSink<RE::RuntimeComponentDBFactory::ReferenceSet3d>
        {
        public:
            static ReferenceSet3dSink& GetSingleton() noexcept
            {
                static ReferenceSet3dSink singleton;
                return singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::RuntimeComponentDBFactory::ReferenceSet3d& a_event,
                RE::BSTEventSource<RE::RuntimeComponentDBFactory::ReferenceSet3d>*) override
            {
                g_sceneSet3dCount.fetch_add(1, std::memory_order_relaxed);

                auto* ref = a_event.ref.get();
                auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
                auto* base = actor ? actor->GetNPC() : nullptr;
                if (!base) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                const auto refID = actor->GetFormID();
                const auto baseID = base->GetFormID();
                bool matchingBase = false;
                bool firstForGeneration = false;
                bool selfRefreshSuppressed = false;
                {
                    const std::scoped_lock lock{ g_eventMutex };
                    matchingBase = g_targetBaseIDs.contains(baseID);
                    if (matchingBase) {
                        const auto suppression = g_sceneSet3dSuppressions.find(refID);
                        if (suppression != g_sceneSet3dSuppressions.end()) {
                            if (::GetTickCount64() <= suppression->second.deadlineMs &&
                                suppression->second.count != 0) {
                                selfRefreshSuppressed = true;
                                if (--suppression->second.count == 0) {
                                    g_sceneSet3dSuppressions.erase(suppression);
                                }
                                g_sceneTargetRefs.insert(refID);
                            } else {
                                g_sceneSet3dSuppressions.erase(suppression);
                            }
                        }
                        if (!selfRefreshSuppressed) {
                            firstForGeneration = g_sceneTargetRefs.insert(refID).second;
                        }
                    }
                }
                if (!matchingBase) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (selfRefreshSuppressed) {
                    g_sceneSuppressedSet3dCount.fetch_add(1, std::memory_order_relaxed);
                    REX::INFO("[NpcAppearance] ReferenceSet3d suppressed self-refresh ref=0x{:08X} base=0x{:08X} eventTid={}",
                              refID, baseID, ::GetCurrentThreadId());
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!firstForGeneration) {
                    g_sceneDebouncedCount.fetch_add(1, std::memory_order_relaxed);
                    return RE::BSEventNotifyControl::kContinue;
                }

                g_sceneMatchingSet3dCount.fetch_add(1, std::memory_order_relaxed);
                const auto eventTid = ::GetCurrentThreadId();
                std::uint64_t sequence = 0;
                {
                    const std::scoped_lock lock{ g_eventMutex };
                    sequence = ++g_nextSceneSequence;
                    g_pendingSceneApply = PendingSceneApply{
                        .refID = refID,
                        .baseID = baseID,
                        .eventThreadID = eventTid,
                        .sequence = sequence,
                    };
                    g_sceneQueuedApplyCount.fetch_add(1, std::memory_order_relaxed);
                }
                REX::INFO("[NpcAppearance] ReferenceSet3d matched ref=0x{:08X} base=0x{:08X} refPtr={} eventTid={}; published native-main-thread handoff sequence={} (no event-thread mutation)",
                          refID, baseID, static_cast<void*>(actor), eventTid, sequence);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        class ReferenceDetachSink :
            public RE::BSTEventSink<RE::RuntimeComponentDBFactory::ReferenceDetach>
        {
        public:
            static ReferenceDetachSink& GetSingleton() noexcept
            {
                static ReferenceDetachSink singleton;
                return singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::RuntimeComponentDBFactory::ReferenceDetach& a_event,
                RE::BSTEventSource<RE::RuntimeComponentDBFactory::ReferenceDetach>*) override
            {
                g_sceneDetachCount.fetch_add(1, std::memory_order_relaxed);

                auto* ref = a_event.ref.get();
                if (!ref) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const auto refID = ref->GetFormID();
                bool wasTarget = false;
                bool selfRefreshSuppressed = false;
                {
                    const std::scoped_lock lock{ g_eventMutex };
                    const auto suppression = g_sceneSet3dSuppressions.find(refID);
                    selfRefreshSuppressed =
                        suppression != g_sceneSet3dSuppressions.end() &&
                        ::GetTickCount64() <= suppression->second.deadlineMs &&
                        suppression->second.count != 0;
                    if (!selfRefreshSuppressed) {
                        wasTarget = g_sceneTargetRefs.erase(refID) != 0;
                        if (g_pendingSceneApply && g_pendingSceneApply->refID == refID) {
                            g_pendingSceneApply.reset();
                        }
                    }
                }
                if (selfRefreshSuppressed) {
                    g_sceneSuppressedDetachCount.fetch_add(1, std::memory_order_relaxed);
                    REX::INFO("[NpcAppearance] ReferenceDetach suppressed self-refresh ref=0x{:08X}; scene generation and persistent removal state preserved eventTid={}",
                              refID, ::GetCurrentThreadId());
                    return RE::BSEventNotifyControl::kContinue;
                }
                const bool forgotPersistent = ForgetPersistentState(refID);
                if (wasTarget) {
                    g_sceneMatchingDetachCount.fetch_add(1, std::memory_order_relaxed);
                    REX::INFO("[NpcAppearance] ReferenceDetach matched ref=0x{:08X}; scene generation cleared persistentStateRetired={} eventTid={}",
                              refID, forgotPersistent, ::GetCurrentThreadId());
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        struct VisualSeedSnapshot
        {
            float                         thin{ 0.0F };
            float                         muscular{ 0.0F };
            float                         fat{ 0.0F };
            std::vector<RE::BGSHeadPart*> headParts;
            const void*                   headPartStorage{ nullptr };
            std::size_t                   morphRegionCount{ 0 };
            const void*                   morphRegionStorage{ nullptr };
            std::size_t                   boneValueCount{ 0 };
            const void*                   boneValueStorage{ nullptr };
            std::size_t                   boneGroupCount{ 0 };
            const void*                   boneGroupStorage{ nullptr };
            std::size_t                   tintCount{ 0 };
            const void*                   tintStorage{ nullptr };
            std::uint8_t                  skinToneIndex{ 0 };
            std::string                   teeth;
            std::string                   jewelryColor;
            std::string                   eyeColor;
            std::string                   hairColor;
            std::string                   facialColor;
            std::string                   eyebrowColor;
            std::size_t                   shapeBlendCount{ 0 };
            const void*                   shapeBlendStorage{ nullptr };
            std::uint8_t                  pronoun{ 0 };

            [[nodiscard]] bool operator==(const VisualSeedSnapshot&) const = default;
        };

        [[nodiscard]] std::string JoinArguments(const std::vector<std::string>& a_args,
                                                const std::size_t a_begin)
        {
            std::string joined;
            for (std::size_t i = a_begin; i < a_args.size(); ++i) {
                if (!joined.empty()) {
                    joined.push_back(' ');
                }
                joined += a_args[i];
            }
            return joined;
        }

        [[nodiscard]] std::optional<std::uint32_t> ParseFormID(std::string_view a_text)
        {
            if (a_text.starts_with("0x") || a_text.starts_with("0X")) {
                a_text.remove_prefix(2);
            }
            if (a_text.empty()) {
                return std::nullopt;
            }
            std::uint32_t value = 0;
            const auto [ptr, ec] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), value, 16);
            if (ec != std::errc{} || ptr != a_text.data() + a_text.size()) {
                return std::nullopt;
            }
            return value;
        }

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

        [[nodiscard]] std::filesystem::path DefaultPackagesDirectory()
        {
            return DefaultPluginDirectory() / L"Packages";
        }

        [[nodiscard]] std::filesystem::path DefaultDataDirectory()
        {
            std::wstring buffer(32768, L'\0');
            const auto length =
                ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0 || length >= buffer.size()) {
                return {};
            }
            buffer.resize(length);
            return std::filesystem::path{ buffer }.parent_path() / L"Data";
        }

        [[nodiscard]] const char* SafeText(const char* a_text) noexcept
        {
            return a_text ? a_text : "";
        }

        [[nodiscard]] NonVisualSnapshot Snapshot(RE::TESNPC* a_npc)
        {
            NonVisualSnapshot snap;
            snap.editorID = SafeText(a_npc->GetFormEditorID());
            snap.name = SafeText(a_npc->GetFullName());
            constexpr auto kSexBit = static_cast<std::uint32_t>(RE::ACTOR_BASE_DATA::Flag::kFemale);
            snap.actorFlagsExceptSex = a_npc->actorData.actorBaseFlags.underlying() & ~kSexBit;
            snap.level = a_npc->actorData.level;
            snap.calcLevelMin = a_npc->actorData.calcLevelMin;
            snap.calcLevelMax = a_npc->actorData.calcLevelMax;
            snap.baseDisposition = a_npc->actorData.baseDisposition;
            snap.templateUseFlags = a_npc->actorData.templateUseFlags.underlying();
            snap.pronoun = a_npc->pronoun.underlying();
            snap.factionCount = a_npc->factions.size();
            snap.factionData = a_npc->factions.data();
            snap.inventoryCount = a_npc->containerObjects.size();
            snap.inventoryData = a_npc->containerObjects.data();
            snap.race = a_npc->formRace;
            snap.originalRace = a_npc->originalRace;
            snap.npcClass = a_npc->npcClass;
            snap.voiceType = a_npc->voiceType;
            snap.combatStyle = a_npc->combatStyle;
            snap.defaultOutfit = a_npc->defaultOutfit;
            snap.sleepOutfit = a_npc->sleepOutfit;
            snap.crimeFaction = a_npc->crimeFaction;
            std::memcpy(snap.aiData.data(), &a_npc->aiData, sizeof(a_npc->aiData));
            return snap;
        }

        [[nodiscard]] bool SameNonVisualIgnoringRefreshDirtyFlag(
            NonVisualSnapshot a_left, NonVisualSnapshot a_right)
        {
            a_left.actorFlagsExceptSex &= ~kAppearanceRefreshDirtyActorFlag;
            a_right.actorFlagsExceptSex &= ~kAppearanceRefreshDirtyActorFlag;
            return a_left == a_right;
        }

        [[nodiscard]] VisualSeedSnapshot SnapshotVisualSeed(RE::TESNPC* a_npc)
        {
            VisualSeedSnapshot snap;
            snap.thin = a_npc->morphWeight.thin;
            snap.muscular = a_npc->morphWeight.muscular;
            snap.fat = a_npc->morphWeight.fat;
            {
                auto headParts = a_npc->headParts.Lock();
                snap.headParts.assign((*headParts).begin(), (*headParts).end());
                snap.headPartStorage = (*headParts).data();
            }
            if (a_npc->unk3D8) {
                snap.morphRegionCount = a_npc->unk3D8->size();
                snap.morphRegionStorage = a_npc->unk3D8;
            }
            if (a_npc->unk3E0) {
                snap.boneValueCount = a_npc->unk3E0->size();
                snap.boneValueStorage = a_npc->unk3E0;
            }
            if (a_npc->unk3E8) {
                snap.boneGroupCount = a_npc->unk3E8->size();
                snap.boneGroupStorage = a_npc->unk3E8;
            }
            snap.tintCount = a_npc->tintAVMData.size();
            snap.tintStorage = a_npc->tintAVMData.data();
            snap.skinToneIndex = a_npc->skinToneIndex;
            snap.teeth = SafeText(a_npc->teeth.c_str());
            snap.jewelryColor = SafeText(a_npc->jewelryColor.c_str());
            snap.eyeColor = SafeText(a_npc->eyeColor.c_str());
            snap.hairColor = SafeText(a_npc->hairColor.c_str());
            snap.facialColor = SafeText(a_npc->facialColor.c_str());
            snap.eyebrowColor = SafeText(a_npc->eyebrowColor.c_str());
            if (a_npc->shapeBlendData) {
                snap.shapeBlendCount = a_npc->shapeBlendData->size();
                snap.shapeBlendStorage = a_npc->shapeBlendData;
            }
            snap.pronoun = a_npc->pronoun.underlying();
            return snap;
        }

        [[nodiscard]] bool SameVisualSeedValues(const VisualSeedSnapshot& a_left,
                                                const VisualSeedSnapshot& a_right)
        {
            return a_left.thin == a_right.thin &&
                   a_left.muscular == a_right.muscular &&
                   a_left.fat == a_right.fat &&
                   a_left.headParts == a_right.headParts &&
                   a_left.morphRegionCount == a_right.morphRegionCount &&
                   a_left.boneValueCount == a_right.boneValueCount &&
                   a_left.boneGroupCount == a_right.boneGroupCount &&
                   a_left.tintCount == a_right.tintCount &&
                   a_left.skinToneIndex == a_right.skinToneIndex &&
                   a_left.teeth == a_right.teeth &&
                   a_left.jewelryColor == a_right.jewelryColor &&
                   a_left.eyeColor == a_right.eyeColor &&
                   a_left.hairColor == a_right.hairColor &&
                   a_left.facialColor == a_right.facialColor &&
                   a_left.eyebrowColor == a_right.eyebrowColor &&
                   a_left.shapeBlendCount == a_right.shapeBlendCount &&
                   a_left.pronoun == a_right.pronoun;
        }

        void ReportVisualSeedComparison(const LineSink& a_out,
                                        const VisualSeedSnapshot& a_source,
                                        const VisualSeedSnapshot& a_donor)
        {
            a_out(std::format(
                "donorseed diff: morph={} headParts={} morphRegions={} boneValues={} boneGroups={} tint={} skinTone={} shapeBlend={} pronoun={} source/donor skin={}/{} pronoun={}/{}",
                a_source.thin == a_donor.thin &&
                    a_source.muscular == a_donor.muscular && a_source.fat == a_donor.fat,
                a_source.headParts == a_donor.headParts,
                a_source.morphRegionCount == a_donor.morphRegionCount,
                a_source.boneValueCount == a_donor.boneValueCount,
                a_source.boneGroupCount == a_donor.boneGroupCount,
                a_source.tintCount == a_donor.tintCount,
                a_source.skinToneIndex == a_donor.skinToneIndex,
                a_source.shapeBlendCount == a_donor.shapeBlendCount,
                a_source.pronoun == a_donor.pronoun,
                a_source.skinToneIndex, a_donor.skinToneIndex,
                a_source.pronoun, a_donor.pronoun));
            a_out(std::format(
                "donorseed diff: teeth={} jewelry={} eye={} hair={} facial={} eyebrow={} source/donor morph=({:.6g},{:.6g},{:.6g})/({:.6g},{:.6g},{:.6g})",
                a_source.teeth == a_donor.teeth,
                a_source.jewelryColor == a_donor.jewelryColor,
                a_source.eyeColor == a_donor.eyeColor,
                a_source.hairColor == a_donor.hairColor,
                a_source.facialColor == a_donor.facialColor,
                a_source.eyebrowColor == a_donor.eyebrowColor,
                a_source.thin, a_source.muscular, a_source.fat,
                a_donor.thin, a_donor.muscular, a_donor.fat));
            a_out(std::format(
                "donorseed diff strings: teeth='{}'/'{}' jewelry='{}'/'{}' eye='{}'/'{}' hair='{}'/'{}' facial='{}'/'{}' eyebrow='{}'/'{}'",
                a_source.teeth, a_donor.teeth,
                a_source.jewelryColor, a_donor.jewelryColor,
                a_source.eyeColor, a_donor.eyeColor,
                a_source.hairColor, a_donor.hairColor,
                a_source.facialColor, a_donor.facialColor,
                a_source.eyebrowColor, a_donor.eyebrowColor));
        }

        [[nodiscard]] bool HasIndependentVisualStorage(const VisualSeedSnapshot& a_source,
                                                       const VisualSeedSnapshot& a_donor)
        {
            const auto independent = [](const std::size_t a_count,
                                        const void* a_sourceStorage,
                                        const void* a_donorStorage) {
                if (a_sourceStorage && a_donorStorage == a_sourceStorage) {
                    return false;
                }
                return a_count == 0 || (a_sourceStorage && a_donorStorage);
            };
            return independent(a_source.headParts.size(), a_source.headPartStorage,
                               a_donor.headPartStorage) &&
                   independent(a_source.morphRegionCount, a_source.morphRegionStorage,
                               a_donor.morphRegionStorage) &&
                   independent(a_source.boneValueCount, a_source.boneValueStorage,
                               a_donor.boneValueStorage) &&
                   independent(a_source.boneGroupCount, a_source.boneGroupStorage,
                               a_donor.boneGroupStorage) &&
                   independent(a_source.tintCount, a_source.tintStorage,
                               a_donor.tintStorage) &&
                   independent(a_source.shapeBlendCount, a_source.shapeBlendStorage,
                               a_donor.shapeBlendStorage);
        }

        [[nodiscard]] bool SameExactVisualValues(RE::TESNPC* a_left, RE::TESNPC* a_right)
        {
            if (!a_left || !a_right ||
                a_left->morphWeight.thin != a_right->morphWeight.thin ||
                a_left->morphWeight.muscular != a_right->morphWeight.muscular ||
                a_left->morphWeight.fat != a_right->morphWeight.fat ||
                a_left->skinToneIndex != a_right->skinToneIndex ||
                a_left->pronoun.underlying() != a_right->pronoun.underlying() ||
                std::string_view{ SafeText(a_left->teeth.c_str()) } !=
                    std::string_view{ SafeText(a_right->teeth.c_str()) } ||
                std::string_view{ SafeText(a_left->jewelryColor.c_str()) } !=
                    std::string_view{ SafeText(a_right->jewelryColor.c_str()) } ||
                std::string_view{ SafeText(a_left->eyeColor.c_str()) } !=
                    std::string_view{ SafeText(a_right->eyeColor.c_str()) } ||
                std::string_view{ SafeText(a_left->hairColor.c_str()) } !=
                    std::string_view{ SafeText(a_right->hairColor.c_str()) } ||
                std::string_view{ SafeText(a_left->facialColor.c_str()) } !=
                    std::string_view{ SafeText(a_right->facialColor.c_str()) } ||
                std::string_view{ SafeText(a_left->eyebrowColor.c_str()) } !=
                    std::string_view{ SafeText(a_right->eyebrowColor.c_str()) }) {
                return false;
            }

            std::vector<RE::BGSHeadPart*> leftHeadParts;
            std::vector<RE::BGSHeadPart*> rightHeadParts;
            {
                auto locked = a_left->headParts.Lock();
                leftHeadParts.assign((*locked).begin(), (*locked).end());
            }
            {
                auto locked = a_right->headParts.Lock();
                rightHeadParts.assign((*locked).begin(), (*locked).end());
            }
            if (leftHeadParts != rightHeadParts) {
                return false;
            }

            if ((a_left->unk3D8 == nullptr) != (a_right->unk3D8 == nullptr)) {
                return false;
            }
            if (a_left->unk3D8) {
                if (a_left->unk3D8->size() != a_right->unk3D8->size()) {
                    return false;
                }
                for (std::uint32_t i = 0; i < a_left->unk3D8->size(); ++i) {
                    if ((*a_left->unk3D8)[i] != (*a_right->unk3D8)[i]) {
                        return false;
                    }
                }
            }

            if ((a_left->unk3E0 == nullptr) != (a_right->unk3E0 == nullptr)) {
                return false;
            }
            if (a_left->unk3E0) {
                if (a_left->unk3E0->size() != a_right->unk3E0->size()) {
                    return false;
                }
                for (const auto& entry : *a_left->unk3E0) {
                    const auto other = a_right->unk3E0->find(entry.key);
                    if (other == a_right->unk3E0->end() || other->value != entry.value) {
                        return false;
                    }
                }
            }

            if ((a_left->unk3E8 == nullptr) != (a_right->unk3E8 == nullptr)) {
                return false;
            }
            if (a_left->unk3E8) {
                if (a_left->unk3E8->size() != a_right->unk3E8->size()) {
                    return false;
                }
                for (const auto& outer : *a_left->unk3E8) {
                    const auto otherOuter = a_right->unk3E8->find(outer.key);
                    if (otherOuter == a_right->unk3E8->end() ||
                        (outer.value == nullptr) != (otherOuter->value == nullptr)) {
                        return false;
                    }
                    if (!outer.value) {
                        continue;
                    }
                    if (outer.value->size() != otherOuter->value->size()) {
                        return false;
                    }
                    for (const auto& inner : *outer.value) {
                        bool matched = false;
                        for (const auto& otherInner : *otherOuter->value) {
                            if (std::string_view{ SafeText(otherInner.key.c_str()) } ==
                                std::string_view{ SafeText(inner.key.c_str()) } &&
                                otherInner.value == inner.value) {
                                matched = true;
                                break;
                            }
                        }
                        if (!matched) {
                            return false;
                        }
                    }
                }
            }

            if (a_left->tintAVMData.size() != a_right->tintAVMData.size()) {
                return false;
            }
            for (const auto& avm : a_left->tintAVMData) {
                const auto other = std::ranges::find_if(
                    a_right->tintAVMData, [&](const RE::AVMData& a_entry) {
                        return std::string_view{ SafeText(a_entry.category.c_str()) } ==
                               std::string_view{ SafeText(avm.category.c_str()) };
                    });
                if (other == a_right->tintAVMData.end() ||
                    other->type != avm.type ||
                    std::string_view{ SafeText(other->unk10.name.c_str()) } !=
                        std::string_view{ SafeText(avm.unk10.name.c_str()) } ||
                    std::string_view{ SafeText(other->unk10.texturePath.c_str()) } !=
                        std::string_view{ SafeText(avm.unk10.texturePath.c_str()) } ||
                    other->unk10.color != avm.unk10.color ||
                    other->unk10.intensity != avm.unk10.intensity) {
                    return false;
                }
            }

            if ((a_left->shapeBlendData == nullptr) !=
                (a_right->shapeBlendData == nullptr)) {
                return false;
            }
            if (a_left->shapeBlendData) {
                if (a_left->shapeBlendData->size() != a_right->shapeBlendData->size()) {
                    return false;
                }
                for (const auto& entry : *a_left->shapeBlendData) {
                    bool matched = false;
                    for (const auto& other : *a_right->shapeBlendData) {
                        if (std::string_view{ SafeText(other.key.c_str()) } ==
                            std::string_view{ SafeText(entry.key.c_str()) } &&
                            other.value == entry.value) {
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) {
                        return false;
                    }
                }
            }
            return true;
        }

        struct OwnedBoneRegionSnapshot
        {
            std::uint32_t                          regionID{ 0 };
            bool                                   hasValues{ false };
            std::vector<std::pair<std::string, float>> values;
        };

        struct OwnedAvmSnapshot
        {
            RE::AVMData::Type type{ RE::AVMData::Type::kNone };
            std::string       category;
            std::string       name;
            std::string       texturePath;
            RE::Color         color;
            std::uint32_t     intensity{ 0 };
        };

        struct OwnedVisualSnapshot
        {
            float thin{ 0.0F };
            float muscular{ 0.0F };
            float fat{ 0.0F };
            std::uint8_t skinToneIndex{ 0 };
            std::uint8_t pronoun{ 0 };
            std::string teeth;
            std::string jewelryColor;
            std::string eyeColor;
            std::string hairColor;
            std::string facialColor;
            std::string eyebrowColor;
            std::vector<RE::TESFormID> headPartFormIDs;
            bool hasBodyMorphRegions{ false };
            std::vector<float> bodyMorphRegions;
            bool hasBoneValues{ false };
            std::vector<std::pair<std::uint32_t, float>> boneValues;
            bool hasBoneRegions{ false };
            std::vector<OwnedBoneRegionSnapshot> boneRegions;
            std::vector<OwnedAvmSnapshot> avms;
            bool hasShapeBlends{ false };
            std::vector<std::pair<std::string, float>> shapeBlends;
        };

        [[nodiscard]] OwnedVisualSnapshot CaptureOwnedVisualSnapshot(RE::TESNPC* a_npc)
        {
            OwnedVisualSnapshot snapshot;
            snapshot.thin = a_npc->morphWeight.thin;
            snapshot.muscular = a_npc->morphWeight.muscular;
            snapshot.fat = a_npc->morphWeight.fat;
            snapshot.skinToneIndex = a_npc->skinToneIndex;
            snapshot.pronoun = a_npc->pronoun.underlying();
            snapshot.teeth = SafeText(a_npc->teeth.c_str());
            snapshot.jewelryColor = SafeText(a_npc->jewelryColor.c_str());
            snapshot.eyeColor = SafeText(a_npc->eyeColor.c_str());
            snapshot.hairColor = SafeText(a_npc->hairColor.c_str());
            snapshot.facialColor = SafeText(a_npc->facialColor.c_str());
            snapshot.eyebrowColor = SafeText(a_npc->eyebrowColor.c_str());

            {
                auto locked = a_npc->headParts.Lock();
                snapshot.headPartFormIDs.reserve((*locked).size());
                for (const auto* part : *locked) {
                    snapshot.headPartFormIDs.push_back(part ? part->GetFormID() : 0);
                }
            }

            snapshot.hasBodyMorphRegions = a_npc->unk3D8 != nullptr;
            if (a_npc->unk3D8) {
                snapshot.bodyMorphRegions.assign(a_npc->unk3D8->begin(), a_npc->unk3D8->end());
            }

            snapshot.hasBoneValues = a_npc->unk3E0 != nullptr;
            if (a_npc->unk3E0) {
                snapshot.boneValues.reserve(a_npc->unk3E0->size());
                for (const auto& entry : *a_npc->unk3E0) {
                    snapshot.boneValues.emplace_back(entry.key, entry.value);
                }
            }

            snapshot.hasBoneRegions = a_npc->unk3E8 != nullptr;
            if (a_npc->unk3E8) {
                snapshot.boneRegions.reserve(a_npc->unk3E8->size());
                for (const auto& outer : *a_npc->unk3E8) {
                    OwnedBoneRegionSnapshot region;
                    region.regionID = outer.key;
                    region.hasValues = outer.value != nullptr;
                    if (outer.value) {
                        region.values.reserve(outer.value->size());
                        for (const auto& inner : *outer.value) {
                            region.values.emplace_back(SafeText(inner.key.c_str()), inner.value);
                        }
                    }
                    snapshot.boneRegions.push_back(std::move(region));
                }
            }

            snapshot.avms.reserve(a_npc->tintAVMData.size());
            for (const auto& avm : a_npc->tintAVMData) {
                snapshot.avms.push_back({
                    avm.type,
                    SafeText(avm.category.c_str()),
                    SafeText(avm.unk10.name.c_str()),
                    SafeText(avm.unk10.texturePath.c_str()),
                    avm.unk10.color,
                    avm.unk10.intensity
                });
            }

            snapshot.hasShapeBlends = a_npc->shapeBlendData != nullptr;
            if (a_npc->shapeBlendData) {
                snapshot.shapeBlends.reserve(a_npc->shapeBlendData->size());
                for (const auto& entry : *a_npc->shapeBlendData) {
                    snapshot.shapeBlends.emplace_back(SafeText(entry.key.c_str()), entry.value);
                }
            }
            return snapshot;
        }

        [[nodiscard]] bool SameExactVisualValues(
            RE::TESNPC* a_npc, const OwnedVisualSnapshot& a_snapshot)
        {
            if (!a_npc ||
                a_npc->morphWeight.thin != a_snapshot.thin ||
                a_npc->morphWeight.muscular != a_snapshot.muscular ||
                a_npc->morphWeight.fat != a_snapshot.fat ||
                a_npc->skinToneIndex != a_snapshot.skinToneIndex ||
                a_npc->pronoun.underlying() != a_snapshot.pronoun ||
                std::string_view{ SafeText(a_npc->teeth.c_str()) } != a_snapshot.teeth ||
                std::string_view{ SafeText(a_npc->jewelryColor.c_str()) } != a_snapshot.jewelryColor ||
                std::string_view{ SafeText(a_npc->eyeColor.c_str()) } != a_snapshot.eyeColor ||
                std::string_view{ SafeText(a_npc->hairColor.c_str()) } != a_snapshot.hairColor ||
                std::string_view{ SafeText(a_npc->facialColor.c_str()) } != a_snapshot.facialColor ||
                std::string_view{ SafeText(a_npc->eyebrowColor.c_str()) } != a_snapshot.eyebrowColor) {
                return false;
            }

            {
                auto locked = a_npc->headParts.Lock();
                if ((*locked).size() != a_snapshot.headPartFormIDs.size()) {
                    return false;
                }
                for (std::uint32_t i = 0; i < (*locked).size(); ++i) {
                    const auto* part = (*locked)[i];
                    if ((part ? part->GetFormID() : 0) != a_snapshot.headPartFormIDs[i]) {
                        return false;
                    }
                }
            }

            if ((a_npc->unk3D8 != nullptr) != a_snapshot.hasBodyMorphRegions) {
                return false;
            }
            if (a_npc->unk3D8) {
                if (a_npc->unk3D8->size() != a_snapshot.bodyMorphRegions.size()) {
                    return false;
                }
                for (std::uint32_t i = 0; i < a_npc->unk3D8->size(); ++i) {
                    if ((*a_npc->unk3D8)[i] != a_snapshot.bodyMorphRegions[i]) {
                        return false;
                    }
                }
            }

            if ((a_npc->unk3E0 != nullptr) != a_snapshot.hasBoneValues) {
                return false;
            }
            if (a_npc->unk3E0) {
                if (a_npc->unk3E0->size() != a_snapshot.boneValues.size()) {
                    return false;
                }
                for (const auto& [key, value] : a_snapshot.boneValues) {
                    const auto other = a_npc->unk3E0->find(key);
                    if (other == a_npc->unk3E0->end() || other->value != value) {
                        return false;
                    }
                }
            }

            if ((a_npc->unk3E8 != nullptr) != a_snapshot.hasBoneRegions) {
                return false;
            }
            if (a_npc->unk3E8) {
                if (a_npc->unk3E8->size() != a_snapshot.boneRegions.size()) {
                    return false;
                }
                for (const auto& region : a_snapshot.boneRegions) {
                    const auto otherOuter = a_npc->unk3E8->find(region.regionID);
                    if (otherOuter == a_npc->unk3E8->end() ||
                        (otherOuter->value != nullptr) != region.hasValues) {
                        return false;
                    }
                    if (!region.hasValues) {
                        continue;
                    }
                    if (otherOuter->value->size() != region.values.size()) {
                        return false;
                    }
                    for (const auto& [key, value] : region.values) {
                        const bool matched = std::ranges::any_of(
                            *otherOuter->value, [&](const auto& a_entry) {
                                return std::string_view{ SafeText(a_entry.key.c_str()) } == key &&
                                       a_entry.value == value;
                            });
                        if (!matched) {
                            return false;
                        }
                    }
                }
            }

            if (a_npc->tintAVMData.size() != a_snapshot.avms.size()) {
                return false;
            }
            for (const auto& expected : a_snapshot.avms) {
                const auto other = std::ranges::find_if(
                    a_npc->tintAVMData, [&](const RE::AVMData& a_entry) {
                        return std::string_view{ SafeText(a_entry.category.c_str()) } ==
                               expected.category;
                    });
                if (other == a_npc->tintAVMData.end() ||
                    other->type != expected.type ||
                    std::string_view{ SafeText(other->unk10.name.c_str()) } != expected.name ||
                    std::string_view{ SafeText(other->unk10.texturePath.c_str()) } != expected.texturePath ||
                    other->unk10.color != expected.color ||
                    other->unk10.intensity != expected.intensity) {
                    return false;
                }
            }

            if ((a_npc->shapeBlendData != nullptr) != a_snapshot.hasShapeBlends) {
                return false;
            }
            if (a_npc->shapeBlendData) {
                if (a_npc->shapeBlendData->size() != a_snapshot.shapeBlends.size()) {
                    return false;
                }
                for (const auto& [key, value] : a_snapshot.shapeBlends) {
                    const bool matched = std::ranges::any_of(
                        *a_npc->shapeBlendData, [&](const auto& a_entry) {
                            return std::string_view{ SafeText(a_entry.key.c_str()) } == key &&
                                   a_entry.value == value;
                        });
                    if (!matched) {
                        return false;
                    }
                }
            }
            return true;
        }

        struct PersistentAppliedState
        {
            RE::TESFormID        baseID{ 0 };
            std::uint64_t        sequence{ 0 };
            OwnedVisualSnapshot originalVisual;
            NonVisualSnapshot   originalNonVisual;
            RE::TESNPC*          originalFaceNPC{ nullptr };
            std::uint32_t        originalActorFlags{ 0 };
        };

        std::unordered_map<RE::TESFormID, PersistentAppliedState> g_persistentAppliedRefs;

        [[nodiscard]] bool ForgetPersistentState(const RE::TESFormID a_refID)
        {
            const std::scoped_lock lock{ g_eventMutex };
            return g_persistentAppliedRefs.erase(a_refID) != 0;
        }

        [[nodiscard]] std::size_t PersistentAppliedCount()
        {
            const std::scoped_lock lock{ g_eventMutex };
            return g_persistentAppliedRefs.size();
        }

        using ResolveFaceDbEntry = bool (*)(
            std::uint32_t, const RE::BSFixedString*, const RE::BSFixedString*,
            RE::AVMData::Entry*);

        struct MaterializedAvmLayer
        {
            RE::AVMData data;
            std::string modulationValue;
        };

        [[nodiscard]] bool MaterializeAvmLayers(
            const LineSink& a_out,
            const AppearancePreset& a_preset,
            ResolveFaceDbEntry a_resolveEntry,
            std::vector<MaterializedAvmLayer>& a_outLayers)
        {
            a_outLayers.clear();
            a_outLayers.reserve(a_preset.postBlendLayers.size());
            for (const auto& layer : a_preset.postBlendLayers) {
                MaterializedAvmLayer materialized;
                materialized.data.category = RE::BSFixedString{ layer.name };
                materialized.modulationValue = layer.modulationValue;
                const RE::BSFixedString value{ layer.value };

                std::uint32_t matchedStore = 0;
                for (std::uint32_t store = 1; store <= 2; ++store) {
                    RE::AVMData::Entry candidate;
                    if (a_resolveEntry(store, &materialized.data.category,
                                       &value, &candidate)) {
                        if (matchedStore != 0) {
                            a_out(std::format(
                                "donorvisual: AVM layer '{}' value '{}' resolves in multiple primary FaceDB stores",
                                layer.name, layer.value));
                            return false;
                        }
                        matchedStore = store;
                        materialized.data.unk10 = candidate;
                    }
                }
                if (matchedStore == 0) {
                    a_out(std::format(
                        "donorvisual: AVM layer '{}' value '{}' did not materialize from primary FaceDB stores",
                        layer.name, layer.value));
                    return false;
                }
                materialized.data.type = static_cast<RE::AVMData::Type>(matchedStore);

                if (!layer.modulationValue.empty()) {
                    const RE::BSFixedString modulationValue{ layer.modulationValue };
                    RE::AVMData::Entry modulation;
                    if (!a_resolveEntry(3, &materialized.data.category,
                                        &modulationValue, &modulation)) {
                        a_out(std::format(
                            "donorvisual: AVM layer '{}' modulation '{}' did not materialize from FaceDB store 3",
                            layer.name, layer.modulationValue));
                        return false;
                    }
                    materialized.data.unk10.color = modulation.color;
                }

                materialized.data.unk10.intensity = static_cast<std::uint32_t>(
                    std::floor(std::clamp(layer.intensity, 0.0, 1.0) * 64.0));
                a_outLayers.push_back(std::move(materialized));
            }
            return true;
        }

        using SetShapeBlend = void (*)(
            RE::TESNPC*, const RE::BSFixedStringCS*, float);
        using SetBodyMorph = void (*)(RE::TESNPC*, std::uint32_t, float);
        using SetFacialBone = void (*)(RE::TESNPC*, std::uint32_t, float);
        using EnsureFacialBoneGroup = void (*)(
            RE::TESNPC*, std::uint32_t, const RE::BSFixedStringCS*);
        using RemoveHeadPart = void (*)(RE::TESNPC*, RE::BGSHeadPart*, bool);
        using ChangeHeadPart = void (*)(RE::TESNPC*, RE::BGSHeadPart*);
        using SetAvmData = void (*)(RE::TESNPC*, const RE::AVMData*);
        using RemoveAvmData = void (*)(RE::TESNPC*, const RE::BSFixedString*);
        using OwnedVisualCopy = void (*)(RE::TESNPC*, RE::TESNPC*, bool);
        using RefreshActorAppearance = void (*)(RE::Actor*, bool, std::uint32_t, bool);
        using DestroyNpc = RE::TESNPC* (*)(RE::TESNPC*, std::uint32_t);

        struct TargetHoldState
        {
            std::uint64_t    serial{ 0 };
            RE::TESFormID    targetFormID{ 0 };
            RE::TESFormID    actorRefID{ 0 };
            RE::TESFormID    backupFormID{ 0 };
            RE::TESFormID    presetFormID{ 0 };
            RE::TESNPC*      target{ nullptr };
            RE::Actor*       actor{ nullptr };
            RE::TESNPC*      backup{ nullptr };
            RE::TESNPC*      originalFaceNPC{ nullptr };
            std::optional<OwnedVisualSnapshot> originalVisual;
            NonVisualSnapshot originalNonVisual;
            std::uint32_t    originalActorFlags{ 0 };
            bool             baseRestoredBeforeWait{ false };
            bool             donorsDestroyedBeforeWait{ false };
        };

        std::mutex                       g_targetHoldMutex;
        std::unique_ptr<TargetHoldState> g_targetHold;
        std::atomic<std::uint64_t>       g_nextTargetHoldSerial{ 0 };
        std::atomic<std::uint64_t>       g_targetHoldRollbackDueSerial{ 0 };
        std::atomic<bool>                g_targetHoldRollbackDeferralLogged{ false };

        void PopulatePresetMorphs(
            RE::TESNPC* a_donor,
            const AppearancePreset& a_preset,
            SetShapeBlend a_setShape,
            SetBodyMorph a_setBody,
            SetFacialBone a_setBone,
            EnsureFacialBoneGroup a_ensureBoneGroup)
        {
            a_donor->morphWeight.thin = static_cast<float>(a_preset.morphWeights.x);
            a_donor->morphWeight.muscular = static_cast<float>(a_preset.morphWeights.y);
            a_donor->morphWeight.fat = static_cast<float>(a_preset.morphWeights.z);
            for (std::size_t i = 0; i < a_preset.bodyMorphRegionValues.size(); ++i) {
                a_setBody(a_donor, static_cast<std::uint32_t>(i),
                          static_cast<float>(a_preset.bodyMorphRegionValues[i]));
            }
            for (const auto& morph : a_preset.facialMorphSliders) {
                const RE::BSFixedStringCS key{ morph.name.c_str() };
                a_setShape(a_donor, &key, static_cast<float>(morph.value));
            }
            for (const auto& region : a_preset.facialBoneRegions) {
                for (const auto& slider : region.sliders) {
                    if (slider.id != 0) {
                        a_setBone(a_donor, slider.id, static_cast<float>(slider.value));
                        continue;
                    }
                    const RE::BSFixedStringCS key{ slider.groupName.c_str() };
                    a_ensureBoneGroup(a_donor, region.regionID, &key);
                    if (!a_donor->unk3E8) {
                        continue;
                    }
                    const auto outer = a_donor->unk3E8->find(region.regionID);
                    if (outer == a_donor->unk3E8->end() || !outer->value) {
                        continue;
                    }
                    for (auto& entry : *outer->value) {
                        if (::_stricmp(SafeText(entry.key.c_str()),
                                       slider.groupName.c_str()) == 0) {
                            entry.value = static_cast<float>(slider.value);
                            break;
                        }
                    }
                }
            }
        }

        void PopulatePresetVisuals(
            RE::TESNPC* a_donor,
            const AppearancePreset& a_preset,
            const ResolvedAppearanceDependencies& a_resolved,
            const std::vector<MaterializedAvmLayer>& a_expectedAvms,
            RemoveHeadPart a_removeHeadPart,
            ChangeHeadPart a_changeHeadPart,
            SetAvmData a_setAvmData,
            RemoveAvmData a_removeAvmData)
        {
            std::vector<RE::BGSHeadPart*> donorHeadParts;
            {
                auto headParts = a_donor->headParts.Lock();
                donorHeadParts.assign((*headParts).begin(), (*headParts).end());
            }
            for (std::size_t i = 1; i < a_resolved.uniqueHeadParts.size(); ++i) {
                if (a_resolved.uniqueHeadParts[i]) {
                    continue;
                }
                for (auto* part : donorHeadParts) {
                    if (part && static_cast<std::size_t>(part->type.get()) == i) {
                        a_removeHeadPart(a_donor, part, false);
                    }
                }
            }
            for (auto* part : a_resolved.uniqueHeadParts) {
                if (!part) {
                    continue;
                }
                bool present = false;
                {
                    auto headParts = a_donor->headParts.Lock();
                    present = std::ranges::find(*headParts, part) != (*headParts).end();
                }
                if (!present) {
                    a_changeHeadPart(a_donor, part);
                }
            }
            for (auto* part : a_resolved.miscHeadParts) {
                bool present = false;
                {
                    auto headParts = a_donor->headParts.Lock();
                    present = std::ranges::find(*headParts, part) != (*headParts).end();
                }
                if (!present) {
                    a_changeHeadPart(a_donor, part);
                }
            }

            a_donor->skinToneIndex = static_cast<std::uint8_t>(a_preset.skinTone);
            a_donor->teeth = a_preset.teethCustomization;
            a_donor->jewelryColor = a_preset.jewelryColor;
            a_donor->eyeColor = a_preset.eyeColor;
            a_donor->hairColor = a_preset.hairColor;
            a_donor->facialColor = a_preset.facialHairColor;
            a_donor->eyebrowColor = a_preset.browHairColor;

            std::vector<RE::BSFixedString> existingAvmCategories;
            existingAvmCategories.reserve(a_donor->tintAVMData.size());
            for (const auto& avm : a_donor->tintAVMData) {
                existingAvmCategories.push_back(avm.category);
            }
            for (const auto& category : existingAvmCategories) {
                const bool desired = std::ranges::any_of(
                    a_expectedAvms, [&](const MaterializedAvmLayer& a_expected) {
                        return ::_stricmp(SafeText(category.c_str()),
                                          SafeText(a_expected.data.category.c_str())) == 0;
                    });
                if (!desired) {
                    a_removeAvmData(a_donor, &category);
                }
            }
            for (const auto& expected : a_expectedAvms) {
                a_setAvmData(a_donor, &expected.data);
            }
        }

        [[nodiscard]] bool ValidateDonorVisualPopulation(
            const LineSink& a_out,
            RE::TESNPC* a_donor,
            const AppearancePreset& a_preset,
            const ResolvedAppearanceDependencies& a_resolved,
            const std::vector<MaterializedAvmLayer>& a_expectedAvms)
        {
            std::size_t checked = 0;
            std::size_t failed = 0;
            std::size_t headPartFailed = 0;
            std::size_t colorFailed = 0;
            std::size_t avmFailed = 0;
            const auto check = [&](const bool a_condition, const std::string& a_label,
                                   std::size_t& a_categoryFailures) {
                ++checked;
                if (!a_condition) {
                    ++failed;
                    ++a_categoryFailures;
                    if (failed <= 12) {
                        a_out(std::format("donorvisual mismatch: {}", a_label));
                    }
                }
            };

            std::vector<RE::BGSHeadPart*> donorHeadParts;
            {
                auto headParts = a_donor->headParts.Lock();
                donorHeadParts.assign((*headParts).begin(), (*headParts).end());
            }
            for (std::size_t i = 0; i < a_resolved.uniqueHeadParts.size(); ++i) {
                const auto expected = a_resolved.uniqueHeadParts[i];
                const auto actual = std::ranges::find_if(
                    donorHeadParts, [&](const RE::BGSHeadPart* a_part) {
                        return a_part && static_cast<std::size_t>(a_part->type.get()) == i;
                    });
                if (i == static_cast<std::size_t>(RE::BGSHeadPart::HeadPartType::kMisc)) {
                    if (expected) {
                        check(std::ranges::find(donorHeadParts, expected) != donorHeadParts.end(),
                              std::format("UniqueHeadPartsA[{}]", i), headPartFailed);
                    }
                } else {
                    check(expected ? actual != donorHeadParts.end() && *actual == expected :
                                     actual == donorHeadParts.end(),
                          std::format("UniqueHeadPartsA[{}]", i), headPartFailed);
                }
            }
            for (std::size_t i = 0; i < a_resolved.miscHeadParts.size(); ++i) {
                check(std::ranges::find(donorHeadParts, a_resolved.miscHeadParts[i]) !=
                          donorHeadParts.end(),
                      std::format("MiscHeadPartsA[{}]", i), headPartFailed);
            }

            check(a_donor->skinToneIndex == static_cast<std::uint8_t>(a_preset.skinTone),
                  "SkinTone", colorFailed);
            check(::_stricmp(SafeText(a_donor->teeth.c_str()),
                             a_preset.teethCustomization.c_str()) == 0,
                  "TeethCustomization", colorFailed);
            check(::_stricmp(SafeText(a_donor->jewelryColor.c_str()),
                             a_preset.jewelryColor.c_str()) == 0,
                  "JewelryColor", colorFailed);
            check(::_stricmp(SafeText(a_donor->eyeColor.c_str()),
                             a_preset.eyeColor.c_str()) == 0,
                  "EyeColor", colorFailed);
            check(::_stricmp(SafeText(a_donor->hairColor.c_str()),
                             a_preset.hairColor.c_str()) == 0,
                  "HairColor", colorFailed);
            check(::_stricmp(SafeText(a_donor->facialColor.c_str()),
                             a_preset.facialHairColor.c_str()) == 0,
                  "FacialHairColor", colorFailed);
            check(::_stricmp(SafeText(a_donor->eyebrowColor.c_str()),
                             a_preset.browHairColor.c_str()) == 0,
                  "BrowHairColor", colorFailed);

            check(a_donor->tintAVMData.size() == a_expectedAvms.size(),
                  "PostBlendFaceCustomization.LayersA size", avmFailed);
            for (const auto& expected : a_expectedAvms) {
                const auto actual = std::ranges::find_if(
                    a_donor->tintAVMData, [&](const RE::AVMData& a_avm) {
                        return ::_stricmp(SafeText(a_avm.category.c_str()),
                                          SafeText(expected.data.category.c_str())) == 0;
                    });
                const auto prefix = std::format("AVM['{}']", expected.data.category.c_str());
                check(actual != a_donor->tintAVMData.end(), prefix + " present", avmFailed);
                if (actual == a_donor->tintAVMData.end()) {
                    continue;
                }
                check(actual->type == expected.data.type, prefix + " type", avmFailed);
                check(::_stricmp(SafeText(actual->unk10.name.c_str()),
                                 SafeText(expected.data.unk10.name.c_str())) == 0,
                      prefix + " value", avmFailed);
                check(::_stricmp(SafeText(actual->unk10.texturePath.c_str()),
                                 SafeText(expected.data.unk10.texturePath.c_str())) == 0,
                      prefix + " texture", avmFailed);
                check(actual->unk10.color == expected.data.unk10.color,
                      prefix + " color", avmFailed);
                check(actual->unk10.intensity == expected.data.unk10.intensity,
                      prefix + " intensity", avmFailed);
            }
            a_out(std::format(
                "donorvisual: validated={} failed={} headPartFailed={} colorFailed={} avmFailed={}",
                checked, failed, headPartFailed, colorFailed, avmFailed));
            return failed == 0;
        }

        [[nodiscard]] bool ValidateDonorMorphPopulation(
            const LineSink& a_out,
            RE::TESNPC* a_donor,
            const AppearancePreset& a_preset)
        {
            std::size_t checked = 0;
            std::size_t failed = 0;
            std::size_t shapeFailed = 0;
            std::size_t boneIDFailed = 0;
            std::size_t boneGroupFailed = 0;
            const auto check = [&](const bool a_condition, const std::string& a_label) {
                ++checked;
                if (!a_condition) {
                    ++failed;
                    if (failed <= 10) {
                        a_out(std::format("donormorph mismatch: {}", a_label));
                    }
                }
            };
            check(a_donor->morphWeight.thin == static_cast<float>(a_preset.morphWeights.x),
                  "MorphWeight.x/thin");
            check(a_donor->morphWeight.muscular == static_cast<float>(a_preset.morphWeights.y),
                  "MorphWeight.y/muscular");
            check(a_donor->morphWeight.fat == static_cast<float>(a_preset.morphWeights.z),
                  "MorphWeight.z/fat");

            check(a_donor->unk3D8 &&
                      a_donor->unk3D8->size() == a_preset.bodyMorphRegionValues.size(),
                  "BodyMorphRegionValuesA size");
            if (a_donor->unk3D8 &&
                a_donor->unk3D8->size() == a_preset.bodyMorphRegionValues.size()) {
                for (std::size_t i = 0; i < a_preset.bodyMorphRegionValues.size(); ++i) {
                    check((*a_donor->unk3D8)[static_cast<std::uint32_t>(i)] ==
                              static_cast<float>(a_preset.bodyMorphRegionValues[i]),
                          std::format("BodyMorphRegionValuesA[{}]", i));
                }
            }

            for (const auto& morph : a_preset.facialMorphSliders) {
                float actual = 0.0F;
                bool found = false;
                if (a_donor->shapeBlendData) {
                    for (const auto& entry : *a_donor->shapeBlendData) {
                        if (::_stricmp(SafeText(entry.key.c_str()), morph.name.c_str()) == 0) {
                            actual = entry.value;
                            found = true;
                            break;
                        }
                    }
                }
                const auto expected = static_cast<float>(morph.value);
                const bool valid =
                    (expected == 0.0F && (!found || actual == 0.0F)) ||
                    (found && actual == expected);
                shapeFailed += !valid;
                check(valid,
                      std::format("FacialMorphSliderDataA['{}'] expected={:.9g} actual={:.9g} found={}",
                                  morph.name, expected, actual, found));
            }

            for (const auto& region : a_preset.facialBoneRegions) {
                for (const auto& slider : region.sliders) {
                    float actual = 0.0F;
                    bool found = false;
                    if (slider.id != 0 && a_donor->unk3E0) {
                        const auto it = a_donor->unk3E0->find(slider.id);
                        if (it != a_donor->unk3E0->end()) {
                            actual = it->value;
                            found = true;
                        }
                    } else if (slider.id == 0 && !slider.groupName.empty() &&
                               a_donor->unk3E8) {
                        const auto outer = a_donor->unk3E8->find(region.regionID);
                        if (outer != a_donor->unk3E8->end() && outer->value) {
                            for (const auto& entry : *outer->value) {
                                if (::_stricmp(SafeText(entry.key.c_str()),
                                               slider.groupName.c_str()) == 0) {
                                    actual = entry.value;
                                    found = true;
                                    break;
                                }
                            }
                        }
                    }
                    const auto expected = static_cast<float>(slider.value);
                    const bool valid =
                        (expected == 0.0F && (!found || actual == 0.0F)) ||
                        (found && actual == expected);
                    if (!valid) {
                        slider.id != 0 ? ++boneIDFailed : ++boneGroupFailed;
                    }
                    check(valid,
                          std::format("FacialBoneRegionDataA[{}].{} expected={:.9g} actual={:.9g} found={}",
                                      region.regionID,
                                      slider.id != 0 ? std::to_string(slider.id) : slider.groupName,
                                      expected, actual, found));
                }
            }
            a_out(std::format(
                "donormorph: validated={} failed={} shapeFailed={} boneIDFailed={} boneGroupFailed={}",
                checked, failed, shapeFailed, boneIDFailed, boneGroupFailed));
            if (shapeFailed != 0 && a_donor->shapeBlendData) {
                std::size_t emitted = 0;
                for (const auto& entry : *a_donor->shapeBlendData) {
                    if (emitted++ >= 16) {
                        break;
                    }
                    a_out(std::format("donormorph shape sample: '{}'={:.9g}",
                                      SafeText(entry.key.c_str()), entry.value));
                }
            }
            return failed == 0;
        }

        void ReportSnapshot(const LineSink& a_out, std::string_view a_label,
                            const NonVisualSnapshot& a_snap)
        {
            a_out(std::format(
                "{} editorID='{}' name='{}' flagsNoSex=0x{:08X} level={}/{}..{} disposition={} templateFlags=0x{:04X} pronoun={}",
                a_label, a_snap.editorID, a_snap.name, a_snap.actorFlagsExceptSex,
                a_snap.level, a_snap.calcLevelMin, a_snap.calcLevelMax,
                a_snap.baseDisposition, a_snap.templateUseFlags, a_snap.pronoun));
            a_out(std::format(
                "{} race={} originalRace={} class={} voice={} combat={} outfits={}/{} crimeFaction={} factions={}@{} inventory={}@{}",
                a_label,
                static_cast<void*>(a_snap.race), static_cast<void*>(a_snap.originalRace),
                static_cast<void*>(a_snap.npcClass), static_cast<void*>(a_snap.voiceType),
                static_cast<void*>(a_snap.combatStyle), static_cast<void*>(a_snap.defaultOutfit),
                static_cast<void*>(a_snap.sleepOutfit), static_cast<void*>(a_snap.crimeFaction),
                a_snap.factionCount, a_snap.factionData, a_snap.inventoryCount, a_snap.inventoryData));
        }

        [[nodiscard]] bool HasExpectedGate(const std::uintptr_t a_address)
        {
            if (!Util::IsReadableRange(a_address, kActorCopyAppearanceGate.size())) {
                return false;
            }
            return std::memcmp(reinterpret_cast<const void*>(a_address),
                               kActorCopyAppearanceGate.data(),
                               kActorCopyAppearanceGate.size()) == 0;
        }

        template <std::size_t N>
        [[nodiscard]] bool HasExpectedBytes(const std::uintptr_t a_address,
                                            const std::array<std::uint8_t, N>& a_expected)
        {
            return Util::IsReadableRange(a_address, a_expected.size()) &&
                   std::memcmp(reinterpret_cast<const void*>(a_address),
                               a_expected.data(), a_expected.size()) == 0;
        }

        void NotifyBaseAppearanceChanged(RE::TESNPC* a_npc, const std::uint32_t a_flag)
        {
            auto* vtable = *reinterpret_cast<std::uintptr_t**>(a_npc);
            using Notify = void (*)(RE::TESNPC*, std::uint32_t);
            reinterpret_cast<Notify>(vtable[0x17])(a_npc, a_flag);
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

        [[nodiscard]] bool TargetHoldActive()
        {
            const std::scoped_lock lock{ g_targetHoldMutex };
            return g_targetHold != nullptr;
        }

        struct PersistentRemovalSummary
        {
            std::size_t refreshed{ 0 };
            std::size_t retiredUnloaded{ 0 };
            std::size_t failed{ 0 };
        };

        [[nodiscard]] PersistentRemovalSummary RemovePersistentAppearances(
            const LineSink& a_out)
        {
            std::vector<std::pair<RE::TESFormID, PersistentAppliedState>> applied;
            {
                const std::scoped_lock lock{ g_eventMutex };
                applied.reserve(g_persistentAppliedRefs.size());
                for (const auto& entry : g_persistentAppliedRefs) {
                    applied.push_back(entry);
                }
            }

            PersistentRemovalSummary summary;
            if (applied.empty()) {
                return summary;
            }

            const auto refreshAddress =
                REL::Relocation<std::uintptr_t>{ kActorAppearanceRefreshID }.address();
            if (!HasExpectedBytes(refreshAddress, kActorAppearanceRefreshGate)) {
                summary.failed = applied.size();
                a_out("scene persistent: removal refresh contract mismatch; FAIL CLOSED");
                return summary;
            }
            const auto refresh = reinterpret_cast<RefreshActorAppearance>(refreshAddress);

            for (const auto& [refID, state] : applied) {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(refID);
                auto* target = RE::TESForm::LookupByID<RE::TESNPC>(state.baseID);
                const bool baseOriginal = target &&
                    SameExactVisualValues(target, state.originalVisual) &&
                    target->faceNPC == state.originalFaceNPC &&
                    state.originalNonVisual == Snapshot(target);
                if (!baseOriginal) {
                    ++summary.failed;
                    a_out(std::format(
                        "scene persistent: removal REFUSED ref=0x{:08X} base=0x{:08X}; original base snapshot no longer matches",
                        refID, state.baseID));
                    continue;
                }

                if (!actor || actor->GetNPC() != target || !HasLoaded3D(actor)) {
                    static_cast<void>(ForgetPersistentState(refID));
                    ++summary.retiredUnloaded;
                    a_out(std::format(
                        "scene persistent: retired unloaded/stale generation ref=0x{:08X} base=0x{:08X}; base remained exactly original",
                        refID, state.baseID));
                    continue;
                }

                NotifyBaseAppearanceChanged(target, 0x800);
                NotifyBaseAppearanceChanged(target, 0x4000);
                SuppressNextSceneSet3d(refID);
                refresh(actor, false, 0x28, false);
                const auto afterRefreshRaw = Snapshot(target);
                const auto refreshDirtyMask =
                    state.originalNonVisual.actorFlagsExceptSex ^
                    afterRefreshRaw.actorFlagsExceptSex;
                const bool refreshNonVisualExpected =
                    (refreshDirtyMask & ~kAppearanceRefreshDirtyActorFlag) == 0 &&
                    SameNonVisualIgnoringRefreshDirtyFlag(
                        state.originalNonVisual, afterRefreshRaw);
                target->actorData.actorBaseFlags =
                    static_cast<RE::ACTOR_BASE_DATA::Flag>(state.originalActorFlags);
                const bool finalExact =
                    SameExactVisualValues(target, state.originalVisual) &&
                    target->faceNPC == state.originalFaceNPC &&
                    state.originalNonVisual == Snapshot(target);
                if (!refreshNonVisualExpected || !finalExact) {
                    ++summary.failed;
                    a_out(std::format(
                        "scene persistent: removal FAILED ref=0x{:08X} base=0x{:08X} refreshDirtyMask=0x{:08X} refreshNonVisualExpected={} finalExact={}; stop without saving",
                        refID, state.baseID, refreshDirtyMask,
                        refreshNonVisualExpected, finalExact));
                    continue;
                }

                static_cast<void>(ForgetPersistentState(refID));
                g_scenePersistentRemovalCount.fetch_add(1, std::memory_order_relaxed);
                ++summary.refreshed;
                a_out(std::format(
                    "scene persistent: REMOVE PASS ref=0x{:08X} base=0x{:08X} sequence={} refreshDirtyMask=0x{:08X}; final base and rendered actor are original",
                    refID, state.baseID, state.sequence, refreshDirtyMask));
            }
            return summary;
        }

        enum class TargetHoldFinish
        {
            kNoActiveHold,
            kRestored,
            kFailed
        };

        TargetHoldFinish FinishTargetHold(const std::string_view a_reason)
        {
            std::unique_ptr<TargetHoldState> state;
            {
                const std::scoped_lock lock{ g_targetHoldMutex };
                if (!g_targetHold) {
                    return TargetHoldFinish::kNoActiveHold;
                }
                state = std::move(g_targetHold);
            }

            const bool usesOwnedSnapshot = state->originalVisual.has_value();
            const auto ownedCopyAddress = kNpcOwnedVisualCopyOffset.address();
            const auto refreshAddress =
                REL::Relocation<std::uintptr_t>{ kActorAppearanceRefreshID }.address();
            const auto destructorAddress =
                REL::Relocation<std::uintptr_t>{ kNpcScalarDeletingDestructorID }.address();
            if (!HasExpectedBytes(refreshAddress, kActorAppearanceRefreshGate) ||
                (!usesOwnedSnapshot &&
                 (!HasExpectedBytes(ownedCopyAddress, kNpcOwnedVisualCopyGate) ||
                  !HasExpectedBytes(destructorAddress, kNpcDestructorGate)))) {
                REX::CRITICAL(
                    "[NpcAppearance] targethold RESTORE FAILED reason={} because a runtime contract changed; stop without saving",
                    a_reason);
                return TargetHoldFinish::kFailed;
            }

            auto* target = RE::TESForm::LookupByID<RE::TESNPC>(state->targetFormID);
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(state->actorRefID);
            auto* backup = RE::TESForm::LookupByID<RE::TESNPC>(state->backupFormID);
            const bool identitiesValid = target == state->target && actor == state->actor &&
                actor && actor->GetNPC() == target &&
                (usesOwnedSnapshot ? backup == nullptr && state->backup == nullptr :
                                     backup == state->backup);
            const bool bodyCompatible = usesOwnedSnapshot ||
                (target && backup && target->unk3D8 && backup->unk3D8 &&
                 target->unk3D8->size() == backup->unk3D8->size());
            if (!identitiesValid || !bodyCompatible) {
                REX::CRITICAL(
                    "[NpcAppearance] targethold RESTORE FAILED reason={} identitiesValid={} bodyCompatible={} ownedSnapshot={} target={} actor={} backup={}; stop without saving",
                    a_reason, identitiesValid, bodyCompatible, usesOwnedSnapshot,
                    static_cast<void*>(target), static_cast<void*>(actor),
                    static_cast<void*>(backup));
                return TargetHoldFinish::kFailed;
            }

            const auto ownedCopy = reinterpret_cast<OwnedVisualCopy>(ownedCopyAddress);
            const auto restoreVisualOnly = [&]() {
                target->morphWeight = backup->morphWeight;
                for (std::uint32_t i = 0; i < backup->unk3D8->size(); ++i) {
                    (*target->unk3D8)[i] = (*backup->unk3D8)[i];
                }
                target->skinToneIndex = backup->skinToneIndex;
                ownedCopy(target, backup, false);
                target->faceNPC = state->originalFaceNPC;
            };
            const auto matchesOriginal = [&]() {
                const bool visualExact = usesOwnedSnapshot ?
                    SameExactVisualValues(target, *state->originalVisual) :
                    SameExactVisualValues(target, backup);
                return visualExact && target->faceNPC == state->originalFaceNPC &&
                    SameNonVisualIgnoringRefreshDirtyFlag(
                        state->originalNonVisual, Snapshot(target));
            };

            std::uint32_t restoreAttempts = 0;
            if (!state->baseRestoredBeforeWait) {
                if (usesOwnedSnapshot) {
                    REX::CRITICAL(
                        "[NpcAppearance] targethold RESTORE FAILED reason={} because an owned-snapshot hold reached cleanup without a pre-restored base; stop without saving",
                        a_reason);
                    return TargetHoldFinish::kFailed;
                }
                restoreVisualOnly();
                restoreAttempts = 1;
            }
            bool restoreExact = matchesOriginal();
            if (!restoreExact && !usesOwnedSnapshot) {
                restoreVisualOnly();
                ++restoreAttempts;
                restoreExact = matchesOriginal();
            }

            std::uint32_t refreshDirtyMask = 0;
            bool refreshNonVisualExpected = false;
            if (restoreExact) {
                const auto refresh = reinterpret_cast<RefreshActorAppearance>(refreshAddress);
                NotifyBaseAppearanceChanged(target, 0x800);
                NotifyBaseAppearanceChanged(target, 0x4000);
                SuppressNextSceneSet3d(state->actorRefID);
                refresh(actor, false, 0x28, false);
                const auto afterRefreshRaw = Snapshot(target);
                refreshDirtyMask = state->originalNonVisual.actorFlagsExceptSex ^
                    afterRefreshRaw.actorFlagsExceptSex;
                refreshNonVisualExpected =
                    (refreshDirtyMask & ~kAppearanceRefreshDirtyActorFlag) == 0 &&
                    SameNonVisualIgnoringRefreshDirtyFlag(
                        state->originalNonVisual, afterRefreshRaw);
            }
            target->actorData.actorBaseFlags =
                static_cast<RE::ACTOR_BASE_DATA::Flag>(state->originalActorFlags);
            const bool finalNonVisualExact = state->originalNonVisual == Snapshot(target);
            const bool finalVisualExact = restoreExact && matchesOriginal();

            if (backup) {
                const auto destroy = reinterpret_cast<DestroyNpc>(destructorAddress);
                destroy(backup, 1);
            }
            const bool donorsUnregistered =
                RE::TESForm::LookupByID<RE::TESNPC>(state->backupFormID) == nullptr &&
                RE::TESForm::LookupByID<RE::TESNPC>(state->presetFormID) == nullptr;
            const bool passed = restoreExact && refreshNonVisualExpected &&
                finalVisualExact && finalNonVisualExact && donorsUnregistered &&
                (!usesOwnedSnapshot || state->donorsDestroyedBeforeWait);
            if (passed) {
                REX::INFO(
                    "[NpcAppearance] targethold RESTORE PASS reason={} attempts={} basePreRestored={} ownedSnapshot={} donorsDestroyedBeforeWait={} refreshDirtyMask=0x{:08X} donorsUnregistered={} final target is original",
                    a_reason, restoreAttempts, state->baseRestoredBeforeWait,
                    usesOwnedSnapshot, state->donorsDestroyedBeforeWait,
                    refreshDirtyMask, donorsUnregistered);
                return TargetHoldFinish::kRestored;
            }

            REX::CRITICAL(
                "[NpcAppearance] targethold RESTORE FAILED reason={} exact={} attempts={} basePreRestored={} ownedSnapshot={} donorsDestroyedBeforeWait={} refreshDirtyMask=0x{:08X} refreshExpected={} visualExact={} nonVisualExact={} donorsUnregistered={}; stop without saving",
                a_reason, restoreExact, restoreAttempts, state->baseRestoredBeforeWait,
                usesOwnedSnapshot, state->donorsDestroyedBeforeWait, refreshDirtyMask,
                refreshNonVisualExpected, finalVisualExact, finalNonVisualExact,
                donorsUnregistered);
            return TargetHoldFinish::kFailed;
        }

        void ScheduleTargetHoldRollback(const std::uint64_t a_serial)
        {
            std::thread([a_serial] {
                std::this_thread::sleep_for(std::chrono::seconds{ kTargetHoldSeconds });
                g_targetHoldRollbackDueSerial.store(a_serial, std::memory_order_release);
            }).detach();
        }

        void OnNpcAppearanceNativeFrame()
        {
            const auto nativeThreadID = ::GetCurrentThreadId();
            const auto nativeDiagnostics = Util::NativeMainThreadQueue::GetDiagnostics();
            if (!nativeDiagnostics.insideDrain) {
                REX::CRITICAL(
                    "[NpcAppearance] native lifecycle callback rejected tid={} drainOwnerTid={}; no mutation",
                    nativeThreadID, nativeDiagnostics.drainOwnerThreadID);
                return;
            }
            g_sceneNativeThreadID.store(nativeThreadID, std::memory_order_relaxed);
            g_sceneNativeFrameCount.fetch_add(1, std::memory_order_relaxed);

            auto* ui = RE::UI::GetSingleton();
            const bool mainMenuOpen = ui && ui->IsMenuOpen(RE::BSFixedString{ "MainMenu" });
            const bool loadingMenuOpen = ui && ui->IsMenuOpen(RE::BSFixedString{ "LoadingMenu" });
            const bool menusBlockMutation = !ui || mainMenuOpen || loadingMenuOpen;

            auto rollbackSerial =
                g_targetHoldRollbackDueSerial.load(std::memory_order_acquire);
            if (rollbackSerial != 0) {
                if (menusBlockMutation) {
                    if (!g_targetHoldRollbackDeferralLogged.exchange(
                            true, std::memory_order_relaxed)) {
                        REX::WARN("[NpcAppearance] targethold rollback serial={} deferred on native-main-thread tid={} uiAvailable={} mainMenuOpen={} loadingMenuOpen={}",
                                  rollbackSerial, nativeThreadID, ui != nullptr,
                                  mainMenuOpen, loadingMenuOpen);
                    }
                } else if (g_targetHoldRollbackDueSerial.compare_exchange_strong(
                               rollbackSerial, 0, std::memory_order_acq_rel)) {
                    g_targetHoldRollbackDeferralLogged.store(false, std::memory_order_relaxed);
                    std::uint64_t activeSerial = 0;
                    {
                        const std::scoped_lock lock{ g_targetHoldMutex };
                        if (g_targetHold) {
                            activeSerial = g_targetHold->serial;
                        }
                    }
                    if (activeSerial == rollbackSerial) {
                        FinishTargetHold("native-main-thread automatic timer");
                    } else {
                        REX::INFO("[NpcAppearance] ignored stale targethold rollback serial={} activeSerial={} nativeTid={}",
                                  rollbackSerial, activeSerial, nativeThreadID);
                    }
                }
            }

            std::optional<PendingSceneApply> pending;
            {
                const std::scoped_lock lock{ g_eventMutex };
                pending = g_pendingSceneApply;
            }
            if (!pending) {
                return;
            }

            if (menusBlockMutation) {
                bool shouldLog = false;
                {
                    const std::scoped_lock lock{ g_eventMutex };
                    if (g_pendingSceneApply &&
                        g_pendingSceneApply->sequence == pending->sequence) {
                        g_pendingSceneApply->stableNativeFrames = 0;
                        if (!g_pendingSceneApply->loadingDeferralLogged) {
                            g_pendingSceneApply->loadingDeferralLogged = true;
                            shouldLog = true;
                        }
                    }
                }
                if (shouldLog) {
                    g_sceneNativeLoadingDeferralCount.fetch_add(1, std::memory_order_relaxed);
                    REX::INFO("[NpcAppearance] native-main-thread handoff sequence={} deferred; uiAvailable={} mainMenuOpen={} loadingMenuOpen={} eventTid={} nativeTid={}",
                              pending->sequence, ui != nullptr, mainMenuOpen,
                              loadingMenuOpen, pending->eventThreadID, nativeThreadID);
                }
                return;
            }

            auto* actor = RE::TESForm::LookupByID<RE::Actor>(pending->refID);
            auto* base = actor ? actor->GetNPC() : nullptr;
            bool generationCurrent = false;
            {
                const std::scoped_lock lock{ g_eventMutex };
                generationCurrent = g_sceneTargetRefs.contains(pending->refID);
            }
            const bool liveAndReady = generationCurrent && base &&
                base->GetFormID() == pending->baseID && HasLoaded3D(actor);
            if (!liveAndReady) {
                bool shouldLog = false;
                {
                    const std::scoped_lock lock{ g_eventMutex };
                    if (g_pendingSceneApply &&
                        g_pendingSceneApply->sequence == pending->sequence) {
                        g_pendingSceneApply->stableNativeFrames = 0;
                        if (!g_pendingSceneApply->invalidStateLogged) {
                            g_pendingSceneApply->invalidStateLogged = true;
                            shouldLog = true;
                        }
                    }
                }
                if (shouldLog) {
                    REX::INFO("[NpcAppearance] native-main-thread handoff sequence={} waiting for stable loaded actor generationCurrent={} actor={} base={} expectedBase=0x{:08X} loaded3D={} nativeTid={}",
                              pending->sequence, generationCurrent,
                              static_cast<void*>(actor), static_cast<void*>(base),
                              pending->baseID, HasLoaded3D(actor), nativeThreadID);
                }
                return;
            }

            std::optional<PendingSceneApply> ready;
            {
                const std::scoped_lock lock{ g_eventMutex };
                if (g_pendingSceneApply &&
                    g_pendingSceneApply->sequence == pending->sequence) {
                    g_pendingSceneApply->invalidStateLogged = false;
                    ++g_pendingSceneApply->stableNativeFrames;
                    if (g_pendingSceneApply->stableNativeFrames >= kSceneStableNativeFrames) {
                        ready = *g_pendingSceneApply;
                        g_pendingSceneApply.reset();
                    }
                }
            }
            if (!ready) {
                return;
            }

            g_sceneRanApplyCount.fetch_add(1, std::memory_order_relaxed);
            g_sceneNativeReadyCount.fetch_add(1, std::memory_order_relaxed);
            const bool observeOnly =
                g_sceneDispatchObserveArmed.exchange(false, std::memory_order_acq_rel);
            if (observeOnly) {
                g_sceneNativeObservePassCount.fetch_add(1, std::memory_order_relaxed);
                REX::INFO("[NpcAppearance] native-main-thread dispatch OBSERVE PASS sequence={} ref=0x{:08X} base=0x{:08X} stableFrames={} menusClosed=true loaded3D=true eventTid={} nativeTid={}; no mutation",
                          ready->sequence, ready->refID, ready->baseID,
                          ready->stableNativeFrames, ready->eventThreadID, nativeThreadID);
                return;
            }

            const bool persistentEnabled =
                g_scenePersistentEnabled.load(std::memory_order_acquire);
            const bool autoTrialArmed = persistentEnabled ? false :
                g_sceneAutoTrialArmed.exchange(false, std::memory_order_acq_rel);
            if (!persistentEnabled && !autoTrialArmed) {
                REX::INFO("[NpcAppearance] native-main-thread dispatch READY sequence={} ref=0x{:08X} base=0x{:08X} stableFrames={} eventTid={} nativeTid={}; mutation disabled",
                          ready->sequence, ready->refID, ready->baseID,
                          ready->stableNativeFrames, ready->eventThreadID, nativeThreadID);
                return;
            }

            std::optional<SelectedAssignment> assignment;
            {
                const std::scoped_lock lock{ g_eventMutex };
                const auto found = g_sceneAssignments.find(ready->baseID);
                if (found != g_sceneAssignments.end()) {
                    assignment = found->second;
                }
            }
            if (!assignment) {
                REX::WARN("[NpcAppearance] native-main-thread assignment sequence={} has no validated winner for base=0x{:08X}; persistentEnabled={} autoTrialArmed={}; no mutation",
                          ready->sequence, ready->baseID, persistentEnabled, autoTrialArmed);
                return;
            }
            if (TargetHoldActive()) {
                REX::WARN("[NpcAppearance] native-main-thread assignment sequence={} skipped because a target hold is already active; persistentEnabled={} autoTrialArmed={}",
                          ready->sequence, persistentEnabled, autoTrialArmed);
                return;
            }

            if (persistentEnabled) {
                {
                    const std::scoped_lock lock{ g_eventMutex };
                    if (g_persistentAppliedRefs.contains(ready->refID)) {
                        REX::INFO("[NpcAppearance] native-main-thread persistent assignment sequence={} ref=0x{:08X} already tracked for this generation; no duplicate apply",
                                  ready->sequence, ready->refID);
                        return;
                    }
                }

                PersistentAppliedState applied{
                    .baseID = ready->baseID,
                    .sequence = ready->sequence,
                    .originalVisual = CaptureOwnedVisualSnapshot(base),
                    .originalNonVisual = Snapshot(base),
                    .originalFaceNPC = base->faceNPC,
                    .originalActorFlags = base->actorData.actorBaseFlags.underlying(),
                };
                const std::vector<std::string> trialArgs{
                    "npcapp",
                    "targetpersistent",
                    assignment->target.plugin,
                    std::format("{:08X}", assignment->target.localFormID),
                    std::format("{:08X}", ready->refID),
                    assignment->presetPath.string()
                };
                const LineSink trialOut = [](const std::string& a_text) {
                    REX::INFO("[NpcAppearance] scene persistent: {}", a_text);
                };
                g_scenePersistentAttemptCount.fetch_add(1, std::memory_order_relaxed);
                REX::INFO("[NpcAppearance] native-main-thread persistent assignment START sequence={} ref=0x{:08X} base=0x{:08X} stableFrames={} eventTid={} nativeTid={}",
                          ready->sequence, ready->refID, ready->baseID,
                          ready->stableNativeFrames, ready->eventThreadID, nativeThreadID);
                bool completed = false;
                RunTargetTrial(
                    trialOut, trialArgs, TargetTrialMode::kPersistentLatch, &completed);

                bool retained = false;
                if (completed) {
                    const std::scoped_lock lock{ g_eventMutex };
                    if (g_scenePersistentEnabled.load(std::memory_order_acquire) &&
                        g_sceneTargetRefs.contains(ready->refID)) {
                        g_persistentAppliedRefs.insert_or_assign(
                            ready->refID, std::move(applied));
                        retained = true;
                    }
                }
                if (retained) {
                    g_scenePersistentApplyCount.fetch_add(1, std::memory_order_relaxed);
                }
                REX::INFO("[NpcAppearance] native-main-thread persistent assignment END sequence={} completed={} tracked={} baseOriginalAtRest=true nativeTid={}",
                          ready->sequence, completed, retained, nativeThreadID);
                return;
            }

            g_sceneAutoTrialAttemptCount.fetch_add(1, std::memory_order_relaxed);
            const std::vector<std::string> trialArgs{
                "npcapp",
                "targethold",
                assignment->target.plugin,
                std::format("{:08X}", assignment->target.localFormID),
                std::format("{:08X}", ready->refID),
                assignment->presetPath.string()
            };
            const LineSink trialOut = [](const std::string& a_text) {
                REX::INFO("[NpcAppearance] scene auto: {}", a_text);
            };
            REX::INFO("[NpcAppearance] native-main-thread automatic bounded trial START sequence={} ref=0x{:08X} base=0x{:08X} stableFrames={} eventTid={} nativeTid={}",
                      ready->sequence, ready->refID, ready->baseID,
                      ready->stableNativeFrames, ready->eventThreadID, nativeThreadID);
            RunTargetTrial(trialOut, trialArgs, TargetTrialMode::kHold);
            const bool active = TargetHoldActive();
            if (active) {
                g_sceneAutoTrialApplyCount.fetch_add(1, std::memory_order_relaxed);
            }
            REX::INFO("[NpcAppearance] native-main-thread automatic bounded trial END sequence={} holdActive={} oneShotArmConsumed=true nativeTid={}",
                      ready->sequence, active, nativeThreadID);
        }

        void RequestNpcAppearanceNativeFrame()
        {
            bool pendingScene = false;
            {
                const std::scoped_lock lock{ g_eventMutex };
                pendingScene = g_pendingSceneApply.has_value();
            }
            const bool rollbackDue =
                g_targetHoldRollbackDueSerial.load(std::memory_order_acquire) != 0;
            if (!pendingScene && !rollbackDue) {
                return;
            }

            bool expected = false;
            if (!g_sceneNativeTaskInFlight.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                return;
            }

            const auto result = Util::NativeMainThreadQueue::Post([] {
                struct InFlightReset
                {
                    ~InFlightReset()
                    {
                        g_sceneNativeTaskInFlight.store(false, std::memory_order_release);
                    }
                } reset;
                OnNpcAppearanceNativeFrame();
            }, "NpcAppearance.SceneLifecycle");
            if (result != Util::NativeMainThreadQueue::PostResult::kQueued) {
                g_sceneNativeTaskInFlight.store(false, std::memory_order_release);
                if (!g_sceneNativePostFailureLogged.exchange(true, std::memory_order_acq_rel)) {
                    REX::WARN(
                        "[NpcAppearance] native lifecycle post deferred: {}; pending work remains fail-closed",
                        Util::NativeMainThreadQueue::ToString(result));
                }
            } else {
                g_sceneNativePostFailureLogged.store(false, std::memory_order_release);
            }
        }

        struct LoadedPlugin
        {
            RE::TESFile* file{ nullptr };
            PluginTier tier{ PluginTier::kFull };
            std::uint32_t index{ 0 };
        };

        [[nodiscard]] std::optional<LoadedPlugin> FindLoadedPlugin(const std::string_view a_name)
        {
            const auto* handler = RE::TESDataHandler::GetSingleton();
            if (!handler) {
                return std::nullopt;
            }
            const std::string needle{ a_name };
            const auto find = [&](const auto& a_files, const PluginTier a_tier)
                -> std::optional<LoadedPlugin> {
                std::uint32_t tierIndex = 0;
                for (auto* file : a_files) {
                    if (file && ::_stricmp(file->fileName, needle.c_str()) == 0) {
                        return LoadedPlugin{ file, a_tier,
                            a_tier == PluginTier::kFull ? file->compileIndex : tierIndex };
                    }
                    ++tierIndex;
                }
                return std::nullopt;
            };
            if (auto found = find(handler->compiledFileCollection.files, PluginTier::kFull)) {
                return found;
            }
            if (auto found = find(handler->compiledFileCollection.mediumFiles, PluginTier::kMedium)) {
                return found;
            }
            return find(handler->compiledFileCollection.smallFiles, PluginTier::kSmall);
        }

        [[nodiscard]] std::optional<RE::TESFormID> ResolveRuntimeFormID(const Target& a_target)
        {
            const auto plugin = FindLoadedPlugin(a_target.plugin);
            if (!plugin) {
                return std::nullopt;
            }
            switch (plugin->tier) {
            case PluginTier::kSmall:
                if (!IsLocalFormIDValidForTier(a_target.localFormID, plugin->tier)) {
                    return std::nullopt;
                }
                return 0xFE000000u | (plugin->index << 12) | a_target.localFormID;
            case PluginTier::kMedium:
                if (!IsLocalFormIDValidForTier(a_target.localFormID, plugin->tier)) {
                    return std::nullopt;
                }
                return 0xFD000000u | (plugin->index << 16) | a_target.localFormID;
            case PluginTier::kFull:
                if (!IsLocalFormIDValidForTier(a_target.localFormID, plugin->tier)) {
                    return std::nullopt;
                }
                return (plugin->index << 24) | a_target.localFormID;
            }
            return std::nullopt;
        }

        [[nodiscard]] RE::TESNPC* ResolveEligibleTarget(const LineSink& a_out, const Target& a_target)
        {
            const auto runtimeID = ResolveRuntimeFormID(a_target);
            if (!runtimeID) {
                a_out(std::format("resolve {}: target plugin absent or local FormID exceeds its tier",
                                  a_target.CanonicalKey()));
                return nullptr;
            }
            auto* npc = RE::TESForm::LookupByID<RE::TESNPC>(*runtimeID);
            if (!npc) {
                a_out(std::format("resolve {} -> 0x{:08X}: not found or not TESNPC",
                                  a_target.CanonicalKey(), *runtimeID));
                return nullptr;
            }
            if (!npc->IsUnique()) {
                a_out(std::format("resolve {}: rejected (base 0x{:08X} is not unique)",
                                  a_target.CanonicalKey(), npc->GetFormID()));
                return nullptr;
            }
            auto* humanRace = RE::TESForm::LookupByEditorID<RE::TESRace>(RE::BSFixedString{ "HumanRace" });
            if (!humanRace || npc->GetRace() != humanRace) {
                a_out(std::format("resolve {}: rejected (race={} expected HumanRace={})",
                                  a_target.CanonicalKey(), static_cast<void*>(npc->GetRace()),
                                  static_cast<void*>(humanRace)));
                return nullptr;
            }
            a_out(std::format("resolve {}: base=0x{:08X} editorID='{}' ptr={} unique=1 race=HumanRace",
                              a_target.CanonicalKey(), npc->GetFormID(), SafeText(npc->GetFormEditorID()),
                              static_cast<void*>(npc)));
            return npc;
        }

        void RunStatus(const LineSink& a_out)
        {
            const auto root = DefaultPluginDirectory();
            a_out("OSF Identity diagnostics: disabled-by-default / explicit commands only");
            a_out(std::format("pluginDirectory={}", root.string()));
            a_out(std::format("packagesDirectory={}", DefaultPackagesDirectory().string()));
            a_out("manifestParser=implemented (strict package schema v1, plugin+localFormId targeting, containment, deterministic conflicts)");
            a_out("npcDecoder=implemented (strict CK 1.16.244 JSON contract; golden matrix and adversarial corpus pass)");
            a_out("dependencyResolver=RUNTIME-PROVEN read-only on Sarah (forms/headparts + facial shape/bone + FaceDB color/teeth/AVM catalogs)");
            a_out("runtimeNpcImporter=NO SAFE SEAM FOUND (SavePCFace is a parse-only console wrapper)");
            a_out("ownedEngineConstruction=RUNTIME-PROVEN (100/100 registered-empty Create(false)+destroy+unregister cycles)");
            a_out("copyAppearance=deep-owned containers STATIC-PROVEN, but also copies pronoun; visual-only path pending");
            a_out("copyRefreshWorker=ID 97401 (static byte-contract gate; runtime proof pending)");
            a_out(std::format("lifecycle=TESObjectLoadedEvent ID 64152 register/unregister RUNTIME-PROVEN; sinkRegistered={} events={} matchingLoads={} matchingUnloads={} debounced={} queued={} ran={}",
                              g_eventRegistered.load(std::memory_order_relaxed),
                              g_eventCount.load(std::memory_order_relaxed),
                              g_matchingLoadCount.load(std::memory_order_relaxed),
                              g_matchingUnloadCount.load(std::memory_order_relaxed),
                              g_debouncedLoadCount.load(std::memory_order_relaxed),
                              g_queuedApplyCount.load(std::memory_order_relaxed),
                              g_ranApplyCount.load(std::memory_order_relaxed)));
            a_out(std::format("sceneLifecycle=ReferenceSet3d ID 49237 + ReferenceDetach ID 40306 -> BSService::TaskQueue IDs 883606/100121 with drain-owner ID 923104; startupPackagesPresent={} startupPersistentArmed={} sinkRegistered={} observeArmed={} autoTrialArmed={} persistentEnabled={} persistentTracked={} set3d={} detach={} matchingSet3d={} matchingDetach={} selfRefreshSuppressedSet3d={} selfRefreshSuppressedDetach={} debounced={} published={} nativeReady={} nativeObservePass={} nativeLoadingDeferrals={} nativeFrames={} nativeTid={} nativeInFlight={} autoAttempts={} autoApplies={} persistentAttempts={} persistentApplies={} persistentRemovals={}",
                              g_startupPackagesPresent.load(std::memory_order_relaxed),
                              g_startupPersistentArmed.load(std::memory_order_relaxed),
                              g_sceneRegistered.load(std::memory_order_relaxed),
                              g_sceneDispatchObserveArmed.load(std::memory_order_relaxed),
                              g_sceneAutoTrialArmed.load(std::memory_order_relaxed),
                              g_scenePersistentEnabled.load(std::memory_order_relaxed),
                              PersistentAppliedCount(),
                              g_sceneSet3dCount.load(std::memory_order_relaxed),
                              g_sceneDetachCount.load(std::memory_order_relaxed),
                              g_sceneMatchingSet3dCount.load(std::memory_order_relaxed),
                              g_sceneMatchingDetachCount.load(std::memory_order_relaxed),
                              g_sceneSuppressedSet3dCount.load(std::memory_order_relaxed),
                              g_sceneSuppressedDetachCount.load(std::memory_order_relaxed),
                              g_sceneDebouncedCount.load(std::memory_order_relaxed),
                              g_sceneQueuedApplyCount.load(std::memory_order_relaxed),
                              g_sceneNativeReadyCount.load(std::memory_order_relaxed),
                              g_sceneNativeObservePassCount.load(std::memory_order_relaxed),
                              g_sceneNativeLoadingDeferralCount.load(std::memory_order_relaxed),
                              g_sceneNativeFrameCount.load(std::memory_order_relaxed),
                              g_sceneNativeThreadID.load(std::memory_order_relaxed),
                              g_sceneNativeTaskInFlight.load(std::memory_order_relaxed),
                              g_sceneAutoTrialAttemptCount.load(std::memory_order_relaxed),
                              g_sceneAutoTrialApplyCount.load(std::memory_order_relaxed),
                              g_scenePersistentAttemptCount.load(std::memory_order_relaxed),
                              g_scenePersistentApplyCount.load(std::memory_order_relaxed),
                              g_scenePersistentRemovalCount.load(std::memory_order_relaxed)));
        }

        void RunSelfTest(const LineSink& a_out)
        {
            const std::filesystem::path root{ LR"(C:\OSFIdentity)" };
            const auto manifestPath = root / L"author.sarah" / L"package.json";
            std::size_t passed = 0;
            std::size_t failed = 0;
            auto check = [&](const bool a_ok, const std::string_view a_name) {
                if (a_ok) {
                    ++passed;
                    a_out(std::format("PASS {}", a_name));
                } else {
                    ++failed;
                    a_out(std::format("FAIL {}", a_name));
                }
            };

            const auto valid = ParsePackageManifest(
                R"({"schemaVersion":1,"packageId":"author.sarah","priority":100,"requires":{"plugins":["Starfield.esm"],"assets":[]},"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"00005983"},"preset":"Presets/Sarah.npc","scope":"faceAndBody"}]})",
                manifestPath, false);
            check(valid.manifest && valid.manifest->assignments.size() == 1 && valid.issues.empty(),
                  "valid production manifest");
            check(valid.manifest && valid.manifest->assignments[0].target.CanonicalKey() ==
                                        "starfield.esm:00005983",
                  "canonical plugin plus local FormID target");

            const auto traversal = ParsePackageManifest(
                R"({"schemaVersion":1,"packageId":"author.traversal","priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"00005983"},"preset":"../escape.npc","scope":"faceAndBody"}]})",
                manifestPath, false);
            check(traversal.HasFatalError(), "parent traversal rejected");

            const auto unsupportedScope = ParsePackageManifest(
                R"({"schemaVersion":1,"packageId":"author.scope","priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"00005983"},"preset":"Sarah.npc","scope":"faceOnly"}]})",
                manifestPath, false);
            check(unsupportedScope.HasFatalError(), "unproven scope rejected");

            const auto unsupported = ParsePackageManifest(
                R"({"schemaVersion":2,"packageId":"author.version","priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[]})",
                manifestPath, false);
            check(unsupported.HasFatalError(), "unknown schema version rejected");

            const auto unknownProperty = ParsePackageManifest(
                R"({"schemaVersion":1,"packageId":"author.unknown","priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[],"surprise":true})",
                manifestPath, false);
            check(unknownProperty.HasFatalError(), "unknown root property rejected");

            const auto malformed = ParsePackageManifest(
                R"({"schemaVersion":1,"packageId":)", manifestPath, false);
            check(malformed.HasFatalError(), "truncated JSON rejected");

            std::string oversized(kMaxManifestBytes + 1, ' ');
            const auto tooLarge = ParsePackageManifest(oversized, manifestPath, false);
            check(tooLarge.HasFatalError(), "oversized manifest rejected");

            a_out(std::format("selftest: {} passed, {} failed", passed, failed));
        }

        void ReportDependencyResolution(
            const LineSink& a_out,
            const ResolvedAppearanceDependencies& a_result);

        void RunScan(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (TargetHoldActive()) {
                a_out("scan: refused while a bounded target hold is active; wait for exact rollback or run npcapp targetrestore");
                return;
            }
            if (g_scenePersistentEnabled.load(std::memory_order_acquire) ||
                PersistentAppliedCount() != 0) {
                a_out("scan: refused while persistent assignment is enabled or tracked; run npcapp scene persistent off first");
                return;
            }
            std::filesystem::path packagesRoot;
            if (a_args.size() > 2) {
                packagesRoot = std::filesystem::path{ JoinArguments(a_args, 2) };
            } else {
                packagesRoot = DefaultPackagesDirectory();
            }

            a_out(std::format("scan packagesRoot={}", packagesRoot.string()));
            auto discovery = DiscoverPackages(packagesRoot, true);
            for (const auto& issue : discovery.issues) {
                a_out(std::format("package issue code={} path={} @{}: {}",
                                  issue.code, issue.path.string(), issue.offset, issue.message));
            }

            std::size_t decodedPresets = 0;
            std::vector<PackageManifest> validatedPackages;
            for (const auto& package : discovery.packages) {
                bool packageRequirementsComplete = true;
                for (const auto& plugin : package.requirements.plugins) {
                    if (!FindLoadedPlugin(plugin)) {
                        a_out(std::format("package '{}' rejected: required plugin '{}' is not loaded",
                                          package.packageID, plugin));
                        packageRequirementsComplete = false;
                    }
                }
                const auto packageAssets =
                    CheckRequiredAssets(package.requirements, DefaultDataDirectory());
                if (!packageAssets.Complete()) {
                    packageRequirementsComplete = false;
                    for (const auto& asset : packageAssets.missing) {
                        a_out(std::format("package '{}' rejected: required Data asset '{}' is missing or is not a regular file",
                                          package.packageID, asset.generic_string()));
                    }
                }
                if (!packageRequirementsComplete) {
                    continue;
                }

                PackageManifest validatedPackage = package;
                validatedPackage.assignments.clear();
                for (const auto& assignment : package.assignments) {
                    bool assignmentRequirementsComplete = true;
                    for (const auto& plugin : assignment.requirements.plugins) {
                        if (!FindLoadedPlugin(plugin)) {
                            a_out(std::format("candidate package='{}' target={} rejected: required plugin '{}' is not loaded",
                                              package.packageID,
                                              assignment.target.CanonicalKey(), plugin));
                            assignmentRequirementsComplete = false;
                        }
                    }
                    const auto assignmentAssets =
                        CheckRequiredAssets(assignment.requirements, DefaultDataDirectory());
                    if (!assignmentAssets.Complete()) {
                        assignmentRequirementsComplete = false;
                        for (const auto& asset : assignmentAssets.missing) {
                            a_out(std::format("candidate package='{}' target={} rejected: required Data asset '{}' is missing or is not a regular file",
                                              package.packageID,
                                              assignment.target.CanonicalKey(),
                                              asset.generic_string()));
                        }
                    }
                    if (!assignmentRequirementsComplete) {
                        continue;
                    }

                    const auto decoded = LoadCkPreset(assignment.presetPath);
                    if (!decoded.preset) {
                        a_out(std::format("candidate package='{}' target={} rejected: preset '{}' does not satisfy a supported 1.16.244 producer contract",
                                          package.packageID,
                                          assignment.target.CanonicalKey(),
                                          assignment.presetPath.string()));
                        for (const auto& issue : decoded.issues) {
                            a_out(std::format("  preset issue code={} path={} @0x{:X}: {}",
                                              issue.code, issue.path.string(), issue.offset,
                                              issue.message));
                        }
                        continue;
                    }
                    ++decodedPresets;

                    auto* npc = ResolveEligibleTarget(a_out, assignment.target);
                    if (!npc) {
                        a_out(std::format("candidate package='{}' target={} rejected: target is absent or ineligible",
                                          package.packageID,
                                          assignment.target.CanonicalKey()));
                        continue;
                    }
                    const auto resolved = ResolveAppearanceDependencies(*decoded.preset, npc);
                    ReportDependencyResolution(a_out, resolved);
                    if (!resolved.Complete()) {
                        a_out(std::format("candidate package='{}' target={} rejected: preset appearance references are incomplete",
                                          package.packageID,
                                          assignment.target.CanonicalKey()));
                        continue;
                    }
                    validatedPackage.assignments.push_back(assignment);
                }
                if (!validatedPackage.assignments.empty()) {
                    validatedPackages.push_back(std::move(validatedPackage));
                }
            }

            const auto selection = SelectAssignments(validatedPackages);
            for (const auto& decision : selection.decisions) {
                a_out(std::format("conflict target={} package='{}' priority={} result={} reason={}",
                                  decision.targetKey, decision.packageID, decision.priority,
                                  decision.winner ? "winner" : "loser", decision.reason));
            }

            std::unordered_set<RE::TESFormID> resolvedBaseIDs;
            std::unordered_map<RE::TESFormID, SelectedAssignment> resolvedAssignments;
            for (const auto& assignment : selection.winners) {
                auto* npc = ResolveEligibleTarget(a_out, assignment.target);
                if (npc) {
                    resolvedBaseIDs.insert(npc->GetFormID());
                    resolvedAssignments.emplace(npc->GetFormID(), assignment);
                }
                a_out(std::format("  winner package='{}' priority={} target={} preset={} targetResolved={}",
                                  assignment.packageID, assignment.priority,
                                  assignment.target.CanonicalKey(), assignment.presetPath.string(),
                                  static_cast<void*>(npc)));
            }
            const auto resolvedCount = resolvedBaseIDs.size();
            {
                const std::scoped_lock lock{ g_eventMutex };
                g_targetBaseIDs = std::move(resolvedBaseIDs);
                g_sceneAssignments = std::move(resolvedAssignments);
                g_loadedTargetRefs.clear();
                g_sceneTargetRefs.clear();
                g_sceneSet3dSuppressions.clear();
                g_pendingSceneApply.reset();
            }
            g_sceneDispatchObserveArmed.store(false, std::memory_order_release);
            g_sceneAutoTrialArmed.store(false, std::memory_order_release);
            a_out(std::format("scan: discoveredPackages={} validPackages={} decodedPresets={} validCandidates={} winners={} resolvedTargets={}; validation only, owned population/application gate prevents mutation",
                              discovery.packages.size(), validatedPackages.size(), decodedPresets,
                              selection.decisions.size(), selection.winners.size(), resolvedCount));
        }

        void RunEvent(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            const std::string_view action =
                a_args.size() >= 3 ? std::string_view{ a_args[2] } : std::string_view{ "status" };
            if (action == "status") {
                std::size_t targets = 0;
                std::size_t loadedRefs = 0;
                {
                    const std::scoped_lock lock{ g_eventMutex };
                    targets = g_targetBaseIDs.size();
                    loadedRefs = g_loadedTargetRefs.size();
                }
                a_out(std::format("event: registered={} targetBases={} loadedTargetRefs={} total={} matchingLoads={} matchingUnloads={} debounced={} queued={} ran={}",
                                  g_eventRegistered.load(std::memory_order_relaxed), targets, loadedRefs,
                                  g_eventCount.load(std::memory_order_relaxed),
                                  g_matchingLoadCount.load(std::memory_order_relaxed),
                                  g_matchingUnloadCount.load(std::memory_order_relaxed),
                                  g_debouncedLoadCount.load(std::memory_order_relaxed),
                                  g_queuedApplyCount.load(std::memory_order_relaxed),
                                  g_ranApplyCount.load(std::memory_order_relaxed)));
                return;
            }

            auto* source = RE::TESObjectLoadedEvent::GetEventSource();
            if (!source) {
                a_out("event: GetEventSource() returned null; FAIL CLOSED");
                return;
            }
            auto* sink = &ObjectLoadedSink::GetSingleton();
            if (action == "on") {
                if (!g_eventRegistered.exchange(true, std::memory_order_acq_rel)) {
                    source->RegisterSink(sink);
                }
                a_out(std::format("event: sink registered source={} (ID 64152); load mappings first to select targets",
                                  static_cast<void*>(source)));
            } else if (action == "off") {
                if (g_eventRegistered.exchange(false, std::memory_order_acq_rel)) {
                    source->UnregisterSink(sink);
                }
                const std::scoped_lock lock{ g_eventMutex };
                g_loadedTargetRefs.clear();
                a_out("event: sink unregistered; per-reference state cleared");
            } else {
                a_out("usage: npcapp event <status|on|off>");
            }
        }

        void RunInspect(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (a_args.size() < 3) {
                a_out("usage: npcapp inspect <preset.npc>");
                return;
            }
            const std::filesystem::path path{ JoinArguments(a_args, 2) };
            const auto result = LoadCkPreset(path);
            if (!result.preset) {
                a_out(std::format("inspect: REJECTED path={} issues={}", path.string(), result.issues.size()));
                for (const auto& issue : result.issues) {
                    a_out(std::format("issue code={} offset=0x{:X}: {}", issue.code, issue.offset,
                                      issue.message));
                }
                return;
            }

            const auto& preset = *result.preset;
            std::size_t boneSliders = 0;
            for (const auto& region : preset.facialBoneRegions) {
                boneSliders += region.sliders.size();
            }
            std::string bodyValues;
            for (const auto value : preset.bodyMorphRegionValues) {
                if (!bodyValues.empty()) {
                    bodyValues += ", ";
                }
                bodyValues += std::format("{:.6g}", value);
            }

            a_out(std::format("inspect: ACCEPTED path={} producer='{}' schema={}", path.string(),
                              preset.producer, preset.schemaVersion));
            a_out(std::format("identity editorID='{}' race='{}' sex={} skinTone={}",
                              preset.npcFormEditorID, preset.raceFormID,
                              preset.sex == PresetSex::kFemale ? "Female" : "Male", preset.skinTone));
            a_out(std::format("colors hair='{}' brow='{}' facialHair='{}' eye='{}' jewelry='{}' teeth='{}'",
                              preset.hairColor, preset.browHairColor, preset.facialHairColor,
                              preset.eyeColor, preset.jewelryColor, preset.teethCustomization));
            a_out(std::format("counts bodyRegions={} miscHeadParts={} uniqueHeadParts={} facialMorphs={} boneRegions={} boneSliders={} tintLayers={}",
                              preset.bodyMorphRegionValues.size(), preset.miscHeadParts.size(),
                              preset.uniqueHeadParts.size(), preset.facialMorphSliders.size(),
                              preset.facialBoneRegions.size(), boneSliders, preset.postBlendLayers.size()));
            a_out(std::format("morphWeights=({:.6g}, {:.6g}, {:.6g}) bodyValues=[{}]",
                              preset.morphWeights.x, preset.morphWeights.y, preset.morphWeights.z,
                              bodyValues));
            a_out("inspect: decoded only; no game state mutated");
        }

        void ReportDependencyResolution(
            const LineSink& a_out,
            const ResolvedAppearanceDependencies& a_result)
        {
            a_out(std::format(
                "refs: forms={} bones={} shapes={} colors={} avm={} stringCatalogs={} complete={} race={} uniqueSlots={} misc={} regionGroups={} boneIDs={} boneGroups={} shapeNames={} colorAtoms={} avmLayers={} avmValues={} avmModulations={} issues={}",
                a_result.formReferencesComplete, a_result.boneReferencesComplete,
                a_result.shapeReferencesComplete, a_result.colorReferencesComplete,
                a_result.avmReferencesComplete,
                a_result.stringCatalogsComplete,
                a_result.Complete(),
                static_cast<void*>(a_result.race), a_result.uniqueHeadParts.size(),
                a_result.miscHeadParts.size(), a_result.validatedBoneRegionGroups,
                a_result.resolvedBoneSliderIDs, a_result.resolvedBoneGroupNames,
                a_result.resolvedFacialShapeNames,
                a_result.resolvedColorAndTeethAtoms,
                a_result.resolvedAvmLayerNames, a_result.resolvedAvmValues,
                a_result.resolvedAvmModulations,
                a_result.issues.size()));
            for (const auto& issue : a_result.issues) {
                a_out(std::format("  dependency issue code={} field={} value='{}': {}",
                                  issue.code, issue.field, issue.value, issue.message));
            }
        }

        void RunSceneEvent(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            const std::string_view action =
                a_args.size() >= 3 ? std::string_view{ a_args[2] } : std::string_view{ "status" };
            if (action == "status") {
                std::size_t targets = 0;
                std::size_t trackedRefs = 0;
                std::size_t assignments = 0;
                std::size_t persistentTracked = 0;
                bool pending = false;
                {
                    const std::scoped_lock lock{ g_eventMutex };
                    targets = g_targetBaseIDs.size();
                    trackedRefs = g_sceneTargetRefs.size();
                    assignments = g_sceneAssignments.size();
                    persistentTracked = g_persistentAppliedRefs.size();
                    pending = g_pendingSceneApply.has_value();
                }
                a_out(std::format("scene: startupPackagesPresent={} startupPersistentArmed={} registered={} observeArmed={} autoTrialArmed={} persistentEnabled={} persistentTracked={} targetBases={} assignments={} trackedRefs={} pending={} set3dTotal={} detachTotal={} matchingSet3d={} matchingDetach={} selfRefreshSuppressedSet3d={} selfRefreshSuppressedDetach={} debounced={} published={} nativeReady={} nativeObservePass={} nativeLoadingDeferrals={} nativeFrames={} nativeTid={} nativeInFlight={} autoAttempts={} autoApplies={} persistentAttempts={} persistentApplies={} persistentRemovals={}",
                                  g_startupPackagesPresent.load(std::memory_order_relaxed),
                                  g_startupPersistentArmed.load(std::memory_order_relaxed),
                                  g_sceneRegistered.load(std::memory_order_relaxed),
                                  g_sceneDispatchObserveArmed.load(std::memory_order_relaxed),
                                  g_sceneAutoTrialArmed.load(std::memory_order_relaxed),
                                  g_scenePersistentEnabled.load(std::memory_order_relaxed),
                                  persistentTracked,
                                  targets, assignments, trackedRefs, pending,
                                  g_sceneSet3dCount.load(std::memory_order_relaxed),
                                  g_sceneDetachCount.load(std::memory_order_relaxed),
                                  g_sceneMatchingSet3dCount.load(std::memory_order_relaxed),
                                  g_sceneMatchingDetachCount.load(std::memory_order_relaxed),
                                  g_sceneSuppressedSet3dCount.load(std::memory_order_relaxed),
                                  g_sceneSuppressedDetachCount.load(std::memory_order_relaxed),
                                  g_sceneDebouncedCount.load(std::memory_order_relaxed),
                                  g_sceneQueuedApplyCount.load(std::memory_order_relaxed),
                                  g_sceneNativeReadyCount.load(std::memory_order_relaxed),
                                  g_sceneNativeObservePassCount.load(std::memory_order_relaxed),
                                  g_sceneNativeLoadingDeferralCount.load(std::memory_order_relaxed),
                                  g_sceneNativeFrameCount.load(std::memory_order_relaxed),
                                  g_sceneNativeThreadID.load(std::memory_order_relaxed),
                                  g_sceneNativeTaskInFlight.load(std::memory_order_relaxed),
                                  g_sceneAutoTrialAttemptCount.load(std::memory_order_relaxed),
                                  g_sceneAutoTrialApplyCount.load(std::memory_order_relaxed),
                                  g_scenePersistentAttemptCount.load(std::memory_order_relaxed),
                                  g_scenePersistentApplyCount.load(std::memory_order_relaxed),
                                  g_scenePersistentRemovalCount.load(std::memory_order_relaxed)));
                return;
            }

            if (action == "dispatch") {
                const std::string_view setting =
                    a_args.size() >= 4 ? std::string_view{ a_args[3] } : std::string_view{};
                if (setting == "on") {
                    std::size_t assignments = 0;
                    {
                        const std::scoped_lock lock{ g_eventMutex };
                        assignments = g_sceneAssignments.size();
                    }
                    if (!g_sceneRegistered.load(std::memory_order_acquire) || assignments == 0) {
                        a_out(std::format("scene dispatch: refused registered={} assignments={}; run scan and scene on first",
                                          g_sceneRegistered.load(std::memory_order_relaxed), assignments));
                        return;
                    }
                    if (g_sceneAutoTrialArmed.load(std::memory_order_acquire) ||
                        g_scenePersistentEnabled.load(std::memory_order_acquire) ||
                        PersistentAppliedCount() != 0 || TargetHoldActive()) {
                        a_out("scene dispatch: refused while an automatic/persistent assignment or target hold is active");
                        return;
                    }
                    g_sceneDispatchObserveArmed.store(true, std::memory_order_release);
                    a_out(std::format("scene dispatch: ARMED observation-only; next matched 3D attach must reach the verified native game-thread drain with menus closed and loaded 3D stable for {} frames; no mutation",
                                      kSceneStableNativeFrames));
                } else if (setting == "off") {
                    g_sceneDispatchObserveArmed.store(false, std::memory_order_release);
                    a_out("scene dispatch: DISARMED; no mutation");
                } else {
                    a_out("usage: npcapp scene dispatch <on|off>");
                }
                return;
            }

            if (action == "persistent") {
                const std::string_view setting =
                    a_args.size() >= 4 ? std::string_view{ a_args[3] } : std::string_view{};
                if (setting == "on") {
                    std::optional<RE::TESFormID> requestedRefID;
                    if (a_args.size() >= 5) {
                        requestedRefID = ParseFormID(a_args[4]);
                        if (!requestedRefID) {
                            a_out("scene persistent: actorRefID must be hexadecimal");
                            return;
                        }
                    }

                    std::size_t assignments = 0;
                    {
                        const std::scoped_lock lock{ g_eventMutex };
                        assignments = g_sceneAssignments.size();
                    }
                    if (!g_sceneRegistered.load(std::memory_order_acquire) || assignments == 0) {
                        a_out(std::format("scene persistent: refused registered={} assignments={}; run scan and scene on first",
                                          g_sceneRegistered.load(std::memory_order_relaxed), assignments));
                        return;
                    }
                    if (TargetHoldActive() ||
                        g_sceneDispatchObserveArmed.load(std::memory_order_acquire) ||
                        g_sceneAutoTrialArmed.load(std::memory_order_acquire)) {
                        a_out("scene persistent: refused while observation, bounded auto, or target hold is active");
                        return;
                    }
                    const auto nativeDiagnostics =
                        Util::NativeMainThreadQueue::GetDiagnostics();
                    if (!nativeDiagnostics.insideDrain ||
                        !nativeDiagnostics.queueEnabled || nativeDiagnostics.singleton == 0) {
                        a_out(std::format("scene persistent: refused because native main-thread proof is not active insideDrain={} queueEnabled={} singleton=0x{:X}; no mutation",
                                          nativeDiagnostics.insideDrain,
                                          nativeDiagnostics.queueEnabled,
                                          nativeDiagnostics.singleton));
                        return;
                    }

                    std::uint64_t sequence = 0;
                    RE::TESFormID requestedBaseID = 0;
                    if (requestedRefID) {
                        auto* actor = RE::TESForm::LookupByID<RE::Actor>(*requestedRefID);
                        auto* base = actor ? actor->GetNPC() : nullptr;
                        if (!base || !HasLoaded3D(actor)) {
                            a_out(std::format("scene persistent: requested ref=0x{:08X} is not a loaded actor with 3D; no mutation",
                                              *requestedRefID));
                            return;
                        }
                        requestedBaseID = base->GetFormID();
                        {
                            const std::scoped_lock lock{ g_eventMutex };
                            if (!g_sceneAssignments.contains(requestedBaseID)) {
                                a_out(std::format("scene persistent: requested ref=0x{:08X} base=0x{:08X} has no validated winning assignment; no mutation",
                                                  *requestedRefID, requestedBaseID));
                                return;
                            }
                            if (g_persistentAppliedRefs.contains(*requestedRefID)) {
                                g_scenePersistentEnabled.store(true, std::memory_order_release);
                                a_out(std::format("scene persistent: already enabled and tracked for ref=0x{:08X}",
                                                  *requestedRefID));
                                return;
                            }
                            if (g_pendingSceneApply) {
                                a_out(std::format("scene persistent: refused explicit reconcile because sequence={} is already pending",
                                                  g_pendingSceneApply->sequence));
                                return;
                            }
                            g_sceneTargetRefs.insert(*requestedRefID);
                            sequence = ++g_nextSceneSequence;
                            g_pendingSceneApply = PendingSceneApply{
                                .refID = *requestedRefID,
                                .baseID = requestedBaseID,
                                .eventThreadID = ::GetCurrentThreadId(),
                                .sequence = sequence,
                            };
                            g_sceneQueuedApplyCount.fetch_add(1, std::memory_order_relaxed);
                        }
                    }

                    g_scenePersistentEnabled.store(true, std::memory_order_release);
                    g_sceneAutoTrialArmed.store(false, std::memory_order_release);
                    if (requestedRefID) {
                        a_out(std::format("scene persistent: ENABLED and queued explicit loaded-generation reconcile sequence={} ref=0x{:08X} base=0x{:08X}; waits for {} stable verified native frames",
                                          sequence, *requestedRefID, requestedBaseID,
                                          kSceneStableNativeFrames));
                    } else {
                        a_out(std::format("scene persistent: ENABLED for {} validated assignment(s); future matched 3D generations will apply automatically",
                                          assignments));
                    }
                } else if (setting == "off") {
                    g_scenePersistentEnabled.store(false, std::memory_order_release);
                    {
                        const std::scoped_lock lock{ g_eventMutex };
                        g_pendingSceneApply.reset();
                    }
                    const auto summary = RemovePersistentAppearances(a_out);
                    a_out(std::format("scene persistent: DISABLED refreshed={} retiredUnloaded={} failed={} remainingTracked={}",
                                      summary.refreshed, summary.retiredUnloaded,
                                      summary.failed, PersistentAppliedCount()));
                } else {
                    a_out("usage: npcapp scene persistent <on [actorRefID]|off>");
                }
                return;
            }

            if (action == "auto") {
                const std::string_view setting =
                    a_args.size() >= 4 ? std::string_view{ a_args[3] } : std::string_view{};
                if (setting == "on") {
                    std::size_t assignments = 0;
                    {
                        const std::scoped_lock lock{ g_eventMutex };
                        assignments = g_sceneAssignments.size();
                    }
                    if (!g_sceneRegistered.load(std::memory_order_acquire) || assignments == 0) {
                        a_out(std::format("scene auto: refused registered={} assignments={}; run scan and scene on first",
                                          g_sceneRegistered.load(std::memory_order_relaxed), assignments));
                        return;
                    }
                    if (TargetHoldActive() ||
                        g_scenePersistentEnabled.load(std::memory_order_acquire) ||
                        PersistentAppliedCount() != 0) {
                        a_out("scene auto: refused while another bounded hold or persistent assignment is active");
                        return;
                    }
                    if (g_sceneDispatchObserveArmed.load(std::memory_order_acquire)) {
                        a_out("scene auto: refused while observation-only dispatch is armed");
                        return;
                    }
                    const auto nativeDiagnostics =
                        Util::NativeMainThreadQueue::GetDiagnostics();
                    if (!nativeDiagnostics.insideDrain ||
                        !nativeDiagnostics.queueEnabled || nativeDiagnostics.singleton == 0) {
                        a_out(std::format("scene auto: refused because native main-thread proof is not active insideDrain={} queueEnabled={} singleton=0x{:X}; no mutation",
                                          nativeDiagnostics.insideDrain,
                                          nativeDiagnostics.queueEnabled,
                                          nativeDiagnostics.singleton));
                        return;
                    }
                    g_sceneAutoTrialArmed.store(true, std::memory_order_release);
                    a_out(std::format("scene auto: ARMED one-shot for {} validated assignment(s); next matched 3D attach waits for menus closed plus {} stable verified native frames, then starts a bounded {}-second apply with native-main-thread exact rollback",
                                      assignments, kSceneStableNativeFrames, kTargetHoldSeconds));
                } else if (setting == "off") {
                    g_sceneAutoTrialArmed.store(false, std::memory_order_release);
                    const auto finish = FinishTargetHold("scene auto disarm");
                    a_out(std::format("scene auto: DISARMED activeHoldResult={}",
                                      finish == TargetHoldFinish::kNoActiveHold ? "none" :
                                      finish == TargetHoldFinish::kRestored ? "restored" : "FAILED"));
                } else {
                    a_out("usage: npcapp scene auto <on|off>");
                }
                return;
            }

            auto* set3dSource = RE::RuntimeComponentDBFactory::ReferenceSet3d::GetEventSource();
            auto* detachSource = RE::RuntimeComponentDBFactory::ReferenceDetach::GetEventSource();
            if (!set3dSource || !detachSource) {
                a_out(std::format("scene: event source unavailable set3d={} detach={}; FAIL CLOSED",
                                  static_cast<void*>(set3dSource), static_cast<void*>(detachSource)));
                return;
            }

            auto* set3dSink = &ReferenceSet3dSink::GetSingleton();
            auto* detachSink = &ReferenceDetachSink::GetSingleton();
            if (action == "on") {
                if (!g_sceneRegistered.exchange(true, std::memory_order_acq_rel)) {
                    set3dSource->RegisterSink(set3dSink);
                    detachSource->RegisterSink(detachSink);
                }
                a_out(std::format("scene: sinks registered set3dSource={} (ID 49237) detachSource={} (ID 40306); load mappings first to select targets",
                                  static_cast<void*>(set3dSource), static_cast<void*>(detachSource)));
            } else if (action == "off") {
                g_sceneDispatchObserveArmed.store(false, std::memory_order_release);
                g_sceneAutoTrialArmed.store(false, std::memory_order_release);
                g_scenePersistentEnabled.store(false, std::memory_order_release);
                const auto finish = FinishTargetHold("scene sink disarm");
                const auto removal = RemovePersistentAppearances(a_out);
                if (removal.failed != 0) {
                    a_out(std::format("scene: sink disarm REFUSED because persistent removal failed={} remainingTracked={}; sinks remain registered",
                                      removal.failed, PersistentAppliedCount()));
                    return;
                }
                if (g_sceneRegistered.exchange(false, std::memory_order_acq_rel)) {
                    set3dSource->UnregisterSink(set3dSink);
                    detachSource->UnregisterSink(detachSink);
                }
                {
                    const std::scoped_lock lock{ g_eventMutex };
                    g_sceneTargetRefs.clear();
                    g_sceneSet3dSuppressions.clear();
                    g_pendingSceneApply.reset();
                }
                a_out(std::format("scene: sinks unregistered; per-reference state cleared; activeHoldResult={} persistentRefreshed={} persistentRetiredUnloaded={}",
                                  finish == TargetHoldFinish::kNoActiveHold ? "none" :
                                  finish == TargetHoldFinish::kRestored ? "restored" : "FAILED",
                                  removal.refreshed, removal.retiredUnloaded));
            } else {
                a_out("usage: npcapp scene <status|on|off|dispatch <on|off>|auto <on|off>|persistent <on [actorRefID]|off>>");
            }
        }

        void RunRefs(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (a_args.size() < 5) {
                a_out("usage: npcapp refs <plugin> <localFormID> <preset.npc>");
                return;
            }
            const auto localFormID = ParseFormID(a_args[3]);
            if (!localFormID || *localFormID > 0x00FFFFFF) {
                a_out("refs: localFormID must be hexadecimal and no greater than 00FFFFFF");
                return;
            }
            auto* target = ResolveEligibleTarget(a_out, Target{ a_args[2], *localFormID });
            if (!target) {
                return;
            }
            const std::filesystem::path path{ JoinArguments(a_args, 4) };
            const auto decoded = LoadCkPreset(path);
            if (!decoded.preset) {
                a_out(std::format("refs: preset rejected path={} issues={}",
                                  path.string(), decoded.issues.size()));
                for (const auto& issue : decoded.issues) {
                    a_out(std::format("  preset issue code={} @0x{:X}: {}",
                                      issue.code, issue.offset, issue.message));
                }
                return;
            }
            const auto resolved = ResolveAppearanceDependencies(*decoded.preset, target);
            ReportDependencyResolution(a_out, resolved);
            a_out("refs: read-only; donor creation and all NPC mutation remain disabled");
        }

        void RunAvmInspect(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (a_args.size() < 5) {
                a_out("usage: npcapp avm <plugin> <localFormID> <preset.npc>");
                return;
            }
            const auto localFormID = ParseFormID(a_args[3]);
            if (!localFormID || *localFormID > 0x00FFFFFF) {
                a_out("avm: localFormID must be hexadecimal and no greater than 00FFFFFF");
                return;
            }
            auto* target = ResolveEligibleTarget(a_out, Target{ a_args[2], *localFormID });
            if (!target) {
                return;
            }
            const std::filesystem::path path{ JoinArguments(a_args, 4) };
            const auto decoded = LoadCkPreset(path);
            if (!decoded.preset) {
                a_out(std::format("avm: preset rejected path={} issues={}",
                                  path.string(), decoded.issues.size()));
                return;
            }
            const auto resolved = ResolveAppearanceDependencies(*decoded.preset, target);
            ReportDependencyResolution(a_out, resolved);
            if (!resolved.Complete()) {
                a_out("avm: dependency resolution incomplete; no live AVM read");
                return;
            }

            std::size_t matched = 0;
            for (const auto& layer : decoded.preset->postBlendLayers) {
                const auto found = std::ranges::find_if(
                    target->tintAVMData, [&](const RE::AVMData& a_avm) {
                        return ::_stricmp(SafeText(a_avm.category.c_str()), layer.name.c_str()) == 0;
                    });
                if (found == target->tintAVMData.end()) {
                    a_out(std::format(
                        "avm layer='{}' presetValue='{}' presetMod='{}' presetIntensity={:.9g} live=MISSING",
                        layer.name, layer.value, layer.modulationValue, layer.intensity));
                    continue;
                }
                ++matched;
                a_out(std::format(
                    "avm layer='{}' presetValue='{}' presetMod='{}' presetIntensity={:.9g} liveType={} liveValue='{}' liveTexture='{}' liveIntensity={} liveColor={:02X}{:02X}{:02X}{:02X}",
                    layer.name, layer.value, layer.modulationValue, layer.intensity,
                    static_cast<std::uint32_t>(found->type),
                    SafeText(found->unk10.name.c_str()),
                    SafeText(found->unk10.texturePath.c_str()),
                    found->unk10.intensity,
                    found->unk10.color.red, found->unk10.color.green,
                    found->unk10.color.blue, found->unk10.color.alpha));
            }
            a_out(std::format(
                "avm: matched={}/{} liveEntries={}; read-only, no donor or target mutation",
                matched, decoded.preset->postBlendLayers.size(), target->tintAVMData.size()));
        }

        void RunResolve(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (a_args.size() < 4) {
                a_out("usage: npcapp resolve <plugin> <localFormID>");
                return;
            }
            const auto localFormID = ParseFormID(a_args[3]);
            if (!localFormID || *localFormID > 0x00FFFFFF) {
                a_out("resolve: localFormID must be hexadecimal and no greater than 00FFFFFF");
                return;
            }
            (void)ResolveEligibleTarget(a_out, Target{ a_args[2], *localFormID });
        }

        void RunDonor(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            std::uint32_t count = 1;
            if (a_args.size() >= 3) {
                const auto [ptr, ec] = std::from_chars(
                    a_args[2].data(), a_args[2].data() + a_args[2].size(), count, 10);
                if (ec != std::errc{} || ptr != a_args[2].data() + a_args[2].size() ||
                    count == 0 || count > 1000) {
                    a_out("donor: count must be a decimal integer from 1 through 1000");
                    return;
                }
            }

            const auto factoryAddress = REL::Relocation<std::uintptr_t>{ kNpcFactorySingletonID }.address();
            const auto factoryVtable = REL::Relocation<std::uintptr_t>{ kNpcFactoryVtableID }.address();
            const auto createAddress = REL::Relocation<std::uintptr_t>{ kNpcFactoryCreateID }.address();
            const auto npcVtable = REL::Relocation<std::uintptr_t>{ kNpcPrimaryVtableID }.address();
            const auto destructorAddress =
                REL::Relocation<std::uintptr_t>{ kNpcScalarDeletingDestructorID }.address();

            if (!Util::IsReadableRange(factoryAddress, sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryAddress) != factoryVtable ||
                !Util::IsReadableRange(factoryVtable + sizeof(std::uintptr_t), sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryVtable + sizeof(std::uintptr_t)) != createAddress ||
                !HasExpectedBytes(createAddress, kNpcFactoryCreateGate) ||
                !HasExpectedBytes(destructorAddress, kNpcDestructorGate)) {
                a_out("donor: factory/create/destructor contract mismatch; FAIL CLOSED");
                return;
            }

            using Create = RE::TESNPC* (*)(void*, bool);
            using Destroy = RE::TESNPC* (*)(RE::TESNPC*, std::uint32_t);
            const auto create = reinterpret_cast<Create>(createAddress);
            const auto destroy = reinterpret_cast<Destroy>(destructorAddress);

            std::size_t valid = 0;
            for (std::uint32_t i = 0; i < count; ++i) {
                auto* donor = create(reinterpret_cast<void*>(factoryAddress), false);
                if (!donor) {
                    a_out(std::format("donor: Create(false) returned null at cycle {}", i));
                    break;
                }
                std::size_t headPartCount = 0;
                {
                    auto headParts = donor->headParts.Lock();
                    headPartCount = (*headParts).size();
                }
                const auto formID = donor->GetFormID();
                const bool registered =
                    formID != 0 && RE::TESForm::LookupByID<RE::TESNPC>(formID) == donor;
                const bool initialized =
                    *reinterpret_cast<const std::uintptr_t*>(donor) == npcVtable &&
                    registered && donor->QRefCount() == 0 && donor->GetRace() == nullptr &&
                    donor->faceNPC == nullptr &&
                    headPartCount == 0 && donor->unk3D8 == nullptr && donor->unk3E0 == nullptr &&
                    donor->unk3E8 == nullptr && donor->tintAVMData.size() == 0 &&
                    donor->shapeBlendData == nullptr && donor->pronoun.underlying() == 0;
                if (i == 0 || !initialized) {
                    a_out(std::format(
                        "donor cycle={} ptr={} formID=0x{:08X} refCount={} vtableMatch={} race={} faceNPC={} headParts={} morphPtrs={}/{}/{} tint={} shapeBlend={} pronoun={} initialized={}",
                        i, static_cast<void*>(donor), formID, donor->QRefCount(),
                        *reinterpret_cast<const std::uintptr_t*>(donor) == npcVtable,
                        static_cast<void*>(donor->GetRace()), static_cast<void*>(donor->faceNPC),
                        headPartCount, static_cast<void*>(donor->unk3D8),
                        static_cast<void*>(donor->unk3E0), static_cast<void*>(donor->unk3E8),
                        donor->tintAVMData.size(), static_cast<void*>(donor->shapeBlendData),
                        donor->pronoun.underlying(), initialized));
                }
                destroy(donor, 1);
                const bool unregistered = RE::TESForm::LookupByID<RE::TESNPC>(formID) == nullptr;
                if (!initialized) {
                    a_out("donor: engine object did not satisfy registered-empty-container invariants; stopped after safe teardown");
                    break;
                }
                if (!unregistered) {
                    a_out(std::format(
                        "donor: formID 0x{:08X} remained registered after engine teardown; FAIL CLOSED",
                        formID));
                    break;
                }
                ++valid;
            }
            a_out(std::format(
                "donor: {}/{} Create(false)+registered-empty+engine-destroy+unregister cycles passed",
                valid, count));
        }

        void RunDonorSeed(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (a_args.size() < 5) {
                a_out("usage: npcapp donorseed <plugin> <localFormID> <preset.npc>");
                return;
            }
            const auto localFormID = ParseFormID(a_args[3]);
            if (!localFormID || *localFormID > 0x00FFFFFF) {
                a_out("donorseed: localFormID must be hexadecimal and no greater than 00FFFFFF");
                return;
            }
            auto* target = ResolveEligibleTarget(a_out, Target{ a_args[2], *localFormID });
            if (!target) {
                return;
            }

            const std::filesystem::path path{ JoinArguments(a_args, 4) };
            const auto decoded = LoadCkPreset(path);
            if (!decoded.preset) {
                a_out(std::format("donorseed: preset rejected path={} issues={}",
                                  path.string(), decoded.issues.size()));
                for (const auto& issue : decoded.issues) {
                    a_out(std::format("  preset issue code={} @0x{:X}: {}",
                                      issue.code, issue.offset, issue.message));
                }
                return;
            }
            const auto resolved = ResolveAppearanceDependencies(*decoded.preset, target);
            ReportDependencyResolution(a_out, resolved);
            if (!resolved.Complete()) {
                a_out("donorseed: dependency resolution incomplete; no donor created");
                return;
            }

            const auto factoryAddress = REL::Relocation<std::uintptr_t>{ kNpcFactorySingletonID }.address();
            const auto factoryVtable = REL::Relocation<std::uintptr_t>{ kNpcFactoryVtableID }.address();
            const auto createAddress = REL::Relocation<std::uintptr_t>{ kNpcFactoryCreateID }.address();
            const auto npcVtable = REL::Relocation<std::uintptr_t>{ kNpcPrimaryVtableID }.address();
            const auto destructorAddress =
                REL::Relocation<std::uintptr_t>{ kNpcScalarDeletingDestructorID }.address();
            const auto copyAddress = REL::Relocation<std::uintptr_t>{ kNpcCopyAppearanceID }.address();

            if (!Util::IsReadableRange(factoryAddress, sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryAddress) != factoryVtable ||
                !Util::IsReadableRange(factoryVtable + sizeof(std::uintptr_t), sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryVtable + sizeof(std::uintptr_t)) != createAddress ||
                !HasExpectedBytes(createAddress, kNpcFactoryCreateGate) ||
                !HasExpectedBytes(destructorAddress, kNpcDestructorGate) ||
                !HasExpectedBytes(copyAddress, kNpcCopyAppearanceGate)) {
                a_out("donorseed: factory/create/copy/destructor contract mismatch; FAIL CLOSED");
                return;
            }

            using Create = RE::TESNPC* (*)(void*, bool);
            using Copy = void (*)(RE::TESNPC*, RE::TESNPC*, bool);
            using Destroy = RE::TESNPC* (*)(RE::TESNPC*, std::uint32_t);
            const auto create = reinterpret_cast<Create>(createAddress);
            const auto copy = reinterpret_cast<Copy>(copyAddress);
            const auto destroy = reinterpret_cast<Destroy>(destructorAddress);

            const auto targetNonVisualBefore = Snapshot(target);
            const auto targetVisualBefore = SnapshotVisualSeed(target);
            auto* donor = create(reinterpret_cast<void*>(factoryAddress), false);
            if (!donor) {
                a_out("donorseed: Create(false) returned null; no target mutation");
                return;
            }
            const auto donorFormID = donor->GetFormID();
            std::size_t donorInitialHeadParts = 0;
            {
                auto headParts = donor->headParts.Lock();
                donorInitialHeadParts = (*headParts).size();
            }
            const bool donorInitialized =
                *reinterpret_cast<const std::uintptr_t*>(donor) == npcVtable &&
                donorFormID != 0 && RE::TESForm::LookupByID<RE::TESNPC>(donorFormID) == donor &&
                donor->QRefCount() == 0 && donor->GetRace() == nullptr && donor->faceNPC == nullptr &&
                donorInitialHeadParts == 0 && donor->unk3D8 == nullptr && donor->unk3E0 == nullptr &&
                donor->unk3E8 == nullptr && donor->tintAVMData.size() == 0 &&
                donor->shapeBlendData == nullptr && donor->pronoun.underlying() == 0;
            if (!donorInitialized) {
                destroy(donor, 1);
                a_out("donorseed: donor failed registered-empty-container invariants; safely destroyed before copy");
                return;
            }

            copy(donor, target, false);
            const bool engineCopiedSkinTone = donor->skinToneIndex == target->skinToneIndex;
            const auto donorVisual = SnapshotVisualSeed(donor);
            const auto targetNonVisualMid = Snapshot(target);
            const auto targetVisualMid = SnapshotVisualSeed(target);
            const bool valuesMatch = SameVisualSeedValues(targetVisualBefore, donorVisual);
            const bool storageIndependent =
                HasIndependentVisualStorage(targetVisualBefore, donorVisual);
            const bool targetUnchangedMid =
                targetNonVisualBefore == targetNonVisualMid && targetVisualBefore == targetVisualMid;

            destroy(donor, 1);
            const bool donorUnregistered =
                RE::TESForm::LookupByID<RE::TESNPC>(donorFormID) == nullptr;
            const auto targetNonVisualAfter = Snapshot(target);
            const auto targetVisualAfter = SnapshotVisualSeed(target);
            const bool targetUnchangedAfter =
                targetNonVisualBefore == targetNonVisualAfter && targetVisualBefore == targetVisualAfter;
            const bool passed = valuesMatch && storageIndependent && targetUnchangedMid &&
                                donorUnregistered && targetUnchangedAfter;

            a_out(std::format(
                "donorseed: donorFormID=0x{:08X} engineCopiedSkinToneByte={} visualValuesMatch={} storageIndependent={} targetUnchangedMid={} donorUnregistered={} targetUnchangedAfter={}",
                donorFormID, engineCopiedSkinTone,
                valuesMatch, storageIndependent, targetUnchangedMid,
                donorUnregistered, targetUnchangedAfter));
            a_out(std::format(
                "donorseed: source/donor headParts={}/{} morphRegions={}/{} boneValues={}/{} boneGroups={}/{} tint={}/{} shapeBlend={}/{}",
                targetVisualBefore.headParts.size(), donorVisual.headParts.size(),
                targetVisualBefore.morphRegionCount, donorVisual.morphRegionCount,
                targetVisualBefore.boneValueCount, donorVisual.boneValueCount,
                targetVisualBefore.boneGroupCount, donorVisual.boneGroupCount,
                targetVisualBefore.tintCount, donorVisual.tintCount,
                targetVisualBefore.shapeBlendCount, donorVisual.shapeBlendCount));
            if (!valuesMatch) {
                ReportVisualSeedComparison(a_out, targetVisualBefore, donorVisual);
            }
            a_out(passed ?
                      "donorseed: PASS engine-owned donor seed/copy/teardown; preset population pending; no target mutation" :
                      "donorseed: FAIL CLOSED; do not advance to preset population or target application");
        }

        void RunDonorMorph(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (a_args.size() < 5) {
                a_out("usage: npcapp donormorph <plugin> <localFormID> <preset.npc>");
                return;
            }
            const auto localFormID = ParseFormID(a_args[3]);
            if (!localFormID || *localFormID > 0x00FFFFFF) {
                a_out("donormorph: localFormID must be hexadecimal and no greater than 00FFFFFF");
                return;
            }
            auto* target = ResolveEligibleTarget(a_out, Target{ a_args[2], *localFormID });
            if (!target) {
                return;
            }
            const std::filesystem::path path{ JoinArguments(a_args, 4) };
            const auto decoded = LoadCkPreset(path);
            if (!decoded.preset) {
                a_out(std::format("donormorph: preset rejected path={} issues={}",
                                  path.string(), decoded.issues.size()));
                return;
            }
            const auto resolved = ResolveAppearanceDependencies(*decoded.preset, target);
            ReportDependencyResolution(a_out, resolved);
            if (!resolved.Complete()) {
                a_out("donormorph: dependency resolution incomplete; no donor created");
                return;
            }

            const auto factoryAddress = REL::Relocation<std::uintptr_t>{ kNpcFactorySingletonID }.address();
            const auto factoryVtable = REL::Relocation<std::uintptr_t>{ kNpcFactoryVtableID }.address();
            const auto createAddress = REL::Relocation<std::uintptr_t>{ kNpcFactoryCreateID }.address();
            const auto npcVtable = REL::Relocation<std::uintptr_t>{ kNpcPrimaryVtableID }.address();
            const auto destructorAddress =
                REL::Relocation<std::uintptr_t>{ kNpcScalarDeletingDestructorID }.address();
            const auto copyAddress = REL::Relocation<std::uintptr_t>{ kNpcCopyAppearanceID }.address();
            const auto shapeAddress = REL::Relocation<std::uintptr_t>{ kNpcSetShapeBlendID }.address();
            const auto bodyAddress = REL::Relocation<std::uintptr_t>{ kNpcSetBodyMorphID }.address();
            const auto boneAddress = REL::Relocation<std::uintptr_t>{ kNpcSetBoneValueID }.address();
            const auto boneGroupAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetBoneGroupValueID }.address();

            if (!Util::IsReadableRange(factoryAddress, sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryAddress) != factoryVtable ||
                !Util::IsReadableRange(factoryVtable + sizeof(std::uintptr_t), sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryVtable + sizeof(std::uintptr_t)) != createAddress ||
                !HasExpectedBytes(createAddress, kNpcFactoryCreateGate) ||
                !HasExpectedBytes(destructorAddress, kNpcDestructorGate) ||
                !HasExpectedBytes(copyAddress, kNpcCopyAppearanceGate) ||
                !HasExpectedBytes(shapeAddress, kNpcSetShapeBlendGate) ||
                !HasExpectedBytes(bodyAddress, kNpcSetBodyMorphGate) ||
                !HasExpectedBytes(boneAddress, kNpcSetBoneValueGate) ||
                !HasExpectedBytes(boneGroupAddress, kNpcSetBoneGroupValueGate)) {
                a_out("donormorph: factory/copy/morph-setter/destructor contract mismatch; FAIL CLOSED");
                return;
            }

            using Create = RE::TESNPC* (*)(void*, bool);
            using Copy = void (*)(RE::TESNPC*, RE::TESNPC*, bool);
            using SetShape = void (*)(RE::TESNPC*, const RE::BSFixedStringCS*, float);
            using SetBody = void (*)(RE::TESNPC*, std::uint32_t, float);
            using SetBone = void (*)(RE::TESNPC*, std::uint32_t, float);
            using EnsureBoneGroup = void (*)(
                RE::TESNPC*, std::uint32_t, const RE::BSFixedStringCS*);
            using Destroy = RE::TESNPC* (*)(RE::TESNPC*, std::uint32_t);
            const auto create = reinterpret_cast<Create>(createAddress);
            const auto copy = reinterpret_cast<Copy>(copyAddress);
            const auto setShape = reinterpret_cast<SetShape>(shapeAddress);
            const auto setBody = reinterpret_cast<SetBody>(bodyAddress);
            const auto setBone = reinterpret_cast<SetBone>(boneAddress);
            const auto ensureBoneGroup = reinterpret_cast<EnsureBoneGroup>(boneGroupAddress);
            const auto destroy = reinterpret_cast<Destroy>(destructorAddress);

            const auto targetNonVisualBefore = Snapshot(target);
            const auto targetVisualBefore = SnapshotVisualSeed(target);
            auto* donor = create(reinterpret_cast<void*>(factoryAddress), false);
            if (!donor) {
                a_out("donormorph: Create(false) returned null; no target mutation");
                return;
            }
            const auto donorFormID = donor->GetFormID();
            const bool donorInitialized =
                *reinterpret_cast<const std::uintptr_t*>(donor) == npcVtable &&
                donorFormID != 0 && RE::TESForm::LookupByID<RE::TESNPC>(donorFormID) == donor &&
                donor->QRefCount() == 0 && donor->unk3D8 == nullptr && donor->unk3E0 == nullptr &&
                donor->unk3E8 == nullptr && donor->shapeBlendData == nullptr;
            if (!donorInitialized) {
                destroy(donor, 1);
                a_out("donormorph: donor failed empty-container invariants; safely destroyed before copy");
                return;
            }

            copy(donor, target, false);
            donor->morphWeight.thin = static_cast<float>(decoded.preset->morphWeights.x);
            donor->morphWeight.muscular = static_cast<float>(decoded.preset->morphWeights.y);
            donor->morphWeight.fat = static_cast<float>(decoded.preset->morphWeights.z);
            for (std::size_t i = 0; i < decoded.preset->bodyMorphRegionValues.size(); ++i) {
                setBody(donor, static_cast<std::uint32_t>(i),
                        static_cast<float>(decoded.preset->bodyMorphRegionValues[i]));
            }
            for (const auto& morph : decoded.preset->facialMorphSliders) {
                const RE::BSFixedStringCS key{ morph.name.c_str() };
                setShape(donor, &key, static_cast<float>(morph.value));
            }
            for (const auto& region : decoded.preset->facialBoneRegions) {
                for (const auto& slider : region.sliders) {
                    if (slider.id != 0) {
                        setBone(donor, slider.id, static_cast<float>(slider.value));
                    } else {
                        const RE::BSFixedStringCS key{ slider.groupName.c_str() };
                        ensureBoneGroup(donor, region.regionID, &key);
                        if (donor->unk3E8) {
                            const auto outer = donor->unk3E8->find(region.regionID);
                            if (outer != donor->unk3E8->end() && outer->value) {
                                for (auto& entry : *outer->value) {
                                    if (::_stricmp(SafeText(entry.key.c_str()),
                                                   slider.groupName.c_str()) == 0) {
                                        entry.value = static_cast<float>(slider.value);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            const bool morphsMatch =
                ValidateDonorMorphPopulation(a_out, donor, *decoded.preset);
            const auto donorVisual = SnapshotVisualSeed(donor);
            const bool storageIndependent =
                HasIndependentVisualStorage(targetVisualBefore, donorVisual);
            const bool targetUnchangedMid =
                targetNonVisualBefore == Snapshot(target) &&
                targetVisualBefore == SnapshotVisualSeed(target);

            destroy(donor, 1);
            const bool donorUnregistered =
                RE::TESForm::LookupByID<RE::TESNPC>(donorFormID) == nullptr;
            const bool targetUnchangedAfter =
                targetNonVisualBefore == Snapshot(target) &&
                targetVisualBefore == SnapshotVisualSeed(target);
            const bool passed = morphsMatch && storageIndependent && targetUnchangedMid &&
                                donorUnregistered && targetUnchangedAfter;
            a_out(std::format(
                "donormorph: populated containers body={} shape={} boneValues={} boneRegionGroups={}",
                donorVisual.morphRegionCount, donorVisual.shapeBlendCount,
                donorVisual.boneValueCount, donorVisual.boneGroupCount));
            a_out(std::format(
                "donormorph: donorFormID=0x{:08X} morphsMatch={} storageIndependent={} targetUnchangedMid={} donorUnregistered={} targetUnchangedAfter={}",
                donorFormID, morphsMatch, storageIndependent, targetUnchangedMid,
                donorUnregistered, targetUnchangedAfter));
            a_out(passed ?
                      "donormorph: PASS decoded body/shape/bone population on temporary donor; headparts/colors/AVM and all target writes remain disabled" :
                      "donormorph: FAIL CLOSED; do not advance to other donor categories or target application");
        }

        void RunDonorVisual(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (a_args.size() < 5) {
                a_out("usage: npcapp donorvisual <plugin> <localFormID> <preset.npc>");
                return;
            }
            const auto localFormID = ParseFormID(a_args[3]);
            if (!localFormID || *localFormID > 0x00FFFFFF) {
                a_out("donorvisual: localFormID must be hexadecimal and no greater than 00FFFFFF");
                return;
            }
            auto* target = ResolveEligibleTarget(a_out, Target{ a_args[2], *localFormID });
            if (!target) {
                return;
            }
            const std::filesystem::path path{ JoinArguments(a_args, 4) };
            const auto decoded = LoadCkPreset(path);
            if (!decoded.preset) {
                a_out(std::format("donorvisual: preset rejected path={} issues={}",
                                  path.string(), decoded.issues.size()));
                return;
            }
            const auto resolved = ResolveAppearanceDependencies(*decoded.preset, target);
            ReportDependencyResolution(a_out, resolved);
            if (!resolved.Complete()) {
                a_out("donorvisual: dependency resolution incomplete; no donor created");
                return;
            }

            const auto factoryAddress = REL::Relocation<std::uintptr_t>{ kNpcFactorySingletonID }.address();
            const auto factoryVtable = REL::Relocation<std::uintptr_t>{ kNpcFactoryVtableID }.address();
            const auto createAddress = REL::Relocation<std::uintptr_t>{ kNpcFactoryCreateID }.address();
            const auto npcVtable = REL::Relocation<std::uintptr_t>{ kNpcPrimaryVtableID }.address();
            const auto destructorAddress =
                REL::Relocation<std::uintptr_t>{ kNpcScalarDeletingDestructorID }.address();
            const auto copyAddress = REL::Relocation<std::uintptr_t>{ kNpcCopyAppearanceID }.address();
            const auto removeHeadAddress =
                REL::Relocation<std::uintptr_t>{ kNpcRemoveHeadPartID }.address();
            const auto changeHeadAddress =
                REL::Relocation<std::uintptr_t>{ kNpcChangeHeadPartID }.address();
            const auto resolveEntryAddress =
                REL::Relocation<std::uintptr_t>{ kFaceDbResolveEntryID }.address();
            const auto setAvmAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetAvmDataID }.address();
            const auto removeAvmAddress =
                REL::Relocation<std::uintptr_t>{ kNpcRemoveAvmDataID }.address();

            if (!Util::IsReadableRange(factoryAddress, sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryAddress) != factoryVtable ||
                !Util::IsReadableRange(factoryVtable + sizeof(std::uintptr_t), sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryVtable + sizeof(std::uintptr_t)) != createAddress ||
                !HasExpectedBytes(createAddress, kNpcFactoryCreateGate) ||
                !HasExpectedBytes(destructorAddress, kNpcDestructorGate) ||
                !HasExpectedBytes(copyAddress, kNpcCopyAppearanceGate) ||
                !HasExpectedBytes(removeHeadAddress, kNpcRemoveHeadPartGate) ||
                !HasExpectedBytes(changeHeadAddress, kNpcChangeHeadPartGate) ||
                !HasExpectedBytes(resolveEntryAddress, kFaceDbResolveEntryGate) ||
                !HasExpectedBytes(setAvmAddress, kNpcSetAvmDataGate) ||
                !HasExpectedBytes(removeAvmAddress, kNpcRemoveAvmDataGate)) {
                a_out("donorvisual: factory/copy/headpart/FaceDB/AVM/destructor contract mismatch; FAIL CLOSED");
                return;
            }

            const auto resolveEntry = reinterpret_cast<ResolveFaceDbEntry>(resolveEntryAddress);
            std::vector<MaterializedAvmLayer> expectedAvms;
            if (!MaterializeAvmLayers(a_out, *decoded.preset, resolveEntry, expectedAvms)) {
                a_out("donorvisual: AVM materialization incomplete; no donor created");
                return;
            }

            using Create = RE::TESNPC* (*)(void*, bool);
            using Copy = void (*)(RE::TESNPC*, RE::TESNPC*, bool);
            using RemoveHeadPart = void (*)(RE::TESNPC*, RE::BGSHeadPart*, bool);
            using ChangeHeadPart = void (*)(RE::TESNPC*, RE::BGSHeadPart*);
            using SetAvmData = void (*)(RE::TESNPC*, const RE::AVMData*);
            using RemoveAvmData = void (*)(RE::TESNPC*, const RE::BSFixedString*);
            using Destroy = RE::TESNPC* (*)(RE::TESNPC*, std::uint32_t);
            const auto create = reinterpret_cast<Create>(createAddress);
            const auto copy = reinterpret_cast<Copy>(copyAddress);
            const auto removeHeadPart = reinterpret_cast<RemoveHeadPart>(removeHeadAddress);
            const auto changeHeadPart = reinterpret_cast<ChangeHeadPart>(changeHeadAddress);
            const auto setAvmData = reinterpret_cast<SetAvmData>(setAvmAddress);
            const auto removeAvmData = reinterpret_cast<RemoveAvmData>(removeAvmAddress);
            const auto destroy = reinterpret_cast<Destroy>(destructorAddress);

            const auto targetNonVisualBefore = Snapshot(target);
            const auto targetVisualBefore = SnapshotVisualSeed(target);
            auto* donor = create(reinterpret_cast<void*>(factoryAddress), false);
            if (!donor) {
                a_out("donorvisual: Create(false) returned null; no target mutation");
                return;
            }
            const auto donorFormID = donor->GetFormID();
            const bool donorInitialized =
                *reinterpret_cast<const std::uintptr_t*>(donor) == npcVtable &&
                donorFormID != 0 && RE::TESForm::LookupByID<RE::TESNPC>(donorFormID) == donor &&
                donor->QRefCount() == 0 && donor->tintAVMData.empty();
            if (!donorInitialized) {
                destroy(donor, 1);
                a_out("donorvisual: donor failed registered-empty invariants; safely destroyed before copy");
                return;
            }

            copy(donor, target, false);

            std::vector<RE::BGSHeadPart*> donorHeadParts;
            {
                auto headParts = donor->headParts.Lock();
                donorHeadParts.assign((*headParts).begin(), (*headParts).end());
            }
            for (std::size_t i = 1; i < resolved.uniqueHeadParts.size(); ++i) {
                if (resolved.uniqueHeadParts[i]) {
                    continue;
                }
                for (auto* part : donorHeadParts) {
                    if (part && static_cast<std::size_t>(part->type.get()) == i) {
                        removeHeadPart(donor, part, false);
                    }
                }
            }
            for (auto* part : resolved.uniqueHeadParts) {
                if (!part) {
                    continue;
                }
                bool present = false;
                {
                    auto headParts = donor->headParts.Lock();
                    present = std::ranges::find(*headParts, part) != (*headParts).end();
                }
                if (!present) {
                    changeHeadPart(donor, part);
                }
            }
            for (auto* part : resolved.miscHeadParts) {
                bool present = false;
                {
                    auto headParts = donor->headParts.Lock();
                    present = std::ranges::find(*headParts, part) != (*headParts).end();
                }
                if (!present) {
                    changeHeadPart(donor, part);
                }
            }

            donor->skinToneIndex = static_cast<std::uint8_t>(decoded.preset->skinTone);
            donor->teeth = decoded.preset->teethCustomization;
            donor->jewelryColor = decoded.preset->jewelryColor;
            donor->eyeColor = decoded.preset->eyeColor;
            donor->hairColor = decoded.preset->hairColor;
            donor->facialColor = decoded.preset->facialHairColor;
            donor->eyebrowColor = decoded.preset->browHairColor;

            std::vector<RE::BSFixedString> existingAvmCategories;
            existingAvmCategories.reserve(donor->tintAVMData.size());
            for (const auto& avm : donor->tintAVMData) {
                existingAvmCategories.push_back(avm.category);
            }
            for (const auto& category : existingAvmCategories) {
                const bool desired = std::ranges::any_of(
                    expectedAvms, [&](const MaterializedAvmLayer& a_expected) {
                        return ::_stricmp(SafeText(category.c_str()),
                                          SafeText(a_expected.data.category.c_str())) == 0;
                    });
                if (!desired) {
                    removeAvmData(donor, &category);
                }
            }
            for (const auto& expected : expectedAvms) {
                setAvmData(donor, &expected.data);
            }

            const bool visualValuesMatch = ValidateDonorVisualPopulation(
                a_out, donor, *decoded.preset, resolved, expectedAvms);
            const auto donorVisual = SnapshotVisualSeed(donor);
            const bool storageIndependent =
                HasIndependentVisualStorage(targetVisualBefore, donorVisual);
            const bool targetUnchangedMid =
                targetNonVisualBefore == Snapshot(target) &&
                targetVisualBefore == SnapshotVisualSeed(target);

            destroy(donor, 1);
            const bool donorUnregistered =
                RE::TESForm::LookupByID<RE::TESNPC>(donorFormID) == nullptr;
            const bool targetUnchangedAfter =
                targetNonVisualBefore == Snapshot(target) &&
                targetVisualBefore == SnapshotVisualSeed(target);
            const bool passed = visualValuesMatch && storageIndependent && targetUnchangedMid &&
                                donorUnregistered && targetUnchangedAfter;
            a_out(std::format(
                "donorvisual: populated headParts={} colors=7 avm={}",
                donorVisual.headParts.size(), donorVisual.tintCount));
            a_out(std::format(
                "donorvisual: donorFormID=0x{:08X} visualValuesMatch={} storageIndependent={} targetUnchangedMid={} donorUnregistered={} targetUnchangedAfter={}",
                donorFormID, visualValuesMatch, storageIndependent, targetUnchangedMid,
                donorUnregistered, targetUnchangedAfter));
            a_out(passed ?
                      "donorvisual: PASS decoded headparts/colors/AVM population on temporary donor; all target writes remain disabled" :
                      "donorvisual: FAIL CLOSED; do not advance to target application");
        }

        void RunDonorCopy(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (a_args.size() < 5) {
                a_out("usage: npcapp donorcopy <plugin> <localFormID> <preset.npc>");
                return;
            }
            const auto localFormID = ParseFormID(a_args[3]);
            if (!localFormID || *localFormID > 0x00FFFFFF) {
                a_out("donorcopy: localFormID must be hexadecimal and no greater than 00FFFFFF");
                return;
            }
            auto* target = ResolveEligibleTarget(a_out, Target{ a_args[2], *localFormID });
            if (!target) {
                return;
            }
            const std::filesystem::path path{ JoinArguments(a_args, 4) };
            const auto decoded = LoadCkPreset(path);
            if (!decoded.preset) {
                a_out(std::format("donorcopy: preset rejected path={} issues={}",
                                  path.string(), decoded.issues.size()));
                return;
            }
            const auto resolved = ResolveAppearanceDependencies(*decoded.preset, target);
            ReportDependencyResolution(a_out, resolved);
            if (!resolved.Complete()) {
                a_out("donorcopy: dependency resolution incomplete; no donors created");
                return;
            }

            const auto factoryAddress = REL::Relocation<std::uintptr_t>{ kNpcFactorySingletonID }.address();
            const auto factoryVtable = REL::Relocation<std::uintptr_t>{ kNpcFactoryVtableID }.address();
            const auto createAddress = REL::Relocation<std::uintptr_t>{ kNpcFactoryCreateID }.address();
            const auto npcVtable = REL::Relocation<std::uintptr_t>{ kNpcPrimaryVtableID }.address();
            const auto destructorAddress =
                REL::Relocation<std::uintptr_t>{ kNpcScalarDeletingDestructorID }.address();
            const auto copyAddress = REL::Relocation<std::uintptr_t>{ kNpcCopyAppearanceID }.address();
            const auto shapeAddress = REL::Relocation<std::uintptr_t>{ kNpcSetShapeBlendID }.address();
            const auto bodyAddress = REL::Relocation<std::uintptr_t>{ kNpcSetBodyMorphID }.address();
            const auto boneAddress = REL::Relocation<std::uintptr_t>{ kNpcSetBoneValueID }.address();
            const auto boneGroupAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetBoneGroupValueID }.address();
            const auto removeHeadAddress =
                REL::Relocation<std::uintptr_t>{ kNpcRemoveHeadPartID }.address();
            const auto changeHeadAddress =
                REL::Relocation<std::uintptr_t>{ kNpcChangeHeadPartID }.address();
            const auto resolveEntryAddress =
                REL::Relocation<std::uintptr_t>{ kFaceDbResolveEntryID }.address();
            const auto setAvmAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetAvmDataID }.address();
            const auto removeAvmAddress =
                REL::Relocation<std::uintptr_t>{ kNpcRemoveAvmDataID }.address();
            const auto ownedCopyAddress = kNpcOwnedVisualCopyOffset.address();

            if (!Util::IsReadableRange(factoryAddress, sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryAddress) != factoryVtable ||
                !Util::IsReadableRange(factoryVtable + sizeof(std::uintptr_t), sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryVtable + sizeof(std::uintptr_t)) != createAddress ||
                !HasExpectedBytes(createAddress, kNpcFactoryCreateGate) ||
                !HasExpectedBytes(destructorAddress, kNpcDestructorGate) ||
                !HasExpectedBytes(copyAddress, kNpcCopyAppearanceGate) ||
                !HasExpectedBytes(shapeAddress, kNpcSetShapeBlendGate) ||
                !HasExpectedBytes(bodyAddress, kNpcSetBodyMorphGate) ||
                !HasExpectedBytes(boneAddress, kNpcSetBoneValueGate) ||
                !HasExpectedBytes(boneGroupAddress, kNpcSetBoneGroupValueGate) ||
                !HasExpectedBytes(removeHeadAddress, kNpcRemoveHeadPartGate) ||
                !HasExpectedBytes(changeHeadAddress, kNpcChangeHeadPartGate) ||
                !HasExpectedBytes(resolveEntryAddress, kFaceDbResolveEntryGate) ||
                !HasExpectedBytes(setAvmAddress, kNpcSetAvmDataGate) ||
                !HasExpectedBytes(removeAvmAddress, kNpcRemoveAvmDataGate) ||
                !HasExpectedBytes(ownedCopyAddress, kNpcOwnedVisualCopyGate)) {
                a_out("donorcopy: factory/population/owned-copy/destructor contract mismatch; FAIL CLOSED");
                return;
            }

            const auto resolveEntry = reinterpret_cast<ResolveFaceDbEntry>(resolveEntryAddress);
            std::vector<MaterializedAvmLayer> expectedAvms;
            if (!MaterializeAvmLayers(a_out, *decoded.preset, resolveEntry, expectedAvms)) {
                a_out("donorcopy: AVM materialization incomplete; no donors created");
                return;
            }

            using Create = RE::TESNPC* (*)(void*, bool);
            using Copy = void (*)(RE::TESNPC*, RE::TESNPC*, bool);
            using OwnedCopy = void (*)(RE::TESNPC*, RE::TESNPC*, bool);
            using Destroy = RE::TESNPC* (*)(RE::TESNPC*, std::uint32_t);
            const auto create = reinterpret_cast<Create>(createAddress);
            const auto copy = reinterpret_cast<Copy>(copyAddress);
            const auto ownedCopy = reinterpret_cast<OwnedCopy>(ownedCopyAddress);
            const auto setShape = reinterpret_cast<SetShapeBlend>(shapeAddress);
            const auto setBody = reinterpret_cast<SetBodyMorph>(bodyAddress);
            const auto setBone = reinterpret_cast<SetFacialBone>(boneAddress);
            const auto ensureBoneGroup =
                reinterpret_cast<EnsureFacialBoneGroup>(boneGroupAddress);
            const auto removeHeadPart = reinterpret_cast<RemoveHeadPart>(removeHeadAddress);
            const auto changeHeadPart = reinterpret_cast<ChangeHeadPart>(changeHeadAddress);
            const auto setAvmData = reinterpret_cast<SetAvmData>(setAvmAddress);
            const auto removeAvmData = reinterpret_cast<RemoveAvmData>(removeAvmAddress);
            const auto destroy = reinterpret_cast<Destroy>(destructorAddress);

            const auto targetNonVisualBefore = Snapshot(target);
            const auto targetVisualBefore = SnapshotVisualSeed(target);
            auto* sourceDonor = create(reinterpret_cast<void*>(factoryAddress), false);
            if (!sourceDonor) {
                a_out("donorcopy: source Create(false) returned null; no target mutation");
                return;
            }
            auto* destinationDonor = create(reinterpret_cast<void*>(factoryAddress), false);
            if (!destinationDonor) {
                const auto sourceFormID = sourceDonor->GetFormID();
                destroy(sourceDonor, 1);
                a_out(std::format(
                    "donorcopy: destination Create(false) returned null; source 0x{:08X} destroyed; no target mutation",
                    sourceFormID));
                return;
            }
            const auto sourceFormID = sourceDonor->GetFormID();
            const auto destinationFormID = destinationDonor->GetFormID();
            const auto initialized = [&](RE::TESNPC* a_donor, RE::TESFormID a_formID) {
                return *reinterpret_cast<const std::uintptr_t*>(a_donor) == npcVtable &&
                       a_formID != 0 &&
                       RE::TESForm::LookupByID<RE::TESNPC>(a_formID) == a_donor &&
                       a_donor->QRefCount() == 0 && a_donor->unk3D8 == nullptr &&
                       a_donor->unk3E0 == nullptr && a_donor->unk3E8 == nullptr &&
                       a_donor->shapeBlendData == nullptr && a_donor->tintAVMData.empty();
            };
            if (!initialized(sourceDonor, sourceFormID) ||
                !initialized(destinationDonor, destinationFormID)) {
                destroy(destinationDonor, 1);
                destroy(sourceDonor, 1);
                a_out("donorcopy: donor pair failed registered-empty invariants; both safely destroyed");
                return;
            }

            copy(sourceDonor, target, false);
            copy(destinationDonor, target, false);
            PopulatePresetMorphs(sourceDonor, *decoded.preset, setShape, setBody,
                                 setBone, ensureBoneGroup);
            PopulatePresetVisuals(sourceDonor, *decoded.preset, resolved, expectedAvms,
                                  removeHeadPart, changeHeadPart,
                                  setAvmData, removeAvmData);

            const bool sourceMorphsValid =
                ValidateDonorMorphPopulation(a_out, sourceDonor, *decoded.preset);
            const bool sourceVisualsValid = ValidateDonorVisualPopulation(
                a_out, sourceDonor, *decoded.preset, resolved, expectedAvms);
            const auto sourceVisualBefore = SnapshotVisualSeed(sourceDonor);

            destinationDonor->morphWeight.thin =
                static_cast<float>(decoded.preset->morphWeights.x);
            destinationDonor->morphWeight.muscular =
                static_cast<float>(decoded.preset->morphWeights.y);
            destinationDonor->morphWeight.fat =
                static_cast<float>(decoded.preset->morphWeights.z);
            for (std::size_t i = 0; i < decoded.preset->bodyMorphRegionValues.size(); ++i) {
                setBody(destinationDonor, static_cast<std::uint32_t>(i),
                        static_cast<float>(decoded.preset->bodyMorphRegionValues[i]));
            }
            destinationDonor->skinToneIndex =
                static_cast<std::uint8_t>(decoded.preset->skinTone);

            const auto destinationNonVisualBefore = Snapshot(destinationDonor);
            const auto destinationVisualBefore = SnapshotVisualSeed(destinationDonor);
            const bool controlledDifference =
                sourceVisualBefore.headParts != destinationVisualBefore.headParts ||
                sourceVisualBefore.eyeColor != destinationVisualBefore.eyeColor ||
                sourceVisualBefore.hairColor != destinationVisualBefore.hairColor;
            const bool facePolicyPrecondition =
                sourceDonor->faceNPC == nullptr && destinationDonor->faceNPC == nullptr;

            ownedCopy(destinationDonor, sourceDonor, false);

            const auto destinationVisualAfter = SnapshotVisualSeed(destinationDonor);
            const bool excludedFieldsPreserved =
                destinationVisualBefore.thin == destinationVisualAfter.thin &&
                destinationVisualBefore.muscular == destinationVisualAfter.muscular &&
                destinationVisualBefore.fat == destinationVisualAfter.fat &&
                destinationVisualBefore.morphRegionCount == destinationVisualAfter.morphRegionCount &&
                destinationVisualBefore.morphRegionStorage == destinationVisualAfter.morphRegionStorage &&
                destinationVisualBefore.skinToneIndex == destinationVisualAfter.skinToneIndex &&
                destinationVisualBefore.pronoun == destinationVisualAfter.pronoun;
            const bool facePolicyMatch =
                facePolicyPrecondition && destinationDonor->faceNPC == nullptr;
            const bool destinationNonVisualPreserved =
                destinationNonVisualBefore == Snapshot(destinationDonor);
            const bool destinationMorphsValid =
                ValidateDonorMorphPopulation(a_out, destinationDonor, *decoded.preset);
            const bool destinationVisualsValid = ValidateDonorVisualPopulation(
                a_out, destinationDonor, *decoded.preset, resolved, expectedAvms);
            const bool completeValuesMatch =
                SameVisualSeedValues(sourceVisualBefore, destinationVisualAfter);
            const bool exactValuesMatch =
                SameExactVisualValues(sourceDonor, destinationDonor);
            const bool storageIndependent =
                HasIndependentVisualStorage(sourceVisualBefore, destinationVisualAfter);
            const bool sourceUnchanged =
                sourceVisualBefore == SnapshotVisualSeed(sourceDonor);
            const bool rollbackBodyCompatible = target->unk3D8 && destinationDonor->unk3D8 &&
                target->unk3D8->size() == destinationDonor->unk3D8->size();
            if (rollbackBodyCompatible) {
                destinationDonor->morphWeight = target->morphWeight;
                for (std::uint32_t i = 0; i < target->unk3D8->size(); ++i) {
                    (*destinationDonor->unk3D8)[i] = (*target->unk3D8)[i];
                }
                destinationDonor->skinToneIndex = target->skinToneIndex;
                ownedCopy(destinationDonor, target, false);
            }
            const bool rollbackExact = rollbackBodyCompatible &&
                SameExactVisualValues(destinationDonor, target);
            const bool rollbackNonVisualPreserved =
                destinationNonVisualBefore == Snapshot(destinationDonor);
            const bool targetUnchangedMid =
                targetNonVisualBefore == Snapshot(target) &&
                targetVisualBefore == SnapshotVisualSeed(target);

            destroy(destinationDonor, 1);
            destroy(sourceDonor, 1);
            const bool donorsUnregistered =
                RE::TESForm::LookupByID<RE::TESNPC>(sourceFormID) == nullptr &&
                RE::TESForm::LookupByID<RE::TESNPC>(destinationFormID) == nullptr;
            const bool targetUnchangedAfter =
                targetNonVisualBefore == Snapshot(target) &&
                targetVisualBefore == SnapshotVisualSeed(target);
            const bool passed =
                sourceMorphsValid && sourceVisualsValid && controlledDifference &&
                excludedFieldsPreserved && facePolicyMatch && destinationNonVisualPreserved &&
                destinationMorphsValid && destinationVisualsValid && completeValuesMatch &&
                exactValuesMatch && storageIndependent && sourceUnchanged &&
                rollbackBodyCompatible && rollbackExact && rollbackNonVisualPreserved &&
                targetUnchangedMid &&
                donorsUnregistered && targetUnchangedAfter;

            a_out(std::format(
                "donorcopy: source=0x{:08X} destination=0x{:08X} controlledDifference={} completeValuesMatch={} exactValuesMatch={} storageIndependent={} sourceUnchanged={}",
                sourceFormID, destinationFormID, controlledDifference,
                completeValuesMatch, exactValuesMatch, storageIndependent, sourceUnchanged));
            a_out(std::format(
                "donorcopy: excludedFieldsPreserved={} facePolicyMatch={} destinationNonVisualPreserved={} rollbackBodyCompatible={} rollbackExact={} rollbackNonVisualPreserved={} targetUnchangedMid={} donorsUnregistered={} targetUnchangedAfter={}",
                excludedFieldsPreserved, facePolicyMatch, destinationNonVisualPreserved,
                rollbackBodyCompatible, rollbackExact, rollbackNonVisualPreserved,
                targetUnchangedMid, donorsUnregistered, targetUnchangedAfter));
            a_out(passed ?
                      "donorcopy: PASS lower owned visual-copy worker on disposable donor pair; all real-target writes remain disabled" :
                      "donorcopy: FAIL CLOSED; do not call the lower worker on a real target");
        }

        void RunTargetTrial(const LineSink& a_out, const std::vector<std::string>& a_args,
                            const TargetTrialMode a_mode, bool* const a_completed)
        {
            if (a_completed) {
                *a_completed = false;
            }
            const bool persistentLatch = a_mode == TargetTrialMode::kPersistentLatch;
            const bool holdForVisualProof = a_mode == TargetTrialMode::kHold ||
                a_mode == TargetTrialMode::kRenderLatch ||
                a_mode == TargetTrialMode::kOwnedSnapshotLatch;
            const bool ownedSnapshotLatch = a_mode == TargetTrialMode::kOwnedSnapshotLatch;
            const bool ownedSnapshotRequired = ownedSnapshotLatch || persistentLatch;
            const bool renderLatch = a_mode == TargetTrialMode::kRenderLatch ||
                ownedSnapshotLatch || persistentLatch;
            const std::string_view trialLabel = persistentLatch ? "targetpersistent" :
                ownedSnapshotLatch ? "targetsnapshot" :
                renderLatch ? "targetlatch" : holdForVisualProof ? "targethold" : "targettrial";
            if (a_args.size() < 6) {
                a_out(persistentLatch ?
                          "usage: npcapp targetpersistent <plugin> <localFormID> <actorRefID> <preset.npc>" :
                      ownedSnapshotLatch ?
                          "usage: npcapp targetsnapshot <plugin> <localFormID> <actorRefID> <preset.npc>" :
                      renderLatch ?
                          "usage: npcapp targetlatch <plugin> <localFormID> <actorRefID> <preset.npc>" :
                          holdForVisualProof ?
                              "usage: npcapp targethold <plugin> <localFormID> <actorRefID> <preset.npc>" :
                              "usage: npcapp targettrial <plugin> <localFormID> <actorRefID> <preset.npc>");
                return;
            }
            if (TargetHoldActive()) {
                a_out("targethold: another visual hold is active; use npcapp targetrestore or wait for automatic rollback");
                return;
            }
            const auto localFormID = ParseFormID(a_args[3]);
            const auto actorRefID = ParseFormID(a_args[4]);
            if (!localFormID || *localFormID > 0x00FFFFFF || !actorRefID) {
                a_out("targettrial: invalid localFormID or actorRefID");
                return;
            }
            auto* target = ResolveEligibleTarget(a_out, Target{ a_args[2], *localFormID });
            if (!target) {
                return;
            }
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(*actorRefID);
            if (!actor || actor->GetNPC() != target) {
                a_out(std::format(
                    "targettrial: actor ref 0x{:08X} missing or bound to a different base (actor={} actorBase={} expected={}); no mutation",
                    *actorRefID, static_cast<void*>(actor),
                    static_cast<void*>(actor ? actor->GetNPC() : nullptr),
                    static_cast<void*>(target)));
                return;
            }

            const std::filesystem::path path{ JoinArguments(a_args, 5) };
            const auto decoded = LoadCkPreset(path);
            if (!decoded.preset) {
                a_out(std::format("targettrial: preset rejected path={} issues={}",
                                  path.string(), decoded.issues.size()));
                return;
            }
            const auto resolved = ResolveAppearanceDependencies(*decoded.preset, target);
            ReportDependencyResolution(a_out, resolved);
            if (!resolved.Complete()) {
                a_out("targettrial: dependency resolution incomplete; no mutation");
                return;
            }

            const auto factoryAddress = REL::Relocation<std::uintptr_t>{ kNpcFactorySingletonID }.address();
            const auto factoryVtable = REL::Relocation<std::uintptr_t>{ kNpcFactoryVtableID }.address();
            const auto createAddress = REL::Relocation<std::uintptr_t>{ kNpcFactoryCreateID }.address();
            const auto npcVtable = REL::Relocation<std::uintptr_t>{ kNpcPrimaryVtableID }.address();
            const auto destructorAddress =
                REL::Relocation<std::uintptr_t>{ kNpcScalarDeletingDestructorID }.address();
            const auto copyAddress = REL::Relocation<std::uintptr_t>{ kNpcCopyAppearanceID }.address();
            const auto shapeAddress = REL::Relocation<std::uintptr_t>{ kNpcSetShapeBlendID }.address();
            const auto bodyAddress = REL::Relocation<std::uintptr_t>{ kNpcSetBodyMorphID }.address();
            const auto boneAddress = REL::Relocation<std::uintptr_t>{ kNpcSetBoneValueID }.address();
            const auto boneGroupAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetBoneGroupValueID }.address();
            const auto removeHeadAddress =
                REL::Relocation<std::uintptr_t>{ kNpcRemoveHeadPartID }.address();
            const auto changeHeadAddress =
                REL::Relocation<std::uintptr_t>{ kNpcChangeHeadPartID }.address();
            const auto resolveEntryAddress =
                REL::Relocation<std::uintptr_t>{ kFaceDbResolveEntryID }.address();
            const auto setAvmAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetAvmDataID }.address();
            const auto removeAvmAddress =
                REL::Relocation<std::uintptr_t>{ kNpcRemoveAvmDataID }.address();
            const auto ownedCopyAddress = kNpcOwnedVisualCopyOffset.address();
            const auto refreshAddress =
                REL::Relocation<std::uintptr_t>{ kActorAppearanceRefreshID }.address();

            if (!Util::IsReadableRange(factoryAddress, sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryAddress) != factoryVtable ||
                !Util::IsReadableRange(factoryVtable + sizeof(std::uintptr_t), sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryVtable + sizeof(std::uintptr_t)) != createAddress ||
                !HasExpectedBytes(createAddress, kNpcFactoryCreateGate) ||
                !HasExpectedBytes(destructorAddress, kNpcDestructorGate) ||
                !HasExpectedBytes(copyAddress, kNpcCopyAppearanceGate) ||
                !HasExpectedBytes(shapeAddress, kNpcSetShapeBlendGate) ||
                !HasExpectedBytes(bodyAddress, kNpcSetBodyMorphGate) ||
                !HasExpectedBytes(boneAddress, kNpcSetBoneValueGate) ||
                !HasExpectedBytes(boneGroupAddress, kNpcSetBoneGroupValueGate) ||
                !HasExpectedBytes(removeHeadAddress, kNpcRemoveHeadPartGate) ||
                !HasExpectedBytes(changeHeadAddress, kNpcChangeHeadPartGate) ||
                !HasExpectedBytes(resolveEntryAddress, kFaceDbResolveEntryGate) ||
                !HasExpectedBytes(setAvmAddress, kNpcSetAvmDataGate) ||
                !HasExpectedBytes(removeAvmAddress, kNpcRemoveAvmDataGate) ||
                !HasExpectedBytes(ownedCopyAddress, kNpcOwnedVisualCopyGate) ||
                !HasExpectedBytes(refreshAddress, kActorAppearanceRefreshGate)) {
                a_out("targettrial: population/copy/refresh/destructor contract mismatch; FAIL CLOSED");
                return;
            }

            const auto resolveEntry = reinterpret_cast<ResolveFaceDbEntry>(resolveEntryAddress);
            std::vector<MaterializedAvmLayer> expectedAvms;
            if (!MaterializeAvmLayers(a_out, *decoded.preset, resolveEntry, expectedAvms)) {
                a_out("targettrial: AVM materialization incomplete; no mutation");
                return;
            }

            using Create = RE::TESNPC* (*)(void*, bool);
            using Copy = void (*)(RE::TESNPC*, RE::TESNPC*, bool);
            using OwnedCopy = void (*)(RE::TESNPC*, RE::TESNPC*, bool);
            using Refresh = void (*)(RE::Actor*, bool, std::uint32_t, bool);
            using Destroy = RE::TESNPC* (*)(RE::TESNPC*, std::uint32_t);
            const auto create = reinterpret_cast<Create>(createAddress);
            const auto copy = reinterpret_cast<Copy>(copyAddress);
            const auto ownedCopy = reinterpret_cast<OwnedCopy>(ownedCopyAddress);
            const auto refresh = reinterpret_cast<Refresh>(refreshAddress);
            const auto setShape = reinterpret_cast<SetShapeBlend>(shapeAddress);
            const auto setBody = reinterpret_cast<SetBodyMorph>(bodyAddress);
            const auto setBone = reinterpret_cast<SetFacialBone>(boneAddress);
            const auto ensureBoneGroup =
                reinterpret_cast<EnsureFacialBoneGroup>(boneGroupAddress);
            const auto removeHeadPart = reinterpret_cast<RemoveHeadPart>(removeHeadAddress);
            const auto changeHeadPart = reinterpret_cast<ChangeHeadPart>(changeHeadAddress);
            const auto setAvmData = reinterpret_cast<SetAvmData>(setAvmAddress);
            const auto removeAvmData = reinterpret_cast<RemoveAvmData>(removeAvmAddress);
            const auto destroy = reinterpret_cast<Destroy>(destructorAddress);

            const auto targetNonVisualBefore = Snapshot(target);
            const auto targetVisualBefore = SnapshotVisualSeed(target);
            auto originalOwnedVisual = CaptureOwnedVisualSnapshot(target);
            const auto originalActorFlags = target->actorData.actorBaseFlags.underlying();
            auto* const originalFaceNPC = target->faceNPC;
            auto* backupDonor = create(reinterpret_cast<void*>(factoryAddress), false);
            auto* presetDonor = create(reinterpret_cast<void*>(factoryAddress), false);
            if (!backupDonor || !presetDonor) {
                if (presetDonor) {
                    destroy(presetDonor, 1);
                }
                if (backupDonor) {
                    destroy(backupDonor, 1);
                }
                a_out("targettrial: failed to create the backup/preset donor pair; no mutation");
                return;
            }
            const auto backupFormID = backupDonor->GetFormID();
            const auto presetFormID = presetDonor->GetFormID();
            const auto initialized = [&](RE::TESNPC* a_donor, RE::TESFormID a_formID) {
                return *reinterpret_cast<const std::uintptr_t*>(a_donor) == npcVtable &&
                       a_formID != 0 &&
                       RE::TESForm::LookupByID<RE::TESNPC>(a_formID) == a_donor &&
                       a_donor->QRefCount() == 0 && a_donor->unk3D8 == nullptr &&
                       a_donor->unk3E0 == nullptr && a_donor->unk3E8 == nullptr &&
                       a_donor->shapeBlendData == nullptr && a_donor->tintAVMData.empty();
            };
            if (!initialized(backupDonor, backupFormID) ||
                !initialized(presetDonor, presetFormID)) {
                destroy(presetDonor, 1);
                destroy(backupDonor, 1);
                a_out("targettrial: donor pair failed registered-empty invariants; no mutation");
                return;
            }

            copy(backupDonor, target, false);
            copy(presetDonor, target, false);
            PopulatePresetMorphs(presetDonor, *decoded.preset, setShape, setBody,
                                 setBone, ensureBoneGroup);
            PopulatePresetVisuals(presetDonor, *decoded.preset, resolved, expectedAvms,
                                  removeHeadPart, changeHeadPart,
                                  setAvmData, removeAvmData);

            const bool backupExact = SameExactVisualValues(backupDonor, target);
            const bool backupIndependent = HasIndependentVisualStorage(
                targetVisualBefore, SnapshotVisualSeed(backupDonor));
            const bool ownedSnapshotExact = !ownedSnapshotRequired ||
                (SameExactVisualValues(target, originalOwnedVisual) &&
                 SameExactVisualValues(backupDonor, originalOwnedVisual));
            const bool presetMorphsValid =
                ValidateDonorMorphPopulation(a_out, presetDonor, *decoded.preset);
            const bool presetVisualsValid = ValidateDonorVisualPopulation(
                a_out, presetDonor, *decoded.preset, resolved, expectedAvms);
            const bool controlledDifference =
                !SameExactVisualValues(presetDonor, backupDonor);
            const bool controlledDifferenceRequired = !persistentLatch;
            const bool rollbackBodyCompatible = backupDonor->unk3D8 &&
                backupDonor->unk3D8->size() == decoded.preset->bodyMorphRegionValues.size();
            if (!backupExact || !backupIndependent || !ownedSnapshotExact || !presetMorphsValid ||
                !presetVisualsValid ||
                (controlledDifferenceRequired && !controlledDifference) ||
                !rollbackBodyCompatible ||
                targetNonVisualBefore != Snapshot(target) ||
                targetVisualBefore != SnapshotVisualSeed(target)) {
                destroy(presetDonor, 1);
                destroy(backupDonor, 1);
                a_out(std::format(
                    "targettrial: preflight failed backupExact={} backupIndependent={} ownedSnapshotExact={} presetMorphsValid={} presetVisualsValid={} controlledDifference={} controlledDifferenceRequired={} rollbackBodyCompatible={}; no mutation",
                    backupExact, backupIndependent, ownedSnapshotExact, presetMorphsValid,
                    presetVisualsValid, controlledDifference, controlledDifferenceRequired,
                    rollbackBodyCompatible));
                return;
            }

            presetDonor->faceNPC = nullptr;
            target->morphWeight.thin = static_cast<float>(decoded.preset->morphWeights.x);
            target->morphWeight.muscular = static_cast<float>(decoded.preset->morphWeights.y);
            target->morphWeight.fat = static_cast<float>(decoded.preset->morphWeights.z);
            for (std::size_t i = 0; i < decoded.preset->bodyMorphRegionValues.size(); ++i) {
                setBody(target, static_cast<std::uint32_t>(i),
                        static_cast<float>(decoded.preset->bodyMorphRegionValues[i]));
            }
            target->skinToneIndex = static_cast<std::uint8_t>(decoded.preset->skinTone);
            ownedCopy(target, presetDonor, false);

            const bool targetMorphsValid =
                ValidateDonorMorphPopulation(a_out, target, *decoded.preset);
            const bool targetVisualsValid = ValidateDonorVisualPopulation(
                a_out, target, *decoded.preset, resolved, expectedAvms);
            const bool targetMatchesPreset = SameExactVisualValues(target, presetDonor);
            const bool targetStorageIndependent = HasIndependentVisualStorage(
                SnapshotVisualSeed(presetDonor), SnapshotVisualSeed(target));
            const auto targetNonVisualAfterApply = Snapshot(target);
            const bool targetNonVisualPreserved =
                targetNonVisualBefore == targetNonVisualAfterApply;
            const bool applyValidated = targetMorphsValid && targetVisualsValid &&
                targetMatchesPreset && targetStorageIndependent &&
                targetNonVisualPreserved && target->faceNPC == nullptr;

            bool actor3DLoaded = false;
            bool refreshIssued = false;
            if (applyValidated) {
                actor3DLoaded = HasLoaded3D(actor);
                NotifyBaseAppearanceChanged(target, 0x800);
                NotifyBaseAppearanceChanged(target, 0x4000);
                SuppressNextSceneSet3d(*actorRefID);
                refresh(actor, false, 0x28, false);
                refreshIssued = true;
            }
            const auto targetNonVisualAfterApplyRefresh = Snapshot(target);
            const auto applyRefreshDirtyMask =
                targetNonVisualBefore.actorFlagsExceptSex ^
                targetNonVisualAfterApplyRefresh.actorFlagsExceptSex;
            const bool applyRefreshNonVisualExpected =
                (applyRefreshDirtyMask & ~kAppearanceRefreshDirtyActorFlag) == 0 &&
                SameNonVisualIgnoringRefreshDirtyFlag(
                    targetNonVisualBefore, targetNonVisualAfterApplyRefresh);

            // The outer ID 68122 worker is useful for seeding disposable donors,
            // but it also writes nonvisual TESNPC fields. Restore with the proven
            // lower visual-only worker plus its three explicitly excluded fields.
            const auto restoreVisualOnly = [&]() {
                target->morphWeight = backupDonor->morphWeight;
                for (std::uint32_t i = 0; i < backupDonor->unk3D8->size(); ++i) {
                    (*target->unk3D8)[i] = (*backupDonor->unk3D8)[i];
                }
                target->skinToneIndex = backupDonor->skinToneIndex;
                ownedCopy(target, backupDonor, false);
                target->faceNPC = originalFaceNPC;
            };

            if (persistentLatch) {
                const bool persistentReady = applyValidated && refreshIssued && actor3DLoaded &&
                    applyRefreshNonVisualExpected && ownedSnapshotExact;
                if (persistentReady) {
                    destroy(presetDonor, 1);
                    presetDonor = nullptr;
                    const bool presetDonorUnregistered =
                        RE::TESForm::LookupByID<RE::TESNPC>(presetFormID) == nullptr;
                    if (presetDonorUnregistered) {
                        restoreVisualOnly();
                        target->actorData.actorBaseFlags =
                            static_cast<RE::ACTOR_BASE_DATA::Flag>(originalActorFlags);
                        const bool baseOriginalNow =
                            SameExactVisualValues(target, backupDonor) &&
                            SameExactVisualValues(target, originalOwnedVisual) &&
                            target->faceNPC == originalFaceNPC &&
                            targetNonVisualBefore == Snapshot(target);
                        if (baseOriginalNow) {
                            destroy(backupDonor, 1);
                            backupDonor = nullptr;
                            const bool donorsUnregistered =
                                RE::TESForm::LookupByID<RE::TESNPC>(backupFormID) == nullptr &&
                                RE::TESForm::LookupByID<RE::TESNPC>(presetFormID) == nullptr;
                            if (donorsUnregistered) {
                                if (a_completed) {
                                    *a_completed = true;
                                }
                                a_out(std::format(
                                    "targetpersistent: APPLY + BASE RESTORE + DONOR TEARDOWN PASS targetMatchesPreset={} storageIndependent={} ownedSnapshotExact={} nonVisualPreserved={} actor3DLoaded={} refreshDirtyMask=0x{:08X} baseOriginalNow={} donorsUnregistered={}; rendered preset remains latched until an original-base refresh",
                                    targetMatchesPreset, targetStorageIndependent,
                                    ownedSnapshotExact, targetNonVisualPreserved,
                                    actor3DLoaded, applyRefreshDirtyMask,
                                    baseOriginalNow, donorsUnregistered));
                                return;
                            }

                            NotifyBaseAppearanceChanged(target, 0x800);
                            NotifyBaseAppearanceChanged(target, 0x4000);
                            SuppressNextSceneSet3d(*actorRefID);
                            refresh(actor, false, 0x28, false);
                            target->actorData.actorBaseFlags =
                                static_cast<RE::ACTOR_BASE_DATA::Flag>(originalActorFlags);
                            a_out("targetpersistent: donor teardown did not unregister both forms; original-base refresh issued and assignment not retained");
                            return;
                        }
                        a_out("targetpersistent: immediate base restore failed; issuing ordinary exact rollback");
                    }
                } else {
                    a_out(std::format(
                        "targetpersistent: preflight failed applyValidated={} actor3DLoaded={} refreshIssued={} refreshNonVisualExpected={} ownedSnapshotExact={}; rolling back immediately",
                        applyValidated, actor3DLoaded, refreshIssued,
                        applyRefreshNonVisualExpected, ownedSnapshotExact));
                }
            }

            if (holdForVisualProof) {
                const bool holdReady = applyValidated && refreshIssued && actor3DLoaded &&
                    applyRefreshNonVisualExpected;
                if (holdReady) {
                    destroy(presetDonor, 1);
                    presetDonor = nullptr;
                    const bool presetDonorUnregistered =
                        RE::TESForm::LookupByID<RE::TESNPC>(presetFormID) == nullptr;
                    if (presetDonorUnregistered) {
                        bool latchBaseRestored = false;
                        if (renderLatch) {
                            restoreVisualOnly();
                            target->actorData.actorBaseFlags =
                                static_cast<RE::ACTOR_BASE_DATA::Flag>(originalActorFlags);
                            latchBaseRestored =
                                SameExactVisualValues(target, backupDonor) &&
                                (!ownedSnapshotLatch ||
                                 SameExactVisualValues(target, originalOwnedVisual)) &&
                                target->faceNPC == originalFaceNPC &&
                                targetNonVisualBefore == Snapshot(target);
                            if (!latchBaseRestored) {
                                a_out(std::format(
                                    "{}: immediate base restore failed; issuing ordinary restore refresh",
                                    trialLabel));
                            }
                        }

                        if (renderLatch && !latchBaseRestored) {
                            // Fall through to the ordinary immediate rollback path.
                        } else {
                            auto state = std::make_unique<TargetHoldState>();
                            state->serial = g_nextTargetHoldSerial.fetch_add(
                                1, std::memory_order_relaxed) + 1;
                            state->targetFormID = target->GetFormID();
                            state->actorRefID = *actorRefID;
                            state->backupFormID = backupFormID;
                            state->presetFormID = presetFormID;
                            state->target = target;
                            state->actor = actor;
                            state->backup = backupDonor;
                            state->originalFaceNPC = originalFaceNPC;
                            state->originalNonVisual = targetNonVisualBefore;
                            state->originalActorFlags = originalActorFlags;
                            state->baseRestoredBeforeWait = renderLatch;

                            bool donorsDestroyedBeforeWait = false;
                            if (ownedSnapshotLatch) {
                                state->originalVisual = std::move(originalOwnedVisual);
                                destroy(backupDonor, 1);
                                backupDonor = nullptr;
                                state->backup = nullptr;
                                donorsDestroyedBeforeWait =
                                    RE::TESForm::LookupByID<RE::TESNPC>(backupFormID) == nullptr &&
                                    RE::TESForm::LookupByID<RE::TESNPC>(presetFormID) == nullptr;
                                state->donorsDestroyedBeforeWait = donorsDestroyedBeforeWait;
                                if (!donorsDestroyedBeforeWait) {
                                    NotifyBaseAppearanceChanged(target, 0x800);
                                    NotifyBaseAppearanceChanged(target, 0x4000);
                                    SuppressNextSceneSet3d(*actorRefID);
                                    refresh(actor, false, 0x28, false);
                                    target->actorData.actorBaseFlags =
                                        static_cast<RE::ACTOR_BASE_DATA::Flag>(originalActorFlags);
                                    a_out("targetsnapshot: donor teardown did not unregister both forms; original-base refresh issued and no hold armed");
                                    return;
                                }
                            }

                            const auto holdSerial = state->serial;
                            {
                                const std::scoped_lock lock{ g_targetHoldMutex };
                                g_targetHold = std::move(state);
                            }
                            ScheduleTargetHoldRollback(holdSerial);
                            if (ownedSnapshotLatch) {
                                a_out(std::format(
                                    "targetsnapshot: APPLY + BASE RESTORE + DONOR TEARDOWN PASS targetMatchesPreset={} storageIndependent={} ownedSnapshotExact={} nonVisualPreserved={} actor3DLoaded={} refreshDirtyMask=0x{:08X} baseOriginalNow={} donorsUnregistered={}",
                                    targetMatchesPreset, targetStorageIndependent,
                                    ownedSnapshotExact, targetNonVisualPreserved,
                                    actor3DLoaded, applyRefreshDirtyMask,
                                    latchBaseRestored, donorsDestroyedBeforeWait));
                                a_out(std::format(
                                    "targetsnapshot: ACTIVE render-latch observation for {} seconds; base is original, both donors are already destroyed, and only plugin-owned values remain",
                                    kTargetHoldSeconds));
                            } else if (renderLatch) {
                                a_out(std::format(
                                    "targetlatch: APPLY + BASE RESTORE PASS targetMatchesPreset={} storageIndependent={} nonVisualPreserved={} actor3DLoaded={} refreshDirtyMask=0x{:08X} baseOriginalNow={}",
                                    targetMatchesPreset, targetStorageIndependent,
                                    targetNonVisualPreserved, actor3DLoaded,
                                    applyRefreshDirtyMask, latchBaseRestored));
                                a_out(std::format(
                                    "targetlatch: ACTIVE render-latch observation for {} seconds; base is already original and no second refresh has run; look for Afro now, then delayed original refresh",
                                    kTargetHoldSeconds));
                            } else {
                                a_out(std::format(
                                    "targethold: APPLY PASS targetMatchesPreset={} storageIndependent={} nonVisualPreserved={} actor3DLoaded={} refreshDirtyMask=0x{:08X}",
                                    targetMatchesPreset, targetStorageIndependent,
                                    targetNonVisualPreserved, actor3DLoaded, applyRefreshDirtyMask));
                                a_out(std::format(
                                    "targethold: ACTIVE for {} seconds; look at Sarah now; DO NOT SAVE, LOAD, FAST-TRAVEL, OR EXIT; automatic exact rollback is armed",
                                    kTargetHoldSeconds));
                            }
                            return;
                        }
                    }
                    a_out(std::format(
                        "{}: setup failed; rolling back immediately", trialLabel));
                } else {
                    a_out(std::format(
                        "{}: visual-hold preflight failed applyValidated={} actor3DLoaded={} refreshIssued={} refreshNonVisualExpected={}; rolling back immediately",
                        trialLabel,
                        applyValidated, actor3DLoaded, refreshIssued,
                        applyRefreshNonVisualExpected));
                }
            }

            restoreVisualOnly();
            auto targetNonVisualAfterRestore = Snapshot(target);
            bool restoreExact = SameExactVisualValues(target, backupDonor) &&
                target->faceNPC == originalFaceNPC &&
                SameNonVisualIgnoringRefreshDirtyFlag(
                    targetNonVisualBefore, targetNonVisualAfterRestore);
            std::uint32_t restoreAttempts = 1;
            if (!restoreExact) {
                restoreVisualOnly();
                targetNonVisualAfterRestore = Snapshot(target);
                restoreExact = SameExactVisualValues(target, backupDonor) &&
                    target->faceNPC == originalFaceNPC &&
                    SameNonVisualIgnoringRefreshDirtyFlag(
                        targetNonVisualBefore, targetNonVisualAfterRestore);
                restoreAttempts = 2;
            }
            if (refreshIssued) {
                NotifyBaseAppearanceChanged(target, 0x800);
                NotifyBaseAppearanceChanged(target, 0x4000);
                SuppressNextSceneSet3d(*actorRefID);
                refresh(actor, false, 0x28, false);
            }
            const auto targetNonVisualAfterRestoreRefreshRaw = Snapshot(target);
            const auto restoreRefreshDirtyMask =
                targetNonVisualBefore.actorFlagsExceptSex ^
                targetNonVisualAfterRestoreRefreshRaw.actorFlagsExceptSex;
            const bool restoreRefreshNonVisualExpected =
                (restoreRefreshDirtyMask & ~kAppearanceRefreshDirtyActorFlag) == 0 &&
                SameNonVisualIgnoringRefreshDirtyFlag(
                    targetNonVisualBefore, targetNonVisualAfterRestoreRefreshRaw);
            target->actorData.actorBaseFlags =
                static_cast<RE::ACTOR_BASE_DATA::Flag>(originalActorFlags);
            const auto targetNonVisualAfterRestoreRefresh = Snapshot(target);

            if (!restoreExact || !applyRefreshNonVisualExpected ||
                !restoreRefreshNonVisualExpected ||
                targetNonVisualBefore != targetNonVisualAfterRestoreRefresh) {
                ReportSnapshot(a_out, "targettrial original       ", targetNonVisualBefore);
                ReportSnapshot(a_out, "targettrial after apply    ", targetNonVisualAfterApply);
                ReportSnapshot(a_out, "targettrial after applyRef ", targetNonVisualAfterApplyRefresh);
                ReportSnapshot(a_out, "targettrial after restore  ", targetNonVisualAfterRestore);
                ReportSnapshot(a_out, "targettrial after restoreRef", targetNonVisualAfterRestoreRefreshRaw);
                ReportVisualSeedComparison(
                    a_out, SnapshotVisualSeed(backupDonor), SnapshotVisualSeed(target));
            }

            if (presetDonor) {
                destroy(presetDonor, 1);
            }
            destroy(backupDonor, 1);
            const bool donorsUnregistered =
                RE::TESForm::LookupByID<RE::TESNPC>(presetFormID) == nullptr &&
                RE::TESForm::LookupByID<RE::TESNPC>(backupFormID) == nullptr;
            const bool targetNonVisualAfter =
                targetNonVisualBefore == targetNonVisualAfterRestoreRefresh;
            const bool passed = applyValidated && refreshIssued && restoreExact &&
                applyRefreshNonVisualExpected && restoreRefreshNonVisualExpected &&
                donorsUnregistered && targetNonVisualAfter;

            a_out(std::format(
                "targettrial: APPLY targetMorphsValid={} targetVisualsValid={} targetMatchesPreset={} storageIndependent={} nonVisualPreserved={} actor3DLoaded={} refreshIssued={}",
                targetMorphsValid, targetVisualsValid, targetMatchesPreset,
                targetStorageIndependent, targetNonVisualPreserved,
                actor3DLoaded, refreshIssued));
            a_out(std::format(
                "targettrial: RESTORE exact={} attempts={} refreshDirtyMasks=0x{:08X}/0x{:08X} refreshNonVisualExpected={}/{} donorsUnregistered={} nonVisualAfter={} faceNPC=restored",
                restoreExact, restoreAttempts, applyRefreshDirtyMask, restoreRefreshDirtyMask,
                applyRefreshNonVisualExpected, restoreRefreshNonVisualExpected,
                donorsUnregistered, targetNonVisualAfter));
            a_out(passed ?
                      "targettrial: PASS transient Sarah base application + vanilla refresh + exact rollback; final target is original" :
                      "targettrial: FAIL CLOSED; inspect Sarah immediately before any save or further test");
        }

        void RunTargetRestore(const LineSink& a_out)
        {
            switch (FinishTargetHold("manual command")) {
            case TargetHoldFinish::kNoActiveHold:
                a_out("targetrestore: no visual hold is active");
                break;
            case TargetHoldFinish::kRestored:
                a_out("targetrestore: PASS exact rollback; final target is original");
                break;
            case TargetHoldFinish::kFailed:
                a_out("targetrestore: FAIL CLOSED; stop Starfield without saving");
                break;
            }
        }

        void RunCopyRef(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (a_args.size() < 4) {
                a_out("usage: npcapp copyref <targetRefID> <sourceRefID> [sourceIsPlayer=0|1]");
                return;
            }
            const auto targetID = ParseFormID(a_args[2]);
            const auto sourceID = ParseFormID(a_args[3]);
            if (!targetID || !sourceID) {
                a_out("copyref: invalid hexadecimal form ID");
                return;
            }
            bool sourceIsPlayer = *sourceID == 0x14;
            if (a_args.size() >= 5) {
                if (a_args[4] != "0" && a_args[4] != "1") {
                    a_out("copyref: sourceIsPlayer must be 0 or 1");
                    return;
                }
                sourceIsPlayer = a_args[4] == "1";
            }

            auto* target = RE::TESForm::LookupByID<RE::Actor>(*targetID);
            auto* source = RE::TESForm::LookupByID<RE::Actor>(*sourceID);
            if (!target || !source) {
                a_out(std::format("copyref: actor lookup failed target={} source={}",
                                  static_cast<void*>(target), static_cast<void*>(source)));
                return;
            }
            auto* targetBase = target->GetNPC();
            auto* sourceBase = source->GetNPC();
            if (!targetBase || !sourceBase || !targetBase->IsUnique()) {
                a_out(std::format("copyref: ineligible target/source base target={} source={} targetUnique={}",
                                  static_cast<void*>(targetBase), static_cast<void*>(sourceBase),
                                  targetBase && targetBase->IsUnique()));
                return;
            }
            if (targetBase->pronoun.underlying() != sourceBase->pronoun.underlying()) {
                a_out(std::format(
                    "copyref: rejected before mutation because TESNPC::CopyAppearance would copy pronoun (target={} source={})",
                    targetBase->pronoun.underlying(), sourceBase->pronoun.underlying()));
                return;
            }

            using Worker = void (*)(RE::Actor*, RE::TESNPC*, bool);
            REL::Relocation<Worker> worker{ kActorCopyAppearanceWorkerID };
            if (!HasExpectedGate(worker.address())) {
                a_out(std::format("copyref: ID 97401 contract mismatch at img+0x{:X}; FAIL CLOSED",
                                  Util::ToRva(worker.address())));
                return;
            }

            const auto before = Snapshot(targetBase);
            ReportSnapshot(a_out, "before", before);
            a_out(std::format("copyref: calling vanilla worker targetRef=0x{:08X} sourceBase=0x{:08X} sourceIsPlayer={}",
                              *targetID, sourceBase->GetFormID(), sourceIsPlayer));
            worker(target, sourceBase, sourceIsPlayer);
            const auto after = Snapshot(targetBase);
            ReportSnapshot(a_out, "after ", after);
            a_out(before == after
                      ? "copyref: PASS nonvisual snapshot unchanged; vanilla refresh invoked"
                      : "copyref: FAIL nonvisual snapshot changed; do not use this path");
        }

        void OnNpcAppearanceDataLoaded()
        {
            g_startupPackagesPresent.store(false, std::memory_order_release);
            g_startupPersistentArmed.store(false, std::memory_order_release);

            const auto packagesRoot = DefaultPackagesDirectory();
            std::error_code ec;
            const bool packagesPresent =
                std::filesystem::is_directory(packagesRoot, ec) && !ec;
            g_startupPackagesPresent.store(packagesPresent, std::memory_order_release);
            if (!packagesPresent) {
                REX::INFO("[NpcAppearance] startup disabled: package directory is absent ({})",
                          packagesRoot.string());
                return;
            }

            const LineSink startupOut = [](const std::string& a_text) {
                REX::INFO("[NpcAppearance] startup: {}", a_text);
            };
            RunScan(startupOut, { "npcapp", "scan" });

            std::size_t assignments = 0;
            {
                const std::scoped_lock lock{ g_eventMutex };
                assignments = g_sceneAssignments.size();
            }
            if (assignments == 0) {
                REX::WARN("[NpcAppearance] startup found no fully validated winning assignments; persistent mutation remains disabled");
                return;
            }

            RunSceneEvent(startupOut, { "npcapp", "scene", "on" });
            if (!g_sceneRegistered.load(std::memory_order_acquire)) {
                REX::WARN("[NpcAppearance] startup could not register scene lifecycle sinks; persistent mutation remains disabled");
                return;
            }

            g_scenePersistentEnabled.store(true, std::memory_order_release);
            g_startupPersistentArmed.store(true, std::memory_order_release);
            REX::INFO("[NpcAppearance] startup persistent manager ARMED assignments={} packagesRoot={}; no main-menu mutation, waiting for a matched stable loaded-3D generation",
                      assignments, packagesRoot.string());
        }

    }

    void Initialize()
    {
        OnNpcAppearanceDataLoaded();
    }

    void OnFrame()
    {
        // SFSE's rotating worker only requests native BSService queue work.
        // All game-object access remains on the verified drain-owner thread.
        RequestNpcAppearanceNativeFrame();
    }

    void RunCommand(const LineSink& a_out, const std::vector<std::string>& a_args)
    {
        if (a_args.size() < 2 || a_args[1] == "status") {
            RunStatus(a_out);
        } else if (a_args[1] == "selftest") {
            RunSelfTest(a_out);
        } else if (a_args[1] == "scan") {
            RunScan(a_out, a_args);
        } else if (a_args[1] == "inspect") {
            RunInspect(a_out, a_args);
        } else if (a_args[1] == "resolve") {
            RunResolve(a_out, a_args);
        } else if (a_args[1] == "refs") {
            RunRefs(a_out, a_args);
        } else if (a_args[1] == "avm") {
            RunAvmInspect(a_out, a_args);
        } else if (a_args[1] == "donor") {
            RunDonor(a_out, a_args);
        } else if (a_args[1] == "donorseed") {
            RunDonorSeed(a_out, a_args);
        } else if (a_args[1] == "donormorph") {
            RunDonorMorph(a_out, a_args);
        } else if (a_args[1] == "donorvisual") {
            RunDonorVisual(a_out, a_args);
        } else if (a_args[1] == "donorcopy") {
            RunDonorCopy(a_out, a_args);
        } else if (a_args[1] == "targettrial") {
            RunTargetTrial(a_out, a_args, TargetTrialMode::kImmediate);
        } else if (a_args[1] == "targethold") {
            RunTargetTrial(a_out, a_args, TargetTrialMode::kHold);
        } else if (a_args[1] == "targetlatch") {
            RunTargetTrial(a_out, a_args, TargetTrialMode::kRenderLatch);
        } else if (a_args[1] == "targetsnapshot") {
            RunTargetTrial(a_out, a_args, TargetTrialMode::kOwnedSnapshotLatch);
        } else if (a_args[1] == "targetrestore") {
            RunTargetRestore(a_out);
        } else if (a_args[1] == "event") {
            RunEvent(a_out, a_args);
        } else if (a_args[1] == "scene") {
            RunSceneEvent(a_out, a_args);
        } else if (a_args[1] == "copyref") {
            RunCopyRef(a_out, a_args);
        } else {
            a_out("npcapp: status|selftest|scan [packagesRoot]|inspect <npc>|resolve <plugin> <localFormID>|refs <plugin> <localFormID> <npc>|avm <plugin> <localFormID> <npc>|donor [count]|donorseed <plugin> <localFormID> <npc>|donormorph <plugin> <localFormID> <npc>|donorvisual <plugin> <localFormID> <npc>|donorcopy <plugin> <localFormID> <npc>|targettrial <plugin> <localFormID> <actorRefID> <npc>|targethold <plugin> <localFormID> <actorRefID> <npc>|targetlatch <plugin> <localFormID> <actorRefID> <npc>|targetsnapshot <plugin> <localFormID> <actorRefID> <npc>|targetrestore|event <status|on|off>|scene <status|on|off|dispatch <on|off>|auto <on|off>|persistent <on [actorRefID]|off>>|copyref <targetRefID> <sourceRefID> [0|1]");
        }
    }
}
