// Translation settings, read from HKEY_CURRENT_USER\Software\TranslationIME.

#include "TranslationConfig.h"

#include <atomic>
#include <cwctype>

const wchar_t* const TranslationConfig::RegistryPath = L"Software\\TranslationIME";

namespace {

const wchar_t kDefaultLanguages[] = L"EN,ZH,KO,FR,ES,DE";
const DWORD kMinTimeoutMs = 1000;
const DWORD kMaxTimeoutMs = 60000;

const wchar_t kDeepLKeyName[] = L"DeepLApiKey";
const wchar_t kGoogleKeyName[] = L"GoogleApiKey";
const wchar_t kProtectorName[] = L"KeyProtector";

std::atomic<bool> g_preloadStarted(false);

DWORD WINAPI PreloadThreadProc(LPVOID param)
{
    TranslationConfig::Load();
    // The reference taken in PreloadKeysAsync keeps this module loaded until here.
    FreeLibraryAndExitThread(static_cast<HMODULE>(param), 0);
}

std::wstring ReadString(HKEY key, const wchar_t* name)
{
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || size == 0)
    {
        return std::wstring();
    }

    std::wstring value(size / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value[0]), &size) != ERROR_SUCCESS)
    {
        return std::wstring();
    }
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

std::vector<BYTE> ReadBinary(HKEY key, const wchar_t* name)
{
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_BINARY || size == 0)
    {
        return std::vector<BYTE>();
    }
    std::vector<BYTE> data(size);
    if (RegQueryValueExW(key, name, nullptr, &type, data.data(), &size) != ERROR_SUCCESS)
    {
        return std::vector<BYTE>();
    }
    data.resize(size);
    return data;
}

bool ReadDword(HKEY key, const wchar_t* name, DWORD* value)
{
    DWORD type = 0;
    DWORD size = sizeof(*value);
    return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD;
}

std::wstring Trim(const std::wstring& text)
{
    size_t start = 0;
    size_t end = text.length();
    while (start < end && iswspace(text[start])) start++;
    while (end > start && iswspace(text[end - 1])) end--;
    return text.substr(start, end - start);
}

// The TPM key belongs to one settings location (the unit tests use their own).
std::wstring TpmKeyName(const wchar_t* registryPath)
{
    std::wstring name = L"TranslationIME/";
    for (const wchar_t* p = registryPath; *p; p++)
    {
        name += (*p == L'\\') ? L'/' : *p;
    }
    return name;
}

// Current format: AES-256-GCM with the data key. Older versions stored the keys as plain
// REG_SZ (1.0) or as a DPAPI blob (1.1); both are still read.
std::wstring ReadApiKey(HKEY key, const wchar_t* name, const std::vector<BYTE>* dataKey)
{
    DWORD type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, nullptr) != ERROR_SUCCESS)
    {
        return std::wstring();
    }
    if (type == REG_SZ || type == REG_EXPAND_SZ)
    {
        return Trim(ReadString(key, name));
    }

    std::vector<BYTE> data = ReadBinary(key, name);
    std::wstring text;
    bool ok = KeyProtection::IsEncryptedValue(data)
        ? (dataKey != nullptr && KeyProtection::DecryptValue(*dataKey, name, data, &text))
        : KeyProtection::DpapiUnprotect(data, &text);
    return ok ? Trim(text) : std::wstring();
}

bool IsCurrentFormat(HKEY key, const wchar_t* name)
{
    DWORD type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, nullptr) != ERROR_SUCCESS)
    {
        return true;    // nothing stored
    }
    return type == REG_BINARY && KeyProtection::IsEncryptedValue(ReadBinary(key, name));
}

bool WriteApiKey(HKEY key, const wchar_t* name, const std::wstring& value, const std::vector<BYTE>& dataKey)
{
    if (value.empty())
    {
        LSTATUS status = RegDeleteValueW(key, name);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
    std::vector<BYTE> encrypted;
    if (!KeyProtection::EncryptValue(dataKey, name, value, &encrypted))
    {
        return false;
    }
    return RegSetValueExW(key, name, 0, REG_BINARY, encrypted.data(), static_cast<DWORD>(encrypted.size())) == ERROR_SUCCESS;
}

// The data key to encrypt with: the stored one while it is TPM-protected (so the IME in other
// applications keeps reading the keys while they are being saved), otherwise a new one.
bool PrepareDataKey(HKEY key, const wchar_t* registryPath, std::vector<BYTE>* dataKey)
{
    const std::wstring tpmKeyName = TpmKeyName(registryPath);
    std::vector<BYTE> protector = ReadBinary(key, kProtectorName);
    if (KeyProtection::GetProtection(protector) == KeyProtection::Protection::Tpm &&
        KeyProtection::UnwrapDataKey(tpmKeyName, protector, dataKey))
    {
        return true;
    }

    KeyProtection::Protection protection = KeyProtection::Protection::None;
    if (!KeyProtection::CreateDataKey(tpmKeyName, true, dataKey, &protector, &protection))
    {
        return false;
    }
    return RegSetValueExW(key, kProtectorName, 0, REG_BINARY, protector.data(), static_cast<DWORD>(protector.size())) == ERROR_SUCCESS;
}

} // namespace

