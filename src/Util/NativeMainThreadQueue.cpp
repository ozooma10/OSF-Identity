#include "Util/NativeMainThreadQueue.h"

#include "pch.h"

#include "Util/StarfieldRuntime.h"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#ifdef ERROR
#    undef ERROR
#endif

namespace Util::NativeMainThreadQueue
{
    namespace
    {
        std::atomic<bool> g_firstProofLogged{ false };

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
    }

    Diagnostics GetDiagnostics() noexcept
    {
        Diagnostics result;
        result.queueEnabled = ReadEnableByte() == 1;
        result.singleton = reinterpret_cast<std::uintptr_t>(ReadSingleton());
        result.currentThreadID = ::GetCurrentThreadId();
        result.drainOwnerThreadID = ReadDrainOwnerThreadID();
        result.insideDrain = result.drainOwnerThreadID != 0 &&
            result.currentThreadID == result.drainOwnerThreadID;
        return result;
    }

    PostResult Post(
        std::function<void()> a_task,
        const std::string_view a_label,
        std::function<void()> a_onDrop)
    {
        if (!a_task) {
            return PostResult::kEmptyTask;
        }

        const auto before = GetDiagnostics();
        if (!before.queueEnabled) {
            return PostResult::kQueueDisabled;
        }
        if (before.singleton == 0) {
            return PostResult::kSingletonUnavailable;
        }
        if (before.insideDrain) {
            // AddTask executes inline from the drain owner. Reject that path so
            // every accepted post retains the same verified dispatch contract.
            return PostResult::kAlreadyInsideDrain;
        }

        auto* queue = reinterpret_cast<RE::BSService::TaskQueue*>(before.singleton);
        const std::string label{ a_label };
        const auto returned = std::make_shared<std::atomic<bool>>(false);
        const auto droppedInline = std::make_shared<std::atomic<bool>>(false);
        queue->AddTask([
            task = std::move(a_task),
            label,
            onDrop = std::move(a_onDrop),
            returned,
            droppedInline
        ]() mutable noexcept {
            bool taskStarted = false;
            bool dropSignaled = false;
            const auto signalDrop = [&]() noexcept {
                if (dropSignaled) {
                    return;
                }
                dropSignaled = true;
                if (!returned->load(std::memory_order_acquire)) {
                    droppedInline->store(true, std::memory_order_release);
                }
                if (onDrop) {
                    try {
                        onDrop();
                    } catch (...) {
                    }
                }
            };
            try {
                const auto during = GetDiagnostics();
                if (!during.insideDrain) {
                    signalDrop();
                    try {
                        REX::CRITICAL(
                            "[NativeMainThreadQueue] DROP '{}' tid={} drainOwnerTid={} enabled={} singleton=0x{:X}; payload not run",
                            label, during.currentThreadID, during.drainOwnerThreadID,
                            during.queueEnabled, during.singleton);
                    } catch (...) {
                    }
                    return;
                }

                if (!g_firstProofLogged.exchange(true, std::memory_order_acq_rel)) {
                    REX::INFO(
                        "[NativeMainThreadQueue] 1.16.244 RUNTIME PROOF PASS label='{}' tid={} drainOwnerTid={} insideDrain=true queueEnabled=true singleton=0x{:X}",
                        label, during.currentThreadID, during.drainOwnerThreadID,
                        during.singleton);
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
        returned->store(true, std::memory_order_release);
        if (droppedInline->load(std::memory_order_acquire)) {
            return PostResult::kDroppedInline;
        }
        return PostResult::kQueued;
    }

    const char* ToString(const PostResult a_result) noexcept
    {
        switch (a_result) {
        case PostResult::kQueued:
            return "queued";
        case PostResult::kQueueDisabled:
            return "queue-disabled";
        case PostResult::kSingletonUnavailable:
            return "singleton-unavailable";
        case PostResult::kAlreadyInsideDrain:
            return "already-inside-drain";
        case PostResult::kDroppedInline:
            return "dropped-inline";
        case PostResult::kEmptyTask:
            return "empty-task";
        default:
            return "unknown";
        }
    }
}
