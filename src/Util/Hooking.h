#pragma once

#include "Util/StarfieldRuntime.h"

#include "REL/ASM.h"

#include <array>
#include <cstring>
#include <format>
#include <string>

namespace Util::Hooking
{
    template <std::size_t N>
    [[nodiscard]] inline bool VerifyExpectedBytes(
        const char* a_label,
        const std::uintptr_t a_address,
        const std::array<std::uint8_t, N>& a_expected)
    {
        const auto* actual = reinterpret_cast<const std::uint8_t*>(a_address);
        if (std::equal(a_expected.begin(), a_expected.end(), actual)) {
            return true;
        }

        std::string actualBytes;
        for (std::size_t i = 0; i < N; ++i) {
            if (i != 0) {
                actualBytes += ' ';
            }
            actualBytes += std::format("{:02X}", actual[i]);
        }

        REX::WARN("{} bytes drifted at 0x{:X}: {}", a_label, a_address, actualBytes);
        return false;
    }

    template <std::size_t N, auto SiteDelta = 0, class T>
    [[nodiscard]] inline std::uintptr_t InstallCallHook(
        const REL::Offset a_offset,
        const char* a_label,
        const std::array<std::uint8_t, N>& a_expectedBytes,
        T a_thunk)
    {
        REL::Relocation<std::uintptr_t> relocation{ a_offset };
        const auto siteAddress = relocation.address() + SiteDelta;
        if (!VerifyExpectedBytes(a_label, siteAddress, a_expectedBytes)) {
            return 0;
        }

        if constexpr (SiteDelta == 0) {
            return relocation.write_call<N>(a_thunk);
        } else {
            return relocation.write_call<N, SiteDelta>(a_thunk);
        }
    }

    template <std::size_t N, class T>
    [[nodiscard]] inline std::uintptr_t InstallEntryHookWithGateway(
        const REL::Offset a_offset,
        const char* a_label,
        const std::array<std::uint8_t, N>& a_expectedBytes,
        T a_thunk)
    {
        REL::Relocation<std::uintptr_t> relocation{ a_offset };
        const auto entryAddress = relocation.address();
        if (!VerifyExpectedBytes(a_label, entryAddress, a_expectedBytes)) {
            return 0;
        }

        auto& trampoline = REL::GetTrampoline();
        auto* gateway = static_cast<std::byte*>(trampoline.allocate(N + sizeof(REL::ASM::JMP14)));
        std::memcpy(gateway, reinterpret_cast<const void*>(entryAddress), N);

        const REL::ASM::JMP14 jumpBack{ entryAddress + N };
        std::memcpy(gateway + N, &jumpBack, sizeof(jumpBack));

        relocation.write_jmp<5>(a_thunk);
        return reinterpret_cast<std::uintptr_t>(gateway);
    }

    template <class T>
    [[nodiscard]] inline std::uintptr_t InstallVTableHook(
        const std::uintptr_t a_vtable,
        const std::size_t a_index,
        T a_thunk)
    {
        REL::Relocation<std::uintptr_t> relocation{ a_vtable };
        return relocation.write_vfunc(a_index, a_thunk);
    }
}
