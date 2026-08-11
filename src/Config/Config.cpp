#include "Config.h"
#include "Util/String.h"

namespace Config
{
    std::string Target::CanonicalKey() const
    {
        return std::format("{}:{:08x}", Util::FoldASCII(plugin), localFormID);
    }
}
