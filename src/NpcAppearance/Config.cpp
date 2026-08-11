#include "NpcAppearance/Config.h"
#include "NpcAppearance/ConfigDetail.h"

namespace NpcAppearance
{
    std::string Target::CanonicalKey() const
    {
        return std::format("{}:{:08x}", Detail::FoldASCII(plugin), localFormID);
    }
}
