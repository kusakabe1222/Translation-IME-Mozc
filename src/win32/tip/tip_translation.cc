// Translation IME addition to Mozc. Distributed under the same terms as
// Mozc (BSD 3-Clause License, see LICENSE at the top of the repository).

#include "win32/tip/tip_translation.h"

#include <InputScope.h>
#include <msctf.h>
#include <wil/com.h>
#include <windows.h>
#include <richedit.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/process.h"
#include "base/win32/com.h"
#include "base/win32/wide_char.h"
#include "client/client_interface.h"
#include "protocol/candidate_window.pb.h"
#include "protocol/commands.pb.h"
#include "win32/tip/tip_dll_module.h"
#include "win32/tip/tip_edit_session.h"
#include "win32/tip/tip_private_context.h"
#include "win32/tip/tip_range_util.h"
#include "win32/tip/tip_surrounding_text.h"
#include "win32/tip/tip_text_service.h"
#include "win32/tip/tip_translation_logic.h"
#include "win32/tip/translation/HttpTranslator.h"
#include "win32/tip/translation/TranslationCache.h"
#include "win32/tip/translation/TranslationConfig.h"
#include "win32/tip/translation/UiText.h"

namespace mozc {
namespace win32 {
namespace tsf {
namespace {

using ::mozc::commands::CandidateWindow;
using ::mozc::commands::Output;
using ::mozc::commands::Result;
using ::mozc::commands::SessionCommand;

#if defined(GOOGLE_JAPANESE_INPUT_BUILD)
constexpr char kSettingsExecutable[] = "GoogleIMEJaTranslationSettings.exe";
#else
constexpr char kSettingsExecutable[] = "mozc_translation_settings.exe";
#endif

constexpr wchar_t kWindowClassName[] = L"MozcTranslationIME.Results";
constexpr UINT kResultsMessage = WM_APP + 0x51;
const wchar_t* NoApiKeyMessage() {
  return UiText::Pick(
      L"APIキーが未設定です (IME アイコンを右クリック →「翻訳の設定」)",
      L"No API key is set (right-click the IME icon → \"Translation Settings\")");
}
const wchar_t* LoadingMessage() {
  return UiText::Pick(L"翻訳しています…", L"Translating…");
}
// A candidate shows at most this much of a translation (a whole selected
// document would make the window taller than the screen); the commit is whole.
constexpr size_t kPreviewLines = 4;
constexpr size_t kPreviewWidth = 100;

// State of the translation UI on this thread (one text service per thread).
struct State {
  TipTextService* text_service = nullptr;  // valid until OnDeactivate
  wil::com_ptr_nothrow<ITfContext> context;
  bool active = false;   // translations replace the candidates
  bool loading = false;
  bool shift_pending = false;  // Shift is down and nothing else was pressed
  bool selection = false;  // translating the application's selection, not the composition
  std::wstring pending_selection;  // selected text found for Shift+Henkan (Decide)
  HWND pending_control = nullptr;  // the edit control it was read from, if read directly
  HWND edit_control = nullptr;     // same, for the selection being translated
  std::wstring source;
  std::wstring source_language;  // "JA", or empty to auto-detect
  std::wstring engine;
  std::wstring key;      // cache key of the shown translations
  std::vector<TranslationItem> items;   // successful translations
  std::vector<std::wstring> languages;  // requested languages, in display order
  std::wstring message;  // status or error when there are no items
  std::wstring note;     // languages that failed
  int selected = 0;
  bool moved = false;  // the user moved the selection (it then follows its language)
  UINT request_id = 0;
  std::wstring pending_key;  // cache key of the request in flight
  HWND window = nullptr;
  std::shared_ptr<CTranslationMailbox> mailbox;
  CTranslationCache cache;
  // While the translations are shown, the private context's last output is a
  // copy of |original| with the candidate window replaced; |overlay_label| (the
  // footer) tells whether that copy is still the one in place.
  Output original;
  std::string overlay_label;
};

thread_local State* g_state = nullptr;

State* GetState() {
  if (g_state == nullptr) {
    g_state = new State();
  }
  return g_state;
}

// Diagnostic trace for support, off unless HKCU\Software\TranslationIME has
// Trace = 1: appends key decisions (key codes and lengths, never text) to
// %TEMP%\TranslationIME-trace.log of the application's process.
bool TraceEnabled() {
  static const bool enabled = [] {
    DWORD value = 0;
    DWORD size = sizeof(value);
    return ::RegGetValueW(HKEY_CURRENT_USER, L"Software\\TranslationIME", L"Trace",
                          RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value != 0;
  }();
  return enabled;
}

void Trace(const char* format, ...) {
  if (!TraceEnabled()) {
    return;
  }
  char line[512];
  va_list args;
  va_start(args, format);
  const int length = vsnprintf(line, sizeof(line) - 2, format, args);
  va_end(args);
  if (length <= 0) {
    return;
  }
  wchar_t path[MAX_PATH + 32] = {};
  const DWORD dir = ::GetTempPathW(MAX_PATH, path);
  if (dir == 0 || dir > MAX_PATH) {
    return;
  }
  wcscat_s(path, L"TranslationIME-trace.log");
  HANDLE file = ::CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  char stamped[600];
  const int total = snprintf(stamped, sizeof(stamped), "%lu pid=%lu %s\r\n",
                             ::GetTickCount(), ::GetCurrentProcessId(), line);
  DWORD written = 0;
  ::WriteFile(file, stamped, static_cast<DWORD>(std::max(total, 0)), &written, nullptr);
  ::CloseHandle(file);
}

bool IsDown(int vk) { return (::GetKeyState(vk) & 0x8000) != 0; }

TranslationModifiers CurrentModifiers() {
  TranslationModifiers modifiers;
  modifiers.shift = IsDown(VK_SHIFT);
  modifiers.control = IsDown(VK_CONTROL);
  modifiers.alt = IsDown(VK_MENU);
  return modifiers;
}

std::wstring PreeditText(const Output& output) {
  std::string text;
  if (output.has_preedit()) {
    for (const auto& segment : output.preedit().segment()) {
      text += segment.value();
    }
  }
  return Utf8ToWide(text);
}

void RefreshUi(State* state) {
  if (state->text_service != nullptr) {
    state->text_service->PostUIUpdateMessage();
  }
}

TipPrivateContext* GetPrivateContext(State* state) {
  if (state->text_service == nullptr || !state->context) {
    return nullptr;
  }
  return state->text_service->GetPrivateContext(state->context.get());
}

// True while the last output is still the translation overlay we installed
// (Mozc replaces it when the composition changes some other way, e.g. a click).
bool OverlayInPlace(State* state) {
  TipPrivateContext* private_context = GetPrivateContext(state);
  if (private_context == nullptr) {
    return false;
  }
  const Output& output = private_context->last_output();
  return output.has_candidate_window() &&
         output.candidate_window().has_footer() &&
         output.candidate_window().footer().label() == state->overlay_label;
}

void BuildCandidateWindow(State* state, Output* output);

void ShowOverlay(State* state) {
  TipPrivateContext* private_context = GetPrivateContext(state);
  if (private_context == nullptr) {
    return;
  }
  Output output = state->original;
  BuildCandidateWindow(state, &output);
  state->overlay_label = output.candidate_window().footer().label();
  *private_context->mutable_last_output() = std::move(output);
  RefreshUi(state);
}

void EndTranslation(State* state) {
  // Put Mozc's own candidates back (unless Mozc already replaced the overlay).
  if (state->active && OverlayInPlace(state)) {
    *GetPrivateContext(state)->mutable_last_output() = state->original;
    RefreshUi(state);
  }
  state->original.Clear();
  state->overlay_label.clear();
  state->selection = false;
  state->edit_control = nullptr;
  state->active = false;
  state->loading = false;
  state->context.reset();
  state->items.clear();
  state->languages.clear();
  state->message.clear();
  state->note.clear();
  state->selected = 0;
  state->moved = false;
}

// While items are added, a selection the user moved stays on its language;
// otherwise the first item stays selected.
std::wstring SelectedLanguage(const State* state) {
  return state->moved && state->selected < static_cast<int>(state->items.size())
             ? state->items[state->selected].language
             : std::wstring();
}

// An early result: shows the languages that have arrived while the rest load.
// Failed languages are reported with the complete result.
void AddEarlyItems(State* state, const std::vector<TranslationItem>& items) {
  const std::wstring selected = SelectedLanguage(state);
  InsertInLanguageOrder(state->languages, items, &state->items);
  state->selected = IndexOfLanguage(state->items, selected);
  if (!state->items.empty()) {
    state->message.clear();
    state->note = LoadingMessage() +
                  ProgressSuffix(state->items.size(), state->languages.size());
  }
}

void ShowItems(State* state, const std::vector<TranslationItem>& items,
               const std::wstring& error) {
  const std::wstring selected = SelectedLanguage(state);
  state->loading = false;
  state->items.clear();
  for (const TranslationItem& item : items) {
    if (item.succeeded) {
      state->items.push_back(item);
    }
  }
  const std::wstring failed = FailedLanguages(items);
  state->note = failed.empty() ? std::wstring()
                               : UiText::Pick(L"翻訳できなかった言語: ", L"Not translated: ") + failed;
  state->message.clear();
  if (state->items.empty()) {
    state->message = error.empty() ? UiText::Pick(L"翻訳に失敗しました", L"Translation failed") : error;
  }
  state->selected = IndexOfLanguage(state->items, selected);
}

void OnResults(State* state) {
  if (!state->mailbox) {
    return;
  }
  for (std::unique_ptr<TranslationResult>& result : state->mailbox->TakeAll()) {
    if (!result->complete) {
      if (state->active && state->loading && result->requestId == state->request_id) {
        AddEarlyItems(state, result->items);
        ShowOverlay(state);
      }
      continue;
    }
    // Only complete results are cached, so a failed language is retried.
    TranslationConfig config = TranslationConfig::Load();
    const std::wstring key = CTranslationCache::MakeKey(
        config, result->sourceLanguage, result->sourceText);
    if (result->AllSucceeded()) {
      state->cache.Store(key, result->items);
    }
    if (key == state->pending_key) {
      state->pending_key.clear();
    }
    if (state->active && state->loading && result->requestId == state->request_id) {
      ShowItems(state, result->items, result->FirstError());
      ShowOverlay(state);
    }
  }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  if (message == kResultsMessage && g_state != nullptr &&
      g_state->window == window) {
    OnResults(g_state);
    return 0;
  }
  return ::DefWindowProcW(window, message, wparam, lparam);
}

HMODULE GetThisModule() {
  HMODULE module = nullptr;
  ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&WindowProc), &module);
  return module;
}

bool EnsureWindow(State* state) {
  if (state->window != nullptr && ::IsWindow(state->window)) {
    return true;
  }
  static std::atomic<bool> registered(false);
  HMODULE module = GetThisModule();
  if (!registered.exchange(true)) {
    WNDCLASSEXW window_class = {sizeof(window_class)};
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = module;
    window_class.lpszClassName = kWindowClassName;
    ::RegisterClassExW(&window_class);
  }
  state->window = ::CreateWindowExW(0, kWindowClassName, L"", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, module, nullptr);
  if (state->window == nullptr) {
    return false;
  }
  state->mailbox =
      std::make_shared<CTranslationMailbox>(state->window, kResultsMessage);
  return true;
}

class SecureFieldReader final : public TipComImplements<ITfEditSession> {
 public:
  explicit SecureFieldReader(wil::com_ptr_nothrow<ITfContext> context)
      : context_(std::move(context)) {}