std::vector<std::wstring> TranslationConfig::ParseLanguageList(const std::wstring& text)
{
    std::vector<std::wstring> languages;
    size_t start = 0;
    while (start <= text.length())
    {
        size_t comma = text.find(L',', start);
        if (comma == std::wstring::npos)
        {
            comma = text.length();
        }
        std::wstring language = Trim(text.substr(start, comma - start));
        for (wchar_t& ch : language)
        {
            ch = static_cast<wchar_t>(towupper(ch));
        }
        if (!language.empty())
        {
            languages.push_back(language);
        }
        start = comma + 1;
    }

    if (languages.empty() && text != kDefaultLanguages)
    {
        return ParseLanguageList(kDefaultLanguages);
    }
    return languages;
}

TranslationConfig TranslationConfig::Load(const wchar_t* registryPath)
{
    TranslationConfig config;
    std::wstring engineName;
    std::wstring languages;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, registryPath, 0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        engineName = ReadString(key, L"Engine");

        // The data key is unwrapped once per process (the TPM is slow); see KeyProtection.
        std::vector<BYTE> protector = ReadBinary(key, kProtectorName);
        std::vector<BYTE> dataKey;
        bool hasDataKey = !protector.empty() && KeyProtection::UnwrapDataKey(TpmKeyName(registryPath), protector, &dataKey);
        config.keyProtection = hasDataKey ? KeyProtection::GetProtection(protector) : KeyProtection::Protection::None;
        config.deeplApiKey = ReadApiKey(key, kDeepLKeyName, hasDataKey ? &dataKey : nullptr);
        config.googleApiKey = ReadApiKey(key, kGoogleKeyName, hasDataKey ? &dataKey : nullptr);
        KeyProtection::SecureClear(&dataKey);

        languages = ReadString(key, L"TargetLanguages");

        DWORD timeout = 0;
        if (ReadDword(key, L"TimeoutMs", &timeout))
        {
            config.timeoutMs = (timeout < kMinTimeoutMs) ? kMinTimeoutMs : (timeout > kMaxTimeoutMs) ? kMaxTimeoutMs : timeout;
        }
        RegCloseKey(key);
    }

    config.targetLanguages = ParseLanguageList(languages);

    if (_wcsicmp(engineName.c_str(), L"DeepL") == 0)
    {
        config.preferredEngine = TranslationEngine::DeepL;
    }
    else if (_wcsicmp(engineName.c_str(), L"Google") == 0)
    {
        config.preferredEngine = TranslationEngine::Google;
    }

    config.ResolveEngine();
    return config;
}

void TranslationConfig::ResolveEngine()
{
    // The preferred engine if it has a key, otherwise any engine that has one.
    const TranslationEngine candidates[] = { preferredEngine, TranslationEngine::DeepL, TranslationEngine::Google };
    for (TranslationEngine candidate : candidates)
    {
        engine = candidate;
        if (candidate != TranslationEngine::None && !ActiveApiKey().empty())
        {
            return;
        }
    }
    engine = TranslationEngine::None;
}

bool TranslationConfig::Save(const TranslationConfig& config, const wchar_t* registryPath)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, registryPath, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        return false;
    }

    auto writeString = [key](const wchar_t* name, const std::wstring& value) {
        if (value.empty())
        {
            LSTATUS status = RegDeleteValueW(key, name);
            return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
        }
        return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                              static_cast<DWORD>((value.length() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    };

    // Keys are never written unencrypted: without a data key they are not saved at all.
    std::vector<BYTE> dataKey;
    bool ok = PrepareDataKey(key, registryPath, &dataKey);
    if (ok)
    {
        ok = WriteApiKey(key, kDeepLKeyName, Trim(config.deeplApiKey), dataKey) && ok;
        ok = WriteApiKey(key, kGoogleKeyName, Trim(config.googleApiKey), dataKey) && ok;
    }
    KeyProtection::SecureClear(&dataKey);

    DWORD timeout = config.timeoutMs;
    ok = writeString(L"Engine", config.preferredEngine == TranslationEngine::None ? std::wstring() : std::wstring(EngineName(config.preferredEngine))) && ok;
    ok = writeString(L"TargetLanguages", JoinLanguageList(config.targetLanguages)) && ok;
    ok = (RegSetValueExW(key, L"TimeoutMs", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&timeout), sizeof(timeout)) == ERROR_SUCCESS) && ok;
    RegCloseKey(key);
    return ok;
}

