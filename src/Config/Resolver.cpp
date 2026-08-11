#include "./Resolver.h"


namespace Config
{
    namespace 
    {
        void AddIssue(ResolvedAppearanceDependencies& a_result, std::string a_field, std::string a_value, std::string a_code, std::string a_message)
        {
            a_result.issues.push_back(DependencyIssue{std::move(a_field), std::move(a_value), std::move(a_code), std::move(a_message) });
        }

    }

    ResolvedAppearanceDependencies Config::ResolveAppearanceDependencies(const AppearancePreset &a_preset, RE::TESNPC *a_target)
{
    return ResolvedAppearanceDependencies();
}
}