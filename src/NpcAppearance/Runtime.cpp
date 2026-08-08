#include "NpcAppearance/Runtime.h"

#include "NpcAppearance/Config.h"
#include "NpcAppearance/Preset.h"
#include "NpcAppearance/Resolver.h"
#include "NpcAppearance/SaveLoadHooks.h"
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
        // ==================================================================
        // Native byte contracts
        // Address Library IDs and the expected prologue bytes of every
        // native routine the runtime may call on 1.16.244. Each call site
        // re-verifies these at runtime; any mismatch fails the apply closed.
        // ==================================================================
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
        constexpr std::uintptr_t kProcessListsVtableRva = 0x4CC01B0;
        constexpr std::uintptr_t kActorVtableRva = 0x4CB9248;
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
        // TESNPC primary vtable slot 0x17, proven identical on Starfield
        // 1.16.242 and 1.16.244 from the unpacked executable images.
        constexpr std::array<std::uint8_t, 16> kNpcAppearanceChangedGate{
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83,
            0xEC, 0x20, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0x0F
        };

        // ==================================================================
        // Production save/load bracket state. Assignment maps are published by
        // validation-only scans and consumed only from the verified native
        // BSService queue drain.
        // ==================================================================
        std::atomic<bool>             g_bracketOperational{ false };
        std::atomic<bool>             g_bracketArmed{ false };
        std::atomic<bool>             g_mutationKilled{ false };
        std::atomic<bool>             g_inBracket{ false };
        std::mutex                    g_eventMutex;
        std::unordered_set<RE::TESFormID> g_targetBaseIDs;
        std::unordered_map<RE::TESFormID, SelectedAssignment> g_sceneAssignments;

        void RunTargetTrial(
            const LineSink& a_out, const std::vector<std::string>& a_args);

        [[nodiscard]] bool MutationOperational() noexcept
        {
            return g_bracketOperational.load(std::memory_order_acquire) &&
                !g_mutationKilled.load(std::memory_order_acquire) &&
                SaveLoadHooks::Operational();
        }

        [[nodiscard]] bool RestoreOperational() noexcept
        {
            return g_bracketOperational.load(std::memory_order_acquire);
        }

        [[nodiscard]] bool SaveGatewayOperational() noexcept
        {
            return g_bracketOperational.load(std::memory_order_acquire) &&
                SaveLoadHooks::Operational();
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
            if (g_bracketOperational.load(std::memory_order_acquire) &&
                !g_mutationKilled.load(std::memory_order_acquire) &&
                !SaveLoadHooks::Operational()) {
                KillMutation("save/load hook provider lost ownership");
            }
            a_out(std::format(
                "{}: mutation disabled bracketOperational={} mutationKilled={}; FAIL CLOSED",
                a_operation,
                g_bracketOperational.load(std::memory_order_relaxed),
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
                "{}: restore disabled because the production bracket was never operational; FAIL CLOSED",
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

        [[nodiscard]] std::optional<Target> ParseTargetToken(
            const std::string_view a_text)
        {
            const auto separator = a_text.rfind(':');
            if (separator == std::string_view::npos || separator == 0 ||
                separator + 1 >= a_text.size()) {
                return std::nullopt;
            }
            const std::string plugin{ a_text.substr(0, separator) };
            const auto extension = std::filesystem::path{ plugin }.extension().string();
            if (plugin.size() <= 4 || plugin.size() > 260 || plugin.contains('/') ||
                plugin.contains('\\') || plugin.contains(':') ||
                (::_stricmp(extension.c_str(), ".esm") != 0 &&
                 ::_stricmp(extension.c_str(), ".esp") != 0 &&
                 ::_stricmp(extension.c_str(), ".esl") != 0)) {
                return std::nullopt;
            }
            const auto localText = a_text.substr(separator + 1);
            if (localText.size() > 8 || localText.starts_with("0x") ||
                localText.starts_with("0X")) {
                return std::nullopt;
            }
            const auto localFormID = ParseFormID(localText);
            if (!localFormID || *localFormID > 0x00FFFFFF) {
                return std::nullopt;
            }
            return Target{
                plugin, *localFormID };
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
        // Production save/load bracket tracking
        // ==================================================================
        struct AppliedBaseState
        {
            RE::TESFormID       baseID{ 0 };
            SelectedAssignment assignment;
            OwnedVisualSnapshot originalVisual;
            NonVisualSnapshot   originalNonVisual;
            RE::TESNPC*         originalFaceNPC{ nullptr };
            std::uint32_t       originalActorFlags{ 0 };
            bool                bracketFailed{ false };
        };

        std::mutex                                      g_appliedBasesMutex;
        std::unordered_map<RE::TESFormID, AppliedBaseState> g_appliedBases;
        std::unordered_set<RE::TESFormID>               g_saveEntryRestoredBases;
        std::atomic<std::uint64_t>                      g_bracketSaveEntries{ 0 };
        std::atomic<std::uint64_t>                      g_bracketSaveReturns{ 0 };
        std::atomic<std::uint64_t>                      g_bracketLoadReturns{ 0 };
        std::atomic<std::uint64_t>                      g_bracketLoadGeneration{ 0 };
        std::atomic<std::uint64_t>                      g_preSaveGeneration{ 0 };
        std::atomic<bool>                               g_preSaveReady{ false };
        std::atomic<bool>                               g_saveGatewayEntered{ false };
        std::atomic<bool>                               g_saveHookObserved{ false };
        std::atomic<bool>                               g_saveLoadEventRegistered{ false };
        constexpr std::uint32_t                         kC2LoadReadyMaxNativeFrames = 600;
        struct DeferredC2LoadTask
        {
            std::uint64_t                    generation{ 0 };
            std::function<bool(std::uint32_t)> run;
            std::uint32_t                   attempts{ 0 };
            bool                            deferralLogged{ false };
        };
        std::mutex                                      g_deferredC2LoadMutex;
        std::shared_ptr<DeferredC2LoadTask>             g_deferredC2LoadTask;
        std::shared_ptr<DeferredC2LoadTask>             g_deferredC2LoadInFlight;
        struct DeferredC2SaveTask
        {
            std::uint64_t       sequence{ 0 };
            std::uint64_t       loadGeneration{ 0 };
            std::function<void()> run;
            bool                deferralLogged{ false };
        };
        std::mutex                                      g_deferredC2SaveMutex;
        std::shared_ptr<DeferredC2SaveTask>             g_deferredC2SaveTask;
        std::shared_ptr<DeferredC2SaveTask>             g_deferredC2SaveInFlight;
        std::atomic<bool>                               g_deferredC2RetryScheduled{ false };
        constexpr std::uint32_t                         kDeferredC2RetryMaxWaits = 400;
        constexpr std::chrono::milliseconds             kDeferredC2RetryDelay{ 25 };

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
                if (!MutationOperational()) {
                    KillMutation("save/load hook provider lost ownership before notify/kick");
                }
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

        [[nodiscard]] RE::TESNPC* ResolveTargetArgument(
            const LineSink& a_out,
            const std::string_view a_text)
        {
            const auto target = ParseTargetToken(a_text);
            if (!target) {
                a_out("invalid target; expected <plugin:localFormID> with a 1-8 digit plugin-local hexadecimal FormID and no 0x prefix");
                return nullptr;
            }
            return ResolveEligibleTarget(a_out, *target);
        }

        // ==================================================================
        // Diagnostic command surface (npcapp ...)
        // Unbound in release builds; mirrors the pipeline stages so each
        // layer can be exercised and inspected in isolation.
        // ==================================================================
        void RunStatus(const LineSink& a_out)
        {
            const auto root = DefaultPluginDirectory();
            std::size_t appliedBases = 0;
            std::size_t failedBases = 0;
            {
                const std::scoped_lock lock{ g_appliedBasesMutex };
                appliedBases = g_appliedBases.size();
                failedBases = static_cast<std::size_t>(std::ranges::count_if(
                    g_appliedBases, [](const auto& a_entry) {
                        return a_entry.second.bracketFailed;
                    }));
            }
            a_out("OSF Identity diagnostics: production save/load bracket + retained pipeline commands");
            a_out(std::format("saveLoadBracketOperational={} saveVetoSupported={} mutationKilled={}",
                              g_bracketOperational.load(std::memory_order_relaxed),
                              SaveLoadHooks::SupportsSaveVeto(),
                              g_mutationKilled.load(std::memory_order_relaxed)));
            a_out(std::format(
                "saveLoadBracketArmed={} autoArm=true appliedBases={} failedBases={} inBracket={} preSaveReady={} saveGatewayEntered={} saveHookObserved={} saveLoadEventRegistered={} saveEntries={} saveReturns={} loadReturns={} loadGeneration={}",
                g_bracketArmed.load(std::memory_order_relaxed),
                appliedBases, failedBases,
                g_inBracket.load(std::memory_order_relaxed),
                g_preSaveReady.load(std::memory_order_relaxed),
                g_saveGatewayEntered.load(std::memory_order_relaxed),
                g_saveHookObserved.load(std::memory_order_relaxed),
                g_saveLoadEventRegistered.load(std::memory_order_relaxed),
                g_bracketSaveEntries.load(std::memory_order_relaxed),
                g_bracketSaveReturns.load(std::memory_order_relaxed),
                g_bracketLoadReturns.load(std::memory_order_relaxed),
                g_bracketLoadGeneration.load(std::memory_order_relaxed)));
            a_out(std::format("pluginDirectory={}", root.string()));
            a_out(std::format("packsDirectory={}", DefaultPacksDirectory().string()));
            a_out("manifestParser=implemented (strict package schema v1, canonical plugin/local targeting, containment, deterministic conflicts)");
            a_out("npcDecoder=implemented (strict CK 1.16.244 JSON contract; golden matrix and adversarial corpus pass)");
            a_out("dependencyResolver=RUNTIME-PROVEN read-only on Sarah (forms/headparts + facial shape/bone + FaceDB color/teeth/AVM catalogs)");
            a_out("runtimeNpcImporter=NO SAFE SEAM FOUND (SavePCFace is a parse-only console wrapper)");
            a_out("ownedEngineConstruction=RUNTIME-PROVEN (100/100 registered-empty Create(false)+destroy+unregister cycles)");
            a_out("copyAppearance=deep-owned containers STATIC-PROVEN, but also copies pronoun; visual-only path pending");
            a_out("appearanceRefresh=ID 101307 (byte-contract gated; runtime-proven one-shot for matching loaded actors)");
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
                R"({"schemaVersion":1,"requires":{},"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"5983"},"preset":"Sarah.npc"}]})",
                manifestPath, false);
            check(valid.manifest && valid.manifest->priority == 0 &&
                      valid.manifest->assignments.size() == 1 && valid.issues.empty(),
                  "valid production manifest");
            check(valid.manifest && valid.manifest->assignments[0].target.CanonicalKey() ==
                                        "starfield.esm:00005983",
                  "canonical plugin-local target");

            const auto traversal = ParsePackageManifest(
                R"({"schemaVersion":1,"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"5983"},"preset":"../escape.npc"}]})",
                manifestPath, false);
            check(traversal.HasFatalError(), "parent traversal rejected");

            const auto removedScope = ParsePackageManifest(
                R"({"schemaVersion":1,"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"5983"},"preset":"Sarah.npc","scope":"faceAndBody"}]})",
                manifestPath, false);
            check(removedScope.HasFatalError(), "removed scope property rejected");

            const auto unsupported = ParsePackageManifest(
                R"({"schemaVersion":2,"priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[]})",
                manifestPath, false);
            check(unsupported.HasFatalError(), "unknown schema version rejected");

            const auto unknownProperty = ParsePackageManifest(
                R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[],"surprise":true})",
                manifestPath, false);
            check(unknownProperty.HasFatalError(), "unknown root property rejected");

            const auto malformed = ParsePackageManifest(
                R"({"schemaVersion":)", manifestPath, false);
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
            std::filesystem::path packsRoot;
            if (a_args.size() > 2) {
                packsRoot = std::filesystem::path{ JoinArguments(a_args, 2) };
            } else {
                packsRoot = DefaultPacksDirectory();
            }

            a_out(std::format("scan packsRoot={}", packsRoot.string()));
            auto discovery = DiscoverPackages(packsRoot, true);
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
                            assignment.requirements,
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
                g_targetBaseIDs = std::move(resolvedBaseIDs);
                g_sceneAssignments = std::move(resolvedAssignments);
            }
            a_out(std::format("scan: discoveredPacks={} implicitPacks={} validPacks={} decodedPresets={} validCandidates={} winners={} resolvedTargets={}; validation only, owned population/application gate prevents mutation",
                              discovery.packages.size(), implicitPacks, validPacks,
                              decodedPresets, validatedCandidates.size(), selection.winners.size(),
                              resolvedCount));
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

        void RunRefs(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (a_args.size() < 4) {
                a_out("usage: npcapp refs <plugin:localFormID> <preset.npc>");
                return;
            }
            auto* target = ResolveTargetArgument(a_out, a_args[2]);
            if (!target) {
                return;
            }
            const std::filesystem::path path{ JoinArguments(a_args, 3) };
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
            if (a_args.size() < 4) {
                a_out("usage: npcapp avm <plugin:localFormID> <preset.npc>");
                return;
            }
            auto* target = ResolveTargetArgument(a_out, a_args[2]);
            if (!target) {
                return;
            }
            const std::filesystem::path path{ JoinArguments(a_args, 3) };
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
            if (a_args.size() < 3) {
                a_out("usage: npcapp resolve <plugin:localFormID>");
                return;
            }
            const auto target = ParseTargetToken(a_args[2]);
            if (!target) {
                a_out("resolve: invalid target token");
                return;
            }
            (void)ResolveEligibleTarget(a_out, *target);
        }

        void RunDonor(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (!RequireMutationOperational(a_out, "donor")) {
                return;
            }
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
            if (!RequireMutationOperational(a_out, "donorseed")) {
                return;
            }
            if (a_args.size() < 4) {
                a_out("usage: npcapp donorseed <plugin:localFormID> <preset.npc>");
                return;
            }
            auto* target = ResolveTargetArgument(a_out, a_args[2]);
            if (!target) {
                return;
            }

            const std::filesystem::path path{ JoinArguments(a_args, 3) };
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
            if (!RequireMutationOperational(a_out, "donormorph")) {
                return;
            }
            if (a_args.size() < 4) {
                a_out("usage: npcapp donormorph <plugin:localFormID> <preset.npc>");
                return;
            }
            auto* target = ResolveTargetArgument(a_out, a_args[2]);
            if (!target) {
                return;
            }
            const std::filesystem::path path{ JoinArguments(a_args, 3) };
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
            if (!RequireMutationOperational(a_out, "donorvisual")) {
                return;
            }
            if (a_args.size() < 4) {
                a_out("usage: npcapp donorvisual <plugin:localFormID> <preset.npc>");
                return;
            }
            auto* target = ResolveTargetArgument(a_out, a_args[2]);
            if (!target) {
                return;
            }
            const std::filesystem::path path{ JoinArguments(a_args, 3) };
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
            if (!RequireMutationOperational(a_out, "donorcopy")) {
                return;
            }
            if (a_args.size() < 4) {
                a_out("usage: npcapp donorcopy <plugin:localFormID> <preset.npc>");
                return;
            }
            auto* target = ResolveTargetArgument(a_out, a_args[2]);
            if (!target) {
                return;
            }
            const std::filesystem::path path{ JoinArguments(a_args, 3) };
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

        void RunTargetTrial(
            const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (!RequireMutationOperational(a_out, "targettrial")) {
                return;
            }
            if (a_args.size() < 5) {
                a_out("usage: npcapp targettrial <plugin:localFormID> <actorRefID> <preset.npc>");
                return;
            }

            const auto nativeDiagnostics =
                Util::NativeMainThreadQueue::GetDiagnostics();
            if (!nativeDiagnostics.insideDrain ||
                !nativeDiagnostics.queueEnabled ||
                nativeDiagnostics.singleton == 0) {
                a_out(std::format(
                    "targettrial: refused outside verified native drain insideDrain={} queueEnabled={} singleton=0x{:X}; no mutation",
                    nativeDiagnostics.insideDrain,
                    nativeDiagnostics.queueEnabled,
                    nativeDiagnostics.singleton));
                return;
            }

            const auto actorRefID = ParseFormID(a_args[3]);
            const auto targetIdentity = ParseTargetToken(a_args[2]);
            if (!actorRefID || !targetIdentity) {
                a_out("targettrial: invalid target token or actorRefID");
                return;
            }
            auto* target = ResolveEligibleTarget(a_out, *targetIdentity);
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(*actorRefID);
            if (!target || !actor || actor->GetNPC() != target) {
                a_out(std::format(
                    "targettrial: actor ref 0x{:08X} is absent or bound to a different base; no mutation",
                    *actorRefID));
                return;
            }

            std::filesystem::path trackedPreset;
            {
                const std::scoped_lock lock{ g_appliedBasesMutex };
                const auto state = g_appliedBases.find(target->GetFormID());
                if (state == g_appliedBases.end() || state->second.bracketFailed) {
                    a_out(std::format(
                        "targettrial: base 0x{:08X} is not safely tracked by the active save bracket; no mutation",
                        target->GetFormID()));
                    return;
                }
                trackedPreset = state->second.assignment.presetPath;
            }

            const std::filesystem::path requestedPreset{ JoinArguments(a_args, 4) };
            std::error_code requestedError;
            std::error_code trackedError;
            const auto canonicalRequested =
                std::filesystem::weakly_canonical(requestedPreset, requestedError);
            const auto canonicalTracked =
                std::filesystem::weakly_canonical(trackedPreset, trackedError);
            if (requestedError || trackedError ||
                canonicalRequested != canonicalTracked) {
                a_out(std::format(
                    "targettrial: requested preset does not match the bracket-tracked winning assignment requested={} tracked={}; no mutation",
                    requestedPreset.string(), trackedPreset.string()));
                return;
            }

            if (!SilentApplyPresetToBase(a_out, target, trackedPreset)) {
                a_out("targettrial: FAIL CLOSED during silent apply; tracked base remains bracket-owned");
                return;
            }
            if (!NotifyAndKick(target, actor, *actorRefID)) {
                a_out("targettrial: FAIL CLOSED during notify/kick; do not save");
                return;
            }
            a_out(std::format(
                "targettrial: PASS production apply + one-shot notify/kick base=0x{:08X} actor=0x{:08X}",
                target->GetFormID(), *actorRefID));
        }

        void RunCopyRef(const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (!RequireMutationOperational(a_out, "copyref")) {
                return;
            }
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
                // (deferral, veto, or retry at the next trigger).
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
        // Overlay feasibility probes (npcapp overlay|probe*)
        // Dev-only instrumentation for the render-time overlay migration:
        // measure whether engine worker 97401 writes the target base, whether
        // its effect serializes, whether a single-drain transient window
        // renders, and whether ReferenceSet3d is a usable trigger. Probes
        // never register in g_appliedBases; the save bracket stays armed as
        // the backstop while they run.
        // ==================================================================
        // Default ON since the 2026-08-07 soak (all trigger paths green);
        // `npcapp overlay off` falls back to the legacy persistent-apply
        // path at the next load while the save bracket still exists.
        std::atomic<bool> g_overlayModeEnabled{ true };

        struct ProbeBaseline
        {
            OwnedVisualSnapshot visual;
            NonVisualSnapshot   nonVisual;
            RE::TESNPC*         faceNPC{ nullptr };
            std::uint32_t       actorFlags{ 0 };
        };

        std::mutex                                        g_probeBaselineMutex;
        std::unordered_map<RE::TESFormID, ProbeBaseline>  g_probeBaselines;

        // Runs the probe body inside the verified drain: inline when already
        // draining, otherwise posted with output redirected to the SFSE log
        // because the interactive sink does not outlive the command.
        [[nodiscard]] bool RunProbeOnDrain(
            const LineSink& a_out,
            const std::string_view a_label,
            std::function<void(const LineSink&)> a_body)
        {
            const auto diagnostics =
                Util::NativeMainThreadQueue::GetDiagnostics();
            if (diagnostics.insideDrain) {
                a_body(a_out);
                return true;
            }
            const LineSink logSink =
                [label = std::string{ a_label }](const std::string& a_text) {
                    REX::INFO("[NpcAppearance] {}: {}", label, a_text);
                };
            const bool queued = QueueOrRunNativeTask(
                [body = std::move(a_body), logSink]() { body(logSink); },
                a_label,
                [label = std::string{ a_label }]() {
                    REX::WARN(
                        "[NpcAppearance] {} dropped by the native queue; probe did not run",
                        label);
                });
            a_out(std::format(
                "{}: {} the verified native drain; output continues in the SFSE log",
                a_label, queued ? "posted to" : "FAILED to post to"));
            return queued;
        }

        // Group-wise live-vs-baseline report. Vector comparisons here are
        // order-sensitive and therefore stricter than SameExactVisualValues
        // (which matches boneRegions/shapeBlends by membership); the verdict
        // always comes from SameExactVisualValues, this only localizes diffs.
        void ReportOwnedVisualDifference(
            const LineSink& a_out,
            RE::TESNPC* a_npc,
            const OwnedVisualSnapshot& a_expected)
        {
            const auto live = CaptureOwnedVisualSnapshot(a_npc);
            a_out(std::format(
                "visual diff vs baseline: morph={} skinTone={} pronoun={} headParts={} bodyRegions={} boneValues={} boneRegions={} avms={} shapeBlends={}",
                live.thin == a_expected.thin &&
                    live.muscular == a_expected.muscular &&
                    live.fat == a_expected.fat,
                live.skinToneIndex == a_expected.skinToneIndex,
                live.pronoun == a_expected.pronoun,
                live.headPartFormIDs == a_expected.headPartFormIDs,
                live.hasBodyMorphRegions == a_expected.hasBodyMorphRegions &&
                    live.bodyMorphRegions == a_expected.bodyMorphRegions,
                live.hasBoneValues == a_expected.hasBoneValues &&
                    live.boneValues == a_expected.boneValues,
                live.hasBoneRegions == a_expected.hasBoneRegions &&
                    live.boneRegions == a_expected.boneRegions,
                live.avms == a_expected.avms,
                live.hasShapeBlends == a_expected.hasShapeBlends &&
                    live.shapeBlends == a_expected.shapeBlends));
            a_out(std::format(
                "visual diff strings: teeth={} jewelry={} eye={} hair={} facial={} eyebrow={} live/baseline morph=({:.6g},{:.6g},{:.6g})/({:.6g},{:.6g},{:.6g}) skin={}/{}",
                live.teeth == a_expected.teeth,
                live.jewelryColor == a_expected.jewelryColor,
                live.eyeColor == a_expected.eyeColor,
                live.hairColor == a_expected.hairColor,
                live.facialColor == a_expected.facialColor,
                live.eyebrowColor == a_expected.eyebrowColor,
                live.thin, live.muscular, live.fat,
                a_expected.thin, a_expected.muscular, a_expected.fat,
                live.skinToneIndex, a_expected.skinToneIndex));
        }

        // ==================================================================
        // Overlay runtime (Mechanism B, probe-proven 2026-08-07; see
        // docs/OVERLAY_PROBE_FINDINGS.md). Per 3D build of a tracked actor:
        // apply preset to base -> notify -> refresh -> restore byte-exactly,
        // all within one verified drain task, so the serializable TESNPC is
        // never preset-mutated at rest. Triggered by ReferenceSet3d (fires
        // outside the drain; handler posts) plus a post-load sweep. While the
        // save bracket still exists, a failed in-window restore escalates the
        // base into g_appliedBases so the bracket regains custody and the
        // veto path protects the next save.
        // ==================================================================
        constexpr std::chrono::milliseconds kOverlayReapplyCooldown{ 1000 };

        struct OverlayRuntimeState
        {
            std::unordered_set<RE::TESFormID> disabledBases;
            std::unordered_set<RE::TESFormID> inFlightRefs;
            std::unordered_map<RE::TESFormID,
                               std::chrono::steady_clock::time_point>
                          lastAppliedByRef;
            std::uint64_t set3dMatches{ 0 };
            std::uint64_t postedApplies{ 0 };
            std::uint64_t droppedPosts{ 0 };
            std::uint64_t applies{ 0 };
            std::uint64_t failures{ 0 };
            std::uint64_t escalations{ 0 };
            std::uint64_t sweeps{ 0 };
            std::uint64_t skippedCooldown{ 0 };
            std::uint64_t skippedNo3D{ 0 };
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
                .bracketFailed = false,
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
                insurance.bracketFailed = true;
                {
                    const std::scoped_lock lock{ g_appliedBasesMutex };
                    g_appliedBases.insert_or_assign(baseID, insurance);
                }
                {
                    const std::scoped_lock lock{ g_overlayRuntimeMutex };
                    ++g_overlayRuntime.escalations;
                    ++g_overlayRuntime.failures;
                }
                KillMutation(
                    "overlay window restore failed; base escalated to bracket custody");
                REX::CRITICAL(
                    "[NpcAppearance] overlay base=0x{:08X} actor=0x{:08X} restore FAILED after applied={} notifiedKicked={}; escalated to save bracket custody",
                    baseID, a_actorRefID, applied, notifiedKicked);
                return false;
            }
            if (!applied || !notifiedKicked) {
                {
                    const std::scoped_lock lock{ g_overlayRuntimeMutex };
                    g_overlayRuntime.disabledBases.insert(baseID);
                    ++g_overlayRuntime.failures;
                }
                REX::WARN(
                    "[NpcAppearance] overlay base=0x{:08X} actor=0x{:08X} applied={} notifiedKicked={}; rendering vanilla and disabling this base for the session",
                    baseID, a_actorRefID, applied, notifiedKicked);
                return false;
            }
            {
                const std::scoped_lock lock{ g_overlayRuntimeMutex };
                ++g_overlayRuntime.applies;
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

            if (!g_overlayModeEnabled.load(std::memory_order_acquire) ||
                !MutationOperational()) {
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
                const std::scoped_lock lock{ g_overlayRuntimeMutex };
                ++g_overlayRuntime.skippedNo3D;
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
                if (!g_overlayModeEnabled.load(std::memory_order_acquire) ||
                    !MutationOperational()) {
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
                    ++g_overlayRuntime.set3dMatches;
                    if (g_overlayRuntime.disabledBases.contains(baseID)) {
                        return;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    const auto last =
                        g_overlayRuntime.lastAppliedByRef.find(refID);
                    if (last != g_overlayRuntime.lastAppliedByRef.end() &&
                        now - last->second < kOverlayReapplyCooldown) {
                        ++g_overlayRuntime.skippedCooldown;
                        return;
                    }
                    if (!g_overlayRuntime.inFlightRefs.insert(refID).second) {
                        return;
                    }
                    ++g_overlayRuntime.postedApplies;
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
                            ++g_overlayRuntime.droppedPosts;
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
            if (!g_overlayModeEnabled.load(std::memory_order_acquire) ||
                !MutationOperational()) {
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
            {
                const std::scoped_lock lock{ g_overlayRuntimeMutex };
                ++g_overlayRuntime.sweeps;
            }
            REX::INFO(
                "[NpcAppearance] overlay sweep reason={} winners={} applied={} skipped={}",
                a_reason, winners.size(), appliedCount, skippedCount);
        }

        void RunOverlayMode(
            const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            const std::string_view mode =
                a_args.size() >= 3 ? std::string_view{ a_args[2] } : "status";
            if (mode == "on" || mode == "off") {
                g_overlayModeEnabled.store(
                    mode == "on", std::memory_order_release);
                if (mode == "on") {
                    static_cast<void>(QueueOrRunNativeTask(
                        []() { RunOverlaySweep("overlay-on"); },
                        "NpcAppearance.OverlaySweep"));
                    a_out("overlay: ON; sweep posted for already-loaded actors, new 3D builds apply via ReferenceSet3d. Legacy persistent apply is skipped at the next load");
                } else {
                    a_out("overlay: OFF; no new windows will open. Reload to return tracked NPCs to the legacy persistent-apply path");
                }
                return;
            }
            if (mode == "sweep") {
                const bool queued = QueueOrRunNativeTask(
                    []() { RunOverlaySweep("manual"); },
                    "NpcAppearance.OverlaySweep");
                a_out(std::format(
                    "overlay: sweep {}",
                    queued ? "posted to the verified native drain"
                           : "FAILED to post"));
                return;
            }
            if (mode != "status") {
                a_out("usage: npcapp overlay [status|on|off|sweep]");
                return;
            }
            OverlayRuntimeState snapshot;
            std::size_t disabledCount = 0;
            std::size_t inFlightCount = 0;
            {
                const std::scoped_lock lock{ g_overlayRuntimeMutex };
                snapshot.set3dMatches = g_overlayRuntime.set3dMatches;
                snapshot.postedApplies = g_overlayRuntime.postedApplies;
                snapshot.droppedPosts = g_overlayRuntime.droppedPosts;
                snapshot.applies = g_overlayRuntime.applies;
                snapshot.failures = g_overlayRuntime.failures;
                snapshot.escalations = g_overlayRuntime.escalations;
                snapshot.sweeps = g_overlayRuntime.sweeps;
                snapshot.skippedCooldown = g_overlayRuntime.skippedCooldown;
                snapshot.skippedNo3D = g_overlayRuntime.skippedNo3D;
                disabledCount = g_overlayRuntime.disabledBases.size();
                inFlightCount = g_overlayRuntime.inFlightRefs.size();
            }
            a_out(std::format(
                "overlay: mode={} sinkRegistered={} mutationOperational={} set3dMatches={} postedApplies={} droppedPosts={} applies={} failures={} escalations={} sweeps={} skippedCooldown={} skippedNo3D={} disabledBases={} inFlight={}",
                g_overlayModeEnabled.load(std::memory_order_relaxed)
                    ? "on" : "off",
                g_overlaySinkRegistered.load(std::memory_order_relaxed),
                MutationOperational(),
                snapshot.set3dMatches, snapshot.postedApplies,
                snapshot.droppedPosts, snapshot.applies, snapshot.failures,
                snapshot.escalations, snapshot.sweeps,
                snapshot.skippedCooldown, snapshot.skippedNo3D,
                disabledCount, inFlightCount));
        }

        void RunProbeBaseline(
            const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (a_args.size() < 3) {
                a_out("usage: npcapp probebaseline <plugin:localFormID>");
                return;
            }
            const auto targetIdentity = ParseTargetToken(a_args[2]);
            if (!targetIdentity) {
                a_out("probebaseline: invalid target token");
                return;
            }
            (void)RunProbeOnDrain(
                a_out, "probebaseline",
                [identity = *targetIdentity](const LineSink& a_sink) {
                    auto* target = ResolveEligibleTarget(a_sink, identity);
                    if (!target) {
                        return;
                    }
                    ProbeBaseline baseline;
                    baseline.visual = CaptureOwnedVisualSnapshot(target);
                    baseline.nonVisual = Snapshot(target);
                    baseline.faceNPC = target->faceNPC;
                    baseline.actorFlags =
                        target->actorData.actorBaseFlags.underlying();
                    const auto baseID = target->GetFormID();
                    a_sink(std::format(
                        "probebaseline: base=0x{:08X} captured headParts={} bodyRegions={} boneValues={} boneRegions={} avms={} shapeBlends={} morphWeight=({:.6g},{:.6g},{:.6g}) skinTone={} faceNPC=0x{:08X} flags=0x{:08X}",
                        baseID,
                        baseline.visual.headPartFormIDs.size(),
                        baseline.visual.bodyMorphRegions.size(),
                        baseline.visual.boneValues.size(),
                        baseline.visual.boneRegions.size(),
                        baseline.visual.avms.size(),
                        baseline.visual.shapeBlends.size(),
                        baseline.visual.thin, baseline.visual.muscular,
                        baseline.visual.fat, baseline.visual.skinToneIndex,
                        baseline.faceNPC ? baseline.faceNPC->GetFormID() : 0,
                        baseline.actorFlags));
                    const std::scoped_lock lock{ g_probeBaselineMutex };
                    g_probeBaselines.insert_or_assign(
                        baseID, std::move(baseline));
                });
        }

        void RunProbeCompare(
            const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (a_args.size() < 3) {
                a_out("usage: npcapp probecompare <plugin:localFormID>");
                return;
            }
            const auto targetIdentity = ParseTargetToken(a_args[2]);
            if (!targetIdentity) {
                a_out("probecompare: invalid target token");
                return;
            }
            (void)RunProbeOnDrain(
                a_out, "probecompare",
                [identity = *targetIdentity](const LineSink& a_sink) {
                    auto* target = ResolveEligibleTarget(a_sink, identity);
                    if (!target) {
                        return;
                    }
                    std::optional<ProbeBaseline> baseline;
                    {
                        const std::scoped_lock lock{ g_probeBaselineMutex };
                        const auto found =
                            g_probeBaselines.find(target->GetFormID());
                        if (found != g_probeBaselines.end()) {
                            baseline = found->second;
                        }
                    }
                    if (!baseline) {
                        a_sink(std::format(
                            "probecompare: base=0x{:08X} has no stored baseline; run probebaseline first",
                            target->GetFormID()));
                        return;
                    }
                    const bool visualExact =
                        SameExactVisualValues(target, baseline->visual);
                    const auto liveNonVisual = Snapshot(target);
                    const bool nonVisualExact =
                        baseline->nonVisual == liveNonVisual;
                    const bool nonVisualExactIgnoringDirty =
                        SameNonVisualIgnoringRefreshDirtyFlag(
                            baseline->nonVisual, liveNonVisual);
                    const bool faceNPCSame =
                        target->faceNPC == baseline->faceNPC;
                    const auto liveFlags =
                        target->actorData.actorBaseFlags.underlying();
                    a_sink(std::format(
                        "probecompare: base=0x{:08X} visualExact={} nonVisualExact={} nonVisualExactIgnoringDirtyFlag={} faceNPCSame={} flagsSame={} flags=0x{:08X}/0x{:08X}",
                        target->GetFormID(), visualExact, nonVisualExact,
                        nonVisualExactIgnoringDirty, faceNPCSame,
                        liveFlags == baseline->actorFlags,
                        liveFlags, baseline->actorFlags));
                    if (!visualExact) {
                        ReportOwnedVisualDifference(
                            a_sink, target, baseline->visual);
                    }
                });
        }

        void RunProbe97401(
            const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (!RequireMutationOperational(a_out, "probe97401")) {
                return;
            }
            if (a_args.size() < 4) {
                a_out("usage: npcapp probe97401 <targetRefID> <sourceRefID> [restore=0|1]");
                return;
            }
            const auto targetID = ParseFormID(a_args[2]);
            const auto sourceID = ParseFormID(a_args[3]);
            if (!targetID || !sourceID) {
                a_out("probe97401: invalid hexadecimal form ID");
                return;
            }
            bool restore = true;
            if (a_args.size() >= 5) {
                if (a_args[4] != "0" && a_args[4] != "1") {
                    a_out("probe97401: restore must be 0 or 1");
                    return;
                }
                restore = a_args[4] == "1";
            }
            (void)RunProbeOnDrain(
                a_out, "probe97401",
                [targetID = *targetID, sourceID = *sourceID,
                 restore](const LineSink& a_sink) {
                    auto* target =
                        RE::TESForm::LookupByID<RE::Actor>(targetID);
                    auto* source =
                        RE::TESForm::LookupByID<RE::Actor>(sourceID);
                    if (!target || !source) {
                        a_sink(std::format(
                            "probe97401: actor lookup failed target={} source={}",
                            static_cast<void*>(target),
                            static_cast<void*>(source)));
                        return;
                    }
                    auto* targetBase = target->GetNPC();
                    auto* sourceBase = source->GetNPC();
                    if (!targetBase || !sourceBase || !targetBase->IsUnique()) {
                        a_sink(std::format(
                            "probe97401: ineligible target/source base target={} source={} targetUnique={}",
                            static_cast<void*>(targetBase),
                            static_cast<void*>(sourceBase),
                            targetBase && targetBase->IsUnique()));
                        return;
                    }
                    if (targetBase->pronoun.underlying() !=
                        sourceBase->pronoun.underlying()) {
                        a_sink(std::format(
                            "probe97401: rejected before mutation because the worker would copy pronoun (target={} source={})",
                            targetBase->pronoun.underlying(),
                            sourceBase->pronoun.underlying()));
                        return;
                    }

                    using Worker = void (*)(RE::Actor*, RE::TESNPC*, bool);
                    REL::Relocation<Worker> worker{
                        kActorCopyAppearanceWorkerID
                    };
                    if (!HasExpectedGate(worker.address())) {
                        a_sink(std::format(
                            "probe97401: ID 97401 contract mismatch at img+0x{:X}; FAIL CLOSED",
                            Util::ToRva(worker.address())));
                        return;
                    }

                    const bool sourceIsPlayer = sourceID == 0x14;
                    AppliedBaseState insurance;
                    insurance.baseID = targetBase->GetFormID();
                    insurance.originalVisual =
                        CaptureOwnedVisualSnapshot(targetBase);
                    insurance.originalNonVisual = Snapshot(targetBase);
                    insurance.originalFaceNPC = targetBase->faceNPC;
                    insurance.originalActorFlags =
                        targetBase->actorData.actorBaseFlags.underlying();
                    const auto sourceVisualBefore =
                        CaptureOwnedVisualSnapshot(sourceBase);

                    const auto started = std::chrono::steady_clock::now();
                    worker(target, sourceBase, sourceIsPlayer);
                    const auto elapsedMs =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started)
                            .count();

                    const bool baseVisualMutated = !SameExactVisualValues(
                        targetBase, insurance.originalVisual);
                    const auto nonVisualAfter = Snapshot(targetBase);
                    const bool baseNonVisualMutated =
                        !(insurance.originalNonVisual == nonVisualAfter);
                    const bool baseNonVisualMutatedIgnoringDirty =
                        !SameNonVisualIgnoringRefreshDirtyFlag(
                            insurance.originalNonVisual, nonVisualAfter);
                    const bool faceNPCChanged =
                        targetBase->faceNPC != insurance.originalFaceNPC;
                    const bool flagsChanged =
                        targetBase->actorData.actorBaseFlags.underlying() !=
                        insurance.originalActorFlags;
                    const bool sourceVisualUntouched = SameExactVisualValues(
                        sourceBase, sourceVisualBefore);
                    a_sink(std::format(
                        "probe97401: RESULT targetRef=0x{:08X} base=0x{:08X} sourceIsPlayer={} baseVisualMutated={} baseNonVisualMutated={} baseNonVisualMutatedIgnoringDirtyFlag={} faceNPCChanged={} flagsChanged={} sourceVisualUntouched={} ms={:.3f}",
                        targetID, insurance.baseID, sourceIsPlayer,
                        baseVisualMutated, baseNonVisualMutated,
                        baseNonVisualMutatedIgnoringDirty, faceNPCChanged,
                        flagsChanged, sourceVisualUntouched, elapsedMs));
                    if (baseVisualMutated) {
                        ReportOwnedVisualDifference(
                            a_sink, targetBase, insurance.originalVisual);
                    }

                    const bool baseDirty =
                        baseVisualMutated || faceNPCChanged || flagsChanged;
                    if (!baseDirty) {
                        a_sink("probe97401: target base untouched; nothing to restore");
                        return;
                    }
                    if (!restore) {
                        a_sink("probe97401: base left mutated BY DESIGN for the save-persistence procedure; save, reload, run probecompare, and do not continue normal play on this session");
                        return;
                    }
                    const bool restoredExact = RestoreAppliedBaseState(
                        a_sink, targetBase, insurance);
                    a_sink(std::format(
                        "probe97401: restored exact={}", restoredExact));
                    if (!restoredExact) {
                        KillMutation(
                            "probe97401 exact restoration failed; base left non-original");
                        a_sink("probe97401: CRITICAL restore failed; do not save this session, reload immediately");
                    }
                });
        }

        void ScheduleProbeTransientRecheck(const AppliedBaseState& a_insurance)
        {
            std::thread{ [insurance = a_insurance]() mutable {
                std::this_thread::sleep_for(std::chrono::seconds{ 2 });
                const auto baseID = insurance.baseID;
                (void)QueueOrRunNativeTask(
                    [insurance = std::move(insurance)]() {
                        auto* target = RE::TESForm::LookupByID<RE::TESNPC>(
                            insurance.baseID);
                        const bool stillExact =
                            ExactOriginalState(target, insurance);
                        REX::INFO(
                            "[NpcAppearance] probetransient: RECHECK base=0x{:08X} baseStillExact={}; visually confirm the actor's rendered appearance",
                            insurance.baseID, stillExact);
                    },
                    "NpcAppearance.ProbeTransientRecheck",
                    [baseID]() {
                        REX::WARN(
                            "[NpcAppearance] probetransient: RECHECK base=0x{:08X} dropped by the native queue",
                            baseID);
                    });
            } }.detach();
        }

        void RunProbeTransient(
            const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            if (!RequireMutationOperational(a_out, "probetransient")) {
                return;
            }
            if (a_args.size() < 5) {
                a_out("usage: npcapp probetransient <plugin:localFormID> <actorRefID> <preset.npc>");
                return;
            }
            const auto targetIdentity = ParseTargetToken(a_args[2]);
            const auto actorRefID = ParseFormID(a_args[3]);
            if (!targetIdentity || !actorRefID) {
                a_out("probetransient: invalid target token or actorRefID");
                return;
            }
            const std::filesystem::path presetPath{ JoinArguments(a_args, 4) };

            // Unlike targettrial this accepts an arbitrary preset: the
            // mutation never persists past this drain task, so the
            // bracket-ownership policy does not apply.
            (void)RunProbeOnDrain(
                a_out, "probetransient",
                [identity = *targetIdentity, actorRefID = *actorRefID,
                 presetPath](const LineSink& a_sink) {
                    auto* target = ResolveEligibleTarget(a_sink, identity);
                    auto* actor =
                        RE::TESForm::LookupByID<RE::Actor>(actorRefID);
                    if (!target || !actor || actor->GetNPC() != target) {
                        a_sink(std::format(
                            "probetransient: actor ref 0x{:08X} is absent or bound to a different base; no mutation",
                            actorRefID));
                        return;
                    }
                    if (!HasLoaded3D(actor)) {
                        a_sink(std::format(
                            "probetransient: actor ref 0x{:08X} has no loaded 3D; the transient-window question needs a rendered actor; no mutation",
                            actorRefID));
                        return;
                    }

                    AppliedBaseState insurance;
                    insurance.baseID = target->GetFormID();
                    insurance.originalVisual =
                        CaptureOwnedVisualSnapshot(target);
                    insurance.originalNonVisual = Snapshot(target);
                    insurance.originalFaceNPC = target->faceNPC;
                    insurance.originalActorFlags =
                        target->actorData.actorBaseFlags.underlying();

                    const auto started = std::chrono::steady_clock::now();
                    const bool applied = SilentApplyPresetToBase(
                        a_sink, target, presetPath);
                    bool notifiedKicked = false;
                    if (applied) {
                        notifiedKicked =
                            NotifyAndKick(target, actor, actorRefID);
                    }
                    const bool restoredExact = RestoreAppliedBaseState(
                        a_sink, target, insurance);
                    const auto elapsedMs =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started)
                            .count();
                    a_sink(std::format(
                        "probetransient: window CLOSED base=0x{:08X} actor=0x{:08X} applied={} notifiedKicked={} restoredExact={} ms={:.3f}; visually confirm whether the actor renders the preset or vanilla",
                        insurance.baseID, actorRefID, applied,
                        notifiedKicked, restoredExact, elapsedMs));
                    if (!restoredExact) {
                        KillMutation(
                            "probetransient exact restoration failed; base left non-original");
                        a_sink("probetransient: CRITICAL restore failed; do not save this session, reload immediately");
                        return;
                    }
                    ScheduleProbeTransientRecheck(insurance);
                });
        }

        struct ProbeSet3dState
        {
            std::atomic<bool>          enabled{ false };
            std::atomic<bool>          registered{ false };
            std::atomic<std::uint64_t> set3dEvents{ 0 };
            std::atomic<std::uint64_t> detachEvents{ 0 };
            std::atomic<std::uint64_t> actorSet3dEvents{ 0 };
            std::atomic<std::uint64_t> trackedSet3dEvents{ 0 };
            std::atomic<std::uint64_t> trackedDetachEvents{ 0 };
            std::atomic<std::uint64_t> untrackedLogged{ 0 };
            std::atomic<std::uint64_t> latencyProbes{ 0 };
        };
        ProbeSet3dState g_probeSet3d;

        constexpr std::uint64_t kProbeSet3dUntrackedLogCap = 25;
        constexpr std::uint64_t kProbeSet3dLatencyProbeCap = 50;

        // Log-only observer; may run on any engine thread, so it never
        // writes game objects and keeps reads to SEH-guarded probes.
        void OnProbeReferenceEvent(
            RE::TESObjectREFR* a_ref, const bool a_set3d) noexcept
        {
            try {
                if (!g_probeSet3d.enabled.load(std::memory_order_acquire)) {
                    return;
                }
                (a_set3d ? g_probeSet3d.set3dEvents
                         : g_probeSet3d.detachEvents)
                    .fetch_add(1, std::memory_order_relaxed);
                auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
                if (!actor) {
                    return;
                }
                if (a_set3d) {
                    g_probeSet3d.actorSet3dEvents.fetch_add(
                        1, std::memory_order_relaxed);
                }
                auto* base = actor->GetNPC();
                const RE::TESFormID baseID = base ? base->GetFormID() : 0;
                bool tracked = false;
                {
                    const std::scoped_lock lock{ g_eventMutex };
                    tracked = baseID != 0 && g_targetBaseIDs.contains(baseID);
                }
                if (tracked) {
                    (a_set3d ? g_probeSet3d.trackedSet3dEvents
                             : g_probeSet3d.trackedDetachEvents)
                        .fetch_add(1, std::memory_order_relaxed);
                }
                if (!a_set3d) {
                    if (tracked) {
                        REX::INFO(
                            "[NpcAppearance] probeset3d: DETACH ref=0x{:08X} base=0x{:08X} tid={}",
                            actor->GetFormID(), baseID,
                            ::GetCurrentThreadId());
                    }
                    return;
                }
                const bool logUntracked =
                    !tracked &&
                    g_probeSet3d.untrackedLogged.fetch_add(
                        1, std::memory_order_relaxed) <
                        kProbeSet3dUntrackedLogCap;
                if (tracked || logUntracked) {
                    const auto diagnostics =
                        Util::NativeMainThreadQueue::GetDiagnostics();
                    REX::INFO(
                        "[NpcAppearance] probeset3d: SET3D ref=0x{:08X} base=0x{:08X} tracked={} tid={} insideDrain={} queueEnabled={} hasLoaded3D={}",
                        actor->GetFormID(), baseID, tracked,
                        diagnostics.currentThreadID, diagnostics.insideDrain,
                        diagnostics.queueEnabled, HasLoaded3D(actor));
                }
                if (tracked &&
                    g_probeSet3d.latencyProbes.fetch_add(
                        1, std::memory_order_relaxed) <
                        kProbeSet3dLatencyProbeCap) {
                    const auto posted = std::chrono::steady_clock::now();
                    const auto refID = actor->GetFormID();
                    (void)QueueOrRunNativeTask(
                        [refID, posted]() {
                            const auto latencyMs =
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - posted)
                                    .count();
                            REX::INFO(
                                "[NpcAppearance] probeset3d: drain latency ref=0x{:08X} ms={:.3f}",
                                refID, latencyMs);
                        },
                        "NpcAppearance.ProbeSet3dLatency");
                }
            } catch (...) {
            }
        }

        class ProbeReferenceEventSink final :
            public RE::BSTEventSink<
                RE::RuntimeComponentDBFactory::ReferenceSet3d>,
            public RE::BSTEventSink<
                RE::RuntimeComponentDBFactory::ReferenceDetach>
        {
        public:
            static ProbeReferenceEventSink& GetSingleton() noexcept
            {
                static ProbeReferenceEventSink singleton;
                return singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::RuntimeComponentDBFactory::ReferenceSet3d& a_event,
                RE::BSTEventSource<
                    RE::RuntimeComponentDBFactory::ReferenceSet3d>*) noexcept
                override
            {
                OnProbeReferenceEvent(a_event.ref.get(), true);
                return RE::BSEventNotifyControl::kContinue;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::RuntimeComponentDBFactory::ReferenceDetach& a_event,
                RE::BSTEventSource<
                    RE::RuntimeComponentDBFactory::ReferenceDetach>*) noexcept
                override
            {
                OnProbeReferenceEvent(a_event.ref.get(), false);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        void RunProbeSet3d(
            const LineSink& a_out, const std::vector<std::string>& a_args)
        {
            const std::string_view mode =
                a_args.size() >= 3 ? std::string_view{ a_args[2] } : "status";
            if (mode == "on") {
                if (!g_probeSet3d.registered.load(std::memory_order_acquire)) {
                    auto* set3dSource = RE::RuntimeComponentDBFactory::
                        ReferenceSet3d::GetEventSource();
                    auto* detachSource = RE::RuntimeComponentDBFactory::
                        ReferenceDetach::GetEventSource();
                    if (!set3dSource || !detachSource) {
                        a_out(std::format(
                            "probeset3d: event source unavailable set3d={} detach={}; sink not registered",
                            static_cast<void*>(set3dSource),
                            static_cast<void*>(detachSource)));
                        return;
                    }
                    set3dSource->RegisterSink(
                        &ProbeReferenceEventSink::GetSingleton());
                    detachSource->RegisterSink(
                        &ProbeReferenceEventSink::GetSingleton());
                    g_probeSet3d.registered.store(
                        true, std::memory_order_release);
                }
                g_probeSet3d.enabled.store(true, std::memory_order_release);
                a_out("probeset3d: ON; now load a save, fast-travel, and let late spawns build 3D, then read the SFSE log");
                return;
            }
            if (mode == "off") {
                g_probeSet3d.enabled.store(false, std::memory_order_release);
                a_out("probeset3d: OFF (sink stays registered; logging disabled)");
                return;
            }
            if (mode != "status") {
                a_out("usage: npcapp probeset3d [on|off|status]");
                return;
            }
            a_out(std::format(
                "probeset3d: enabled={} registered={} set3dEvents={} detachEvents={} actorSet3d={} trackedSet3d={} trackedDetach={} untrackedLogged={} latencyProbes={}",
                g_probeSet3d.enabled.load(std::memory_order_relaxed),
                g_probeSet3d.registered.load(std::memory_order_relaxed),
                g_probeSet3d.set3dEvents.load(std::memory_order_relaxed),
                g_probeSet3d.detachEvents.load(std::memory_order_relaxed),
                g_probeSet3d.actorSet3dEvents.load(std::memory_order_relaxed),
                g_probeSet3d.trackedSet3dEvents.load(std::memory_order_relaxed),
                g_probeSet3d.trackedDetachEvents.load(std::memory_order_relaxed),
                g_probeSet3d.untrackedLogged.load(std::memory_order_relaxed),
                g_probeSet3d.latencyProbes.load(std::memory_order_relaxed)));
        }

        [[nodiscard]] bool RestoreBasesForSave(
            const std::uint64_t a_entry,
            const std::vector<AppliedBaseState>& a_states)
        {
            const auto totalStarted = std::chrono::steady_clock::now();
            const auto diagnostics =
                Util::NativeMainThreadQueue::GetDiagnostics();
            if (!diagnostics.insideDrain) {
                REX::CRITICAL(
                    "[NpcAppearance] C2 PRE-SAVE entry={} reached outside the verified native drain tid={} drainOwnerTid={}; no game-object access",
                    a_entry, diagnostics.currentThreadID,
                    diagnostics.drainOwnerThreadID);
                return false;
            }

            std::size_t restoredCount = 0;
            std::size_t failedCount = 0;
            for (const auto& state : a_states) {
                const auto started = std::chrono::steady_clock::now();
                bool restoredExact = false;
                try {
                    auto* target =
                        RE::TESForm::LookupByID<RE::TESNPC>(state.baseID);
                    const LineSink out = [baseID = state.baseID](
                                             const std::string& a_text) {
                        REX::INFO(
                            "[NpcAppearance] C2 bracket base=0x{:08X}: {}",
                            baseID, a_text);
                    };
                    restoredExact = RestoreAppliedBaseState(out, target, state);
                } catch (const std::exception& e) {
                    REX::CRITICAL(
                        "[NpcAppearance] C2 PRE-SAVE target=0x{:08X} restore threw '{}'; swallowed per target",
                        state.baseID, e.what());
                } catch (...) {
                    REX::CRITICAL(
                        "[NpcAppearance] C2 PRE-SAVE target=0x{:08X} restore threw; swallowed per target",
                        state.baseID);
                }

                {
                    const std::scoped_lock lock{ g_appliedBasesMutex };
                    const auto found = g_appliedBases.find(state.baseID);
                    if (found != g_appliedBases.end()) {
                        if (restoredExact) {
                            g_saveEntryRestoredBases.insert(state.baseID);
                        } else {
                            found->second.bracketFailed = true;
                        }
                    } else {
                        restoredExact = false;
                    }
                }
                if (restoredExact) {
                    ++restoredCount;
                } else {
                    ++failedCount;
                }
                const auto elapsedMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
                REX::INFO(
                    "[NpcAppearance] C2 PRE-SAVE entry={} target=0x{:08X} tid={} insideDrain={} drainOwnerTid={} restoredExact={} bracketFailed={} ms={:.3f}",
                    a_entry, state.baseID, ::GetCurrentThreadId(),
                    diagnostics.insideDrain, diagnostics.drainOwnerThreadID,
                    restoredExact, !restoredExact, elapsedMs);
                if (!restoredExact) {
                    REX::CRITICAL(
                        "[NpcAppearance] C2 PRE-SAVE target=0x{:08X} exact restoration FAILED; engine save must be vetoed",
                        state.baseID);
                }
            }

            const auto totalMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - totalStarted).count();
            REX::INFO(
                "[NpcAppearance] C2 PRE-SAVE done entry={} targets={} restored={} failed={} tid={} insideDrain={} ms={:.3f}",
                a_entry, a_states.size(), restoredCount, failedCount,
                ::GetCurrentThreadId(), diagnostics.insideDrain, totalMs);
            return failedCount == 0;
        }

        void PumpDeferredC2LoadTask() noexcept;
        void PumpDeferredC2SaveTask() noexcept;

        void ScheduleDeferredC2Retry() noexcept
        {
            bool expected = false;
            if (!g_deferredC2RetryScheduled.compare_exchange_strong(
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
                             wait <= kDeferredC2RetryMaxWaits;
                             ++wait) {
                            std::this_thread::sleep_for(kDeferredC2RetryDelay);
                            const auto diagnostics =
                                Util::NativeMainThreadQueue::GetDiagnostics();
                            if (!diagnostics.queueEnabled ||
                                diagnostics.singleton == 0) {
                                continue;
                            }

                            const auto* tasks = SFSE::GetTaskInterface();
                            if (!tasks) {
                                g_deferredC2RetryScheduled.store(
                                    false, std::memory_order_release);
                                KillMutation(
                                    "SFSE task interface unavailable for deferred bracket retry");
                                REX::CRITICAL(
                                    "[NpcAppearance] deferred bracket retry could not acquire the SFSE task interface; pending work remains fail-closed");
                                return;
                            }
                            tasks->AddTask([] {
                                g_deferredC2RetryScheduled.store(
                                    false, std::memory_order_release);
                                PumpDeferredC2SaveTask();
                                PumpDeferredC2LoadTask();
                            });
                            return;
                        }

                        g_deferredC2RetryScheduled.store(
                            false, std::memory_order_release);
                        KillMutation(
                            "native queue unavailable for deferred bracket retry");
                        REX::CRITICAL(
                            "[NpcAppearance] deferred bracket retry timed out after {} ms waiting for the native queue; pending work remains fail-closed",
                            kDeferredC2RetryMaxWaits *
                                static_cast<std::uint32_t>(kDeferredC2RetryDelay.count()));
                    } catch (const std::exception& e) {
                        g_deferredC2RetryScheduled.store(
                            false, std::memory_order_release);
                        KillMutation("deferred bracket retry worker threw");
                        REX::CRITICAL(
                            "[NpcAppearance] deferred bracket retry worker threw '{}'; pending work remains fail-closed",
                            e.what());
                    } catch (...) {
                        g_deferredC2RetryScheduled.store(
                            false, std::memory_order_release);
                        KillMutation("deferred bracket retry worker threw");
                        REX::CRITICAL(
                            "[NpcAppearance] deferred bracket retry worker threw; pending work remains fail-closed");
                    }
                }).detach();
            } catch (const std::exception& e) {
                g_deferredC2RetryScheduled.store(
                    false, std::memory_order_release);
                KillMutation("deferred bracket retry scheduling threw");
                REX::CRITICAL(
                    "[NpcAppearance] deferred bracket retry scheduling threw '{}'; pending work remains fail-closed",
                    e.what());
            } catch (...) {
                g_deferredC2RetryScheduled.store(
                    false, std::memory_order_release);
                KillMutation("deferred bracket retry scheduling threw");
                REX::CRITICAL(
                    "[NpcAppearance] deferred bracket retry scheduling threw; pending work remains fail-closed");
            }
        }

        void PumpDeferredC2LoadTask() noexcept
        {
            std::shared_ptr<DeferredC2LoadTask> pending;
            try {
                {
                    const std::scoped_lock lock{ g_deferredC2LoadMutex };
                    if (!g_deferredC2LoadTask || g_deferredC2LoadInFlight) {
                        return;
                    }
                    pending = g_deferredC2LoadTask;
                    g_deferredC2LoadInFlight = pending;
                }

                auto execute = [pending] {
                    bool complete = true;
                    std::uint32_t attempt = 0;
                    try {
                        {
                            const std::scoped_lock lock{ g_deferredC2LoadMutex };
                            attempt = ++pending->attempts;
                        }
                        complete = pending->run(attempt);
                        if (!complete &&
                            attempt >= kC2LoadReadyMaxNativeFrames) {
                            KillMutation("load-return actor readiness timed out");
                            REX::CRITICAL(
                                "[NpcAppearance] C2 LOAD-RETURN generation={} readiness TIMEOUT after {} verified native frames; no mutation",
                                pending->generation, attempt);
                            complete = true;
                        }
                    } catch (const std::exception& e) {
                        KillMutation("deferred load-return native task threw");
                        try {
                            REX::CRITICAL(
                                "[NpcAppearance] deferred C2 LOAD-RETURN generation={} threw '{}' inside the verified drain",
                                pending->generation, e.what());
                        } catch (...) {
                        }
                    } catch (...) {
                        KillMutation("deferred load-return native task threw");
                        try {
                            REX::CRITICAL(
                                "[NpcAppearance] deferred C2 LOAD-RETURN generation={} threw inside the verified drain",
                                pending->generation);
                        } catch (...) {
                        }
                    }
                    bool retry = false;
                    {
                        const std::scoped_lock lock{ g_deferredC2LoadMutex };
                        if (complete && g_deferredC2LoadTask == pending) {
                            g_deferredC2LoadTask.reset();
                        }
                        retry = g_deferredC2LoadTask != nullptr;
                        if (g_deferredC2LoadInFlight == pending) {
                            g_deferredC2LoadInFlight.reset();
                        }
                    }
                    if (retry) {
                        ScheduleDeferredC2Retry();
                    }
                };

                const auto diagnostics =
                    Util::NativeMainThreadQueue::GetDiagnostics();
                if (diagnostics.insideDrain) {
                    execute();
                    return;
                }

                const auto postResult = Util::NativeMainThreadQueue::Post(
                    std::move(execute), "NpcAppearance.C2.LoadApply",
                    [pending] {
                        bool retry = false;
                        {
                            const std::scoped_lock lock{ g_deferredC2LoadMutex };
                            if (g_deferredC2LoadInFlight == pending) {
                                g_deferredC2LoadInFlight.reset();
                            }
                            retry = g_deferredC2LoadTask != nullptr;
                        }
                        if (retry) {
                            ScheduleDeferredC2Retry();
                        }
                    });
                if (postResult ==
                    Util::NativeMainThreadQueue::PostResult::kQueued) {
                    if (pending->attempts == 0) {
                        REX::INFO(
                            "[NpcAppearance] C2 LOAD-RETURN generation={} queued for verified native drain after queueDeferral={}",
                            pending->generation, pending->deferralLogged);
                    }
                    return;
                }

                bool logDeferral = false;
                {
                    const std::scoped_lock lock{ g_deferredC2LoadMutex };
                    if (g_deferredC2LoadInFlight == pending) {
                        g_deferredC2LoadInFlight.reset();
                    }
                    if (g_deferredC2LoadTask == pending &&
                        !pending->deferralLogged) {
                        pending->deferralLogged = true;
                        logDeferral = true;
                    }
                }
                if (logDeferral) {
                    REX::INFO(
                        "[NpcAppearance] C2 LOAD-RETURN generation={} deferred until native queue is available result={} tid={} queueEnabled={} singleton=0x{:X}",
                        pending->generation,
                        Util::NativeMainThreadQueue::ToString(postResult),
                        diagnostics.currentThreadID, diagnostics.queueEnabled,
                        diagnostics.singleton);
                }
                ScheduleDeferredC2Retry();
            } catch (const std::exception& e) {
                {
                    const std::scoped_lock lock{ g_deferredC2LoadMutex };
                    if (g_deferredC2LoadInFlight == pending) {
                        g_deferredC2LoadInFlight.reset();
                    }
                }
                KillMutation("deferred load-return scheduling threw");
                REX::CRITICAL(
                    "[NpcAppearance] deferred C2 LOAD-RETURN scheduling threw '{}'; no mutation",
                    e.what());
            } catch (...) {
                {
                    const std::scoped_lock lock{ g_deferredC2LoadMutex };
                    if (g_deferredC2LoadInFlight == pending) {
                        g_deferredC2LoadInFlight.reset();
                    }
                }
                KillMutation("deferred load-return scheduling threw");
                REX::CRITICAL(
                    "[NpcAppearance] deferred C2 LOAD-RETURN scheduling threw; no mutation");
            }
        }

        void PumpDeferredC2SaveTask() noexcept
        {
            std::shared_ptr<DeferredC2SaveTask> pending;
            try {
                {
                    const std::scoped_lock lock{ g_deferredC2SaveMutex };
                    if (!g_deferredC2SaveTask || g_deferredC2SaveInFlight) {
                        return;
                    }
                    pending = g_deferredC2SaveTask;
                    g_deferredC2SaveInFlight = pending;
                }

                auto execute = [pending] {
                    try {
                        pending->run();
                    } catch (const std::exception& e) {
                        KillMutation("deferred save-return native task threw");
                        try {
                            REX::CRITICAL(
                                "[NpcAppearance] deferred C2 SAVE-RETURN sequence={} threw '{}' inside the verified drain; restored originals remain at rest",
                                pending->sequence, e.what());
                        } catch (...) {
                        }
                    } catch (...) {
                        KillMutation("deferred save-return native task threw");
                        try {
                            REX::CRITICAL(
                                "[NpcAppearance] deferred C2 SAVE-RETURN sequence={} threw inside the verified drain; restored originals remain at rest",
                                pending->sequence);
                        } catch (...) {
                        }
                    }
                    bool retry = false;
                    {
                        const std::scoped_lock lock{ g_deferredC2SaveMutex };
                        if (g_deferredC2SaveTask == pending) {
                            g_deferredC2SaveTask.reset();
                        }
                        retry = g_deferredC2SaveTask != nullptr;
                        if (g_deferredC2SaveInFlight == pending) {
                            g_deferredC2SaveInFlight.reset();
                        }
                    }
                    if (retry) {
                        ScheduleDeferredC2Retry();
                    }
                };

                const auto diagnostics =
                    Util::NativeMainThreadQueue::GetDiagnostics();
                if (diagnostics.insideDrain) {
                    execute();
                    return;
                }

                const auto postResult = Util::NativeMainThreadQueue::Post(
                    std::move(execute), "NpcAppearance.C2.SaveReapply",
                    [pending] {
                        bool retry = false;
                        {
                            const std::scoped_lock lock{ g_deferredC2SaveMutex };
                            if (g_deferredC2SaveInFlight == pending) {
                                g_deferredC2SaveInFlight.reset();
                            }
                            retry = g_deferredC2SaveTask != nullptr;
                        }
                        if (retry) {
                            ScheduleDeferredC2Retry();
                        }
                    });
                if (postResult ==
                    Util::NativeMainThreadQueue::PostResult::kQueued) {
                    REX::INFO(
                        "[NpcAppearance] C2 SAVE-RETURN sequence={} queued for verified native drain after queueDeferral={}",
                        pending->sequence, pending->deferralLogged);
                    return;
                }

                bool logDeferral = false;
                {
                    const std::scoped_lock lock{ g_deferredC2SaveMutex };
                    if (g_deferredC2SaveInFlight == pending) {
                        g_deferredC2SaveInFlight.reset();
                    }
                    if (g_deferredC2SaveTask == pending &&
                        !pending->deferralLogged) {
                        pending->deferralLogged = true;
                        logDeferral = true;
                    }
                }
                if (logDeferral) {
                    REX::INFO(
                        "[NpcAppearance] C2 SAVE-RETURN sequence={} deferred until native queue is available result={} tid={} queueEnabled={} singleton=0x{:X}; restored originals remain at rest",
                        pending->sequence,
                        Util::NativeMainThreadQueue::ToString(postResult),
                        diagnostics.currentThreadID, diagnostics.queueEnabled,
                        diagnostics.singleton);
                }
                ScheduleDeferredC2Retry();
            } catch (const std::exception& e) {
                {
                    const std::scoped_lock lock{ g_deferredC2SaveMutex };
                    if (g_deferredC2SaveInFlight == pending) {
                        g_deferredC2SaveInFlight.reset();
                    }
                }
                KillMutation("deferred save-return scheduling threw");
                REX::CRITICAL(
                    "[NpcAppearance] deferred C2 SAVE-RETURN scheduling threw '{}'; restored originals remain at rest",
                    e.what());
            } catch (...) {
                {
                    const std::scoped_lock lock{ g_deferredC2SaveMutex };
                    if (g_deferredC2SaveInFlight == pending) {
                        g_deferredC2SaveInFlight.reset();
                    }
                }
                KillMutation("deferred save-return scheduling threw");
                REX::CRITICAL(
                    "[NpcAppearance] deferred C2 SAVE-RETURN scheduling threw; restored originals remain at rest");
            }
        }

        [[nodiscard]] bool OnSaveGameEntryImpl() noexcept
        {
            if (!g_bracketArmed.load(std::memory_order_acquire)) {
                return true;
            }
            try {
                std::size_t tracked = 0;
                std::size_t restored = 0;
                {
                    const std::scoped_lock lock{ g_appliedBasesMutex };
                    tracked = g_appliedBases.size();
                    restored = g_saveEntryRestoredBases.size();
                }
                if (tracked == 0) {
                    REX::INFO(
                        "[NpcAppearance] C2 SAVE-ENTRY no tracked mutation; engine save allowed without a bracket tid={}",
                        ::GetCurrentThreadId());
                    return true;
                }
                g_saveHookObserved.store(true, std::memory_order_release);
                if (!SaveGatewayOperational()) {
                    KillMutation("save/load hook provider lost ownership at save entry");
                    REX::CRITICAL(
                        "[NpcAppearance] C2 SAVE-ENTRY save gateway is not operational; save veto requested");
                    return false;
                }
                if (!g_saveLoadEventRegistered.load(std::memory_order_acquire)) {
                    KillMutation("save entry arrived without SaveLoadEvent pre-save registration");
                    REX::CRITICAL(
                        "[NpcAppearance] C2 SAVE-ENTRY tracked={} but the pre-save event sink is not registered; save veto requested",
                        tracked);
                    return false;
                }

                const bool active =
                    g_inBracket.load(std::memory_order_acquire);
                const bool ready =
                    g_preSaveReady.exchange(false, std::memory_order_acq_rel);
                const bool reentrant =
                    g_saveGatewayEntered.exchange(true, std::memory_order_acq_rel);
                if (!active || !ready || reentrant || restored != tracked) {
                    g_saveGatewayEntered.store(false, std::memory_order_release);
                    g_preSaveGeneration.fetch_add(1, std::memory_order_acq_rel);
                    REX::CRITICAL(
                        "[NpcAppearance] C2 SAVE-ENTRY pre-save validation FAILED active={} ready={} reentrant={} tracked={} restored={}; SAVE VETO requested and engine gateway must not run",
                        active, ready, reentrant, tracked, restored);
                    return false;
                }

                REX::INFO(
                    "[NpcAppearance] C2 SAVE-ENTRY accepted pre-restored bracket entry={} tracked={} restored={} tid={}; engine gateway may run",
                    g_bracketSaveEntries.load(std::memory_order_relaxed),
                    tracked, restored, ::GetCurrentThreadId());
                return true;
            } catch (const std::exception& e) {
                REX::CRITICAL(
                    "[NpcAppearance] C2 SAVE-ENTRY callback threw '{}'; save veto requested",
                    e.what());
            } catch (...) {
                REX::CRITICAL(
                    "[NpcAppearance] C2 SAVE-ENTRY callback threw; save veto requested");
            }
            KillMutation("save-entry callback threw");
            return false;
        }

        void OnSaveGameReturnImpl() noexcept
        {
            if (!g_bracketArmed.load(std::memory_order_acquire)) {
                return;
            }
            try {
                g_saveGatewayEntered.store(false, std::memory_order_release);
                g_preSaveReady.store(false, std::memory_order_release);
                g_preSaveGeneration.fetch_add(1, std::memory_order_acq_rel);

                std::vector<AppliedBaseState> states;
                {
                    const std::scoped_lock lock{ g_appliedBasesMutex };
                    if (!g_inBracket.load(std::memory_order_acquire)) {
                        REX::INFO(
                            "[NpcAppearance] C2 SAVE-RETURN has no active pre-save bracket; no tracked mutation was serialized and reapply is unnecessary");
                        return;
                    }
                    states.reserve(g_saveEntryRestoredBases.size());
                    for (const auto baseID : g_saveEntryRestoredBases) {
                        const auto found = g_appliedBases.find(baseID);
                        if (found != g_appliedBases.end() &&
                            !found->second.bracketFailed) {
                            states.push_back(found->second);
                        }
                    }
                }

                const auto saveReturn =
                    g_bracketSaveReturns.fetch_add(1, std::memory_order_relaxed) + 1;
                const auto loadGeneration =
                    g_bracketLoadGeneration.load(std::memory_order_acquire);
                auto pending = std::make_shared<DeferredC2SaveTask>();
                pending->sequence = saveReturn;
                pending->loadGeneration = loadGeneration;
                pending->run = [saveReturn, loadGeneration,
                                states = std::move(states)] {
                        const auto finishBracket = [loadGeneration]() noexcept {
                            if (g_bracketLoadGeneration.load(
                                    std::memory_order_acquire) != loadGeneration) {
                                return;
                            }
                            try {
                                const std::scoped_lock lock{ g_appliedBasesMutex };
                                g_saveEntryRestoredBases.clear();
                                g_inBracket.store(false, std::memory_order_release);
                            } catch (...) {
                                KillMutation("save-return bracket cleanup threw");
                            }
                        };
                        try {
                            if (g_bracketLoadGeneration.load(
                                    std::memory_order_acquire) != loadGeneration) {
                                REX::WARN(
                                    "[NpcAppearance] C2 SAVE-RETURN return={} loadGeneration={} canceled after a newer load; no game-object access",
                                    saveReturn, loadGeneration);
                                return;
                            }
                            const auto diagnostics =
                                Util::NativeMainThreadQueue::GetDiagnostics();
                            if (!diagnostics.insideDrain) {
                                KillMutation("save-return reapply reached outside verified native drain");
                                finishBracket();
                                REX::CRITICAL(
                                    "[NpcAppearance] C2 SAVE-RETURN return={} reached outside verified native drain; no game-object access and restored originals remain at rest tid={} drainOwnerTid={}",
                                    saveReturn, diagnostics.currentThreadID,
                                    diagnostics.drainOwnerThreadID);
                                return;
                            }
                            if (!MutationOperational()) {
                                finishBracket();
                                REX::CRITICAL(
                                    "[NpcAppearance] C2 SAVE-RETURN return={} mutation disabled; restored originals remain at rest and reapply is skipped tid={} insideDrain={}",
                                    saveReturn, ::GetCurrentThreadId(),
                                    diagnostics.insideDrain);
                                return;
                            }

                            const auto totalStarted = std::chrono::steady_clock::now();
                            std::size_t reappliedCount = 0;
                            std::size_t failedCount = 0;
                            for (const auto& state : states) {
                                if (g_bracketLoadGeneration.load(
                                        std::memory_order_acquire) != loadGeneration) {
                                    REX::WARN(
                                        "[NpcAppearance] C2 SAVE-RETURN return={} canceled before target=0x{:08X} after a newer load",
                                        saveReturn, state.baseID);
                                    break;
                                }
                                bool reapplied = false;
                                RE::TESNPC* target = nullptr;
                                try {
                                    target = RE::TESForm::LookupByID<RE::TESNPC>(state.baseID);
                                    const LineSink out = [baseID = state.baseID](
                                                             const std::string& a_text) {
                                        REX::INFO(
                                            "[NpcAppearance] C2 reapply base=0x{:08X}: {}",
                                            baseID, a_text);
                                    };
                                    const auto actorResolution =
                                        target ? ResolveTargetActor(target) :
                                                 TargetActorResolution{};
                                    const bool hasLoaded3D =
                                        HasLoaded3D(actorResolution.actor);
                                    const bool refreshRequired =
                                        actorResolution.actor != nullptr && hasLoaded3D;
                                    const auto refreshAddress =
                                        REL::Relocation<std::uintptr_t>{
                                            kActorAppearanceRefreshID }.address();
                                    const bool refreshGateValid =
                                        !refreshRequired || HasExpectedBytes(
                                            refreshAddress,
                                            kActorAppearanceRefreshGate);
                                    NotifyNpcAppearanceChanged notify = nullptr;
                                    const bool notifyGateValid = target &&
                                        ResolveNpcAppearanceChanged(target, notify);
                                    if (!refreshGateValid) {
                                        KillMutation(
                                            "save-return refresh byte gate failed");
                                        REX::CRITICAL(
                                            "[NpcAppearance] C2 SAVE-RETURN target=0x{:08X} actorMatches={} highActors={} processListsValid={} refreshRequired=true refreshGate=false; exact original remains at rest and reapply is skipped",
                                            state.baseID, actorResolution.matches,
                                            actorResolution.highActors,
                                            actorResolution.processListsValid);
                                    }
                                    if (!notifyGateValid) {
                                        KillMutation(
                                            "save-return TESNPC appearance notification byte gate failed");
                                        REX::CRITICAL(
                                            "[NpcAppearance] C2 SAVE-RETURN target=0x{:08X} notifyGate=false; exact original remains at rest and reapply is skipped",
                                            state.baseID);
                                    }

                                    const bool silentlyApplied =
                                        refreshGateValid && notifyGateValid && target &&
                                        SilentApplyPresetToBase(
                                            out, target,
                                            state.assignment.presetPath);
                                    bool notified = false;
                                    bool actorRefreshed = !refreshRequired;
                                    if (silentlyApplied) {
                                        notified =
                                            NotifyBaseAppearanceChanged(target, 0x800) &&
                                            NotifyBaseAppearanceChanged(target, 0x4000);
                                        if (notified && refreshRequired) {
                                            reinterpret_cast<RefreshActorAppearance>(
                                                refreshAddress)(
                                                actorResolution.actor, false,
                                                0x28, false);
                                            actorRefreshed = true;
                                        }
                                    }
                                    reapplied = silentlyApplied && notified &&
                                        actorRefreshed;
                                    REX::INFO(
                                        "[NpcAppearance] C2 SAVE-RETURN return={} target=0x{:08X} actor=0x{:08X} actorMatches={} highActors={} processListsValid={} hasLoaded3D={} baseApplied={} notified={} refreshRequired={} actorRefreshed={} reapplied={}",
                                        saveReturn, state.baseID,
                                        actorResolution.actorRefID,
                                        actorResolution.matches,
                                        actorResolution.highActors,
                                        actorResolution.processListsValid,
                                        hasLoaded3D, silentlyApplied, notified,
                                        refreshRequired, actorRefreshed, reapplied);
                                    if (!reapplied && target &&
                                        refreshGateValid && notifyGateValid) {
                                        const bool safeOriginal =
                                            RestoreAppliedBaseState(out, target, state);
                                        REX::CRITICAL(
                                            "[NpcAppearance] C2 SAVE-RETURN target=0x{:08X} apply/refresh failed; exact-original fallback={}",
                                            state.baseID, safeOriginal);
                                        if (!safeOriginal) {
                                            KillMutation("save-return apply/refresh and exact-original fallback failed");
                                        }
                                    }
                                } catch (const std::exception& e) {
                                    REX::CRITICAL(
                                        "[NpcAppearance] C2 SAVE-RETURN target=0x{:08X} reapply threw '{}'; swallowed per target",
                                        state.baseID, e.what());
                                    try {
                                        const LineSink fallbackOut = [baseID = state.baseID](
                                                                         const std::string& a_text) {
                                            REX::INFO(
                                                "[NpcAppearance] C2 reapply fallback base=0x{:08X}: {}",
                                                baseID, a_text);
                                        };
                                        const bool safeOriginal = RestoreAppliedBaseState(
                                            fallbackOut, target, state);
                                        REX::CRITICAL(
                                            "[NpcAppearance] C2 SAVE-RETURN target=0x{:08X} exception fallback exactOriginal={}",
                                            state.baseID, safeOriginal);
                                        if (!safeOriginal) {
                                            KillMutation("save-return exception fallback failed");
                                        }
                                    } catch (...) {
                                        KillMutation("save-return exception fallback threw");
                                    }
                                } catch (...) {
                                    REX::CRITICAL(
                                        "[NpcAppearance] C2 SAVE-RETURN target=0x{:08X} reapply threw; swallowed per target",
                                        state.baseID);
                                    try {
                                        const LineSink fallbackOut = [baseID = state.baseID](
                                                                         const std::string& a_text) {
                                            REX::INFO(
                                                "[NpcAppearance] C2 reapply fallback base=0x{:08X}: {}",
                                                baseID, a_text);
                                        };
                                        const bool safeOriginal = RestoreAppliedBaseState(
                                            fallbackOut, target, state);
                                        REX::CRITICAL(
                                            "[NpcAppearance] C2 SAVE-RETURN target=0x{:08X} exception fallback exactOriginal={}",
                                            state.baseID, safeOriginal);
                                        if (!safeOriginal) {
                                            KillMutation("save-return exception fallback failed");
                                        }
                                    } catch (...) {
                                        KillMutation("save-return exception fallback threw");
                                    }
                                }

                                bool trackingLost = false;
                                {
                                    const std::scoped_lock lock{ g_appliedBasesMutex };
                                    const auto found = g_appliedBases.find(state.baseID);
                                    if (found != g_appliedBases.end()) {
                                        found->second.bracketFailed = !reapplied;
                                    } else {
                                        trackingLost = true;
                                    }
                                }
                                if (trackingLost && reapplied) {
                                    const LineSink fallbackOut = [baseID = state.baseID](
                                                                     const std::string& a_text) {
                                        REX::INFO(
                                            "[NpcAppearance] C2 reapply tracking fallback base=0x{:08X}: {}",
                                            baseID, a_text);
                                    };
                                    const bool safeOriginal =
                                        RestoreAppliedBaseState(
                                            fallbackOut, target, state);
                                    reapplied = false;
                                    KillMutation(
                                        safeOriginal ?
                                            "save-return applied base lost tracking; exact original restored" :
                                            "save-return applied base lost tracking and exact-original fallback failed");
                                }
                                if (reapplied) {
                                    ++reappliedCount;
                                } else {
                                    ++failedCount;
                                }
                            }

                            finishBracket();
                            const auto totalMs = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - totalStarted).count();
                            REX::INFO(
                                "[NpcAppearance] C2 SAVE-RETURN done return={} candidates={} reapplied={} failed={} tid={} insideDrain={} ms={:.3f}",
                                saveReturn, states.size(), reappliedCount, failedCount,
                                ::GetCurrentThreadId(), diagnostics.insideDrain, totalMs);
                        } catch (const std::exception& e) {
                            finishBracket();
                            KillMutation("save-return native task threw");
                            REX::CRITICAL(
                                "[NpcAppearance] C2 SAVE-RETURN native task threw '{}'; restored originals remain at rest",
                                e.what());
                        } catch (...) {
                            finishBracket();
                            KillMutation("save-return native task threw");
                            REX::CRITICAL(
                                "[NpcAppearance] C2 SAVE-RETURN native task threw; restored originals remain at rest");
                        }
                    };
                {
                    const std::scoped_lock lock{ g_deferredC2SaveMutex };
                    if (g_deferredC2SaveTask) {
                        if (g_deferredC2SaveTask->loadGeneration ==
                            loadGeneration) {
                            KillMutation("overlapping deferred save-return tasks");
                            REX::CRITICAL(
                                "[NpcAppearance] C2 SAVE-RETURN return={} found an existing deferred reapply sequence={} in the same load generation; restored originals remain at rest",
                                saveReturn, g_deferredC2SaveTask->sequence);
                            return;
                        }
                        REX::WARN(
                            "[NpcAppearance] C2 SAVE-RETURN return={} supersedes stale sequence={} from loadGeneration={}",
                            saveReturn, g_deferredC2SaveTask->sequence,
                            g_deferredC2SaveTask->loadGeneration);
                    }
                    g_deferredC2SaveTask = std::move(pending);
                }
                PumpDeferredC2SaveTask();
            } catch (const std::exception& e) {
                {
                    const std::scoped_lock lock{ g_appliedBasesMutex };
                    g_saveEntryRestoredBases.clear();
                }
                g_inBracket.store(false, std::memory_order_release);
                KillMutation("save-return callback threw");
                REX::CRITICAL(
                    "[NpcAppearance] C2 SAVE-RETURN callback threw '{}'; swallowed at callback boundary",
                    e.what());
            } catch (...) {
                {
                    const std::scoped_lock lock{ g_appliedBasesMutex };
                    g_saveEntryRestoredBases.clear();
                }
                g_inBracket.store(false, std::memory_order_release);
                KillMutation("save-return callback threw");
                REX::CRITICAL(
                    "[NpcAppearance] C2 SAVE-RETURN callback threw; swallowed at callback boundary");
            }
        }

        [[nodiscard]] constexpr bool IsC2SaveOperation(
            const RE::SaveLoadEvent::OpType a_op) noexcept
        {
            switch (a_op) {
            case RE::SaveLoadEvent::OpType::kAutosave:
            case RE::SaveLoadEvent::OpType::kQuicksave:
            case RE::SaveLoadEvent::OpType::kManualSave:
            case RE::SaveLoadEvent::OpType::kExitSaveToMainMenu:
            case RE::SaveLoadEvent::OpType::kExitSaveToDesktop:
                return true;
            default:
                return false;
            }
        }

        void OnC2SaveLoadEvent(const RE::SaveLoadEvent& a_event)
        {
            if (!g_bracketArmed.load(std::memory_order_acquire) ||
                !IsC2SaveOperation(a_event.opType)) {
                return;
            }

            try {
                const auto op = static_cast<std::uint32_t>(a_event.opType);
                const auto status = static_cast<std::uint32_t>(a_event.status);
                if (a_event.status == RE::SaveLoadEvent::Status::kBegin) {
                    g_preSaveReady.store(false, std::memory_order_release);
                    g_saveGatewayEntered.store(false, std::memory_order_release);
                    g_saveHookObserved.store(false, std::memory_order_release);
                    const auto generation =
                        g_preSaveGeneration.fetch_add(
                            1, std::memory_order_acq_rel) + 1;

                    std::vector<AppliedBaseState> states;
                    {
                        const std::scoped_lock lock{ g_appliedBasesMutex };
                        g_saveEntryRestoredBases.clear();
                        states.reserve(g_appliedBases.size());
                        for (auto& [baseID, state] : g_appliedBases) {
                            static_cast<void>(baseID);
                            state.bracketFailed = false;
                            states.push_back(state);
                        }
                    }
                    if (states.empty()) {
                        g_inBracket.store(false, std::memory_order_release);
                        REX::INFO(
                            "[NpcAppearance] C2 PRE-SAVE event op={} status={} generation={} has no tracked mutation; no bracket required tid={}",
                            op, status, generation, ::GetCurrentThreadId());
                        return;
                    }
                    if (g_inBracket.exchange(true, std::memory_order_acq_rel)) {
                        KillMutation("overlapping SaveLoadEvent pre-save brackets");
                        REX::CRITICAL(
                            "[NpcAppearance] C2 PRE-SAVE event op={} generation={} overlapped an active bracket; save will be vetoed",
                            op, generation);
                        return;
                    }

                    const auto entry =
                        g_bracketSaveEntries.fetch_add(
                            1, std::memory_order_relaxed) + 1;
                    const auto eventTid = ::GetCurrentThreadId();
                    REX::INFO(
                        "[NpcAppearance] C2 PRE-SAVE event BEGIN op={} generation={} entry={} targets={} eventTid={}; publishing restoration to verified native drain",
                        op, generation, entry, states.size(), eventTid);
                    const bool queued = QueueOrRunNativeTask(
                        [generation, entry, op, eventTid,
                         states = std::move(states)] {
                            if (g_preSaveGeneration.load(
                                    std::memory_order_acquire) != generation ||
                                !g_inBracket.load(std::memory_order_acquire)) {
                                REX::INFO(
                                    "[NpcAppearance] C2 PRE-SAVE op={} generation={} entry={} superseded before native restoration eventTid={} nativeTid={}; no mutation",
                                    op, generation, entry, eventTid,
                                    ::GetCurrentThreadId());
                                return;
                            }
                            if (!RestoreOperational()) {
                                REX::CRITICAL(
                                    "[NpcAppearance] C2 PRE-SAVE op={} generation={} entry={} restoration is not operational; save will be vetoed",
                                    op, generation, entry);
                                return;
                            }
                            const bool restored =
                                RestoreBasesForSave(entry, states);
                            const bool current =
                                g_preSaveGeneration.load(
                                    std::memory_order_acquire) == generation &&
                                g_inBracket.load(std::memory_order_acquire);
                            const bool gatewayOperational =
                                SaveGatewayOperational();
                            if (restored && current && gatewayOperational) {
                                g_preSaveReady.store(
                                    true, std::memory_order_release);
                                REX::INFO(
                                    "[NpcAppearance] C2 PRE-SAVE READY op={} generation={} entry={} targets={} eventTid={} nativeTid={}",
                                    op, generation, entry, states.size(),
                                    eventTid, ::GetCurrentThreadId());
                                return;
                            }
                            if (!restored) {
                                KillMutation("pre-save native restoration failed");
                            } else if (!gatewayOperational) {
                                KillMutation("save gateway lost ownership during pre-save restoration");
                            }
                            REX::CRITICAL(
                                "[NpcAppearance] C2 PRE-SAVE NOT READY op={} generation={} entry={} restored={} current={} gatewayOperational={}; save will be vetoed",
                                op, generation, entry, restored, current,
                                gatewayOperational);
                        },
                        "NpcAppearance.C2.PreSaveRestore",
                        [generation, entry] {
                            if (g_preSaveGeneration.load(
                                    std::memory_order_acquire) == generation) {
                                g_preSaveReady.store(
                                    false, std::memory_order_release);
                                try {
                                    REX::CRITICAL(
                                        "[NpcAppearance] C2 PRE-SAVE generation={} entry={} was dropped by the native queue; save will be vetoed without killing future restoration attempts",
                                        generation, entry);
                                } catch (...) {
                                }
                            }
                        });
                    if (!queued) {
                        REX::CRITICAL(
                            "[NpcAppearance] C2 PRE-SAVE op={} generation={} entry={} could not queue restoration; save will be vetoed",
                            op, generation, entry);
                    }
                    return;
                }

                if (a_event.status == RE::SaveLoadEvent::Status::kFailed) {
                    g_preSaveReady.store(false, std::memory_order_release);
                    g_preSaveGeneration.fetch_add(1, std::memory_order_acq_rel);
                    const bool hookObserved =
                        g_saveHookObserved.exchange(
                            false, std::memory_order_acq_rel);
                    const bool active =
                        g_inBracket.load(std::memory_order_acquire);
                    REX::INFO(
                        "[NpcAppearance] C2 PRE-SAVE event END op={} status={} failed active={} hookObserved={} elapsedMs={} fileSize={} tid={}",
                        op, status, active, hookObserved, a_event.elapsedMs,
                        a_event.fileSizeBytes, ::GetCurrentThreadId());
                    if (active && !hookObserved) {
                        REX::INFO(
                            "[NpcAppearance] C2 PRE-SAVE op={} failed before the save gateway; scheduling restoration-state reapply",
                            op);
                        OnSaveGameReturnImpl();
                    }
                    return;
                }

                if (a_event.status ==
                    RE::SaveLoadEvent::Status::kSaveCompleted) {
                    const bool hookObserved =
                        g_saveHookObserved.exchange(
                            false, std::memory_order_acq_rel);
                    REX::INFO(
                        "[NpcAppearance] C2 PRE-SAVE event END op={} status={} completed hookObserved={} elapsedMs={} fileSize={} tid={}",
                        op, status, hookObserved, a_event.elapsedMs,
                        a_event.fileSizeBytes, ::GetCurrentThreadId());
                }
            } catch (const std::exception& e) {
                KillMutation("SaveLoadEvent callback threw");
                g_preSaveReady.store(false, std::memory_order_release);
                REX::CRITICAL(
                    "[NpcAppearance] C2 SaveLoadEvent callback threw '{}'; save must fail closed",
                    e.what());
            } catch (...) {
                KillMutation("SaveLoadEvent callback threw");
                g_preSaveReady.store(false, std::memory_order_release);
                REX::CRITICAL(
                    "[NpcAppearance] C2 SaveLoadEvent callback threw; save must fail closed");
            }
        }

        class C2SaveLoadEventSink :
            public RE::BSTEventSink<RE::SaveLoadEvent>
        {
        public:
            static C2SaveLoadEventSink& GetSingleton() noexcept
            {
                static C2SaveLoadEventSink singleton;
                return singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::SaveLoadEvent& a_event,
                RE::BSTEventSource<RE::SaveLoadEvent>*) noexcept override
            {
                try {
                    OnC2SaveLoadEvent(a_event);
                } catch (const std::exception& e) {
                    KillMutation("SaveLoadEvent sink boundary threw");
                    try {
                        REX::CRITICAL(
                            "[NpcAppearance] C2 SaveLoadEvent sink boundary swallowed '{}'",
                            e.what());
                    } catch (...) {
                    }
                } catch (...) {
                    KillMutation("SaveLoadEvent sink boundary threw");
                    try {
                        REX::CRITICAL(
                            "[NpcAppearance] C2 SaveLoadEvent sink boundary swallowed an unknown exception");
                    } catch (...) {
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        void OnLoadGameReturnImpl() noexcept
        {
            if (!g_bracketArmed.load(std::memory_order_acquire)) {
                return;
            }
            try {
                const auto loadGeneration =
                    g_bracketLoadGeneration.fetch_add(
                        1, std::memory_order_acq_rel) + 1;
                g_inBracket.store(false, std::memory_order_release);
                g_preSaveReady.store(false, std::memory_order_release);
                g_saveGatewayEntered.store(false, std::memory_order_release);
                g_saveHookObserved.store(false, std::memory_order_release);
                g_preSaveGeneration.fetch_add(1, std::memory_order_acq_rel);

                std::vector<AppliedBaseState> staleStates;
                {
                    const std::scoped_lock lock{ g_appliedBasesMutex };
                    staleStates.reserve(g_appliedBases.size());
                    for (const auto& [baseID, state] : g_appliedBases) {
                        static_cast<void>(baseID);
                        staleStates.push_back(state);
                    }
                    g_saveEntryRestoredBases.clear();
                }
                std::shared_ptr<DeferredC2SaveTask> canceledSaveTask;
                {
                    const std::scoped_lock lock{ g_deferredC2SaveMutex };
                    canceledSaveTask = std::move(g_deferredC2SaveTask);
                    g_deferredC2SaveInFlight.reset();
                }
                if (canceledSaveTask) {
                    REX::WARN(
                        "[NpcAppearance] C2 LOAD-RETURN generation={} canceled deferred SAVE-RETURN sequence={} from loadGeneration={}",
                        loadGeneration, canceledSaveTask->sequence,
                        canceledSaveTask->loadGeneration);
                }

                const auto loadReturn =
                    g_bracketLoadReturns.fetch_add(1, std::memory_order_relaxed) + 1;
                // Overlay mode: the base is never left preset-mutated, so the
                // legacy persistent apply is skipped entirely. Stale-state
                // reconciliation still runs, and the deferred task finishes
                // with an overlay sweep once the blocking menus close.
                const bool overlayMode =
                    g_overlayModeEnabled.load(std::memory_order_acquire);
                std::vector<std::pair<RE::TESFormID, SelectedAssignment>> assignments;
                if (!overlayMode) {
                    const std::scoped_lock lock{ g_eventMutex };
                    assignments.reserve(g_sceneAssignments.size());
                    for (const auto& assignment : g_sceneAssignments) {
                        assignments.push_back(assignment);
                    }
                } else {
                    REX::INFO(
                        "[NpcAppearance] C2 LOAD-RETURN generation={} overlay mode active; legacy persistent apply skipped in favor of Set3d windows + post-load sweep",
                        loadGeneration);
                }

                auto pending = std::make_shared<DeferredC2LoadTask>();
                pending->generation = loadGeneration;
                pending->run = [loadReturn, loadGeneration, overlayMode,
                                staleStates = std::move(staleStates),
                                assignments = std::move(assignments)](
                                   const std::uint32_t attempt) {
                try {
                if (g_bracketLoadGeneration.load(std::memory_order_acquire) !=
                    loadGeneration) {
                    REX::WARN(
                        "[NpcAppearance] C2 LOAD-RETURN return={} generation={} superseded before native execution; no mutation",
                        loadReturn, loadGeneration);
                    return true;
                }
                const auto diagnostics =
                    Util::NativeMainThreadQueue::GetDiagnostics();
                auto* ui = RE::UI::GetSingleton();
                const bool menusBlockMutation = !ui ||
                    ui->IsMenuOpen(RE::BSFixedString{ "MainMenu" }) ||
                    ui->IsMenuOpen(RE::BSFixedString{ "LoadingMenu" });
                if (menusBlockMutation) {
                    if (attempt == 1 || (attempt % 60) == 0) {
                        REX::INFO(
                            "[NpcAppearance] C2 LOAD-RETURN return={} generation={} readiness WAIT attempt={} reason=blocking-menu tid={} insideDrain={}",
                            loadReturn, loadGeneration, attempt,
                            ::GetCurrentThreadId(), diagnostics.insideDrain);
                    }
                    return false;
                }

                bool staleStateClean = true;
                for (const auto& state : staleStates) {
                    bool exactOriginal = false;
                    try {
                        auto* target =
                            RE::TESForm::LookupByID<RE::TESNPC>(state.baseID);
                        const LineSink out = [baseID = state.baseID](
                                                 const std::string& a_text) {
                            REX::INFO(
                                "[NpcAppearance] C2 load reconciliation base=0x{:08X}: {}",
                                baseID, a_text);
                        };
                        exactOriginal = ExactOriginalState(target, state) ||
                            RestoreAppliedBaseState(out, target, state);
                    } catch (const std::exception& e) {
                        REX::CRITICAL(
                            "[NpcAppearance] C2 LOAD-RETURN generation={} stale target=0x{:08X} reconciliation threw '{}'",
                            loadGeneration, state.baseID, e.what());
                    } catch (...) {
                        REX::CRITICAL(
                            "[NpcAppearance] C2 LOAD-RETURN generation={} stale target=0x{:08X} reconciliation threw",
                            loadGeneration, state.baseID);
                    }

                    {
                        const std::scoped_lock lock{ g_appliedBasesMutex };
                        const auto found = g_appliedBases.find(state.baseID);
                        if (found != g_appliedBases.end()) {
                            if (exactOriginal) {
                                g_appliedBases.erase(found);
                            } else {
                                found->second.bracketFailed = true;
                            }
                        }
                    }
                    if (!exactOriginal) {
                        staleStateClean = false;
                        REX::CRITICAL(
                            "[NpcAppearance] C2 LOAD-RETURN generation={} stale target=0x{:08X} is not proven exact-original; state retained and saves remain veto-capable",
                            loadGeneration, state.baseID);
                    }
                }
                if (!staleStateClean) {
                    KillMutation("load-return stale base reconciliation failed");
                    return true;
                }

                if (!MutationOperational()) {
                    if (SaveGatewayOperational() &&
                        g_mutationKilled.load(std::memory_order_acquire)) {
                        REX::WARN(
                            "[NpcAppearance] C2 LOAD-RETURN return={} generation={} restored all stale bases but mutation remains killed; no new preset will be applied",
                            loadReturn, loadGeneration);
                    } else {
                        KillMutation("mutation lost before queued load-return work");
                        REX::CRITICAL(
                            "[NpcAppearance] C2 LOAD-RETURN return={} generation={} mutation is not operational inside native drain; no new mutation",
                            loadReturn, loadGeneration);
                    }
                    return true;
                }

                for (const auto& [expectedBaseID, assignment] : assignments) {
                    const LineSink quietOut = [](const std::string&) {};
                    auto* target = ResolveEligibleTarget(
                        quietOut, assignment.target);
                    if (!target || target->GetFormID() != expectedBaseID) {
                        KillMutation("load-return readiness target identity mismatch");
                        REX::CRITICAL(
                            "[NpcAppearance] C2 LOAD-RETURN return={} generation={} readiness FAILED target={} expectedBase=0x{:08X} resolvedBase={}; no mutation",
                            loadReturn, loadGeneration,
                            assignment.target.CanonicalKey(), expectedBaseID,
                            target ? std::format("0x{:08X}", target->GetFormID()) :
                                     std::string{ "<none>" });
                        return true;
                    }
                    NotifyNpcAppearanceChanged notify = nullptr;
                    if (!ResolveNpcAppearanceChanged(target, notify)) {
                        KillMutation(
                            "load-return readiness TESNPC appearance notification byte gate failed");
                        REX::CRITICAL(
                            "[NpcAppearance] C2 LOAD-RETURN return={} generation={} readiness FAILED notifyGate=false target=0x{:08X}; no mutation",
                            loadReturn, loadGeneration, expectedBaseID);
                        return true;
                    }
                    const auto actorResolution = ResolveTargetActor(target);
                    const bool hasLoaded3D =
                        HasLoaded3D(actorResolution.actor);
                    if (!actorResolution.actor || !hasLoaded3D) {
                        if (attempt == 1) {
                            REX::INFO(
                                "[NpcAppearance] C2 LOAD-RETURN return={} generation={} readiness target=0x{:08X} actorMatches={} highActors={} processListsValid={} hasLoaded3D={}; proceeding with apply+notify and no actor kick tid={} insideDrain={}",
                                loadReturn, loadGeneration, expectedBaseID,
                                actorResolution.matches,
                                actorResolution.highActors,
                                actorResolution.processListsValid,
                                hasLoaded3D,
                                ::GetCurrentThreadId(),
                                diagnostics.insideDrain);
                        }
                        continue;
                    }
                    const auto refreshAddress =
                        REL::Relocation<std::uintptr_t>{
                            kActorAppearanceRefreshID }.address();
                    if (!HasExpectedBytes(
                            refreshAddress, kActorAppearanceRefreshGate)) {
                        KillMutation("load-return readiness refresh byte gate failed");
                        REX::CRITICAL(
                            "[NpcAppearance] C2 LOAD-RETURN return={} generation={} readiness FAILED refreshGate=false; no mutation",
                            loadReturn, loadGeneration);
                        return true;
                    }
                }

                const auto totalStarted = std::chrono::steady_clock::now();
                std::size_t appliedCount = 0;
                std::size_t failedCount = 0;
                for (const auto& [expectedBaseID, assignment] : assignments) {
                    if (g_bracketLoadGeneration.load(
                            std::memory_order_acquire) != loadGeneration) {
                        REX::WARN(
                            "[NpcAppearance] C2 LOAD-RETURN return={} generation={} superseded before target=0x{:08X}; no further mutation",
                            loadReturn, loadGeneration, expectedBaseID);
                        break;
                    }
                    bool applied = false;
                    bool stateRecorded = false;
                    AppliedBaseState state;
                    RE::TESNPC* target = nullptr;
                    const auto finalizeFailedState = [expectedBaseID,
                                                      &stateRecorded](
                                                         const bool a_safeOriginal) {
                        if (!stateRecorded) {
                            return;
                        }
                        const std::scoped_lock lock{ g_appliedBasesMutex };
                        const auto found = g_appliedBases.find(expectedBaseID);
                        if (found == g_appliedBases.end()) {
                            return;
                        }
                        if (a_safeOriginal) {
                            g_appliedBases.erase(found);
                        } else {
                            found->second.bracketFailed = true;
                        }
                    };
                    try {
                        const LineSink out = [expectedBaseID](
                                                 const std::string& a_text) {
                            REX::INFO(
                                "[NpcAppearance] C2 load base=0x{:08X}: {}",
                                expectedBaseID, a_text);
                        };
                        target = ResolveEligibleTarget(out, assignment.target);
                        if (!target || target->GetFormID() != expectedBaseID) {
                            REX::CRITICAL(
                                "[NpcAppearance] C2 LOAD-RETURN winner target={} expectedBase=0x{:08X} resolvedBase={} mismatch; no mutation",
                                assignment.target.CanonicalKey(), expectedBaseID,
                                target ? std::format("0x{:08X}", target->GetFormID()) :
                                         std::string{ "<none>" });
                            ++failedCount;
                            continue;
                        }

                        const auto actorResolution = ResolveTargetActor(target);
                        const auto refreshAddress =
                            REL::Relocation<std::uintptr_t>{
                                kActorAppearanceRefreshID }.address();
                        const bool hasLoaded3D =
                            HasLoaded3D(actorResolution.actor);
                        const bool refreshRequired =
                            actorResolution.actor != nullptr && hasLoaded3D;
                        if (refreshRequired &&
                            !HasExpectedBytes(
                                refreshAddress, kActorAppearanceRefreshGate)) {
                            KillMutation("load-return refresh byte gate failed");
                            REX::CRITICAL(
                                "[NpcAppearance] C2 LOAD-RETURN target=0x{:08X} actorMatches={} highActors={} processListsValid={} refreshRequired=true refreshGate=false; no mutation",
                                expectedBaseID, actorResolution.matches,
                                actorResolution.highActors,
                                actorResolution.processListsValid);
                            ++failedCount;
                            continue;
                        }
                        NotifyNpcAppearanceChanged notify = nullptr;
                        if (!ResolveNpcAppearanceChanged(target, notify)) {
                            KillMutation(
                                "load-return TESNPC appearance notification byte gate failed");
                            REX::CRITICAL(
                                "[NpcAppearance] C2 LOAD-RETURN target=0x{:08X} notifyGate=false; no mutation",
                                expectedBaseID);
                            ++failedCount;
                            continue;
                        }

                        state = AppliedBaseState{
                            .baseID = expectedBaseID,
                            .assignment = assignment,
                            .originalVisual = CaptureOwnedVisualSnapshot(target),
                            .originalNonVisual = Snapshot(target),
                            .originalFaceNPC = target->faceNPC,
                            .originalActorFlags =
                                target->actorData.actorBaseFlags.underlying(),
                            .bracketFailed = true,
                        };
                        {
                            const std::scoped_lock lock{ g_appliedBasesMutex };
                            g_appliedBases.insert_or_assign(
                                expectedBaseID, state);
                            stateRecorded = true;
                        }
                        const bool silentlyApplied = SilentApplyPresetToBase(
                            out, target, assignment.presetPath);
                        bool notified = false;
                        bool actorRefreshed = !refreshRequired;
                        if (silentlyApplied) {
                            notified =
                                NotifyBaseAppearanceChanged(target, 0x800) &&
                                NotifyBaseAppearanceChanged(target, 0x4000);
                            if (notified && refreshRequired) {
                                reinterpret_cast<RefreshActorAppearance>(
                                    refreshAddress)(
                                    actorResolution.actor, false, 0x28, false);
                                actorRefreshed = true;
                            }
                        }
                        applied = silentlyApplied && notified && actorRefreshed;
                        if (!applied) {
                            const bool safeOriginal =
                                RestoreAppliedBaseState(out, target, state);
                            REX::CRITICAL(
                                "[NpcAppearance] C2 LOAD-RETURN target=0x{:08X} apply/refresh failed; exact-original fallback={}; failed state retained={}",
                                expectedBaseID, safeOriginal,
                                stateRecorded && !safeOriginal);
                            finalizeFailedState(safeOriginal);
                            if (!safeOriginal) {
                                KillMutation("load-return apply and exact-original fallback failed");
                            }
                        } else {
                            bool trackingLost = false;
                            {
                                const std::scoped_lock lock{ g_appliedBasesMutex };
                                const auto found = g_appliedBases.find(expectedBaseID);
                                if (found != g_appliedBases.end()) {
                                    found->second.bracketFailed = false;
                                } else {
                                    g_appliedBases.insert_or_assign(
                                        expectedBaseID, state);
                                    trackingLost = true;
                                    applied = false;
                                }
                            }
                            if (trackingLost) {
                                const bool safeOriginal =
                                    RestoreAppliedBaseState(out, target, state);
                                finalizeFailedState(safeOriginal);
                                KillMutation(
                                    safeOriginal ?
                                        "load-return applied base lost tracking; exact original restored" :
                                        "load-return applied base lost tracking and exact-original fallback failed");
                            }
                        }
                        REX::INFO(
                            "[NpcAppearance] C2 LOAD-RETURN return={} target=0x{:08X} actor=0x{:08X} actorMatches={} highActors={} processListsValid={} hasLoaded3D={} baseApplied={} notified={} refreshRequired={} actorRefreshed={} recorded={}",
                            loadReturn, expectedBaseID,
                            actorResolution.actorRefID, actorResolution.matches,
                            actorResolution.highActors,
                            actorResolution.processListsValid,
                            hasLoaded3D, silentlyApplied, notified,
                            refreshRequired, actorRefreshed, applied);
                    } catch (const std::exception& e) {
                        REX::CRITICAL(
                            "[NpcAppearance] C2 LOAD-RETURN target=0x{:08X} threw '{}'; swallowed per target with pre-write tracking retained until exact-original proof",
                            expectedBaseID, e.what());
                        bool safeOriginal = false;
                        if (target && state.baseID == expectedBaseID) {
                            try {
                                const LineSink fallbackOut = [expectedBaseID](
                                                                 const std::string& a_text) {
                                    REX::INFO(
                                        "[NpcAppearance] C2 load fallback base=0x{:08X}: {}",
                                        expectedBaseID, a_text);
                                };
                                safeOriginal = RestoreAppliedBaseState(
                                    fallbackOut, target, state);
                                REX::CRITICAL(
                                    "[NpcAppearance] C2 LOAD-RETURN target=0x{:08X} exception fallback exactOriginal={}",
                                    expectedBaseID, safeOriginal);
                                if (!safeOriginal) {
                                    KillMutation("load-return exception fallback failed");
                                }
                            } catch (...) {
                                KillMutation("load-return exception fallback threw");
                            }
                        }
                        finalizeFailedState(safeOriginal);
                    } catch (...) {
                        REX::CRITICAL(
                            "[NpcAppearance] C2 LOAD-RETURN target=0x{:08X} threw; swallowed per target with pre-write tracking retained until exact-original proof",
                            expectedBaseID);
                        bool safeOriginal = false;
                        if (target && state.baseID == expectedBaseID) {
                            try {
                                const LineSink fallbackOut = [expectedBaseID](
                                                                 const std::string& a_text) {
                                    REX::INFO(
                                        "[NpcAppearance] C2 load fallback base=0x{:08X}: {}",
                                        expectedBaseID, a_text);
                                };
                                safeOriginal = RestoreAppliedBaseState(
                                    fallbackOut, target, state);
                                REX::CRITICAL(
                                    "[NpcAppearance] C2 LOAD-RETURN target=0x{:08X} exception fallback exactOriginal={}",
                                    expectedBaseID, safeOriginal);
                                if (!safeOriginal) {
                                    KillMutation("load-return exception fallback failed");
                                }
                            } catch (...) {
                                KillMutation("load-return exception fallback threw");
                            }
                        }
                        finalizeFailedState(safeOriginal);
                    }
                    if (applied) {
                        ++appliedCount;
                    } else {
                        ++failedCount;
                    }
                    if (!MutationOperational()) {
                        break;
                    }
                }

                if (overlayMode) {
                    RunOverlaySweep("load-return");
                }

                const auto totalMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - totalStarted).count();
                std::size_t tracked = 0;
                {
                    const std::scoped_lock lock{ g_appliedBasesMutex };
                    tracked = g_appliedBases.size();
                }
                REX::INFO(
                    "[NpcAppearance] C2 LOAD-RETURN done return={} generation={} winners={} applied={} failed={} tracked={} overlayMode={} tid={} insideDrain={} ms={:.3f}",
                    loadReturn, loadGeneration, assignments.size(), appliedCount,
                    failedCount, tracked, overlayMode, ::GetCurrentThreadId(),
                    diagnostics.insideDrain, totalMs);
                return true;
            } catch (const std::exception& e) {
                KillMutation("load-return native task threw");
                REX::CRITICAL(
                    "[NpcAppearance] C2 LOAD-RETURN native task threw '{}'; swallowed inside verified drain",
                    e.what());
            } catch (...) {
                KillMutation("load-return native task threw");
                REX::CRITICAL(
                    "[NpcAppearance] C2 LOAD-RETURN native task threw; swallowed inside verified drain");
            }
                return true;
                };
                std::optional<std::uint64_t> supersededGeneration;
                {
                    const std::scoped_lock lock{ g_deferredC2LoadMutex };
                    if (g_deferredC2LoadTask) {
                        supersededGeneration =
                            g_deferredC2LoadTask->generation;
                    }
                    g_deferredC2LoadTask = std::move(pending);
                }
                if (supersededGeneration) {
                    REX::WARN(
                        "[NpcAppearance] C2 LOAD-RETURN generation={} superseded pending generation={}; successor will run after the in-flight identity retires",
                        loadGeneration, *supersededGeneration);
                }
                PumpDeferredC2LoadTask();
            } catch (const std::exception& e) {
                KillMutation("load-return callback scheduling threw");
                REX::CRITICAL(
                    "[NpcAppearance] C2 LOAD-RETURN scheduling threw '{}'; no mutation",
                    e.what());
            } catch (...) {
                KillMutation("load-return callback scheduling threw");
                REX::CRITICAL(
                    "[NpcAppearance] C2 LOAD-RETURN scheduling threw; no mutation");
            }
        }

        // ==================================================================
        // Startup
        // Fail-closed arming sequence: operational save/load hooks -> packs
        // directory -> validated winners -> pre-save event sink -> bracket.
        // ==================================================================
        void OnNpcAppearanceDataLoaded()
        {

            if (!MutationOperational()) {
                if (g_bracketOperational.load(std::memory_order_acquire) &&
                    !g_mutationKilled.load(std::memory_order_acquire) &&
                    !SaveLoadHooks::Operational()) {
                    KillMutation("save/load hook provider lost ownership before startup arming");
                }
                REX::CRITICAL(
                    "[NpcAppearance] startup mutation disabled because save/load hooks are not operational or the process kill switch is set; bracketOperational={} mutationKilled={}",
                    g_bracketOperational.load(std::memory_order_relaxed),
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
            RunScan(startupOut, { "npcapp", "scan" });

            std::size_t assignments = 0;
            {
                const std::scoped_lock lock{ g_eventMutex };
                assignments = g_sceneAssignments.size();
            }
            if (assignments == 0) {
                REX::WARN("[NpcAppearance] startup found no fully validated winning assignments; save/load bracket remains disabled");
                return;
            }

            auto* saveLoadSource = RE::SaveLoadEvent::GetEventSource();
            if (!saveLoadSource) {
                KillMutation("SaveLoadEvent source unavailable during bracket arming");
                REX::CRITICAL(
                    "[NpcAppearance] save/load bracket could not register the pre-save event sink; mutation disabled and saves with tracked state will be vetoed");
                return;
            }
            if (!g_saveLoadEventRegistered.load(std::memory_order_acquire)) {
                saveLoadSource->RegisterSink(
                    &C2SaveLoadEventSink::GetSingleton());
                g_saveLoadEventRegistered.store(
                    true, std::memory_order_release);
            }
            // The overlay trigger registers unconditionally; its handler
            // no-ops until `npcapp overlay on` flips the mode atomic, which
            // keeps the toggle safe from any thread. Absence is non-fatal:
            // overlay stays unusable while the legacy bracket path is intact.
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
                        "[NpcAppearance] ReferenceSet3d event source unavailable; overlay mode will have no trigger this session");
                }
            }
            g_bracketArmed.store(true, std::memory_order_release);
            REX::INFO(
                "[NpcAppearance] save/load bracket ARMED assignments={} autoArm=true saveLoadEventRegistered={}; load-side recipe=base apply + notify + loaded-actor refresh",
                assignments,
                g_saveLoadEventRegistered.load(std::memory_order_relaxed));
        }

        void RunBracketStatus(const LineSink& a_out)
        {
            std::size_t assignments = 0;
            {
                const std::scoped_lock lock{ g_eventMutex };
                assignments = g_sceneAssignments.size();
            }
            std::size_t appliedBases = 0;
            std::size_t failedBases = 0;
            std::size_t restoredBases = 0;
            {
                const std::scoped_lock lock{ g_appliedBasesMutex };
                appliedBases = g_appliedBases.size();
                failedBases = static_cast<std::size_t>(std::ranges::count_if(
                    g_appliedBases, [](const auto& a_entry) {
                        return a_entry.second.bracketFailed;
                    }));
                restoredBases = g_saveEntryRestoredBases.size();
            }
            bool loadPending = false;
            bool loadInFlight = false;
            {
                const std::scoped_lock lock{ g_deferredC2LoadMutex };
                loadPending = g_deferredC2LoadTask != nullptr;
                loadInFlight = static_cast<bool>(g_deferredC2LoadInFlight);
            }
            bool savePending = false;
            bool saveInFlight = false;
            {
                const std::scoped_lock lock{ g_deferredC2SaveMutex };
                savePending = g_deferredC2SaveTask != nullptr;
                saveInFlight = static_cast<bool>(g_deferredC2SaveInFlight);
            }
            const auto nativeDiagnostics =
                Util::NativeMainThreadQueue::GetDiagnostics();
            a_out(std::format(
                "bracket: operational={} veto={} armed={} mutationKilled={} inBracket={} assignments={} appliedBases={} failedBases={} restoredAtSaveEntry={} preSaveReady={} gatewayEntered={} hookObserved={} entries={} saveReturns={} loadReturns={} loadGeneration={}",
                g_bracketOperational.load(std::memory_order_relaxed),
                SaveLoadHooks::SupportsSaveVeto(),
                g_bracketArmed.load(std::memory_order_relaxed),
                g_mutationKilled.load(std::memory_order_relaxed),
                g_inBracket.load(std::memory_order_relaxed),
                assignments, appliedBases, failedBases, restoredBases,
                g_preSaveReady.load(std::memory_order_relaxed),
                g_saveGatewayEntered.load(std::memory_order_relaxed),
                g_saveHookObserved.load(std::memory_order_relaxed),
                g_bracketSaveEntries.load(std::memory_order_relaxed),
                g_bracketSaveReturns.load(std::memory_order_relaxed),
                g_bracketLoadReturns.load(std::memory_order_relaxed),
                g_bracketLoadGeneration.load(std::memory_order_relaxed)));
            a_out(std::format(
                "bracket: loadPending={} loadInFlight={} savePending={} saveInFlight={} retryScheduled={} insideDrain={} queueEnabled={} nativeTid={}",
                loadPending, loadInFlight, savePending, saveInFlight,
                g_deferredC2RetryScheduled.load(std::memory_order_relaxed),
                nativeDiagnostics.insideDrain,
                nativeDiagnostics.queueEnabled,
                nativeDiagnostics.currentThreadID));
        }

    }

    void Initialize() noexcept
    {
        try {
        g_bracketArmed.store(false, std::memory_order_release);
        const SaveLoadHooks::Callbacks callbacks{
            .onSaveGameEntry = &OnSaveGameEntryImpl,
            .onSaveGameReturn = &OnSaveGameReturnImpl,
            .onLoadGameReturn = &OnLoadGameReturnImpl,
        };
        const bool hooksInstalled = SaveLoadHooks::Install(callbacks);
        g_bracketOperational.store(hooksInstalled, std::memory_order_release);
        if (!hooksInstalled) {
            KillMutation("SaveGame/LoadGame hook installation failed");
        }
        const bool saveVetoSupported = SaveLoadHooks::SupportsSaveVeto();
        const bool deferredRetryAvailable = SFSE::GetTaskInterface() != nullptr;
        if (hooksInstalled && !saveVetoSupported) {
            KillMutation(
                "production bracket requires save veto support so failed restoration cannot serialize");
        }
        if (!deferredRetryAvailable) {
            KillMutation(
                "SFSE task interface unavailable for demand-driven bracket retries");
        }
        REX::INFO(
            "[NpcAppearance] save/load hook state operational={} saveVetoSupported={} deferredRetryAvailable={} mutationKilled={} bracketArmed={} autoArmPending={} callbacks=native-queue-shaped",
            g_bracketOperational.load(std::memory_order_relaxed),
            saveVetoSupported, deferredRetryAvailable,
            g_mutationKilled.load(std::memory_order_relaxed),
            g_bracketArmed.load(std::memory_order_relaxed),
            MutationOperational() && saveVetoSupported);
        try {
            if (!QueueOrRunNativeTask(
                    [] { OnNpcAppearanceDataLoaded(); },
                    "NpcAppearance.StartupScan",
                    [] {
                        KillMutation("startup scan payload was dropped by the native queue");
                        try {
                            REX::CRITICAL(
                                "[NpcAppearance] startup scan payload was dropped before verified native execution; mutation remains fail closed");
                        } catch (...) {
                        }
                    })) {
                KillMutation("startup scan could not enter the verified native queue");
            }
        } catch (const std::exception& e) {
            KillMutation("startup scan scheduling threw");
            REX::CRITICAL(
                "[NpcAppearance] startup scan scheduling threw '{}'; no mutation",
                e.what());
        } catch (...) {
            KillMutation("startup scan scheduling threw");
            REX::CRITICAL(
                "[NpcAppearance] startup scan scheduling threw; no mutation");
        }
        } catch (const std::exception& e) {
            KillMutation("initialization boundary threw");
            try {
                REX::CRITICAL(
                    "[NpcAppearance] initialization boundary swallowed '{}'",
                    e.what());
            } catch (...) {
            }
        } catch (...) {
            KillMutation("initialization boundary threw");
            try {
                REX::CRITICAL(
                    "[NpcAppearance] initialization boundary swallowed an unknown exception");
            } catch (...) {
            }
        }
    }

    void FailClosed(const std::string_view a_reason) noexcept
    {
        KillMutation(a_reason);
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
            RunTargetTrial(a_out, a_args);
        } else if (a_args[1] == "bracket") {
            RunBracketStatus(a_out);
        } else if (a_args[1] == "copyref") {
            RunCopyRef(a_out, a_args);
        } else if (a_args[1] == "overlay") {
            RunOverlayMode(a_out, a_args);
        } else if (a_args[1] == "probebaseline") {
            RunProbeBaseline(a_out, a_args);
        } else if (a_args[1] == "probecompare") {
            RunProbeCompare(a_out, a_args);
        } else if (a_args[1] == "probe97401") {
            RunProbe97401(a_out, a_args);
        } else if (a_args[1] == "probetransient") {
            RunProbeTransient(a_out, a_args);
        } else if (a_args[1] == "probeset3d") {
            RunProbeSet3d(a_out, a_args);
        } else {
            a_out("npcapp: status|bracket|selftest|scan [packsRoot]|inspect <npc>|resolve <plugin:localFormID>|refs <plugin:localFormID> <npc>|avm <plugin:localFormID> <npc>|donor [count]|donorseed <plugin:localFormID> <npc>|donormorph <plugin:localFormID> <npc>|donorvisual <plugin:localFormID> <npc>|donorcopy <plugin:localFormID> <npc>|targettrial <plugin:localFormID> <actorRefID> <npc>|copyref <targetRefID> <sourceRefID> [0|1]|overlay [status|on|off|sweep]|probebaseline <plugin:localFormID>|probecompare <plugin:localFormID>|probe97401 <targetRefID> <sourceRefID> [restore=0|1]|probetransient <plugin:localFormID> <actorRefID> <npc>|probeset3d [on|off|status]");
        }
    }
}
