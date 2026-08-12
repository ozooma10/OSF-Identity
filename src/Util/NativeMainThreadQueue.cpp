#include "Util/NativeMainThreadQueue.h"

#include "pch.h"

namespace Util::NativeMainThreadQueue
{
    namespace {
        // Runs payload only if executing thread owns drain lock; otherwise fires onDrop.
        // owned by the engine if QueueTask steals ref; owned by Post if refused.
        class GuardedTask final : public RE::BSService::QueuedDelegate
        {
            public:
                GuardedTask(std::function<void()> a_task, std::string_view a_label, std::function<void()> a_onDrop) :
                    _task(std::move(a_task)), _label(a_label), _onDrop(std::move(a_onDrop)) {}

                void Run() override
                {
                    const auto owningTid = RE::BSService::TaskQueue::GetDrainOwnerThreadID();
                    const auto currentTid = REX::W32::GetCurrentThreadId();
                    if(currentTid != owningTid) {
                        if(_onDrop) {
                            try {
                                _onDrop();
                            } catch (...) {
                                //Swallow throws
                            }
                        }
                        REX::CRITICAL(
                            "[NativeMainThreadQueue] DROP '{}' tid={} drainOwnerTid={}; payload not run",
                            _label, currentTid, owningTid);
                        return;
                    }

                    try {
                        _task();
                    } catch (const std::exception& e) {
                        REX::ERROR("[NativeMainThreadQueue] guarded task threw '{}'; payload stopped", e.what());
                    } catch (...) {
                        REX::ERROR("[NativeMainThreadQueue] guarded task threw an unknown exception; payload stopped");
                    }
                }

            private:
                std::function<void()> _task;
                std::string           _label;
                std::function<void()> _onDrop;
        };

        bool InsideDrain() noexcept
        {
            return SnapshotState().insideDrain;
        }
    }

    PostResult Post(std::function<void()> a_task, const std::string_view a_label, std::function<void()> a_onDrop)
    {

        if(InsideDrain()) {
            // Already on verified drain, so run task directly (should mirror engine behavior).
            a_task();
            return PostResult::kRanInline;            
        }

        auto* queue = RE::BSService::TaskQueue::GetSingleton();
        if(!queue) {
            return PostResult::kUnavailable;
        }

        // QueueTask nulls ref when it enqueues (steals ownership basically)
        // if ref survives, it means the gate refused, so we just abort and dont run (since we only want to run on main thread).
        RE::BSService::QueuedDelegate* ref = new GuardedTask{std::move(a_task), a_label, std::move(a_onDrop)};
        queue->QueueTask(ref);

        if(!ref) {
            return PostResult::kQueued;
        }
        delete ref;
        return PostResult::kUnavailable;
    }

    QueueState SnapshotState()
    {
        QueueState state{};

        state.currentThreadID = REX::W32::GetCurrentThreadId();
        state.drainOwnerThreadID = RE::BSService::TaskQueue::GetDrainOwnerThreadID();

        auto* queue = RE::BSService::TaskQueue::GetSingleton();
        state.singleton = reinterpret_cast<std::uintptr_t>(queue);
        state.queueEnabled = queue != nullptr && RE::BSService::TaskQueue::IsQueueEnabled();

        state.insideDrain = state.drainOwnerThreadID != 0 && state.currentThreadID == state.drainOwnerThreadID;

        return state;
    }

    bool IsAvailable()
    {
        const auto state = SnapshotState();
        return state.insideDrain || (state.singleton != 0 && state.queueEnabled);
    }

    const char* ToString(const PostResult a_result)
    {
        switch (a_result) {
        case PostResult::kQueued:
            return "queued";
        case PostResult::kRanInline:
            return "ran-inline";
        case PostResult::kUnavailable:
            return "unavailable";
        default:
            return "unknown";
        }
    }
}
