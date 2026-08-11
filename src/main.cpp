#include "Config/PackScanner.h"
#include "Runtime/OverlayRuntime.h"
#include "Events/ReferenceSet3dEvent.h"

namespace
{
    void OnDataLoaded() noexcept
    {
        Config::ScanPacks();
        auto isArmed = Runtime::GetOverlayRuntime().IsArmed();
        REX::INFO("[PackScanner] startup scan complete; overlay runtime armed={}", isArmed);

        if(isArmed) {
            if(auto set3dSource = RE::RuntimeComponentDBFactory::ReferenceSet3d::GetEventSource()) {
                set3dSource->RegisterSink(Events::ReferenceSet3dEventHandler::GetSingleton());
            }
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
