// Translation IME addition to Mozc. Distributed under the same terms as
// Mozc (BSD 3-Clause License, see LICENSE at the top of the repository).
//
// Offline tests of the shared translation module (ported from the original
// Translation IME's tests\UnitTests). The ones that touch the registry or the
// TPM (KeyProtection, saving the config) stay there, as they would change the
// user's settings.

#include <windows.h>
#include <winhttp.h>

#include <memory>
#include <string>
#include <vector>

#include "testing/gunit.h"
#include "win32/tip/translation/HttpTranslator.h"
#include "win32/tip/translation/Json.h"
#include "win32/tip/translation/TranslationCache.h"
#include "win32/tip/translation/TranslationConfig.h"

namespace {

bool Contains(const std::wstring& text, const std::wstring& part) {
  return text.find(part) != std::wstring::npos;
}

TEST(TranslationJsonTest, ParsesDeepLResponse) {
  std::wstring text;
  EXPECT_TRUE(HttpTranslator::ParseDeepLResponse(
      L"{\"translations\":[{\"detected_source_language\":\"JA\",\"text\":"
      L"\"He said \\\"hi\\\" \\u00e9 \\ud83d\\ude00\\nok\"}]}",
      &text));
  EXPECT_EQ(text, L"He said \"hi\" é \U0001F600\nok");
  EXPECT_FALSE(HttpTranslator::ParseDeepLResponse(
      L"{\"message\":\"Wrong endpoint\"}", &text));
}

TEST(TranslationJsonTest, ParsesGoogleResponse) {
  std::wstring text;
  EXPECT_TRUE(HttpTranslator::ParseGoogleResponse(
      L"{\n  \"data\": {\n    \"translations\": [\n      {\n        "
      L"\"translatedText\": \"It's \\\"fine\\\"\"\n      }\n    ]\n  }\n}\n",
      &text));
  EXPECT_EQ(text, L"It's \"fine\"");
  EXPECT_FALSE(HttpTranslator::ParseGoogleResponse(L"<html>", &text));
}

TEST(TranslationJsonTest, RejectsBrokenJson) {
  Json::Value value;
  EXPECT_FALSE(Json::Parse(L"{\"a\":", &value));
  EXPECT_FALSE(Json::Parse(L"{\"a\":1} trailing", &value));
  EXPECT_FALSE(Json::Parse(std::wstring(10000, L'['), &value));
  ASSERT_TRUE(Json::Parse(L" [1, -2.5e3, true, false, null, \"x\", {}] ", &value));
  EXPECT_EQ(value.arrayValue.size(), 7u);
}

TEST(TranslationJsonTest, QuoteRoundTrip) {
  const std::wstring tricky = L"改行\n\"引用\" \\ タブ\t制御\x01 終わり";
  Json::Value value;
  ASSERT_TRUE(Json::Parse(Json::Quote(tricky), &value));
  EXPECT_TRUE(value.IsString());
  EXPECT_EQ(value.stringValue, tricky);
}

TEST(TranslationErrorTest, DescribesHttpErrors) {
  std::wstring error = HttpTranslator::DescribeHttpError(
      400, L"{\"message\":\"Value for 'target_lang' not supported.\"}");
  EXPECT_TRUE(Contains(error, L"target_lang")) << error;
  EXPECT_TRUE(Contains(error, L"400")) << error;
  error = HttpTranslator::DescribeHttpError(403, L"");
  EXPECT_TRUE(Contains(error, L"403")) << error;
  error = HttpTranslator::DescribeHttpError(
      403,
      L"{\"error\":{\"code\":403,\"details\":[{\"reason\":\"API_KEY_SERVICE_"
      L"BLOCKED\"}]}}");
  EXPECT_TRUE(Contains(error, L"許可されていません")) << error;
}

TEST(TranslationErrorTest, RetriesOnlyTransientNetworkErrors) {
  // A connection cut mid-response (seen as 12152 in Notepad) is retried;
  // "no network" and timeouts are not.
  EXPECT_TRUE(HttpTranslator::IsTransientNetworkError(
      ERROR_WINHTTP_INVALID_SERVER_RESPONSE));
  EXPECT_TRUE(
      HttpTranslator::IsTransientNetworkError(ERROR_WINHTTP_CONNECTION_ERROR));
  EXPECT_FALSE(
      HttpTranslator::IsTransientNetworkError(ERROR_WINHTTP_NAME_NOT_RESOLVED));
  EXPECT_FALSE(HttpTranslator::IsTransientNetworkError(ERROR_WINHTTP_TIMEOUT));
  const std::wstring error = HttpTranslator::DescribeNetworkError(
      ERROR_WINHTTP_INVALID_SERVER_RESPONSE);
  EXPECT_TRUE(Contains(error, L"途中で切れました")) << error;
  EXPECT_TRUE(Contains(error, L"12152")) << error;
}

TEST(TranslationRequestTest, BuildsDeepLRequest) {
  Json::Value value;
  ASSERT_TRUE(Json::Parse(
      HttpTranslator::BuildDeepLRequest(L"テスト\"です", L"EN"), &value));
  const Json::Value* target = value.Get(L"target_lang");
  const Json::Value* source = value.Get(L"source_lang");
  const Json::Value* texts = value.Get(L"text");
  ASSERT_NE(target, nullptr);
  EXPECT_EQ(target->stringValue, L"EN-US");
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(source->stringValue, L"JA");
  ASSERT_NE(texts, nullptr);
  ASSERT_NE(texts->At(0), nullptr);
  EXPECT_EQ(texts->At(0)->stringValue, L"テスト\"です");
}

TEST(TranslationRequestTest, BuildsGoogleRequest) {
  Json::Value value;
  ASSERT_TRUE(
      Json::Parse(HttpTranslator::BuildGoogleRequest(L"テスト", L"ZH"), &value));
  ASSERT_NE(value.Get(L"target"), nullptr);
  EXPECT_EQ(value.Get(L"target")->stringValue, L"zh-CN");
  ASSERT_NE(value.Get(L"format"), nullptr);
  EXPECT_EQ(value.Get(L"format")->stringValue, L"text");
}

TEST(TranslationRequestTest, LanguageCodes) {
  EXPECT_EQ(HttpTranslator::DeepLTargetCode(L"ZH"), L"ZH-HANS");
  EXPECT_EQ(HttpTranslator::DeepLTargetCode(L"KO"), L"KO");
  EXPECT_EQ(HttpTranslator::GoogleTargetCode(L"KO"), L"ko");
  EXPECT_EQ(HttpTranslator::GoogleTargetCode(L"ZH-TW"), L"zh-TW");
}

TEST(TranslationRequestTest, AutoDetect) {
  Json::Value value;
  ASSERT_TRUE(Json::Parse(
      HttpTranslator::BuildDeepLRequest(L"Hello", L"JA", L""), &value));
  EXPECT_EQ(value.Get(L"source_lang"), nullptr);
  ASSERT_NE(value.Get(L"target_lang"), nullptr);
  EXPECT_EQ(value.Get(L"target_lang")->stringValue, L"JA");

  // The source language has no region.
  ASSERT_TRUE(Json::Parse(
      HttpTranslator::BuildDeepLRequest(L"Hello", L"JA", L"EN-US"), &value));
  ASSERT_NE(value.Get(L"source_lang"), nullptr);
  EXPECT_EQ(value.Get(L"source_lang")->stringValue, L"EN");

  ASSERT_TRUE(Json::Parse(
      HttpTranslator::BuildGoogleRequest(L"Hello", L"JA", L""), &value));
  EXPECT_EQ(value.Get(L"source"), nullptr);
}

TEST(TranslationRequestTest, DetectedLanguage) {
  std::wstring text;
  std::wstring detected;
  EXPECT_TRUE(HttpTranslator::ParseDeepLResponse(
      L"{\"translations\":[{\"detected_source_language\":\"EN\",\"text\":"
      L"\"こんにちは\"}]}",
      &text, &detected));
  EXPECT_EQ(detected, L"EN");
  EXPECT_TRUE(HttpTranslator::ParseGoogleResponse(
      L"{\"data\":{\"translations\":[{\"translatedText\":\"x\","
      L"\"detectedSourceLanguage\":\"en\"}]}}",
      &text, &detected));
  EXPECT_EQ(detected, L"EN");   // upper-cased
}

TEST(TranslationRequestTest, DeepLUsage) {
  DeepLUsage usage;
  ASSERT_TRUE(HttpTranslator::ParseDeepLUsage(
      L"{\"character_count\":180118,\"character_limit\":500000}", &usage));
  EXPECT_EQ(usage.characterCount, 180118);
  EXPECT_EQ(usage.characterLimit, 500000);
  EXPECT_FALSE(
      HttpTranslator::ParseDeepLUsage(L"{\"message\":\"Forbidden\"}", &usage));
}

TEST(TranslationRequestTest, LooksJapanese) {
  EXPECT_TRUE(HttpTranslator::LooksJapanese(L"今日はいい天気"));
  EXPECT_TRUE(HttpTranslator::LooksJapanese(L"ｶﾀｶﾅ"));
  EXPECT_FALSE(HttpTranslator::LooksJapanese(L"The weather is nice"));
  EXPECT_FALSE(HttpTranslator::LooksJapanese(L"안녕하세요"));
}

TEST(TranslationConfigTest, ParsesLanguageList) {
  std::vector<std::wstring> languages =
      TranslationConfig::ParseLanguageList(L" en, zh ,,ko ");
  ASSERT_EQ(languages.size(), 3u);
  EXPECT_EQ(languages[0], L"EN");
  EXPECT_EQ(languages[1], L"ZH");
  EXPECT_EQ(languages[2], L"KO");
  languages = TranslationConfig::ParseLanguageList(L"");
  ASSERT_EQ(languages.size(), 6u);
  EXPECT_EQ(languages[0], L"EN");
}

TEST(TranslationConfigTest, DefaultHasNoEngine) {
  TranslationConfig config;
  EXPECT_EQ(config.engine, TranslationEngine::None);
  EXPECT_TRUE(config.ActiveApiKey().empty());
}

TEST(TranslationCacheTest, KeyDependsOnTheSourceLanguage) {
  TranslationConfig config;
  config.engine = TranslationEngine::DeepL;
  config.targetLanguages = {L"EN"};
  EXPECT_NE(CTranslationCache::MakeKey(config, L"JA", L"x"),
            CTranslationCache::MakeKey(config, L"", L"x"));
}

// The mailbox wakes this window when results arrive.
constexpr UINT kTestResultsMessage = WM_APP + 1;
bool g_woken = false;

LRESULT CALLBACK TestWindowProc(HWND window, UINT message, WPARAM wparam,
                                LPARAM lparam) {
  if (message == kTestResultsMessage) {
    g_woken = true;
    return 0;
  }
  return ::DefWindowProcW(window, message, wparam, lparam);
}

HWND CreateTestWindow() {
  WNDCLASSEXW window_class = {sizeof(window_class)};
  window_class.lpfnWndProc = TestWindowProc;
  window_class.hInstance = ::GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"MozcTranslationTest";
  ::RegisterClassExW(&window_class);
  return ::CreateWindowExW(0, window_class.lpszClassName, L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, window_class.hInstance,
                           nullptr);
}

// Pumps messages until the mailbox wakes the window or the timeout passes.
std::vector<std::unique_ptr<TranslationResult>> WaitForResults(
    const std::shared_ptr<CTranslationMailbox>& mailbox, DWORD timeout_ms) {
  g_woken = false;
  const ULONGLONG start = ::GetTickCount64();
  while (!g_woken && ::GetTickCount64() - start < timeout_ms) {
    MSG message;
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      ::DispatchMessageW(&message);
    }
    ::Sleep(10);
  }
  return mailbox->TakeAll();
}

TEST(TranslationAsyncTest, NoApiKeyGivesAnErrorResult) {
  HWND window = CreateTestWindow();
  ASSERT_NE(window, nullptr);
  auto mailbox =
      std::make_shared<CTranslationMailbox>(window, kTestResultsMessage);

  // No API key: the worker finishes at once with an error, also when
  // progressive (there is no language to report early).
  TranslationConfig config;
  ASSERT_TRUE(HttpTranslator::TranslateAsync(
      config, L"テスト", HttpTranslator::Japanese, 42, mailbox,
      /*progressive=*/true));
  auto results = WaitForResults(mailbox, 5000);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0]->requestId, 42u);
  EXPECT_TRUE(results[0]->complete);
  EXPECT_FALSE(results[0]->AnySucceeded());
  EXPECT_FALSE(results[0]->FirstError().empty());

  // Results posted after Close() are dropped without touching the window.
  mailbox->Close();
  ASSERT_TRUE(HttpTranslator::TranslateAsync(
      config, L"テスト", HttpTranslator::Japanese, 43, mailbox));
  ::Sleep(500);
  EXPECT_TRUE(mailbox->TakeAll().empty());

  ::DestroyWindow(window);
}

}  // namespace
