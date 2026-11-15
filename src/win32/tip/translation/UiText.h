// The language of the translation feature's messages: Japanese when Windows is
// displayed in Japanese, English otherwise.

#pragma once

#include <windows.h>

namespace UiText {

inline bool IsJapanese()
{
    static const bool japanese = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE;
    return japanese;
}

// The Japanese or the English text, for the user's display language.
inline const wchar_t* Pick(const wchar_t* japanese, const wchar_t* english)
{
    return IsJapanese() ? japanese : english;
}

} // namespace UiText
