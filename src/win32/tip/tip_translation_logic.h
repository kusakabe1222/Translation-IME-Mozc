// Translation IME addition to Mozc. Distributed under the same terms as
// Mozc (BSD 3-Clause License, see LICENSE at the top of the repository).

#ifndef MOZC_WIN32_TIP_TIP_TRANSLATION_LOGIC_H_
#define MOZC_WIN32_TIP_TIP_TRANSLATION_LOGIC_H_

#include <InputScope.h>
#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

#include "win32/tip/translation/HttpTranslator.h"

namespace mozc {
namespace win32 {
namespace tsf {

// The parts of the translation UI (tip_translation.cc) that do not need TSF:
// what a key does and how the translations are ordered. Kept apart so that
// they can be unit tested.

enum class TranslationAction {
  kNone,      // not ours: Mozc handles the key
  kToggle,    // Shift released alone
  kTranslateSelection,  // Shift+Henkan with a selection
  kNext,
  kPrevious,
  kCommit,
  kCancel,
  kWait,      // eaten, nothing happens yet (selection: the translations are on their way)
};

struct TranslationModifiers {
  bool shift = false;
  bool control = false;
  bool alt = false;
};

// A key down that may start a Shift tap (Shift alone, released with nothing
// else pressed in between).
bool StartsShiftTap(WPARAM vk, TranslationModifiers modifiers);

// A key down that translates the selection while nothing is shown: Shift+Henkan
// (Henkan alone is Mozc's reconversion).
bool IsSelectionTranslationKey(WPARAM vk, TranslationModifiers modifiers);

// What a key down does while |item_count| translations are shown, |selected|
// being focused. |*index| is the item to commit for kCommit. With |selection|
// (translating the application's selection), Enter and the numbers never reach
// the application: there they would replace the selected text.
TranslationAction DecideShownKeyDown(WPARAM vk, TranslationModifiers modifiers,
                                     size_t item_count, int selected,
                                     int* index, bool selection = false);

// Keys that end the translation UI without being consumed (e.g. typing goes on).
bool PassesThroughAfterCancel(WPARAM vk, bool has_items);

// The selection after kNext / kPrevious (wraps around; 0 when empty).
int NextIndex(int selected, size_t count);
int PreviousIndex(int selected, size_t count);

// Position of |language| in |languages| (the configured order); languages not
// in it come last.
size_t LanguageRank(const std::vector<std::wstring>& languages,
                    const std::wstring& language);

// Adds the successful items of an early result to |items|, keeping the
// configured order. Failed ones are left for the complete result.
void InsertInLanguageOrder(const std::vector<std::wstring>& languages,
                           const std::vector<TranslationItem>& arrived,
                           std::vector<TranslationItem>* items);

// Index of the item in |language|, or 0 when there is none (or it is empty).
int IndexOfLanguage(const std::vector<TranslationItem>& items,
                    const std::wstring& language);

// The languages that failed, as "EN, ZH" (empty when all succeeded).
std::wstring FailedLanguages(const std::vector<TranslationItem>& items);

// " (2/6)": how many of the languages have arrived.
std::wstring ProgressSuffix(size_t arrived, size_t total);

// Password and PIN fields: their text must never reach a translation API.
bool IsSecureInputScope(InputScope scope);

// Whether a selection is mainly Japanese: it then goes to the configured
// languages, otherwise into Japanese. Japanese characters (kana, kanji) are
// weighed against words in other scripts, so that English or code with a
// Japanese comment counts as foreign, while Japanese with a few English words
// stays Japanese.
bool SelectionIsJapanese(const std::wstring& text);

// What a candidate shows of |text|: at most |max_lines| lines of at most
// |max_width| columns (full-width characters count 2), with "..." (U+2026)
// where it was cut. Committing still inserts the whole translation.
std::wstring CandidatePreview(const std::wstring& text, size_t max_lines,
                              size_t max_width);

}  // namespace tsf
}  // namespace win32
}  // namespace mozc

#endif  // MOZC_WIN32_TIP_TIP_TRANSLATION_LOGIC_H_
