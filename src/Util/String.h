#pragma once

#include <string>
#include <string_view>

namespace Util
{
    inline std::string FoldASCII(const std::string_view a_text)
    {
        std::string folded{ a_text };
        for (char& ch : folded) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        return folded;
    }

    inline const char* SafeText(const char* a_text)
    {
        return a_text ? a_text : "";
    }
}
