#include "RuntimeSafety.h"

#include <atomic>
#include <cstdint>

namespace Runtime
{
    namespace
    {
        std::atomic<std::uint8_t> g_runtimeOperational{ 1 };

        static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
        static_assert(sizeof(g_runtimeOperational) == sizeof(std::uint8_t));
    }

    bool IsRuntimeOperational()
    {
        return g_runtimeOperational.load(std::memory_order_acquire) != 0;
    }

    void KillRuntime(const std::string_view a_reason)
    {
        if (g_runtimeOperational.exchange(0, std::memory_order_acq_rel) != 0) {
            REX::CRITICAL("[RuntimeSafety] appearance injection disabled for the process: {}", a_reason);
        }
    }

    const void* RuntimeOperationalFlagAddress() noexcept
    {
        return &g_runtimeOperational;
    }
}
