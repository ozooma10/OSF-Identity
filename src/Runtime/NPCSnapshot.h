#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Runtime
{
    struct NonVisualState
    {
        std::string         editorID;
        std::string         name;
        std::uint16_t       level{ 0 };
        std::uint16_t       calcLevelMin{ 0 };
        std::uint16_t       calcLevelMax{ 0 };
        std::uint16_t       baseDisposition{ 0 };
        std::uint16_t       templateUseFlags{ 0 };
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

        [[nodiscard]] bool operator==(const NonVisualState&) const = default;
    };

    struct VisualStorageState
    {
        std::size_t headPartCount{ 0 };
        const void* headPartStorage{ nullptr };
        std::size_t morphRegionCount{ 0 };
        const void* morphRegionStorage{ nullptr };
        std::size_t boneValueCount{ 0 };
        const void* boneValueStorage{ nullptr };
        std::size_t boneGroupCount{ 0 };
        const void* boneGroupStorage{ nullptr };
        std::size_t tintCount{ 0 };
        const void* tintStorage{ nullptr };
        std::size_t shapeBlendCount{ 0 };
        const void* shapeBlendStorage{ nullptr };
    };

    struct OriginalNPCState
    {
        NonVisualState nonVisual;
        RE::TESNPC*    faceNPC{ nullptr };
        std::uint32_t  actorFlags{ 0 };
    };

    [[nodiscard]] NonVisualState CaptureNonVisualState(RE::TESNPC* a_npc);
    [[nodiscard]] VisualStorageState CaptureVisualStorageState(RE::TESNPC* a_npc);
    [[nodiscard]] OriginalNPCState CaptureOriginalNPCState(RE::TESNPC* a_npc);

    [[nodiscard]] bool HasIndependentVisualStorage(const VisualStorageState& a_source, const VisualStorageState& a_donor);

    [[nodiscard]] bool SameExactVisualValues(RE::TESNPC* a_left, RE::TESNPC* a_right);

    [[nodiscard]] bool SameExactOriginalState(RE::TESNPC* a_target, RE::TESNPC* a_restoreDonor, const OriginalNPCState& a_original);
}
