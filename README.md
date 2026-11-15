# Translation IME

A Japanese input method for Windows that translates as you type. It is built on
[Mozc](https://github.com/google/mozc), so Japanese input works the way Mozc does
(conversion, learning, user dictionary), and it adds one thing: while you type
Japanese, one key shows your text in other languages, ready to insert.

[日本語の説明はこちら](#日本語)

| You do | Translation IME does |
| --- | --- |
| Type Japanese, then tap **Shift** | The candidate window shows the translations (up to 17 languages). **Enter** inserts the selected one. |
| Select text, then press **Shift + Henkan** | Translates the selection: other languages into Japanese, Japanese into your languages. **Enter** replaces the selection. |

Translations come from [DeepL](https://www.deepl.com/pro-api) (recommended, the
free plan is enough for most people) or
[Google Cloud Translation](https://cloud.google.com/translate), with your own API key.

## Install

1. Download `TranslationIME64.msi` from
   [Releases](https://github.com/kusakabe1222/Translation-IME-Mozc/releases) and run it.
   It needs administrator rights.
2. Press **Win + Space** and choose **Translation IME**.
3. Right-click the IME icon in the taskbar, choose **Translation Settings**, and paste
   your API key. **Test connection** checks it with a real request.

Requirements: Windows 10 version 1809 or later, or Windows 11, 64-bit (x64).

The installer is not code-signed yet, so Windows SmartScreen may warn before it runs.

Translation IME uses its own name, folders and identifiers, so it can be installed
next to Mozc or Google Japanese Input.

## Use

| Key | While composing | While translations are shown |
| --- | --- | --- |
| Shift (alone) | Show translations of the composition | Back to the conversion candidates |
| Space / ↓ / Tab | Convert (Mozc) | Next translation |
| ↑ / Shift + Tab | | Previous translation |
| Enter | Commit (Mozc) | Insert the selected translation |
| 1 – 9 | | Insert that translation |
| Esc / Backspace | | Back to the conversion |
| Shift + Henkan (with a selection) | Translate the selection | |

- Languages appear as they arrive; the footer shows the progress.
- In password and PIN fields translation is off, and nothing you type there is sent anywhere.
- The settings app turns translation off entirely when you only want Japanese input.
- A translation preview shows at most 4 lines; the whole translation is inserted.
- Up to 5,000 characters are translated at a time.

## When something does not work

| Problem | What to do |
| --- | --- |
| "No API key is set" | Open **Translation Settings** and paste the key; **Test connection** checks it. |
| "This key may not use the Cloud Translation API" | In the Google Cloud Console, add Cloud Translation API to the key's API restrictions and enable the API. |
| Shift does nothing | Check that **Translation IME** is the current input method (Win + Space) and that translation is turned on in the settings. |
| Nothing happens in one application | Set `Trace` (DWORD) = `1` under `HKEY_CURRENT_USER\Software\TranslationIME`, try again, and look at `%TEMP%\TranslationIME-trace.log`. It records key codes and decisions only, never text. Delete the value afterwards. |

## Privacy

Text leaves your computer only when you ask for a translation (Shift, Shift + Henkan,
or **Test connection**), and only to the service you chose. Translation IME has no
telemetry. API keys are stored encrypted (AES-256-GCM, the key protected by the TPM
and Windows DPAPI). Details: [PRIVACY.md](PRIVACY.md).

## Build from source

Windows, with Visual Studio 2022 or 2026 (C++ desktop and ATL), Python 3,
[Bazelisk](https://github.com/bazelbuild/bazelisk) and the .NET SDK 8:

```
cd src
python build_tools/update_deps.py
python build_tools/build_qt.py --release --confirm_license
bazelisk build package --config release_build
```

The installer is `src/bazel-bin/win32/installer/TranslationIME64.msi`.
See [docs/build_mozc_in_windows.md](docs/build_mozc_in_windows.md) for details.

Where the translation feature lives:

| Path | What |
| --- | --- |
| `src/win32/tip/tip_translation.*` | Translation UI in the candidate window, selection translation, keys |
| `src/win32/tip/tip_translation_logic.*` | Key handling and text rules (unit tested) |
| `src/win32/tip/translation/` | DeepL / Google requests, settings, API key encryption |
| `src/win32/translation_settings/` | Settings app |

```
bazelisk test //win32/tip:tip_translation_logic_test //win32/tip/translation:translation_test --config release_build
```

## License

Translation IME is distributed under the BSD 3-Clause License, like Mozc
([LICENSE](LICENSE)). The Mozc code is Copyright Google LLC; the translation feature
is Copyright 2026 Translation IME Project. Third-party components (Qt and others)
keep their own licenses, listed in `src/data/installer/credits_en.html`.

Translation IME is not affiliated with or endorsed by Google, DeepL or Microsoft.
The original Mozc README is [README.mozc.md](README.mozc.md).

---

## 日本語

入力した日本語をその場で翻訳できる、Windows 用の日本語入力 (IME) です。
[Mozc](https://github.com/google/mozc) をもとにしているので、変換・学習・ユーザー辞書は
Mozc と同じように使えます。そこに翻訳機能を加えました。

| 操作 | 動作 |
| --- | --- |
| 日本語を入力して **Shift** を押して離す | 候補ウィンドウが翻訳結果 (最大 17 言語) に切り替わり、**Enter** で訳文を確定 |
| 文字を選択して **Shift + 変換** | 選択範囲を翻訳 (外国語は日本語へ、日本語は設定した言語へ)。**Enter** で置き換え |

翻訳には [DeepL](https://www.deepl.com/ja/pro-api) (おすすめ、無料プランで十分です) か
[Google Cloud Translation](https://cloud.google.com/translate) の API キーを使います。

### インストール

1. [Releases](https://github.com/kusakabe1222/Translation-IME-Mozc/releases) から
   `TranslationIME64.msi` をダウンロードして実行します (管理者権限が必要です)。
2. **Win + Space** で **Translation IME** を選びます。
3. タスクバーの IME アイコンを右クリック →「翻訳の設定」で API キーを貼り付けます。
   「接続テスト」で実際に翻訳して確認できます。

Windows 10 (1809 以降) / Windows 11 の 64 ビット版で動きます。
インストーラーはまだコード署名していないため、SmartScreen の警告が出ることがあります。
独自の名前と識別子を使うので、Mozc や Google 日本語入力と同時にインストールできます。

### キー操作

| キー | 入力中 | 翻訳表示中 |
| --- | --- | --- |
| Shift (単独) | 入力中の文字を翻訳 | 変換候補に戻る |
| Space / ↓ / Tab | 変換 (Mozc) | 次の訳 |
| ↑ / Shift + Tab | | 前の訳 |
| Enter | 確定 (Mozc) | 選んだ訳を確定 |
| 1 〜 9 | | その番号の訳を確定 |
| Esc / Backspace | | 変換候補に戻る |
| Shift + 変換 (選択中) | 選択範囲を翻訳 | |

- 訳は届いた言語から順に表示します。
- パスワード欄・PIN 欄では翻訳しません (入力した文字がどこかへ送られることはありません)。
- 設定アプリで翻訳機能をオフにすると、普通の日本語入力として使えます。
- 候補ウィンドウには訳文を最大 4 行まで表示します。確定されるのは全文です。
- 一度に翻訳できるのは 5,000 文字までです。

### うまく動かないとき

| 症状 | 対処 |
| --- | --- |
| 「APIキーが未設定です」 | 「翻訳の設定」でキーを貼り付け、「接続テスト」で確認します |
| 「このキーでは Cloud Translation API が許可されていません」 | Google Cloud Console でキーの「API の制限」に Cloud Translation API を追加し、API を有効にします |
| Shift を押しても何も起きない | Win + Space で **Translation IME** が選ばれているか、設定で翻訳機能がオンになっているかを確認します |
| 特定のアプリだけで動かない | `HKEY_CURRENT_USER\Software\TranslationIME` に `Trace` (DWORD) = `1` を作って操作し、`%TEMP%\TranslationIME-trace.log` を見ます。記録するのはキーコードと判定だけで、文字は記録しません。終わったら値を消してください |

### プライバシー

文字が外部に送られるのは、翻訳を指示したとき (Shift、Shift + 変換、接続テスト) だけで、
送り先は選んだ翻訳サービスだけです。利用状況の送信 (テレメトリー) はありません。
API キーは AES-256-GCM で暗号化して保存し、その鍵を TPM と Windows の DPAPI で保護します。
詳しくは [PRIVACY.md](PRIVACY.md) を見てください。

### ライセンス

Mozc と同じ BSD 3-Clause License です ([LICENSE](LICENSE))。Mozc のコードの著作権は
Google LLC、翻訳機能の部分は 2026 Translation IME Project にあります。
Qt などのサードパーティー製部品は、それぞれのライセンスに従います。
Translation IME は Google、DeepL、Microsoft とは関係がなく、これらの企業が推奨するものでもありません。