int TranslationConfig::ProtectLegacyKeys(const wchar_t* registryPath)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, registryPath, 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS)
    {
        return 0;
    }

    const wchar_t* const names[] = { kDeepLKeyName, kGoogleKeyName };
    bool needed = false;
    for (const wchar_t* name : names)
    {
        needed = needed || !IsCurrentFormat(key, name);
    }
    // A protector without the TPM is replaced once a TPM can be used.
    std::vector<BYTE> protector = ReadBinary(key, kProtectorName);
    bool upgradeProtector = !protector.empty() && KeyProtection::GetProtection(protector) != KeyProtection::Protection::Tpm;
    if (!needed && !upgradeProtector)
    {
        RegCloseKey(key);
        return 0;
    }

    // Read everything with the current data key first, then re-encrypt with the new one.
    std::vector<BYTE> oldDataKey;
    bool hasOldDataKey = !protector.empty() && KeyProtection::UnwrapDataKey(TpmKeyName(registryPath), protector, &oldDataKey);
    if (!protector.empty() && !hasOldDataKey)
    {
        // The stored keys cannot be read right now (e.g. the TPM is busy): change nothing,
        // replacing the protector would make them unreadable for good.
        RegCloseKey(key);
        return -1;
    }
    std::wstring values[2];
    bool readable[2] = { false, false };
    bool legacy[2] = { false, false };
    for (int i = 0; i < 2; i++)
    {
        DWORD type = 0;
        if (RegQueryValueExW(key, names[i], nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS)
        {
            values[i] = ReadApiKey(key, names[i], hasOldDataKey ? &oldDataKey : nullptr);
            readable[i] = !values[i].empty();
            legacy[i] = !IsCurrentFormat(key, names[i]);
        }
    }
    KeyProtection::SecureClear(&oldDataKey);

    std::vector<BYTE> dataKey;
    KeyProtection::Protection protection = KeyProtection::Protection::None;
    bool failed = !KeyProtection::CreateDataKey(TpmKeyName(registryPath), true, &dataKey, &protector, &protection);
    if (!failed && upgradeProtector && protection != KeyProtection::Protection::Tpm && !needed)
    {
        // Still no TPM: keep what is stored.
        KeyProtection::SecureClear(&dataKey);
        RegCloseKey(key);
        return 0;
    }

    int converted = 0;
    if (!failed)
    {
        failed = RegSetValueExW(key, kProtectorName, 0, REG_BINARY, protector.data(), static_cast<DWORD>(protector.size())) != ERROR_SUCCESS;
    }
    for (int i = 0; i < 2 && !failed; i++)
    {
        if (!readable[i])
        {
            continue;
        }
        if (WriteApiKey(key, names[i], values[i], dataKey))
        {
            converted += legacy[i] ? 1 : 0;
        }
        else
        {
            failed = true;
        }
    }
    KeyProtection::SecureClear(&dataKey);
    SecureZeroMemory(&values[0][0], values[0].size() * sizeof(wchar_t));
    SecureZeroMemory(&values[1][0], values[1].size() * sizeof(wchar_t));
    RegCloseKey(key);
    return failed ? -1 : converted;
}

void TranslationConfig::PreloadKeysAsync()
{
    if (g_preloadStarted.exchange(true))
    {
        return;
    }
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&PreloadThreadProc), &module))
    {
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, PreloadThreadProc, module, 0, nullptr);
    if (!thread)
    {
        FreeLibrary(module);
        return;
    }
    CloseHandle(thread);
}

std::wstring TranslationConfig::JoinLanguageList(const std::vector<std::wstring>& languages)
{
    std::wstring text;
    for (const std::wstring& language : languages)
    {
        if (!text.empty())
        {
            text += L',';
        }
        text += language;
    }
    return text;
}

const std::vector<LanguageInfo>& TranslationConfig::SupportedLanguages()
{
    static const std::vector<LanguageInfo> languages = {
        { L"EN", L"英語" },
        { L"ZH", L"中国語 (簡体字)" },
        { L"ZH-TW", L"中国語 (繁体字)" },
        { L"KO", L"韓国語" },
        { L"FR", L"フランス語" },
        { L"ES", L"スペイン語" },
        { L"DE", L"ドイツ語" },
        { L"IT", L"イタリア語" },
        { L"PT", L"ポルトガル語" },
        { L"RU", L"ロシア語" },
        { L"ID", L"インドネシア語" },
        { L"NL", L"オランダ語" },
        { L"PL", L"ポーランド語" },
        { L"TR", L"トルコ語" },
        { L"UK", L"ウクライナ語" },
        { L"SV", L"スウェーデン語" },
        { L"AR", L"アラビア語" },
    };
    return languages;
}

std::wstring TranslationConfig::LanguageName(const std::wstring& code)
{
    if (_wcsicmp(code.c_str(), L"JA") == 0)
    {
        return L"日本語";
    }
    for (const LanguageInfo& language : SupportedLanguages())
    {
        if (_wcsicmp(code.c_str(), language.code) == 0)
        {
            return language.name;
        }
    }
    return code;
}

const std::wstring& TranslationConfig::ActiveApiKey() const
{
    static const std::wstring empty;
    switch (engine)
    {
    case TranslationEngine::DeepL:  return deeplApiKey;
    case TranslationEngine::Google: return googleApiKey;
    default:                        return empty;
    }
}

const wchar_t* TranslationConfig::EngineName() const
{
    return EngineName(engine);
}

const wchar_t* TranslationConfig::EngineName(TranslationEngine engine)
{
    switch (engine)
    {
    case TranslationEngine::DeepL:  return L"DeepL";
    case TranslationEngine::Google: return L"Google";
    default:                        return L"None";
    }
}
