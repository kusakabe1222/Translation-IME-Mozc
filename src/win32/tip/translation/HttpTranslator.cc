// Translation through the DeepL / Google Cloud Translation REST APIs.

#include "HttpTranslator.h"

#include <winhttp.h>
#include <atomic>
#include <thread>

#include "Json.h"
#include "UiText.h"

#pragma comment(lib, "winhttp.lib")

namespace {

const size_t kMaxSourceChars = 5000;
const size_t kMaxResponseBytes = 1024 * 1024;
const size_t kMaxErrorDetailChars = 60;
const LONG kMaxConcurrentJobs = 4;
const int kMaxRetries = 2;
const DWORD kRetryBaseDelayMs = 400;

std::atomic<LONG> g_runningJobs(0);

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return std::string();
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size > 0 ? size : 0, '\0');
    if (size > 0)
    {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &result[0], size, nullptr, nullptr);
    }
    return result;
}

std::wstring FromUtf8(const std::string& text)
{
    if (text.empty())
    {
        return std::wstring();
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size > 0 ? size : 0, L'\0');
    if (size > 0)
    {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &result[0], size);
    }
    return result;
}

std::wstring ToLower(std::wstring text)
{
    for (wchar_t& ch : text)
    {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return text;
}

bool EndsWith(const std::wstring& text, const std::wstring& suffix)
{
    return text.length() >= suffix.length() &&
        text.compare(text.length() - suffix.length(), suffix.length(), suffix) == 0;
}

// API keys go into an HTTP header, so anything that could break the header is rejected.
bool IsValidApiKey(const std::wstring& key)
{
    if (key.empty())
    {
        return false;
    }
    for (wchar_t ch : key)
    {
        if (ch < 0x21 || ch > 0x7E)
        {
            return false;
        }
    }
    return true;
}

struct HttpResponse
{
    DWORD status = 0;
    DWORD error = 0;    // WinHTTP / Win32 error when the request itself failed
    std::string body;
};

HttpResponse SendRequest(HINTERNET session, const wchar_t* method, const wchar_t* host, const wchar_t* path,
                         const std::wstring& headers, const std::string& body)
{
    HttpResponse response;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;

    connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect)
    {
        response.error = GetLastError();
        goto Exit;
    }

    request = WinHttpOpenRequest(connect, method, path, nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request)
    {
        response.error = GetLastError();
        goto Exit;
    }

    if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1L),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request, nullptr))
    {
        response.error = GetLastError();
        goto Exit;
    }

    {
        DWORD status = 0;
        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        {
            response.error = GetLastError();
            goto Exit;
        }
        response.status = status;
    }

    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            response.error = GetLastError();
            break;
        }
        if (available == 0)
        {
            break;
        }
        if (response.body.size() + available > kMaxResponseBytes)
        {
            response.error = ERROR_INSUFFICIENT_BUFFER;
            break;
        }

        size_t offset = response.body.size();
        response.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, &response.body[offset], available, &read))
        {
            response.error = GetLastError();
            break;
        }
        response.body.resize(offset + read);
    }

Exit:
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    return response;
}

std::wstring ExtractApiErrorMessage(const std::wstring& body)
{
    Json::Value root;
    if (!Json::Parse(body, &root))
    {
        return std::wstring();
    }

    // DeepL: {"message": "..."}   Google: {"error": {"message": "..."}}
    const Json::Value* message = root.Get(L"message");
    if (!message)
    {
        const Json::Value* error = root.Get(L"error");
        message = error ? error->Get(L"message") : nullptr;
    }
    if (!message || !message->IsString())
    {
        return std::wstring();
    }

    std::wstring text = message->stringValue;
    if (text.length() > kMaxErrorDetailChars)
    {
        text = text.substr(0, kMaxErrorDetailChars) + L"…";
    }
    return text;
}

