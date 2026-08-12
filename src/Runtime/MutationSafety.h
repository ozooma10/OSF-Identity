#pragma once

#include <string_view>

namespace Runtime
{
    bool IsMutationOperational();

    void KillMutation(std::string_view a_reason);
}
