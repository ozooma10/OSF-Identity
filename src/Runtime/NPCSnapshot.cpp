#include "NPCSnapshot.h"

#include "Util/String.h"

#include <cstring>

namespace Runtime
{
    namespace
    {
        using Util::SafeText;

        constexpr std::size_t kMaxRootHeadParts = 64;
        constexpr std::size_t kMaxHeadPartCapacity = 256;
        constexpr std::size_t kMaxExtraPartsPerHeadPart = 64;
        constexpr std::size_t kMaxExtraPartCapacity = 256;
        constexpr std::size_t kMaxReachableHeadParts = 256;

        [[nodiscard]] bool IndependentStorage(
            const std::size_t a_count,
            const void* a_sourceStorage,
            const void* a_donorStorage)
        {
            if (a_sourceStorage && a_donorStorage == a_sourceStorage) {
                return false;
            }
            return a_count == 0 || (a_sourceStorage && a_donorStorage);
        }
    }

    NonVisualState CaptureNonVisualState(RE::TESNPC* a_npc)
    {
        NonVisualState state;
        state.editorID = SafeText(a_npc->GetFormEditorID());
        state.name = SafeText(a_npc->GetFullName());
        state.level = a_npc->actorData.level;
        state.calcLevelMin = a_npc->actorData.calcLevelMin;
        state.calcLevelMax = a_npc->actorData.calcLevelMax;
        state.baseDisposition = a_npc->actorData.baseDisposition;
        state.templateUseFlags = a_npc->actorData.templateUseFlags.underlying();
        state.factionCount = a_npc->factions.size();
        state.factionData = a_npc->factions.data();
        state.inventoryCount = a_npc->containerObjects.size();
        state.inventoryData = a_npc->containerObjects.data();
        state.race = a_npc->formRace;
        state.originalRace = a_npc->originalRace;
        state.npcClass = a_npc->npcClass;
        state.voiceType = a_npc->voiceType;
        state.combatStyle = a_npc->combatStyle;
        state.defaultOutfit = a_npc->defaultOutfit;
        state.sleepOutfit = a_npc->sleepOutfit;
        state.crimeFaction = a_npc->crimeFaction;
        std::memcpy(state.aiData.data(), &a_npc->aiData, sizeof(a_npc->aiData));
        return state;
    }

    VisualStorageState CaptureVisualStorageState(RE::TESNPC* a_npc)
    {
        VisualStorageState state;
        {
            auto headParts = a_npc->headParts.Lock();
            state.headPartCount = (*headParts).size();
            state.headPartStorage = (*headParts).data();
        }
        if (a_npc->bodyMorphValues) {
            state.morphRegionCount = a_npc->bodyMorphValues->size();
            state.morphRegionStorage = a_npc->bodyMorphValues;
        }
        if (a_npc->facialBoneValues) {
            state.boneValueCount = a_npc->facialBoneValues->size();
            state.boneValueStorage = a_npc->facialBoneValues;
        }
        if (a_npc->facialBoneGroupValues) {
            state.boneGroupCount = a_npc->facialBoneGroupValues->size();
            state.boneGroupStorage = a_npc->facialBoneGroupValues;
        }
        state.tintCount = a_npc->tintAVMData.size();
        state.tintStorage = a_npc->tintAVMData.data();
        if (a_npc->shapeBlendData) {
            state.shapeBlendCount = a_npc->shapeBlendData->size();
            state.shapeBlendStorage = a_npc->shapeBlendData;
        }
        return state;
    }

    OriginalNPCState CaptureOriginalNPCState(RE::TESNPC* a_npc)
    {
        return OriginalNPCState{
            .nonVisual = CaptureNonVisualState(a_npc),
            .faceNPC = a_npc->faceNPC,
            .actorFlags = a_npc->actorData.actorBaseFlags.underlying(),
        };
    }

