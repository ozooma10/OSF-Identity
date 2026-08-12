#include "Config/Preset.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>

namespace
{
    namespace NA = Config;

    std::size_t g_failed = 0;

    void Check(const bool a_condition, const char* a_name)
    {
        std::cout << (a_condition ? "PASS " : "FAIL ") << a_name << '\n';
        if (!a_condition) {
            ++g_failed;
        }
    }

    [[nodiscard]] std::string Read(const std::filesystem::path& a_path)
    {
        std::ifstream stream{ a_path, std::ios::binary };
        return { std::istreambuf_iterator<char>{ stream }, std::istreambuf_iterator<char>{} };
    }

    void Write(const std::filesystem::path& a_path, const std::string_view a_text)
    {
        std::filesystem::create_directories(a_path.parent_path());
        std::ofstream stream{ a_path, std::ios::binary };
        stream.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
    }

    [[nodiscard]] bool ReplaceOnce(std::string& a_text, const std::string_view a_from,
                                   const std::string_view a_to)
    {
        const auto pos = a_text.find(a_from);
        if (pos == std::string::npos) {
            return false;
        }
        a_text.replace(pos, a_from.size(), a_to);
        return true;
    }

    [[nodiscard]] NA::AppearancePreset* RequirePreset(NA::PresetResult& a_result, const char* a_name)
    {
        Check(a_result.preset.has_value() && a_result.issues.empty(), a_name);
        return a_result.preset ? std::addressof(*a_result.preset) : nullptr;
    }

    [[nodiscard]] NA::PresetNamedMorph* FindMorph(NA::AppearancePreset& a_preset,
                                                   const std::string_view a_name)
    {
        const auto it = std::ranges::find(a_preset.facialMorphSliders, a_name,
                                          &NA::PresetNamedMorph::name);
        return it == a_preset.facialMorphSliders.end() ? nullptr : std::addressof(*it);
    }

    [[nodiscard]] const NA::PresetTintLayer* FindTint(const NA::AppearancePreset& a_preset,
                                                       const std::string_view a_name)
    {
        const auto it = std::ranges::find(a_preset.postBlendLayers, a_name,
                                          &NA::PresetTintLayer::name);
        return it == a_preset.postBlendLayers.end() ? nullptr : std::addressof(*it);
    }

    [[nodiscard]] std::uint32_t PackedTintIntensity(const NA::PresetTintLayer& a_layer)
    {
        return a_layer.packedIntensity;
    }

    [[nodiscard]] bool Rejects(const std::string& a_json)
    {
        const auto result = NA::ParseCkPreset(a_json, "adversarial.npc");
        return result.HasFatalError() && !result.issues.empty();
    }

    [[nodiscard]] bool Accepts(const std::string& a_json)
    {
        const auto result = NA::ParseCkPreset(a_json, "permissive.npc");
        return result.preset.has_value() && result.issues.empty();
    }
}

