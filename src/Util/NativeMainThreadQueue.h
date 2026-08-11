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
        kRanInline,
        kInlineThrew,
        kQueueDisabled,
        kSingletonUnavailable,
        kDroppedInline,
        kEmptyTask
    };

    // The single readiness definition shared by dispatch and retry polling.
    [[nodiscard]] constexpr bool IsQueueUsable(const State& a_state) noexcept
    {
        return a_state.queueEnabled && a_state.singleton != 0;
    }

    // Dispatches through Starfield's BSService command stream. A caller
    // already inside the verified drain runs the task immediately
    // (kRanInline / kInlineThrew; a_onDrop is not invoked — the caller's
    // failure path owns that cleanup). Everyone else posts: payloads are
    // guarded again at execution and are dropped unless the callback runs
    // while the current thread owns the native queue's drain lock, with
    // a_onDrop as the cancellation signal.
    [[nodiscard]] PostResult PostOrRunInline(
        std::function<void()> a_task,
        std::string_view a_label,
        std::function<void()> a_onDrop = {});

    [[nodiscard]] State SnapshotState() noexcept;
    [[nodiscard]] const char* ToString(PostResult a_result) noexcept;
}
