#include "Probe/NpcAppearanceConfig.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
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
        "fixtures/npc-appearance-loader/Packages/author.sarah-example/package.json", false);
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

    Write(root / "duplicate-a" / "package.json", Manifest("author.duplicate", 1));
    Write(root / "duplicate-a" / "Presets" / "Sarah.npc", "fixture");
    Write(root / "duplicate-b" / "package.json", Manifest("author.duplicate", 2));
    Write(root / "duplicate-b" / "Presets" / "Sarah.npc", "fixture");
    Write(root / "unique" / "package.json", Manifest("author.unique", 3));
    Write(root / "unique" / "Presets" / "Sarah.npc", "fixture");
    const auto discovery = NA::DiscoverPackages(root, true);
    Check(discovery.packages.size() == 1 && discovery.packages[0].packageID == "author.unique",
          "all duplicate packageIds rejected independently");
    Check(discovery.issues.size() == 2, "duplicate package diagnostics emitted");

    std::cout << "RESULT failed=" << g_failed << '\n';
    return g_failed == 0 ? 0 : 1;
}