std::wstring DeepLHost(const std::wstring& apiKey)
{
    return EndsWith(apiKey, L":fx") ? L"api-free.deepl.com" : L"api.deepl.com";
}

TranslationItem TranslateOne(HINTERNET session, const TranslationConfig& config, const std::wstring& text,
                             const std::wstring& language, const std::wstring& sourceLanguage)
{
    TranslationItem item;
    item.language = language;

    const std::wstring& apiKey = config.ActiveApiKey();
    if (!IsValidApiKey(apiKey))
    {
        item.error = UiText::Pick(L"APIキーの形式が正しくありません", L"The API key is not in the right format");
        return item;
    }

    std::wstring host;
    std::wstring path;
    std::wstring headers;
    std::wstring requestBody;

    if (config.engine == TranslationEngine::DeepL)
    {
        host = DeepLHost(apiKey);
        path = L"/v2/translate";
        headers = L"Authorization: DeepL-Auth-Key " + apiKey + L"\r\nContent-Type: application/json\r\n";
        requestBody = HttpTranslator::BuildDeepLRequest(text, language, sourceLanguage);
    }
    else
    {
        host = L"translation.googleapis.com";
        path = L"/language/translate/v2";
        headers = L"X-Goog-Api-Key: " + apiKey + L"\r\nContent-Type: application/json; charset=utf-8\r\n";
        requestBody = HttpTranslator::BuildGoogleRequest(text, language, sourceLanguage);
    }

    // Rate limits (429), server errors and dropped connections are usually transient:
    // back off and retry.
    const std::string utf8Body = ToUtf8(requestBody);
    HttpResponse response;
    for (int attempt = 0; ; attempt++)
    {
        response = SendRequest(session, L"POST", host.c_str(), path.c_str(), headers, utf8Body);
        bool retryable = (response.error == 0)
            ? (response.status == 429 || response.status >= 500)
            : HttpTranslator::IsTransientNetworkError(response.error);
        if (!retryable || attempt >= kMaxRetries)
        {
            break;
        }
        Sleep(kRetryBaseDelayMs << (attempt * 2));   // 400 ms, 1600 ms
    }
    if (response.error != 0)
    {
        item.error = HttpTranslator::DescribeNetworkError(response.error);
        return item;
    }

    std::wstring responseBody = FromUtf8(response.body);
    if (response.status != 200)
    {
        item.error = HttpTranslator::DescribeHttpError(response.status, responseBody);
        return item;
    }

    bool parsed = (config.engine == TranslationEngine::DeepL)
        ? HttpTranslator::ParseDeepLResponse(responseBody, &item.text, &item.detectedSourceLanguage)
        : HttpTranslator::ParseGoogleResponse(responseBody, &item.text, &item.detectedSourceLanguage);
    if (!parsed || item.text.empty())
    {
        item.text.clear();
        item.error = UiText::Pick(L"応答を解析できませんでした", L"Could not read the response");
        return item;
    }

    item.succeeded = true;
    return item;
}

struct AsyncJob
{
    TranslationConfig config;
    std::wstring text;
    std::wstring sourceLanguage;
    UINT requestId = 0;
    std::shared_ptr<CTranslationMailbox> mailbox;
    bool progressive = false;
    HMODULE module = nullptr;
};

