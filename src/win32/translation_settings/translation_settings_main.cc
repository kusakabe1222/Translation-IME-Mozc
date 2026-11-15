// Translation IME settings app (mozc_translation_settings.exe), opened from the
// Mozc language bar menu (翻訳の設定) and the Start menu.
//
// Edits HKEY_CURRENT_USER\Software\TranslationIME through TranslationConfig, the same
// code the TIP reads it with (win32/tip/translation). Network calls (connection test,
// DeepL usage) run on worker threads and report back with posted messages.

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "win32/tip/translation/HttpTranslator.h"
#include "win32/tip/translation/TranslationConfig.h"
#include "win32/tip/translation/UiText.h"
#include "win32/translation_settings/translation_settings_resource.h"

#pragma comment(lib, "comctl32.lib")

namespace {

const UINT WM_APP_TEST_RESULT = WM_APP + 1;
const UINT WM_APP_USAGE = WM_APP + 2;
const UINT WM_APP_REFRESH_FONTS = WM_APP + 3;

const wchar_t* WindowTitle()
{
    return UiText::Pick(L"Translation IME 設定", L"Translation IME Settings");
}
const wchar_t kTestText[] = L"こんにちは";
const WCHAR kPasswordChar = 0x2022;   // •

const COLORREF kBrand = RGB(0x00, 0x67, 0xC0);
const COLORREF kBackground = RGB(0xFF, 0xFF, 0xFF);
const COLORREF kFooterBackground = RGB(0xF3, 0xF3, 0xF3);
const COLORREF kDivider = RGB(0xE0, 0xE0, 0xE0);
const COLORREF kText = RGB(0x1A, 0x1A, 0x1A);
const COLORREF kMuted = RGB(0x60, 0x60, 0x60);
const COLORREF kSuccess = RGB(0x0F, 0x7B, 0x0F);
const COLORREF kError = RGB(0xC4, 0x2B, 0x1C);

// Language check boxes, in dialog units (see translation_settings.rc).
const int kLanguageColumns = 4;
const int kLanguageLeft = 14;
const int kLanguageTop = 152;
const int kLanguageColumnWidth = 74;
const int kLanguageRowHeight = 12;

struct TestOutcome
{
    bool succeeded = false;
    std::wstring message;
};

struct UsageOutcome
{
    bool succeeded = false;
    DeepLUsage usage;
    std::wstring error;
};

struct AppState
{
    HFONT sectionFont = nullptr;
    HFONT titleFont = nullptr;
    HFONT subtitleFont = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH footerBrush = nullptr;
    HBRUSH brandBrush = nullptr;
    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    COLORREF testResultColor = kMuted;
};

AppState g_state;

std::wstring GetItemText(HWND dialog, int id)
{
    HWND control = GetDlgItem(dialog, id);
    int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, &text[0], length + 1);
    text.resize(static_cast<size_t>(length));

    size_t start = text.find_first_not_of(L" \t\r\n");
    size_t end = text.find_last_not_of(L" \t\r\n");
    return (start == std::wstring::npos) ? std::wstring() : text.substr(start, end - start + 1);
}

RECT GetItemRect(HWND dialog, int id)
{
    RECT rc = { 0, 0, 0, 0 };
    HWND control = GetDlgItem(dialog, id);
    if (control)
    {
        GetWindowRect(control, &rc);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rc), 2);
    }
    return rc;
}

std::wstring FormatCount(unsigned long long value)
{
    std::wstring digits = std::to_wstring(value);
    std::wstring result;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
    {
        if (count > 0 && count % 3 == 0)
        {
            result.insert(result.begin(), L',');
        }
        result.insert(result.begin(), *it);
        count++;
    }
    return result;
}

