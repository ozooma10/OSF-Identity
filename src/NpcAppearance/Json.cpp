#include "NpcAppearance/Json.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <memory>
#include <ranges>
#include <system_error>
#include <unordered_set>

namespace NpcAppearance::Json
{
    const Value* Value::Find(const std::string_view a_key) const noexcept
    {
        const auto it = std::ranges::find_if(object, [&](const auto& a_entry) {
            return a_entry.first == a_key;
        });
        return it == object.end() ? nullptr : std::addressof(it->second);
    }

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

    bool IsValidUTF8(const std::string_view a_text) noexcept
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

    bool Reader::Parse(Value& a_out)
    {
        _totalNodes = 0;
        SkipWhitespace();
        if (!ParseValue(a_out, 0)) {
            return false;
        }
        SkipWhitespace();
        return _pos == _text.size() || Fail("trailing data after JSON value");
    }

    bool Reader::ParseValue(Value& a_out, const std::size_t a_depth)
    {
        if (_totalNodes >= _limits.maxTotalNodes) {
            return Fail("JSON value count exceeds safety limit");
        }
        ++_totalNodes;
        if (a_depth > _limits.maxDepth) {
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
            a_out.kind = Value::Kind::kString;
            return ParseString(a_out.string);
        case 't':
            a_out.kind = Value::Kind::kBoolean;
            a_out.boolean = true;
            return ConsumeLiteral("true");
        case 'f':
            a_out.kind = Value::Kind::kBoolean;
            a_out.boolean = false;
            return ConsumeLiteral("false");
        case 'n':
            a_out.kind = Value::Kind::kNull;
            return ConsumeLiteral("null");
        default:
            if (_text[_pos] == '-' || (_text[_pos] >= '0' && _text[_pos] <= '9')) {
                a_out.kind = Value::Kind::kNumber;
                return ParseNumber(a_out);
            }
            return Fail("invalid JSON value");
        }
    }

    bool Reader::ParseObject(Value& a_out, const std::size_t a_depth)
    {
        a_out.kind = Value::Kind::kObject;
        ++_pos;
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }
        std::unordered_set<std::string> keys;
        for (;;) {
            if (a_out.object.size() >= _limits.maxObjectProperties) {
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
            Value value;
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

    bool Reader::ParseArray(Value& a_out, const std::size_t a_depth)
    {
        a_out.kind = Value::Kind::kArray;
        ++_pos;
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }
        for (;;) {
            if (a_out.array.size() >= _limits.maxArrayElements) {
                return Fail("JSON array exceeds element safety limit");
            }
            Value value;
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

    bool Reader::ParseNumber(Value& a_out)
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

        a_out.numberIsInteger = true;
        if (_pos < _text.size() && _text[_pos] == '.') {
            if (_limits.integersOnly) {
                return Fail("number must be an integer");
            }
            a_out.numberIsInteger = false;
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
            if (_limits.integersOnly) {
                return Fail("number must be an integer");
            }
            a_out.numberIsInteger = false;
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
        if (_limits.integersOnly) {
            const auto [ptr, ec] = std::from_chars(first, last, a_out.integer, 10);
            if (ec != std::errc{} || ptr != last) {
                return FailAt(begin, "JSON integer is out of range");
            }
            a_out.number = static_cast<double>(a_out.integer);
            return true;
        }
        const auto [ptr, ec] = std::from_chars(first, last, a_out.number, std::chars_format::general);
        if (ec != std::errc{} || ptr != last || !std::isfinite(a_out.number)) {
            return FailAt(begin, "JSON number is out of finite range");
        }
        return true;
    }

    bool Reader::ParseHex4(std::uint32_t& a_out)
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

    bool Reader::ParseString(std::string& a_out)
    {
        if (!Consume('"')) {
            return Fail("expected JSON string");
        }
        while (_pos < _text.size()) {
            const auto ch = static_cast<unsigned char>(_text[_pos++]);
            if (ch == '"') {
                if (_limits.validateUTF8 && !IsValidUTF8(a_out)) {
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
                switch (_text[_pos++]) {
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
            if (a_out.size() > _limits.maxStringBytes) {
                return Fail("JSON string exceeds safety limit");
            }
        }
        return Fail("unterminated JSON string");
    }

    bool Reader::ConsumeLiteral(const std::string_view a_literal)
    {
        if (_text.substr(_pos, a_literal.size()) != a_literal) {
            return Fail("invalid JSON literal");
        }
        _pos += a_literal.size();
        return true;
    }

    void Reader::SkipWhitespace() noexcept
    {
        while (_pos < _text.size() && (_text[_pos] == ' ' || _text[_pos] == '\t' ||
                                        _text[_pos] == '\r' || _text[_pos] == '\n')) {
            ++_pos;
        }
    }

    bool Reader::Consume(const char a_expected) noexcept
    {
        if (_pos < _text.size() && _text[_pos] == a_expected) {
            ++_pos;
            return true;
        }
        return false;
    }

    bool Reader::Fail(std::string a_message)
    {
        return FailAt(_pos, std::move(a_message));
    }

    bool Reader::FailAt(const std::size_t a_offset, std::string a_message)
    {
        if (_error.empty()) {
            _errorOffset = a_offset;
            _error = std::move(a_message);
        }
        return false;
    }
}
