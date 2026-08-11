#pragma once

#include "NpcAppearance/Config.h"

#include "RE/Starfield.h"

#include "Util/StarfieldRuntime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Shared internals of the overlay runtime pipeline
// Snapshot.cpp (byte-exact base-NPC capture/compare), Donor.cpp (preset ->
// donor population, silent apply, owned restore), TargetScan.cpp
// (validation-only pack scan), and Runtime.cpp (arming state, overlay
// windows, startup). Not part of the public API: Runtime.h stays
// Initialize-only.
namespace NpcAppearance::Detail
{
    // Per-line log emitter; production sinks are REX::INFO adapters that
    // carry a context prefix ("startup: ", per-actor base ID, ...).
    using LineSink = std::function<void(const std::string&)>;

    [[nodiscard]] inline const char* SafeText(const char* a_text) noexcept
    {
        return a_text ? a_text : "";
    }

    // ==================================================================
    // Native byte contracts
    // Address Library IDs and the expected prologue bytes of every
    // native routine the runtime may call on 1.16.244. Each call site
    // re-verifies these at runtime; any mismatch fails the apply closed.
    // ==================================================================
    inline constexpr REL::ID kNpcFactorySingletonID{
        RE::ID::TESNPCFormFactory::Singleton };
    inline constexpr REL::ID kNpcFactoryVtableID{
        RE::VTABLE::ConcreteBoundObjectFormFactory_TESNPC_50_13_0_[0] };
    inline constexpr REL::ID kNpcFactoryCreateID{
        RE::ID::TESNPCFormFactory::Create };
    inline constexpr REL::ID kNpcPrimaryVtableID{ RE::TESNPC::PRIMARY_VTABLE };
    inline constexpr REL::ID kNpcScalarDeletingDestructorID{
        RE::ID::TESNPC::ScalarDeletingDestructor };
    inline constexpr REL::ID kNpcCopyAppearanceID{
        RE::ID::TESNPC::CopyAppearance };
    inline constexpr REL::ID kNpcSetShapeBlendID{ 68207 };
    inline constexpr REL::ID kNpcSetBodyMorphID{ RE::ID::TESNPC::SetBodyMorph };
    inline constexpr REL::ID kNpcSetBoneValueID{ 68210 };
    inline constexpr REL::ID kNpcSetBoneGroupValueID{ 68212 };
    inline constexpr REL::ID kNpcRemoveHeadPartID{ 68188 };
    inline constexpr REL::ID kNpcChangeHeadPartID{ 68189 };
    inline constexpr REL::ID kFaceDbResolveEntryID{ 37340 };
    inline constexpr REL::ID kNpcSetAvmDataID{ 68087 };
    inline constexpr REL::ID kNpcRemoveAvmDataID{ 68088 };
    inline constexpr REL::ID kActorAppearanceRefreshID{ 101307 };
    inline constexpr std::uintptr_t kProcessListsVtableRva = 0x4CC01B0;
    inline constexpr std::uintptr_t kActorVtableRva = 0x4CB9248;
    inline constexpr REL::ID kNpcOwnedVisualCopyID{
        RE::ID::TESNPC::CopyOwnedAppearance };
    inline constexpr std::array<std::uint8_t, 16> kNpcFactoryCreateGate{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
        0xEC, 0x30, 0x8B, 0xDA, 0xB9, 0x58, 0x04, 0x00
    };
    inline constexpr std::array<std::uint8_t, 16> kNpcDestructorGate{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
        0xEC, 0x20, 0x8B, 0xDA, 0x48, 0x8B, 0xF9, 0xE8
    };
    inline constexpr std::array<std::uint8_t, 16> kNpcCopyAppearanceGate{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
        0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57
    };
    inline constexpr std::array<std::uint8_t, 16> kNpcSetShapeBlendGate{
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48,
        0x89, 0x68, 0x20, 0xC5, 0xFA, 0x11, 0x50, 0x18
    };
    inline constexpr std::array<std::uint8_t, 16> kNpcSetBodyMorphGate{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
        0x24, 0x10, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83
    };
    inline constexpr std::array<std::uint8_t, 16> kNpcSetBoneValueGate{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x89, 0x54, 0x24,
        0x10, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30
    };
    inline constexpr std::array<std::uint8_t, 16> kNpcSetBoneGroupValueGate{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0xC5, 0xFA, 0x11,
        0x5C, 0x24, 0x20, 0x89, 0x54, 0x24, 0x10, 0x55
    };
    inline constexpr std::array<std::uint8_t, 16> kNpcRemoveHeadPartGate{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
        0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57
    };
    inline constexpr std::array<std::uint8_t, 16> kNpcChangeHeadPartGate{
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x54,
        0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41
    };
    inline constexpr std::array<std::uint8_t, 16> kFaceDbResolveEntryGate{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
        0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x8B
    };
    inline constexpr std::array<std::uint8_t, 16> kNpcSetAvmDataGate{
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83,
        0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9
    };
    inline constexpr std::array<std::uint8_t, 16> kNpcRemoveAvmDataGate{
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
        0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48
    };
    inline constexpr std::array<std::uint8_t, 16> kNpcOwnedVisualCopyGate{
        0x44, 0x88, 0x44, 0x24, 0x18, 0x53, 0x56, 0x57,
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
    };
    inline constexpr std::array<std::uint8_t, 16> kActorAppearanceRefreshGate{
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
        0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57
    };
    // TESNPC primary vtable slot 0x17, proven identical on Starfield
    // 1.16.242 and 1.16.244 from the unpacked executable images.
    inline constexpr std::array<std::uint8_t, 16> kNpcAppearanceChangedGate{
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83,
        0xEC, 0x20, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0x0F
    };

