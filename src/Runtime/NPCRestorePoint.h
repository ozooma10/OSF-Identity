#pragma once

#include "NPCSnapshot.h"
#include "TemporaryNPCDonor.h"

#include <optional>

namespace Runtime
{
    class NPCRestorePoint
    {
    public:
        NPCRestorePoint(const NPCRestorePoint&) = delete;
        NPCRestorePoint& operator=(const NPCRestorePoint&) = delete;

        NPCRestorePoint(NPCRestorePoint&&);
        NPCRestorePoint& operator=(NPCRestorePoint&&) = delete;
        ~NPCRestorePoint();

        static std::optional<NPCRestorePoint> Capture(RE::TESNPC* a_target);

        bool RestoreExact(RE::TESNPC* a_target) const;

        bool ReleaseAndVerify();

        const OriginalNPCState& OriginalState() const
        {
            return m_original;
        }

    private:
        NPCRestorePoint(TemporaryNPCDonor a_donor, OriginalNPCState a_original);

        TemporaryNPCDonor m_donor;
        OriginalNPCState m_original;
    };
}
