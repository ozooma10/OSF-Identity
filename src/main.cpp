#include "NpcAppearance/Runtime.h"
#include "pch.h"

#include <format>
#include <sstream>
#include <string>
#include <vector>

// C ABI diagnostic bridge: lets the OSF RE sandbox CommandFile channel drive
// `npcapp` inside this DLL — its own runtime/probe state, not the RE spike's.
// The caller supplies the full command line ("npcapp <sub> ..."); output lines
// are pushed through a_emit. Exceptions never cross the ABI.
extern "C" __declspec(dllexport) void OSFIdentity_RunDiagnosticCommand(
    const char* a_line,
    void (*a_emit)(void* a_context, const char* a_text),
    void* a_context) noexcept
{
    if (!a_line || !a_emit) {
        return;
    }
    try {
        std::vector<std::string> args;
        std::istringstream stream{ std::string{ a_line } };
        std::string token;
        while (stream >> token) {
            args.push_back(std::move(token));
        }
        if (args.empty()) {
            args.emplace_back("npcapp");
        }
        const NpcAppearance::LineSink sink =
            [a_emit, a_context](const std::string& a_text) {
                a_emit(a_context, a_text.c_str());
            };
        NpcAppearance::RunCommand(sink, args);
    } catch (const std::exception& e) {
        try {
            a_emit(a_context,
                   std::format("error: OSF Identity command threw '{}'", e.what()).c_str());
        } catch (...) {
        }
    } catch (...) {
        try {
            a_emit(a_context, "error: OSF Identity command threw");
        } catch (...) {
        }
    }
}

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
