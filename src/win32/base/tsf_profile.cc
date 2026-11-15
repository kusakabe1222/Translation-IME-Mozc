// Copyright 2010-2021, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "win32/base/tsf_profile.h"

#include "win32/base/display_name_resource.h"

namespace mozc {
namespace win32 {

namespace {

#ifdef GOOGLE_JAPANESE_INPUT_BUILD

// {D5A86FD5-5308-47EA-AD16-9C4EB160EC3C}
static const GUID kGoogleJapaneseInputTextService = {
    0xd5a86fd5,
    0x5308,
    0x47ea,
    {0xad, 0x16, 0x9c, 0x4e, 0xb1, 0x60, 0xec, 0x3c}};

// {773EB24E-CA1D-4B1B-B420-FA985BB0B80D}
static const GUID kGoogleJapaneseInputProfile = {
    0x773eb24e,
    0xca1d,
    0x4b1b,
    {0xb4, 0x20, 0xfa, 0x98, 0x5b, 0xb0, 0xb8, 0x0d}};

#else  // GOOGLE_JAPANESE_INPUT_BUILD

// Translation IME (not Mozc's {10A67BC8-...} / {186F700C-...}, so that both
// can be installed).
// {F97625AB-6601-4B6B-936B-F52405DBA02E}
static const GUID kMozcTextService = {
    0xf97625ab,
    0x6601,
    0x4b6b,
    {0x93, 0x6b, 0xf5, 0x24, 0x05, 0xdb, 0xa0, 0x2e}};

// {C0F9F8C8-B4A9-4C53-AFF1-47B7185479A1}
static const GUID kMozcProfile = {
    0xc0f9f8c8,
    0xb4a9,
    0x4c53,
    {0xaf, 0xf1, 0x47, 0xb7, 0x18, 0x54, 0x79, 0xa1}};

#endif  // GOOGLE_JAPANESE_INPUT_BUILD

// Represents the language ID of this text service.
const LANGID kTextServiceLanguage = MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT);

}  // namespace

const GUID& TsfProfile::GetTextServiceGuid() {
#ifdef GOOGLE_JAPANESE_INPUT_BUILD
  return kGoogleJapaneseInputTextService;
#else   // GOOGLE_JAPANESE_INPUT_BUILD
  return kMozcTextService;
#endif  // GOOGLE_JAPANESE_INPUT_BUILD
}

const GUID& TsfProfile::GetProfileGuid() {
#ifdef GOOGLE_JAPANESE_INPUT_BUILD
  return kGoogleJapaneseInputProfile;
#else   // GOOGLE_JAPANESE_INPUT_BUILD
  return kMozcProfile;
#endif  // GOOGLE_JAPANESE_INPUT_BUILD
}

LANGID TsfProfile::GetLangId() { return kTextServiceLanguage; }

int TsfProfile::GetIconIndex() { return 0; }

int TsfProfile::GetDescriptionTextIndex() {
  return IDS_TEXTSERVICE_DISPLAYNAME_SYNONYM;
}

}  // namespace win32
}  // namespace mozc
