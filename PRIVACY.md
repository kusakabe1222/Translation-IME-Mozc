# Privacy

[日本語](#日本語)

Translation IME is a Japanese input method. Most of what it does happens only on
your computer, like any other input method. This page lists what leaves it.

## What is sent, and when

| When | What is sent | To |
| --- | --- | --- |
| You tap **Shift** while composing | The text you are composing | The translation service you chose (DeepL or Google Cloud Translation) |
| You press **Shift + Henkan** with text selected | The selected text | Same |
| You press **Test connection** in the settings | The word "こんにちは" | Same |
| The settings app opens (DeepL only) | Your API key, to ask for this month's usage | DeepL |

Each request carries your API key and the target language. Nothing is sent while you
only type and convert Japanese, and nothing is sent from password or PIN fields.

What the translation service does with the text is up to that service and your
contract with it:
[DeepL privacy policy](https://www.deepl.com/privacy),
[Google Cloud privacy](https://cloud.google.com/terms/cloud-privacy-notice).

## What is not sent

- No telemetry, usage statistics or crash reports. The open-source Mozc it is built on
  does not send any either.
- Your conversion history and user dictionary stay in
  `%USERPROFILE%\AppData\LocalLow\Translation IME`.

## What is stored on your computer

| Where | What |
| --- | --- |
| `HKEY_CURRENT_USER\Software\TranslationIME` | Settings. API keys are encrypted with AES-256-GCM; that key is wrapped by an RSA key kept in the TPM and encrypted with Windows DPAPI for your user account. Without this computer's TPM the keys cannot be decrypted. |
| Memory of the application you type in | Recent translations (up to 100), until the application closes |
| `%TEMP%\TranslationIME-trace.log` | Only when you turn on the diagnostic trace (`Trace` = 1 in the key above): key codes and decisions, never text |

Programs running as you on this computer can still use the keys: the input method
itself has to, so no design can prevent that.

To remove everything: uninstall Translation IME, then delete
`HKEY_CURRENT_USER\Software\TranslationIME` and the folder above.

---

## 日本語

Translation IME は日本語入力 (IME) です。ほとんどの処理は、ほかの IME と同じくこの PC の中だけで
行います。ここでは、外部に送られるものをまとめます。

### 送られるものとタイミング

| タイミング | 送られるもの | 送り先 |
| --- | --- | --- |
| 入力中に **Shift** を押して離したとき | 入力中の文字 | 選んだ翻訳サービス (DeepL または Google Cloud Translation) |
| 文字を選択して **Shift + 変換** を押したとき | 選択した文字 | 同上 |
| 設定アプリで「接続テスト」を押したとき | 「こんにちは」という語 | 同上 |
| 設定アプリを開いたとき (DeepL のみ) | 今月の使用量を問い合わせるための API キー | DeepL |

どのリクエストにも API キーと翻訳先の言語が含まれます。日本語を入力・変換しているだけのときや、
パスワード欄・PIN 欄では何も送りません。

送った文字の扱いは各サービスの規約に従います
([DeepL](https://www.deepl.com/ja/privacy)、[Google Cloud](https://cloud.google.com/terms/cloud-privacy-notice))。

### 送らないもの

- 利用状況、統計、クラッシュレポートは送りません (もとにしたオープンソース版 Mozc も送りません)。
- 変換の履歴とユーザー辞書は `%USERPROFILE%\AppData\LocalLow\Translation IME` にだけ保存されます。

### この PC に保存するもの

| 場所 | 内容 |
| --- | --- |
| `HKEY_CURRENT_USER\Software\TranslationIME` | 設定。API キーは AES-256-GCM で暗号化し、その鍵を TPM の中の RSA 鍵と Windows の DPAPI で保護します。この PC の TPM がなければ復号できません。 |
| 入力しているアプリのメモリ | 直近の翻訳結果 (100 件まで)。アプリを閉じると消えます |
| `%TEMP%\TranslationIME-trace.log` | 診断用のトレースをオンにしたとき (上のキーの `Trace` = 1) だけ。キーコードと判定のみで、文字は記録しません |

同じユーザーとして動くプログラムからはキーを使えます。IME 自身がキーを使う以上、
どの方式でもこれは防げません。

すべて消すには、Translation IME をアンインストールしてから、上のレジストリキーとフォルダーを削除します。
