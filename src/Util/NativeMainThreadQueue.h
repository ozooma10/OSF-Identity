#pragma once

namespace Util::NativeMainThreadQueue
{
    enum class PostResult
    {
        kQueued,        // engine took task; runs inside next drain (or onDrop fires)
        kRanInline,     // caller is the drain thread; ran before returning
        kUnavailable,   // queue disabled/singleton missing; refused, caller must retry.
    };

    struct QueueState
    {
        std::uintptr_t singleton{ 0 };
        std::uint32_t currentThreadID{ 0 };
        std::uint32_t drainOwnerThreadID{ 0 };
        bool queueEnabled{ false };
        bool insideDrain{ false };
    };

    QueueState SnapshotState();

    bool IsAvailable();

    PostResult Post(
        std::function<void()> a_task,
        std::string_view a_label,
        std::function<void()> a_onDrop = {});

    const char* ToString(PostResult a_result);
}
