#include "NpcAppearance/Preset.h"

#include "NpcAppearance/Config.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <ranges>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace NpcAppearance
{
    namespace
    {
        constexpr std::size_t kMaxJsonDepth = 32;
        constexpr std::size_t kMaxJsonProperties = 128;
        constexpr std::size_t kMaxJsonArrayElements = 4096;
        constexpr std::size_t kMaxJsonStringBytes = 4096;
        constexpr std::size_t kMaxSemanticStringBytes = 256;
        constexpr std::size_t kBodyMorphRegionCount = 5;
        constexpr std::size_t kMaxHeadParts = 128;
        constexpr std::size_t kMaxFacialMorphs = 512;
        constexpr std::size_t kMaxBoneRegions = 256;
        constexpr std::size_t kMaxBoneSlidersPerRegion = 256;
        constexpr std::size_t kMaxTintLayers = 256;
        constexpr double kCharGenMenuTintScale = 128.0;
        constexpr double kRuntimeTintScale = 64.0;
        constexpr double kTintQuantizationTolerance = 1.0e-6;

        void AppendUTF8(std::string& a_out, const std::uint32_t a_cp)
        {
            if (a_cp <= 0x7F) {
                a_out.push_back(static_cast<char>(a_cp));
            } else if (a_cp <= 0x7FF) {
                a_out.push_back(static_cast<char>(0xC0 | (a_cp >> 6)));
                a_out.push_back(static_cast<char>(0x80 | (a_cp & 0x3F)));
            } else if (a_cp <= 0xFFFF) {
                a_out.push_back(static_cast<char>(0xE0 | (a_cp >> 12)));
                a_out.push_back(static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F)));
                a_out.push_back(static_cast<char>(0x80 | (a_cp & 0x3F)));
            } else {
                a_out.push_back(static_cast<char>(0xF0 | (a_cp >> 18)));
                a_out.push_back(static_cast<char>(0x80 | ((a_cp >> 12) & 0x3F)));
                a_out.push_back(static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F)));
                a_out.push_back(static_cast<char>(0x80 | (a_cp & 0x3F)));
            }
        }

        [[nodiscard]] bool IsValidUTF8(const std::string_view a_text) noexcept
        {
            for (std::size_t i = 0; i < a_text.size();) {
                const auto first = static_cast<unsigned char>(a_text[i]);
                if (first <= 0x7F) {
                    ++i;
                    continue;
                }
                std::size_t count = 0;
                std::uint32_t cp = 0;
                if (first >= 0xC2 && first <= 0xDF) {
                    count = 2;
                    cp = first & 0x1F;
                } else if (first >= 0xE0 && first <= 0xEF) {
                    count = 3;
                    cp = first & 0x0F;
                } else if (first >= 0xF0 && first <= 0xF4) {
                    count = 4;
                    cp = first & 0x07;
                } else {
                    return false;
                }
                if (a_text.size() - i < count) {
                    return false;
                }
                for (std::size_t j = 1; j < count; ++j) {
                    const auto next = static_cast<unsigned char>(a_text[i + j]);
                    if ((next & 0xC0) != 0x80) {
                        return false;
                    }
                    cp = (cp << 6) | (next & 0x3F);
                }
                if ((count == 3 && cp < 0x800) || (count == 4 && cp < 0x10000) ||
                    (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
                    return false;
                }
                i += count;
            }
            return true;
        }

        struct JsonValue
        {
            enum class Kind
            {
                kNull,
                kBoolean,
                kNumber,
                kString,
                kArray,
                kObject
            };

            Kind kind{ Kind::kNull };
            std::size_t offset{ 0 };
            bool boolean{ false };
            double number{ 0.0 };
            bool numberIsInteger{ false };
            std::string string;
            std::vector<JsonValue> array;
            std::vector<std::pair<std::string, JsonValue>> object;

            [[nodiscard]] const JsonValue* Find(const std::string_view a_key) const noexcept
            {
                const auto it = std::ranges::find_if(object, [&](const auto& a_entry) {
                    return a_entry.first == a_key;
                });
                return it == object.end() ? nullptr : std::addressof(it->second);
            }
        };

        class JsonReader
        {
        public:
            explicit JsonReader(const std::string_view a_text) : _text(a_text) {}

            [[nodiscard]] bool Parse(JsonValue& a_out)
            {
                SkipWhitespace();
                if (!ParseValue(a_out, 0)) {
                    return false;
                }
                SkipWhitespace();
                return _pos == _text.size() || Fail("trailing data after JSON value");
            }

            [[nodiscard]] std::size_t ErrorOffset() const noexcept { return _errorOffset; }
            [[nodiscard]] const std::string& Error() const noexcept { return _error; }

        private:
            [[nodiscard]] bool ParseValue(JsonValue& a_out, const std::size_t a_depth)
            {
                if (a_depth > kMaxJsonDepth) {
                    return Fail("JSON nesting exceeds safety limit");
                }
                SkipWhitespace();
                a_out.offset = _pos;
                if (_pos >= _text.size()) {
                    return Fail("truncated JSON value");
                }
                switch (_text[_pos]) {
                case '{': return ParseObject(a_out, a_depth + 1);
                case '[': return ParseArray(a_out, a_depth + 1);
                case '"':
                    a_out.kind = JsonValue::Kind::kString;
                    return ParseString(a_out.string);
                case 't':
                    a_out.kind = JsonValue::Kind::kBoolean;
                    a_out.boolean = true;
                    return ConsumeLiteral("true");
                case 'f':
                    a_out.kind = JsonValue::Kind::kBoolean;
                    a_out.boolean = false;
                    return ConsumeLiteral("false");
                case 'n':
                    a_out.kind = JsonValue::Kind::kNull;
                    return ConsumeLiteral("null");
                default:
                    if (_text[_pos] == '-' || (_text[_pos] >= '0' && _text[_pos] <= '9')) {
                        a_out.kind = JsonValue::Kind::kNumber;
                        return ParseNumber(a_out.number, a_out.numberIsInteger);
                    }
                    return Fail("invalid JSON value");
                }
            }

            [[nodiscard]] bool ParseObject(JsonValue& a_out, const std::size_t a_depth)
            {
                a_out.kind = JsonValue::Kind::kObject;
                ++_pos;
                SkipWhitespace();
                if (Consume('}')) {
                    return true;
                }
                std::unordered_set<std::string> keys;
                for (;;) {
                    if (a_out.object.size() >= kMaxJsonProperties) {
                        return Fail("JSON object exceeds property safety limit");
                    }
                    const auto keyOffset = _pos;
                    std::string key;
                    if (!ParseString(key)) {
                        return false;
                    }
                    if (!keys.insert(key).second) {
                        return FailAt(keyOffset, "duplicate object property '" + key + "'");
                    }
                    SkipWhitespace();
                    if (!Consume(':')) {
                        return Fail("expected ':' after object property");
                    }
                    JsonValue value;
                    if (!ParseValue(value, a_depth)) {
                        return false;
                    }
                    a_out.object.emplace_back(std::move(key), std::move(value));
                    SkipWhitespace();
                    if (Consume('}')) {
                        return true;
                    }
                    if (!Consume(',')) {
                        return Fail("expected ',' or '}' in object");
                    }
                    SkipWhitespace();
                }
            }

            [[nodiscard]] bool ParseArray(JsonValue& a_out, const std::size_t a_depth)
            {
                a_out.kind = JsonValue::Kind::kArray;
                ++_pos;
                SkipWhitespace();
                if (Consume(']')) {
                    return true;
                }
                for (;;) {
                    if (a_out.array.size() >= kMaxJsonArrayElements) {
                        return Fail("JSON array exceeds element safety limit");
                    }
                    JsonValue value;
                    if (!ParseValue(value, a_depth)) {
                        return false;
                    }
                    a_out.array.push_back(std::move(value));
                    SkipWhitespace();
                    if (Consume(']')) {
                        return true;
                    }
                    if (!Consume(',')) {
                        return Fail("expected ',' or ']' in array");
                    }
                    SkipWhitespace();
                }
            }

            [[nodiscard]] bool ParseNumber(double& a_out, bool& a_integer)
            {
                const auto begin = _pos;
                if (_text[_pos] == '-') {
                    ++_pos;
                }
                if (_pos >= _text.size()) {
                    return Fail("truncated JSON number");
                }
                if (_text[_pos] == '0') {
                    ++_pos;
                    if (_pos < _text.size() && std::isdigit(static_cast<unsigned char>(_text[_pos]))) {
                        return Fail("JSON number has a leading zero");
                    }
                } else if (_text[_pos] >= '1' && _text[_pos] <= '9') {
                    while (_pos < _text.size() && std::isdigit(static_cast<unsigned char>(_text[_pos]))) {
                        ++_pos;
                    }
                } else {
                    return Fail("invalid JSON number");
                }

                a_integer = true;
                if (_pos < _text.size() && _text[_pos] == '.') {
                    a_integer = false;
                    ++_pos;
                    const auto fraction = _pos;
                    while (_pos < _text.size() && std::isdigit(static_cast<unsigned char>(_text[_pos]))) {
                        ++_pos;
                    }
                    if (_pos == fraction) {
                        return Fail("JSON fraction requires at least one digit");
                    }
                }
                if (_pos < _text.size() && (_text[_pos] == 'e' || _text[_pos] == 'E')) {
                    a_integer = false;
                    ++_pos;
                    if (_pos < _text.size() && (_text[_pos] == '+' || _text[_pos] == '-')) {
                        ++_pos;
                    }
                    const auto exponent = _pos;
                    while (_pos < _text.size() && std::isdigit(static_cast<unsigned char>(_text[_pos]))) {
                        ++_pos;
                    }
                    if (_pos == exponent) {
                        return Fail("JSON exponent requires at least one digit");
                    }
                }

                const auto* first = _text.data() + begin;
                const auto* last = _text.data() + _pos;
                const auto [ptr, ec] = std::from_chars(first, last, a_out, std::chars_format::general);
                if (ec != std::errc{} || ptr != last || !std::isfinite(a_out)) {
                    return FailAt(begin, "JSON number is out of finite range");
                }
                return true;
            }

            [[nodiscard]] bool ParseHex4(std::uint32_t& a_out)
            {
                if (_text.size() - _pos < 4) {
                    return Fail("truncated Unicode escape");
                }
                std::uint32_t value = 0;
                for (std::size_t i = 0; i < 4; ++i) {
                    const char ch = _text[_pos++];
                    value <<= 4;
                    if (ch >= '0' && ch <= '9') value |= static_cast<std::uint32_t>(ch - '0');
                    else if (ch >= 'a' && ch <= 'f') value |= static_cast<std::uint32_t>(ch - 'a' + 10);
                    else if (ch >= 'A' && ch <= 'F') value |= static_cast<std::uint32_t>(ch - 'A' + 10);
                    else return Fail("invalid Unicode escape");
                }
                a_out = value;
                return true;
            }

            [[nodiscard]] bool ParseString(std::string& a_out)
            {
                if (!Consume('"')) {
                    return Fail("expected JSON string");
                }
                while (_pos < _text.size()) {
                    const auto ch = static_cast<unsigned char>(_text[_pos++]);
                    if (ch == '"') {
                        if (!IsValidUTF8(a_out)) {
                            return Fail("invalid UTF-8 in JSON string");
                        }
                        return true;
                    }
                    if (ch < 0x20) {
                        return Fail("control character in JSON string");
                    }
                    if (ch != '\\') {
                        a_out.push_back(static_cast<char>(ch));
                    } else {
                        if (_pos >= _text.size()) {
                            return Fail("truncated JSON escape");
                        }
                        switch (const char esc = _text[_pos++]) {
                        case '"': a_out.push_back('"'); break;
                        case '\\': a_out.push_back('\\'); break;
                        case '/': a_out.push_back('/'); break;
                        case 'b': a_out.push_back('\b'); break;
                        case 'f': a_out.push_back('\f'); break;
                        case 'n': a_out.push_back('\n'); break;
                        case 'r': a_out.push_back('\r'); break;
                        case 't': a_out.push_back('\t'); break;
                        case 'u': {
                            std::uint32_t cp = 0;
                            if (!ParseHex4(cp)) return false;
                            if (cp >= 0xD800 && cp <= 0xDBFF) {
                                if (_text.size() - _pos < 6 || _text[_pos] != '\\' || _text[_pos + 1] != 'u') {
                                    return Fail("high surrogate without low surrogate");
                                }
                                _pos += 2;
                                std::uint32_t low = 0;
                                if (!ParseHex4(low)) return false;
                                if (low < 0xDC00 || low > 0xDFFF) return Fail("invalid low surrogate");
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                                return Fail("unpaired low surrogate");
                            }
                            AppendUTF8(a_out, cp);
                            break;
                        }
                        default: return Fail("invalid JSON escape");
                        }
                    }
                    if (a_out.size() > kMaxJsonStringBytes) {
                        return Fail("JSON string exceeds safety limit");
                    }
                }
                return Fail("unterminated JSON string");
            }

            [[nodiscard]] bool ConsumeLiteral(const std::string_view a_literal)
            {
                if (_text.substr(_pos, a_literal.size()) != a_literal) {
                    return Fail("invalid JSON literal");
                }
                _pos += a_literal.size();
                return true;
            }

            void SkipWhitespace() noexcept
            {
                while (_pos < _text.size() && (_text[_pos] == ' ' || _text[_pos] == '\t' ||
                                                _text[_pos] == '\r' || _text[_pos] == '\n')) {
                    ++_pos;
                }
            }

            [[nodiscard]] bool Consume(const char a_expected) noexcept
            {
                if (_pos < _text.size() && _text[_pos] == a_expected) {
                    ++_pos;
                    return true;
                }
                return false;
            }

            [[nodiscard]] bool Fail(std::string a_message)
            {
                return FailAt(_pos, std::move(a_message));
            }

            [[nodiscard]] bool FailAt(const std::size_t a_offset, std::string a_message)
            {
                if (_error.empty()) {
                    _errorOffset = a_offset;
                    _error = std::move(a_message);
                }
                return false;
            }

            std::string_view _text;
            std::size_t _pos{ 0 };
            std::size_t _errorOffset{ 0 };
            std::string _error;
        };

        void AddIssue(PresetResult& a_result, const std::filesystem::path& a_path,
                      const std::size_t a_offset, std::string a_code, std::string a_message)
        {
            a_result.issues.push_back({ a_path, a_offset, std::move(a_code), std::move(a_message) });
        }

        [[nodiscard]] bool HasOnlyProperties(const JsonValue& a_object,
                                             const std::initializer_list<std::string_view> a_allowed,
                                             PresetResult& a_result,
                                             const std::filesystem::path& a_path,
                                             const std::string_view a_context)
        {
            for (const auto& [name, value] : a_object.object) {
                if (std::ranges::find(a_allowed, name) == a_allowed.end()) {
                    AddIssue(a_result, a_path, value.offset, "unknown_property",
                             std::string(a_context) + " has unknown property '" + name + "'");
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] const JsonValue* Require(const JsonValue& a_object, const std::string_view a_name,
                                               const JsonValue::Kind a_kind, PresetResult& a_result,
                                               const std::filesystem::path& a_path,
                                               const std::string_view a_context)
        {
            const auto* value = a_object.Find(a_name);
            if (!value) {
                AddIssue(a_result, a_path, a_object.offset, "missing_property",
                         std::string(a_context) + " is missing property '" + std::string(a_name) + "'");
                return nullptr;
            }
            if (value->kind != a_kind) {
                AddIssue(a_result, a_path, value->offset, "wrong_type",
                         std::string(a_context) + " property '" + std::string(a_name) + "' has the wrong type");
                return nullptr;
            }
            return value;
        }

        [[nodiscard]] bool IsSemanticString(const std::string_view a_value, const bool a_allowEmpty) noexcept
        {
            if ((!a_allowEmpty && a_value.empty()) || a_value.size() > kMaxSemanticStringBytes) {
                return false;
            }
            return std::ranges::all_of(a_value, [](const unsigned char a_ch) {
                return a_ch >= 0x20 && a_ch != 0x7F;
            });
        }

        [[nodiscard]] bool ReadString(const JsonValue& a_object, const std::string_view a_name,
                                      std::string& a_out, PresetResult& a_result,
                                      const std::filesystem::path& a_path,
                                      const bool a_allowEmpty = true)
        {
            const auto* value = Require(a_object, a_name, JsonValue::Kind::kString, a_result, a_path, "preset");
            if (!value) {
                return false;
            }
            if (!IsSemanticString(value->string, a_allowEmpty)) {
                AddIssue(a_result, a_path, value->offset, "invalid_string",
                         "property '" + std::string(a_name) + "' contains an invalid or oversized string");
                return false;
            }
            a_out = value->string;
            return true;
        }

        [[nodiscard]] bool ReadBoundedNumber(const JsonValue& a_value, const double a_min,
                                             const double a_max, double& a_out, PresetResult& a_result,
                                             const std::filesystem::path& a_path,
                                             const std::string_view a_context)
        {
            if (a_value.kind != JsonValue::Kind::kNumber) {
                AddIssue(a_result, a_path, a_value.offset, "wrong_type",
                         std::string(a_context) + " must be a number");
                return false;
            }
            if (!std::isfinite(a_value.number) || a_value.number < a_min || a_value.number > a_max) {
                AddIssue(a_result, a_path, a_value.offset, "number_out_of_range",
                         std::string(a_context) + " is outside the accepted range");
                return false;
            }
            a_out = a_value.number;
            return true;
        }

        [[nodiscard]] bool ReadUInt(const JsonValue& a_value, const std::uint32_t a_max,
                                    std::uint32_t& a_out, PresetResult& a_result,
                                    const std::filesystem::path& a_path,
                                    const std::string_view a_context)
        {
            if (a_value.kind != JsonValue::Kind::kNumber || !a_value.numberIsInteger ||
                a_value.number < 0.0 || a_value.number > static_cast<double>(a_max)) {
                AddIssue(a_result, a_path, a_value.offset, "invalid_integer",
                         std::string(a_context) + " must be a bounded non-negative integer");
                return false;
            }
            a_out = static_cast<std::uint32_t>(a_value.number);
            return true;
        }

        [[nodiscard]] bool ReadStringArray(const JsonValue& a_value, const std::size_t a_maxCount,
                                           std::vector<std::string>& a_out, PresetResult& a_result,
                                           const std::filesystem::path& a_path,
                                           const std::string_view a_context)
        {
            if (a_value.kind != JsonValue::Kind::kArray) {
                AddIssue(a_result, a_path, a_value.offset, "wrong_type",
                         std::string(a_context) + " must be an array");
                return false;
            }
            if (a_value.array.size() > a_maxCount) {
                AddIssue(a_result, a_path, a_value.offset, "count_out_of_range",
                         std::string(a_context) + " exceeds its element limit");
                return false;
            }
            a_out.reserve(a_value.array.size());
            for (const auto& entry : a_value.array) {
                if (entry.kind != JsonValue::Kind::kString || !IsSemanticString(entry.string, true)) {
                    AddIssue(a_result, a_path, entry.offset, "invalid_string",
                             std::string(a_context) + " contains a non-string or invalid string");
                    return false;
                }
                a_out.push_back(entry.string);
            }
            return true;
        }

        [[nodiscard]] bool ReadMorphWeights(const JsonValue& a_value, PresetMorphWeights& a_out,
                                            PresetResult& a_result, const std::filesystem::path& a_path)
        {
            if (a_value.kind != JsonValue::Kind::kObject ||
                !HasOnlyProperties(a_value, { "x", "y", "z" }, a_result, a_path, "MorphWeights")) {
                if (a_value.kind != JsonValue::Kind::kObject) {
                    AddIssue(a_result, a_path, a_value.offset, "wrong_type", "MorphWeights must be an object");
                }
                return false;
            }
            const auto* x = Require(a_value, "x", JsonValue::Kind::kNumber, a_result, a_path, "MorphWeights");
            const auto* y = Require(a_value, "y", JsonValue::Kind::kNumber, a_result, a_path, "MorphWeights");
            const auto* z = Require(a_value, "z", JsonValue::Kind::kNumber, a_result, a_path, "MorphWeights");
            return x && y && z &&
                   ReadBoundedNumber(*x, 0.0, 1.0, a_out.x, a_result, a_path, "MorphWeights.x") &&
                   ReadBoundedNumber(*y, 0.0, 1.0, a_out.y, a_result, a_path, "MorphWeights.y") &&
                   ReadBoundedNumber(*z, 0.0, 1.0, a_out.z, a_result, a_path, "MorphWeights.z");
        }

        [[nodiscard]] bool ReadNamedMorphs(const JsonValue& a_value,
                                           std::vector<PresetNamedMorph>& a_out,
                                           PresetResult& a_result,
                                           const std::filesystem::path& a_path)
        {
            if (a_value.kind != JsonValue::Kind::kArray || a_value.array.size() > kMaxFacialMorphs) {
                AddIssue(a_result, a_path, a_value.offset, "count_or_type",
                         "FacialMorphSliderDataA must be a bounded array");
                return false;
            }
            std::unordered_set<std::string> names;
            a_out.reserve(a_value.array.size());
            for (const auto& entry : a_value.array) {
                if (entry.kind != JsonValue::Kind::kObject ||
                    !HasOnlyProperties(entry, { "Name", "Value" }, a_result, a_path,
                                       "FacialMorphSliderDataA element")) {
                    if (entry.kind != JsonValue::Kind::kObject) {
                        AddIssue(a_result, a_path, entry.offset, "wrong_type",
                                 "FacialMorphSliderDataA elements must be objects");
                    }
                    return false;
                }
                const auto* name = Require(entry, "Name", JsonValue::Kind::kString, a_result, a_path,
                                           "FacialMorphSliderDataA element");
                const auto* value = Require(entry, "Value", JsonValue::Kind::kNumber, a_result, a_path,
                                            "FacialMorphSliderDataA element");
                if (!name || !value || !IsSemanticString(name->string, false)) {
                    if (name && !IsSemanticString(name->string, false)) {
                        AddIssue(a_result, a_path, name->offset, "invalid_string", "facial morph name is invalid");
                    }
                    return false;
                }
                if (!names.insert(name->string).second) {
                    AddIssue(a_result, a_path, name->offset, "duplicate_semantic_key",
                             "duplicate facial morph name '" + name->string + "'");
                    return false;
                }
                PresetNamedMorph decoded{ .name = name->string };
                if (!ReadBoundedNumber(*value, 0.0, 1.0, decoded.value, a_result, a_path,
                                       "facial morph value")) {
                    return false;
                }
                a_out.push_back(std::move(decoded));
            }
            return true;
        }

        [[nodiscard]] bool ReadBoneRegions(const JsonValue& a_value,
                                           std::vector<PresetBoneRegion>& a_out,
                                           PresetResult& a_result,
                                           const std::filesystem::path& a_path)
        {
            if (a_value.kind != JsonValue::Kind::kArray || a_value.array.size() > kMaxBoneRegions) {
                AddIssue(a_result, a_path, a_value.offset, "count_or_type",
                         "FacialBoneRegionDataA must be a bounded array");
                return false;
            }
            std::unordered_set<std::uint32_t> regionIDs;
            a_out.reserve(a_value.array.size());
            for (const auto& entry : a_value.array) {
                if (entry.kind != JsonValue::Kind::kObject ||
                    !HasOnlyProperties(entry, { "RegionID", "SlidersA" }, a_result, a_path,
                                       "FacialBoneRegionDataA element")) {
                    if (entry.kind != JsonValue::Kind::kObject) {
                        AddIssue(a_result, a_path, entry.offset, "wrong_type",
                                 "FacialBoneRegionDataA elements must be objects");
                    }
                    return false;
                }
                const auto* regionID = Require(entry, "RegionID", JsonValue::Kind::kNumber, a_result, a_path,
                                               "FacialBoneRegionDataA element");
                const auto* sliders = Require(entry, "SlidersA", JsonValue::Kind::kArray, a_result, a_path,
                                              "FacialBoneRegionDataA element");
                PresetBoneRegion decoded;
                if (!regionID || !sliders || !ReadUInt(*regionID, 65535, decoded.regionID, a_result, a_path,
                                                       "RegionID")) {
                    return false;
                }
                if (!regionIDs.insert(decoded.regionID).second) {
                    AddIssue(a_result, a_path, regionID->offset, "duplicate_semantic_key",
                             "duplicate facial bone RegionID");
                    return false;
                }
                if (sliders->array.size() > kMaxBoneSlidersPerRegion) {
                    AddIssue(a_result, a_path, sliders->offset, "count_out_of_range",
                             "SlidersA exceeds its element limit");
                    return false;
                }
                std::unordered_set<std::string> sliderKeys;
                decoded.sliders.reserve(sliders->array.size());
                for (const auto& slider : sliders->array) {
                    if (slider.kind != JsonValue::Kind::kObject ||
                        !HasOnlyProperties(slider, { "GroupName", "ID", "Value" }, a_result, a_path,
                                           "SlidersA element")) {
                        if (slider.kind != JsonValue::Kind::kObject) {
                            AddIssue(a_result, a_path, slider.offset, "wrong_type",
                                     "SlidersA elements must be objects");
                        }
                        return false;
                    }
                    const auto* group = Require(slider, "GroupName", JsonValue::Kind::kString, a_result, a_path,
                                                "SlidersA element");
                    const auto* id = Require(slider, "ID", JsonValue::Kind::kNumber, a_result, a_path,
                                             "SlidersA element");
                    const auto* value = Require(slider, "Value", JsonValue::Kind::kNumber, a_result, a_path,
                                                "SlidersA element");
                    PresetBoneSlider decodedSlider;
                    if (!group || !id || !value || !IsSemanticString(group->string, true) ||
                        !ReadUInt(*id, 65535, decodedSlider.id, a_result, a_path, "slider ID") ||
                        !ReadBoundedNumber(*value, -1.0, 1.0, decodedSlider.value, a_result, a_path,
                                           "bone slider value")) {
                        if (group && !IsSemanticString(group->string, true)) {
                            AddIssue(a_result, a_path, group->offset, "invalid_string", "slider group is invalid");
                        }
                        return false;
                    }
                    decodedSlider.groupName = group->string;
                    const auto key = decodedSlider.groupName + '\x1F' + std::to_string(decodedSlider.id);
                    if (!sliderKeys.insert(key).second) {
                        AddIssue(a_result, a_path, slider.offset, "duplicate_semantic_key",
                                 "duplicate slider group/ID within facial bone region");
                        return false;
                    }
                    decoded.sliders.push_back(std::move(decodedSlider));
                }
                a_out.push_back(std::move(decoded));
            }
            return true;
        }

        [[nodiscard]] bool ReadValueWrapper(const JsonValue& a_object, std::string& a_out,
                                            PresetResult& a_result, const std::filesystem::path& a_path,
                                            const std::string_view a_context)
        {
            if (a_object.kind != JsonValue::Kind::kObject ||
                !HasOnlyProperties(a_object, { "Value" }, a_result, a_path, a_context)) {
                if (a_object.kind != JsonValue::Kind::kObject) {
                    AddIssue(a_result, a_path, a_object.offset, "wrong_type",
                             std::string(a_context) + " must be an object");
                }
                return false;
            }
            const auto* value = Require(a_object, "Value", JsonValue::Kind::kString, a_result, a_path, a_context);
            if (!value || !IsSemanticString(value->string, true)) {
                if (value && !IsSemanticString(value->string, true)) {
                    AddIssue(a_result, a_path, value->offset, "invalid_string",
                             std::string(a_context) + " string is invalid");
                }
                return false;
            }
            a_out = value->string;
            return true;
        }

        [[nodiscard]] bool ReadTintLayers(const JsonValue& a_value,
                                          std::vector<PresetTintLayer>& a_out,
                                          PresetResult& a_result,
                                          const std::filesystem::path& a_path,
                                          const bool a_charGenMenu)
        {
            if (a_value.kind != JsonValue::Kind::kObject ||
                !HasOnlyProperties(a_value, { "LayersA" }, a_result, a_path,
                                   "PostBlendFaceCustomization")) {
                if (a_value.kind != JsonValue::Kind::kObject) {
                    AddIssue(a_result, a_path, a_value.offset, "wrong_type",
                             "PostBlendFaceCustomization must be an object");
                }
                return false;
            }
            const auto* layers = Require(a_value, "LayersA", JsonValue::Kind::kArray, a_result, a_path,
                                         "PostBlendFaceCustomization");
            if (!layers || layers->array.size() > kMaxTintLayers) {
                if (layers && layers->array.size() > kMaxTintLayers) {
                    AddIssue(a_result, a_path, layers->offset, "count_out_of_range",
                             "PostBlendFaceCustomization.LayersA exceeds its element limit");
                }
                return false;
            }
            std::unordered_set<std::string> names;
            a_out.reserve(layers->array.size());
            for (const auto& layer : layers->array) {
                if (layer.kind != JsonValue::Kind::kObject ||
                    !HasOnlyProperties(layer, { "Intensity", "ModulationValue", "Name", "Value" },
                                       a_result, a_path, "tint layer")) {
                    if (layer.kind != JsonValue::Kind::kObject) {
                        AddIssue(a_result, a_path, layer.offset, "wrong_type", "tint layers must be objects");
                    }
                    return false;
                }
                const auto* intensity = Require(layer, "Intensity", JsonValue::Kind::kNumber, a_result, a_path,
                                                "tint layer");
                const auto* modulation = Require(layer, "ModulationValue", JsonValue::Kind::kObject,
                                                 a_result, a_path, "tint layer");
                const auto* name = Require(layer, "Name", JsonValue::Kind::kString, a_result, a_path,
                                           "tint layer");
                const auto* value = Require(layer, "Value", JsonValue::Kind::kObject, a_result, a_path,
                                            "tint layer");
                if (!intensity || !modulation || !name || !value || !IsSemanticString(name->string, false)) {
                    if (name && !IsSemanticString(name->string, false)) {
                        AddIssue(a_result, a_path, name->offset, "invalid_string", "tint layer name is invalid");
                    }
                    return false;
                }
                if (!names.insert(name->string).second) {
                    AddIssue(a_result, a_path, name->offset, "duplicate_semantic_key",
                             "duplicate tint layer name '" + name->string + "'");
                    return false;
                }
                PresetTintLayer decoded{ .name = name->string };
                if (!ReadBoundedNumber(*intensity, 0.0, a_charGenMenu ? 0.5 : 1.0,
                                       decoded.intensity, a_result, a_path,
                                       "tint layer intensity") ||
                    !ReadValueWrapper(*modulation, decoded.modulationValue, a_result, a_path,
                                      "tint layer ModulationValue") ||
                    !ReadValueWrapper(*value, decoded.value, a_result, a_path, "tint layer Value")) {
                    return false;
                }
                if (a_charGenMenu) {
                    const auto scaled = decoded.intensity * kCharGenMenuTintScale;
                    const auto packed = std::round(scaled);
                    if (std::abs(scaled - packed) > kTintQuantizationTolerance) {
                        AddIssue(a_result, a_path, intensity->offset, "invalid_char_gen_tint_intensity",
                                 "CharGenMenu tint intensity must be a 1/128-quantized value from 0 to 0.5");
                        return false;
                    }
                    decoded.intensity = packed / kRuntimeTintScale;
                }
                a_out.push_back(std::move(decoded));
            }
            return true;
        }
    }

    PresetResult ParseCkPreset(const std::string_view a_json, const std::filesystem::path& a_path)
    {
        PresetResult result;
        if (a_json.empty() || a_json.size() > kMaxPresetBytes) {
            AddIssue(result, a_path, 0, "invalid_size", "preset is empty or exceeds the 32 MiB safety limit");
            return result;
        }

        JsonValue root;
        JsonReader reader{ a_json };
        if (!reader.Parse(root)) {
            AddIssue(result, a_path, reader.ErrorOffset(), "invalid_json", reader.Error());
            return result;
        }
        if (root.kind != JsonValue::Kind::kObject) {
            AddIssue(result, a_path, root.offset, "wrong_type", "CK preset root must be an object");
            return result;
        }
        if (!HasOnlyProperties(root,
                               { "BodyMorphRegionValuesA", "BrowHairColor", "EyeColor",
                                 "FacialBoneRegionDataA", "FacialHairColor", "FacialMorphSliderDataA",
                                 "HairColor", "JewelryColor", "MiscHeadPartsA", "MorphWeights",
                                 "NPCFormEditorID", "PostBlendFaceCustomization", "RaceFormID", "Sex",
                                 "SkinTone", "TeethCustomization", "UniqueHeadPartsA" },
                               result, a_path, "CK preset root")) {
            return result;
        }

        AppearancePreset preset;
        const auto* body = Require(root, "BodyMorphRegionValuesA", JsonValue::Kind::kArray,
                                   result, a_path, "CK preset root");
        const auto* bones = Require(root, "FacialBoneRegionDataA", JsonValue::Kind::kArray,
                                    result, a_path, "CK preset root");
        const auto* facialMorphs = Require(root, "FacialMorphSliderDataA", JsonValue::Kind::kArray,
                                           result, a_path, "CK preset root");
        const auto* miscHeadParts = Require(root, "MiscHeadPartsA", JsonValue::Kind::kArray,
                                            result, a_path, "CK preset root");
        const auto* weights = Require(root, "MorphWeights", JsonValue::Kind::kObject,
                                      result, a_path, "CK preset root");
        const auto* postBlend = Require(root, "PostBlendFaceCustomization", JsonValue::Kind::kObject,
                                        result, a_path, "CK preset root");
        const auto* skinTone = Require(root, "SkinTone", JsonValue::Kind::kNumber,
                                       result, a_path, "CK preset root");
        const auto* uniqueHeadParts = Require(root, "UniqueHeadPartsA", JsonValue::Kind::kArray,
                                              result, a_path, "CK preset root");
        const auto* sex = Require(root, "Sex", JsonValue::Kind::kString,
                                  result, a_path, "CK preset root");

        if (!body || !bones || !facialMorphs || !miscHeadParts || !weights || !postBlend ||
            !skinTone || !uniqueHeadParts || !sex ||
            !ReadString(root, "BrowHairColor", preset.browHairColor, result, a_path) ||
            !ReadString(root, "EyeColor", preset.eyeColor, result, a_path) ||
            !ReadString(root, "FacialHairColor", preset.facialHairColor, result, a_path) ||
            !ReadString(root, "HairColor", preset.hairColor, result, a_path) ||
            !ReadString(root, "JewelryColor", preset.jewelryColor, result, a_path) ||
            !ReadString(root, "NPCFormEditorID", preset.npcFormEditorID, result, a_path) ||
            !ReadString(root, "RaceFormID", preset.raceFormID, result, a_path, false) ||
            !ReadString(root, "TeethCustomization", preset.teethCustomization, result, a_path) ||
            !ReadUInt(*skinTone, 255, preset.skinTone, result, a_path, "SkinTone")) {
            return result;
        }

        const bool isCharGenMenu = preset.npcFormEditorID.empty();
        if (isCharGenMenu) {
            preset.producer = kCharGenMenuPresetProducer;
        }

        if (sex->string == "Female") {
            preset.sex = PresetSex::kFemale;
        } else if (sex->string == "Male") {
            preset.sex = PresetSex::kMale;
        } else {
            AddIssue(result, a_path, sex->offset, "unsupported_value", "Sex must be exactly 'Male' or 'Female'");
            return result;
        }

        if (body->array.size() != kBodyMorphRegionCount) {
            AddIssue(result, a_path, body->offset, "count_out_of_range",
                     "BodyMorphRegionValuesA must contain exactly five values for CK 1.16.244");
            return result;
        }
        preset.bodyMorphRegionValues.reserve(body->array.size());
        for (const auto& value : body->array) {
            double decoded = 0.0;
            if (!ReadBoundedNumber(value, 0.0, 1.0, decoded, result, a_path,
                                   "body morph region value")) {
                return result;
            }
            preset.bodyMorphRegionValues.push_back(decoded);
        }

        if (!ReadMorphWeights(*weights, preset.morphWeights, result, a_path) ||
            !ReadStringArray(*miscHeadParts, kMaxHeadParts, preset.miscHeadParts, result, a_path,
                             "MiscHeadPartsA") ||
            !ReadStringArray(*uniqueHeadParts, kMaxHeadParts, preset.uniqueHeadParts, result, a_path,
                             "UniqueHeadPartsA") ||
            !ReadNamedMorphs(*facialMorphs, preset.facialMorphSliders, result, a_path) ||
            !ReadBoneRegions(*bones, preset.facialBoneRegions, result, a_path) ||
            !ReadTintLayers(*postBlend, preset.postBlendLayers, result, a_path, isCharGenMenu)) {
            return result;
        }

        result.preset = std::move(preset);
        return result;
    }

    PresetResult LoadCkPreset(const std::filesystem::path& a_path)
    {
        PresetResult result;
        auto extension = a_path.extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char a_ch) {
            return static_cast<char>(std::tolower(a_ch));
        });
        if (extension != ".npc") {
            AddIssue(result, a_path, 0, "invalid_extension", "preset path must use the .npc extension");
            return result;
        }

        std::error_code ec;
        const auto size = std::filesystem::file_size(a_path, ec);
        if (ec || size == 0 || size > kMaxPresetBytes) {
            AddIssue(result, a_path, 0, "invalid_size",
                     ec ? "could not determine preset size: " + ec.message()
                        : "preset is empty or exceeds the 32 MiB safety limit");
            return result;
        }

        std::string bytes(static_cast<std::size_t>(size), '\0');
        std::ifstream stream{ a_path, std::ios::binary };
        if (!stream.is_open() || !stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
            AddIssue(result, a_path, 0, "read_failed", "could not read the complete preset file");
            return result;
        }
        return ParseCkPreset(bytes, a_path);
    }
}
