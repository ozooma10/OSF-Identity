#include "RuntimeSafety.h"

#include <atomic>

namespace Runtime
{
    namespace
    {
        std::atomic<bool> g_runtimeOperational{ true };
    }

    bool IsRuntimeOperational()
    {
        return g_runtimeOperational.load(std::memory_order_acquire);
    }

    void KillRuntime(const std::string_view a_reason)
    {
        if (g_runtimeOperational.exchange(false, std::memory_order_acq_rel)) {
            REX::CRITICAL("[RuntimeSafety] appearance injection disabled for the process: {}", a_reason);
        }
    }
}
