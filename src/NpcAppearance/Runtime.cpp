#include "NpcAppearance/Runtime.h"

#include "NpcAppearance/Config.h"
#include "NpcAppearance/Preset.h"
#include "NpcAppearance/Resolver.h"
#include "pch.h"

#include "Util/NativeMainThreadQueue.h"
#include "Util/StarfieldRuntime.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
        // Per-line log emitter; production sinks are REX::INFO adapters that
        // carry a context prefix ("startup: ", per-actor base ID, ...).
        using LineSink = std::function<void(const std::string&)>;

        // ==================================================================
        // Native byte contracts
        // Address Library IDs and the expected prologue bytes of every
        // native routine the runtime may call on 1.16.244. Each call site
        // re-verifies these at runtime; any mismatch fails the apply closed.
        // ==================================================================
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
        constexpr std::uintptr_t kProcessListsVtableRva = 0x4CC01B0;
        constexpr std::uintptr_t kActorVtableRva = 0x4CB9248;
        constexpr REL::Offset kNpcOwnedVisualCopyOffset{ 0xCD56E0 };
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
        // TESNPC primary vtable slot 0x17, proven identical on Starfield
        // 1.16.242 and 1.16.244 from the unpacked executable images.
        constexpr std::array<std::uint8_t, 16> kNpcAppearanceChangedGate{
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83,
            0xEC, 0x20, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0x0F
        };

        // ==================================================================
        // Runtime arming state. Assignment maps are published by
        // validation-only scans and consumed only from the verified native
        // BSService queue drain.
        // ==================================================================
        std::atomic<bool>             g_runtimeOperational{ false };
        std::atomic<bool>             g_runtimeArmed{ false };
        std::atomic<bool>             g_mutationKilled{ false };
        std::atomic<bool>             g_saveLoadSinkRegistered{ false };
        std::mutex                    g_eventMutex;
        std::unordered_map<RE::TESFormID, SelectedAssignment> g_sceneAssignments;

        // The SaveLoadEvent sink is observer-only telemetry: it is not
        // load-bearing for mutation safety, so none of these gates consult
        // it. Correctness comes from the per-call byte gates, the verified
        // drain, and the overlay window's restore proof.
        [[nodiscard]] bool MutationOperational() noexcept
        {
            return g_runtimeOperational.load(std::memory_order_acquire) &&
                !g_mutationKilled.load(std::memory_order_acquire);
        }

        [[nodiscard]] bool RestoreOperational() noexcept
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

        [[nodiscard]] bool RequireMutationOperational(
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

        [[nodiscard]] bool RequireRestoreOperational(
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

        // ==================================================================
        // Snapshots
        // Byte-exact captures of base NPC state taken before mutation and
        // compared after, so restore paths can prove original-at-rest.
        // ==================================================================
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

            [[nodiscard]] bool operator==(const OwnedBoneRegionSnapshot&) const = default;
        };

        struct OwnedAvmSnapshot
        {
            RE::AVMData::Type type{ RE::AVMData::Type::kNone };
            std::string       category;
            std::string       name;
            std::string       texturePath;
            RE::Color         color;
            std::uint32_t     intensity{ 0 };

            [[nodiscard]] bool operator==(const OwnedAvmSnapshot&) const = default;
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

        // ==================================================================
        // Preset -> donor population
        // Writes decoded preset data onto a temporary donor NPC through the
        // game's own setters; the donor is later copied onto the target and
        // destroyed.
        // ==================================================================
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

        template <std::size_t N>
        [[nodiscard]] bool HasExpectedBytes(const std::uintptr_t a_address,
                                            const std::array<std::uint8_t, N>& a_expected)
        {
            return Util::IsReadableRange(a_address, a_expected.size()) &&
                   std::memcmp(reinterpret_cast<const void*>(a_address),
                               a_expected.data(), a_expected.size()) == 0;
        }

        using NotifyNpcAppearanceChanged = void (*)(RE::TESNPC*, std::uint32_t);

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
        // Target resolution
        // Owning-plugin + local FormID -> eligible unique HumanRace TESNPC.
        // ==================================================================
        struct LoadedPlugin
        {
            RE::TESFile* file{ nullptr };
            PluginTier tier{ PluginTier::kFull };
            std::uint32_t index{ 0 };
        };

        [[nodiscard]] std::optional<LoadedPlugin> FindLoadedPlugin(
            const std::string_view a_name)
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
                        return LoadedPlugin{
                            file,
                            a_tier,
                            a_tier == PluginTier::kFull ? file->compileIndex : tierIndex
                        };
                    }
                    ++tierIndex;
                }
                return std::nullopt;
            };
            if (auto found = find(
                    handler->compiledFileCollection.files, PluginTier::kFull)) {
                return found;
            }
            if (auto found = find(
                    handler->compiledFileCollection.mediumFiles, PluginTier::kMedium)) {
                return found;
            }
            return find(handler->compiledFileCollection.smallFiles, PluginTier::kSmall);
        }

        [[nodiscard]] std::optional<RE::TESFormID> ResolveRuntimeFormID(
            const Target& a_target)
        {
            const auto plugin = FindLoadedPlugin(a_target.plugin);
            if (!plugin) {
                return std::nullopt;
            }
            return EncodeRuntimeFormID(
                a_target.localFormID, plugin->tier, plugin->index);
        }

        [[nodiscard]] RE::TESNPC* ResolveEligibleTarget(const LineSink& a_out, const Target& a_target)
        {
            const auto runtimeFormID = ResolveRuntimeFormID(a_target);
            if (!runtimeFormID) {
                a_out(std::format(
                    "resolve {}: target plugin absent, tier index invalid, or local FormID exceeds its tier",
                    a_target.CanonicalKey()));
                return nullptr;
            }
            auto* npc = RE::TESForm::LookupByID<RE::TESNPC>(*runtimeFormID);
            if (!npc) {
                a_out(std::format("resolve {} -> 0x{:08X}: not found or not TESNPC",
                                  a_target.CanonicalKey(), *runtimeFormID));
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

        void ReportDependencyResolution(
            const LineSink& a_out,
            const ResolvedAppearanceDependencies& a_result);

        void RunScan(const LineSink& a_out, const std::filesystem::path& a_packsRoot)
        {
            a_out(std::format("scan packsRoot={}", a_packsRoot.string()));
            auto discovery = DiscoverPackages(a_packsRoot, true);
            for (const auto& issue : discovery.issues) {
                a_out(std::format("pack issue code={} path={} @{}: {}",
                                  issue.code, issue.path.string(), issue.offset, issue.message));
            }

            std::size_t decodedPresets = 0;
            std::size_t implicitPacks = 0;
            std::size_t validPacks = 0;
            std::vector<ResolvedAssignment> validatedCandidates;
            for (const auto& package : discovery.packages) {
                if (package.implicitManifest) {
                    ++implicitPacks;
                }
                a_out(std::format("pack '{}' discovery={} priority={} root={} assignments={}",
                                  package.packageID,
                                  package.implicitManifest ? "implicit" : "manifest",
                                  package.priority, package.PackageRoot().string(),
                                  package.assignments.size()));
                bool packageRequirementsComplete = true;
                for (const auto& plugin : package.requirements.plugins) {
                    if (!FindLoadedPlugin(plugin)) {
                        a_out(std::format("pack '{}' rejected: required plugin '{}' is not loaded",
                                          package.packageID, plugin));
                        packageRequirementsComplete = false;
                    }
                }
                const auto packageAssets =
                    CheckRequiredAssets(package.requirements, DefaultDataDirectory());
                if (!packageAssets.Complete()) {
                    packageRequirementsComplete = false;
                    for (const auto& asset : packageAssets.missing) {
                        a_out(std::format("pack '{}' rejected: required Data asset '{}' is missing or is not a regular file",
                                          package.packageID, asset.generic_string()));
                    }
                }
                if (!packageRequirementsComplete) {
                    continue;
                }

                struct TargetResolution
                {
                    const Assignment* assignment{ nullptr };
                    RE::TESNPC* npc{ nullptr };
                };
                std::vector<TargetResolution> targetResolutions;
                targetResolutions.reserve(package.assignments.size());
                std::unordered_map<RE::TESFormID, std::string> resolvedTargets;
                bool duplicateResolvedTarget = false;
                for (const auto& assignment : package.assignments) {
                    auto* npc = ResolveEligibleTarget(a_out, assignment.target);
                    if (npc) {
                        const auto [found, inserted] = resolvedTargets.emplace(
                            npc->GetFormID(), assignment.target.CanonicalKey());
                        if (!inserted) {
                            duplicateResolvedTarget = true;
                            a_out(std::format(
                                "pack '{}' rejected: code=package_rejected_duplicate_resolved_target targets {} and {} both resolve to base 0x{:08X}",
                                package.packageID, found->second,
                                assignment.target.CanonicalKey(), npc->GetFormID()));
                        }
                    }
                    targetResolutions.push_back({ &assignment, npc });
                }
                if (duplicateResolvedTarget) {
                    continue;
                }

                bool packageHasValidCandidate = false;
                for (const auto& resolution : targetResolutions) {
                    const auto& assignment = *resolution.assignment;
                    bool assignmentRequirementsComplete = true;
                    for (const auto& plugin : assignment.requirements.plugins) {
                        if (!FindLoadedPlugin(plugin)) {
                            a_out(std::format("candidate pack='{}' target={} rejected: required plugin '{}' is not loaded",
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
                            a_out(std::format("candidate pack='{}' target={} rejected: required Data asset '{}' is missing or is not a regular file",
                                              package.packageID,
                                              assignment.target.CanonicalKey(),
                                              asset.generic_string()));
                        }
                    }
                    if (!assignmentRequirementsComplete) {
                        continue;
                    }

                    auto* npc = resolution.npc;
                    if (!npc) {
                        a_out(std::format("candidate pack='{}' target={} rejected: target is absent or ineligible",
                                          package.packageID,
                                          assignment.target.CanonicalKey()));
                        continue;
                    }

                    const auto decoded = LoadCkPreset(assignment.presetPath);
                    if (!decoded.preset) {
                        a_out(std::format("candidate pack='{}' target={} rejected: preset '{}' does not satisfy a supported 1.16.244 producer contract",
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

                    const auto resolved = ResolveAppearanceDependencies(*decoded.preset, npc);
                    ReportDependencyResolution(a_out, resolved);
                    if (!resolved.Complete()) {
                        a_out(std::format("candidate pack='{}' target={} rejected: preset appearance references are incomplete",
                                          package.packageID,
                                          assignment.target.CanonicalKey()));
                        continue;
                    }
                    validatedCandidates.push_back({
                        npc->GetFormID(),
                        SelectedAssignment{
                            assignment.target,
                            assignment.presetPath,
                            package.packageID,
                            package.priority
                        }
                    });
                    packageHasValidCandidate = true;
                }
                if (packageHasValidCandidate) {
                    ++validPacks;
                }
            }

            const auto selection = SelectResolvedAssignments(validatedCandidates);
            for (const auto& decision : selection.decisions) {
                a_out(std::format("conflict target={} pack='{}' priority={} result={} reason={}",
                                  decision.targetKey, decision.packageID, decision.priority,
                                  decision.winner ? "winner" : "loser", decision.reason));
            }

            std::unordered_set<RE::TESFormID> resolvedBaseIDs;
            std::unordered_map<RE::TESFormID, SelectedAssignment> resolvedAssignments;
            for (const auto& resolvedWinner : selection.winners) {
                const auto& assignment = resolvedWinner.assignment;
                auto* npc = ResolveEligibleTarget(a_out, assignment.target);
                if (npc && npc->GetFormID() == resolvedWinner.baseFormID) {
                    resolvedBaseIDs.insert(resolvedWinner.baseFormID);
                    resolvedAssignments.emplace(resolvedWinner.baseFormID, assignment);
                } else if (npc) {
                    a_out(std::format(
                        "  winner pack='{}' target={} changed resolved base from 0x{:08X} to 0x{:08X}; rejected fail-closed",
                        assignment.packageID, assignment.target.CanonicalKey(),
                        resolvedWinner.baseFormID, npc->GetFormID()));
                }
                a_out(std::format("  winner pack='{}' priority={} target={} preset={} targetResolved={}",
                                  assignment.packageID, assignment.priority,
                                  assignment.target.CanonicalKey(), assignment.presetPath.string(),
                                  static_cast<void*>(npc)));
            }
            const auto resolvedCount = resolvedBaseIDs.size();
            {
                const std::scoped_lock lock{ g_eventMutex };
                g_sceneAssignments = std::move(resolvedAssignments);
            }
            a_out(std::format("scan: discoveredPacks={} implicitPacks={} validPacks={} decodedPresets={} validCandidates={} winners={} resolvedTargets={}; validation only, owned population/application gate prevents mutation",
                              discovery.packages.size(), implicitPacks, validPacks,
                              decodedPresets, validatedCandidates.size(), selection.winners.size(),
                              resolvedCount));
        }

        void ReportDependencyResolution(
            const LineSink& a_out,
            const ResolvedAppearanceDependencies& a_result)
        {
            a_out(std::format(
                "refs: forms={} bones={} shapes={} colors={} avm={} complete={} race={} uniqueSlots={} misc={} issues={}",
                a_result.formReferencesComplete, a_result.boneReferencesComplete,
                a_result.shapeReferencesComplete, a_result.colorReferencesComplete,
                a_result.avmReferencesComplete,
                a_result.Complete(),
                static_cast<void*>(a_result.race), a_result.uniqueHeadParts.size(),
                a_result.miscHeadParts.size(),
                a_result.issues.size()));
            for (const auto& issue : a_result.issues) {
                a_out(std::format("  dependency issue code={} field={} value='{}': {}",
                                  issue.code, issue.field, issue.value, issue.message));
            }
        }

        using CreateNpc = RE::TESNPC* (*)(void*, bool);
        using CopyNpcAppearance = void (*)(RE::TESNPC*, RE::TESNPC*, bool);

        struct NpcDonorDeleter
        {
            DestroyNpc destroy{ nullptr };

            void operator()(RE::TESNPC* a_donor) const noexcept
            {
                if (a_donor && destroy) {
                    static_cast<void>(destroy(a_donor, 1));
                }
            }
        };

        using ScopedNpcDonor = std::unique_ptr<RE::TESNPC, NpcDonorDeleter>;

        struct SnapshotDonorFunctions
        {
            std::uintptr_t          factoryAddress{ 0 };
            std::uintptr_t          npcVtable{ 0 };
            CreateNpc               create{ nullptr };
            DestroyNpc              destroy{ nullptr };
            SetShapeBlend           setShape{ nullptr };
            SetBodyMorph            setBody{ nullptr };
            SetFacialBone           setBone{ nullptr };
            EnsureFacialBoneGroup   ensureBoneGroup{ nullptr };
            ChangeHeadPart          changeHeadPart{ nullptr };
            SetAvmData              setAvmData{ nullptr };
            OwnedVisualCopy         ownedCopy{ nullptr };
        };

        struct BuiltSnapshotDonor
        {
            ScopedNpcDonor donor;
            RE::TESFormID  formID{ 0 };
        };

        [[nodiscard]] std::optional<SnapshotDonorFunctions>
        ResolveSnapshotDonorFunctions(const LineSink& a_out)
        {
            const auto factoryAddress =
                REL::Relocation<std::uintptr_t>{ kNpcFactorySingletonID }.address();
            const auto factoryVtable =
                REL::Relocation<std::uintptr_t>{ kNpcFactoryVtableID }.address();
            const auto createAddress =
                REL::Relocation<std::uintptr_t>{ kNpcFactoryCreateID }.address();
            const auto npcVtable =
                REL::Relocation<std::uintptr_t>{ kNpcPrimaryVtableID }.address();
            const auto destructorAddress =
                REL::Relocation<std::uintptr_t>{ kNpcScalarDeletingDestructorID }.address();
            const auto shapeAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetShapeBlendID }.address();
            const auto bodyAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetBodyMorphID }.address();
            const auto boneAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetBoneValueID }.address();
            const auto boneGroupAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetBoneGroupValueID }.address();
            const auto changeHeadAddress =
                REL::Relocation<std::uintptr_t>{ kNpcChangeHeadPartID }.address();
            const auto setAvmAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetAvmDataID }.address();
            const auto ownedCopyAddress = kNpcOwnedVisualCopyOffset.address();

            if (!Util::IsReadableRange(factoryAddress, sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(factoryAddress) != factoryVtable ||
                !Util::IsReadableRange(
                    factoryVtable + sizeof(std::uintptr_t), sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(
                    factoryVtable + sizeof(std::uintptr_t)) != createAddress ||
                !HasExpectedBytes(createAddress, kNpcFactoryCreateGate) ||
                !HasExpectedBytes(destructorAddress, kNpcDestructorGate) ||
                !HasExpectedBytes(shapeAddress, kNpcSetShapeBlendGate) ||
                !HasExpectedBytes(bodyAddress, kNpcSetBodyMorphGate) ||
                !HasExpectedBytes(boneAddress, kNpcSetBoneValueGate) ||
                !HasExpectedBytes(boneGroupAddress, kNpcSetBoneGroupValueGate) ||
                !HasExpectedBytes(changeHeadAddress, kNpcChangeHeadPartGate) ||
                !HasExpectedBytes(setAvmAddress, kNpcSetAvmDataGate) ||
                !HasExpectedBytes(ownedCopyAddress, kNpcOwnedVisualCopyGate)) {
                KillMutation("owned snapshot restore byte gate failed");
                a_out("ownedrestore: donor/setter/copy/destructor contract mismatch; FAIL CLOSED");
                return std::nullopt;
            }

            return SnapshotDonorFunctions{
                .factoryAddress = factoryAddress,
                .npcVtable = npcVtable,
                .create = reinterpret_cast<CreateNpc>(createAddress),
                .destroy = reinterpret_cast<DestroyNpc>(destructorAddress),
                .setShape = reinterpret_cast<SetShapeBlend>(shapeAddress),
                .setBody = reinterpret_cast<SetBodyMorph>(bodyAddress),
                .setBone = reinterpret_cast<SetFacialBone>(boneAddress),
                .ensureBoneGroup =
                    reinterpret_cast<EnsureFacialBoneGroup>(boneGroupAddress),
                .changeHeadPart = reinterpret_cast<ChangeHeadPart>(changeHeadAddress),
                .setAvmData = reinterpret_cast<SetAvmData>(setAvmAddress),
                .ownedCopy = reinterpret_cast<OwnedVisualCopy>(ownedCopyAddress),
            };
        }

        [[nodiscard]] std::optional<BuiltSnapshotDonor> BuildOwnedVisualSnapshotDonor(
            const LineSink& a_out,
            const OwnedVisualSnapshot& a_snapshot,
            const SnapshotDonorFunctions& a_functions)
        {
            auto* donor = a_functions.create(
                reinterpret_cast<void*>(a_functions.factoryAddress), false);
            if (!donor) {
                a_out("ownedrestore: Create(false) returned null; no target mutation");
                return std::nullopt;
            }

            const auto donorFormID = donor->GetFormID();
            ScopedNpcDonor donorOwner{ donor, NpcDonorDeleter{ a_functions.destroy } };
            std::size_t headPartCount = 0;
            {
                auto headParts = donor->headParts.Lock();
                headPartCount = (*headParts).size();
            }
            const bool initialized =
                *reinterpret_cast<const std::uintptr_t*>(donor) == a_functions.npcVtable &&
                donorFormID != 0 &&
                RE::TESForm::LookupByID<RE::TESNPC>(donorFormID) == donor &&
                donor->QRefCount() == 0 && donor->GetRace() == nullptr &&
                donor->faceNPC == nullptr && headPartCount == 0 &&
                donor->unk3D8 == nullptr && donor->unk3E0 == nullptr &&
                donor->unk3E8 == nullptr && donor->tintAVMData.empty() &&
                donor->shapeBlendData == nullptr && donor->pronoun.underlying() == 0;
            if (!initialized) {
                a_out("ownedrestore: donor failed registered-empty invariants; no target mutation");
                return std::nullopt;
            }

            donor->morphWeight.thin = a_snapshot.thin;
            donor->morphWeight.muscular = a_snapshot.muscular;
            donor->morphWeight.fat = a_snapshot.fat;
            if (a_snapshot.hasBodyMorphRegions) {
                for (std::size_t i = 0; i < a_snapshot.bodyMorphRegions.size(); ++i) {
                    a_functions.setBody(
                        donor, static_cast<std::uint32_t>(i),
                        a_snapshot.bodyMorphRegions[i]);
                }
            }
            if (a_snapshot.hasBoneValues) {
                for (const auto& [key, value] : a_snapshot.boneValues) {
                    a_functions.setBone(donor, key, value);
                }
            }
            if (a_snapshot.hasBoneRegions) {
                for (const auto& region : a_snapshot.boneRegions) {
                    if (!region.hasValues) {
                        continue;
                    }
                    for (const auto& [key, value] : region.values) {
                        const RE::BSFixedStringCS fixedKey{ key.c_str() };
                        a_functions.ensureBoneGroup(donor, region.regionID, &fixedKey);
                        if (!donor->unk3E8) {
                            continue;
                        }
                        const auto outer = donor->unk3E8->find(region.regionID);
                        if (outer == donor->unk3E8->end() || !outer->value) {
                            continue;
                        }
                        for (auto& entry : *outer->value) {
                            if (std::string_view{ SafeText(entry.key.c_str()) } == key) {
                                entry.value = value;
                                break;
                            }
                        }
                    }
                }
            }
            if (a_snapshot.hasShapeBlends) {
                for (const auto& [key, value] : a_snapshot.shapeBlends) {
                    const RE::BSFixedStringCS fixedKey{ key.c_str() };
                    a_functions.setShape(donor, &fixedKey, value);
                }
            }
            for (const auto formID : a_snapshot.headPartFormIDs) {
                auto* part = RE::TESForm::LookupByID<RE::BGSHeadPart>(formID);
                if (!part) {
                    a_out(std::format(
                        "ownedrestore: headpart 0x{:08X} did not resolve; no target mutation",
                        formID));
                    return std::nullopt;
                }
                a_functions.changeHeadPart(donor, part);
            }

            donor->skinToneIndex = a_snapshot.skinToneIndex;
            donor->pronoun = static_cast<RE::TESNPC::PRONOUN_TYPE>(a_snapshot.pronoun);
            donor->teeth = a_snapshot.teeth;
            donor->jewelryColor = a_snapshot.jewelryColor;
            donor->eyeColor = a_snapshot.eyeColor;
            donor->hairColor = a_snapshot.hairColor;
            donor->facialColor = a_snapshot.facialColor;
            donor->eyebrowColor = a_snapshot.eyebrowColor;
            for (const auto& avm : a_snapshot.avms) {
                RE::AVMData materialized{};
                materialized.type = avm.type;
                materialized.category = RE::BSFixedString{ avm.category.c_str() };
                materialized.unk10.name = RE::BSFixedString{ avm.name.c_str() };
                materialized.unk10.texturePath =
                    RE::BSFixedString{ avm.texturePath.c_str() };
                materialized.unk10.color = avm.color;
                materialized.unk10.intensity = avm.intensity;
                a_functions.setAvmData(donor, &materialized);
            }

            if (!SameExactVisualValues(donor, a_snapshot)) {
                a_out("ownedrestore: constructed donor did not exactly match snapshot; no target mutation");
                return std::nullopt;
            }
            return BuiltSnapshotDonor{
                .donor = std::move(donorOwner),
                .formID = donorFormID,
            };
        }

        [[nodiscard]] bool RestoreOwnedVisualSnapshot(
            const LineSink& a_out,
            RE::TESNPC* a_target,
            const OwnedVisualSnapshot& a_snapshot,
            RE::TESNPC* a_originalFaceNPC)
        {
            if (!RequireRestoreOperational(a_out, "ownedrestore") || !a_target) {
                if (!a_target) {
                    a_out("ownedrestore: target is null; no mutation");
                }
                return false;
            }

            const auto functions = ResolveSnapshotDonorFunctions(a_out);
            if (!functions) {
                return false;
            }
            auto built = BuildOwnedVisualSnapshotDonor(a_out, a_snapshot, *functions);
            if (!built) {
                return false;
            }

            a_target->morphWeight.thin = a_snapshot.thin;
            a_target->morphWeight.muscular = a_snapshot.muscular;
            a_target->morphWeight.fat = a_snapshot.fat;
            if (a_snapshot.hasBodyMorphRegions) {
                for (std::size_t i = 0; i < a_snapshot.bodyMorphRegions.size(); ++i) {
                    functions->setBody(
                        a_target, static_cast<std::uint32_t>(i),
                        a_snapshot.bodyMorphRegions[i]);
                }
            }
            a_target->skinToneIndex = a_snapshot.skinToneIndex;
            functions->ownedCopy(a_target, built->donor.get(), false);
            a_target->faceNPC = a_originalFaceNPC;
            const bool targetExact = SameExactVisualValues(a_target, a_snapshot) &&
                a_target->faceNPC == a_originalFaceNPC;

            const auto donorFormID = built->formID;
            built->donor.reset();
            const bool donorUnregistered =
                RE::TESForm::LookupByID<RE::TESNPC>(donorFormID) == nullptr;
            a_out(std::format(
                "ownedrestore: donorExact=true targetExact={} donorUnregistered={}",
                targetExact, donorUnregistered));
            return targetExact && donorUnregistered;
        }

        // Proven production apply pipeline. This mutates only the TESNPC base:
        // no notifications, actor refresh, or retained donors.
        [[nodiscard]] bool SilentApplyPresetToBase(
            const LineSink& a_out,
            RE::TESNPC* a_target,
            const std::filesystem::path& a_path)
        {
            if (!RequireMutationOperational(a_out, "silentapply") || !a_target) {
                if (!a_target) {
                    a_out("silentapply: target is null; no mutation");
                }
                return false;
            }

            const auto decoded = LoadCkPreset(a_path);
            if (!decoded.preset) {
                a_out(std::format(
                    "silentapply: preset rejected path={} issues={}",
                    a_path.string(), decoded.issues.size()));
                return false;
            }
            const auto resolved = ResolveAppearanceDependencies(*decoded.preset, a_target);
            if (!resolved.Complete()) {
                a_out("silentapply: dependency resolution incomplete; no mutation");
                return false;
            }

            const auto factoryAddress =
                REL::Relocation<std::uintptr_t>{ kNpcFactorySingletonID }.address();
            const auto factoryVtable =
                REL::Relocation<std::uintptr_t>{ kNpcFactoryVtableID }.address();
            const auto createAddress =
                REL::Relocation<std::uintptr_t>{ kNpcFactoryCreateID }.address();
            const auto npcVtable =
                REL::Relocation<std::uintptr_t>{ kNpcPrimaryVtableID }.address();
            const auto destructorAddress =
                REL::Relocation<std::uintptr_t>{ kNpcScalarDeletingDestructorID }.address();
            const auto copyAddress =
                REL::Relocation<std::uintptr_t>{ kNpcCopyAppearanceID }.address();
            const auto shapeAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetShapeBlendID }.address();
            const auto bodyAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetBodyMorphID }.address();
            const auto boneAddress =
                REL::Relocation<std::uintptr_t>{ kNpcSetBoneValueID }.address();
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
                !Util::IsReadableRange(
                    factoryVtable + sizeof(std::uintptr_t), sizeof(std::uintptr_t)) ||
                *reinterpret_cast<const std::uintptr_t*>(
                    factoryVtable + sizeof(std::uintptr_t)) != createAddress ||
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
                KillMutation("silent preset apply byte gate failed");
                a_out("silentapply: population/copy/destructor contract mismatch; FAIL CLOSED");
                return false;
            }

            const auto resolveEntry =
                reinterpret_cast<ResolveFaceDbEntry>(resolveEntryAddress);
            std::vector<MaterializedAvmLayer> expectedAvms;
            if (!MaterializeAvmLayers(
                    a_out, *decoded.preset, resolveEntry, expectedAvms)) {
                a_out("silentapply: AVM materialization incomplete; no mutation");
                return false;
            }

            const auto create = reinterpret_cast<CreateNpc>(createAddress);
            const auto copy = reinterpret_cast<CopyNpcAppearance>(copyAddress);
            const auto ownedCopy = reinterpret_cast<OwnedVisualCopy>(ownedCopyAddress);
            const auto setShape = reinterpret_cast<SetShapeBlend>(shapeAddress);
            const auto setBody = reinterpret_cast<SetBodyMorph>(bodyAddress);
            const auto setBone = reinterpret_cast<SetFacialBone>(boneAddress);
            const auto ensureBoneGroup =
                reinterpret_cast<EnsureFacialBoneGroup>(boneGroupAddress);
            const auto removeHeadPart =
                reinterpret_cast<RemoveHeadPart>(removeHeadAddress);
            const auto changeHeadPart =
                reinterpret_cast<ChangeHeadPart>(changeHeadAddress);
            const auto setAvmData = reinterpret_cast<SetAvmData>(setAvmAddress);
            const auto removeAvmData =
                reinterpret_cast<RemoveAvmData>(removeAvmAddress);
            const auto destroy = reinterpret_cast<DestroyNpc>(destructorAddress);

            const auto targetNonVisualBefore = Snapshot(a_target);
            const auto targetVisualBefore = SnapshotVisualSeed(a_target);
            const auto originalActorFlags =
                a_target->actorData.actorBaseFlags.underlying();
            auto* const originalFaceNPC = a_target->faceNPC;

            ScopedNpcDonor backupDonor{
                create(reinterpret_cast<void*>(factoryAddress), false),
                NpcDonorDeleter{ destroy }
            };
            ScopedNpcDonor presetDonor{
                create(reinterpret_cast<void*>(factoryAddress), false),
                NpcDonorDeleter{ destroy }
            };
            if (!backupDonor || !presetDonor) {
                a_out("silentapply: failed to create the donor pair; no mutation");
                return false;
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
            if (!initialized(backupDonor.get(), backupFormID) ||
                !initialized(presetDonor.get(), presetFormID)) {
                a_out("silentapply: donor pair failed registered-empty invariants; no mutation");
                return false;
            }

            copy(backupDonor.get(), a_target, false);
            copy(presetDonor.get(), a_target, false);
            PopulatePresetMorphs(
                presetDonor.get(), *decoded.preset, setShape, setBody,
                setBone, ensureBoneGroup);
            PopulatePresetVisuals(
                presetDonor.get(), *decoded.preset, resolved, expectedAvms,
                removeHeadPart, changeHeadPart, setAvmData, removeAvmData);

            const bool backupExact =
                SameExactVisualValues(backupDonor.get(), a_target);
            const bool backupIndependent = HasIndependentVisualStorage(
                targetVisualBefore, SnapshotVisualSeed(backupDonor.get()));
            const bool presetMorphsValid = ValidateDonorMorphPopulation(
                a_out, presetDonor.get(), *decoded.preset);
            const bool presetVisualsValid = ValidateDonorVisualPopulation(
                a_out, presetDonor.get(), *decoded.preset, resolved, expectedAvms);
            const bool bodyCompatible = backupDonor->unk3D8 &&
                backupDonor->unk3D8->size() ==
                    decoded.preset->bodyMorphRegionValues.size();
            if (!backupExact || !backupIndependent || !presetMorphsValid ||
                !presetVisualsValid || !bodyCompatible ||
                targetNonVisualBefore != Snapshot(a_target) ||
                targetVisualBefore != SnapshotVisualSeed(a_target)) {
                a_out(std::format(
                    "silentapply: preflight failed backupExact={} backupIndependent={} presetMorphsValid={} presetVisualsValid={} bodyCompatible={}; no mutation",
                    backupExact, backupIndependent, presetMorphsValid,
                    presetVisualsValid, bodyCompatible));
                return false;
            }

            presetDonor->faceNPC = nullptr;
            a_target->morphWeight.thin =
                static_cast<float>(decoded.preset->morphWeights.x);
            a_target->morphWeight.muscular =
                static_cast<float>(decoded.preset->morphWeights.y);
            a_target->morphWeight.fat =
                static_cast<float>(decoded.preset->morphWeights.z);
            for (std::size_t i = 0;
                 i < decoded.preset->bodyMorphRegionValues.size(); ++i) {
                setBody(
                    a_target, static_cast<std::uint32_t>(i),
                    static_cast<float>(decoded.preset->bodyMorphRegionValues[i]));
            }
            a_target->skinToneIndex =
                static_cast<std::uint8_t>(decoded.preset->skinTone);
            ownedCopy(a_target, presetDonor.get(), false);

            const bool targetMorphsValid = ValidateDonorMorphPopulation(
                a_out, a_target, *decoded.preset);
            const bool targetVisualsValid = ValidateDonorVisualPopulation(
                a_out, a_target, *decoded.preset, resolved, expectedAvms);
            const bool targetMatchesPreset =
                SameExactVisualValues(a_target, presetDonor.get());
            const bool targetStorageIndependent = HasIndependentVisualStorage(
                SnapshotVisualSeed(presetDonor.get()), SnapshotVisualSeed(a_target));
            const bool targetNonVisualPreserved =
                targetNonVisualBefore == Snapshot(a_target);
            const bool applied = targetMorphsValid && targetVisualsValid &&
                targetMatchesPreset && targetStorageIndependent &&
                targetNonVisualPreserved && a_target->faceNPC == nullptr;

            if (!applied) {
                a_target->morphWeight = backupDonor->morphWeight;
                for (std::uint32_t i = 0; i < backupDonor->unk3D8->size(); ++i) {
                    setBody(a_target, i, (*backupDonor->unk3D8)[i]);
                }
                a_target->skinToneIndex = backupDonor->skinToneIndex;
                ownedCopy(a_target, backupDonor.get(), false);
                a_target->faceNPC = originalFaceNPC;
                a_target->actorData.actorBaseFlags =
                    static_cast<RE::ACTOR_BASE_DATA::Flag>(originalActorFlags);
                const bool rolledBack =
                    SameExactVisualValues(a_target, backupDonor.get()) &&
                    a_target->faceNPC == originalFaceNPC &&
                    targetNonVisualBefore == Snapshot(a_target);
                a_out(std::format(
                    "silentapply: post-validate failed morphs={} visuals={} matchesPreset={} storageIndependent={} nonVisualPreserved={} faceNPCCleared={}; rolledBack={}",
                    targetMorphsValid, targetVisualsValid, targetMatchesPreset,
                    targetStorageIndependent, targetNonVisualPreserved,
                    a_target->faceNPC == nullptr, rolledBack));
                if (!rolledBack) {
                    KillMutation("silent preset apply rollback failed");
                }
            }

            presetDonor.reset();
            backupDonor.reset();
            const bool donorsUnregistered =
                RE::TESForm::LookupByID<RE::TESNPC>(presetFormID) == nullptr &&
                RE::TESForm::LookupByID<RE::TESNPC>(backupFormID) == nullptr;
            a_out(std::format(
                "silentapply: applied={} morphsValid={} visualsValid={} matchesPreset={} storageIndependent={} nonVisualPreserved={} donorsUnregistered={} faceNPCCleared={}",
                applied, targetMorphsValid, targetVisualsValid, targetMatchesPreset,
                targetStorageIndependent, targetNonVisualPreserved,
                donorsUnregistered, applied && a_target->faceNPC == nullptr));
            if (!donorsUnregistered) {
                KillMutation("silent preset donor teardown failed");
            }
            return applied && donorsUnregistered;
        }

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
            RunScan(startupOut, packsRoot);

            std::size_t assignments = 0;
            {
                const std::scoped_lock lock{ g_eventMutex };
                assignments = g_sceneAssignments.size();
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
                KillMutation(
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
                        KillMutation("startup scan payload was dropped by the native queue");
                        REX::CRITICAL(
                            "[NpcAppearance] startup scan payload was dropped before verified native execution; mutation remains fail closed");
                    })) {
                KillMutation("startup scan could not enter the verified native queue");
            }
        } catch (const std::exception& e) {
            KillMutation("initialization threw");
            REX::CRITICAL(
                "[NpcAppearance] initialization threw '{}'; mutation stays fail closed",
                e.what());
        } catch (...) {
            KillMutation("initialization threw");
            REX::CRITICAL(
                "[NpcAppearance] initialization threw an unknown exception; mutation stays fail closed");
        }
    }

}