void DeleteFonts()
{
    HFONT* fonts[] = { &g_state.sectionFont, &g_state.titleFont, &g_state.subtitleFont };
    for (HFONT* font : fonts)
    {
        if (*font)
        {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
}

void CreateFonts(HWND dialog)
{
    DeleteFonts();

    LOGFONTW base = {};
    HFONT dialogFont = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
    if (!dialogFont || !GetObjectW(dialogFont, sizeof(base), &base))
    {
        return;
    }
    base.lfQuality = CLEARTYPE_QUALITY;

    LOGFONTW section = base;
    section.lfWeight = FW_SEMIBOLD;
    section.lfHeight = MulDiv(base.lfHeight, 112, 100);
    g_state.sectionFont = CreateFontIndirectW(&section);

    LOGFONTW title = base;
    title.lfWeight = FW_SEMIBOLD;
    title.lfHeight = MulDiv(base.lfHeight, 160, 100);
    g_state.titleFont = CreateFontIndirectW(&title);

    LOGFONTW subtitle = base;
    subtitle.lfHeight = MulDiv(base.lfHeight, 95, 100);
    g_state.subtitleFont = CreateFontIndirectW(&subtitle);

    const int sections[] = { IDC_SECTION_ENGINE, IDC_SECTION_LANGUAGES, IDC_SECTION_OTHER };
    for (int id : sections)
    {
        SendDlgItemMessageW(dialog, id, WM_SETFONT, reinterpret_cast<WPARAM>(g_state.sectionFont), TRUE);
    }
}

void CreateLanguageCheckBoxes(HWND dialog)
{
    HFONT dialogFont = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
    HWND insertAfter = GetDlgItem(dialog, IDC_LANGUAGE_HINT);   // keeps the tab order in reading order

    const std::vector<LanguageInfo>& languages = TranslationConfig::SupportedLanguages();
    for (size_t i = 0; i < languages.size() && i < LANGUAGE_CHECKBOX_COUNT; i++)
    {
        int column = static_cast<int>(i) % kLanguageColumns;
        int row = static_cast<int>(i) / kLanguageColumns;
        RECT rc = { kLanguageLeft + column * kLanguageColumnWidth, kLanguageTop + row * kLanguageRowHeight,
                    kLanguageLeft + column * kLanguageColumnWidth + kLanguageColumnWidth - 2, kLanguageTop + row * kLanguageRowHeight + 10 };
        MapDialogRect(dialog, &rc);

        HWND checkBox = CreateWindowExW(0, L"BUTTON", languages[i].DisplayName(),
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, dialog,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LANGUAGE_FIRST + i)),
                                        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE)), nullptr);
        SendMessageW(checkBox, WM_SETFONT, reinterpret_cast<WPARAM>(dialogFont), FALSE);
        SetWindowPos(checkBox, insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        insertAfter = checkBox;
    }
}

void LoadIntoDialog(HWND dialog)
{
    TranslationConfig config = TranslationConfig::Load();

    TranslationEngine engine = (config.preferredEngine != TranslationEngine::None) ? config.preferredEngine : config.engine;
    CheckRadioButton(dialog, IDC_ENGINE_DEEPL, IDC_ENGINE_GOOGLE,
                     (engine == TranslationEngine::Google) ? IDC_ENGINE_GOOGLE : IDC_ENGINE_DEEPL);
    SetDlgItemTextW(dialog, IDC_DEEPL_KEY, config.deeplApiKey.c_str());
    SetDlgItemTextW(dialog, IDC_GOOGLE_KEY, config.googleApiKey.c_str());

    const std::vector<LanguageInfo>& languages = TranslationConfig::SupportedLanguages();
    for (size_t i = 0; i < languages.size() && i < LANGUAGE_CHECKBOX_COUNT; i++)
    {
        bool selected = false;
        for (const std::wstring& code : config.targetLanguages)
        {
            selected = selected || (_wcsicmp(code.c_str(), languages[i].code) == 0);
        }
        CheckDlgButton(dialog, IDC_LANGUAGE_FIRST + static_cast<int>(i), selected ? BST_CHECKED : BST_UNCHECKED);
    }

    SendDlgItemMessageW(dialog, IDC_TIMEOUT_SPIN, UDM_SETRANGE32, 1, 60);
    SendDlgItemMessageW(dialog, IDC_TIMEOUT_SPIN, UDM_SETPOS32, 0, static_cast<LPARAM>(config.timeoutMs / 1000));
    CheckDlgButton(dialog, IDC_ENABLED, config.enabled ? BST_CHECKED : BST_UNCHECKED);

    const wchar_t* protection = UiText::Pick(L"キーは保存時に AES-256-GCM で暗号化されます", L"Keys are encrypted with AES-256-GCM when saved");
    switch (config.keyProtection)
    {
    case KeyProtection::Protection::Tpm:   protection = UiText::Pick(L"キーの保護: AES-256-GCM + TPM + DPAPI", L"Key protection: AES-256-GCM + TPM + DPAPI"); break;
    case KeyProtection::Protection::Dpapi: protection = UiText::Pick(L"キーの保護: AES-256-GCM + DPAPI (TPM なし)", L"Key protection: AES-256-GCM + DPAPI (no TPM)"); break;
    default: break;
    }
    SetDlgItemTextW(dialog, IDC_KEY_PROTECTION, protection);
}

