#include "NpcAppearance/Runtime.h"
#include "pch.h"

namespace
{
    void OnDataLoaded()
    {
        REX::INFO("=== OSF Identity: data loaded ===");
        NpcAppearance::Initialize();

        REX::INFO("=== OSF Identity: ready ===");
    }

    void OnSFSEMessage(SFSE::MessagingInterface::Message* a_message)
    {
        if (a_message && a_message->type == SFSE::MessagingInterface::kPostPostDataLoad) {
            OnDataLoaded();
        }
    }
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse, {
        .trampoline = true,
        .trampolineSize = 1 << 12,
    });

    const auto* messaging = SFSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnSFSEMessage)) {
        REX::CRITICAL("Could not register the SFSE message listener");
        return false;
    }

    REX::INFO("OSF Identity loaded");
    return true;
}
