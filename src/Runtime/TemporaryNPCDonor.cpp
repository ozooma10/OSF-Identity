#include "TemporaryNPCDonor.h"

#include "MutationSafety.h"

#include <string_view>
#include <utility>

namespace Runtime
{
    namespace
    {
        std::string_view PurposeName(const NPCDonorPurpose a_purpose)
        {
            switch (a_purpose) {
            case NPCDonorPurpose::kRestore:
                return "restore";
            case NPCDonorPurpose::kPreset:
                return "preset";
            }
            return "unknown";
        }

        bool IsRegisteredEmptyDonor(RE::TESNPC* a_donor, const RE::TESFormID a_formID)
        {
            if (!a_donor || a_formID == 0 || RE::TESForm::LookupByID<RE::TESNPC>(a_formID) != a_donor || a_donor->QRefCount() != 0 || a_donor->GetRace() != nullptr ||
                a_donor->faceNPC != nullptr || a_donor->bodyMorphValues != nullptr || a_donor->facialBoneValues != nullptr || a_donor->unk3E8 != nullptr ||
                !a_donor->tintAVMData.empty() || a_donor->shapeBlendData != nullptr || a_donor->pronoun.underlying() != 0) {
                return false;
            }

            auto headParts = a_donor->headParts.Lock();
            return (*headParts).empty();
        }
    }

    TemporaryNPCDonor::TemporaryNPCDonor(RE::TESNPC* a_donor, const RE::TESFormID a_formID, const NPCDonorPurpose a_purpose) 
        : m_donor(a_donor), m_formID(a_formID), m_purpose(a_purpose) {}

    TemporaryNPCDonor::TemporaryNPCDonor(TemporaryNPCDonor&& a_other) 
        : m_donor(std::exchange(a_other.m_donor, nullptr)), m_formID(std::exchange(a_other.m_formID, 0)), m_purpose(a_other.m_purpose) {}

    TemporaryNPCDonor::~TemporaryNPCDonor()
    {
        if (m_donor) {
            static_cast<void>(ReleaseAndVerify());
        }
    }

    std::optional<TemporaryNPCDonor> TemporaryNPCDonor::Create(const NPCDonorPurpose a_purpose)
    {
        if (!IsMutationOperational()) {
            return std::nullopt;
        }

        auto* donor = RE::TESNPC::Create(false);
        if (!donor) {
            REX::WARN("[TemporaryNPCDonor] {} donor creation returned null", PurposeName(a_purpose));
            return std::nullopt;
        }

        TemporaryNPCDonor result{ donor, donor->GetFormID(), a_purpose };
        if (!IsRegisteredEmptyDonor(result.m_donor, result.m_formID)) {
            REX::WARN("[TemporaryNPCDonor] {} donor failed registered-empty invariants", PurposeName(a_purpose));
            static_cast<void>(result.ReleaseAndVerify());
            return std::nullopt;
        }

        return std::optional<TemporaryNPCDonor>{ std::move(result) };
    }

    bool TemporaryNPCDonor::ReleaseAndVerify() noexcept
    {
        if (!m_donor) {
            return false;
        }

        const auto formID = m_formID;
        const auto purpose = m_purpose;
        delete std::exchange(m_donor, nullptr);
        m_formID = 0;

        const bool unregistered = formID != 0 && RE::TESForm::LookupByID<RE::TESNPC>(formID) == nullptr;
        if (!unregistered) {
            KillMutation( purpose == NPCDonorPurpose::kRestore ? "restore donor did not unregister" : "preset donor did not unregister");
        }
        return unregistered;
    }
}