  bool secure() const { return secure_; }

 private:
  STDMETHODIMP DoEditSession(TfEditCookie read_cookie) override {
    wil::com_ptr_nothrow<ITfRange> range;
    HRESULT result = TipRangeUtil::GetDefaultSelection(context_.get(),
                                                       read_cookie, &range, nullptr);
    if (FAILED(result)) {
      return result;
    }
    std::vector<InputScope> input_scopes;
    result = TipRangeUtil::GetInputScopes(range.get(), read_cookie, &input_scopes);
    if (FAILED(result)) {
      return result;
    }
    for (const InputScope scope : input_scopes) {
      secure_ = secure_ || IsSecureInputScope(scope);
    }
    return S_OK;
  }

  wil::com_ptr_nothrow<ITfContext> context_;
  bool secure_ = false;
};

// True when the focused field is a password or PIN field. Mozc starts such
// fields in direct input, but Hankaku/Zenkaku still turns kana input on there.
// (Win32 ES_PASSWORD edits get no IME input at all.)
bool IsSecureField(TipTextService* text_service, ITfContext* context) {
  auto reader = MakeComPtr<SecureFieldReader>(context);
  HRESULT edit_session_result = S_OK;
  if (!reader ||
      FAILED(context->RequestEditSession(text_service->GetClientID(),
                                         reader.get(), TF_ES_SYNC | TF_ES_READ,
                                         &edit_session_result)) ||
      FAILED(edit_session_result)) {
    return false;
  }
  return reader->secure();
}

bool IsComposing(TipTextService* text_service, ITfContext* context) {
  TipPrivateContext* private_context = text_service->GetPrivateContext(context);
  return private_context != nullptr &&
         !PreeditText(private_context->last_output()).empty();
}

// Shows the translations of |source| in the candidate window. With |selection|
// the text is the application's selection, replaced on commit; otherwise it is
// the composition (as currently converted).
void BeginTranslation(TipTextService* text_service, ITfContext* context,
                      const std::wstring& source, bool selection) {
  State* state = GetState();
  TipPrivateContext* private_context = text_service->GetPrivateContext(context);
  if (private_context == nullptr || source.empty()) {
    return;
  }

  EndTranslation(state);
  state->text_service = text_service;
  state->context = context;
  state->active = true;
  state->selection = selection;
  state->source = source;
  state->original = private_context->last_output();

  TranslationConfig config = TranslationConfig::Load();
  state->engine = (config.engine == TranslationEngine::None)
                      ? std::wstring()
                      : std::wstring(config.EngineName());
  if (config.engine == TranslationEngine::None) {
    state->message = NoApiKeyMessage();
    ShowOverlay(state);
    return;
  }

  // A selection mainly in another language is auto-detected and translated
  // into Japanese. Any kana or kanji is not enough: a selected document of
  // English or code often has a Japanese comment.
  const bool japanese = !selection || SelectionIsJapanese(source);
  state->source_language =
      japanese ? std::wstring(HttpTranslator::Japanese) : std::wstring();
  if (!japanese) {
    config.targetLanguages.assign(1, HttpTranslator::Japanese);
  }
  state->languages = config.targetLanguages;

  state->key = CTranslationCache::MakeKey(config, state->source_language, source);
  std::vector<TranslationItem> cached;
  if (state->cache.Lookup(state->key, cached)) {
    ShowItems(state, cached, std::wstring());
    ShowOverlay(state);
    return;
  }

  state->loading = true;
  state->message = LoadingMessage();
  // One request per text at a time: pressing Shift again shows the same request.
  if (state->pending_key != state->key) {
    state->request_id++;
    if (EnsureWindow(state) &&
        HttpTranslator::TranslateAsync(config, source, state->source_language,
                                       state->request_id, state->mailbox,
                                       /*progressive=*/true)) {
      state->pending_key = state->key;
    } else {
      state->loading = false;
      state->message = UiText::Pick(L"翻訳を開始できませんでした", L"Could not start the translation");
    }
  }
  ShowOverlay(state);
}

// Shift: translate the whole composition (as currently converted).
void StartTranslation(TipTextService* text_service, ITfContext* context) {
  TipPrivateContext* private_context = text_service->GetPrivateContext(context);
  if (private_context != nullptr) {
    BeginTranslation(text_service, context,
                     PreeditText(private_context->last_output()), false);
  }
}

// The selection of the focused standard edit control (EDIT or RichEdit), read
// directly. Classic Win32 applications reach the IME through IMM32, and there
// RichEdit hands only short selections (about 100 characters) to the IME while
// EDIT hands none. The TIP runs on the application's own thread, so it can ask
// its focused control. Password controls are never read.
enum class EditControl { kNone, kEdit, kRichEdit };

EditControl KindOfEditControl(HWND window) {
  if (window == nullptr || !::IsWindowUnicode(window) ||
      (::GetWindowLongW(window, GWL_STYLE) & ES_PASSWORD) != 0) {
    return EditControl::kNone;
  }
  wchar_t class_name[32] = {};
  ::GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
  if (_wcsnicmp(class_name, L"RichEdit", 8) == 0) {
    return EditControl::kRichEdit;
  }
  return _wcsicmp(class_name, L"Edit") == 0 ? EditControl::kEdit
                                            : EditControl::kNone;
}

std::wstring SelectedTextOfEditControl(HWND focus) {
  const EditControl kind = KindOfEditControl(focus);
  if (kind == EditControl::kRichEdit) {
    CHARRANGE range = {};
    ::SendMessageW(focus, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&range));
    if (range.cpMax <= range.cpMin) {
      return std::wstring();
    }
    std::wstring text(static_cast<size_t>(range.cpMax - range.cpMin) + 1, L'\0');
    const LRESULT length = ::SendMessageW(focus, EM_GETSELTEXT, 0,
                                          reinterpret_cast<LPARAM>(&text[0]));
    text.resize(static_cast<size_t>(std::clamp<LRESULT>(length, 0, text.size() - 1)));
    return text;
  }
  if (kind == EditControl::kEdit) {
    DWORD start = 0, end = 0;
    ::SendMessageW(focus, EM_GETSEL, reinterpret_cast<WPARAM>(&start),
                   reinterpret_cast<LPARAM>(&end));
    const LRESULT length = ::SendMessageW(focus, WM_GETTEXTLENGTH, 0, 0);
    if (end <= start || length <= 0 || end > static_cast<DWORD>(length)) {
      return std::wstring();
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const LRESULT copied = ::SendMessageW(focus, WM_GETTEXT, text.size(),
                                          reinterpret_cast<LPARAM>(&text[0]));
    text.resize(static_cast<size_t>(std::max<LRESULT>(copied, 0)));
    return end <= text.size() ? text.substr(start, end - start) : std::wstring();
  }
  return std::wstring();
}

// The selected text when there is a selection and no composition, else empty.
// |*control| is the edit control it was read from directly, else nullptr.
std::wstring SelectedText(TipTextService* text_service, ITfContext* context,
                          HWND* control) {
  *control = nullptr;
  TipSurroundingTextInfo info;
  const bool read = TipSurroundingText::Get(text_service, context, &info);
  Trace("selection: tsf=%d composing=%d has=%d length=%zu", read ? 1 : 0,
        info.in_composition ? 1 : 0, info.has_selected_text ? 1 : 0,
        info.selected_text.size());
  if (read) {
    if (info.in_composition) {
      return std::wstring();
    }
    if (info.has_selected_text && !info.selected_text.empty()) {
      return info.selected_text;
    }
  }
  HWND focus = ::GetFocus();
  std::wstring text = SelectedTextOfEditControl(focus);
  Trace("selection: control kind=%d length=%zu", static_cast<int>(KindOfEditControl(focus)),
        text.size());
  if (!text.empty()) {
    *control = focus;
  }
  return text;
}

// Replaces the selection of |control| (a selection read from it directly) with
// |text|, as one undoable edit. Through IMM32 a long result would be cut, so
// the control does it itself. False when |control| is no longer the place.
bool ReplaceSelectionOfEditControl(HWND control, const std::wstring& text) {
  if (control == nullptr || control != ::GetFocus()) {
    return false;
  }
  const EditControl kind = KindOfEditControl(control);
  if (kind == EditControl::kNone) {
    return false;
  }
  std::wstring replacement;
  replacement.reserve(text.size() + 16);
  for (size_t i = 0; i < text.size(); ++i) {
    // An EDIT control shows a lone \n as a box: give it \r\n.
    if (kind == EditControl::kEdit && text[i] == L'\n' &&
        (i == 0 || text[i - 1] != L'\r')) {
      replacement += L'\r';
    }
    replacement += text[i];
  }
  ::SendMessageW(control, EM_REPLACESEL, TRUE,
                 reinterpret_cast<LPARAM>(replacement.c_str()));
  return true;
}

// Commits a translation in place of the composition. |async| when called
// outside a key event (an edit session cannot be synchronous there).
void Commit(TipTextService* text_service, ITfContext* context, int index,
            bool async = false) {
  State* state = GetState();
  if (index < 0 || index >= static_cast<int>(state->items.size())) {
    return;
  }
  const std::wstring text = state->items[index].text;
  const bool selection = state->selection;
  const HWND edit_control = state->edit_control;
  Output output = state->original;
  state->overlay_label.clear();   // nothing to restore: the commit replaces it
  EndTranslation(state);

  TipPrivateContext* private_context = text_service->GetPrivateContext(context);
  if (private_context == nullptr) {
    return;
  }
  if (selection) {
    // No composition: committing replaces the selected text.
    output.clear_candidate_window();
    output.clear_preedit();
    output.clear_all_candidate_words();
    if (ReplaceSelectionOfEditControl(edit_control, text)) {
      // Done by the control; the output only closes the candidate window.
      output.set_consumed(true);
      if (async) {
        TipEditSession::OnOutputReceivedAsync(text_service, context, std::move(output));
      } else {
        TipEditSession::OnOutputReceivedSync(text_service, context, std::move(output));
      }
      return;
    }
  } else {
    // Drop the composition on the server, then commit the translation instead.
    output.Clear();
    SessionCommand command;
    command.set_type(SessionCommand::REVERT);
    if (!private_context->GetClient()->SendCommand(command, &output)) {
      output.Clear();
    }
  }
  output.set_consumed(true);
  Result* result = output.mutable_result();
  result->set_type(Result::STRING);
  result->set_value(WideToUtf8(text));
  if (async) {
    TipEditSession::OnOutputReceivedAsync(text_service, context, std::move(output));
  } else {
    TipEditSession::OnOutputReceivedSync(text_service, context, std::move(output));
  }
}

using Action = TranslationAction;

// Decides what a key does. |index| is set for kCommit.
Action Decide(TipTextService* text_service, ITfContext* context,
              bool is_key_down, WPARAM vk, int* index) {
  State* state = GetState();
  if (state->active && !OverlayInPlace(state)) {
    EndTranslation(state);   // Mozc moved on (e.g. a click committed the text)
  }
  const bool shown = state->active && state->context.get() == context;
  *index = state->selected;
  Trace("key vk=0x%02X %s active=%d shown=%d selection=%d items=%zu shift_pending=%d",
        static_cast<unsigned>(vk), is_key_down ? "down" : "up", state->active ? 1 : 0,
        shown ? 1 : 0, state->selection ? 1 : 0, state->items.size(),
        state->shift_pending ? 1 : 0);

  // The checks that need an edit session (IsComposing, IsSecureField,
  // SelectedText) come last, so that they run only for the trigger keys.
  if (!is_key_down) {
    if (vk == VK_SHIFT && state->shift_pending) {
      if (shown) {
        return Action::kToggle;   // back to the conversion
      }
      if (IsComposing(text_service, context) && TranslationConfig::Load().enabled &&
          !IsSecureField(text_service, context)) {
        return Action::kToggle;
      }
    }
    // While the translations are shown, key releases are ours as well. Mozc
    // answers a release (Shift after Shift+Henkan or Shift+Tab, for one) with
    // a new output that would replace the translations, and the next key would
    // then reach the application: Enter would replace the selected text.
    return shown ? Action::kWait : Action::kNone;
  }

  if (!shown) {
    if (IsSelectionTranslationKey(vk, CurrentModifiers())) {
      Trace("Shift+Henkan: composing=%d enabled=%d secure=%d",
            IsComposing(text_service, context) ? 1 : 0,
            TranslationConfig::Load().enabled ? 1 : 0,
            IsSecureField(text_service, context) ? 1 : 0);
    }
    if (IsSelectionTranslationKey(vk, CurrentModifiers()) &&
        !IsComposing(text_service, context) &&
        TranslationConfig::Load().enabled && !IsSecureField(text_service, context)) {
      state->pending_selection =
          SelectedText(text_service, context, &state->pending_control);
      if (!state->pending_selection.empty()) {
        return Action::kTranslateSelection;
      }
    }
    return Action::kNone;
  }
  return DecideShownKeyDown(vk, CurrentModifiers(), state->items.size(),
                            state->selected, index, state->selection);
}

}  // namespace

namespace {

// Applications call either OnTestKey* then OnKey*, or only OnKey* (Notepad
// does the latter), so both track the keys the same way.
void TrackKey(State* state, bool is_key_down, WPARAM wparam) {
  if (is_key_down) {
    // The first key typed loads the API keys in the background (TPM).
    TranslationConfig::PreloadKeysAsync();
    state->shift_pending = StartsShiftTap(wparam, CurrentModifiers());
  }
}

}  // namespace

bool TipTranslation::OnTestKey(TipTextService* text_service,
                               ITfContext* context, bool is_key_down,
                               WPARAM wparam, BOOL* eaten) {
  State* state = GetState();
  TrackKey(state, is_key_down, wparam);

  int index = 0;
  const Action action = Decide(text_service, context, is_key_down, wparam, &index);
  switch (action) {
    case Action::kNone:
      return false;
    case Action::kCancel:
      if (PassesThroughAfterCancel(wparam, !state->items.empty())) {
        // Close the translations now; Mozc then handles the key as usual.
        EndTranslation(state);
        return false;
      }
      *eaten = TRUE;
      return true;
    default:
      *eaten = TRUE;
      return true;
  }
}

bool TipTranslation::OnKey(TipTextService* text_service, ITfContext* context,
                           bool is_key_down, WPARAM wparam, BOOL* eaten) {
  State* state = GetState();
  TrackKey(state, is_key_down, wparam);
  int index = 0;
  const Action action = Decide(text_service, context, is_key_down, wparam, &index);
  const int count = static_cast<int>(state->items.size());
  switch (action) {
    case Action::kNone:
      return false;
    case Action::kToggle:
      state->shift_pending = false;
      if (state->active && state->context.get() == context) {
        EndTranslation(state);
      } else {
        StartTranslation(text_service, context);
      }
      break;
    case Action::kTranslateSelection: {
      const std::wstring text = state->pending_selection;
      const HWND control = state->pending_control;
      state->pending_selection.clear();
      state->pending_control = nullptr;
      BeginTranslation(text_service, context, text, true);
      state->edit_control = control;
      break;
    }
    case Action::kNext:
      if (count > 0) {
        state->selected = NextIndex(state->selected, count);
        state->moved = true;
        ShowOverlay(state);
      }
      break;
    case Action::kPrevious:
      if (count > 0) {
        state->selected = PreviousIndex(state->selected, count);
        state->moved = true;
        ShowOverlay(state);
      }
      break;
    case Action::kCommit:
      Commit(text_service, context, index);
      break;
    case Action::kCancel:
      if (PassesThroughAfterCancel(wparam, count > 0)) {
        EndTranslation(state);
        return false;
      }
      EndTranslation(state);
      break;
    case Action::kWait:
      break;
  }
  *eaten = TRUE;
  return true;
}

namespace {

// The translations shown in |context|, or nullptr.
State* ShownIn(ITfContext* context) {
  State* state = g_state;
  if (state == nullptr || !state->active || state->context.get() != context) {
    return nullptr;
  }
  if (!OverlayInPlace(state)) {
    EndTranslation(state);   // Mozc moved on
    return nullptr;
  }
  return state;
}

}  // namespace

bool TipTranslation::OnUiElementSelect(TipTextService* text_service,
                                       ITfContext* context, UINT index) {
  State* state = ShownIn(context);
  if (state == nullptr) {
    return false;
  }
  if (index < state->items.size()) {
    state->selected = static_cast<int>(index);
    state->moved = true;
    ShowOverlay(state);
  }
  return true;
}

bool TipTranslation::OnUiElementFinalize(TipTextService* text_service,
                                         ITfContext* context, bool commit) {
  State* state = ShownIn(context);
  if (state == nullptr) {
    return false;
  }
  if (commit && !state->items.empty()) {
    Commit(text_service, context, state->selected, /*async=*/true);
  } else {
    EndTranslation(state);   // back to the conversion (or the selection)
  }
  return true;
}

namespace {

void BuildCandidateWindow(State* state, Output* output) {
  CandidateWindow* window = output->mutable_candidate_window();
  const uint32_t position = window->has_position() ? window->position() : 0;
  window->Clear();
  window->set_position(position);
  window->set_category(commands::CONVERSION);
  window->set_display_type(commands::MAIN);
  window->set_page_size(9);

  if (!state->items.empty()) {
    for (size_t i = 0; i < state->items.size(); ++i) {
      CandidateWindow::Candidate* candidate = window->add_candidate();
      candidate->set_index(static_cast<uint32_t>(i));
      candidate->set_id(static_cast<int32_t>(i));
      candidate->set_value(WideToUtf8(CandidatePreview(
          state->items[i].text, kPreviewLines, kPreviewWidth)));
      candidate->mutable_annotation()->set_description(
          WideToUtf8(state->items[i].language));
      if (i < 9) {
        candidate->mutable_annotation()->set_shortcut(std::to_string(i + 1));
      }
    }
    window->set_size(static_cast<uint32_t>(state->items.size()));
    window->set_focused_index(static_cast<uint32_t>(state->selected));
  } else {
    CandidateWindow::Candidate* candidate = window->add_candidate();
    candidate->set_index(0);
    candidate->set_id(-1);
    candidate->set_value(WideToUtf8(state->message));
    window->set_size(1);
  }

  commands::Footer* footer = window->mutable_footer();
  const std::wstring name = state->selection ? UiText::Pick(L"選択範囲を翻訳", L"Translate selection") : UiText::Pick(L"翻訳", L"Translate");
  const std::wstring title =
      state->engine.empty() ? name : name + L" (" + state->engine + L")";
  const std::wstring hints = state->selection ? UiText::Pick(L"  Enter: 置き換え  Esc: キャンセル", L"  Enter: replace  Esc: cancel")
                                              : UiText::Pick(L"  Enter: 確定  Shift: 戻る", L"  Enter: insert  Shift: back");
  footer->set_label(WideToUtf8(
      state->note.empty() ? title + hints : title + L"  " + state->note));
  footer->set_index_visible(!state->items.empty());

  // The candidate list UI element reads all_candidate_words: applications that
  // draw the candidates themselves and screen readers see the same list.
  commands::CandidateList* words = output->mutable_all_candidate_words();
  words->Clear();
  words->set_category(commands::CONVERSION);
  for (int i = 0; i < window->candidate_size(); ++i) {
    const CandidateWindow::Candidate& candidate = window->candidate(i);
    commands::CandidateWord* word = words->add_candidates();
    word->set_id(candidate.id());
    word->set_index(static_cast<uint32_t>(i));
    word->set_value(candidate.value());
    if (candidate.has_annotation()) {
      *word->mutable_annotation() = candidate.annotation();
    }
  }
  words->set_focused_index(state->items.empty() ? 0 : static_cast<uint32_t>(state->selected));
}

}  // namespace

void TipTranslation::OnDeactivate() {
  State* state = g_state;
  if (state == nullptr) {
    return;
  }
  g_state = nullptr;
  if (state->mailbox) {
    state->mailbox->Close();
  }
  if (state->window != nullptr) {
    ::DestroyWindow(state->window);
  }
  delete state;
}

bool TipTranslation::OpenSettings() {
  return Process::SpawnMozcProcess(kSettingsExecutable, "");
}

}  // namespace tsf
}  // namespace win32
}  // namespace mozc
