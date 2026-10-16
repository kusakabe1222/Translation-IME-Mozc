// Minimal JSON reader/writer helpers for the translation API responses.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace Json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value
{
    Type type = Type::Null;
    bool boolValue = false;
    double numberValue = 0;
    std::wstring stringValue;
    std::vector<Value> arrayValue;
    std::vector<std::pair<std::wstring, Value>> objectValue;

    // Object member lookup; nullptr if this is not an object or the key is missing.
    const Value* Get(const wchar_t* key) const;
    // Array element; nullptr if this is not an array or the index is out of range.
    const Value* At(size_t index) const;

    bool IsString() const { return type == Type::String; }
};

// Parses UTF-16 JSON text. Returns false on malformed input.
bool Parse(const std::wstring& text, Value* out);

// Returns text as a quoted JSON string literal.
std::wstring Quote(const std::wstring& text);

} // namespace Json
