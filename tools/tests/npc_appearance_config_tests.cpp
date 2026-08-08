#include "NpcAppearance/Config.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string>

namespace
{
    std::size_t g_failed = 0;

    void Check(const bool a_condition, const char* a_name)
    {
        std::cout << (a_condition ? "PASS " : "FAIL ") << a_name << '\n';
        if (!a_condition) {
            ++g_failed;
        }
    }

    std::string Manifest(const int a_priority = 100,
                         const std::string& a_localFormID = "5983",
                         const std::string& a_preset = "Sarah.npc",
                         const std::string& a_plugin = "Starfield.esm")
    {
        return std::format(
            R"({{"schemaVersion":1,"priority":{},"requires":{{"plugins":["SharedAssets.esm"]}},"assignments":[{{"target":{{"plugin":"{}","localFormId":"{}"}},"preset":"{}"}}]}})",
            a_priority, a_plugin, a_localFormID, a_preset);
    }

    void Write(const std::filesystem::path& a_path, const std::string_view a_text)
    {
        std::filesystem::create_directories(a_path.parent_path());
        std::ofstream stream{ a_path, std::ios::binary };
        stream.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
    }

    bool HasPlugin(const NpcAppearance::Requirements& a_requirements,
                   const std::string_view a_plugin)
    {
        return std::ranges::any_of(a_requirements.plugins, [&](const auto& a_candidate) {
            return a_candidate == a_plugin;
        });
    }

    bool HasIssue(const std::vector<NpcAppearance::ManifestIssue>& a_issues,
                  const std::string_view a_code)
    {
        return std::ranges::any_of(a_issues, [&](const auto& a_issue) {
            return a_issue.code == a_code;
        });
    }

    NpcAppearance::SelectedAssignment SelectedFrom(
        const NpcAppearance::PackageManifest& a_package)
    {
        const auto& assignment = a_package.assignments.front();
        return {
            assignment.target,
            assignment.presetPath,
            assignment.requirements,
            a_package.packageID,
            a_package.priority
        };
    }
}

