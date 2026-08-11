#include "Util/NativeMainThreadQueue.h"

#include "pch.h"

#include "Util/StarfieldRuntime.h"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

// <wingdi.h> defines ERROR as a bare macro, which would mangle REX::ERROR below.
#ifdef ERROR
#    undef ERROR
#endif

namespace Util::NativeMainThreadQueue
{
    namespace
    {
        [[nodiscard]] std::uint8_t ReadEnableByte() noexcept
        {
            const auto address = REL::Relocation<std::uintptr_t>{ RE::ID::BSService::TaskQueue::Enabled }.address();
            if (!Util::IsReadableRange(address, sizeof(std::uint8_t))) {
                return 0;
            }
            std::uint8_t value = 0;
            std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
            return value;
        }

        [[nodiscard]] std::uint32_t ReadDrainOwnerThreadID() noexcept
        {
            const auto address = REL::Relocation<std::uintptr_t>{ RE::ID::BSService::TaskQueue::DrainOwnerThreadID }.address();
            if (!Util::IsReadableRange(address, sizeof(std::uint32_t))) {
                return 0;
            }
            std::uint32_t value = 0;
            std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
            return value;
        }

        [[nodiscard]] RE::BSService::TaskQueue* ReadSingleton() noexcept
        {
            const auto slot = REL::Relocation<std::uintptr_t>{
                RE::ID::BSService::TaskQueue::Singleton
            }.address();
            std::uintptr_t value = 0;
            if (!Util::SafeReadQword(slot, value) ||
                !Util::IsReadableRange(value, sizeof(std::uintptr_t))) {
                return nullptr;
            }
            return reinterpret_cast<RE::BSService::TaskQueue*>(value);
        }

        // Both drop-signal flags share one lifetime; one control block.
        struct PostFlags
        {
            std::atomic<bool> returned{ false };
            std::atomic<bool> droppedInline{ false };
        };
    }

    State SnapshotState() noexcept
    {
        State result;
        result.queueEnabled = ReadEnableByte() == 1;
        result.singleton = reinterpret_cast<std::uintptr_t>(ReadSingleton());
        result.currentThreadID = ::GetCurrentThreadId();
        result.drainOwnerThreadID = ReadDrainOwnerThreadID();
        result.insideDrain = result.drainOwnerThreadID != 0 &&
            result.currentThreadID == result.drainOwnerThreadID;
        return result;
    }

    PostResult PostOrRunInline(std::function<void()> a_task, const std::string_view a_label, std::function<void()> a_onDrop)
    {
        if (!a_task) {
            return PostResult::kEmptyTask;
        }

        const auto before = SnapshotState();
        if (before.insideDrain) {
            // Already on the verified drain (AddTask would execute inline
            // anyway): run now, ahead of the availability checks — inline
            // dispatch needs neither the enable byte nor the singleton.
            try {
                a_task();
                return PostResult::kRanInline;
            } catch (const std::exception& e) {
                try {
                    REX::CRITICAL(
                        "[NativeMainThreadQueue] '{}' threw '{}' inside the verified drain",
                        a_label, e.what());
                } catch (...) {
                }
                return PostResult::kInlineThrew;
            } catch (...) {
                try {
                    REX::CRITICAL(
                        "[NativeMainThreadQueue] '{}' threw inside the verified drain",
                        a_label);
                } catch (...) {
                }
                return PostResult::kInlineThrew;
            }
        }
        if (!IsQueueUsable(before)) {
            return before.queueEnabled ? PostResult::kSingletonUnavailable
                                       : PostResult::kQueueDisabled;
        }

        auto* queue = reinterpret_cast<RE::BSService::TaskQueue*>(before.singleton);
        const auto flags = std::make_shared<PostFlags>();
        queue->AddTask([
            task = std::move(a_task),
            label = std::string{ a_label },
            onDrop = std::move(a_onDrop),
            flags
        ]() mutable noexcept {
            bool taskStarted = false;
            bool dropSignaled = false;
            const auto signalDrop = [&]() noexcept {
                if (dropSignaled) {
                    return;
                }
                dropSignaled = true;
                if (!flags->returned.load(std::memory_order_acquire)) {
                    flags->droppedInline.store(true, std::memory_order_release);
                }
                if (onDrop) {
                    try {
                        onDrop();
                    } catch (...) {
                    }
                }
            };
            try {
                const auto currentTid = ::GetCurrentThreadId();
                const auto drainOwnerTid = ReadDrainOwnerThreadID();
                if (drainOwnerTid == 0 || currentTid != drainOwnerTid) {
                    signalDrop();
                    // Full snapshot only on the drop path, where the extra
                    // fields are actually printed.
                    const auto during = SnapshotState();
                    try {
                        REX::CRITICAL(
                            "[NativeMainThreadQueue] DROP '{}' tid={} drainOwnerTid={} enabled={} singleton=0x{:X}; payload not run",
                            label, currentTid, drainOwnerTid,
                            during.queueEnabled, during.singleton);
                    } catch (...) {
                    }
                    return;
                }

                taskStarted = true;
                task();
            } catch (const std::exception& e) {
                if (!taskStarted) {
                    signalDrop();
                }
                try {
                    REX::ERROR("[NativeMainThreadQueue] '{}' threw '{}'; payload stopped", label, e.what());
                } catch (...) {
                }
            } catch (...) {
                if (!taskStarted) {
                    signalDrop();
                }
                try {
                    REX::ERROR("[NativeMainThreadQueue] '{}' threw an unknown exception; payload stopped", label);
                } catch (...) {
                }
            }
        });
        flags->returned.store(true, std::memory_order_release);
        if (flags->droppedInline.load(std::memory_order_acquire)) {
            return PostResult::kDroppedInline;
        }
        return PostResult::kQueued;
    }

    const char* ToString(const PostResult a_result) noexcept
    {
        switch (a_result) {
        case PostResult::kQueued:
            return "queued";
        case PostResult::kRanInline:
            return "ran-inline";
        case PostResult::kInlineThrew:
            return "inline-threw";
        case PostResult::kQueueDisabled:
            return "queue-disabled";
        case PostResult::kSingletonUnavailable:
            return "singleton-unavailable";
        case PostResult::kDroppedInline:
            return "dropped-inline";
        case PostResult::kEmptyTask:
            return "empty-task";
        default:
            return "unknown";
        }
    }
}
