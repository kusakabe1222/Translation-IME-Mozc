// Minimal JSON reader/writer helpers for the translation API responses.

#include "Json.h"

#include <cstdlib>

namespace Json {

namespace {

const int kMaxDepth = 64;

class Parser
{
public:
    explicit Parser(const std::wstring& text) : _text(text) {}

    bool ParseDocument(Value* out)
    {
        SkipWhitespace();
        if (!ParseValue(out, 0))
        {
            return false;
        }
        SkipWhitespace();
        return _pos == _text.length();
    }

private:
    bool ParseValue(Value* out, int depth)
    {
        if (depth > kMaxDepth || _pos >= _text.length())
        {
            return false;
        }
        switch (_text[_pos])
        {
        case L'{': return ParseObject(out, depth);
        case L'[': return ParseArray(out, depth);
        case L'"':
            out->type = Type::String;
            return ParseString(&out->stringValue);
        case L't':
            out->type = Type::Bool;
            out->boolValue = true;
            return ConsumeLiteral(L"true");
        case L'f':
            out->type = Type::Bool;
            out->boolValue = false;
            return ConsumeLiteral(L"false");
        case L'n':
            out->type = Type::Null;
            return ConsumeLiteral(L"null");
        default:
            return ParseNumber(out);
        }
    }

    bool ParseObject(Value* out, int depth)
    {
        out->type = Type::Object;
        ++_pos; // '{'
        SkipWhitespace();
        if (Peek() == L'}')
        {
            ++_pos;
            return true;
        }
        for (;;)
        {
            SkipWhitespace();
            std::wstring key;
            if (Peek() != L'"' || !ParseString(&key))
            {
                return false;
            }
            SkipWhitespace();
            if (Peek() != L':')
            {
                return false;
            }
            ++_pos;
            SkipWhitespace();
            Value member;
            if (!ParseValue(&member, depth + 1))
            {
                return false;
            }
            out->objectValue.emplace_back(std::move(key), std::move(member));
            SkipWhitespace();
            if (Peek() == L',')
            {
                ++_pos;
                continue;
            }
            if (Peek() == L'}')
            {
                ++_pos;
                return true;
            }
            return false;
        }
    }

    bool ParseArray(Value* out, int depth)
    {
        out->type = Type::Array;
        ++_pos; // '['
        SkipWhitespace();
        if (Peek() == L']')
        {
            ++_pos;
            return true;
        }
        for (;;)
        {
            SkipWhitespace();
            Value element;
            if (!ParseValue(&element, depth + 1))
            {
                return false;
            }
            out->arrayValue.push_back(std::move(element));
            SkipWhitespace();
            if (Peek() == L',')
            {
                ++_pos;
                continue;
            }
            if (Peek() == L']')
            {
                ++_pos;
                return true;
            }
            return false;
        }
    }

    bool ParseString(std::wstring* out)
    {
        ++_pos; // opening quote
        while (_pos < _text.length())
        {
            wchar_t ch = _text[_pos++];
            if (ch == L'"')
            {
                return true;
            }
            if (ch != L'\\')
            {
                out->push_back(ch);
                continue;
            }
            if (_pos >= _text.length())
            {
                return false;
            }
            wchar_t esc = _text[_pos++];
            switch (esc)
            {
            case L'"':  out->push_back(L'"'); break;
            case L'\\': out->push_back(L'\\'); break;
            case L'/':  out->push_back(L'/'); break;
            case L'b':  out->push_back(L'\b'); break;
            case L'f':  out->push_back(L'\f'); break;
            case L'n':  out->push_back(L'\n'); break;
            case L'r':  out->push_back(L'\r'); break;
            case L't':  out->push_back(L'\t'); break;
            case L'u':
                {
                    // wchar_t is UTF-16, so surrogate pairs come out as two \u escapes naturally.
                    if (_pos + 4 > _text.length())
                    {
                        return false;
                    }
                    unsigned int code = 0;
                    for (int i = 0; i < 4; i++)
                    {
                        wchar_t hex = _text[_pos++];
                        code <<= 4;
                        if (hex >= L'0' && hex <= L'9') code |= hex - L'0';
                        else if (hex >= L'a' && hex <= L'f') code |= hex - L'a' + 10;
                        else if (hex >= L'A' && hex <= L'F') code |= hex - L'A' + 10;
                        else return false;
                    }
                    out->push_back(static_cast<wchar_t>(code));
                }
                break;
            default:
                return false;
            }
        }
        return false;
    }

    bool ParseNumber(Value* out)
    {
        size_t start = _pos;
        while (_pos < _text.length())
        {
            wchar_t ch = _text[_pos];
            if ((ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'+' || ch == L'.' || ch == L'e' || ch == L'E')
            {
                ++_pos;
                continue;
            }
            break;
        }
        if (start == _pos)
        {
            return false;
        }
        std::wstring number = _text.substr(start, _pos - start);
        wchar_t* end = nullptr;
        out->type = Type::Number;
        out->numberValue = wcstod(number.c_str(), &end);
        return end && *end == L'\0';
    }

    bool ConsumeLiteral(const wchar_t* literal)
    {
        std::wstring expected(literal);
        if (_text.compare(_pos, expected.length(), expected) != 0)
        {
            return false;
        }
        _pos += expected.length();
        return true;
    }

    void SkipWhitespace()
    {
        while (_pos < _text.length())
        {
            wchar_t ch = _text[_pos];
            if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n')
            {
                break;
            }
            ++_pos;
        }
    }

    wchar_t Peek() const { return (_pos < _text.length()) ? _text[_pos] : L'\0'; }

    const std::wstring& _text;
    size_t _pos = 0;
};

} // namespace

const Value* Value::Get(const wchar_t* key) const
{
    if (type != Type::Object)
    {
        return nullptr;
    }
    for (const auto& member : objectValue)
    {
        if (member.first == key)
        {
            return &member.second;
        }
    }
    return nullptr;
}

const Value* Value::At(size_t index) const
{
    if (type != Type::Array || index >= arrayValue.size())
    {
        return nullptr;
    }
    return &arrayValue[index];
}

bool Parse(const std::wstring& text, Value* out)
{
    *out = Value();
    Parser parser(text);
    return parser.ParseDocument(out);
}

std::wstring Quote(const std::wstring& text)
{
    static const wchar_t kHex[] = L"0123456789abcdef";

    std::wstring out;
    out.reserve(text.length() + 2);
    out.push_back(L'"');
    for (wchar_t ch : text)
    {
        switch (ch)
        {
        case L'"':  out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default:
            if (ch < 0x20)
            {
                out += L"\\u00";
                out.push_back(kHex[(ch >> 4) & 0xF]);
                out.push_back(kHex[ch & 0xF]);
            }
            else
            {
                out.push_back(ch);
            }
            break;
        }
    }
    out.push_back(L'"');
    return out;
}

} // namespace Json
