#include "Probe/NpcAppearanceConfig.h"

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

    std::string Manifest(const std::string& a_packageID = "author.sarah",
                         const int a_priority = 100,
                         const std::string& a_plugin = "Starfield.esm",
                         const std::string& a_formID = "00005983",
                         const std::string& a_preset = "Presets/Sarah.npc",
                         const std::string& a_scope = "faceAndBody")
    {
        return std::format(
            R"({{"schemaVersion":1,"packageId":"{}","priority":{},"requires":{{"plugins":["{}"],"assets":[]}},"assignments":[{{"target":{{"plugin":"{}","localFormId":"{}"}},"preset":"{}","scope":"{}"}}]}})",
            a_packageID, a_priority, a_plugin, a_plugin, a_formID, a_preset, a_scope);
    }

    void Write(const std::filesystem::path& a_path, const std::string_view a_text)
    {
        std::filesystem::create_directories(a_path.parent_path());
        std::ofstream stream{ a_path, std::ios::binary };
        stream.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
    }

    bool HasPlugin(const Probe::NpcAppearance::Requirements& a_requirements,
                   const std::string_view a_plugin)
    {
        return std::ranges::any_of(a_requirements.plugins, [&](const auto& a_candidate) {
            return a_candidate == a_plugin;
        });
    }

    bool HasIssue(const std::vector<Probe::NpcAppearance::ManifestIssue>& a_issues,
                  const std::string_view a_code)
    {
        return std::ranges::any_of(a_issues, [&](const auto& a_issue) {
            return a_issue.code == a_code;
        });
    }
}

