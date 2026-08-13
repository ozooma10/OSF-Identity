#pragma once

#include <string_view>

namespace Runtime
{
    bool IsRuntimeOperational();
    void KillRuntime(std::string_view a_reason);
}
