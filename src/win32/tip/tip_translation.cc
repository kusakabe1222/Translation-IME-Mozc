// Translation IME addition to Mozc. Distributed under the same terms as
// Mozc (BSD 3-Clause License, see LICENSE at the top of the repository).

#include "win32/tip/tip_translation.h"

#include <InputScope.h>
#include <msctf.h>
#include <wil/com.h>
#include <windows.h>

#include <atomic>
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
#include "win32/tip/translation/HttpTranslator.h"
#include "win32/tip/translation/TranslationCache.h"
#include "win32/tip/translation/TranslationConfig.h"

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
constexpr wchar_t kNoApiKeyMessage[] =
    L"APIキーが未設定です (IME アイコンを右クリック →「翻訳の設定」)";
constexpr wchar_t kLoadingMessage[] = L"翻訳しています…";

// State of the translation UI on this thread (one text service per thread).
struct State {
  TipTextService* text_service = nullptr;  // valid until OnDeactivate
  wil::com_ptr_nothrow<ITfContext> context;
  bool active = false;   // translations replace the candidates
  bool loading = false;
  bool shift_pending = false;  // Shift is down and nothing else was pressed
  bool selection = false;  // translating the application's selection, not the composition
  std::wstring pending_selection;  // selected text found for Shift+Henkan (Decide)
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

bool IsDown(int vk) { return (::GetKeyState(vk) & 0x8000) != 0; }

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

void SelectLanguage(State* state, const std::wstring& language) {
  state->selected = 0;
  for (size_t i = 0; !language.empty() && i < state->items.size(); ++i) {
    if (state->items[i].language == language) {
      state->selected = static_cast<int>(i);
    }
  }
}

size_t LanguageRank(const State* state, const std::wstring& language) {
  for (size_t i = 0; i < state->languages.size(); ++i) {
    if (state->languages[i] == language) {
      return i;
    }
  }
  return state->languages.size();
}

// An early result: shows the languages that have arrived while the rest load.
void AddEarlyItems(State* state, const std::vector<TranslationItem>& items) {
  const std::wstring selected = SelectedLanguage(state);
  for (const TranslationItem& item : items) {
    if (!item.succeeded) {
      continue;   // reported with the complete result
    }
    auto position = state->items.begin();
    while (position != state->items.end() &&
           LanguageRank(state, position->language) <= LanguageRank(state, item.language)) {
      ++position;
    }
    state->items.insert(position, item);
  }
  SelectLanguage(state, selected);
  if (!state->items.empty()) {
    state->message.clear();
    state->note = std::wstring(kLoadingMessage) + L" (" +
                  std::to_wstring(state->items.size()) + L"/" +
                  std::to_wstring(state->languages.size()) + L")";
  }
}

void ShowItems(State* state, const std::vector<TranslationItem>& items,
               const std::wstring& error) {
  const std::wstring selected = SelectedLanguage(state);
  state->loading = false;
  state->items.clear();
  std::wstring failed;
  for (const TranslationItem& item : items) {
    if (item.succeeded) {
      state->items.push_back(item);
    } else {
      failed += (failed.empty() ? L"" : L", ") + item.language;
    }
  }
  state->note = failed.empty() ? std::wstring()
                               : L"翻訳できなかった言語: " + failed;
  state->message.clear();
  if (state->items.empty()) {
    state->message = error.empty() ? L"翻訳に失敗しました" : error;
  }
  SelectLanguage(state, selected);
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

// Password and PIN fields: their text must never reach a translation API.
bool IsSecureInputScope(InputScope scope) {
  switch (scope) {
    case IS_PASSWORD:
    case IS_NUMERIC_PASSWORD:
    case IS_NUMERIC_PIN:
    case IS_ALPHANUMERIC_PIN:
    case IS_ALPHANUMERIC_PIN_SET:
      return true;
    default:
      return false;
  }
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
    state->message = kNoApiKeyMessage;
    ShowOverlay(state);
    return;
  }

  // A selection in another language is auto-detected and translated into Japanese.
  const bool japanese = !selection || HttpTranslator::LooksJapanese(source);
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
  state->message = kLoadingMessage;
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
      state->message = L"翻訳を開始できませんでした";
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

// The selected text when there is a selection and no composition, else empty.
std::wstring SelectedText(TipTextService* text_service, ITfContext* context) {
  TipSurroundingTextInfo info;
  if (!TipSurroundingText::Get(text_service, context, &info) ||
      info.in_composition || !info.has_selected_text) {
    return std::wstring();
  }
  return info.selected_text;
}

// Commits a translation in place of the composition.
void Commit(TipTextService* text_service, ITfContext* context, int index) {
  State* state = GetState();
  if (index < 0 || index >= static_cast<int>(state->items.size())) {
    return;
  }
  const std::wstring text = state->items[index].text;
  const bool selection = state->selection;
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
  TipEditSession::OnOutputReceivedSync(text_service, context, std::move(output));
}

enum class Action {
  kNone,      // not ours: Mozc handles the key
  kToggle,    // Shift released alone
  kTranslateSelection,  // Shift+Henkan with a selection
  kNext,
  kPrevious,
  kCommit,
  kCancel,
};

// Decides what a key does. |index| is set for kCommit.
Action Decide(TipTextService* text_service, ITfContext* context,
              bool is_key_down, WPARAM vk, int* index) {
  State* state = GetState();
  if (state->active && !OverlayInPlace(state)) {
    EndTranslation(state);   // Mozc moved on (e.g. a click committed the text)
  }
  const bool shown = state->active && state->context.get() == context;
  *index = state->selected;

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
    return Action::kNone;
  }

  if (!shown) {
    // Shift+Henkan translates the selection (Henkan alone is Mozc's reconversion).
    if (vk == VK_CONVERT && IsDown(VK_SHIFT) && !IsDown(VK_CONTROL) &&
        !IsDown(VK_MENU) && !IsComposing(text_service, context) &&
        TranslationConfig::Load().enabled && !IsSecureField(text_service, context)) {
      state->pending_selection = SelectedText(text_service, context);
      if (!state->pending_selection.empty()) {
        return Action::kTranslateSelection;
      }
    }
    return Action::kNone;
  }
  if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
    return Action::kNone;
  }
  if (IsDown(VK_CONTROL) || IsDown(VK_MENU)) {
    return Action::kCancel;
  }
  const bool has_items = !state->items.empty();
  switch (vk) {
    case VK_DOWN:
    case VK_SPACE:
    case VK_CONVERT:
      return Action::kNext;
    case VK_TAB:
      return IsDown(VK_SHIFT) ? Action::kPrevious : Action::kNext;
    case VK_UP:
      return Action::kPrevious;
    case VK_RETURN:
      // Nothing to commit yet: back to the conversion, which Enter commits.
      return has_items ? Action::kCommit : Action::kCancel;
    case VK_ESCAPE:
    case VK_BACK:
      return Action::kCancel;
    default:
      break;
  }
  if (vk >= '1' && vk <= '9' && !IsDown(VK_SHIFT)) {
    const int number = static_cast<int>(vk - '1');
    if (number < static_cast<int>(state->items.size())) {
      *index = number;
      return Action::kCommit;
    }
  }
  return Action::kCancel;
}