TranslationConfig ConfigFromDialog(HWND dialog)
{
    TranslationConfig config;
    config.preferredEngine = IsDlgButtonChecked(dialog, IDC_ENGINE_GOOGLE) ? TranslationEngine::Google : TranslationEngine::DeepL;
    config.deeplApiKey = GetItemText(dialog, IDC_DEEPL_KEY);
    config.googleApiKey = GetItemText(dialog, IDC_GOOGLE_KEY);

    const std::vector<LanguageInfo>& languages = TranslationConfig::SupportedLanguages();
    for (size_t i = 0; i < languages.size() && i < LANGUAGE_CHECKBOX_COUNT; i++)
    {
        if (IsDlgButtonChecked(dialog, IDC_LANGUAGE_FIRST + static_cast<int>(i)))
        {
            config.targetLanguages.push_back(languages[i].code);
        }
    }

    BOOL translated = FALSE;
    UINT seconds = GetDlgItemInt(dialog, IDC_TIMEOUT, &translated, FALSE);
    if (!translated || seconds < 1) seconds = 8;
    if (seconds > 60) seconds = 60;
    config.timeoutMs = seconds * 1000;
    config.enabled = IsDlgButtonChecked(dialog, IDC_ENABLED) == BST_CHECKED;

    config.ResolveEngine();
    return config;
}

void SetTestResult(HWND dialog, const std::wstring& text, COLORREF color)
{
    g_state.testResultColor = color;
    SetDlgItemTextW(dialog, IDC_TEST_RESULT, text.c_str());
    InvalidateRect(GetDlgItem(dialog, IDC_TEST_RESULT), nullptr, TRUE);
}

void StartUsageQuery(HWND dialog)
{
    TranslationConfig config = ConfigFromDialog(dialog);
    HWND bar = GetDlgItem(dialog, IDC_USAGE_BAR);
    if (config.deeplApiKey.empty())
    {
        SendMessageW(bar, PBM_SETPOS, 0, 0);
        SetDlgItemTextW(dialog, IDC_USAGE_TEXT, UiText::Pick(L"DeepL キー未設定", L"No DeepL key"));
        return;
    }

    SetDlgItemTextW(dialog, IDC_USAGE_TEXT, UiText::Pick(L"取得中…", L"Loading…"));
    std::thread([dialog, config]() {
        std::unique_ptr<UsageOutcome> outcome(new UsageOutcome());
        outcome->succeeded = HttpTranslator::GetDeepLUsage(config, &outcome->usage, &outcome->error);
        if (PostMessageW(dialog, WM_APP_USAGE, 0, reinterpret_cast<LPARAM>(outcome.get())))
        {
            outcome.release();
        }
    }).detach();
}