int main()
{
    namespace NA = NpcAppearance;
    const auto root = std::filesystem::absolute("tmp/npc-appearance-config-tests");
    std::filesystem::remove_all(root);
    const auto manifestPath = root / "author.sarah" / "package.json";
    Write(root / "author.sarah" / "Sarah.npc", "fixture");

    const auto valid = NA::ParsePackageManifest(Manifest(), manifestPath, true);
    Check(valid.manifest && valid.manifest->assignments.size() == 1 && valid.issues.empty(),
          "valid production manifest");
    Check(valid.manifest && valid.manifest->packageID == "author.sarah",
          "manifest package ID comes from its parent folder");
    Check(valid.manifest && valid.manifest->assignments[0].target.CanonicalKey() ==
                                 "starfield.esm:00005983",
          "canonical plugin-local target key");
    Check(valid.manifest &&
              HasPlugin(valid.manifest->assignments[0].requirements, "SharedAssets.esm") &&
              HasPlugin(valid.manifest->assignments[0].requirements, "Starfield.esm"),
          "pack and implicit target plugin requirements are inherited by the assignment");

    const auto omittedRequirements = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"5983"},"preset":"Sarah.npc"}]})",
        root / "author.no-requirements" / "package.json", false);
    Check(omittedRequirements.manifest && omittedRequirements.issues.empty() &&
              omittedRequirements.manifest->priority == 0 &&
              omittedRequirements.manifest->requirements.plugins.empty() &&
              omittedRequirements.manifest->requirements.assets.empty() &&
              omittedRequirements.manifest->assignments[0].requirements.plugins.size() == 1 &&
              HasPlugin(omittedRequirements.manifest->assignments[0].requirements,
                        "Starfield.esm") &&
              omittedRequirements.manifest->assignments[0].requirements.assets.empty(),
          "omitted priority and package requirements use their defaults");

    const auto pluginLocal = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":25,"requires":{"plugins":["SharedAssets.esm"]},"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"00005983"},"preset":"Sarah.npc","requires":{"plugins":["ExampleHairMod.esm"]}}]})",
        root / "author.plugin-local" / "package.json", false);
    Check(pluginLocal.manifest &&
              pluginLocal.manifest->assignments[0].target.plugin == "Starfield.esm" &&
              pluginLocal.manifest->assignments[0].target.localFormID == 0x00005983 &&
              pluginLocal.manifest->assignments[0].target.CanonicalKey() ==
                  "starfield.esm:00005983",
          "plugin-local target parses into a load-order-independent canonical key");
    Check(pluginLocal.manifest &&
              HasPlugin(pluginLocal.manifest->assignments[0].requirements,
                        "SharedAssets.esm") &&
              HasPlugin(pluginLocal.manifest->assignments[0].requirements,
                        "ExampleHairMod.esm") &&
              HasPlugin(pluginLocal.manifest->assignments[0].requirements,
                        "Starfield.esm") &&
              pluginLocal.manifest->assignments[0].requirements.plugins.size() == 3,
          "plugin-local owning plugin is an implicit additive requirement");
    Check(NA::IsLocalFormIDValidForTier(0x00FFFFFF, NA::PluginTier::kFull) &&
              !NA::IsLocalFormIDValidForTier(0x01000000, NA::PluginTier::kFull) &&
              NA::IsLocalFormIDValidForTier(0x0000FFFF, NA::PluginTier::kMedium) &&
              !NA::IsLocalFormIDValidForTier(0x00010000, NA::PluginTier::kMedium) &&
              NA::IsLocalFormIDValidForTier(0x00000FFF, NA::PluginTier::kSmall) &&
              !NA::IsLocalFormIDValidForTier(0x00001000, NA::PluginTier::kSmall),
          "full, medium, and small plugin local FormID bounds are enforced");
    Check(NA::EncodeRuntimeFormID(0x00123456u, NA::PluginTier::kFull, 5u) ==
                  0x05123456u &&
              NA::EncodeRuntimeFormID(0x00001234u, NA::PluginTier::kMedium, 3u) ==
                  0xFD031234u &&
              NA::EncodeRuntimeFormID(0x00000234u, NA::PluginTier::kSmall, 2u) ==
                  0xFE002234u,
          "full, medium, and small runtime FormID encoding is deterministic");

    const auto explicitPerPreset = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"requires":{"plugins":["SharedAssets.esm"],"assets":["Textures/Shared.dds"]},"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"29A8EB"},"preset":"Daniel.npc","requires":{"plugins":["ExampleHairMod.esm"],"assets":["Meshes/Hair/Example.mesh"]}}]})",
        root / "explicit-per-preset" / "package.json", false);
    Check(explicitPerPreset.manifest &&
              explicitPerPreset.manifest->assignments[0].requirements.plugins.size() == 3 &&
              HasPlugin(explicitPerPreset.manifest->assignments[0].requirements,
                        "SharedAssets.esm") &&
              HasPlugin(explicitPerPreset.manifest->assignments[0].requirements,
                        "ExampleHairMod.esm") &&
              explicitPerPreset.manifest->assignments[0].requirements.assets.size() == 2,
          "explicit per-assignment requirements are additive");

    const auto assetManifest = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"requires":{"assets":["Textures/Author/Required.dds"]},"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"5983"},"preset":"Sarah.npc"}]})",
        root / "author.assets" / "package.json", false);
    const auto dataRoot = root / "Data";
    Check(assetManifest.manifest &&
              !NA::CheckRequiredAssets(*assetManifest.manifest, dataRoot).Complete(),
          "missing required Data asset rejects runtime package");
    Write(dataRoot / "Textures" / "Author" / "Required.dds", "fixture");
    Check(assetManifest.manifest &&
              NA::CheckRequiredAssets(*assetManifest.manifest, dataRoot).Complete(),
          "present required Data asset passes runtime package");
    const auto originalCurrentPath = std::filesystem::current_path();
    std::filesystem::current_path(dataRoot);
    Check(assetManifest.manifest &&
              !NA::CheckRequiredAssets(*assetManifest.manifest, {}).Complete(),
          "unresolved Data root fails closed even when relative asset exists in cwd");
    std::filesystem::current_path(originalCurrentPath);

    const auto checkedInExample = NA::LoadPackageManifest(
        "fixtures/osf-identity/Packs/author.sarah-example/package.json", false);
    Check(checkedInExample.manifest &&
              checkedInExample.manifest->packageID == "author.sarah-example" &&
              checkedInExample.manifest->assignments[0].target.plugin == "Starfield.esm" &&
              checkedInExample.manifest->assignments[0].target.localFormID == 0x00005983,
          "checked-in stable example uses plugin-local targeting");

    const auto missing = NA::ParsePackageManifest(
        Manifest(0, "5983", "Missing.npc"),
        root / "author.missing" / "package.json", true);
    Check(missing.HasFatalError() && !missing.issues.empty(), "missing preset rejects package");

    const auto traversal = NA::ParsePackageManifest(
        Manifest(0, "5983", "../Sarah.npc"),
        root / "author.traversal" / "package.json", false);
    Check(traversal.HasFatalError(), "preset parent traversal rejected");

    const auto backslashTraversal = NA::ParsePackageManifest(
        Manifest(0, "5983", "Presets\\\\..\\\\Sarah.npc"),
        root / "author.backslash-traversal" / "package.json", false);
    Check(backslashTraversal.HasFatalError(),
          "Windows-style preset parent traversal rejected on every host");

    const auto absolute = NA::ParsePackageManifest(
        Manifest(0, "5983", "C:\\\\Sarah.npc"),
        root / "author.absolute" / "package.json", false);
    Check(absolute.HasFatalError(), "absolute preset path rejected");

    const auto wrongExtension = NA::ParsePackageManifest(
        Manifest(0, "5983", "Sarah.json"),
        root / "author.extension" / "package.json", false);
    Check(wrongExtension.HasFatalError(), "non-npc preset rejected");

    const auto removedScope = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"5983"},"preset":"Sarah.npc","scope":"faceAndBody"}]})",
        root / "author.scope" / "package.json", false);
    Check(removedScope.HasFatalError() && HasIssue(removedScope.issues, "unknown_property"),
          "removed scope property is rejected");

    const auto noncanonicalTarget = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"assignments":[{"target":{"editorId":"Companion_SarahMorgan"},"preset":"Sarah.npc"}]})",
        root / "author.noncanonical-target" / "package.json", false);
    Check(noncanonicalTarget.HasFatalError() &&
              HasIssue(noncanonicalTarget.issues, "unknown_property"),
          "noncanonical target property is rejected by strict validation");

    const auto partialPluginTarget = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[{"target":{"plugin":"Starfield.esm"},"preset":"Sarah.npc"}]})",
        root / "author.partial-target" / "package.json", false);
    Check(partialPluginTarget.HasFatalError() &&
              HasIssue(partialPluginTarget.issues, "missing_property"),
          "partial plugin-local target is rejected");

    const auto badTargetPlugin = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[{"target":{"plugin":"not-a-plugin","localFormId":"00005983"},"preset":"Sarah.npc"}]})",
        root / "author.bad-target-plugin" / "package.json", false);
    Check(badTargetPlugin.HasFatalError() &&
              HasIssue(badTargetPlugin.issues, "invalid_plugin"),
          "invalid target plugin is rejected");

    const auto emptyStemTargetPlugin = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"assignments":[{"target":{"plugin":".esm","localFormId":"5983"},"preset":"Sarah.npc"}]})",
        root / "author.empty-stem-target-plugin" / "package.json", false);
    Check(emptyStemTargetPlugin.HasFatalError() &&
              HasIssue(emptyStemTargetPlugin.issues, "invalid_plugin"),
          "target plugin filename requires a nonempty stem");

    const auto badLocalFormID = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"01000000"},"preset":"Sarah.npc"}]})",
        root / "author.bad-local-form" / "package.json", false);
    Check(badLocalFormID.HasFatalError() &&
              HasIssue(badLocalFormID.issues, "invalid_local_form_id"),
          "out-of-range plugin-local FormID is rejected");

    const auto prefixedLocalFormID = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"0x5983"},"preset":"Sarah.npc"}]})",
        root / "author.prefixed-local-form" / "package.json", false);
    Check(prefixedLocalFormID.HasFatalError() &&
              HasIssue(prefixedLocalFormID.issues, "invalid_local_form_id"),
          "0x-prefixed plugin-local FormID is rejected");

    const auto runtimeLocalFormID = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"01005983"},"preset":"Sarah.npc"}]})",
        root / "author.runtime-local-form" / "package.json", false);
    Check(runtimeLocalFormID.HasFatalError() &&
              HasIssue(runtimeLocalFormID.issues, "invalid_local_form_id"),
          "load-order-prefixed runtime FormID is rejected");

    const auto spidStyleTarget = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"assignments":[{"target":"Starfield.esm|00005983","preset":"Sarah.npc"}]})",
        root / "author.spid-style-target" / "package.json", false);
    Check(spidStyleTarget.HasFatalError() &&
              HasIssue(spidStyleTarget.issues, "wrong_type"),
          "SPID-style target string is rejected by strict validation");

    const auto badPriority = NA::ParsePackageManifest(
        Manifest(NA::kMaxPriority + 1),
        root / "author.priority" / "package.json", false);
    Check(badPriority.HasFatalError(), "out-of-range priority rejected");

    const auto wrongPriorityType = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":"high"})",
        root / "author.priority-type" / "package.json", false);
    Check(wrongPriorityType.HasFatalError() &&
              HasIssue(wrongPriorityType.issues, "wrong_type"),
          "present priority must be an integer");

    // A null requirements gate must not read as "no requirements": that would
    // apply the pack with none of its declared dependencies checked.
    const auto nullRequires = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"requires":null,"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"5983"},"preset":"Sarah.npc"}]})",
        root / "author.null-requires" / "package.json", false);
    Check(nullRequires.HasFatalError() && HasIssue(nullRequires.issues, "wrong_type"),
          "null requirements gate is rejected, not read as empty");

    const auto nullAssignments = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"assignments":null})",
        root / "author.null-assignments" / "package.json", false);
    Check(nullAssignments.HasFatalError() && HasIssue(nullAssignments.issues, "wrong_type"),
          "null assignments is rejected, not treated as a convention pack");

    const auto nullNestedRequires = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"requires":{"plugins":null}})",
        root / "author.null-plugins" / "package.json", false);
    Check(nullNestedRequires.HasFatalError() && HasIssue(nullNestedRequires.issues, "wrong_type"),
          "null inside requirements is rejected");

    const auto assetTraversal = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"requires":{"assets":["Textures\\..\\Hair.dds"]}})",
        root / "author.asset-traversal" / "package.json", false);
    Check(assetTraversal.HasFatalError() &&
              HasIssue(assetTraversal.issues, "invalid_asset_path"),
          "Windows-style asset parent traversal rejected on every host");

    const auto unknownVersion = NA::ParsePackageManifest(
        R"({"schemaVersion":2,"priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[]})",
        root / "version" / "package.json", false);
    Check(unknownVersion.HasFatalError(), "unknown schema rejected");

    const auto unknownProperty = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[],"surprise":true})",
        root / "unknown" / "package.json", false);
    Check(unknownProperty.HasFatalError(), "unknown root property rejected");

    const auto manifestPackageID = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"packageId":"legacy.override","priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[]})",
        root / "folder-is-authoritative" / "package.json", false);
    Check(manifestPackageID.HasFatalError(),
          "manifest packageId override is rejected because the folder is authoritative");

    const auto inferredConvention = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]}})",
        root / "no-format" / "package.json", false);
    Check(inferredConvention.manifest && inferredConvention.issues.empty() &&
              inferredConvention.manifest->format ==
                  NA::PackageFormat::kPluginFolderLocalFormID &&
              inferredConvention.manifest->assignments.empty(),
          "manifest without assignments infers plugin-folder convention");

    const auto flatConventionRoot = root / "flat-convention";
    Write(flatConventionRoot / "package.json",
          R"({"schemaVersion":1,"priority":0})");
    Write(flatConventionRoot / "00005983.npc", "fixture");
    const auto flatConvention = NA::LoadPackageManifest(
        flatConventionRoot / "package.json", true);
    Check(flatConvention.manifest && flatConvention.manifest->assignments.empty() &&
              HasIssue(flatConvention.issues, "invalid_convention_layout"),
          "flat root preset is invalid under the canonical convention");

    const auto noncanonicalConventionSelector = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"presetConvention":"pluginFolderLocalFormId"})",
        root / "noncanonical-convention-selector" / "package.json", false);
    Check(noncanonicalConventionSelector.HasFatalError() &&
              HasIssue(noncanonicalConventionSelector.issues, "unknown_property"),
          "redundant convention selector is rejected by strict validation");

    const auto malformed = NA::ParsePackageManifest(
        R"({"schemaVersion":)", root / "malformed" / "package.json", false);
    Check(malformed.HasFatalError(), "truncated JSON rejected");

    std::string oversized(NA::kMaxManifestBytes + 1, ' ');
    const auto invalidSize = NA::ParsePackageManifest(
        oversized, root / "oversized" / "package.json", false);
    Check(invalidSize.HasFatalError(), "manifest byte bound enforced");

    const std::string duplicateTargetJson =
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"5983"},"preset":"a.npc"},{"target":{"plugin":"STARFIELD.ESM","localFormId":"00005983"},"preset":"b.npc"}]})";
    const auto duplicateTarget = NA::ParsePackageManifest(
        duplicateTargetJson, root / "duplicate-target" / "package.json", false);
    Check(duplicateTarget.HasFatalError(), "duplicate target rejects package");

    const auto conventionRoot = root / "convention";
    Write(conventionRoot / "package.json",
          R"({"schemaVersion":1,"priority":100,"requires":{"plugins":["SharedAssets.esm"],"assets":["Textures/Shared.dds"]}})");
    Write(conventionRoot / "Starfield.esm" / "29A8EB.npc", "fixture");
    Write(conventionRoot / "Starfield.esm" / "29A8EB.json",
          R"({"schemaVersion":1,"requires":{"plugins":["ExampleHairMod.esm"],"assets":["Meshes/Hair/Example.mesh"]}})");
    Write(conventionRoot / "Starfield.esm" / "5983.npc", "fixture");
    const auto convention = NA::LoadPackageManifest(conventionRoot / "package.json", true);
    const auto daniel = convention.manifest ?
        std::ranges::find_if(convention.manifest->assignments, [](const auto& a_assignment) {
            return a_assignment.target.plugin == "Starfield.esm" &&
                a_assignment.target.localFormID == 0x0029A8EB;
        }) : std::vector<NA::Assignment>::const_iterator{};
    Check(convention.manifest &&
              convention.manifest->format ==
                  NA::PackageFormat::kPluginFolderLocalFormID &&
              convention.manifest->assignments.size() == 2,
          "plugin-folder convention discovers plugin-local presets");
    Check(convention.manifest && daniel != convention.manifest->assignments.end() &&
              HasPlugin(daniel->requirements, "SharedAssets.esm") &&
              HasPlugin(daniel->requirements, "ExampleHairMod.esm") &&
              HasPlugin(daniel->requirements, "Starfield.esm") &&
              daniel->requirements.assets.size() == 2,
          "convention sidecar and implicit owning-plugin requirements are additive");

    const auto aliasRoot = root / "convention-alias";
    Write(aliasRoot / "package.json", R"({"schemaVersion":1,"priority":0})");
    Write(aliasRoot / "Starfield.esm" / "5983.npc", "fixture");
    Write(aliasRoot / "STARFIELD.ESM" / "00005983.npc", "fixture");
    const auto aliases = NA::LoadPackageManifest(aliasRoot / "package.json", true);
    Check(aliases.manifest && aliases.manifest->assignments.empty() &&
              HasIssue(aliases.issues, "duplicate_target"),
          "equivalent flexible-hex and case-folded plugin targets are canonical duplicates");

    const auto optionalSidecarRoot = root / "optional-sidecar-requirements";
    Write(optionalSidecarRoot / "package.json",
          R"({"schemaVersion":1,"priority":0})");
    Write(optionalSidecarRoot / "Starfield.esm" / "5983.npc", "fixture");
    Write(optionalSidecarRoot / "Starfield.esm" / "5983.json",
          R"({"schemaVersion":1})");
    const auto optionalSidecar = NA::LoadPackageManifest(
        optionalSidecarRoot / "package.json", true);
    Check(optionalSidecar.manifest && optionalSidecar.issues.empty() &&
              optionalSidecar.manifest->assignments.size() == 1 &&
              optionalSidecar.manifest->assignments[0].requirements.plugins.size() == 1 &&
              HasPlugin(optionalSidecar.manifest->assignments[0].requirements,
                        "Starfield.esm") &&
              optionalSidecar.manifest->assignments[0].requirements.assets.empty(),
          "omitted sidecar requirements default to only the implicit owning plugin");

    const auto isolatedRoot = root / "isolated-invalid";
    Write(isolatedRoot / "package.json",
          R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]}})");
    Write(isolatedRoot / "Starfield.esm" / "5984.npc", "fixture");
    Write(isolatedRoot / "Starfield.esm" / "5984.json", "{");
    Write(isolatedRoot / "Starfield.esm" / "5983.npc", "fixture");
    Write(isolatedRoot / "Starfield.esm" / "0x5985.npc", "fixture");
    Write(isolatedRoot / "Starfield.esm" / "7777.json",
          R"({"schemaVersion":1,"requires":{"plugins":[],"assets":[]}})");
    Write(isolatedRoot / "Starfield.esm" / "Nested" / "5986.npc", "fixture");
    const auto isolated = NA::LoadPackageManifest(isolatedRoot / "package.json", true);
    Check(isolated.manifest && isolated.manifest->assignments.size() == 1 &&
              isolated.manifest->assignments[0].target.localFormID == 0x00005983,
          "malformed sidecar and invalid filename disable only affected presets");
    Check(HasIssue(isolated.issues, "invalid_preset_metadata_json") &&
              HasIssue(isolated.issues, "invalid_convention_form_id") &&
              HasIssue(isolated.issues, "orphan_preset_metadata") &&
              HasIssue(isolated.issues, "invalid_convention_layout"),
          "convention diagnostics cover malformed, orphaned, and nested entries");

    const auto missingPackageRoot = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]}})",
        root / "missing-preset-root" / "package.json", true);
    Check(missingPackageRoot.manifest && missingPackageRoot.manifest->assignments.empty() &&
              HasIssue(missingPackageRoot.issues, "package_root_missing"),
          "missing convention package root leaves package non-mutating");

    const auto limitRoot = root / "convention-limit";
    Write(limitRoot / "package.json",
          R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]}})");
    for (std::uint32_t i = 0; i <= NA::kMaxAssignments; ++i) {
        Write(limitRoot / "Starfield.esm" / std::format("{:X}.npc", i + 1),
              "fixture");
    }
    const auto overLimit = NA::LoadPackageManifest(limitRoot / "package.json", true);
    Check(overLimit.manifest && overLimit.manifest->assignments.empty() &&
              HasIssue(overLimit.issues, "assignment_limit_exceeded"),
          "convention assignment safety limit fails closed");

    const auto checkedConventionExample = NA::LoadPackageManifest(
        "fixtures/osf-identity/Packs/project.community-example/package.json", false);
    Check(checkedConventionExample.manifest &&
              checkedConventionExample.manifest->format ==
                  NA::PackageFormat::kPluginFolderLocalFormID,
          "checked-in convention example matches runtime schema");

    const auto low = NA::ParsePackageManifest(
        Manifest(10), root / "author.low" / "package.json", false);
    const auto highZ = NA::ParsePackageManifest(
        Manifest(20), root / "author.z-high" / "package.json", false);
    const auto highA = NA::ParsePackageManifest(
        Manifest(20), root / "Author A High!" / "package.json", false);
    std::vector<NA::PackageManifest> packages{ *low.manifest, *highZ.manifest, *highA.manifest };
    const auto selection = NA::SelectAssignments(packages);
    Check(selection.winners.size() == 1 && selection.winners[0].packageID == "Author A High!",
          "highest priority then case-insensitive ascending folder name wins");
    Check(selection.decisions.size() == 3, "all conflict decisions reported");
    auto invalidHigh = *highA.manifest;
    invalidHigh.assignments.clear();
    const auto promoted = NA::SelectAssignments({ invalidHigh, *low.manifest });
    Check(promoted.winners.size() == 1 &&
              promoted.winners[0].packageID == "author.low",
          "removing an invalid high-priority candidate promotes a valid lower candidate");

    auto lowResolvedAssignment = SelectedFrom(*low.manifest);
    auto pluginResolvedAssignment = SelectedFrom(*pluginLocal.manifest);
    const auto resolvedAliasConflict = NA::SelectResolvedAssignments({
        { 0x00005983, lowResolvedAssignment },
        { 0x00005983, pluginResolvedAssignment }
    });
    Check(resolvedAliasConflict.winners.size() == 1 &&
              resolvedAliasConflict.winners[0].assignment.packageID ==
                  "author.plugin-local" &&
              resolvedAliasConflict.decisions.size() == 2,
          "plugin-local candidates compete by resolved base FormID");

    auto samePackageFirst = SelectedFrom(*valid.manifest);
    auto samePackagePlugin = SelectedFrom(*pluginLocal.manifest);
    samePackagePlugin.target.plugin = "Other.esm";
    samePackagePlugin.packageID = samePackageFirst.packageID;
    samePackagePlugin.priority = samePackageFirst.priority;
    const auto rejectedResolvedAlias = NA::SelectResolvedAssignments({
        { 0x00005983, samePackageFirst },
        { 0x00005983, samePackagePlugin }
    });
    Check(rejectedResolvedAlias.winners.empty() &&
              rejectedResolvedAlias.rejectedPackages.size() == 1 &&
              rejectedResolvedAlias.decisions.size() == 2 &&
              std::ranges::all_of(rejectedResolvedAlias.decisions, [](const auto& a_decision) {
                  return !a_decision.winner &&
                      a_decision.reason ==
                          "package_rejected_duplicate_resolved_target";
              }),
          "same-package locators resolving to one base reject the package");

    const auto resolvedFallback = NA::SelectResolvedAssignments({
        { 0x00005983, lowResolvedAssignment }
    });
    Check(resolvedFallback.winners.size() == 1 &&
              resolvedFallback.winners[0].assignment.packageID == "author.low",
          "an invalid higher candidate omitted by validation cannot block resolved fallback");

    const auto discoveryRoot = root / "discovery";
    Write(discoveryRoot / "Duplicate A!" / "package.json", Manifest(1));
    Write(discoveryRoot / "Duplicate A!" / "Sarah.npc", "fixture");
    Write(discoveryRoot / "Duplicate B (Alternate)" / "package.json", Manifest(2));
    Write(discoveryRoot / "Duplicate B (Alternate)" / "Sarah.npc", "fixture");
    Write(discoveryRoot / "Unique Pack #3" / "package.json", Manifest(3));
    Write(discoveryRoot / "Unique Pack #3" / "Sarah.npc", "fixture");
    const auto discovery = NA::DiscoverPackages(discoveryRoot, true);
    Check(discovery.packages.size() == 3 &&
              std::ranges::any_of(discovery.packages, [](const auto& a_package) {
                  return a_package.packageID == "Unique Pack #3";
              }),
          "arbitrary manifest pack folder names become their package IDs");
    Check(discovery.issues.empty(), "distinct folder-derived package IDs need no manifest IDs");
    Check(!discovery.packages.empty() && !discovery.packages[0].implicitManifest,
          "package with a package.json is not reported as implicit");

    const auto implicitRoot = root / "implicit-discovery";
    Write(implicitRoot / "Author.MyPack" / "Starfield.esm" / "29A8EB.npc", "fixture");
    Write(implicitRoot / "Author.MyPack" / "Starfield.esm" / "29A8EB.json",
          R"({"schemaVersion":1,"requires":{"plugins":[],"assets":[]}})");
    Write(implicitRoot / "My Cool Pack!" / "Starfield.esm" / "5983.npc", "fixture");
    Write(implicitRoot / "ab" / "Starfield.esm" / "5983.npc", "fixture");
    Write(implicitRoot / "author.suspect" / "package.jsn", "{}");
    Write(implicitRoot / "author.suspect" / "Starfield.esm" / "5983.npc", "fixture");
    Write(implicitRoot / "author.stray-json" / "notes.json", "{}");
    Write(implicitRoot / "author.stray-json" / "Starfield.esm" / "5983.npc",
          "fixture");
    Write(implicitRoot / "author.nested" / "inner" / "package.json", Manifest());
    Write(implicitRoot / "author.nested" / "inner" / "Sarah.npc", "fixture");
    std::filesystem::create_directories(implicitRoot / "author.empty");
    Write(implicitRoot / "loose-note.txt", "stray");
    const auto implicitDiscovery = NA::DiscoverPackages(implicitRoot, true);
    const auto implicitPack = std::ranges::find_if(
        implicitDiscovery.packages, [](const auto& a_package) {
            return a_package.packageID == "Author.MyPack";
        });
    Check(implicitPack != implicitDiscovery.packages.end() &&
              implicitPack->implicitManifest && implicitPack->priority == 0 &&
              implicitPack->format == NA::PackageFormat::kPluginFolderLocalFormID &&
              implicitPack->assignments.size() == 1 &&
              implicitPack->assignments[0].target.CanonicalKey() ==
                  "starfield.esm:0029a8eb",
          "manifest-less package preserves its folder name as the package ID at priority 0");
    Check(implicitDiscovery.packages.size() == 4 &&
              std::ranges::any_of(implicitDiscovery.packages, [](const auto& a_package) {
                  return a_package.packageID == "author.empty" && a_package.assignments.empty();
              }) &&
              std::ranges::any_of(implicitDiscovery.packages, [](const auto& a_package) {
                  return a_package.packageID == "My Cool Pack!";
              }) &&
              std::ranges::any_of(implicitDiscovery.packages, [](const auto& a_package) {
                  return a_package.packageID == "ab";
              }),
          "arbitrary and short manifest-less folder names are accepted; an empty pack stays non-mutating");
    Check(HasIssue(implicitDiscovery.issues, "suspect_package_root_file") &&
              HasIssue(implicitDiscovery.issues, "manifest_not_at_package_root") &&
              HasIssue(implicitDiscovery.issues, "stray_package_root_file"),
          "manifest-less discovery diagnoses near-miss and stray files, and nested manifests");

    const auto mixedRoot = root / "implicit-vs-explicit";
    Write(mixedRoot / "author.implicit-pack" / "Starfield.esm" / "5983.npc",
          "fixture");
    Write(mixedRoot / "author.explicit-pack" / "package.json",
          R"({"schemaVersion":1,"priority":100,"requires":{"plugins":[],"assets":[]}})");
    Write(mixedRoot / "author.explicit-pack" / "Starfield.esm" / "00005983.npc",
          "fixture");
    const auto mixedDiscovery = NA::DiscoverPackages(mixedRoot, true);
    const auto mixedSelection = NA::SelectAssignments(mixedDiscovery.packages);
    Check(mixedDiscovery.packages.size() == 2 && mixedSelection.winners.size() == 1 &&
              mixedSelection.winners[0].packageID == "author.explicit-pack",
          "an explicit manifest priority outranks a manifest-less package at the same target");

    const auto checkedInPacks =
        NA::DiscoverPackages("fixtures/osf-identity/Packs", false);
    Check(checkedInPacks.packages.size() == 3 &&
              std::ranges::any_of(checkedInPacks.packages, [](const auto& a_package) {
                  return a_package.packageID == "author.folder-only-example" &&
                      a_package.implicitManifest;
              }) &&
              checkedInPacks.issues.empty(),
          "checked-in example packs discover cleanly, including the manifest-less one");

    std::cout << "RESULT failed=" << g_failed << '\n';
    return g_failed == 0 ? 0 : 1;
}