int main()
{
    namespace NA = Config;
    const auto fixtures = std::filesystem::path{ "fixtures/osf-identity/Presets/CK" };
    auto baselineResult = NA::LoadCkPreset(fixtures / "Baseline.npc");
    auto headpartResult = NA::LoadCkPreset(fixtures / "HeadpartOnly.npc");
    auto facialResult = NA::LoadCkPreset(fixtures / "FacialMorphOnly.npc");
    auto tintResult = NA::LoadCkPreset(fixtures / "TintOnly.npc");
    auto bodyResult = NA::LoadCkPreset(fixtures / "BodyMorphOnly.npc");
    auto compositeResult = NA::LoadCkPreset(fixtures / "Sarah.npc");

    const auto charGenFixtures =
        std::filesystem::path{ "fixtures/osf-identity/Presets/CharGenMenu" };
    auto charGenBaselineResult = NA::LoadCkPreset(charGenFixtures / "Baseline.npc");
    auto charGenHeadpartResult = NA::LoadCkPreset(charGenFixtures / "HeadpartOnly.npc");
    auto charGenFacialResult = NA::LoadCkPreset(charGenFixtures / "FacialMorphOnly.npc");
    auto charGenTintResult = NA::LoadCkPreset(charGenFixtures / "TintOnly.npc");
    auto charGenBodyResult = NA::LoadCkPreset(charGenFixtures / "BodyMorphOnly.npc");
    auto charGenCompositeResult = NA::LoadCkPreset(charGenFixtures / "Sarah.npc");

    auto* baseline = RequirePreset(baselineResult, "CK Baseline.npc decodes");
    auto* headpart = RequirePreset(headpartResult, "CK HeadpartOnly.npc decodes");
    auto* facial = RequirePreset(facialResult, "CK FacialMorphOnly.npc decodes");
    auto* tint = RequirePreset(tintResult, "CK TintOnly.npc decodes");
    auto* body = RequirePreset(bodyResult, "CK BodyMorphOnly.npc decodes");
    auto* composite = RequirePreset(compositeResult, "CK Sarah.npc decodes");
    auto* charGenBaseline = RequirePreset(charGenBaselineResult, "CharGenMenu Baseline.npc decodes");
    auto* charGenHeadpart = RequirePreset(charGenHeadpartResult, "CharGenMenu HeadpartOnly.npc decodes");
    auto* charGenFacial = RequirePreset(charGenFacialResult, "CharGenMenu FacialMorphOnly.npc decodes");
    auto* charGenTint = RequirePreset(charGenTintResult, "CharGenMenu TintOnly.npc decodes");
    auto* charGenBody = RequirePreset(charGenBodyResult, "CharGenMenu BodyMorphOnly.npc decodes");
    auto* charGenComposite = RequirePreset(charGenCompositeResult, "CharGenMenu Sarah.npc decodes");
    if (!baseline || !headpart || !facial || !tint || !body || !composite ||
        !charGenBaseline || !charGenHeadpart || !charGenFacial || !charGenTint ||
        !charGenBody || !charGenComposite) {
        std::cout << "RESULT failed=" << ++g_failed << '\n';
        return 1;
    }

    Check(baseline->npcFormEditorID == "Companion_SarahMorgan" &&
              baseline->raceFormID == "HumanRace" && baseline->sex == NA::PresetSex::kFemale &&
              baseline->skinTone == 2,
          "CK 1.16.244 identity fields decode");
    Check(baseline->bodyMorphRegionValues.size() == 5 &&
              baseline->facialMorphSliders.size() == 37 &&
              baseline->facialBoneRegions.size() == 20 &&
              baseline->postBlendLayers.size() == 9 &&
              baseline->uniqueHeadParts.size() == 14,
          "CK golden structural counts decode");

    auto expected = *baseline;
    const auto oldHair = std::ranges::find(expected.uniqueHeadParts, "Human_Female_Hair_Choppy_Bob");
    Check(oldHair != expected.uniqueHeadParts.end(), "baseline hair headpart found");
    if (oldHair != expected.uniqueHeadParts.end()) {
        *oldHair = "Human_Female_Hair_Faded_Afro";
    }
    Check(expected == *headpart, "HeadpartOnly changes exactly the decoded hair headpart");

    expected = *baseline;
    auto* expectedEyeMorph = FindMorph(expected, "female_eu_md2_Eyes");
    Check(expectedEyeMorph != nullptr, "baseline controlled facial morph found");
    if (expectedEyeMorph) {
        expectedEyeMorph->value = 0.31000000238418579;
    }
    Check(expected == *facial, "FacialMorphOnly changes exactly the decoded eye morph");

    expected = *baseline;
    expected.eyeColor = "Blue";
    Check(expected == *tint, "TintOnly changes exactly decoded EyeColor");

    expected = *baseline;
    expected.bodyMorphRegionValues[1] = 0.31000000238418579;
    Check(expected == *body, "BodyMorphOnly changes exactly decoded body region 1");

    expected = *baseline;
    expected.eyeColor = "Blue";
    expected.bodyMorphRegionValues[1] = 0.31000000238418579;
    auto expectedCompositeHair = std::ranges::find(expected.uniqueHeadParts,
                                                    "Human_Female_Hair_Choppy_Bob");
    if (expectedCompositeHair != expected.uniqueHeadParts.end()) {
        *expectedCompositeHair = "Human_Female_Hair_Faded_Afro";
    }
    auto* expectedCompositeMorph = FindMorph(expected, "female_eu_md2_Eyes");
    if (expectedCompositeMorph) {
        expectedCompositeMorph->value = 0.31000000238418579;
    }
    Check(expected == *composite, "Sarah composite is exactly the four decoded controlled edits");

    Check(charGenBaseline->npcFormEditorID.empty() &&
              charGenBaseline->raceFormID == "HumanRace" &&
              charGenBaseline->sex == NA::PresetSex::kFemale && charGenBaseline->skinTone == 2,
          "CharGenMenu producer contract and identity fields decode");
    Check(charGenBaseline->bodyMorphRegionValues.size() == 5 &&
              charGenBaseline->facialMorphSliders.size() == 37 &&
              charGenBaseline->facialBoneRegions.size() == 20 &&
              charGenBaseline->postBlendLayers.size() == 9 &&
              charGenBaseline->uniqueHeadParts.size() == 14,
          "CharGenMenu golden structural counts decode");

    expected = *charGenBaseline;
    const auto charGenOldHair =
        std::ranges::find(expected.uniqueHeadParts, "Human_Female_Hair_Choppy_Bob");
    Check(charGenOldHair != expected.uniqueHeadParts.end(), "CharGenMenu baseline hair headpart found");
    if (charGenOldHair != expected.uniqueHeadParts.end()) {
        *charGenOldHair = "Human_Female_Hair_Faded_Afro";
    }
    Check(expected == *charGenHeadpart,
          "CharGenMenu HeadpartOnly changes exactly the decoded hair headpart");

    expected = *charGenBaseline;
    auto* charGenExpectedEyeMorph = FindMorph(expected, "female_eu_md2_Eyes");
    Check(charGenExpectedEyeMorph != nullptr, "CharGenMenu baseline controlled facial morph found");
    if (charGenExpectedEyeMorph) {
        charGenExpectedEyeMorph->value = 0.31000000238418579;
    }
    Check(expected == *charGenFacial,
          "CharGenMenu FacialMorphOnly changes exactly the decoded eye morph");

    expected = *charGenBaseline;
    expected.eyeColor = "Blue";
    Check(expected == *charGenTint, "CharGenMenu TintOnly changes exactly decoded EyeColor");

    expected = *charGenBaseline;
    expected.bodyMorphRegionValues[1] = 0.31000000238418579;
    Check(expected == *charGenBody,
          "CharGenMenu BodyMorphOnly changes exactly decoded body region 1");

    expected = *charGenBaseline;
    expected.eyeColor = "Blue";
    expected.bodyMorphRegionValues[1] = 0.31000000238418579;
    const auto charGenCompositeHair =
        std::ranges::find(expected.uniqueHeadParts, "Human_Female_Hair_Choppy_Bob");
    if (charGenCompositeHair != expected.uniqueHeadParts.end()) {
        *charGenCompositeHair = "Human_Female_Hair_Faded_Afro";
    }
    auto* charGenCompositeMorph = FindMorph(expected, "female_eu_md2_Eyes");
    if (charGenCompositeMorph) {
        charGenCompositeMorph->value = 0.31000000238418579;
    }
    Check(expected == *charGenComposite,
          "CharGenMenu Sarah composite is exactly the four decoded controlled edits");

    bool tintPackingMatches = baseline->postBlendLayers.size() == charGenBaseline->postBlendLayers.size();
    for (const auto& ckLayer : baseline->postBlendLayers) {
        const auto* charGenLayer = FindTint(*charGenBaseline, ckLayer.name);
        tintPackingMatches = tintPackingMatches && charGenLayer &&
                             PackedTintIntensity(ckLayer) == PackedTintIntensity(*charGenLayer);
    }
    Check(tintPackingMatches,
          "CharGenMenu 1/128 tint encoding normalizes to the CK runtime packed intensities");

    const auto baselineJson = Read(fixtures / "Baseline.npc");
    const auto charGenBaselineJson = Read(charGenFixtures / "Baseline.npc");
    Check(!baselineJson.empty(), "baseline JSON read for adversarial corpus");
    Check(!charGenBaselineJson.empty(), "CharGenMenu baseline JSON read for adversarial corpus");

    Check(Rejects(baselineJson.substr(0, baselineJson.size() / 2)), "truncated JSON rejected");
    Check(Rejects(baselineJson + " trailing"), "trailing data rejected");

    // A repeated key is not rejected: Glaze takes the last occurrence, matching
    // every mainstream JSON reader. The pack author sees the value they wrote
    // last rather than an error.
    auto modified = baselineJson;
    Check(ReplaceOnce(modified, "{", R"({"Surprise":1,)") && Rejects(modified),
          "unknown root property rejected");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("EyeColor" : "Green",)", "") && Rejects(modified),
          "missing required property rejected");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("SkinTone" : 2)", R"("SkinTone" : "2")") && Rejects(modified),
          "wrong primitive type rejected");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("SkinTone" : 2)", R"("SkinTone" : 4281943808)") && Rejects(modified),
          "CK SkinTone rejects upper-byte data");

    modified = charGenBaselineJson;
    auto paddedSkinToneReplaced = ReplaceOnce(modified, R"("SkinTone": 2)", R"("SkinTone": 4281943808)");
    auto paddedSkinToneResult = NA::ParseCkPreset(modified, "char-gen-padded-skin-tone.npc");
    Check(paddedSkinToneReplaced && paddedSkinToneResult.preset && paddedSkinToneResult.issues.empty() &&
              paddedSkinToneResult.preset->skinTone == 0,
          "CharGenMenu SkinTone ignores producer padding and preserves the low byte");

    modified = charGenBaselineJson;
    auto maxSkinToneReplaced = ReplaceOnce(modified, R"("SkinTone": 2)", R"("SkinTone": 4294967295)");
    auto maxSkinToneResult = NA::ParseCkPreset(modified, "char-gen-max-skin-tone.npc");
    Check(maxSkinToneReplaced && maxSkinToneResult.preset && maxSkinToneResult.issues.empty() &&
              maxSkinToneResult.preset->skinTone == 255,
          "CharGenMenu SkinTone accepts the full producer uint32 range");

    modified = charGenBaselineJson;
    Check(ReplaceOnce(modified, R"("SkinTone": 2)", R"("SkinTone": 4294967296)") && Rejects(modified),
          "CharGenMenu SkinTone uint32 overflow rejected");

    modified = charGenBaselineJson;
    Check(ReplaceOnce(modified, R"("SkinTone": 2)", R"("SkinTone": -1)") && Rejects(modified),
          "negative CharGenMenu SkinTone rejected");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("Sex" : "Female")", R"("Sex" : "Unknown")") && Rejects(modified),
          "unsupported Sex value rejected");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("z" : 0)", R"("z" : 0, "w" : 0)") && Rejects(modified),
          "unknown nested property rejected");

    modified = baselineJson;
    Check(ReplaceOnce(modified, "[ 0, 0.20999999344348907, 0, 0, 0 ]",
                      "[ 0, 0.20999999344348907, 0, 0 ]") && Rejects(modified),
          "wrong body region count rejected");

    modified = baselineJson;
    auto emptyBodyReplaced = ReplaceOnce(modified, "[ 0, 0.20999999344348907, 0, 0, 0 ]", "[]");
    auto emptyBodyResult = NA::ParseCkPreset(modified, "empty-body-regions.npc");
    Check(emptyBodyReplaced && emptyBodyResult.preset && emptyBodyResult.issues.empty() &&
              emptyBodyResult.preset->bodyMorphRegionValues.empty(),
          "producer-empty body morph region array preserves target body data");

    modified = baselineJson;
    Check(ReplaceOnce(modified, "[ 0, 0.20999999344348907, 0, 0, 0 ]",
                      "[ 0, 1.25, 0, 0, 0 ]") && Rejects(modified),
          "out-of-range body value rejected");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("x" : 0.20999999344348907)", R"("x" : -0.1)") && Rejects(modified),
          "out-of-range morph weight rejected");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("Value" : -0.86000001430511475)", R"("Value" : -1.1)") &&
              Rejects(modified),
          "out-of-range bone slider rejected");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("Name" : "female_af_md1_Forehead")",
                      R"("Name" : "female_af_md1_Ears")") && Accepts(modified),
          "duplicate facial morph name accepted");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("RegionID" : 3)", R"("RegionID" : 1)") && Accepts(modified),
          "duplicate facial bone region accepted");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("GroupName" : "Eyes")", R"("GroupName" : "Chin")") &&
              Accepts(modified),
          "duplicate facial bone slider group and ID accepted");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("Name" : "Cheeks2")", R"("Name" : "Cheeks1")") && Accepts(modified),
          "duplicate tint layer name accepted");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("RegionID" : 1)", R"("RegionID" : 4294967295)") && Accepts(modified),
          "full 32-bit facial bone region ID range accepted");

    modified = baselineJson;
    auto signedRegionReplaced = ReplaceOnce(modified, R"("RegionID" : 1)", R"("RegionID" : -1)");
    auto signedRegionResult = NA::ParseCkPreset(modified, "signed-region-id.npc");
    Check(signedRegionReplaced && signedRegionResult.preset && signedRegionResult.issues.empty() &&
              !signedRegionResult.preset->facialBoneRegions.empty() &&
              signedRegionResult.preset->facialBoneRegions.front().regionID == 0xFFFFFFFFu,
          "producer-signed facial bone region ID preserves its 32-bit bit pattern");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("RegionID" : 1)", R"("RegionID" : -2147483649)") && Rejects(modified),
          "facial bone region ID signed underflow rejected before conversion");

    modified = baselineJson;
    auto signedSliderReplaced = ReplaceOnce(modified, R"("ID" : 0)", R"("ID" : -1)");
    auto signedSliderResult = NA::ParseCkPreset(modified, "signed-slider-id.npc");
    Check(signedSliderReplaced && signedSliderResult.preset && signedSliderResult.issues.empty() &&
              !signedSliderResult.preset->facialBoneRegions.empty() &&
              !signedSliderResult.preset->facialBoneRegions.front().sliders.empty() &&
              signedSliderResult.preset->facialBoneRegions.front().sliders.front().id == 0xFFFFFFFFu,
          "producer-signed facial bone slider ID preserves its 32-bit bit pattern");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("RegionID" : 1)", R"("RegionID" : 4294967296)") && Rejects(modified),
          "facial bone region ID overflow rejected before conversion");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("Intensity" : 1,)", R"("Intensity" : 1.01,)") && Rejects(modified),
          "out-of-range tint intensity rejected");

    modified = charGenBaselineJson;
    Check(ReplaceOnce(modified, R"("Intensity": 0.2890625,)", R"("Intensity": 0.29,)") &&
              Rejects(modified),
          "non-quantized CharGenMenu tint intensity rejected");

    modified = charGenBaselineJson;
    auto extendedTintReplaced = ReplaceOnce(modified, R"("Intensity": 0.2890625,)", R"("Intensity": 0.5078125,)");
    auto extendedTintResult = NA::ParseCkPreset(modified, "char-gen-extended-tint.npc");
    const auto* extendedTint = extendedTintResult.preset ? FindTint(*extendedTintResult.preset, "Dermaesthetic") : nullptr;
    Check(extendedTintReplaced && extendedTintResult.preset && extendedTintResult.issues.empty() &&
              extendedTint && extendedTint->packedIntensity == 65,
          "CharGenMenu tint intensity preserves packed values above 64");

    modified = charGenBaselineJson;
    auto maxTintReplaced = ReplaceOnce(modified, R"("Intensity": 0.2890625,)", R"("Intensity": 1,)");
    auto maxTintResult = NA::ParseCkPreset(modified, "char-gen-max-tint.npc");
    const auto* maxTint = maxTintResult.preset ? FindTint(*maxTintResult.preset, "Dermaesthetic") : nullptr;
    Check(maxTintReplaced && maxTintResult.preset && maxTintResult.issues.empty() &&
              maxTint && maxTint->packedIntensity == 128,
          "CharGenMenu tint intensity accepts the producer packed maximum");

    modified = charGenBaselineJson;
    Check(ReplaceOnce(modified, R"("Intensity": 0.2890625,)", R"("Intensity": 1.0078125,)") &&
              Rejects(modified),
          "out-of-range CharGenMenu tint intensity rejected");

    auto customColorJson = charGenBaselineJson;
    auto customColorInserted = ReplaceOnce(
        customColorJson,
        R"("ModulationValue": {)",
        R"("ModulationValue": {"CustomColorValue":{"Blue":87,"Green":132,"Red":119,"Rough":229},)");
    auto customColorResult = NA::ParseCkPreset(customColorJson, "char-gen-custom-color.npc");
    const auto* customTint = customColorResult.preset ? FindTint(*customColorResult.preset, "Dermaesthetic") : nullptr;
    Check(customColorInserted && customColorResult.preset && customColorResult.issues.empty() &&
              customTint && customTint->customColor &&
              customTint->customColor->red == 119 && customTint->customColor->green == 132 &&
              customTint->customColor->blue == 87 && customTint->customColor->rough == 229,
          "CharGenMenu custom modulation color decodes with producer channel ordering");

    modified = customColorJson;
    Check(ReplaceOnce(modified, R"("Blue":87)", R"("Blue":256)") && Rejects(modified),
          "custom modulation color channel overflow rejected");

    modified = customColorJson;
    Check(ReplaceOnce(modified, R"("Rough":229)", R"("Rough":-1)") && Rejects(modified),
          "negative custom modulation roughness rejected");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("NPCFormEditorID" : "Companion_SarahMorgan")",
                      R"("NPCFormEditorID" : "")") && Rejects(modified),
          "empty editor ID requires the complete CharGenMenu intensity contract");

    modified = baselineJson;
    Check(ReplaceOnce(modified, "[ 0, 0.20999999344348907, 0, 0, 0 ]",
                      "[ 01, 0.20999999344348907, 0, 0, 0 ]") && Rejects(modified),
          "invalid numeric leading zero rejected");

    modified = baselineJson;
    Check(ReplaceOnce(modified, "[ 0, 0.20999999344348907, 0, 0, 0 ]",
                      "[ 1e999, 0.20999999344348907, 0, 0, 0 ]") && Rejects(modified),
          "non-finite numeric result rejected");

    modified = baselineJson;
    Check(ReplaceOnce(modified, R"("EyeColor" : "Green")",
                      std::string{ R"("EyeColor" : ")" } + std::string(257, 'A') + R"(")") &&
              Accepts(modified),
          "long semantic string accepted");

    modified = baselineJson;
    Check(ReplaceOnce(modified, "[ 0, 0.20999999344348907, 0, 0, 0 ]",
                      "[ 1e-1, 0.20999999344348907, 0, 0, 0 ]") &&
              !NA::ParseCkPreset(modified, "exponent.npc").HasFatalError(),
          "finite exponent JSON number accepted");

    std::string oversized(NA::kMaxPresetBytes + 1, ' ');
    Check(Rejects(oversized), "preset byte bound enforced before parse");

    const auto tempRoot = std::filesystem::absolute("tmp/npc-appearance-preset-tests");
    std::filesystem::remove_all(tempRoot);
    Write(tempRoot / "wrong.json", baselineJson);
    Write(tempRoot / "empty.npc", "");
    Check(NA::LoadCkPreset(tempRoot / "wrong.json").HasFatalError(), "non-.npc file rejected");
    Check(NA::LoadCkPreset(tempRoot / "empty.npc").HasFatalError(), "empty .npc file rejected");
    std::filesystem::remove_all(tempRoot);

    std::cout << "RESULT failed=" << g_failed << '\n';
    return g_failed == 0 ? 0 : 1;
}
