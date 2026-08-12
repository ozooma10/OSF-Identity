#include "NPCRestorePoint.h"

#include "MutationSafety.h"

#include <utility>

namespace Runtime
{
    namespace
    {
        constexpr std::size_t kBodyMorphRegionCount = 5;
    }

    NPCRestorePoint::NPCRestorePoint(TemporaryNPCDonor a_donor, OriginalNPCState a_original) 
        : m_donor(std::move(a_donor)), m_original(std::move(a_original)) {}

    NPCRestorePoint::NPCRestorePoint(NPCRestorePoint&&) = default;
    NPCRestorePoint::~NPCRestorePoint() = default;

    std::optional<NPCRestorePoint> NPCRestorePoint::Capture(RE::TESNPC* a_target)
    {
        if (!a_target) {
            return std::nullopt;
        }

        const auto original = CaptureOriginalNPCState(a_target);
        const auto sourceStorage = CaptureVisualStorageState(a_target);
        auto donor = TemporaryNPCDonor::Create(NPCDonorPurpose::kRestore);
        if (!donor) {
            return std::nullopt;
        }

        try {
            donor->Get()->CopyAppearance(a_target, false);
        } catch (const std::exception& error) {
            REX::ERROR("[NPCRestorePoint] capture copy threw: {}", error.what());
            return std::nullopt;
        } catch (...) {
            REX::ERROR("[NPCRestorePoint] capture copy threw an unknown exception");
            return std::nullopt;
        }

        const bool exact = SameExactVisualValues(donor->Get(), a_target);
        const bool independent = HasIndependentVisualStorage(sourceStorage, CaptureVisualStorageState(donor->Get()));
        const bool sourcePreserved = CaptureNonVisualState(a_target) == original.nonVisual && a_target->faceNPC == original.faceNPC && a_target->actorData.actorBaseFlags.underlying() == original.actorFlags;
        if (!sourcePreserved) {
            KillMutation("restore-point capture changed the source NPC");
            return std::nullopt;
        }
        if (!exact || !independent) {
            REX::WARN("[NPCRestorePoint] capture failed exact={} independent={}", exact, independent);
            return std::nullopt;
        }

        return NPCRestorePoint{ std::move(*donor), original };
    }

    bool NPCRestorePoint::RestoreExact(RE::TESNPC* a_target) const
    {
        auto* donor = m_donor.Get();
        if (!a_target || !donor) {
            return false;
        }

        const auto* originalBodyMorphs = donor->bodyMorphValues;
        const bool originalBodyMorphsCanonical = !originalBodyMorphs || originalBodyMorphs->size() == kBodyMorphRegionCount;
        const bool targetBodyMorphsCanonical = !a_target->bodyMorphValues || a_target->bodyMorphValues->size() == kBodyMorphRegionCount;
        if (!originalBodyMorphsCanonical || !targetBodyMorphsCanonical) {
            return false;
        }

        a_target->morphWeight = donor->morphWeight;
        for (std::uint32_t i = 0; i < kBodyMorphRegionCount; ++i) {
            a_target->SetBodyMorph(i, originalBodyMorphs ? (*originalBodyMorphs)[i] : 0.0F);
        }
        a_target->skinToneIndex = donor->skinToneIndex;
        a_target->CopyOwnedAppearance(donor, false);
        a_target->faceNPC = m_original.faceNPC;
        a_target->actorData.actorBaseFlags = static_cast<RE::ACTOR_BASE_DATA::Flag>(m_original.actorFlags);

        return SameExactOriginalState(a_target, donor, m_original);
    }

    bool NPCRestorePoint::ReleaseAndVerify()
    {
        return m_donor.ReleaseAndVerify();
    }
}
