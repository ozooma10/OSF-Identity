#pragma once

#include "PreparedAssignment.h"

namespace Config
{
    std::filesystem::path DefaultPacksDirectory();
    PreparedAssignmentMap RunScan(const std::filesystem::path& a_packsRoot);
}