#pragma once

#include <optional>

namespace Runtime
{
    enum class NPCDonorPurpose
    {
        kRestore,
        kPreset
    };

    class TemporaryNPCDonor
    {
    public:
        TemporaryNPCDonor(const TemporaryNPCDonor&) = delete;
        TemporaryNPCDonor& operator=(const TemporaryNPCDonor&) = delete;

        TemporaryNPCDonor(TemporaryNPCDonor&& a_other);
        TemporaryNPCDonor& operator=(TemporaryNPCDonor&&) = delete;
        ~TemporaryNPCDonor();

        static std::optional<TemporaryNPCDonor> Create(NPCDonorPurpose a_purpose);

        RE::TESNPC* Get() const { return m_donor; }

        bool ReleaseAndVerify();

    private:
        TemporaryNPCDonor(RE::TESNPC* a_donor, RE::TESFormID a_formID, NPCDonorPurpose a_purpose);

        RE::TESNPC* m_donor{ nullptr };
        RE::TESFormID m_formID{ 0 };
        NPCDonorPurpose m_purpose{ NPCDonorPurpose::kPreset };
    };
}