DWORD WINAPI TranslationThreadProc(LPVOID param)
{
    HMODULE module = nullptr;
    {
        std::unique_ptr<AsyncJob> job(static_cast<AsyncJob*>(param));
        module = job->module;
        try
        {
            std::function<void(const TranslationItem&)> onItem;
            if (job->progressive)
            {
                const AsyncJob* early = job.get();
                onItem = [early](const TranslationItem& item) {
                    std::unique_ptr<TranslationResult> partial(new TranslationResult());
                    partial->requestId = early->requestId;
                    partial->sourceText = early->text;
                    partial->sourceLanguage = early->sourceLanguage;
                    partial->items.push_back(item);
                    partial->complete = false;
                    early->mailbox->Post(std::move(partial));
                };
            }
            std::unique_ptr<TranslationResult> result(new TranslationResult(
                HttpTranslator::Translate(job->config, job->text, job->sourceLanguage, onItem)));
            result->requestId = job->requestId;
            job->mailbox->Post(std::move(result));
        }
        catch (...)
        {
            // Never let an exception escape into the host application, but still answer
            // so the IME does not wait for this request forever.
            try
            {
                std::unique_ptr<TranslationResult> failed(new TranslationResult());
                failed->requestId = job->requestId;
                failed->sourceText = job->text;
                failed->sourceLanguage = job->sourceLanguage;
                failed->error = UiText::Pick(L"翻訳中にエラーが発生しました", L"An error occurred while translating");
                job->mailbox->Post(std::move(failed));
            }
            catch (...)
            {
            }
        }
    }
    g_runningJobs--;

    // The module reference taken in TranslateAsync keeps this DLL loaded until the
    // thread is completely done with its code.
    FreeLibraryAndExitThread(module, 0);
}

} // namespace

//+---------------------------------------------------------------------------
// TranslationResult
//----------------------------------------------------------------------------

bool TranslationResult::AllSucceeded() const
{
    if (!error.empty() || items.empty())
    {
        return false;
    }
    for (const TranslationItem& item : items)
    {
        if (!item.succeeded) return false;
    }
    return true;
}

bool TranslationResult::AnySucceeded() const
{
    for (const TranslationItem& item : items)
    {
        if (item.succeeded) return true;
    }
    return false;
}

std::wstring TranslationResult::FirstError() const
{
    if (!error.empty())
    {
        return error;
    }
    for (const TranslationItem& item : items)
    {
        if (!item.succeeded && !item.error.empty()) return item.error;
    }
    return UiText::Pick(L"翻訳に失敗しました", L"Translation failed");
}

//+---------------------------------------------------------------------------
// CTranslationMailbox
//----------------------------------------------------------------------------

CTranslationMailbox::CTranslationMailbox(HWND notifyWnd, UINT notifyMessage)
    : _notifyWnd(notifyWnd), _notifyMessage(notifyMessage)
{
}

void CTranslationMailbox::Post(std::unique_ptr<TranslationResult> result)
{
    std::lock_guard<std::mutex> guard(_lock);
    if (!_notifyWnd)
    {
        return;
    }
    _results.push_back(std::move(result));
    PostMessage(_notifyWnd, _notifyMessage, 0, 0);
}

std::vector<std::unique_ptr<TranslationResult>> CTranslationMailbox::TakeAll()
{
    std::lock_guard<std::mutex> guard(_lock);
    std::vector<std::unique_ptr<TranslationResult>> results;
    results.swap(_results);
    return results;
}

void CTranslationMailbox::Close()
{
    std::lock_guard<std::mutex> guard(_lock);
    _notifyWnd = nullptr;
    _results.clear();
}

//+---------------------------------------------------------------------------
// HttpTranslator
//----------------------------------------------------------------------------

