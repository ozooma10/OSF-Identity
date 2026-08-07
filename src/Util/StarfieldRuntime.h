#pragma once

#include <algorithm>
#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

#include "REL/ASM.h"
#include "REL/Offset.h"
#include "REL/Relocation.h"
#include "REL/Trampoline.h"
#include "REX/FModule.h"
#include "REX/LOG.h"

namespace Util
{
    // --- Memory probing -----------------------------------------------------

    [[nodiscard]] inline std::uintptr_t ToRva(const std::uintptr_t a_address) noexcept
    {
        const auto base = REX::FModule::GetExecutingModule().GetBaseAddress();
        return (base != 0 && a_address >= base) ? (a_address - base) : 0;
    }

    [[nodiscard]] inline bool IsReadableRange(const std::uintptr_t a_address, const std::size_t a_size)
    {
        if (a_address == 0 || a_size == 0) {
            return false;
        }

        std::uintptr_t cursor = a_address;
        const auto end = a_address + a_size;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0) {
                return false;
            }

            if (mbi.State != MEM_COMMIT) {
                return false;
            }

            const auto protect = mbi.Protect & 0xFF;
            if ((mbi.Protect & PAGE_GUARD) != 0 || protect == PAGE_NOACCESS) {
                return false;
            }

            const auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto regionEnd = regionBase + mbi.RegionSize;
            if (regionEnd <= cursor) {
                return false;
            }

            cursor = std::min(end, regionEnd);
        }

        return true;
    }

    [[nodiscard]] inline bool SafeReadQword(const std::uintptr_t a_address, std::uintptr_t& a_value)
    {
        if (!IsReadableRange(a_address, sizeof(std::uintptr_t))) {
            return false;
        }

        __try {
            a_value = *reinterpret_cast<const std::uintptr_t*>(a_address);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // --- Hooking ------------------------------------------------------------

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
}
