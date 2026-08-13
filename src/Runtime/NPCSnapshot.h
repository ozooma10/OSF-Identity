#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

        bool operator==(const VisualStorageState&) const = default;
    };

    struct OriginalNPCState
    {
        NonVisualState nonVisual;
        RE::TESNPC*    faceNPC{ nullptr };
        std::uint32_t  actorFlags{ 0 };

        bool operator==(const OriginalNPCState&) const = default;
    };

    struct HeadPartGraphNodeState
    {
        RE::BGSHeadPart*              headPart{ nullptr };
        RE::TESFormID                 formID{ 0 };
        std::uint32_t                 type{ 0 };
        const void*                   extraPartStorage{ nullptr };
        std::size_t                   extraPartCapacity{ 0 };
        std::vector<RE::BGSHeadPart*> extraParts;

        bool operator==(const HeadPartGraphNodeState&) const = default;
    };

    // Exact process-lifetime structure expected by FaceDB after a detached source is frozen.
    // Refcount is deliberately excluded because transient engine owners may change it.
    struct RenderSourceStructureState
    {
        RE::TESNPC*                       source{ nullptr };
        const void*                       headPartStorage{ nullptr };
        std::size_t                       headPartCapacity{ 0 };
        std::vector<RE::BGSHeadPart*>     headParts;
        std::vector<HeadPartGraphNodeState> headPartGraph;

        bool operator==(const RenderSourceStructureState&) const = default;
    };

    NonVisualState CaptureNonVisualState(RE::TESNPC* a_npc);
    VisualStorageState CaptureVisualStorageState(RE::TESNPC* a_npc);
    OriginalNPCState CaptureOriginalNPCState(RE::TESNPC* a_npc);

    // Validates that every reachable head part is a registered form and captures the exact root/extra-part graph without retaining ownership of global head-part forms.
    [[nodiscard]] std::optional<RenderSourceStructureState> CaptureRenderSourceStructure(RE::TESNPC* a_source) noexcept;
    [[nodiscard]] bool MatchesRenderSourceStructure(RE::TESNPC* a_source, const RenderSourceStructureState& a_expected) noexcept;

    bool HasIndependentVisualStorage(const VisualStorageState& a_source, const VisualStorageState& a_donor);

    bool SameExactVisualValues(RE::TESNPC* a_left, RE::TESNPC* a_right);

}
