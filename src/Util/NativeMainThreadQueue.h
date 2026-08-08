#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace Util::NativeMainThreadQueue
{
    struct Diagnostics
    {
        bool           queueEnabled{ false };
        std::uintptr_t singleton{ 0 };
        std::uint32_t  currentThreadID{ 0 };
        std::uint32_t  drainOwnerThreadID{ 0 };
        bool           insideDrain{ false };
        std::uint64_t  posted{ 0 };
        std::uint64_t  executed{ 0 };
        std::uint64_t  rejected{ 0 };
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

    // Posts through Starfield's BSService command stream. Payloads are guarded
    // again at execution and are dropped unless the callback runs while the
    // current thread owns the native queue's drain lock.
    [[nodiscard]] PostResult Post(
        std::function<void()> a_task,
        std::string_view a_label,
        std::function<void()> a_onDrop = {});

    [[nodiscard]] Diagnostics GetDiagnostics() noexcept;
    [[nodiscard]] const char* ToString(PostResult a_result) noexcept;
}
