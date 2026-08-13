#include "NPCSnapshot.h"

#include "Util/String.h"

#include <cstring>

namespace Runtime
{
    namespace
    {
        using Util::SafeText;

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
        if (a_npc->unk3E8) {
            state.boneGroupCount = a_npc->unk3E8->size();
            state.boneGroupStorage = a_npc->unk3E8;
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
