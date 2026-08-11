#include "Config/PackScanner.h"
#include "Runtime/OverlayRuntime.h"
#include "Events/ReferenceSet3dEvent.h"
#include "Util/NativeMainThreadQueue.h"

namespace
{
    void RunStartupScan()
    {
        auto assignments = Config::RunScan(Config::DefaultPacksDirectory());
        if(assignments.empty()) {
            REX::WARN("[PackScanner] startup scan found no valid assignments");
            return;
        }

        auto& runtime = Runtime::GetOverlayRuntime();
        if(!runtime.Arm(std::move(assignments))) {
            REX::WARN("[PackScanner] failed to arm overlay runtime");
            return;
        }

        auto* set3dSource = RE::RuntimeComponentDBFactory::ReferenceSet3d::GetEventSource();
        if(!set3dSource) {
            REX::WARN("[PackScanner] failed to get ReferenceSet3d event source");
            return;
        }
        set3dSource->RegisterSink(Events::ReferenceSet3dEventHandler::GetSingleton());
        REX::INFO("[PackScanner] startup scan complete; overlay runtime armed");
    }

    void OnDataLoaded() noexcept
    {
        try {
            const auto result = Util::NativeMainThreadQueue::Post([] {
                RunStartupScan();
            },
            "PackScanner.StartupScan",
            [] {
                REX::CRITICAL("[PackScanner] failed to queue startup scan on main thread");
            });
            if(result == Util::NativeMainThreadQueue::PostResult::kUnavailable) {
                REX::CRITICAL("[PackScanner] failed to queue startup scan on main thread; queue unavailable");
            }
        } catch (const std::exception& e) {
            REX::ERROR("[PackScanner] exception during startup scan: {}", e.what());
        } catch (...) {
            REX::ERROR("[PackScanner] unknown exception during startup scan");
        }
    }

    void OnSFSEMessage(SFSE::MessagingInterface::Message* a_message) noexcept
    {
        if (a_message && a_message->type == SFSE::MessagingInterface::kPostPostDataLoad) {
            OnDataLoaded();
        }
    }
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    SFSE::GetMessagingInterface()->RegisterListener(OnSFSEMessage);
    return true;
}
