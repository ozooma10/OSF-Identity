#pragma once

#include "NPCSnapshot.h"

#include <memory>
#include <optional>

namespace Runtime
{
    class NPCRestorePoint
    {
    public:
        NPCRestorePoint(const NPCRestorePoint&) = delete;
        NPCRestorePoint& operator=(const NPCRestorePoint&) = delete;

        NPCRestorePoint(NPCRestorePoint&&) noexcept;
        NPCRestorePoint& operator=(NPCRestorePoint&&) = delete;
        ~NPCRestorePoint();

        static std::optional<NPCRestorePoint> Capture(RE::TESNPC* a_target);

        bool RestoreExact(RE::TESNPC* a_target) const;

        bool ReleaseAndVerify();

        RE::TESNPC* Donor() const noexcept;

    private:
        struct Impl;

        explicit NPCRestorePoint(std::unique_ptr<Impl> a_impl) noexcept;

        std::unique_ptr<Impl> m_impl;
    };
}
