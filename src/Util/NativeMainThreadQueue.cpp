#include "Util/NativeMainThreadQueue.h"

#include "pch.h"

#include "Util/StarfieldRuntime.h"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <string>

#ifdef ERROR
#    undef ERROR
#endif

namespace Util::NativeMainThreadQueue
{
    namespace
    {
        // Runtime-proven by TaskQueueProbe on 1.16.242; current .244 RVAs are
        // supplied by the Address Library. Keep the gate/owner diagnostics
        // local until the .244 runtime proof below passes.
        constexpr REL::ID kEnableByteID{ 810305 };
        constexpr REL::ID kDrainLockID{ 923104 };

        std::atomic<std::uint64_t> g_posted{ 0 };
        std::atomic<std::uint64_t> g_executed{ 0 };
        std::atomic<std::uint64_t> g_rejected{ 0 };
        std::atomic<bool>          g_firstProofLogged{ false };

        [[nodiscard]] std::uint8_t ReadEnableByte() noexcept
        {
            const auto address = REL::Relocation<std::uintptr_t>{ kEnableByteID }.address();
            if (!Util::IsReadableRange(address, sizeof(std::uint8_t))) {
                return 0;
            }
            std::uint8_t value = 0;
            std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
            return value;
        }

        [[nodiscard]] std::uint32_t ReadDrainOwnerThreadID() noexcept
        {
            const auto address = REL::Relocation<std::uintptr_t>{ kDrainLockID }.address();
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
        result.posted = g_posted.load(std::memory_order_relaxed);
        result.executed = g_executed.load(std::memory_order_relaxed);
        result.rejected = g_rejected.load(std::memory_order_relaxed);
        return result;
    }

    PostResult Post(std::function<void()> a_task, const std::string_view a_label)
    {
        if (!a_task) {
            g_rejected.fetch_add(1, std::memory_order_relaxed);
            return PostResult::kEmptyTask;
        }

        const auto before = GetDiagnostics();
        if (!before.queueEnabled) {
            g_rejected.fetch_add(1, std::memory_order_relaxed);
            return PostResult::kQueueDisabled;
        }
        if (before.singleton == 0) {
            g_rejected.fetch_add(1, std::memory_order_relaxed);
            return PostResult::kSingletonUnavailable;
        }
        if (before.insideDrain) {
            // AddTask executes inline from the drain owner. Reject that path so
            // every accepted post retains the same verified dispatch contract.
            g_rejected.fetch_add(1, std::memory_order_relaxed);
            return PostResult::kAlreadyInsideDrain;
        }

        auto* queue = reinterpret_cast<RE::BSService::TaskQueue*>(before.singleton);
        const std::string label{ a_label };
        g_posted.fetch_add(1, std::memory_order_relaxed);
        queue->AddTask([
            task = std::move(a_task),
            label
        ]() mutable {
            const auto during = GetDiagnostics();
            if (!during.insideDrain) {
                g_rejected.fetch_add(1, std::memory_order_relaxed);
                REX::CRITICAL(
                    "[NativeMainThreadQueue] DROP '{}' tid={} drainOwnerTid={} enabled={} singleton=0x{:X}; payload not run",
                    label, during.currentThreadID, during.drainOwnerThreadID,
                    during.queueEnabled, during.singleton);
                return;
            }

            g_executed.fetch_add(1, std::memory_order_relaxed);
            if (!g_firstProofLogged.exchange(true, std::memory_order_acq_rel)) {
                REX::INFO(
                    "[NativeMainThreadQueue] 1.16.244 RUNTIME PROOF PASS label='{}' tid={} drainOwnerTid={} insideDrain=true queueEnabled=true singleton=0x{:X}",
                    label, during.currentThreadID, during.drainOwnerThreadID,
                    during.singleton);
            }

            try {
                task();
            } catch (const std::exception& e) {
                REX::ERROR("[NativeMainThreadQueue] '{}' threw '{}'; payload stopped", label, e.what());
            } catch (...) {
                REX::ERROR("[NativeMainThreadQueue] '{}' threw an unknown exception; payload stopped", label);
            }
        });
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
        case PostResult::kEmptyTask:
            return "empty-task";
        default:
            return "unknown";
        }
    }
}
