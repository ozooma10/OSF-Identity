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
                         const std::string& a_editorID = "Companion_SarahMorgan",
                         const std::string& a_preset = "Sarah.npc",
                         const std::string& a_scope = "faceAndBody")
    {
        return std::format(
            R"({{"schemaVersion":1,"priority":{},"requires":{{"plugins":["Starfield.esm"],"assets":[]}},"assignments":[{{"target":{{"editorId":"{}"}},"preset":"{}","scope":"{}"}}]}})",
            a_priority, a_editorID, a_preset, a_scope);
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

    const NpcAppearance::EditorIDTarget* EditorTarget(
        const NpcAppearance::Target& a_target)
    {
        return a_target.AsEditorID();
    }

    const NpcAppearance::PluginLocalFormIDTarget* PluginLocalTarget(
        const NpcAppearance::Target& a_target)
    {
        return a_target.AsPluginLocalFormID();
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
                                 "companion_sarahmorgan",
          "canonical case-insensitive EditorID target key");
    Check(valid.manifest &&
              EditorTarget(valid.manifest->assignments[0].target) &&
              !PluginLocalTarget(valid.manifest->assignments[0].target),
          "EditorID target uses the typed EditorID locator");
    Check(valid.manifest &&
              HasPlugin(valid.manifest->assignments[0].requirements, "Starfield.esm"),
          "pack plugin requirement is inherited by the assignment");

    const auto pluginLocal = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":25,"requires":{"plugins":["SharedAssets.esm"],"assets":[]},"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"00005983"},"preset":"Sarah.npc","scope":"faceAndBody","requires":{"plugins":["ExampleHairMod.esm"],"assets":[]}}]})",
        root / "author.plugin-local" / "package.json", false);
    const auto* pluginLocalValue = pluginLocal.manifest ?
        PluginLocalTarget(pluginLocal.manifest->assignments[0].target) : nullptr;
    Check(pluginLocal.manifest && pluginLocalValue &&
              pluginLocalValue->plugin == "Starfield.esm" &&
              pluginLocalValue->localFormID == 0x00005983 &&
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
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":["SharedAssets.esm"],"assets":["Textures/Shared.dds"]},"assignments":[{"target":{"editorId":"Crew_ConstellationDaniel"},"preset":"Daniel.npc","scope":"faceAndBody","requires":{"plugins":["ExampleHairMod.esm"],"assets":["Meshes/Hair/Example.mesh"]}}]})",
        root / "explicit-per-preset" / "package.json", false);
    Check(explicitPerPreset.manifest &&
              explicitPerPreset.manifest->assignments[0].requirements.plugins.size() == 2 &&
              HasPlugin(explicitPerPreset.manifest->assignments[0].requirements,
                        "SharedAssets.esm") &&
              HasPlugin(explicitPerPreset.manifest->assignments[0].requirements,
                        "ExampleHairMod.esm") &&
              explicitPerPreset.manifest->assignments[0].requirements.assets.size() == 2,
          "explicit per-assignment requirements are additive");

    const auto assetManifest = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":["Textures/Author/Required.dds"]},"assignments":[{"target":{"editorId":"Companion_SarahMorgan"},"preset":"Sarah.npc","scope":"faceAndBody"}]})",
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
    const auto* checkedInTarget = checkedInExample.manifest ?
        PluginLocalTarget(checkedInExample.manifest->assignments[0].target) : nullptr;
    Check(checkedInExample.manifest && checkedInTarget &&
              checkedInExample.manifest->packageID == "author.sarah-example" &&
              checkedInTarget->plugin == "Starfield.esm" &&
              checkedInTarget->localFormID == 0x00005983,
          "checked-in stable example uses plugin-local targeting");

    const auto missing = NA::ParsePackageManifest(
        Manifest(0, "Companion_SarahMorgan", "Missing.npc"),
        root / "author.missing" / "package.json", true);
    Check(missing.HasFatalError() && !missing.issues.empty(), "missing preset rejects package");

    const auto traversal = NA::ParsePackageManifest(
        Manifest(0, "Companion_SarahMorgan", "../Sarah.npc"),
        root / "author.traversal" / "package.json", false);
    Check(traversal.HasFatalError(), "preset parent traversal rejected");

    const auto absolute = NA::ParsePackageManifest(
        Manifest(0, "Companion_SarahMorgan", "C:\\\\Sarah.npc"),
        root / "author.absolute" / "package.json", false);
    Check(absolute.HasFatalError(), "absolute preset path rejected");

    const auto wrongExtension = NA::ParsePackageManifest(
        Manifest(0, "Companion_SarahMorgan", "Sarah.json"),
        root / "author.extension" / "package.json", false);
    Check(wrongExtension.HasFatalError(), "non-npc preset rejected");

    const auto wrongScope = NA::ParsePackageManifest(
        Manifest(0, "Companion_SarahMorgan", "Sarah.npc", "faceOnly"),
        root / "author.scope" / "package.json", false);
    Check(wrongScope.HasFatalError(), "unproven scope rejected");

    const auto badEditorID = NA::ParsePackageManifest(
        Manifest(0, "Companion Sarah/Morgan"),
        root / "author.editor" / "package.json", false);
    Check(badEditorID.HasFatalError(), "invalid EditorID rejected");

    const auto mixedTargetLocators = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[{"target":{"editorId":"Companion_SarahMorgan","plugin":"Starfield.esm","localFormId":"00005983"},"preset":"Sarah.npc","scope":"faceAndBody"}]})",
        root / "author.mixed-target" / "package.json", false);
    Check(mixedTargetLocators.HasFatalError() &&
              HasIssue(mixedTargetLocators.issues, "invalid_target_locator"),
          "mixed EditorID and plugin-local target is rejected");

    const auto partialPluginTarget = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[{"target":{"plugin":"Starfield.esm"},"preset":"Sarah.npc","scope":"faceAndBody"}]})",
        root / "author.partial-target" / "package.json", false);
    Check(partialPluginTarget.HasFatalError() &&
              HasIssue(partialPluginTarget.issues, "invalid_target_locator"),
          "partial plugin-local target is rejected");

    const auto badTargetPlugin = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[{"target":{"plugin":"not-a-plugin","localFormId":"00005983"},"preset":"Sarah.npc","scope":"faceAndBody"}]})",
        root / "author.bad-target-plugin" / "package.json", false);
    Check(badTargetPlugin.HasFatalError() &&
              HasIssue(badTargetPlugin.issues, "invalid_plugin"),
          "invalid target plugin is rejected");

    const auto badLocalFormID = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"01000000"},"preset":"Sarah.npc","scope":"faceAndBody"}]})",
        root / "author.bad-local-form" / "package.json", false);
    Check(badLocalFormID.HasFatalError() &&
              HasIssue(badLocalFormID.issues, "invalid_local_form_id"),
          "out-of-range plugin-local FormID is rejected");

    const auto badPriority = NA::ParsePackageManifest(
        Manifest(NA::kMaxPriority + 1),
        root / "author.priority" / "package.json", false);
    Check(badPriority.HasFatalError(), "out-of-range priority rejected");

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

    const auto missingFormat = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]}})",
        root / "no-format" / "package.json", false);
    Check(missingFormat.HasFatalError(), "package requires exactly one authoring format");

    const auto mixedFormats = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"presetConvention":"editorIdFilename","assignments":[{"target":{"editorId":"Companion_SarahMorgan"},"preset":"Sarah.npc","scope":"faceAndBody"}]})",
        root / "mixed-format" / "package.json", false);
    Check(mixedFormats.HasFatalError(), "explicit and convention formats cannot be mixed");

    const auto unknownConvention = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"presetConvention":"pluginFolderLocalFormId"})",
        root / "unknown-convention" / "package.json", false);
    Check(unknownConvention.HasFatalError(), "unknown preset convention rejected");

    const auto malformed = NA::ParsePackageManifest(
        R"({"schemaVersion":)", root / "malformed" / "package.json", false);
    Check(malformed.HasFatalError(), "truncated JSON rejected");

    std::string oversized(NA::kMaxManifestBytes + 1, ' ');
    const auto invalidSize = NA::ParsePackageManifest(
        oversized, root / "oversized" / "package.json", false);
    Check(invalidSize.HasFatalError(), "manifest byte bound enforced");

    const std::string duplicateTargetJson =
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[{"target":{"editorId":"Companion_SarahMorgan"},"preset":"a.npc","scope":"faceAndBody"},{"target":{"editorId":"companion_sarahmorgan"},"preset":"b.npc","scope":"faceAndBody"}]})";
    const auto duplicateTarget = NA::ParsePackageManifest(
        duplicateTargetJson, root / "duplicate-target" / "package.json", false);
    Check(duplicateTarget.HasFatalError(), "duplicate target rejects package");

    const auto conventionRoot = root / "convention";
    Write(conventionRoot / "package.json",
          R"({"schemaVersion":1,"priority":100,"requires":{"plugins":["SharedAssets.esm"],"assets":["Textures/Shared.dds"]},"presetConvention":"editorIdFilename"})");
    Write(conventionRoot / "Crew_ConstellationDaniel.npc", "fixture");
    Write(conventionRoot / "Crew_ConstellationDaniel.json",
          R"({"schemaVersion":1,"requires":{"plugins":["ExampleHairMod.esm"],"assets":["Meshes/Hair/Example.mesh"]}})");
    Write(conventionRoot / "Companion_SarahMorgan.npc", "fixture");
    const auto convention = NA::LoadPackageManifest(conventionRoot / "package.json", true);
    const auto daniel = convention.manifest ?
        std::ranges::find_if(convention.manifest->assignments, [](const auto& a_assignment) {
            const auto* target = EditorTarget(a_assignment.target);
            return target && target->editorID == "Crew_ConstellationDaniel";
        }) : std::vector<NA::Assignment>::const_iterator{};
    Check(convention.manifest &&
              convention.manifest->format == NA::PackageFormat::kEditorIDFilename &&
              convention.manifest->assignments.size() == 2,
          "flat convention discovers direct EditorID presets");
    Check(convention.manifest && daniel != convention.manifest->assignments.end() &&
              HasPlugin(daniel->requirements, "SharedAssets.esm") &&
              HasPlugin(daniel->requirements, "ExampleHairMod.esm") &&
              daniel->requirements.assets.size() == 2,
          "convention sidecar requirements are additive");

    const auto isolatedRoot = root / "isolated-invalid";
    Write(isolatedRoot / "package.json",
          R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"presetConvention":"editorIdFilename"})");
    Write(isolatedRoot / "BrokenSidecar.npc", "fixture");
    Write(isolatedRoot / "BrokenSidecar.json", "{");
    Write(isolatedRoot / "ValidEditorID.npc", "fixture");
    Write(isolatedRoot / "Invalid-Editor-ID.npc", "fixture");
    Write(isolatedRoot / "Orphan.json",
          R"({"schemaVersion":1,"requires":{"plugins":[],"assets":[]}})");
    Write(isolatedRoot / "Nested" / "NestedEditorID.npc", "fixture");
    const auto isolated = NA::LoadPackageManifest(isolatedRoot / "package.json", true);
    const auto* isolatedTarget = isolated.manifest &&
            isolated.manifest->assignments.size() == 1 ?
        EditorTarget(isolated.manifest->assignments[0].target) : nullptr;
    Check(isolatedTarget && isolatedTarget->editorID == "ValidEditorID",
          "malformed sidecar and invalid filename disable only affected presets");
    Check(HasIssue(isolated.issues, "invalid_preset_metadata_json") &&
              HasIssue(isolated.issues, "invalid_convention_editor_id") &&
              HasIssue(isolated.issues, "orphan_preset_metadata") &&
              HasIssue(isolated.issues, "invalid_convention_layout"),
          "convention diagnostics cover malformed, orphaned, and nested entries");

    const auto missingPackageRoot = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"presetConvention":"editorIdFilename"})",
        root / "missing-preset-root" / "package.json", true);
    Check(missingPackageRoot.manifest && missingPackageRoot.manifest->assignments.empty() &&
              HasIssue(missingPackageRoot.issues, "package_root_missing"),
          "missing convention package root leaves package non-mutating");

    const auto limitRoot = root / "convention-limit";
    Write(limitRoot / "package.json",
          R"({"schemaVersion":1,"priority":0,"requires":{"plugins":[],"assets":[]},"presetConvention":"editorIdFilename"})");
    for (std::uint32_t i = 0; i <= NA::kMaxAssignments; ++i) {
        Write(limitRoot / std::format("Npc_{:08X}.npc", i + 1),
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
                  NA::PackageFormat::kEditorIDFilename,
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

    auto editorResolvedAssignment = SelectedFrom(*low.manifest);
    auto pluginResolvedAssignment = SelectedFrom(*pluginLocal.manifest);
    const auto resolvedAliasConflict = NA::SelectResolvedAssignments({
        { 0x00005983, editorResolvedAssignment },
        { 0x00005983, pluginResolvedAssignment }
    });
    Check(resolvedAliasConflict.winners.size() == 1 &&
              resolvedAliasConflict.winners[0].assignment.packageID ==
                  "author.plugin-local" &&
              resolvedAliasConflict.decisions.size() == 2,
          "EditorID and plugin-local locators compete by resolved base FormID");

    auto samePackageEditor = SelectedFrom(*valid.manifest);
    auto samePackagePlugin = SelectedFrom(*pluginLocal.manifest);
    samePackagePlugin.packageID = samePackageEditor.packageID;
    samePackagePlugin.priority = samePackageEditor.priority;
    const auto rejectedResolvedAlias = NA::SelectResolvedAssignments({
        { 0x00005983, samePackageEditor },
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
        { 0x00005983, editorResolvedAssignment }
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
    Write(implicitRoot / "Author.MyPack" / "Crew_ConstellationDaniel.npc", "fixture");
    Write(implicitRoot / "Author.MyPack" / "Crew_ConstellationDaniel.json",
          R"({"schemaVersion":1,"requires":{"plugins":[],"assets":[]}})");
    Write(implicitRoot / "My Cool Pack!" / "Companion_SarahMorgan.npc", "fixture");
    Write(implicitRoot / "ab" / "Companion_SarahMorgan.npc", "fixture");
    Write(implicitRoot / "author.suspect" / "package.jsn", "{}");
    Write(implicitRoot / "author.suspect" / "Companion_SarahMorgan.npc", "fixture");
    Write(implicitRoot / "author.stray-json" / "notes.json", "{}");
    Write(implicitRoot / "author.stray-json" / "Companion_SarahMorgan.npc",
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
              implicitPack->format == NA::PackageFormat::kEditorIDFilename &&
              implicitPack->assignments.size() == 1 &&
              implicitPack->assignments[0].target.CanonicalKey() == "crew_constellationdaniel",
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
    Write(mixedRoot / "author.implicit-pack" / "Companion_SarahMorgan.npc",
          "fixture");
    Write(mixedRoot / "author.explicit-pack" / "package.json",
          R"({"schemaVersion":1,"priority":100,"requires":{"plugins":[],"assets":[]},"presetConvention":"editorIdFilename"})");
    Write(mixedRoot / "author.explicit-pack" / "Companion_SarahMorgan.npc",
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