void StartConnectionTest(HWND dialog)
{
    TranslationConfig config = ConfigFromDialog(dialog);
    if (config.engine == TranslationEngine::None)
    {
        SetTestResult(dialog, UiText::Pick(L"API キーを入力してください", L"Enter an API key"), kError);
        return;
    }

    std::wstring target = config.targetLanguages.empty() ? std::wstring(L"EN") : config.targetLanguages.front();
    config.targetLanguages.assign(1, target);

    EnableWindow(GetDlgItem(dialog, IDC_TEST), FALSE);
    SetTestResult(dialog,
                  UiText::IsJapanese() ? std::wstring(config.EngineName()) + L" に接続しています…"
                                       : L"Connecting to " + std::wstring(config.EngineName()) + L"…",
                  kMuted);

    std::thread([dialog, config]() {
        std::unique_ptr<TestOutcome> outcome(new TestOutcome());
        TranslationResult result = HttpTranslator::Translate(config, kTestText);
        if (result.AnySucceeded())
        {
            outcome->succeeded = true;
            outcome->message = UiText::IsJapanese()
                ? L"✓ " + std::wstring(config.EngineName()) + L" に接続できました: 「" + kTestText + L"」→ " + result.items.front().text
                : L"✓ Connected to " + std::wstring(config.EngineName()) + L": “" + kTestText + L"” → " + result.items.front().text;
        }
        else
        {
            outcome->message = L"✗ " + result.FirstError();
        }
        if (PostMessageW(dialog, WM_APP_TEST_RESULT, 0, reinterpret_cast<LPARAM>(outcome.get())))
        {
            outcome.release();
        }
    }).detach();

    StartUsageQuery(dialog);
}

void TogglePasswordVisibility(HWND dialog, int buttonId, int editId)
{
    bool show = IsDlgButtonChecked(dialog, buttonId) == BST_CHECKED;
    HWND edit = GetDlgItem(dialog, editId);
    SendMessageW(edit, EM_SETPASSWORDCHAR, show ? 0 : kPasswordChar, 0);
    SetDlgItemTextW(dialog, buttonId, show ? UiText::Pick(L"隠す", L"Hide") : UiText::Pick(L"表示", L"Show"));
    InvalidateRect(edit, nullptr, TRUE);
}

