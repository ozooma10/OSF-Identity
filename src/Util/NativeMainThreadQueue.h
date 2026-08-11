#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace Util::NativeMainThreadQueue
{
    enum class PostResult
    {
        kQueued,        // engine took task; runs inside next drain (or onDrop fires)
        kRanInline,     // caller is the drain thread; ran before returning
        kUnavailable,   // queue disabled/singleton missing; refused, caller must retry.
    };

    [[nodiscard]] bool IsAvailable() noexcept;

    [[nodiscard]] PostResult Post(
        std::function<void()> a_task,
        std::string_view a_label,
        std::function<void()> a_onDrop = {});

    [[nodiscard]] const char* ToString(PostResult a_result) noexcept;
}
