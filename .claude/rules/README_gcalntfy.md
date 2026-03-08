---
created: 2026-03-08 12:33:47
updated: 2026-03-08 12:33:47
tags:
  - "carecom/84/0"
  - "knowledge"
  - "project/my-activity"
  - "AIgen"
project: my-activity
---
## 概要

Google Calendar の予定を定期ポーリングし、4 分前に Windows Toast 通知する常駐デーモン gcalntfy の開発知見。C++20（MSVC）で実装し、WinRT Toast Notifications・WASAPI・WinHTTP・toml++ を組み合わせたネイティブ Windows アプリケーション。GAS Web App の calendar API を経由してデータを取得する構成だが、直接 Google Calendar API にアクセスする設計も調査済み。

## 背景と課題

Google Calendar の予定通知は標準のブラウザ通知だと見逃しやすく、デスクトップ常駐型の通知デーモンが必要だった。以下の技術的課題に対処しながら機能を拡張してきた。

- Windows Toast 通知で独自チャイム音を鳴らしつつ OS 通知音との重複を防ぐ必要がある
- BLE ヘッドホンは接続遅延があり、再生開始直後の音声が切れる
- 通知音再生中に他アプリの音声をダッキング（一時ミュート）する要件
- システムトレイアイコンの常駐と、エクスプローラ再起動時の自動復帰
- ポーリング間隔中にイベント開始時刻を過ぎるとツールチップの件数が古くなる

## 実行方法

### ビルド

```shell
task build
```

Visual Studio 2022 または Build Tools（C++20、MSVC）が必要。成果物は `gcalntfy/out/gcalntfy.exe`。リリースビルドは `task release` で zip を生成する。

ビルドスクリプト `build.ps1` は公式 DLL モジュール方式（`Microsoft.VisualStudio.DevShell.dll` + `Enter-VsDevShell`）で VS 開発環境をロードする。Build Tools 単体環境にも対応している。

### 設定

`gcalntfy.toml`（または `gcalntfy.local.toml`）を exe と同フォルダに配置する。

```toml
api_url = "https://script.google.com/macros/s/{DEPLOY_ID}/exec"
api_token = "YOUR_SECRET"
schedule = [1, 1, 1, 1, 1, 1, 1, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 3, 3, 1, 1]
# duck_targets = ["chrome.exe", "msedge.exe"]
```

`schedule` は 24 要素の配列で、各時間帯のポーリング回数（回/時）を指定する。正時 :00 を起点に等間隔でポーリングし、`0` の時間帯はポーリングしない。

### 事前準備

- GAS Web App のデプロイと API トークン設定が必要（詳細は [[README_my-activity]] を参照）
- ffmpeg（ffplay）のインストール（通知音再生に使用）
- Scoop でインストールする場合は `scoop install gcalntfy` で ffmpeg 依存も自動解決される

## 技術的アプローチ

### アーキテクチャ

単一の `main.cpp` に全機能を実装するシングルファイル構成。ポーリングスレッドと通知スレッドを分離し、メインスレッドはウィンドウメッセージループで駆動する。

- **ポーリングスレッド**: `schedule` に従って GAS Web App から当日のイベントを取得
- **通知スレッド**: イベント開始 4 分前に Toast 通知を発行し、チャイム音を再生
- **メインスレッド**: トレイアイコンの管理、ウィンドウメッセージ処理

### 多重起動制御

Job Object を使用して新プロセス起動時に旧プロセスと子プロセス（ffplay）をまとめて終了する。これにより Scoop のアップデート時やユーザの手動再起動時に旧プロセスが残留しない。

### Toast 通知

WinRT の `Windows.UI.Notifications` を使用。アプリアイコン（`app.ico`）をリソースとしてバイナリに埋め込み、通知に表示する。OS 通知音は `<audio silent="true"/>` で無効化し、独自チャイムとの重複を防止する。「Calendar」ボタンからイベントページを直接開ける。

### 通知音再生

ffplay で `audio1.opus`（チャイム音）を再生後、`audio2.opus` があれば続けて再生する。BLE ヘッドホンの接続遅延に対応するため `adelay=1000` で冒頭 1 秒の無音を挿入する。

### ダッキング（WASAPI）

WASAPI Core Audio API を使用し、通知音再生中に `duck_targets` で指定したプロセスをミュートする。再生完了後に自動復元する。

### 設定ホットリロード

5 分間隔で TOML 設定ファイルを再読み込みする。プロセス再起動なしに `schedule` や `duck_targets` の変更を反映できる。`gcalntfy.local.toml` が存在する場合はキー単位で優先して上書き適用する。

