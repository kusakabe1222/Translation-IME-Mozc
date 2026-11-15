// Translation IME addition to Mozc. Distributed under the same terms as
// Mozc (BSD 3-Clause License, see LICENSE at the top of the repository).

#include "win32/tip/tip_translation_logic.h"

#include <InputScope.h>
#include <windows.h>

#include <string>
#include <vector>

#include "testing/gunit.h"
#include "win32/tip/translation/HttpTranslator.h"

namespace mozc {
namespace win32 {
namespace tsf {
namespace {

constexpr TranslationModifiers kNoModifiers = {};
constexpr TranslationModifiers kShift = {true, false, false};
constexpr TranslationModifiers kControl = {false, true, false};
constexpr TranslationModifiers kAlt = {false, false, true};

TranslationItem Item(const std::wstring& language, bool succeeded = true) {
  TranslationItem item;
  item.language = language;
  item.text = succeeded ? L"text-" + language : std::wstring();
  item.succeeded = succeeded;
  return item;
}

std::wstring Languages(const std::vector<TranslationItem>& items) {
  std::wstring result;
  for (const TranslationItem& item : items) {
    result += (result.empty() ? L"" : L",") + item.language;
  }
  return result;
}

TranslationAction Shown(WPARAM vk, TranslationModifiers modifiers = kNoModifiers,
                        size_t item_count = 3, int selected = 1,
                        int* index = nullptr) {
  int unused = -1;
  return DecideShownKeyDown(vk, modifiers, item_count, selected,
                            index != nullptr ? index : &unused);
}

TEST(TipTranslationLogicTest, StartsShiftTap) {
  EXPECT_TRUE(StartsShiftTap(VK_SHIFT, kNoModifiers));
  EXPECT_TRUE(StartsShiftTap(VK_SHIFT, kShift));
  EXPECT_FALSE(StartsShiftTap(VK_SHIFT, kControl));
  EXPECT_FALSE(StartsShiftTap(VK_SHIFT, kAlt));
  EXPECT_FALSE(StartsShiftTap('A', kShift));
  EXPECT_FALSE(StartsShiftTap(VK_CONTROL, kNoModifiers));
}

TEST(TipTranslationLogicTest, IsSelectionTranslationKey) {
  EXPECT_TRUE(IsSelectionTranslationKey(VK_CONVERT, kShift));
  // Henkan alone stays Mozc's reconversion.
  EXPECT_FALSE(IsSelectionTranslationKey(VK_CONVERT, kNoModifiers));
  EXPECT_FALSE(IsSelectionTranslationKey(VK_CONVERT, {true, true, false}));
  EXPECT_FALSE(IsSelectionTranslationKey(VK_CONVERT, {true, false, true}));
  EXPECT_FALSE(IsSelectionTranslationKey(VK_NONCONVERT, kShift));
  EXPECT_FALSE(IsSelectionTranslationKey(VK_SPACE, kShift));
}

TEST(TipTranslationLogicTest, ModifierKeysAreLeftToMozc) {
  EXPECT_EQ(Shown(VK_SHIFT), TranslationAction::kNone);
  EXPECT_EQ(Shown(VK_CONTROL), TranslationAction::kNone);
  EXPECT_EQ(Shown(VK_MENU), TranslationAction::kNone);
  EXPECT_EQ(Shown(VK_SHIFT, kControl), TranslationAction::kNone);
}

TEST(TipTranslationLogicTest, ControlOrAltCancels) {
  EXPECT_EQ(Shown('C', kControl), TranslationAction::kCancel);
  EXPECT_EQ(Shown(VK_DOWN, kControl), TranslationAction::kCancel);
  EXPECT_EQ(Shown(VK_RETURN, kAlt), TranslationAction::kCancel);
  EXPECT_EQ(Shown('1', kAlt), TranslationAction::kCancel);
}

TEST(TipTranslationLogicTest, MovesTheSelection) {
  EXPECT_EQ(Shown(VK_DOWN), TranslationAction::kNext);
  EXPECT_EQ(Shown(VK_SPACE), TranslationAction::kNext);
  EXPECT_EQ(Shown(VK_CONVERT), TranslationAction::kNext);
  EXPECT_EQ(Shown(VK_TAB), TranslationAction::kNext);
  EXPECT_EQ(Shown(VK_TAB, kShift), TranslationAction::kPrevious);
  EXPECT_EQ(Shown(VK_UP), TranslationAction::kPrevious);
  // Also while loading (no items yet).
  EXPECT_EQ(Shown(VK_DOWN, kNoModifiers, 0, 0), TranslationAction::kNext);
}

TEST(TipTranslationLogicTest, EnterCommitsTheSelectedItem) {
  int index = -1;
  EXPECT_EQ(Shown(VK_RETURN, kNoModifiers, 3, 2, &index),
            TranslationAction::kCommit);
  EXPECT_EQ(index, 2);
  // Nothing to commit yet: back to the conversion.
  EXPECT_EQ(Shown(VK_RETURN, kNoModifiers, 0, 0), TranslationAction::kCancel);
}

TEST(TipTranslationLogicTest, NumbersCommitThatItem) {
  int index = -1;
  EXPECT_EQ(Shown('1', kNoModifiers, 3, 2, &index), TranslationAction::kCommit);
  EXPECT_EQ(index, 0);
  EXPECT_EQ(Shown('3', kNoModifiers, 3, 0, &index), TranslationAction::kCommit);
  EXPECT_EQ(index, 2);
  EXPECT_EQ(Shown('9', kNoModifiers, 9, 0, &index), TranslationAction::kCommit);
  EXPECT_EQ(index, 8);
  // No such item, Shift+number (a symbol) or 0: the UI closes.
  EXPECT_EQ(Shown('4', kNoModifiers, 3, 0), TranslationAction::kCancel);
  EXPECT_EQ(Shown('1', kShift, 3, 0), TranslationAction::kCancel);
  EXPECT_EQ(Shown('0', kNoModifiers, 3, 0), TranslationAction::kCancel);
}

TEST(TipTranslationLogicTest, SelectionKeepsEnterAndNumbersWhileLoading) {
  // Translating a selection, Enter or a number pressed before the
  // translations arrive must not reach the application, which would replace
  // the selected text (a whole document after Ctrl+A) with it.
  int index = -1;
  EXPECT_EQ(DecideShownKeyDown(VK_RETURN, kNoModifiers, 0, 0, &index, true),
            TranslationAction::kWait);
  EXPECT_EQ(DecideShownKeyDown('1', kNoModifiers, 0, 0, &index, true),
            TranslationAction::kWait);
  EXPECT_EQ(DecideShownKeyDown('5', kNoModifiers, 2, 0, &index, true),
            TranslationAction::kWait);
  // Once they are there, the usual commit.
  EXPECT_EQ(DecideShownKeyDown(VK_RETURN, kNoModifiers, 1, 0, &index, true),
            TranslationAction::kCommit);
  EXPECT_EQ(DecideShownKeyDown('2', kNoModifiers, 2, 0, &index, true),
            TranslationAction::kCommit);
  EXPECT_EQ(index, 1);
  // Esc still cancels; typing still replaces the selection as usual.
  EXPECT_EQ(DecideShownKeyDown(VK_ESCAPE, kNoModifiers, 0, 0, &index, true),
            TranslationAction::kCancel);
  EXPECT_EQ(DecideShownKeyDown('A', kNoModifiers, 0, 0, &index, true),
            TranslationAction::kCancel);
}

TEST(TipTranslationLogicTest, OtherKeysCancel) {
  EXPECT_EQ(Shown(VK_ESCAPE), TranslationAction::kCancel);
  EXPECT_EQ(Shown(VK_BACK), TranslationAction::kCancel);
  EXPECT_EQ(Shown('A'), TranslationAction::kCancel);
  EXPECT_EQ(Shown(VK_LEFT), TranslationAction::kCancel);
  EXPECT_EQ(Shown(VK_NONCONVERT), TranslationAction::kCancel);
}

TEST(TipTranslationLogicTest, IndexIsTheSelectionUnlessANumber) {
  int index = -1;
  Shown(VK_DOWN, kNoModifiers, 3, 1, &index);
  EXPECT_EQ(index, 1);
  Shown('A', kNoModifiers, 3, 2, &index);
  EXPECT_EQ(index, 2);
}

TEST(TipTranslationLogicTest, PassesThroughAfterCancel) {
  // Esc and Back only close the translations.
  EXPECT_FALSE(PassesThroughAfterCancel(VK_ESCAPE, true));
  EXPECT_FALSE(PassesThroughAfterCancel(VK_BACK, false));
  // Enter with nothing to commit goes on to Mozc, which commits the conversion.
  EXPECT_TRUE(PassesThroughAfterCancel(VK_RETURN, false));
  EXPECT_FALSE(PassesThroughAfterCancel(VK_RETURN, true));
  // Typing goes on.
  EXPECT_TRUE(PassesThroughAfterCancel('A', true));
  EXPECT_TRUE(PassesThroughAfterCancel(VK_LEFT, false));
}

TEST(TipTranslationLogicTest, NextAndPreviousWrapAround) {
  EXPECT_EQ(NextIndex(0, 3), 1);
  EXPECT_EQ(NextIndex(2, 3), 0);
  EXPECT_EQ(PreviousIndex(1, 3), 0);
  EXPECT_EQ(PreviousIndex(0, 3), 2);
  EXPECT_EQ(NextIndex(0, 1), 0);
  EXPECT_EQ(PreviousIndex(0, 1), 0);
  EXPECT_EQ(NextIndex(0, 0), 0);
  EXPECT_EQ(PreviousIndex(0, 0), 0);
}

TEST(TipTranslationLogicTest, LanguageRank) {
  const std::vector<std::wstring> languages = {L"EN", L"ZH", L"KO"};
  EXPECT_EQ(LanguageRank(languages, L"EN"), 0u);
  EXPECT_EQ(LanguageRank(languages, L"KO"), 2u);
  EXPECT_EQ(LanguageRank(languages, L"FR"), 3u);
  EXPECT_EQ(LanguageRank({}, L"EN"), 0u);
}

TEST(TipTranslationLogicTest, InsertInLanguageOrder) {
  const std::vector<std::wstring> languages = {L"EN", L"ZH", L"KO", L"FR"};
  std::vector<TranslationItem> items;
  // Arriving in any order, they are shown in the configured order.
  InsertInLanguageOrder(languages, {Item(L"KO")}, &items);
  EXPECT_EQ(Languages(items), L"KO");
  InsertInLanguageOrder(languages, {Item(L"FR")}, &items);
  EXPECT_EQ(Languages(items), L"KO,FR");
  InsertInLanguageOrder(languages, {Item(L"EN")}, &items);
  EXPECT_EQ(Languages(items), L"EN,KO,FR");
  InsertInLanguageOrder(languages, {Item(L"ZH")}, &items);
  EXPECT_EQ(Languages(items), L"EN,ZH,KO,FR");
}

TEST(TipTranslationLogicTest, InsertInLanguageOrderSkipsFailures) {
  const std::vector<std::wstring> languages = {L"EN", L"ZH"};
  std::vector<TranslationItem> items;
  InsertInLanguageOrder(languages, {Item(L"EN", false), Item(L"ZH")}, &items);
  EXPECT_EQ(Languages(items), L"ZH");
}

TEST(TipTranslationLogicTest, InsertInLanguageOrderPutsUnknownLast) {
  const std::vector<std::wstring> languages = {L"EN", L"ZH"};
  std::vector<TranslationItem> items;
  InsertInLanguageOrder(languages, {Item(L"XX"), Item(L"ZH"), Item(L"EN")},
                        &items);
  EXPECT_EQ(Languages(items), L"EN,ZH,XX");
}

TEST(TipTranslationLogicTest, IndexOfLanguage) {
  const std::vector<TranslationItem> items = {Item(L"EN"), Item(L"ZH"),
                                              Item(L"KO")};
  EXPECT_EQ(IndexOfLanguage(items, L"ZH"), 1);
  EXPECT_EQ(IndexOfLanguage(items, L"KO"), 2);
  EXPECT_EQ(IndexOfLanguage(items, L"FR"), 0);
  EXPECT_EQ(IndexOfLanguage(items, L""), 0);
  EXPECT_EQ(IndexOfLanguage({}, L"EN"), 0);
}

TEST(TipTranslationLogicTest, FailedLanguages) {
  EXPECT_EQ(FailedLanguages({Item(L"EN"), Item(L"ZH")}), L"");
  EXPECT_EQ(FailedLanguages({Item(L"EN", false), Item(L"ZH"),
                             Item(L"KO", false)}),
            L"EN, KO");
  EXPECT_EQ(FailedLanguages({}), L"");
}

TEST(TipTranslationLogicTest, ProgressSuffix) {
  EXPECT_EQ(ProgressSuffix(2, 6), L" (2/6)");
  EXPECT_EQ(ProgressSuffix(0, 1), L" (0/1)");
}

TEST(TipTranslationLogicTest, IsSecureInputScope) {
  EXPECT_TRUE(IsSecureInputScope(IS_PASSWORD));
  EXPECT_TRUE(IsSecureInputScope(IS_NUMERIC_PASSWORD));
  EXPECT_TRUE(IsSecureInputScope(IS_NUMERIC_PIN));
  EXPECT_TRUE(IsSecureInputScope(IS_ALPHANUMERIC_PIN));
  EXPECT_TRUE(IsSecureInputScope(IS_ALPHANUMERIC_PIN_SET));
  EXPECT_FALSE(IsSecureInputScope(IS_DEFAULT));
  EXPECT_FALSE(IsSecureInputScope(IS_TEXT));
  EXPECT_FALSE(IsSecureInputScope(IS_EMAIL_SMTPEMAILADDRESS));
  EXPECT_FALSE(IsSecureInputScope(IS_NUMBER));
}

TEST(TipTranslationLogicTest, SelectionIsJapanese) {
  EXPECT_TRUE(SelectionIsJapanese(L"今日はいい天気です。"));
  EXPECT_TRUE(SelectionIsJapanese(L"東京"));
  EXPECT_TRUE(SelectionIsJapanese(L"ｶﾀｶﾅ"));
  // A few English words in Japanese.
  EXPECT_TRUE(SelectionIsJapanese(L"今日はGitHubでPull Requestを作った"));
  EXPECT_TRUE(SelectionIsJapanese(L"明日の会議はZoomで行います。"));

  EXPECT_FALSE(SelectionIsJapanese(L"Good morning."));
  EXPECT_FALSE(SelectionIsJapanese(L"안녕하세요"));
  EXPECT_FALSE(SelectionIsJapanese(L"Привет, мир"));
  EXPECT_FALSE(SelectionIsJapanese(L""));
  EXPECT_FALSE(SelectionIsJapanese(L"12345 !?"));
}

TEST(TipTranslationLogicTest, SelectedCodeWithAJapaneseCommentIsForeign) {
  // Select-all of a file: any kana used to make the whole text "Japanese",
  // so it went to the configured languages and Japanese was not offered.
  EXPECT_FALSE(SelectionIsJapanese(
      L"// 1. ユーザー情報を取得する\r"
      L"const user = await fetch(\"/api/user\");\r"
      L"console.log(\"Loaded the user profile\");\r"
      L"if (!user.ok) {\r"
      L"  console.error(\"Could not load the user\");\r"
      L"}"));
  EXPECT_FALSE(SelectionIsJapanese(
      L"The meeting is at 10. 会議は10時から。 Please bring your laptop and "
      L"the printed agenda."));
}

TEST(TipTranslationLogicTest, CandidatePreviewKeepsShortText) {
  EXPECT_EQ(CandidatePreview(L"Good morning.", 4, 100), L"Good morning.");
  EXPECT_EQ(CandidatePreview(L"a\r\nb\rc\nd", 4, 100), L"a\nb\nc\nd");
  EXPECT_EQ(CandidatePreview(L"", 4, 100), L"");
}

TEST(TipTranslationLogicTest, CandidatePreviewCutsLines) {
  EXPECT_EQ(CandidatePreview(L"1\n2\n3\n4\n5\n6", 4, 100),
            L"1\n2\n3\n4…");
  // Trailing line breaks are not lines.
  EXPECT_EQ(CandidatePreview(L"1\n2\n\n\r\n", 4, 100), L"1\n2");
  EXPECT_EQ(CandidatePreview(L"1\n\n3", 4, 100), L"1\n\n3");
}

TEST(TipTranslationLogicTest, CandidatePreviewCutsWideLines) {
  EXPECT_EQ(CandidatePreview(L"abcdefgh", 4, 5), L"abcde…");
  EXPECT_EQ(CandidatePreview(L"abcde", 4, 5), L"abcde");
  // Full-width characters take two columns.
  EXPECT_EQ(CandidatePreview(L"あいうえおかきく", 4, 6), L"あいう…");
  EXPECT_EQ(CandidatePreview(L"aあいう", 4, 6), L"aあい…");
  // A surrogate pair is never split.
  EXPECT_EQ(CandidatePreview(L"ab\U0001F600cd", 4, 3), L"ab…");
  EXPECT_EQ(CandidatePreview(L"ab\U0001F600cd", 4, 4), L"ab\U0001F600…");
}

}  // namespace
}  // namespace tsf
}  // namespace win32
}  // namespace mozc
