#include "MutationSafety.h"

#include <atomic>

namespace Runtime
{
    namespace
    {
        std::atomic<bool> g_mutationOperational{ true };
    }

    bool IsMutationOperational()
    {
        return g_mutationOperational.load(std::memory_order_acquire);
    }

    void KillMutation(const std::string_view a_reason)
    {
        if (g_mutationOperational.exchange(false, std::memory_order_acq_rel)) {
            REX::CRITICAL("[MutationSafety] mutation disabled for the process: {}", a_reason);
        }
    }
}