    std::optional<RenderSourceStructureState> CaptureRenderSourceStructure(RE::TESNPC* a_source) noexcept
    {
        if (!a_source || a_source->GetFormID() != 0)
        {
            REX::WARN("[NPCSnapshot] detached render-source structure rejected: source={} formID=0x{:08X}",
                      static_cast<const void*>(a_source), a_source ? a_source->GetFormID() : 0);
            return std::nullopt;
        }

        try
        {
            RenderSourceStructureState state;
            state.source = a_source;
            {
                auto locked = a_source->headParts.Lock();
                const auto count = static_cast<std::size_t>((*locked).size());
                const auto capacity = static_cast<std::size_t>((*locked).capacity());
                auto* storage = (*locked).data();
                if (count > capacity || count > kMaxRootHeadParts || capacity > kMaxHeadPartCapacity || (count != 0 && !storage))
                {
                    REX::WARN("[NPCSnapshot] detached render-source head-part array rejected: count={} capacity={} storage={}",
                              count, capacity, static_cast<const void*>(storage));
                    return std::nullopt;
                }
                state.headPartStorage = storage;
                state.headPartCapacity = capacity;
                if (count != 0)
                {
                    state.headParts.assign((*locked).begin(), (*locked).end());
                }
            }

            std::vector<RE::BGSHeadPart*> pending = state.headParts;
            for (std::size_t cursor = 0; cursor < pending.size(); ++cursor)
            {
                auto* headPart = pending[cursor];
                if (!headPart)
                {
                    REX::WARN("[NPCSnapshot] detached render-source head-part graph rejected: null node at traversal index={}", cursor);
                    return std::nullopt;
                }
                if (std::ranges::find_if(state.headPartGraph, [headPart](const HeadPartGraphNodeState& a_node)
                                         { return a_node.headPart == headPart; }) != state.headPartGraph.end())
                {
                    continue;
                }
                if (state.headPartGraph.size() >= kMaxReachableHeadParts)
                {
                    REX::WARN("[NPCSnapshot] detached render-source head-part graph rejected: reachable-node limit={} exceeded", kMaxReachableHeadParts);
                    return std::nullopt;
                }

                const auto formID = headPart->GetFormID();
                const auto type = static_cast<std::uint32_t>(headPart->type.get());
                if (formID == 0 || RE::TESForm::LookupByID<RE::BGSHeadPart>(formID) != headPart ||
                    type > static_cast<std::uint32_t>(RE::BGSHeadPart::HeadPartType::kCreatureWings))
                {
                    REX::WARN("[NPCSnapshot] detached render-source head-part graph rejected: node={} formID=0x{:08X} type={} registered={}",
                              static_cast<const void*>(headPart), formID, type,
                              formID != 0 && RE::TESForm::LookupByID<RE::BGSHeadPart>(formID) == headPart);
                    return std::nullopt;
                }

                const auto count = static_cast<std::size_t>(headPart->extraParts.size());
                const auto capacity = static_cast<std::size_t>(headPart->extraParts.capacity());
                auto* storage = headPart->extraParts.data();
                if (count > capacity || count > kMaxExtraPartsPerHeadPart || capacity > kMaxExtraPartCapacity || (count != 0 && !storage))
                {
                    REX::WARN("[NPCSnapshot] detached render-source extra-part array rejected: headPart=0x{:08X} count={} capacity={} storage={}",
                              formID, count, capacity, static_cast<const void*>(storage));
                    return std::nullopt;
                }

                HeadPartGraphNodeState node{
                    .headPart = headPart,
                    .formID = formID,
                    .type = type,
                    .extraPartStorage = storage,
                    .extraPartCapacity = capacity,
                };
                if (count != 0)
                {
                    node.extraParts.assign(headPart->extraParts.begin(), headPart->extraParts.end());
                }
                pending.insert(pending.end(), node.extraParts.begin(), node.extraParts.end());
                if (pending.size() > kMaxReachableHeadParts + kMaxExtraPartsPerHeadPart)
                {
                    REX::WARN("[NPCSnapshot] detached render-source head-part graph rejected: traversal edge limit exceeded");
                    return std::nullopt;
                }
                state.headPartGraph.push_back(std::move(node));
            }
            return state;
        }
        catch (const std::exception& error)
        {
            REX::ERROR("[NPCSnapshot] detached render-source structure capture threw: {}", error.what());
        }
        catch (...)
        {
            REX::ERROR("[NPCSnapshot] detached render-source structure capture threw an unknown exception");
        }
        return std::nullopt;
    }

    bool MatchesRenderSourceStructure(RE::TESNPC* a_source, const RenderSourceStructureState& a_expected) noexcept
    {
        if (!a_source || a_expected.source != a_source)
        {
            return false;
        }
        const auto actual = CaptureRenderSourceStructure(a_source);
        return actual && *actual == a_expected;
    }

    bool HasIndependentVisualStorage(
        const VisualStorageState& a_source,
        const VisualStorageState& a_donor)
    {
        return IndependentStorage(a_source.headPartCount, a_source.headPartStorage, a_donor.headPartStorage) &&
               IndependentStorage(a_source.morphRegionCount, a_source.morphRegionStorage, a_donor.morphRegionStorage) &&
               IndependentStorage(a_source.boneValueCount, a_source.boneValueStorage, a_donor.boneValueStorage) &&
               IndependentStorage(a_source.boneGroupCount, a_source.boneGroupStorage, a_donor.boneGroupStorage) &&
               IndependentStorage(a_source.tintCount, a_source.tintStorage, a_donor.tintStorage) &&
               IndependentStorage(a_source.shapeBlendCount, a_source.shapeBlendStorage, a_donor.shapeBlendStorage);
    }

