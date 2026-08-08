#include "NpcAppearance/Runtime.h"
#include "pch.h"

namespace
{
    void OnDataLoaded() noexcept
    {
        try {
            REX::INFO("=== OSF Identity: data loaded ===");
            NpcAppearance::Initialize();

            REX::INFO("=== OSF Identity: ready ===");
        } catch (const std::exception& e) {
            NpcAppearance::FailClosed("data-loaded callback threw");
            try {
                REX::CRITICAL(
                    "OSF Identity data-loaded callback swallowed '{}'",
                    e.what());
            } catch (...) {
            }
        } catch (...) {
            NpcAppearance::FailClosed("data-loaded callback threw");
            try {
                REX::CRITICAL(
                    "OSF Identity data-loaded callback swallowed an unknown exception");
            } catch (...) {
            }
        }
    }

    void OnSFSEMessage(SFSE::MessagingInterface::Message* a_message) noexcept
    {
        try {
            if (a_message && a_message->type == SFSE::MessagingInterface::kPostPostDataLoad) {
                OnDataLoaded();
            }
        } catch (const std::exception& e) {
            NpcAppearance::FailClosed("SFSE message boundary threw");
            try {
                REX::CRITICAL(
                    "OSF Identity SFSE message boundary swallowed '{}'",
                    e.what());
            } catch (...) {
            }
        } catch (...) {
            NpcAppearance::FailClosed("SFSE message boundary threw");
            try {
                REX::CRITICAL(
                    "OSF Identity SFSE message boundary swallowed an unknown exception");
            } catch (...) {
            }
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
