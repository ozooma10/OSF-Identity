#include <filesystem>
#include "./Config.h"
#include "./PackScanner.h"
#include "Runtime/OverlayRuntime.h"

namespace Config::PackScanner
{
    namespace 
    {
        std::filesystem::path DefaultPacksDirectory()
        {
            return std::filesystem::path{
                REX::FModule::GetCurrentModule().GetFileName()
            }.parent_path() / L"OSFIdentity" / L"Packs";
        }


        std::unordered_map<RE::TESFormID, SelectedAssignment> RunScan(const std::filesystem::path& a_packsRoot)
        {
            static_cast<void>(a_packsRoot);
            return {};
        }
    }

    void ScanPacks()
    {
        const auto packsRoot = DefaultPacksDirectory();
        std::error_code ec;
        const bool packsPresent = std::filesystem::is_directory(packsRoot, ec) && !ec;
        if (!packsPresent) {
            REX::INFO("[PackScanner] startup disabled: packs directory is absent ({})", packsRoot.string());
            return;
        }

        auto resolvedAssignments = RunScan(packsRoot);
        if (resolvedAssignments.empty()) {
            REX::WARN("[PackScanner] startup found no valid assignments;");
            return;
        }

        Runtime::GetOverlayRuntime().Arm(std::move(resolvedAssignments));
    }
}
