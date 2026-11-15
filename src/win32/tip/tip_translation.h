// Translation IME addition to Mozc. Distributed under the same terms as
// Mozc (BSD 3-Clause License, see LICENSE at the top of the repository).

#ifndef MOZC_WIN32_TIP_TIP_TRANSLATION_H_
#define MOZC_WIN32_TIP_TIP_TRANSLATION_H_

#include <msctf.h>
#include <windows.h>

#include "protocol/commands.pb.h"
#include "win32/tip/tip_text_service.h"

namespace mozc {
namespace win32 {
namespace tsf {

// Translation IME: pressing and releasing Shift alone while composing shows the
// translations of the composition (DeepL / Google) in the candidate window.
// While they are shown, the private context's last output is a copy whose
// candidate window lists the translations, so the renderer, the visibility
// logic and the UI elements all show them.
// Space / arrows move, Enter or a number commits a translation, Esc returns to
// the conversion. Requests run on worker threads; the results come back to the
// TIP thread through a message-only window.
class TipTranslation {
 public:
  TipTranslation() = delete;

  // Key hooks, called before Mozc handles the key. They return true when the
  // key belongs to the translation UI; |*eaten| is set in that case.
  static bool OnTestKey(TipTextService* text_service, ITfContext* context,
                        bool is_key_down, WPARAM wparam, BOOL* eaten);
  static bool OnKey(TipTextService* text_service, ITfContext* context,
                    bool is_key_down, WPARAM wparam, BOOL* eaten);

  // The text service is going away on this thread.
  static void OnDeactivate();

  // An application drawing the candidates itself (the candidate list UI
  // element) selected or finalized a candidate. They return true when the
  // translations are shown in |context|, the call then being theirs. Finalize
  // commits the selected translation; |commit| false (Abort) goes back.
  static bool OnUiElementSelect(TipTextService* text_service,
                                ITfContext* context, UINT index);
  static bool OnUiElementFinalize(TipTextService* text_service,
                                  ITfContext* context, bool commit);

  // Opens the settings app (API keys, languages, on/off), installed next to
  // the other Mozc executables.
  static bool OpenSettings();
};

}  // namespace tsf
}  // namespace win32
}  // namespace mozc

#endif  // MOZC_WIN32_TIP_TIP_TRANSLATION_H_