namespace HttpTranslator {

const wchar_t* const Japanese = L"JA";

std::wstring DeepLTargetCode(const std::wstring& language)
{
    if (language == L"EN") return L"EN-US";
    if (language == L"PT") return L"PT-BR";
    if (language == L"ZH" || language == L"ZH-CN") return L"ZH-HANS";
    if (language == L"ZH-TW") return L"ZH-HANT";
    return language;
}

std::wstring GoogleTargetCode(const std::wstring& language)
{
    if (language == L"ZH" || language == L"ZH-HANS" || language == L"ZH-CN") return L"zh-CN";
    if (language == L"ZH-HANT" || language == L"ZH-TW") return L"zh-TW";
    if (language == L"EN-US" || language == L"EN-GB") return L"en";
    if (language == L"PT-BR" || language == L"PT-PT") return L"pt";
    return ToLower(language);
}

std::wstring BuildDeepLRequest(const std::wstring& text, const std::wstring& language, const std::wstring& sourceLanguage)
{
    // DeepL source codes have no region (EN, not EN-US); omitting source_lang means auto-detect.
    std::wstring source;
    if (!sourceLanguage.empty())
    {
        source = L",\"source_lang\":" + Json::Quote(sourceLanguage.substr(0, sourceLanguage.find(L'-')));
    }
    return L"{\"text\":[" + Json::Quote(text) + L"]" + source + L",\"target_lang\":" +
        Json::Quote(DeepLTargetCode(language)) + L"}";
}

std::wstring BuildGoogleRequest(const std::wstring& text, const std::wstring& language, const std::wstring& sourceLanguage)
{
    std::wstring source;
    if (!sourceLanguage.empty())
    {
        source = L",\"source\":" + Json::Quote(GoogleTargetCode(sourceLanguage));
    }
    return L"{\"q\":[" + Json::Quote(text) + L"]" + source + L",\"target\":" +
        Json::Quote(GoogleTargetCode(language)) + L",\"format\":\"text\"}";
}

bool ParseDeepLResponse(const std::wstring& body, std::wstring* text, std::wstring* detectedSourceLanguage)
{
    // {"translations":[{"detected_source_language":"JA","text":"..."}]}
    Json::Value root;
    if (!Json::Parse(body, &root))
    {
        return false;
    }
    const Json::Value* translations = root.Get(L"translations");
    const Json::Value* first = translations ? translations->At(0) : nullptr;
    const Json::Value* value = first ? first->Get(L"text") : nullptr;
    if (!value || !value->IsString())
    {
        return false;
    }
    *text = value->stringValue;

    const Json::Value* detected = first->Get(L"detected_source_language");
    if (detectedSourceLanguage && detected && detected->IsString())
    {
        *detectedSourceLanguage = detected->stringValue;
    }
    return true;
}

bool ParseGoogleResponse(const std::wstring& body, std::wstring* text, std::wstring* detectedSourceLanguage)
{
    // {"data":{"translations":[{"translatedText":"..."}]}}
    Json::Value root;
    if (!Json::Parse(body, &root))
    {
        return false;
    }
    const Json::Value* data = root.Get(L"data");
    const Json::Value* translations = data ? data->Get(L"translations") : nullptr;
    const Json::Value* first = translations ? translations->At(0) : nullptr;
    const Json::Value* value = first ? first->Get(L"translatedText") : nullptr;
    if (!value || !value->IsString())
    {
        return false;
    }
    *text = value->stringValue;

    const Json::Value* detected = first->Get(L"detectedSourceLanguage");
    if (detectedSourceLanguage && detected && detected->IsString())
    {
        std::wstring code = detected->stringValue;
        for (wchar_t& ch : code)
        {
            if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
        }
        *detectedSourceLanguage = code;
    }
    return true;
}

bool ParseDeepLUsage(const std::wstring& body, DeepLUsage* usage)
{
    // {"character_count":180118,"character_limit":500000}
    Json::Value root;
    if (!Json::Parse(body, &root))
    {
        return false;
    }
    const Json::Value* count = root.Get(L"character_count");
    const Json::Value* limit = root.Get(L"character_limit");
    if (!count || !limit || count->type != Json::Type::Number || limit->type != Json::Type::Number)
    {
        return false;
    }
    usage->characterCount = static_cast<unsigned long long>(count->numberValue);
    usage->characterLimit = static_cast<unsigned long long>(limit->numberValue);
    return true;
}

bool LooksJapanese(const std::wstring& text)
{
    for (wchar_t ch : text)
    {
        if ((ch >= 0x3040 && ch <= 0x30FF) ||   // hiragana, katakana
            (ch >= 0x4E00 && ch <= 0x9FFF) ||   // CJK ideographs
            (ch >= 0xFF66 && ch <= 0xFF9F))     // half-width katakana
        {
            return true;
        }
    }
    return false;
}

std::wstring DescribeHttpError(DWORD status, const std::wstring& body)
{
    std::wstring message;
    switch (status)
    {
    case 401:
    case 403:
        // Google reports why the key was refused in error.details[].reason.
        if (body.find(L"API_KEY_SERVICE_BLOCKED") != std::wstring::npos)
        {
            message = UiText::Pick(L"このキーでは Cloud Translation API が許可されていません (Google Cloud Console でキーの「API の制限」に追加してください)", L"This key may not use the Cloud Translation API (add it under \"API restrictions\" for the key in the Google Cloud Console)");
        }
        else if (body.find(L"SERVICE_DISABLED") != std::wstring::npos)
        {
            message = UiText::Pick(L"Cloud Translation API が有効になっていません", L"The Cloud Translation API is not enabled");
        }
        else
        {
            message = UiText::Pick(L"認証エラー: APIキーを確認してください", L"Authentication failed: check the API key");
        }
        break;
    case 429: message = UiText::Pick(L"リクエストが多すぎます", L"Too many requests"); break;
    case 456: message = UiText::Pick(L"DeepLの利用上限に達しました", L"The DeepL usage limit has been reached"); break;
    default:
        if (status >= 500)
        {
            message = UiText::Pick(L"サーバーエラー", L"Server error");
        }
        else
        {
            message = ExtractApiErrorMessage(body);
            if (message.empty())
            {
                message = UiText::Pick(L"リクエストエラー", L"Request error");
            }
        }
        break;
    }
    // No-break space: a wrapped message keeps "HTTP 403" on one line.
    return message + L" (HTTP " + std::to_wstring(status) + L")";
}

bool IsTransientNetworkError(DWORD error)
{
    switch (error)
    {
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:   // 12152: e.g. the connection was cut mid-response
    case ERROR_WINHTTP_CONNECTION_ERROR:          // 12030: connection reset
    case ERROR_WINHTTP_RESEND_REQUEST:            // 12032
        return true;
    default:
        return false;
    }
}

std::wstring DescribeNetworkError(DWORD error)
{
    switch (error)
    {
    case ERROR_WINHTTP_TIMEOUT:
        return UiText::Pick(L"タイムアウトしました", L"Timed out");
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return UiText::Pick(L"サーバーに接続できません (", L"Cannot connect to the server (") + std::to_wstring(error) + L")";
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
        return UiText::Pick(L"サーバーとの通信が途中で切れました (", L"The connection to the server was cut (") + std::to_wstring(error) + L")";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return UiText::Pick(L"安全な接続を確立できません", L"Cannot establish a secure connection");
    case ERROR_ACCESS_DENIED:
        return UiText::Pick(L"このアプリからは通信できません", L"This application may not connect to the internet");
    default:
        return UiText::Pick(L"通信エラー (", L"Network error (") + std::to_wstring(error) + L")";
    }
}

HINTERNET OpenSession(const TranslationConfig& config)
{
    HINTERNET session = WinHttpOpen(L"TranslationIME/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1 or later.
        session = WinHttpOpen(L"TranslationIME/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (session)
    {
        const int timeout = static_cast<int>(config.timeoutMs);
        WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);
    }
    return session;
}

bool GetDeepLUsage(const TranslationConfig& config, DeepLUsage* usage, std::wstring* error)
{
    if (!IsValidApiKey(config.deeplApiKey))
    {
        *error = UiText::Pick(L"DeepL の API キーが未設定です", L"No DeepL API key is set");
        return false;
    }

    HINTERNET session = OpenSession(config);
    if (!session)
    {
        *error = DescribeNetworkError(GetLastError());
        return false;
    }
    HttpResponse response = SendRequest(session, L"GET", DeepLHost(config.deeplApiKey).c_str(), L"/v2/usage",
                                        L"Authorization: DeepL-Auth-Key " + config.deeplApiKey + L"\r\n", std::string());
    WinHttpCloseHandle(session);

    if (response.error != 0)
    {
        *error = DescribeNetworkError(response.error);
        return false;
    }
    std::wstring body = FromUtf8(response.body);
    if (response.status != 200)
    {
        *error = DescribeHttpError(response.status, body);
        return false;
    }
    if (!ParseDeepLUsage(body, usage))
    {
        *error = UiText::Pick(L"応答を解析できませんでした", L"Could not read the response");
        return false;
    }
    return true;
}

TranslationResult Translate(const TranslationConfig& config, const std::wstring& text, const std::wstring& sourceLanguage,
                            const std::function<void(const TranslationItem&)>& onItem)
{
    TranslationResult result;
    result.sourceText = text;
    result.sourceLanguage = sourceLanguage;

    if (config.engine == TranslationEngine::None)
    {
        result.error = UiText::Pick(L"APIキーが未設定です", L"No API key is set");
        return result;
    }
    if (text.empty())
    {
        result.error = UiText::Pick(L"翻訳する文字がありません", L"Nothing to translate");
        return result;
    }
    if (text.length() > kMaxSourceChars)
    {
        result.error = UiText::Pick(L"文字数が多すぎます (5,000 文字まで)", L"The text is too long (5,000 characters at most)");
        return result;
    }
    if (config.targetLanguages.empty())
    {
        result.error = UiText::Pick(L"翻訳先の言語が設定されていません", L"No target language is set");
        return result;
    }

    HINTERNET session = OpenSession(config);
    if (!session)
    {
        result.error = DescribeNetworkError(GetLastError());
        return result;
    }

    const size_t count = config.targetLanguages.size();
    result.items.resize(count);

    // One request per language, run in parallel. A session handle may be shared between threads.
    std::vector<std::thread> workers;
    for (size_t i = 0; i < count; i++)
    {
        auto work = [&result, &config, &text, &sourceLanguage, &onItem, session, i]() {
            result.items[i] = TranslateOne(session, config, text, config.targetLanguages[i], sourceLanguage);
            if (onItem)
            {
                try
                {
                    onItem(result.items[i]);
                }
                catch (...)
                {
                    // An early result is only a preview: the complete result still follows.
                }
            }
        };
        try
        {
            workers.emplace_back(work);
        }
        catch (...)
        {
            work();
        }
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }

    WinHttpCloseHandle(session);
    return result;
}

bool TranslateAsync(const TranslationConfig& config, const std::wstring& text, const std::wstring& sourceLanguage,
                    UINT requestId, const std::shared_ptr<CTranslationMailbox>& mailbox, bool progressive)
{
    if (!mailbox)
    {
        return false;
    }
    if (++g_runningJobs > kMaxConcurrentJobs)
    {
        g_runningJobs--;
        return false;
    }

    std::unique_ptr<AsyncJob> job(new (std::nothrow) AsyncJob());
    if (!job)
    {
        g_runningJobs--;
        return false;
    }
    job->config = config;
    job->text = text;
    job->sourceLanguage = sourceLanguage;
    job->requestId = requestId;
    job->mailbox = mailbox;
    job->progressive = progressive;

    // Keep this module loaded while the worker runs, even if TSF releases the IME meanwhile.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&TranslationThreadProc), &job->module))
    {
        g_runningJobs--;
        return false;
    }

    HMODULE module = job->module;
    HANDLE thread = CreateThread(nullptr, 0, TranslationThreadProc, job.get(), 0, nullptr);
    if (!thread)
    {
        FreeLibrary(module);
        g_runningJobs--;
        return false;
    }
    job.release();   // owned by the thread now
    CloseHandle(thread);
    return true;
}

} // namespace HttpTranslator
