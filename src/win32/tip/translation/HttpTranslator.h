// Translation through the DeepL / Google Cloud Translation REST APIs.
//
// Independent of TSF: worker threads never touch IME objects. Results are handed to
// the UI thread through CTranslationMailbox, which wakes the UI thread with a posted
// message and keeps working (and freeing memory) even after the UI side is closed.

#pragma once

#include <windows.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "TranslationConfig.h"

struct TranslationItem
{
    std::wstring language;  // e.g. L"EN"
    std::wstring text;      // translated text (when succeeded)
    std::wstring detectedSourceLanguage;  // e.g. L"EN" when the source language was auto-detected
    std::wstring error;     // short user-facing message (when failed)
    bool succeeded = false;
};

struct TranslationResult
{
    UINT requestId = 0;
    std::wstring sourceText;
    std::wstring sourceLanguage;    // as requested; empty = auto-detect
    std::vector<TranslationItem> items;
    std::wstring error;     // set when the request could not be made at all

    bool AllSucceeded() const;
    bool AnySucceeded() const;
    std::wstring FirstError() const;
};

class CTranslationMailbox
{
public:
    CTranslationMailbox(HWND notifyWnd, UINT notifyMessage);

    // Any thread. Dropped silently once Close() has been called.
    void Post(std::unique_ptr<TranslationResult> result);

    // UI thread.
    std::vector<std::unique_ptr<TranslationResult>> TakeAll();

    // UI thread, before the notify window is destroyed.
    void Close();

private:
    std::mutex _lock;
    HWND _notifyWnd;
    UINT _notifyMessage;
    std::vector<std::unique_ptr<TranslationResult>> _results;
};

struct DeepLUsage
{
    unsigned long long characterCount = 0;
    unsigned long long characterLimit = 0;
};

namespace HttpTranslator
{
    // Source language used for text typed with the IME.
    extern const wchar_t* const Japanese;

    // Blocking. Translates text into every language in config.targetLanguages, in parallel.
    // sourceLanguage empty = let the service detect it. Never call this on a UI thread.
    TranslationResult Translate(const TranslationConfig& config, const std::wstring& text,
                                const std::wstring& sourceLanguage = Japanese);

    // Runs Translate() on a worker thread and posts the result to mailbox.
    // Returns false if the work could not be started.
    bool TranslateAsync(const TranslationConfig& config, const std::wstring& text, const std::wstring& sourceLanguage,
                        UINT requestId, const std::shared_ptr<CTranslationMailbox>& mailbox);

    // Blocking. Characters used / allowed in the current DeepL billing period.
    bool GetDeepLUsage(const TranslationConfig& config, DeepLUsage* usage, std::wstring* error);

    // True when the text contains kana or kanji (i.e. is most likely Japanese).
    bool LooksJapanese(const std::wstring& text);

    // Exposed for unit tests.
    std::wstring DeepLTargetCode(const std::wstring& language);
    std::wstring GoogleTargetCode(const std::wstring& language);
    std::wstring BuildDeepLRequest(const std::wstring& text, const std::wstring& language, const std::wstring& sourceLanguage = Japanese);
    std::wstring BuildGoogleRequest(const std::wstring& text, const std::wstring& language, const std::wstring& sourceLanguage = Japanese);
    bool ParseDeepLResponse(const std::wstring& body, std::wstring* text, std::wstring* detectedSourceLanguage = nullptr);
    bool ParseGoogleResponse(const std::wstring& body, std::wstring* text, std::wstring* detectedSourceLanguage = nullptr);
    bool ParseDeepLUsage(const std::wstring& body, DeepLUsage* usage);
    std::wstring DescribeHttpError(DWORD status, const std::wstring& body);
    std::wstring DescribeNetworkError(DWORD error);
    // WinHTTP errors worth retrying (the connection broke, not "no network").
    bool IsTransientNetworkError(DWORD error);
}
