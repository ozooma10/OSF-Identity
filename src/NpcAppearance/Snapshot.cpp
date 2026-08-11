#include "NpcAppearance/RuntimeDetail.h"

#include "pch.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace NpcAppearance::Detail
{
    NonVisualSnapshot Snapshot(RE::TESNPC* a_npc)
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

    VisualSeedSnapshot SnapshotVisualSeed(RE::TESNPC* a_npc)
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
        if (a_npc->bodyMorphValues) {
            snap.morphRegionCount = a_npc->bodyMorphValues->size();
            snap.morphRegionStorage = a_npc->bodyMorphValues;
        }
        if (a_npc->facialBoneValues) {
            snap.boneValueCount = a_npc->facialBoneValues->size();
            snap.boneValueStorage = a_npc->facialBoneValues;
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

    bool HasIndependentVisualStorage(const VisualSeedSnapshot& a_source,
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

    bool SameExactVisualValues(RE::TESNPC* a_left, RE::TESNPC* a_right)
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

    OwnedVisualSnapshot CaptureOwnedVisualSnapshot(RE::TESNPC* a_npc)
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

        snapshot.hasBodyMorphRegions = a_npc->bodyMorphValues != nullptr;
        if (a_npc->bodyMorphValues) {
            snapshot.bodyMorphRegions.assign(a_npc->bodyMorphValues->begin(), a_npc->bodyMorphValues->end());
        }

        snapshot.hasBoneValues = a_npc->facialBoneValues != nullptr;
        if (a_npc->facialBoneValues) {
            snapshot.boneValues.reserve(a_npc->facialBoneValues->size());
            for (const auto& entry : *a_npc->facialBoneValues) {
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

    bool SameExactVisualValues(
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

        if ((a_npc->bodyMorphValues != nullptr) != a_snapshot.hasBodyMorphRegions) {
            return false;
        }
        if (a_npc->bodyMorphValues) {
            if (a_npc->bodyMorphValues->size() != a_snapshot.bodyMorphRegions.size()) {
                return false;
            }
            for (std::uint32_t i = 0; i < a_npc->bodyMorphValues->size(); ++i) {
                if ((*a_npc->bodyMorphValues)[i] != a_snapshot.bodyMorphRegions[i]) {
                    return false;
                }
            }
        }

        if ((a_npc->facialBoneValues != nullptr) != a_snapshot.hasBoneValues) {
            return false;
        }
        if (a_npc->facialBoneValues) {
            if (a_npc->facialBoneValues->size() != a_snapshot.boneValues.size()) {
                return false;
            }
            for (const auto& [key, value] : a_snapshot.boneValues) {
                const auto other = a_npc->facialBoneValues->find(key);
                if (other == a_npc->facialBoneValues->end() || other->value != value) {
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
}
