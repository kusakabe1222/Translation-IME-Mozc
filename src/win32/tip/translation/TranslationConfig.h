// Translation settings, read from HKEY_CURRENT_USER\Software\TranslationIME.
//
//   Engine           REG_SZ     "DeepL" or "Google" (optional; picked from the keys if missing)
//   DeepLApiKey      REG_BINARY DeepL authentication key (free keys end with ":fx")
//   GoogleApiKey     REG_BINARY Google Cloud Translation API key
//   KeyProtector     REG_BINARY Data key that encrypts the two keys above
//   TargetLanguages  REG_SZ     Comma separated, e.g. "EN,ZH,KO,FR,ES,DE"
//   TimeoutMs        REG_DWORD  Per-request timeout in milliseconds
//   Enabled          REG_DWORD  0 turns translation off (Shift then does nothing); default 1
//
// API keys are encrypted with AES-256-GCM; the data key is protected by the TPM and DPAPI
// (see KeyProtection.h). Keys stored by older versions (plain REG_SZ, or DPAPI only) are still
// read, and ProtectLegacyKeys converts them.
//
// Use the settings app (TranslationIMESettings.exe) to write the keys; Set-TranslationConfig.ps1
// hands new keys to it.

#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include "KeyProtection.h"

enum class TranslationEngine
{
    None,
    DeepL,
    Google,
};

struct LanguageInfo
{
    const wchar_t* code;    // e.g. L"EN"
    const wchar_t* name;    // Japanese display name, e.g. L"英語"
};

struct TranslationConfig
{
    // Engine that will be used: the stored preference if it has a key, otherwise whichever
    // engine has a key, otherwise None.
    TranslationEngine engine = TranslationEngine::None;
    // The "Engine" value as stored (None when not set).
    TranslationEngine preferredEngine = TranslationEngine::None;
    std::wstring deeplApiKey;
    std::wstring googleApiKey;
    std::vector<std::wstring> targetLanguages;   // Upper case, e.g. L"EN", L"ZH"
    DWORD timeoutMs = 8000;
    bool enabled = true;   // translation on/off (settings app)
    // How the stored keys are protected (set by Load; None for keys from older versions).
    KeyProtection::Protection keyProtection = KeyProtection::Protection::None;

    static const wchar_t* const RegistryPath;

    static TranslationConfig Load(const wchar_t* registryPath = RegistryPath);
    static bool Save(const TranslationConfig& config, const wchar_t* registryPath = RegistryPath);
    // Re-encrypts API keys stored by older versions (plain text or DPAPI only) in the current
    // format, and moves a DPAPI-only data key to the TPM once one is usable. Returns how many
    // keys were rewritten, or -1 if something could not be.
    static int ProtectLegacyKeys(const wchar_t* registryPath = RegistryPath);
    // Loads the settings once on a background thread so that the data key is unwrapped
    // (and cached) before the first translation. Later calls do nothing.
    static void PreloadKeysAsync();

    // Parses "EN, zh ,ko" into { L"EN", L"ZH", L"KO" }. Falls back to the defaults when empty.
    static std::vector<std::wstring> ParseLanguageList(const std::wstring& text);
    static std::wstring JoinLanguageList(const std::vector<std::wstring>& languages);

    // Languages offered in the settings app (all supported by DeepL and Google).
    static const std::vector<LanguageInfo>& SupportedLanguages();
    // Japanese display name for a code; the code itself if unknown.
    static std::wstring LanguageName(const std::wstring& code);

    // Sets engine from preferredEngine and the keys that are present.
    void ResolveEngine();

    const std::wstring& ActiveApiKey() const;
    const wchar_t* EngineName() const;
    static const wchar_t* EngineName(TranslationEngine engine);
};