void PaintChrome(HWND dialog, HDC dc)
{
    // Header band with the app icon, title and tagline.
    RECT header = GetItemRect(dialog, IDC_HEADER);
    FillRect(dc, &header, g_state.brandBrush);

    int height = header.bottom - header.top;
    int iconSize = height * 56 / 100;
    RECT margin = { 14, 0, 0, 0 };
    MapDialogRect(dialog, &margin);
    int x = header.left + margin.left;
    if (g_state.largeIcon)
    {
        DrawIconEx(dc, x, header.top + (height - iconSize) / 2, g_state.largeIcon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
    }
    x += iconSize + margin.left * 2 / 3;

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    RECT title = { x, header.top, header.right - margin.left, header.top + height * 58 / 100 };
    HGDIOBJ oldFont = SelectObject(dc, g_state.titleFont);
    DrawTextW(dc, L"Translation IME", -1, &title, DT_SINGLELINE | DT_BOTTOM | DT_NOPREFIX);
    RECT subtitle = { x, title.bottom, header.right - margin.left, header.bottom };
    SelectObject(dc, g_state.subtitleFont);
    SetTextColor(dc, RGB(0xDD, 0xEB, 0xF8));
    DrawTextW(dc, UiText::Pick(L"入力した日本語を、その場で翻訳", L"Translate Japanese right where you type"), -1, &subtitle, DT_SINGLELINE | DT_TOP | DT_NOPREFIX);
    SelectObject(dc, oldFont);

    // Footer band behind the buttons.
    RECT footer = GetItemRect(dialog, IDC_FOOTER);
    FillRect(dc, &footer, g_state.footerBrush);
    RECT divider = { footer.left, footer.top, footer.right, footer.top + 1 };
    HBRUSH dividerBrush = CreateSolidBrush(kDivider);
    FillRect(dc, &divider, dividerBrush);
    DeleteObject(dividerBrush);
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_INITDIALOG:
        {
            SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_state.largeIcon));
            SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_state.smallIcon));
            CreateFonts(dialog);
            CreateLanguageCheckBoxes(dialog);

            SendDlgItemMessageW(dialog, IDC_USAGE_BAR, PBM_SETRANGE32, 0, 1000);
            SetDlgItemTextW(dialog, IDC_USAGE_HELP, UiText::Pick(
                L"変換中に Shift で翻訳 ・ 文字を選択して Shift+変換 で選択範囲を翻訳\n"
                L"タスクバーの IME アイコンを右クリック →「翻訳の設定」でいつでも開けます",
                L"Shift while converting translates  ·  select text and press Shift+Henkan to translate it\n"
                L"Right-click the IME icon in the taskbar → \"Translation Settings\" to open this window"));
            SendDlgItemMessageW(dialog, IDC_DEEPL_KEY, EM_SETPASSWORDCHAR, kPasswordChar, 0);
            SendDlgItemMessageW(dialog, IDC_GOOGLE_KEY, EM_SETPASSWORDCHAR, kPasswordChar, 0);
            SendDlgItemMessageW(dialog, IDC_DEEPL_KEY, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"xxxxxxxx-...:fx"));
            SendDlgItemMessageW(dialog, IDC_GOOGLE_KEY, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"AIza..."));

            LoadIntoDialog(dialog);
            StartUsageQuery(dialog);
        }
        return TRUE;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(dialog, &ps);
            PaintChrome(dialog, dc);
            EndPaint(dialog, &ps);
        }
        return TRUE;

    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(g_state.backgroundBrush);

    case WM_CTLCOLORSTATIC:
        {
            HDC dc = reinterpret_cast<HDC>(wParam);
            int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            SetBkMode(dc, TRANSPARENT);
            switch (id)
            {
            case IDC_TEST_RESULT:
                SetTextColor(dc, g_state.testResultColor);
                break;
            case IDC_USAGE_TEXT:
            case IDC_LANGUAGE_HINT:
            case IDC_USAGE_HELP:
            case IDC_KEY_PROTECTION:
                SetTextColor(dc, kMuted);
                break;
            default:
                SetTextColor(dc, kText);
                break;
            }
            return reinterpret_cast<INT_PTR>(g_state.backgroundBrush);
        }

    case WM_CTLCOLORBTN:
        {
            int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            return reinterpret_cast<INT_PTR>((id == IDOK || id == IDCANCEL) ? g_state.footerBrush : g_state.backgroundBrush);
        }

    case WM_DPICHANGED:
        // The dialog rescales itself; refresh the derived fonts once its font has changed.
        PostMessageW(dialog, WM_APP_REFRESH_FONTS, 0, 0);
        return FALSE;

    case WM_APP_REFRESH_FONTS:
        CreateFonts(dialog);
        InvalidateRect(dialog, nullptr, TRUE);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_DEEPL_SHOW:
            TogglePasswordVisibility(dialog, IDC_DEEPL_SHOW, IDC_DEEPL_KEY);
            return TRUE;
        case IDC_GOOGLE_SHOW:
            TogglePasswordVisibility(dialog, IDC_GOOGLE_SHOW, IDC_GOOGLE_KEY);
            return TRUE;
        case IDC_TEST:
            StartConnectionTest(dialog);
            return TRUE;
        case IDC_DEEPL_KEY:
        case IDC_GOOGLE_KEY:
        case IDC_ENGINE_DEEPL:
        case IDC_ENGINE_GOOGLE:
            if (HIWORD(wParam) == EN_CHANGE || HIWORD(wParam) == BN_CLICKED)
            {
                SetTestResult(dialog, std::wstring(), kMuted);
            }
            return TRUE;
        case IDOK:
            {
                TranslationConfig config = ConfigFromDialog(dialog);
                if (config.targetLanguages.empty())
                {
                    MessageBoxW(dialog, UiText::Pick(L"翻訳先の言語を 1 つ以上選んでください。", L"Choose at least one target language."), WindowTitle(), MB_ICONINFORMATION);
                    return TRUE;
                }
                if (!TranslationConfig::Save(config))
                {
                    MessageBoxW(dialog, UiText::Pick(L"設定を保存できませんでした。", L"The settings could not be saved."), WindowTitle(), MB_ICONERROR);
                    return TRUE;
                }
                EndDialog(dialog, IDOK);
            }
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_APP_TEST_RESULT:
        {
            std::unique_ptr<TestOutcome> outcome(reinterpret_cast<TestOutcome*>(lParam));
            SetTestResult(dialog, outcome->message, outcome->succeeded ? kSuccess : kError);
            EnableWindow(GetDlgItem(dialog, IDC_TEST), TRUE);
        }
        return TRUE;

    case WM_APP_USAGE:
        {
            std::unique_ptr<UsageOutcome> outcome(reinterpret_cast<UsageOutcome*>(lParam));
            HWND bar = GetDlgItem(dialog, IDC_USAGE_BAR);
            if (!outcome->succeeded)
            {
                SendMessageW(bar, PBM_SETPOS, 0, 0);
                SetDlgItemTextW(dialog, IDC_USAGE_TEXT, UiText::Pick(L"取得できません", L"Unavailable"));
                return TRUE;
            }

            const DeepLUsage& usage = outcome->usage;
            int permille = (usage.characterLimit > 0)
                ? static_cast<int>((std::min)(1000ULL, usage.characterCount * 1000ULL / usage.characterLimit))
                : 0;
            SendMessageW(bar, PBM_SETSTATE, (permille >= 900) ? PBST_ERROR : (permille >= 750) ? PBST_PAUSED : PBST_NORMAL, 0);
            SendMessageW(bar, PBM_SETPOS, permille, 0);
            std::wstring text = FormatCount(usage.characterCount) + L" / " + FormatCount(usage.characterLimit) + UiText::Pick(L" 字", L" chars");
            SetDlgItemTextW(dialog, IDC_USAGE_TEXT, text.c_str());
        }
        return TRUE;
    }
    return FALSE;
}

} // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR commandLine, _In_ int)
{
    // mozc_translation_settings.exe --protect-keys: re-encrypt keys stored in an older format
    // and exit (used by Set-TranslationConfig.ps1). Exit code 0 on success.
    if (commandLine && wcsstr(commandLine, L"--protect-keys"))
    {
        return (TranslationConfig::ProtectLegacyKeys() < 0) ? 1 : 0;
    }

    // One settings window at a time.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"TranslationIME.Settings");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND existing = FindWindowW(L"#32770", WindowTitle());
        if (existing)
        {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(mutex);
        return 0;
    }

    // Keys saved by older versions (plain text, DPAPI only) are re-encrypted the first time the
    // settings open. Creating the TPM key the first time takes about half a second.
    TranslationConfig::ProtectLegacyKeys();

    // The dialog resource (Japanese or English) follows the same language as the texts.
    SetThreadUILanguage(UiText::IsJapanese() ? MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT)
                                             : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));

    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&controls);

    g_state.backgroundBrush = CreateSolidBrush(kBackground);
    g_state.footerBrush = CreateSolidBrush(kFooterBackground);
    g_state.brandBrush = CreateSolidBrush(kBrand);
    LoadIconWithScaleDown(instance, MAKEINTRESOURCEW(IDI_APP), GetSystemMetrics(SM_CXICON) * 2, GetSystemMetrics(SM_CYICON) * 2, &g_state.largeIcon);
    LoadIconWithScaleDown(instance, MAKEINTRESOURCEW(IDI_APP), GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), &g_state.smallIcon);

    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, DialogProc, 0);

    DeleteFonts();
    DeleteObject(g_state.backgroundBrush);
    DeleteObject(g_state.footerBrush);
    DeleteObject(g_state.brandBrush);
    if (g_state.largeIcon) DestroyIcon(g_state.largeIcon);
    if (g_state.smallIcon) DestroyIcon(g_state.smallIcon);
    if (mutex) CloseHandle(mutex);
    return 0;
}
