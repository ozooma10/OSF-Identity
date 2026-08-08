#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace Util::NativeMainThreadQueue
{
    // Point-in-time snapshot of the native queue's state, used to gate dispatch.
    struct State
    {
        bool           queueEnabled{ false };
        std::uintptr_t singleton{ 0 };
        std::uint32_t  currentThreadID{ 0 };
        std::uint32_t  drainOwnerThreadID{ 0 };
        bool           insideDrain{ false };
    };

    enum class PostResult
    {
        kQueued,
        kQueueDisabled,
        kSingletonUnavailable,
        kAlreadyInsideDrain,
        kDroppedInline,
        kEmptyTask
    };

    // Posts through Starfield's BSService command stream. Payloads are guarded again at execution and are dropped unless the callback runs while the current thread owns the native queue's drain lock.
    [[nodiscard]] PostResult Post(
        std::function<void()> a_task,
        std::string_view a_label,
        std::function<void()> a_onDrop = {});

    [[nodiscard]] State SnapshotState() noexcept;
    [[nodiscard]] const char* ToString(PostResult a_result) noexcept;
}