// Keys that end the translation UI without being consumed (e.g. typing goes on).
bool PassesThroughAfterCancel(WPARAM vk, bool has_items) {
  if (vk == VK_ESCAPE || vk == VK_BACK) {
    return false;
  }
  if (vk == VK_RETURN) {
    return !has_items;
  }
  return true;
}

}  // namespace

namespace {

// Applications call either OnTestKey* then OnKey*, or only OnKey* (Notepad
// does the latter), so both track the keys the same way.
void TrackKey(State* state, bool is_key_down, WPARAM wparam) {
  if (is_key_down) {
    // The first key typed loads the API keys in the background (TPM).
    TranslationConfig::PreloadKeysAsync();
    state->shift_pending =
        (wparam == VK_SHIFT) && !IsDown(VK_CONTROL) && !IsDown(VK_MENU);
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
      state->pending_selection.clear();
      BeginTranslation(text_service, context, text, true);
      break;
    }
    case Action::kNext:
      if (count > 0) {
        state->selected = (state->selected + 1) % count;
        state->moved = true;
        ShowOverlay(state);
      }
      break;
    case Action::kPrevious:
      if (count > 0) {
        state->selected = (state->selected + count - 1) % count;
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
  }
  *eaten = TRUE;
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
      candidate->set_value(WideToUtf8(state->items[i].text));
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
  const std::wstring name = state->selection ? L"選択範囲を翻訳" : L"翻訳";
  const std::wstring title =
      state->engine.empty() ? name : name + L" (" + state->engine + L")";
  const std::wstring hints = state->selection ? L"  Enter: 置き換え  Esc: キャンセル"
                                              : L"  Enter: 確定  Shift: 戻る";
  footer->set_label(WideToUtf8(
      state->note.empty() ? title + hints : title + L"  " + state->note));
  footer->set_index_visible(!state->items.empty());
  output->clear_all_candidate_words();
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