int main()
{
    namespace NA = Probe::NpcAppearance;
    const auto root = std::filesystem::absolute("tmp/npc-appearance-config-tests");
    std::filesystem::remove_all(root);
    const auto manifestPath = root / "author.sarah" / "package.json";
    Write(root / "author.sarah" / "Presets" / "Sarah.npc", "fixture");

    const auto valid = NA::ParsePackageManifest(Manifest(), manifestPath, true);
    Check(valid.manifest && valid.manifest->assignments.size() == 1 && valid.issues.empty(),
          "valid production manifest");
    Check(valid.manifest && valid.manifest->assignments[0].target.CanonicalKey() ==
                                 "starfield.esm:00005983",
          "canonical load-order-independent target key");
    Check(valid.manifest &&
              HasPlugin(valid.manifest->assignments[0].requirements, "Starfield.esm"),
          "target-owning plugin is an implicit assignment requirement");
    Check(NA::IsLocalFormIDValidForTier(0x00FFFFFF, NA::PluginTier::kFull) &&
              !NA::IsLocalFormIDValidForTier(0x01000000, NA::PluginTier::kFull) &&
              NA::IsLocalFormIDValidForTier(0x0000FFFF, NA::PluginTier::kMedium) &&
              !NA::IsLocalFormIDValidForTier(0x00010000, NA::PluginTier::kMedium) &&
              NA::IsLocalFormIDValidForTier(0x00000FFF, NA::PluginTier::kSmall) &&
              !NA::IsLocalFormIDValidForTier(0x00001000, NA::PluginTier::kSmall),
          "full, medium, and small plugin local FormID bounds enforced");

    const auto explicitPerPreset = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"packageId":"author.per-preset","priority":0,"requires":{"plugins":["SharedAssets.esm"],"assets":["Textures/Shared.dds"]},"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"0029A8EB"},"preset":"Presets/Daniel.npc","scope":"faceAndBody","requires":{"plugins":["ExampleHairMod.esm"],"assets":["Meshes/Hair/Example.mesh"]}}]})",
        root / "explicit-per-preset" / "package.json", false);
    Check(explicitPerPreset.manifest &&
              explicitPerPreset.manifest->assignments[0].requirements.plugins.size() == 3 &&
              HasPlugin(explicitPerPreset.manifest->assignments[0].requirements,
                        "SharedAssets.esm") &&
              HasPlugin(explicitPerPreset.manifest->assignments[0].requirements,
                        "ExampleHairMod.esm") &&
              HasPlugin(explicitPerPreset.manifest->assignments[0].requirements,
                        "Starfield.esm") &&
              explicitPerPreset.manifest->assignments[0].requirements.assets.size() == 2,
          "explicit per-assignment requirements are additive");

    const auto assetManifest = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"packageId":"author.assets","priority":0,"requires":{"plugins":[],"assets":["Textures/Author/Required.dds"]},"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"00005983"},"preset":"Presets/Sarah.npc","scope":"faceAndBody"}]})",
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
        "fixtures/osf-identity/Packages/author.sarah-example/package.json", false);
    Check(checkedInExample.manifest && checkedInExample.manifest->packageID == "author.sarah-example",
          "checked-in example matches runtime schema");

    const auto missing = NA::ParsePackageManifest(
        Manifest("author.missing", 0, "Starfield.esm", "00005983", "Presets/Missing.npc"),
        root / "author.missing" / "package.json", true);
    Check(missing.HasFatalError() && !missing.issues.empty(), "missing preset rejects package");

    const auto traversal = NA::ParsePackageManifest(
        Manifest("author.traversal", 0, "Starfield.esm", "00005983", "../Sarah.npc"),
        root / "author.traversal" / "package.json", false);
    Check(traversal.HasFatalError(), "preset parent traversal rejected");

    const auto absolute = NA::ParsePackageManifest(
        Manifest("author.absolute", 0, "Starfield.esm", "00005983", "C:\\\\Sarah.npc"),
        root / "author.absolute" / "package.json", false);
    Check(absolute.HasFatalError(), "absolute preset path rejected");

    const auto wrongExtension = NA::ParsePackageManifest(
        Manifest("author.extension", 0, "Starfield.esm", "00005983", "Sarah.json"),
        root / "author.extension" / "package.json", false);
    Check(wrongExtension.HasFatalError(), "non-npc preset rejected");

    const auto wrongScope = NA::ParsePackageManifest(
        Manifest("author.scope", 0, "Starfield.esm", "00005983", "Sarah.npc", "faceOnly"),
        root / "author.scope" / "package.json", false);
    Check(wrongScope.HasFatalError(), "unproven scope rejected");

    const auto badPlugin = NA::ParsePackageManifest(
        Manifest("author.plugin", 0, "not-a-plugin", "00005983"),
        root / "author.plugin" / "package.json", false);
    Check(badPlugin.HasFatalError(), "invalid target plugin rejected");

    const auto badFormID = NA::ParsePackageManifest(
        Manifest("author.form", 0, "Starfield.esm", "01000000"),
        root / "author.form" / "package.json", false);
    Check(badFormID.HasFatalError(), "out-of-range local FormID rejected");

    const auto badPriority = NA::ParsePackageManifest(
        Manifest("author.priority", NA::kMaxPriority + 1),
        root / "author.priority" / "package.json", false);
    Check(badPriority.HasFatalError(), "out-of-range priority rejected");

    const auto unknownVersion = NA::ParsePackageManifest(
        R"({"schemaVersion":2,"packageId":"author.version","priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[]})",
        root / "version" / "package.json", false);
    Check(unknownVersion.HasFatalError(), "unknown schema rejected");

    const auto unknownProperty = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"packageId":"author.unknown","priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[],"surprise":true})",
        root / "unknown" / "package.json", false);
    Check(unknownProperty.HasFatalError(), "unknown root property rejected");

    const auto missingFormat = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"packageId":"author.no-format","priority":0,"requires":{"plugins":[],"assets":[]}})",
        root / "no-format" / "package.json", false);
    Check(missingFormat.HasFatalError(), "package requires exactly one authoring format");

    const auto mixedFormats = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"packageId":"author.mixed","priority":0,"requires":{"plugins":[],"assets":[]},"presetConvention":"pluginFolderLocalFormId","assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"00005983"},"preset":"Sarah.npc","scope":"faceAndBody"}]})",
        root / "mixed-format" / "package.json", false);
    Check(mixedFormats.HasFatalError(), "explicit and convention formats cannot be mixed");

    const auto unknownConvention = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"packageId":"author.convention","priority":0,"requires":{"plugins":[],"assets":[]},"presetConvention":"editorIdFilename"})",
        root / "unknown-convention" / "package.json", false);
    Check(unknownConvention.HasFatalError(), "unknown preset convention rejected");

    const auto malformed = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"packageId":)", root / "malformed" / "package.json", false);
    Check(malformed.HasFatalError(), "truncated JSON rejected");

    std::string oversized(NA::kMaxManifestBytes + 1, ' ');
    const auto invalidSize = NA::ParsePackageManifest(
        oversized, root / "oversized" / "package.json", false);
    Check(invalidSize.HasFatalError(), "manifest byte bound enforced");

    const std::string duplicateTargetJson =
        R"({"schemaVersion":1,"packageId":"author.duplicate-target","priority":0,"requires":{"plugins":[],"assets":[]},"assignments":[{"target":{"plugin":"Starfield.esm","localFormId":"00005983"},"preset":"a.npc","scope":"faceAndBody"},{"target":{"plugin":"starfield.esm","localFormId":"00005983"},"preset":"b.npc","scope":"faceAndBody"}]})";
    const auto duplicateTarget = NA::ParsePackageManifest(
        duplicateTargetJson, root / "duplicate-target" / "package.json", false);
    Check(duplicateTarget.HasFatalError(), "duplicate target rejects package");

    const auto conventionRoot = root / "convention";
    Write(conventionRoot / "package.json",
          R"({"schemaVersion":1,"packageId":"project.community","priority":100,"requires":{"plugins":["SharedAssets.esm"],"assets":["Textures/Shared.dds"]},"presetConvention":"pluginFolderLocalFormId"})");
    Write(conventionRoot / "Presets" / "Starfield.esm" / "0029A8EB.npc", "fixture");
    Write(conventionRoot / "Presets" / "Starfield.esm" / "0029A8EB.identity.json",
          R"({"schemaVersion":1,"requires":{"plugins":["ExampleHairMod.esm"],"assets":["Meshes/Hair/Example.mesh"]}})");
    Write(conventionRoot / "Presets" / "Starfield.esm" / "00005983.npc", "fixture");
    const auto convention = NA::LoadPackageManifest(conventionRoot / "package.json", true);
    const auto daniel = convention.manifest ?
        std::ranges::find_if(convention.manifest->assignments, [](const auto& a_assignment) {
            return a_assignment.target.localFormID == 0x0029A8EB;
        }) : std::vector<NA::Assignment>::const_iterator{};
    Check(convention.manifest &&
              convention.manifest->format == NA::PackageFormat::kPluginFolderLocalFormID &&
              convention.manifest->assignments.size() == 2,
          "plugin-folder convention discovers direct FormID presets");
    Check(convention.manifest && daniel != convention.manifest->assignments.end() &&
              daniel->target.plugin == "Starfield.esm" &&
              HasPlugin(daniel->requirements, "SharedAssets.esm") &&
              HasPlugin(daniel->requirements, "ExampleHairMod.esm") &&
              HasPlugin(daniel->requirements, "Starfield.esm") &&
              daniel->requirements.assets.size() == 2,
          "convention sidecar requirements are additive and target-qualified");

    const auto isolatedRoot = root / "isolated-invalid";
    Write(isolatedRoot / "package.json",
          R"({"schemaVersion":1,"packageId":"project.isolated","priority":0,"requires":{"plugins":[],"assets":[]},"presetConvention":"pluginFolderLocalFormId"})");
    Write(isolatedRoot / "Presets" / "Starfield.esm" / "00000001.npc", "fixture");
    Write(isolatedRoot / "Presets" / "Starfield.esm" / "00000001.identity.json", "{");
    Write(isolatedRoot / "Presets" / "Starfield.esm" / "00000002.npc", "fixture");
    Write(isolatedRoot / "Presets" / "Starfield.esm" / "EditorName.npc", "fixture");
    Write(isolatedRoot / "Presets" / "Starfield.esm" / "00000003.identity.json",
          R"({"schemaVersion":1,"requires":{"plugins":[],"assets":[]}})");
    Write(isolatedRoot / "Presets" / "Starfield.esm" / "Nested" / "00000004.npc", "fixture");
    const auto isolated = NA::LoadPackageManifest(isolatedRoot / "package.json", true);
    Check(isolated.manifest && isolated.manifest->assignments.size() == 1 &&
              isolated.manifest->assignments[0].target.localFormID == 2,
          "malformed sidecar and invalid filename disable only affected presets");
    Check(HasIssue(isolated.issues, "invalid_preset_metadata_json") &&
              HasIssue(isolated.issues, "invalid_convention_form_id") &&
              HasIssue(isolated.issues, "orphan_preset_metadata") &&
              HasIssue(isolated.issues, "invalid_convention_layout"),
          "convention diagnostics cover malformed, orphaned, and nested entries");

    const auto missingPresetRoot = NA::ParsePackageManifest(
        R"({"schemaVersion":1,"packageId":"project.empty","priority":0,"requires":{"plugins":[],"assets":[]},"presetConvention":"pluginFolderLocalFormId"})",
        root / "missing-preset-root" / "package.json", true);
    Check(missingPresetRoot.manifest && missingPresetRoot.manifest->assignments.empty() &&
              HasIssue(missingPresetRoot.issues, "preset_root_missing"),
          "missing convention preset root leaves package non-mutating");

    const auto limitRoot = root / "convention-limit";
    Write(limitRoot / "package.json",
          R"({"schemaVersion":1,"packageId":"project.limit","priority":0,"requires":{"plugins":[],"assets":[]},"presetConvention":"pluginFolderLocalFormId"})");
    for (std::uint32_t i = 0; i <= NA::kMaxAssignments; ++i) {
        Write(limitRoot / "Presets" / "Starfield.esm" /
                  std::format("{:08X}.npc", i + 1),
              "fixture");
    }
    const auto overLimit = NA::LoadPackageManifest(limitRoot / "package.json", true);
    Check(overLimit.manifest && overLimit.manifest->assignments.empty() &&
              HasIssue(overLimit.issues, "assignment_limit_exceeded"),
          "convention assignment safety limit fails closed");

    const auto checkedConventionExample = NA::LoadPackageManifest(
        "fixtures/osf-identity/Packages/project.community-example/package.json", false);
    Check(checkedConventionExample.manifest &&
              checkedConventionExample.manifest->format ==
                  NA::PackageFormat::kPluginFolderLocalFormID,
          "checked-in convention example matches runtime schema");

    const auto low = NA::ParsePackageManifest(
        Manifest("author.low", 10), root / "low" / "package.json", false);
    const auto highZ = NA::ParsePackageManifest(
        Manifest("author.z-high", 20), root / "high-z" / "package.json", false);
    const auto highA = NA::ParsePackageManifest(
        Manifest("author.a-high", 20), root / "high-a" / "package.json", false);
    std::vector<NA::PackageManifest> packages{ *low.manifest, *highZ.manifest, *highA.manifest };
    const auto selection = NA::SelectAssignments(packages);
    Check(selection.winners.size() == 1 && selection.winners[0].packageID == "author.a-high",
          "highest priority then ascending packageId wins");
    Check(selection.decisions.size() == 3, "all conflict decisions reported");
    auto invalidHigh = *highA.manifest;
    invalidHigh.assignments.clear();
    const auto promoted = NA::SelectAssignments({ invalidHigh, *low.manifest });
    Check(promoted.winners.size() == 1 &&
              promoted.winners[0].packageID == "author.low",
          "removing an invalid high-priority candidate promotes a valid lower candidate");

    const auto discoveryRoot = root / "discovery";
    Write(discoveryRoot / "duplicate-a" / "package.json", Manifest("author.duplicate", 1));
    Write(discoveryRoot / "duplicate-a" / "Presets" / "Sarah.npc", "fixture");
    Write(discoveryRoot / "duplicate-b" / "package.json", Manifest("author.duplicate", 2));
    Write(discoveryRoot / "duplicate-b" / "Presets" / "Sarah.npc", "fixture");
    Write(discoveryRoot / "unique" / "package.json", Manifest("author.unique", 3));
    Write(discoveryRoot / "unique" / "Presets" / "Sarah.npc", "fixture");
    const auto discovery = NA::DiscoverPackages(discoveryRoot, true);
    Check(discovery.packages.size() == 1 && discovery.packages[0].packageID == "author.unique",
          "all duplicate packageIds rejected independently");
    Check(discovery.issues.size() == 2, "duplicate package diagnostics emitted");

    std::cout << "RESULT failed=" << g_failed << '\n';
    return g_failed == 0 ? 0 : 1;
}
