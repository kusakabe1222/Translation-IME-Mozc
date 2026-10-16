// Encryption of the stored API keys: AES-256-GCM, with the data key protected by the TPM and DPAPI.
// See KeyProtection.h for the design and the formats.

#include "KeyProtection.h"

#include <bcrypt.h>
#include <ncrypt.h>
#include <wincrypt.h>

#include <cstring>
#include <mutex>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "crypt32.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

namespace KeyProtection {

namespace {

const BYTE kProtectorMagic[4] = { 'T', 'I', 'D', 'K' };
const BYTE kProtectorVersion = 1;
const BYTE kValueMagic[4] = { 'T', 'I', 'K', '2' };

const size_t kDataKeyBytes = 32;    // AES-256
const size_t kNonceBytes = 12;      // GCM
const size_t kTagBytes = 16;
const size_t kProtectorHeaderBytes = sizeof(kProtectorMagic) + 3;
const size_t kValueHeaderBytes = sizeof(kValueMagic) + kNonceBytes + kTagBytes;

const BYTE kProtectionTpm = 1;
const BYTE kProtectionDpapi = 2;

// RSA padding used to wrap the data key with the TPM key (stored in the protector).
const BYTE kPaddingNone = 0;
const BYTE kPaddingOaepSha256 = 1;
const BYTE kPaddingOaepSha1 = 2;
const BYTE kPaddingPkcs1 = 3;

// Extra DPAPI entropy (not secrets). Version 1.x encrypted the keys themselves with the first one.
const char kLegacyKeyEntropy[] = "TranslationIME.ApiKey.v1";
const char kDataKeyEntropy[] = "TranslationIME.DataKey.v2";

// The most recently unwrapped data key, so the TPM is used once per protector and process.
std::mutex g_cacheLock;
std::vector<BYTE> g_cachedProtector;
std::vector<BYTE> g_cachedDataKey;

bool RandomBytes(BYTE* buffer, size_t size)
{
    return NT_SUCCESS(BCryptGenRandom(nullptr, buffer, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool Dpapi(bool protect, const BYTE* data, size_t size, const char* entropyText, std::vector<BYTE>* out)
{
    DATA_BLOB input;
    input.pbData = const_cast<BYTE*>(data);
    input.cbData = static_cast<DWORD>(size);
    DATA_BLOB entropy;
    entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(entropyText));
    entropy.cbData = static_cast<DWORD>(strlen(entropyText));
    DATA_BLOB output = {};

    BOOL ok = protect
        ? CryptProtectData(&input, L"Translation IME", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
        : CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok)
    {
        return false;
    }
    out->assign(output.pbData, output.pbData + output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
}

// The TPM key: RSA-2048 in the Microsoft Platform Crypto Provider, usable only for decryption.
class TpmKey
{
public:
    ~TpmKey()
    {
        if (_key) NCryptFreeObject(_key);
        if (_provider) NCryptFreeObject(_provider);
    }

    bool Open(const std::wstring& name, bool create)
    {
        if (NCryptOpenStorageProvider(&_provider, MS_PLATFORM_CRYPTO_PROVIDER, 0) != ERROR_SUCCESS)
        {
            _provider = 0;
            return false;
        }
        SECURITY_STATUS status = NCryptOpenKey(_provider, &_key, name.c_str(), 0, NCRYPT_SILENT_FLAG);
        if (status == ERROR_SUCCESS)
        {
            return true;
        }
        _key = 0;
        if (!create || status != NTE_BAD_KEYSET)
        {
            return false;
        }

        if (NCryptCreatePersistedKey(_provider, &_key, BCRYPT_RSA_ALGORITHM, name.c_str(), 0, 0) != ERROR_SUCCESS)
        {
            _key = 0;
            return false;
        }
        DWORD length = 2048;
        DWORD usage = NCRYPT_ALLOW_DECRYPT_FLAG;
        if (NCryptSetProperty(_key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<BYTE*>(&length), sizeof(length), 0) != ERROR_SUCCESS ||
            NCryptSetProperty(_key, NCRYPT_KEY_USAGE_PROPERTY, reinterpret_cast<BYTE*>(&usage), sizeof(usage), 0) != ERROR_SUCCESS ||
            NCryptFinalizeKey(_key, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS)
        {
            NCryptDeleteKey(_key, 0);   // also frees the handle
            _key = 0;
            return false;
        }
        return true;
    }

    bool Encrypt(BYTE padding, const std::vector<BYTE>& plain, std::vector<BYTE>* out)
    {
        return Crypt(true, padding, plain, out);
    }

    bool Decrypt(BYTE padding, const std::vector<BYTE>& wrapped, std::vector<BYTE>* out)
    {
        return Crypt(false, padding, wrapped, out);
    }

    bool Delete()
    {
        if (!_key)
        {
            return false;
        }
        bool ok = NCryptDeleteKey(_key, 0) == ERROR_SUCCESS;
        _key = 0;
        return ok;
    }

private:
    bool Crypt(bool encrypt, BYTE padding, const std::vector<BYTE>& input, std::vector<BYTE>* out)
    {
        BCRYPT_OAEP_PADDING_INFO oaep = { nullptr, nullptr, 0 };
        void* paddingInfo = nullptr;
        DWORD flags = 0;
        switch (padding)
        {
        case kPaddingOaepSha256: oaep.pszAlgId = BCRYPT_SHA256_ALGORITHM; paddingInfo = &oaep; flags = NCRYPT_PAD_OAEP_FLAG; break;
        case kPaddingOaepSha1:   oaep.pszAlgId = BCRYPT_SHA1_ALGORITHM;   paddingInfo = &oaep; flags = NCRYPT_PAD_OAEP_FLAG; break;
        case kPaddingPkcs1:      flags = NCRYPT_PAD_PKCS1_FLAG; break;
        default: return false;
        }
        flags |= NCRYPT_SILENT_FLAG;

        BYTE* data = const_cast<BYTE*>(input.data());
        DWORD inputSize = static_cast<DWORD>(input.size());
        DWORD size = 0;
        SECURITY_STATUS status = encrypt
            ? NCryptEncrypt(_key, data, inputSize, paddingInfo, nullptr, 0, &size, flags)
            : NCryptDecrypt(_key, data, inputSize, paddingInfo, nullptr, 0, &size, flags);
        if (status != ERROR_SUCCESS || size == 0)
        {
            return false;
        }
        out->assign(size, 0);
        status = encrypt
            ? NCryptEncrypt(_key, data, inputSize, paddingInfo, out->data(), size, &size, flags)
            : NCryptDecrypt(_key, data, inputSize, paddingInfo, out->data(), size, &size, flags);
        if (status != ERROR_SUCCESS)
        {
            SecureClear(out);
            return false;
        }
        out->resize(size);
        return true;
    }

    NCRYPT_PROV_HANDLE _provider = 0;
    NCRYPT_KEY_HANDLE _key = 0;
};

// Wraps the data key with the TPM, trying the paddings from the strongest. Each candidate is
// checked by unwrapping it again, since encryption only needs the public key.
bool TpmWrap(const std::wstring& tpmKeyName, const std::vector<BYTE>& dataKey, std::vector<BYTE>* wrapped, BYTE* padding)
{
    TpmKey key;
    if (!key.Open(tpmKeyName, true))
    {
        return false;
    }
    for (BYTE candidate : { kPaddingOaepSha256, kPaddingOaepSha1, kPaddingPkcs1 })
    {
        std::vector<BYTE> check;
        if (key.Encrypt(candidate, dataKey, wrapped) && key.Decrypt(candidate, *wrapped, &check) && check == dataKey)
        {
            SecureClear(&check);
            *padding = candidate;
            return true;
        }
        SecureClear(&check);
    }
    return false;
}

class AesGcm
{
public:
    explicit AesGcm(const std::vector<BYTE>& dataKey)
    {
        if (dataKey.size() != kDataKeyBytes ||
            !NT_SUCCESS(BCryptOpenAlgorithmProvider(&_algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0)))
        {
            _algorithm = nullptr;
            return;
        }
        if (!NT_SUCCESS(BCryptSetProperty(_algorithm, BCRYPT_CHAINING_MODE, reinterpret_cast<BYTE*>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0)) ||
            !NT_SUCCESS(BCryptGenerateSymmetricKey(_algorithm, &_key, nullptr, 0, const_cast<BYTE*>(dataKey.data()),
                                                   static_cast<ULONG>(dataKey.size()), 0)))
        {
            _key = nullptr;
        }
    }

    ~AesGcm()
    {
        if (_key) BCryptDestroyKey(_key);
        if (_algorithm) BCryptCloseAlgorithmProvider(_algorithm, 0);
    }

    bool Crypt(bool encrypt, const std::vector<BYTE>& aad, BYTE* nonce, BYTE* tag, const BYTE* input, size_t size, BYTE* output)
    {
        if (!_key)
        {
            return false;
        }
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce;
        info.cbNonce = static_cast<ULONG>(kNonceBytes);
        info.pbAuthData = const_cast<BYTE*>(aad.data());
        info.cbAuthData = static_cast<ULONG>(aad.size());
        info.pbTag = tag;
        info.cbTag = static_cast<ULONG>(kTagBytes);

        ULONG written = 0;
        NTSTATUS status = encrypt
            ? BCryptEncrypt(_key, const_cast<BYTE*>(input), static_cast<ULONG>(size), &info, nullptr, 0, output, static_cast<ULONG>(size), &written, 0)
            : BCryptDecrypt(_key, const_cast<BYTE*>(input), static_cast<ULONG>(size), &info, nullptr, 0, output, static_cast<ULONG>(size), &written, 0);
        return NT_SUCCESS(status) && written == size;
    }

private:
    BCRYPT_ALG_HANDLE _algorithm = nullptr;
    BCRYPT_KEY_HANDLE _key = nullptr;
};

std::vector<BYTE> NameBytes(const std::wstring& name)
{
    const BYTE* bytes = reinterpret_cast<const BYTE*>(name.data());
    return std::vector<BYTE>(bytes, bytes + name.size() * sizeof(wchar_t));
}

bool HasProtectorHeader(const std::vector<BYTE>& protector)
{
    return protector.size() > kProtectorHeaderBytes &&
        memcmp(protector.data(), kProtectorMagic, sizeof(kProtectorMagic)) == 0 &&
        protector[4] == kProtectorVersion;
}

void CacheDataKey(const std::vector<BYTE>& protector, const std::vector<BYTE>& dataKey)
{
    std::lock_guard<std::mutex> guard(g_cacheLock);
    SecureClear(&g_cachedDataKey);
    g_cachedProtector = protector;
    g_cachedDataKey = dataKey;
}

} // namespace

void SecureClear(std::vector<BYTE>* data)
{
    if (!data->empty())
    {
        SecureZeroMemory(data->data(), data->size());
    }
    data->clear();
}

bool CreateDataKey(const std::wstring& tpmKeyName, bool allowTpm,
                   std::vector<BYTE>* dataKey, std::vector<BYTE>* protector, Protection* protection)
{
    dataKey->assign(kDataKeyBytes, 0);
    if (!RandomBytes(dataKey->data(), dataKey->size()))
    {
        SecureClear(dataKey);
        return false;
    }

    BYTE kind = kProtectionDpapi;
    BYTE padding = kPaddingNone;
    std::vector<BYTE> inner;
    if (allowTpm && TpmWrap(tpmKeyName, *dataKey, &inner, &padding))
    {
        kind = kProtectionTpm;
    }
    else
    {
        inner = *dataKey;
        padding = kPaddingNone;
    }

    std::vector<BYTE> blob;
    bool ok = Dpapi(true, inner.data(), inner.size(), kDataKeyEntropy, &blob);
    SecureClear(&inner);
    if (!ok)
    {
        SecureClear(dataKey);
        return false;
    }

    protector->assign(kProtectorMagic, kProtectorMagic + sizeof(kProtectorMagic));
    protector->push_back(kProtectorVersion);
    protector->push_back(kind);
    protector->push_back(padding);
    protector->insert(protector->end(), blob.begin(), blob.end());
    *protection = (kind == kProtectionTpm) ? Protection::Tpm : Protection::Dpapi;

    CacheDataKey(*protector, *dataKey);
    return true;
}

bool UnwrapDataKey(const std::wstring& tpmKeyName, const std::vector<BYTE>& protector, std::vector<BYTE>* dataKey)
{
    if (!HasProtectorHeader(protector))
    {
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(g_cacheLock);
        if (!g_cachedDataKey.empty() && g_cachedProtector == protector)
        {
            *dataKey = g_cachedDataKey;
            return true;
        }
    }

    const BYTE kind = protector[5];
    const BYTE padding = protector[6];
    std::vector<BYTE> inner;
    if (!Dpapi(false, protector.data() + kProtectorHeaderBytes, protector.size() - kProtectorHeaderBytes, kDataKeyEntropy, &inner))
    {
        return false;
    }

    bool ok = false;
    if (kind == kProtectionDpapi)
    {
        *dataKey = inner;
        ok = true;
    }
    else if (kind == kProtectionTpm)
    {
        TpmKey key;
        ok = key.Open(tpmKeyName, false) && key.Decrypt(padding, inner, dataKey);
    }
    SecureClear(&inner);

    if (!ok || dataKey->size() != kDataKeyBytes)
    {
        SecureClear(dataKey);
        return false;
    }
    CacheDataKey(protector, *dataKey);
    return true;
}

Protection GetProtection(const std::vector<BYTE>& protector)
{
    if (!HasProtectorHeader(protector))
    {
        return Protection::None;
    }
    switch (protector[5])
    {
    case kProtectionTpm:   return Protection::Tpm;
    case kProtectionDpapi: return Protection::Dpapi;
    default:               return Protection::None;
    }
}

bool EncryptValue(const std::vector<BYTE>& dataKey, const std::wstring& name, const std::wstring& text,
                  std::vector<BYTE>* encrypted)
{
    const BYTE* plain = reinterpret_cast<const BYTE*>(text.data());
    const size_t size = text.size() * sizeof(wchar_t);

    encrypted->assign(kValueHeaderBytes + size, 0);
    memcpy(encrypted->data(), kValueMagic, sizeof(kValueMagic));
    BYTE* nonce = encrypted->data() + sizeof(kValueMagic);
    BYTE* tag = nonce + kNonceBytes;
    BYTE* cipher = tag + kTagBytes;

    AesGcm aes(dataKey);
    if (!RandomBytes(nonce, kNonceBytes) || !aes.Crypt(true, NameBytes(name), nonce, tag, plain, size, cipher))
    {
        encrypted->clear();
        return false;
    }
    return true;
}

bool DecryptValue(const std::vector<BYTE>& dataKey, const std::wstring& name, const std::vector<BYTE>& encrypted,
                  std::wstring* text)
{
    if (!IsEncryptedValue(encrypted))
    {
        return false;
    }
    std::vector<BYTE> header(encrypted.begin(), encrypted.begin() + kValueHeaderBytes);
    BYTE* nonce = header.data() + sizeof(kValueMagic);
    BYTE* tag = nonce + kNonceBytes;
    const size_t size = encrypted.size() - kValueHeaderBytes;
    if (size % sizeof(wchar_t) != 0)
    {
        return false;
    }

    std::vector<BYTE> plain(size, 0);
    AesGcm aes(dataKey);
    bool ok = aes.Crypt(false, NameBytes(name), nonce, tag, encrypted.data() + kValueHeaderBytes, size, plain.data());
    if (ok)
    {
        text->assign(reinterpret_cast<const wchar_t*>(plain.data()), size / sizeof(wchar_t));
    }
    SecureClear(&plain);
    return ok;
}

bool IsEncryptedValue(const std::vector<BYTE>& data)
{
    return data.size() >= kValueHeaderBytes && memcmp(data.data(), kValueMagic, sizeof(kValueMagic)) == 0;
}

bool DpapiProtect(const std::wstring& text, std::vector<BYTE>* encrypted)
{
    return Dpapi(true, reinterpret_cast<const BYTE*>(text.data()), text.size() * sizeof(wchar_t), kLegacyKeyEntropy, encrypted);
}

bool DpapiUnprotect(const std::vector<BYTE>& encrypted, std::wstring* text)
{
    std::vector<BYTE> plain;
    if (!Dpapi(false, encrypted.data(), encrypted.size(), kLegacyKeyEntropy, &plain) || plain.size() % sizeof(wchar_t) != 0)
    {
        SecureClear(&plain);
        return false;
    }
    text->assign(reinterpret_cast<const wchar_t*>(plain.data()), plain.size() / sizeof(wchar_t));
    SecureClear(&plain);
    return true;
}

bool DeleteTpmKey(const std::wstring& tpmKeyName)
{
    TpmKey key;
    return key.Open(tpmKeyName, false) && key.Delete();
}

} // namespace KeyProtection