    bool SameExactVisualValues(RE::TESNPC* a_left, RE::TESNPC* a_right)
    {
        if (!a_left || !a_right ||
            a_left->morphWeight.thin != a_right->morphWeight.thin ||
            a_left->morphWeight.muscular != a_right->morphWeight.muscular ||
            a_left->morphWeight.fat != a_right->morphWeight.fat ||
            a_left->skinToneIndex != a_right->skinToneIndex ||
            a_left->pronoun.underlying() != a_right->pronoun.underlying() ||
            std::string_view{ SafeText(a_left->teeth.c_str()) } != std::string_view{ SafeText(a_right->teeth.c_str()) } ||
            std::string_view{ SafeText(a_left->jewelryColor.c_str()) } != std::string_view{ SafeText(a_right->jewelryColor.c_str()) } ||
            std::string_view{ SafeText(a_left->eyeColor.c_str()) } != std::string_view{ SafeText(a_right->eyeColor.c_str()) } ||
            std::string_view{ SafeText(a_left->hairColor.c_str()) } != std::string_view{ SafeText(a_right->hairColor.c_str()) } ||
            std::string_view{ SafeText(a_left->facialColor.c_str()) } != std::string_view{ SafeText(a_right->facialColor.c_str()) } ||
            std::string_view{ SafeText(a_left->eyebrowColor.c_str()) } != std::string_view{ SafeText(a_right->eyebrowColor.c_str()) }) {
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

        if ((a_left->bodyMorphValues == nullptr) != (a_right->bodyMorphValues == nullptr)) {
            return false;
        }
        if (a_left->bodyMorphValues) {
            if (a_left->bodyMorphValues->size() != a_right->bodyMorphValues->size()) {
                return false;
            }
            for (std::uint32_t i = 0; i < a_left->bodyMorphValues->size(); ++i) {
                if ((*a_left->bodyMorphValues)[i] != (*a_right->bodyMorphValues)[i]) {
                    return false;
                }
            }
        }

        if ((a_left->facialBoneValues == nullptr) != (a_right->facialBoneValues == nullptr)) {
            return false;
        }
        if (a_left->facialBoneValues) {
            if (a_left->facialBoneValues->size() != a_right->facialBoneValues->size()) {
                return false;
            }
            for (const auto& entry : *a_left->facialBoneValues) {
                const auto other = a_right->facialBoneValues->find(entry.key);
                if (other == a_right->facialBoneValues->end() || other->value != entry.value) {
                    return false;
                }
            }
        }

        if ((a_left->facialBoneGroupValues == nullptr) != (a_right->facialBoneGroupValues == nullptr)) {
            return false;
        }
        if (a_left->facialBoneGroupValues) {
            if (a_left->facialBoneGroupValues->size() != a_right->facialBoneGroupValues->size()) {
                return false;
            }
            for (const auto& outer : *a_left->facialBoneGroupValues) {
                const auto otherOuter = a_right->facialBoneGroupValues->find(outer.key);
                if (otherOuter == a_right->facialBoneGroupValues->end() ||
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
                    const bool matched = std::ranges::any_of(
                        *otherOuter->value,
                        [&](const auto& a_other) {
                            return std::string_view{ SafeText(a_other.key.c_str()) } == std::string_view{ SafeText(inner.key.c_str()) } && a_other.value == inner.value;
                        });
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
                a_right->tintAVMData,
                [&](const RE::AVMData& a_entry) {
                    return std::string_view{ SafeText(a_entry.category.c_str()) } == std::string_view{ SafeText(avm.category.c_str()) };
                });
            if (other == a_right->tintAVMData.end() || other->type != avm.type ||
                std::string_view{ SafeText(other->unk10.name.c_str()) } != std::string_view{ SafeText(avm.unk10.name.c_str()) } ||
                std::string_view{ SafeText(other->unk10.texturePath.c_str()) } != std::string_view{ SafeText(avm.unk10.texturePath.c_str()) } ||
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
                const bool matched = std::ranges::any_of(
                    *a_right->shapeBlendData,
                    [&](const auto& a_other) {
                        return std::string_view{ SafeText(a_other.key.c_str()) } == std::string_view{ SafeText(entry.key.c_str()) } && a_other.value == entry.value;
                    });
                if (!matched) {
                    return false;
                }
            }
        }
        return true;
    }

}
