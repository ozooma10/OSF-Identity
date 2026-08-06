#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Strict, bounded JSON decoding shared by the package-manifest and preset
// parsers. This is deliberately not a general-purpose JSON library: every
// dimension of the input (depth, string bytes, array elements, object
// properties) is capped so a hostile document fails closed instead of
// exhausting memory, and duplicate object properties are rejected outright.
namespace NpcAppearance::Json
{
    struct Value
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
        // Every number carries its double value; `integer` is additionally
        // valid whenever `numberIsInteger` is set (always, under
        // ReaderLimits::integersOnly).
        double number{ 0.0 };
        bool numberIsInteger{ false };
        std::int64_t integer{ 0 };
        std::string string;
        std::vector<Value> array;
        std::vector<std::pair<std::string, Value>> object;

        [[nodiscard]] const Value* Find(std::string_view a_key) const noexcept;
    };

    struct ReaderLimits
    {
        std::size_t maxDepth{ 32 };
        std::size_t maxStringBytes{ 4096 };
        std::size_t maxArrayElements{ 4096 };
        // No property cap by default; the preset parser opts into one.
        std::size_t maxObjectProperties{ SIZE_MAX };
        // Reject fractions and exponents entirely and require the value to fit
        // std::int64_t, matching the manifest schema's integer-only fields.
        bool integersOnly{ false };
        // Validate that decoded strings are well-formed UTF-8.
        bool validateUTF8{ false };
    };

    class Reader
    {
    public:
        explicit Reader(std::string_view a_text, ReaderLimits a_limits = {}) noexcept
            : _text(a_text), _limits(a_limits)
        {}

        [[nodiscard]] bool Parse(Value& a_out);

        [[nodiscard]] std::size_t ErrorOffset() const noexcept { return _errorOffset; }
        [[nodiscard]] const std::string& Error() const noexcept { return _error; }

    private:
        [[nodiscard]] bool ParseValue(Value& a_out, std::size_t a_depth);
        [[nodiscard]] bool ParseObject(Value& a_out, std::size_t a_depth);
        [[nodiscard]] bool ParseArray(Value& a_out, std::size_t a_depth);
        [[nodiscard]] bool ParseNumber(Value& a_out);
        [[nodiscard]] bool ParseHex4(std::uint32_t& a_out);
        [[nodiscard]] bool ParseString(std::string& a_out);
        [[nodiscard]] bool ConsumeLiteral(std::string_view a_literal);
        void SkipWhitespace() noexcept;
        [[nodiscard]] bool Consume(char a_expected) noexcept;
        [[nodiscard]] bool Fail(std::string a_message);
        [[nodiscard]] bool FailAt(std::size_t a_offset, std::string a_message);

        std::string_view _text;
        ReaderLimits _limits;
        std::size_t _pos{ 0 };
        std::size_t _errorOffset{ 0 };
        std::string _error;
    };

    void AppendUTF8(std::string& a_out, std::uint32_t a_cp);
    [[nodiscard]] bool IsValidUTF8(std::string_view a_text) noexcept;
}
