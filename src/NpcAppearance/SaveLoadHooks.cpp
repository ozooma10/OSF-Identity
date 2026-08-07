#include "NpcAppearance/SaveLoadHooks.h"

#define OSF_SAVE_LOAD_HOOK_HOST 1
#include "API/OSFSaveLoadHookAPI.h"

#include "pch.h"

#include "Util/StarfieldRuntime.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <intrin.h>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace NpcAppearance::SaveLoadHooks
{
    namespace
    {
        constexpr REL::ID kID_SaveGame{ 98376 };
        constexpr REL::ID kID_LoadGame{ 98380 };
        constexpr std::uint32_t kIdentityListenerID =
            OSF_SAVE_LOAD_HOOK_FOURCC('O', 'S', 'F', 'I');
        constexpr std::size_t kMaxListeners = 32;
        constexpr std::size_t kMaxListenerName = 96;

        // Full prologue byte gates, proven identical on Starfield 1.16.242 and
        // 1.16.244. Both arrays end on an instruction boundary.
        constexpr std::array<std::uint8_t, 32> kSaveGameGate{
            0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48,
            0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
        };
        constexpr std::array<std::uint8_t, 31> kLoadGameGate{
            0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x44, 0x88, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x48,
            0x89, 0x48, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
        };

        // Minimal instruction-boundary-safe steal lengths: SaveGame's first
        // instruction is 5 bytes; LoadGame's first two instructions are 7.
        constexpr std::array<std::uint8_t, 5> kSaveGameEntry{
            0x4C, 0x89, 0x4C, 0x24, 0x20
        };
        constexpr std::array<std::uint8_t, 7> kLoadGameEntry{
            0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20
        };

        using SaveGameFn = std::uint64_t (*)(void*, void*, void*, const char*);
        using LoadGameFn = std::uint64_t (*)(void*, char*, std::uint8_t, std::uint8_t);

        SaveGameFn g_saveGateway = nullptr;
        LoadGameFn g_loadGateway = nullptr;
        Callbacks  g_callbacks{};

        std::atomic<bool>          g_installAttempted{ false };
        std::atomic<bool>          g_installed{ false };
        std::atomic<std::uint64_t> g_saveEntries{ 0 };
        std::atomic<std::uint64_t> g_saveReturns{ 0 };
        std::atomic<std::uint64_t> g_loadEntries{ 0 };
        std::atomic<std::uint64_t> g_loadReturns{ 0 };
        std::atomic<std::uint64_t> g_directSaveSequence{ 0 };
        std::atomic<std::uint64_t> g_directLoadSequence{ 0 };
        std::atomic<const OSFSaveLoadHookAPI*> g_activeProvider{ nullptr };
        std::atomic<bool> g_saveVetoSupported{ false };
        char g_providerName[kMaxListenerName]{ "<none>" };

        struct CapturedName
        {
            char value[260]{};
        };

        struct ListenerRecord
        {
            std::uint32_t listenerID{ 0 };
            void* context{ nullptr };
            std::uint8_t (OSF_SAVE_LOAD_HOOK_CALL *onEvent)(
                void*, const OSFSaveLoadHookEventV1*){ nullptr };
            char name[kMaxListenerName]{};
        };

        [[nodiscard]] CapturedName CopyName(const char* a_source) noexcept
        {
            CapturedName captured{};
            if (!a_source) {
                return captured;
            }

            __try {
                std::size_t i = 0;
                for (; i < (sizeof(captured.value) - 1) && a_source[i] != '\0'; ++i) {
                    captured.value[i] = a_source[i];
                }
                captured.value[i] = '\0';
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                captured.value[0] = '\0';
            }
            return captured;
        }

        void CopyText(char* a_destination, const std::size_t a_capacity, const char* a_source) noexcept
        {
            if (!a_destination || a_capacity == 0) {
                return;
            }
            a_destination[0] = '\0';
            if (!a_source) {
                return;
            }

            __try {
                std::size_t i = 0;
                for (; i < (a_capacity - 1) && a_source[i] != '\0'; ++i) {
                    a_destination[i] = a_source[i];
                }
                a_destination[i] = '\0';
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                a_destination[0] = '\0';
            }
        }

        template <class F>
        void SwallowAtThunkBoundary(const char* a_label, F&& a_function) noexcept
        {
            try {
                std::forward<F>(a_function)();
            } catch (const std::exception& e) {
                try {
                    REX::CRITICAL("[SaveLoadHooks] {} threw '{}'; swallowed at thunk boundary", a_label, e.what());
                } catch (...) {
                }
            } catch (...) {
                try {
                    REX::CRITICAL("[SaveLoadHooks] {} threw; swallowed at thunk boundary", a_label);
                } catch (...) {
                }
            }
        }

        [[nodiscard]] bool EntryTargets(
            const std::uintptr_t a_entry,
            const std::uintptr_t a_thunk) noexcept
        {
            if (!Util::IsReadableRange(a_entry, 5)) {
                return false;
            }
            const auto* code = reinterpret_cast<const std::uint8_t*>(a_entry);
            if (code[0] != 0xE9) {
                return false;
            }

            std::int32_t displacement = 0;
            std::memcpy(&displacement, code + 1, sizeof(displacement));
            const auto branch = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(a_entry + 5) + displacement);
            if (branch == a_thunk) {
                return true;
            }
            if (!Util::IsReadableRange(branch, sizeof(REL::ASM::JMP14))) {
                return false;
            }
            const auto* island = reinterpret_cast<const std::uint8_t*>(branch);
            if (island[0] != 0xFF || island[1] != 0x25 ||
                island[2] != 0 || island[3] != 0 ||
                island[4] != 0 || island[5] != 0) {
                return false;
            }
            std::uintptr_t destination = 0;
            std::memcpy(&destination, island + 6, sizeof(destination));
            return destination == a_thunk;
        }

        class LocalBroker
        {
        public:
            static LocalBroker& GetSingleton() noexcept
            {
                static LocalBroker singleton;
                return singleton;
            }

            void SetReady(
                const std::uint32_t a_flags,
                const std::uintptr_t a_saveEntry,
                const std::uintptr_t a_saveThunk,
                const std::uintptr_t a_loadEntry,
                const std::uintptr_t a_loadThunk) noexcept
            {
                _saveEntry = a_saveEntry;
                _saveThunk = a_saveThunk;
                _loadEntry = a_loadEntry;
                _loadThunk = a_loadThunk;
                _flags.store(a_flags, std::memory_order_release);
                _ready.store(
                    (a_flags & OSF_SAVE_LOAD_HOOK_STATUS_REQUIRED) ==
                        OSF_SAVE_LOAD_HOOK_STATUS_REQUIRED,
                    std::memory_order_release);
            }

            [[nodiscard]] bool IsReady() const noexcept
            {
                return _ready.load(std::memory_order_acquire) && OwnsHooks();
            }

            [[nodiscard]] bool Register(const OSFSaveLoadHookListenerV2* a_listener)
            {
                if (!IsReady() || !a_listener ||
                    a_listener->size < sizeof(OSFSaveLoadHookListenerV2) ||
                    a_listener->listenerID == 0 || !a_listener->OnEvent) {
                    return false;
                }

                const std::scoped_lock lock{ _mutex };
                for (std::size_t i = 0; i < _listenerCount; ++i) {
                    const auto& existing = _listeners[i];
                    if (existing.listenerID == a_listener->listenerID) {
                        return existing.context == a_listener->context &&
                            existing.onEvent == a_listener->OnEvent;
                    }
                }
                if (_listenerCount >= _listeners.size()) {
                    REX::CRITICAL("[SaveLoadHooks] listener capacity exhausted ({})", _listeners.size());
                    return false;
                }

                auto& record = _listeners[_listenerCount++];
                record.listenerID = a_listener->listenerID;
                record.context = a_listener->context;
                record.onEvent = a_listener->OnEvent;
                CopyText(record.name, sizeof(record.name), a_listener->listenerName);
                REX::INFO("[SaveLoadHooks] broker listener registered id=0x{:08X} name='{}'",
                          record.listenerID,
                          record.name[0] != '\0' ? record.name : "<unnamed>");
                return true;
            }

            [[nodiscard]] bool Unregister(
                const std::uint32_t a_listenerID,
                void* a_context)
            {
                const std::scoped_lock lock{ _mutex };
                for (std::size_t i = 0; i < _listenerCount; ++i) {
                    if (_listeners[i].listenerID == a_listenerID &&
                        _listeners[i].context == a_context) {
                        for (std::size_t j = i + 1; j < _listenerCount; ++j) {
                            _listeners[j - 1] = _listeners[j];
                        }
                        _listeners[--_listenerCount] = {};
                        return true;
                    }
                }
                return false;
            }

            [[nodiscard]] bool Dispatch(const OSFSaveLoadHookEventV1& a_event) noexcept
            {
                std::array<ListenerRecord, kMaxListeners> listeners{};
                std::size_t count = 0;
                try {
                    const std::scoped_lock lock{ _mutex };
                    count = _listenerCount;
                    std::copy_n(_listeners.begin(), count, listeners.begin());
                } catch (...) {
                    try {
                        REX::CRITICAL("[SaveLoadHooks] broker snapshot failed; event phase={} sequence={} dropped",
                                      a_event.phase, a_event.sequence);
                    } catch (...) {
                    }
                    return false;
                }

                const bool saveEntry =
                    a_event.phase == OSF_SAVE_LOAD_HOOK_PHASE_SAVE_ENTRY;
                bool allowSave = !saveEntry || count != 0;
                for (std::size_t i = 0; i < count; ++i) {
                    const auto listener = listeners[i];
                    std::uint8_t listenerResult = 0;
                    SwallowAtThunkBoundary("broker listener", [&] {
                        listenerResult = listener.onEvent(listener.context, &a_event);
                    });
                    if (saveEntry && listenerResult == 0) {
                        allowSave = false;
                    }
                }
                return allowSave;
            }

            [[nodiscard]] std::uint32_t Flags() const noexcept
            {
                auto flags = _flags.load(std::memory_order_acquire);
                if (!OwnsHooks()) {
                    flags &= ~(OSF_SAVE_LOAD_HOOK_STATUS_SAVE_HOOK |
                               OSF_SAVE_LOAD_HOOK_STATUS_LOAD_HOOK |
                               OSF_SAVE_LOAD_HOOK_STATUS_SAVE_VETO);
                }
                return flags;
            }

        private:
            [[nodiscard]] bool OwnsHooks() const noexcept
            {
                return _saveEntry != 0 && _saveThunk != 0 &&
                    _loadEntry != 0 && _loadThunk != 0 &&
                    EntryTargets(_saveEntry, _saveThunk) &&
                    EntryTargets(_loadEntry, _loadThunk);
            }

            std::mutex _mutex;
            std::array<ListenerRecord, kMaxListeners> _listeners{};
            std::size_t _listenerCount{ 0 };
            std::atomic<bool> _ready{ false };
            std::atomic<std::uint32_t> _flags{ 0 };
            std::uintptr_t _saveEntry{ 0 };
            std::uintptr_t _saveThunk{ 0 };
            std::uintptr_t _loadEntry{ 0 };
            std::uintptr_t _loadThunk{ 0 };
        };

        std::uint8_t OSF_SAVE_LOAD_HOOK_CALL APIIsReady(const OSFSaveLoadHookAPI* a_api)
        {
            return a_api && a_api->context &&
                static_cast<LocalBroker*>(a_api->context)->IsReady();
        }

        std::uint8_t OSF_SAVE_LOAD_HOOK_CALL APIRegister(
            const OSFSaveLoadHookAPI* a_api,
            const OSFSaveLoadHookListenerV2* a_listener)
        {
            return a_api && a_api->context &&
                static_cast<LocalBroker*>(a_api->context)->Register(a_listener);
        }

        std::uint8_t OSF_SAVE_LOAD_HOOK_CALL APIUnregister(
            const OSFSaveLoadHookAPI* a_api,
            const std::uint32_t a_listenerID,
            void* a_context)
        {
            return a_api && a_api->context &&
                static_cast<LocalBroker*>(a_api->context)->Unregister(
                    a_listenerID, a_context);
        }

        std::uint8_t OSF_SAVE_LOAD_HOOK_CALL APIGetStatus(
            const OSFSaveLoadHookAPI* a_api,
            OSFSaveLoadHookStatusV1* a_status)
        {
            if (!a_api || !a_api->context || !a_status ||
                a_status->size < sizeof(OSFSaveLoadHookStatusV1)) {
                return 0;
            }
            a_status->flags = static_cast<LocalBroker*>(a_api->context)->Flags();
            a_status->providerName = "OSF Identity";
            return 1;
        }

        std::uint32_t OSF_SAVE_LOAD_HOOK_CALL APIVersion(const OSFSaveLoadHookAPI*)
        {
            return OSF_SAVE_LOAD_HOOK_API_VERSION;
        }

        [[nodiscard]] const OSFSaveLoadHookAPI& LocalAPITable()
        {
            static const OSFSaveLoadHookAPI api{
                sizeof(OSFSaveLoadHookAPI),
                &LocalBroker::GetSingleton(),
                &APIIsReady,
                &APIRegister,
                &APIUnregister,
                &APIGetStatus,
                &APIVersion,
            };
            return api;
        }

        struct ProviderInfo
        {
            const OSFSaveLoadHookAPI* api{ nullptr };
            OSFSaveLoadHookStatusV1 status{};
            std::string name;
        };

        [[nodiscard]] bool CompatibleAPI(const OSFSaveLoadHookAPI* a_api)
        {
            return a_api && a_api->size >= sizeof(OSFSaveLoadHookAPI) &&
                a_api->IsReady && a_api->RegisterListener &&
                a_api->UnregisterListener && a_api->GetStatus &&
                a_api->GetInterfaceVersion && a_api->IsReady(a_api) &&
                OSF_SAVE_LOAD_HOOK_API_MAJOR(a_api->GetInterfaceVersion(a_api)) ==
                    OSF_SAVE_LOAD_HOOK_API_MAJOR(OSF_SAVE_LOAD_HOOK_API_VERSION);
        }

        [[nodiscard]] std::optional<ProviderInfo> FindExternalProvider()
        {
            HMODULE selfModule = nullptr;
            (void)::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&g_activeProvider),
                &selfModule);

            const HANDLE snapshot = ::CreateToolhelp32Snapshot(
                TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                ::GetCurrentProcessId());
            if (snapshot == INVALID_HANDLE_VALUE) {
                return std::nullopt;
            }

            MODULEENTRY32W module{};
            module.dwSize = sizeof(module);
            bool haveModule = ::Module32FirstW(snapshot, &module) != FALSE;
            while (haveModule) {
                if (module.hModule != selfModule) {
                    const auto request = reinterpret_cast<OSF_RequestSaveLoadHookAPI_t>(
                        ::GetProcAddress(module.hModule, "OSF_RequestSaveLoadHookAPI"));
                    if (request) {
                        const OSFSaveLoadHookAPI* api = nullptr;
                        try {
                            api = request(OSF_SAVE_LOAD_HOOK_API_VERSION);
                        } catch (...) {
                            api = nullptr;
                        }
                        if (CompatibleAPI(api)) {
                            OSFSaveLoadHookStatusV1 status{
                                .size = sizeof(OSFSaveLoadHookStatusV1),
                            };
                            if (api->GetStatus(api, &status) &&
                                (status.flags & OSF_SAVE_LOAD_HOOK_STATUS_REQUIRED) ==
                                    OSF_SAVE_LOAD_HOOK_STATUS_REQUIRED) {
                                ProviderInfo result{
                                    .api = api,
                                    .status = status,
                                    .name = status.providerName ? status.providerName : "<unnamed>",
                                };
                                ::CloseHandle(snapshot);
                                return result;
                            }
                        }
                    }
                }
                haveModule = ::Module32NextW(snapshot, &module) != FALSE;
            }
            ::CloseHandle(snapshot);
            return std::nullopt;
        }

        void InvokeCallback(const std::function<void()>& a_callback, const char* a_label) noexcept
        {
            if (!a_callback) {
                return;
            }
            SwallowAtThunkBoundary(a_label, [&a_callback] {
                a_callback();
            });
        }

        [[nodiscard]] bool InvokeSaveEntryCallback() noexcept
        {
            if (!g_callbacks.onSaveGameEntry) {
                return true;
            }
            try {
                return g_callbacks.onSaveGameEntry();
            } catch (const std::exception& e) {
                try {
                    REX::CRITICAL(
                        "[SaveLoadHooks] SAVE entry callback threw '{}'; save vetoed at thunk boundary",
                        e.what());
                } catch (...) {
                }
            } catch (...) {
                try {
                    REX::CRITICAL(
                        "[SaveLoadHooks] SAVE entry callback threw; save vetoed at thunk boundary");
                } catch (...) {
                }
            }
            return false;
        }

        std::uint8_t OSF_SAVE_LOAD_HOOK_CALL OnBrokerEvent(
            void*,
            const OSFSaveLoadHookEventV1* a_event) noexcept
        {
            if (!a_event || a_event->size < sizeof(OSFSaveLoadHookEventV1)) {
                return 0;
            }
            const char* name = a_event->name && a_event->name[0] != '\0' ?
                a_event->name : "<none>";
            switch (a_event->phase) {
            case OSF_SAVE_LOAD_HOOK_PHASE_SAVE_ENTRY: {
                const auto entries = g_saveEntries.fetch_add(1, std::memory_order_relaxed) + 1;
                SwallowAtThunkBoundary("SAVE entry log", [&] {
                    REX::INFO(
                        "[SaveLoadHooks] SAVE entry sequence={} tid={} name='{}' provider='{}' entries={}",
                        a_event->sequence, ::GetCurrentThreadId(), name, g_providerName, entries);
                });
                return InvokeSaveEntryCallback() ? 1 : 0;
            }
            case OSF_SAVE_LOAD_HOOK_PHASE_SAVE_RETURN: {
                InvokeCallback(g_callbacks.onSaveGameReturn, "SAVE return callback");
                const auto returns = g_saveReturns.fetch_add(1, std::memory_order_relaxed) + 1;
                SwallowAtThunkBoundary("SAVE return log", [&] {
                    REX::INFO(
                        "[SaveLoadHooks] SAVE return sequence={} tid={} name='{}' provider='{}' resultValid={} vetoed={} rc={} entries={} returns={}",
                        a_event->sequence, ::GetCurrentThreadId(), name, g_providerName,
                        (a_event->flags & OSF_SAVE_LOAD_HOOK_EVENT_RESULT_VALID) != 0,
                        (a_event->flags & OSF_SAVE_LOAD_HOOK_EVENT_SAVE_VETOED) != 0,
                        a_event->result,
                        g_saveEntries.load(std::memory_order_relaxed), returns);
                });
                break;
            }
            case OSF_SAVE_LOAD_HOOK_PHASE_LOAD_ENTRY: {
                const auto entries = g_loadEntries.fetch_add(1, std::memory_order_relaxed) + 1;
                SwallowAtThunkBoundary("LOAD entry log", [&] {
                    REX::INFO(
                        "[SaveLoadHooks] LOAD entry sequence={} tid={} name='{}' provider='{}' entries={}",
                        a_event->sequence, ::GetCurrentThreadId(), name, g_providerName, entries);
                });
                break;
            }
            case OSF_SAVE_LOAD_HOOK_PHASE_LOAD_RETURN: {
                InvokeCallback(g_callbacks.onLoadGameReturn, "LOAD return callback");
                const auto returns = g_loadReturns.fetch_add(1, std::memory_order_relaxed) + 1;
                SwallowAtThunkBoundary("LOAD return log", [&] {
                    REX::INFO(
                        "[SaveLoadHooks] LOAD return sequence={} tid={} name='{}' provider='{}' resultValid={} rc={} entries={} returns={}",
                        a_event->sequence, ::GetCurrentThreadId(), name, g_providerName,
                        (a_event->flags & OSF_SAVE_LOAD_HOOK_EVENT_RESULT_VALID) != 0,
                        a_event->result,
                        g_loadEntries.load(std::memory_order_relaxed), returns);
                });
                break;
            }
            default:
                break;
            }
            return 1;
        }

        [[nodiscard]] OSFSaveLoadHookListenerV2 IdentityListener() noexcept
        {
            return {
                .size = sizeof(OSFSaveLoadHookListenerV2),
                .listenerID = kIdentityListenerID,
                .listenerName = "OSF Identity",
                .context = nullptr,
                .OnEvent = &OnBrokerEvent,
            };
        }

        std::uint64_t SaveGameThunk(
            void* a_game,
            void* a_context,
            void* a_writer,
            const char* a_name) noexcept
        {
            const auto sequence =
                g_directSaveSequence.fetch_add(1, std::memory_order_relaxed) + 1;
            const auto name = CopyName(a_name);
            OSFSaveLoadHookEventV1 event{
                .size = sizeof(OSFSaveLoadHookEventV1),
                .phase = OSF_SAVE_LOAD_HOOK_PHASE_SAVE_ENTRY,
                .sequence = sequence,
                .name = name.value,
            };
            // Every listener must positively allow serialization. An empty or
            // failed dispatch therefore vetoes the engine gateway.
            const bool proceed = LocalBroker::GetSingleton().Dispatch(event);

            std::uint64_t result = 0;
            if (proceed) {
                result = g_saveGateway(a_game, a_context, a_writer, a_name);
            } else {
                SwallowAtThunkBoundary("SAVE veto log", [&] {
                    REX::CRITICAL(
                        "[SaveLoadHooks] SAVE sequence={} name='{}' vetoed before the engine gateway",
                        sequence,
                        name.value[0] != '\0' ? name.value : "<none>");
                });
            }

            event.phase = OSF_SAVE_LOAD_HOOK_PHASE_SAVE_RETURN;
            event.flags = OSF_SAVE_LOAD_HOOK_EVENT_RESULT_VALID |
                (proceed ? 0u : OSF_SAVE_LOAD_HOOK_EVENT_SAVE_VETOED);
            event.result = result;
            (void)LocalBroker::GetSingleton().Dispatch(event);
            return result;
        }

        std::uint64_t LoadGameThunk(
            void* a_game,
            char* a_reader,
            const std::uint8_t a_flag1,
            const std::uint8_t a_flag2) noexcept
        {
            const auto sequence =
                g_directLoadSequence.fetch_add(1, std::memory_order_relaxed) + 1;
            const auto name = CopyName(a_reader);
            OSFSaveLoadHookEventV1 event{
                .size = sizeof(OSFSaveLoadHookEventV1),
                .phase = OSF_SAVE_LOAD_HOOK_PHASE_LOAD_ENTRY,
                .sequence = sequence,
                .name = name.value,
            };
            (void)LocalBroker::GetSingleton().Dispatch(event);

            const auto result = g_loadGateway(a_game, a_reader, a_flag1, a_flag2);

            event.phase = OSF_SAVE_LOAD_HOOK_PHASE_LOAD_RETURN;
            event.flags = OSF_SAVE_LOAD_HOOK_EVENT_RESULT_VALID;
            event.result = result;
            (void)LocalBroker::GetSingleton().Dispatch(event);
            return result;
        }

        [[nodiscard]] bool RegisterWithProvider(
            const OSFSaveLoadHookAPI* a_provider,
            const OSFSaveLoadHookStatusV1& a_status,
            const char* a_providerName)
        {
            const auto listener = IdentityListener();
            if (!a_provider->RegisterListener(a_provider, &listener)) {
                REX::CRITICAL("[SaveLoadHooks] provider '{}' rejected OSF Identity listener",
                              a_providerName);
                return false;
            }
            CopyText(g_providerName, sizeof(g_providerName), a_providerName);
            g_activeProvider.store(a_provider, std::memory_order_release);
            g_saveVetoSupported.store(
                (a_status.flags & OSF_SAVE_LOAD_HOOK_STATUS_SAVE_VETO) != 0,
                std::memory_order_release);
            const auto flags = a_status.flags;
            REX::INFO(
                "[SaveLoadHooks] installed: provider='{}' mode=broker | SaveGame(98376) gate={} hook={} veto={} | LoadGame(98380) gate={} hook={} operational=true",
                g_providerName,
                (flags & OSF_SAVE_LOAD_HOOK_STATUS_SAVE_GATE) != 0,
                (flags & OSF_SAVE_LOAD_HOOK_STATUS_SAVE_HOOK) != 0,
                (flags & OSF_SAVE_LOAD_HOOK_STATUS_SAVE_VETO) != 0,
                (flags & OSF_SAVE_LOAD_HOOK_STATUS_LOAD_GATE) != 0,
                (flags & OSF_SAVE_LOAD_HOOK_STATUS_LOAD_HOOK) != 0);
            return true;
        }

        [[nodiscard]] bool InstallDirectProvider()
        {
            const bool saveGate = Util::VerifyExpectedBytes(
                "[SaveLoadHooks] SaveGame 98376 full gate",
                kID_SaveGame.address(),
                kSaveGameGate);
            const bool loadGate = Util::VerifyExpectedBytes(
                "[SaveLoadHooks] LoadGame 98380 full gate",
                kID_LoadGame.address(),
                kLoadGameGate);
            if (!saveGate || !loadGate) {
                REX::INFO(
                    "[SaveLoadHooks] installed: provider='OSF Identity' mode=direct | SaveGame(98376) gate={} hook=false | LoadGame(98380) gate={} hook=false operational=false",
                    saveGate, loadGate);
                return false;
            }

            const auto saveGateway = Util::InstallEntryHookWithGateway<5>(
                REL::Offset(kID_SaveGame.offset()),
                "[SaveLoadHooks] SaveGame 98376 entry",
                kSaveGameEntry,
                &SaveGameThunk);
            g_saveGateway = reinterpret_cast<SaveGameFn>(saveGateway);
            if (g_saveGateway) {
                const auto loadGateway = Util::InstallEntryHookWithGateway<7>(
                    REL::Offset(kID_LoadGame.offset()),
                    "[SaveLoadHooks] LoadGame 98380 entry",
                    kLoadGameEntry,
                    &LoadGameThunk);
                g_loadGateway = reinterpret_cast<LoadGameFn>(loadGateway);
            }

            const std::uint32_t flags =
                OSF_SAVE_LOAD_HOOK_STATUS_SAVE_GATE |
                OSF_SAVE_LOAD_HOOK_STATUS_LOAD_GATE |
                (g_saveGateway ? OSF_SAVE_LOAD_HOOK_STATUS_SAVE_HOOK : 0u) |
                (g_saveGateway ? OSF_SAVE_LOAD_HOOK_STATUS_SAVE_VETO : 0u) |
                (g_loadGateway ? OSF_SAVE_LOAD_HOOK_STATUS_LOAD_HOOK : 0u);
            auto& broker = LocalBroker::GetSingleton();
            broker.SetReady(
                flags,
                kID_SaveGame.address(),
                reinterpret_cast<std::uintptr_t>(&SaveGameThunk),
                kID_LoadGame.address(),
                reinterpret_cast<std::uintptr_t>(&LoadGameThunk));
            if (!broker.IsReady()) {
                REX::CRITICAL("[SaveLoadHooks] direct hook pair did not install completely");
                return false;
            }

            const auto& api = LocalAPITable();
            OSFSaveLoadHookStatusV1 status{
                .size = sizeof(OSFSaveLoadHookStatusV1),
                .flags = flags,
                .providerName = "OSF Identity",
            };
            return RegisterWithProvider(&api, status, status.providerName);
        }
    }

    bool Install(const Callbacks& a_callbacks)
    {
        if (g_installAttempted.exchange(true, std::memory_order_acq_rel)) {
            return g_installed.load(std::memory_order_acquire);
        }

        bool installed = false;
        try {
            g_callbacks = a_callbacks;
            if (const auto provider = FindExternalProvider()) {
                installed = RegisterWithProvider(
                    provider->api, provider->status, provider->name.c_str());
            } else {
                installed = InstallDirectProvider();
            }
        } catch (const std::exception& e) {
            REX::CRITICAL("[SaveLoadHooks] installation threw '{}'; hooks remain fail closed", e.what());
        } catch (...) {
            REX::CRITICAL("[SaveLoadHooks] installation threw; hooks remain fail closed");
        }

        g_installed.store(installed, std::memory_order_release);
        return installed;
    }

    bool Operational() noexcept
    {
        if (!g_installed.load(std::memory_order_acquire)) {
            return false;
        }
        const auto* provider = g_activeProvider.load(std::memory_order_acquire);
        try {
            if (!CompatibleAPI(provider)) {
                return false;
            }
            OSFSaveLoadHookStatusV1 status{
                .size = sizeof(OSFSaveLoadHookStatusV1),
            };
            return provider->GetStatus(provider, &status) &&
                (status.flags & OSF_SAVE_LOAD_HOOK_STATUS_REQUIRED) ==
                    OSF_SAVE_LOAD_HOOK_STATUS_REQUIRED;
        } catch (...) {
            return false;
        }
    }

    bool SupportsSaveVeto() noexcept
    {
        return g_saveVetoSupported.load(std::memory_order_acquire) &&
            Operational();
    }

    [[nodiscard]] const OSFSaveLoadHookAPI* RequestAPI(const std::uint32_t a_requestedVersion)
    {
        if (OSF_SAVE_LOAD_HOOK_API_MAJOR(a_requestedVersion) !=
                OSF_SAVE_LOAD_HOOK_API_MAJOR(OSF_SAVE_LOAD_HOOK_API_VERSION) ||
            OSF_SAVE_LOAD_HOOK_API_MINOR(a_requestedVersion) >
                OSF_SAVE_LOAD_HOOK_API_MINOR(OSF_SAVE_LOAD_HOOK_API_VERSION)) {
            return nullptr;
        }
        const auto* provider = g_activeProvider.load(std::memory_order_acquire);
        return CompatibleAPI(provider) ? provider : nullptr;
    }
}

OSF_SAVE_LOAD_HOOK_EXPORT const OSFSaveLoadHookAPI* OSF_SAVE_LOAD_HOOK_CALL
    OSF_RequestSaveLoadHookAPI(const std::uint32_t a_requestedVersion)
{
    return NpcAppearance::SaveLoadHooks::RequestAPI(a_requestedVersion);
}