### システムトレイアイコン

タスクトレイにアイコンを常駐させ、以下の操作を提供する。

- **ホバー**: 「この後の予定：N 件」のように現在時刻以降の当日予定件数を表示。ポップアップ・メニュー表示中はツールチップを抑制する
- **左クリック**: 当日の予定一覧をポップアップ表示。クリックで Google Calendar のイベントページを開く
- **右クリック**: コンテキストメニュー（音声通知設定・設定ファイル・ログファイル・再起動・終了）

エクスプローラ再起動時は `WM_TASKBARCREATED` メッセージを検出してアイコンを自動復帰する。

### 音声設定のレジストリ永続化

「音声通知」ON/OFF と「マイク/カメラ使用中は無効」の設定は `HKCU\SOFTWARE\gcalntfy` に保存され、再起動後も維持される。

### ツールチップの定期更新

`WM_TIMER`（`IDT_TOOLTIP_REFRESH`）で 60 秒間隔にツールチップの予定件数を更新する。これにより、ポーリング間隔中にイベント開始時刻を過ぎても件数が古いまま残る問題を解決した。`addTrayIcon` で `SetTimer` を設定し、`removeTrayIcon` で `KillTimer` する。

### ミーティング中の音声自動ミュート

マイクまたはカメラが使用中の場合、通知音を自動的に無効化する。ミーティング中に通知チャイムが鳴ってしまう問題を防ぐ。

## 知見

### 成功した方法

#### g_popupShowing フラグによるツールチップ抑制

左クリックポップアップや右クリックコンテキストメニュー表示中にツールチップが重なって表示される問題を、`static std::atomic<bool> g_popupShowing{false}` フラグで制御して解決した。`NIM_MODIFY` でツールチップテキストを空文字にすることで即座に非表示にできる。`clearTrayTooltip()` ヘルパー関数で DRY を維持している。

#### WM_TIMER による件数リアルタイム更新

ポーリングは数分～数十分間隔だが、ツールチップは 60 秒間隔で更新することで、イベント開始時刻を過ぎた直後に件数が減少する。ポーリングとツールチップ更新を独立したタイミングで実行する設計が有効だった。

#### adelay による BLE ヘッドホン対応

BLE ヘッドホンは音声出力開始から実際に聴こえるまでに 0.5～1 秒の遅延がある。ffplay の `adelay=1000` フィルタで冒頭 1 秒の無音を挿入することで、チャイム音の冒頭切れを防止した。

#### Job Object による多重起動制御

Windows の Job Object に自プロセスを登録し、`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` フラグを設定することで、新プロセス起動時に旧プロセスと ffplay 子プロセスをまとめて終了できる。Scoop のアップデートフローとの相性が良い。

### 要注意点

#### Toast 通知の OS 通知音無効化

`<audio silent="true"/>` を XML テンプレートに含めないと、OS のデフォルト通知音と独自チャイムが同時に鳴る。

#### エクスプローラ再起動時のトレイアイコン消失

エクスプローラが再起動すると通知領域がリセットされ、アイコンが消失する。`RegisterWindowMessage(L"TaskbarCreated")` で検出して `addTrayIcon` を再実行する必要がある。

#### ツールチップと左クリック一覧の件数不一致

`updateTrayTooltip` がポーリング時にしか呼ばれない場合、ポーリング間隔中にイベント開始時刻を過ぎるとツールチップの件数がポーリング時点の古い値のまま残る。一方 `showSchedulePopup`（左クリック）はクリック時にリアルタイム計算するため、両者の表示件数に不一致が発生する。`WM_TIMER` による 60 秒間隔の定期更新で解決済み。

#### レジストリ永続化のキー設計

`HKCU\SOFTWARE\gcalntfy` に `DWORD` 値で保存する。将来 OAuth トークンもここに保存する設計のため、キー名の衝突に注意する。

## 関連リソース

- [[README_my-activity]] - プロジェクト全体の知見（GAS Web App 含む）
- [WinRT Toast Notifications](https://learn.microsoft.com/windows/apps/design/shell/tiles-and-notifications/toast-notifications-overview)
- [WASAPI Core Audio API](https://learn.microsoft.com/windows/win32/coreaudio/core-audio-apis-in-windows-vista)
- [toml++ ライブラリ](https://marzer.github.io/tomlplusplus/)
- [ffplay ドキュメント](https://ffmpeg.org/ffplay.html)
