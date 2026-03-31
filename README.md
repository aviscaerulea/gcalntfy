# gcalntfy

Google Calendar の予定を数分前に Windows 通知で知らせる常駐アプリ。

## インストール

[Scoop](https://scoop.sh/) でインストールできる。

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install gcalntfy
```

## 実行

```powershell
gcalntfy
```

起動するとシステムトレイに常駐し、Google Calendar をポーリングして予定を通知する。

## 動作

- OAuth 2.0 で Google Calendar API をポーリングし、当日の予定を取得
- 起動すると常駐し、`gcalntfy.toml` の `schedule` に従って当日の Calendar イベントをポーリング
- 次のイベントの `notify_minutes` 分前（デフォルト 5 分）に Windows Toast 通知を表示し、exe 同フォルダの `sound.wav`（チャイム音）を再生。音声ファイルがない場合は Toast 通知のみ
- 日付が変わると通知済みセットをリセットして当日分を再取得

## 機能

- **時間帯別ポーリング回数**: `schedule` に 24 要素の配列（回/時）で各時間帯のポーリング頻度を設定可能
- **即時ポーリング**: PC スリープ復帰・セッションロック解除・ネットワーク復帰時に即座にポーリングを実行
- **Toast 通知**: 通知の「Calendar」ボタンからイベントページを直接開ける
- **通知音**: exe 同フォルダに `sound.wav`（16bit PCM WAV）を配置すると通知時にチャイム音を再生
- **BLE ヘッドホン対応**: 接続遅延による冒頭切れを防止する無音を自動挿入
- **ダッキング**: 通知音再生中に `duck_targets` で指定したプロセスをミュートし終了後に自動復元
- **多重起動制御**: 新プロセス起動時に旧プロセスを自動終了
- **設定オーバーライド**: `gcalntfy.local.toml` があればキー単位で優先適用（変更はアプリ再起動で反映）
- **システムトレイ**: 以降予定ありの場合アイコン右下にバッジを表示、ホバーで残り予定件数表示、左クリックで予定一覧（ヘッダー・フッタークリックで Google Calendar 当日ページを開く）、右クリックで各種設定メニューを操作可能
- **音声設定の永続化**: 音声通知と「マイク/カメラ使用中は無効」の設定は再起動後も維持
- **起動時キャッシュ復元**: 直前のポーリング結果を `events.json` にキャッシュし、起動直後にネットワーク不通でもポーリング成功時の予定データを表示
- **変更検知通知**: ポーリング時に前回との差分を検知し、予定の日時変更・キャンセル・新規追加を Toast 通知で報告
- **複数カレンダー対応**: `ext_calendar_ids` で追加のカレンダー ID を指定し、外部カレンダーもポーリング対象にできる
- **予定の通知設定に対応**: 予定に設定されている通知（reminders）の popup タイミングでも追加通知を行う（`notify_minutes` はベースラインとして常に通知）

## 設定

`gcalntfy.toml`（または `gcalntfy.local.toml`）を exe と同フォルダに配置する。

```toml
schedule = [1, 1, 1, 1, 1, 1, 1, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 3, 3, 1, 1]
# イベント開始何分前に通知するか（0〜30、デフォルト 5）
# notify_minutes = 5
# 通知音再生中にミュートするプロセス名（空配列で無効）
# duck_targets = ["chrome.exe", "msedge.exe"]
# 追加でポーリングするカレンダー ID（primary は常に有効）
# カレンダー ID は Google Calendar の「設定」→「カレンダーの統合」で確認できる
# ext_calendar_ids = ["abc123@group.calendar.google.com"]
```

## 初回起動時の認証

初回起動時はアクセストークンがないため、Toast 通知でブラウザが開く。Google アカウントで「許可」をクリックすると認証が完了し、リフレッシュトークンがレジストリ（`HKCU\SOFTWARE\gcalntfy`）に保存される。以降の起動では再認証不要。

## ビルド

```shell
task build
```

Visual Studio 2022 または Build Tools（C++20、MSVC）が必要。成果物は `out/gcalntfy.exe`。

ビルド前に `.env` を作成して `GOOGLE_CLIENT_ID` / `GOOGLE_CLIENT_SECRET` を設定すること。