    template <std::size_t N>
    [[nodiscard]] bool HasExpectedBytes(const std::uintptr_t a_address,
                                        const std::array<std::uint8_t, N>& a_expected)
    {
        return Util::IsReadableRange(a_address, a_expected.size()) &&
               std::memcmp(reinterpret_cast<const void*>(a_address),
                           a_expected.data(), a_expected.size()) == 0;
    }

    // ==================================================================
    // Runtime arming state, owned by Runtime.cpp. The SaveLoadEvent sink
    // is observer-only telemetry: it is not load-bearing for mutation
    // safety, so none of these gates consult it. Correctness comes from
    // the per-call byte gates, the verified drain, and the overlay
    // window's restore proof.
    // ==================================================================
    [[nodiscard]] bool MutationOperational() noexcept;
    [[nodiscard]] bool RestoreOperational() noexcept;
    void KillMutation(std::string_view a_reason) noexcept;
    [[nodiscard]] bool RequireMutationOperational(
        const LineSink& a_out, std::string_view a_operation);
    [[nodiscard]] bool RequireRestoreOperational(
        const LineSink& a_out, std::string_view a_operation);

    // ==================================================================
    // Snapshots, defined in Snapshot.cpp
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

    [[nodiscard]] NonVisualSnapshot Snapshot(RE::TESNPC* a_npc);
    [[nodiscard]] VisualSeedSnapshot SnapshotVisualSeed(RE::TESNPC* a_npc);
    [[nodiscard]] bool HasIndependentVisualStorage(const VisualSeedSnapshot& a_source,
                                                   const VisualSeedSnapshot& a_donor);
    [[nodiscard]] bool SameExactVisualValues(RE::TESNPC* a_left, RE::TESNPC* a_right);
    [[nodiscard]] OwnedVisualSnapshot CaptureOwnedVisualSnapshot(RE::TESNPC* a_npc);
    [[nodiscard]] bool SameExactVisualValues(
        RE::TESNPC* a_npc, const OwnedVisualSnapshot& a_snapshot);

    // ==================================================================
    // Preset apply / owned restore, defined in Donor.cpp
    // ==================================================================

    // Proven production apply pipeline. This mutates only the TESNPC base:
    // no notifications, actor refresh, or retained donors.
    [[nodiscard]] bool SilentApplyPresetToBase(
        const LineSink& a_out,
        RE::TESNPC* a_target,
        const std::filesystem::path& a_path);

    [[nodiscard]] bool RestoreOwnedVisualSnapshot(
        const LineSink& a_out,
        RE::TESNPC* a_target,
        const OwnedVisualSnapshot& a_snapshot,
        RE::TESNPC* a_originalFaceNPC);

    // ==================================================================
    // Target resolution + pack scan, defined in TargetScan.cpp
    // Validation only: resolves packs to eligible unique HumanRace
    // targets and returns the winning assignments for the caller to
    // publish; performs no game-object mutation.
    // ==================================================================
    [[nodiscard]] std::unordered_map<RE::TESFormID, SelectedAssignment> RunScan(
        const LineSink& a_out, const std::filesystem::path& a_packsRoot);
}
