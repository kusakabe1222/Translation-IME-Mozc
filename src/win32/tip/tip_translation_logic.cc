// Translation IME addition to Mozc. Distributed under the same terms as
// Mozc (BSD 3-Clause License, see LICENSE at the top of the repository).

#include "win32/tip/tip_translation_logic.h"

#include <InputScope.h>
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "win32/tip/translation/HttpTranslator.h"

namespace mozc {
namespace win32 {
namespace tsf {

bool StartsShiftTap(WPARAM vk, TranslationModifiers modifiers) {
  return vk == VK_SHIFT && !modifiers.control && !modifiers.alt;
}

bool IsSelectionTranslationKey(WPARAM vk, TranslationModifiers modifiers) {
  return vk == VK_CONVERT && modifiers.shift && !modifiers.control &&
         !modifiers.alt;
}

TranslationAction DecideShownKeyDown(WPARAM vk, TranslationModifiers modifiers,
                                     size_t item_count, int selected,
                                     int* index, bool selection) {
  *index = selected;
  if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
    return TranslationAction::kNone;
  }
  if (modifiers.control || modifiers.alt) {
    return TranslationAction::kCancel;
  }
  switch (vk) {
    case VK_DOWN:
    case VK_SPACE:
    case VK_CONVERT:
      return TranslationAction::kNext;
    case VK_TAB:
      return modifiers.shift ? TranslationAction::kPrevious
                             : TranslationAction::kNext;
    case VK_UP:
      return TranslationAction::kPrevious;
    case VK_RETURN:
      if (item_count > 0) {
        return TranslationAction::kCommit;
      }
      // Nothing to commit yet. Composition: back to the conversion, which
      // Enter commits. Selection: the application would replace the selected
      // text with a line break, so wait for the translations.
      return selection ? TranslationAction::kWait : TranslationAction::kCancel;
    case VK_ESCAPE:
    case VK_BACK:
      return TranslationAction::kCancel;
    default:
      break;
  }
  if (vk >= '1' && vk <= '9' && !modifiers.shift) {
    const size_t number = static_cast<size_t>(vk - '1');
    if (number < item_count) {
      *index = static_cast<int>(number);
      return TranslationAction::kCommit;
    }
    if (selection) {
      return TranslationAction::kWait;   // not there (yet): keep the selection
    }
  }
  return TranslationAction::kCancel;
}

bool PassesThroughAfterCancel(WPARAM vk, bool has_items) {
  if (vk == VK_ESCAPE || vk == VK_BACK) {
    return false;
  }
  if (vk == VK_RETURN) {
    return !has_items;
  }
  return true;
}

int NextIndex(int selected, size_t count) {
  if (count == 0) {
    return 0;
  }
  const int size = static_cast<int>(count);
  return (selected + 1) % size;
}

int PreviousIndex(int selected, size_t count) {
  if (count == 0) {
    return 0;
  }
  const int size = static_cast<int>(count);
  return (selected + size - 1) % size;
}

size_t LanguageRank(const std::vector<std::wstring>& languages,
                    const std::wstring& language) {
  for (size_t i = 0; i < languages.size(); ++i) {
    if (languages[i] == language) {
      return i;
    }
  }
  return languages.size();
}

void InsertInLanguageOrder(const std::vector<std::wstring>& languages,
                           const std::vector<TranslationItem>& arrived,
                           std::vector<TranslationItem>* items) {
  for (const TranslationItem& item : arrived) {
    if (!item.succeeded) {
      continue;
    }
    const size_t rank = LanguageRank(languages, item.language);
    auto position = items->begin();
    while (position != items->end() &&
           LanguageRank(languages, position->language) <= rank) {
      ++position;
    }
    items->insert(position, item);
  }
}

int IndexOfLanguage(const std::vector<TranslationItem>& items,
                    const std::wstring& language) {
  for (size_t i = 0; !language.empty() && i < items.size(); ++i) {
    if (items[i].language == language) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

std::wstring FailedLanguages(const std::vector<TranslationItem>& items) {
  std::wstring failed;
  for (const TranslationItem& item : items) {
    if (!item.succeeded) {
      failed += (failed.empty() ? L"" : L", ") + item.language;
    }
  }
  return failed;
}

std::wstring ProgressSuffix(size_t arrived, size_t total) {
  return L" (" + std::to_wstring(arrived) + L"/" + std::to_wstring(total) +
         L")";
}

bool IsSecureInputScope(InputScope scope) {
  switch (scope) {
    case IS_PASSWORD:
    case IS_NUMERIC_PASSWORD:
    case IS_NUMERIC_PIN:
    case IS_ALPHANUMERIC_PIN:
    case IS_ALPHANUMERIC_PIN_SET:
      return true;
    default:
      return false;
  }
}

namespace {

bool IsJapaneseCharacter(wchar_t ch) {
  return (ch >= 0x3040 && ch <= 0x30FF) ||  // hiragana, katakana
         (ch >= 0x31F0 && ch <= 0x31FF) ||  // katakana phonetic extensions
         (ch >= 0x3005 && ch <= 0x3007) ||  // iteration mark, closing mark, zero
         (ch >= 0x3400 && ch <= 0x4DBF) ||  // CJK extension A
         (ch >= 0x4E00 && ch <= 0x9FFF) ||  // CJK ideographs
         (ch >= 0xF900 && ch <= 0xFAFF) ||  // CJK compatibility ideographs
         (ch >= 0xFF66 && ch <= 0xFF9F);    // half-width katakana
}

// Letters of the other scripts a selection is likely to be in.
bool IsForeignLetter(wchar_t ch) {
  return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
         (ch >= 0x00C0 && ch <= 0x024F && ch != 0x00D7 && ch != 0x00F7) ||
         (ch >= 0x0370 && ch <= 0x052F) ||  // Greek, Cyrillic
         (ch >= 0x0590 && ch <= 0x06FF) ||  // Hebrew, Arabic
         (ch >= 0x0E00 && ch <= 0x0E7F) ||  // Thai
         (ch >= 0x1100 && ch <= 0x11FF) ||  // Hangul jamo
         (ch >= 0xAC00 && ch <= 0xD7A3) ||  // Hangul syllables
         (ch >= 0xFF21 && ch <= 0xFF3A) ||  // full-width Latin
         (ch >= 0xFF41 && ch <= 0xFF5A);
}

// Columns a character takes in the candidate window.
size_t ColumnWidth(wchar_t ch) {
  const bool wide = (ch >= 0x1100 && ch <= 0x115F) ||
                    (ch >= 0x2E80 && ch <= 0xA4CF) ||
                    (ch >= 0xAC00 && ch <= 0xD7A3) ||
                    (ch >= 0xF900 && ch <= 0xFAFF) ||
                    (ch >= 0xFE30 && ch <= 0xFE4F) ||
                    (ch >= 0xFF00 && ch <= 0xFF60) ||
                    (ch >= 0xFFE0 && ch <= 0xFFE6);
  return wide ? 2 : 1;
}

bool IsLowSurrogate(wchar_t ch) { return ch >= 0xDC00 && ch <= 0xDFFF; }

constexpr wchar_t kEllipsis = 0x2026;

}  // namespace

bool SelectionIsJapanese(const std::wstring& text) {
  // A Japanese word is about two characters, so one foreign word weighs two.
  size_t japanese = 0;
  size_t foreign_words = 0;
  bool in_word = false;
  for (wchar_t ch : text) {
    const bool letter = IsForeignLetter(ch);
    if (letter && !in_word) {
      ++foreign_words;
    }
    in_word = letter;
    if (IsJapaneseCharacter(ch)) {
      ++japanese;
    }
  }
  return japanese > 0 && japanese >= 2 * foreign_words;
}

std::wstring CandidatePreview(const std::wstring& text, size_t max_lines,
                              size_t max_width) {
  std::vector<std::wstring> lines(1);
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == L'\r' || text[i] == L'\n') {
      if (text[i] == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') {
        ++i;
      }
      lines.emplace_back();
    } else {
      lines.back() += text[i];
    }
  }
  while (lines.size() > 1 && lines.back().empty()) {
    lines.pop_back();
  }

  std::wstring preview;
  const size_t shown = (std::min)(lines.size(), max_lines);
  for (size_t i = 0; i < shown; ++i) {
    const std::wstring& line = lines[i];
    size_t width = 0;
    size_t end = 0;
    while (end < line.size() && width + ColumnWidth(line[end]) <= max_width) {
      width += ColumnWidth(line[end]);
      ++end;
    }
    if (end < line.size() && end > 0 && IsLowSurrogate(line[end])) {
      --end;  // never split a surrogate pair
    }
    if (i > 0) {
      preview += L'\n';
    }
    preview += line.substr(0, end);
    if (end < line.size() || (i + 1 == shown && shown < lines.size())) {
      preview += kEllipsis;
    }
  }
  return preview;
}

}  // namespace tsf
}  // namespace win32
}  // namespace mozc
