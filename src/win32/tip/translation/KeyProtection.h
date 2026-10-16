// Encryption of the stored API keys.
//
// Each API key is encrypted with AES-256-GCM using a random 256-bit data key. The data key
// is stored next to the keys ("key protector") and is itself protected in two layers:
//
//   1. wrapped with an RSA-2048 key that lives in this PC's TPM (Microsoft Platform Crypto
//      Provider; the private key never leaves the TPM), when a TPM is available, and
//   2. encrypted with DPAPI for the current Windows user.
//
// Copying the registry (or the user's DPAPI master keys) to another machine is therefore not
// enough to read the API keys: the TPM of this PC is needed as well. Programs running as the
// same user on this PC can still decrypt them; no local storage can prevent that.
//
// Formats (all REG_BINARY):
//   protector: "TIDK" | version (1) | protection (1 = TPM + DPAPI, 2 = DPAPI) | padding | DPAPI blob
//              (the DPAPI blob holds the RSA ciphertext of the data key, or the data key itself)
//   value:     "TIK2" | nonce (12) | tag (16) | AES-256-GCM ciphertext of the UTF-16 text
//              (the value name is the additional authenticated data)

#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace KeyProtection {

enum class Protection
{
    None,
    Tpm,        // data key wrapped by the TPM, then DPAPI
    Dpapi,      // data key protected by DPAPI only (no usable TPM)
};

// Creates a new random data key and its protector. Uses the TPM key tpmKeyName (created on
// first use) when allowTpm is true and a TPM is usable, otherwise DPAPI only.
bool CreateDataKey(const std::wstring& tpmKeyName, bool allowTpm,
                   std::vector<BYTE>* dataKey, std::vector<BYTE>* protector, Protection* protection);

// Recovers the data key from a protector. The result is cached in this process, so the TPM is
// used only once per protector.
bool UnwrapDataKey(const std::wstring& tpmKeyName, const std::vector<BYTE>& protector, std::vector<BYTE>* dataKey);

Protection GetProtection(const std::vector<BYTE>& protector);

bool EncryptValue(const std::vector<BYTE>& dataKey, const std::wstring& name, const std::wstring& text,
                  std::vector<BYTE>* encrypted);
bool DecryptValue(const std::vector<BYTE>& dataKey, const std::wstring& name, const std::vector<BYTE>& encrypted,
                  std::wstring* text);
bool IsEncryptedValue(const std::vector<BYTE>& data);

// DPAPI only (format of version 1.x): kept to read and convert keys saved by older versions.
bool DpapiProtect(const std::wstring& text, std::vector<BYTE>* encrypted);
bool DpapiUnprotect(const std::vector<BYTE>& encrypted, std::wstring* text);

// Removes the TPM key (used by the unit tests for their own key).
bool DeleteTpmKey(const std::wstring& tpmKeyName);

void SecureClear(std::vector<BYTE>* data);

} // namespace KeyProtection
