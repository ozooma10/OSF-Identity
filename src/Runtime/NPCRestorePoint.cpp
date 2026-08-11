#include "NPCRestorePoint.h"

#include <atomic>
#include <memory>
#include <string_view>

namespace Runtime
{
    constexpr std::size_t kBodyMorphRegionCount = 5;
    namespace
    {
        std::atomic<bool> g_restoreDonorOperational{ true };

        void KillRestoreDonorMutation(const std::string_view a_reason)
        {
            const bool wasOperational =
                g_restoreDonorOperational.exchange(false, std::memory_order_acq_rel);
            if (wasOperational) {
                REX::CRITICAL(
                    "[NPCRestorePoint] mutation disabled for the process: {}",
                    a_reason);
            }
        }

        [[nodiscard]] bool IsRegisteredEmptyDonor(
            RE::TESNPC* a_donor,
            const RE::TESFormID a_formID)
        {
            if (!a_donor ||
                a_formID == 0 ||
                RE::TESForm::LookupByID<RE::TESNPC>(a_formID) != a_donor ||
                a_donor->QRefCount() != 0 || a_donor->GetRace() != nullptr ||
                a_donor->faceNPC != nullptr || a_donor->unk3D8 != nullptr ||
                a_donor->unk3E0 != nullptr || a_donor->unk3E8 != nullptr ||
                !a_donor->tintAVMData.empty() ||
                a_donor->shapeBlendData != nullptr ||
                a_donor->pronoun.underlying() != 0) {
                return false;
            }

            auto headParts = a_donor->headParts.Lock();
            return (*headParts).empty();
        }
    }

    struct NPCRestorePoint::Impl
    {
        OriginalNPCState original;
        RE::TESNPC* donor{ nullptr };
        RE::TESFormID donorFormID{ 0 };
        bool released{ false };

        ~Impl()
        {
            Destroy();
        }

        void Destroy() noexcept
        {
            if (donor) {
                delete donor;
                donor = nullptr;
            }
        }
    };

    NPCRestorePoint::NPCRestorePoint(std::unique_ptr<Impl> a_impl) noexcept :
        m_impl(std::move(a_impl))
    {}

    NPCRestorePoint::NPCRestorePoint(NPCRestorePoint&&) noexcept = default;

    NPCRestorePoint::~NPCRestorePoint()
    {
        if (!m_impl || m_impl->released) {
            return;
        }

        const auto donorFormID = m_impl->donorFormID;
        m_impl->Destroy();
        if (donorFormID != 0 &&
            RE::TESForm::LookupByID<RE::TESNPC>(donorFormID) != nullptr) {
            KillRestoreDonorMutation("restore donor destructor did not unregister its form");
        }
    }

    std::optional<NPCRestorePoint> NPCRestorePoint::Capture(RE::TESNPC* a_target)
    {
        if (!a_target ||
            !g_restoreDonorOperational.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        auto impl = std::make_unique<Impl>();
        impl->original = CaptureOriginalNPCState(a_target);
        const auto sourceStorage = CaptureVisualStorageState(a_target);

        impl->donor = RE::TESNPC::Create(false);
        if (!impl->donor) {
            REX::WARN("[NPCRestorePoint] Create(false) returned null");
            return std::nullopt;
        }

        impl->donorFormID = impl->donor->GetFormID();
        const auto discardDonor = [&]() {
            const auto donorFormID = impl->donorFormID;
            impl->Destroy();
            if (donorFormID != 0 && RE::TESForm::LookupByID<RE::TESNPC>(donorFormID) != nullptr) {
                KillRestoreDonorMutation("failed capture donor did not unregister");
            }
        };
        if (!IsRegisteredEmptyDonor(impl->donor, impl->donorFormID)) {
            REX::WARN("[NPCRestorePoint] donor failed registered-empty invariants");
            discardDonor();
            return std::nullopt;
        }

        impl->donor->CopyAppearance(a_target, false);
        const bool exact = SameExactVisualValues(impl->donor, a_target);
        const bool independent = HasIndependentVisualStorage(sourceStorage, CaptureVisualStorageState(impl->donor));
        const bool sourceMetadataPreserved = a_target->faceNPC == impl->original.faceNPC && a_target->actorData.actorBaseFlags.underlying() == impl->original.actorFlags && CaptureNonVisualState(a_target) == impl->original.nonVisual;
        if (!exact || !independent || !sourceMetadataPreserved) {
            REX::WARN(
                "[NPCRestorePoint] capture validation failed exact={} independent={} sourceMetadataPreserved={}",
                exact, independent, sourceMetadataPreserved);
            discardDonor();
            return std::nullopt;
        }

        return NPCRestorePoint{ std::move(impl) };
    }

    bool NPCRestorePoint::RestoreExact(RE::TESNPC* a_target) const
    {
        if (!a_target || !m_impl || !m_impl->donor || m_impl->released || !g_restoreDonorOperational.load(std::memory_order_acquire)) {
            return false;
        }

        const auto* const originalBodyMorphs = m_impl->donor->unk3D8;
        const bool originalBodyMorphsCanonical = !originalBodyMorphs || originalBodyMorphs->size() == kBodyMorphRegionCount;
        const bool targetBodyMorphsCanonical = !a_target->unk3D8 || a_target->unk3D8->size() == kBodyMorphRegionCount;
        if(!originalBodyMorphsCanonical || !targetBodyMorphsCanonical) {
            KillRestoreDonorMutation("body-morph storage is not canonical");
            return false;
        }

        a_target->morphWeight = m_impl->donor->morphWeight;

        for(auto i = 0; i < kBodyMorphRegionCount; i++) {
            a_target->SetBodyMorph(i, originalBodyMorphs ? (*originalBodyMorphs)[i] : 0.0f);
        }

        a_target->skinToneIndex = m_impl->donor->skinToneIndex;
        a_target->CopyOwnedAppearance(m_impl->donor, false);
        a_target->faceNPC = m_impl->original.faceNPC;
        a_target->actorData.actorBaseFlags = static_cast<RE::ACTOR_BASE_DATA::Flag>(m_impl->original.actorFlags);

        const bool restored = SameExactOriginalState(a_target, m_impl->donor, m_impl->original);
        if (!restored) {
            KillRestoreDonorMutation("restore did not reproduce the original state");
        }
        return restored;
    }

    bool NPCRestorePoint::ReleaseAndVerify()
    {
        if (!m_impl || m_impl->released) {
            return false;
        }

        const auto donorFormID = m_impl->donorFormID;
        m_impl->Destroy();
        m_impl->released = true;
        const bool unregistered = donorFormID != 0 && RE::TESForm::LookupByID<RE::TESNPC>(donorFormID) == nullptr;
        if (!unregistered) {
            KillRestoreDonorMutation("restore donor teardown failed");
        }
        return unregistered;
    }

    RE::TESNPC* NPCRestorePoint::Donor() const noexcept
    {
        return m_impl ? m_impl->donor : nullptr;
    }
}
