#pragma once

#include <algorithm>

#include <cstddef>
#include <cstdint>

#include "REX/FModule.h"

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
        return true;
    }

    [[nodiscard]] inline bool SafeReadQword(const std::uintptr_t a_address, std::uintptr_t& a_value)
    {
        if (!IsReadableRange(a_address, sizeof(std::uintptr_t))) {
            return false;
        }
        
        a_value = *reinterpret_cast<const std::uintptr_t*>(a_address);
        return true;
    }
}
