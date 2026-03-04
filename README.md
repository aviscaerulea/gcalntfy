## My Google Activities

Google Workspace 上のアクティビティを活用するための 2 コンポーネント構成のプロジェクト。

- **GAS Web App**: 指定日の Chat メッセージ・Calendar イベント・Gmail 送信メール・Drive 更新ファイルを JSON で返す REST API。活動の振り返りや週次報告の材料収集（「誰と何をしたか」）に使う
- **gcalntfy**: GAS Web App の calendar API を定期ポーリングし、予定を 4 分前に Windows Toast 通知する常駐デーモン（Windows 用）

## 機能

- 指定日に自分がメッセージを送信したスペースの全 Chat メッセージを収集（他者含む）
- DM ・グループチャット・名前付きスペースすべてに対応
- 指定日のプライマリカレンダーのイベントを収集（会議・タスク等）
- 指定日に自分が送信した Gmail メールを収集（件名・本文・宛先）
- Calendar イベントの参加者、Gmail メールの宛先を氏名解決して取得
- 指定日に自分が作成・更新した Google Drive ファイルのメタデータを収集（マイドライブ・共有ドライブ対象）
- 全メディアをまとめて取得する `media=all` をサポート
- メールアドレス→氏名変換とグループ展開を行う `media=member` をサポート
- POST + トークン認証の Web API として公開

## API 一覧

すべての機能は POST メソッドで提供する。エンドポイントは GAS Web App のデプロイ URL。

### 共通パラメータ

| パラメータ | 必須 | 説明 |
|---|---|---|
| `token` | 必須 | API トークン（Script Properties の `API_TOKEN` と照合） |
| `media` | 必須 | 取得対象（下表参照） |
| `date` | `member` 以外は必須 | 取得対象日時（JST）。`YYYY-MM-DD` で当日全体、`YYYY-MM-DD HH:MM` で指定時刻から当日終わりまで |
| `fields` | 任意 | 返却するフィールド名の配列。省略時は全フィールドを返す |

### media パラメータ

| `media` | 機能 | 追加の必須パラメータ |
|---|---|---|
| `chat` | 指定日の Chat メッセージ取得（自分が発信したスペースの全メッセージ） | `date` |
| `calendar` | 指定日のプライマリカレンダーのイベント取得 | `date` |
| `mail` | 指定日の Gmail 送信メール取得 | `date` |
| `drive` | 指定日のマイドライブ・共有ドライブの更新ファイル取得 | `date` |
| `all` | 全メディアまとめて取得 | `date` |
| `member` | メールアドレス→氏名解決・グループ展開 | `emails`（アドレス配列） |

## 使い方

すべての機能は POST メソッドで提供する。`token` と `media` は必須。

```bash
BASE="https://script.google.com/macros/s/{DEPLOY_ID}/exec"

# 個別取得
curl -X POST -H "Content-Type: application/json" \
  -d '{"token":"YOUR_SECRET","date":"2026-02-27","media":"chat"}' "$BASE"
curl -X POST -H "Content-Type: application/json" \
  -d '{"token":"YOUR_SECRET","date":"2026-02-27","media":"calendar"}' "$BASE"
curl -X POST -H "Content-Type: application/json" \
  -d '{"token":"YOUR_SECRET","date":"2026-02-27","media":"mail"}' "$BASE"
curl -X POST -H "Content-Type: application/json" \
  -d '{"token":"YOUR_SECRET","date":"2026-02-27","media":"drive"}' "$BASE"

# 全メディアまとめて取得
curl -X POST -H "Content-Type: application/json" \
  -d '{"token":"YOUR_SECRET","date":"2026-02-27","media":"all"}' "$BASE"

# 時刻指定（16:00 以降のみ）
curl -X POST -H "Content-Type: application/json" \
  -d '{"token":"YOUR_SECRET","date":"2026-02-27 16:00","media":"chat"}' "$BASE"

# フィールド絞り込み（attendees/sender の高コスト API 解決をスキップして高速化）
curl -X POST -H "Content-Type: application/json" \
  -d '{"token":"YOUR_SECRET","date":"2026-02-27 09:00","media":"calendar","fields":["datetime","content"]}' "$BASE"

# 氏名解決（グループ展開なし）
curl -X POST -H "Content-Type: application/json" \
  -d '{"token":"YOUR_SECRET","media":"member","emails":["a@example.com","group@example.com"]}' "$BASE"

# 氏名解決（グループ展開人数の上限指定）
curl -X POST -H "Content-Type: application/json" \
  -d '{"token":"YOUR_SECRET","media":"member","emails":["group@example.com"],"expandLimit":20}' "$BASE"
```

## レスポンス形式

### 個別取得（例: media=chat）

メディア名をキーとして返す。

```json
{
  "chat": [
    {
      "datetime": "2026-02-27T09:23:50.228Z",
      "content": "@全員 MFA を有効にしましょう...",
      "sender": "me",
      "spaceName": "開発G情報共有",
      "spaceType": "SPACE",
      "permalink": "https://chat.google.com/room/AAAASextXjU/WZZ04XGAHno.WZZ04XGAHno",
      "threadId": "WZZ04XGAHno",
      "isThreadHead": true
    }
  ]
}
```

### 全体取得（media=all）

全メディアをひとつの JSON で返す。

```json
{
  "chat": [...],
  "calendar": [...],
  "mail": [...],
  "drive": [...]
}
```

### 氏名解決（media=member）

```json
{
  "member": {
    "a@example.com": "山田太郎",
    "smallgroup@example.com": { "_expanded": true, "members": ["田中花子", "佐藤一郎"] },
    "biggroup@example.com": "全社メーリングリスト"
  }
}
```

- 個人アドレス：氏名文字列
- グループ（`expandLimit` 以内）：`{ _expanded: true, members: [氏名, ...] }`
- グループ（`expandLimit` 超過）：グループ表示名の文字列

各メディアのフィールド定義:

#### calendar

```json
{
  "datetime": "2026-02-27T10:00:00.000Z",
  "content": "週次定例ミーティング",
  "sender": "me",
  "permalink": "https://www.google.com/calendar/event?eid=...",
  "attendees": ["田中 一郎", "鈴木 花子"]
}
```

#### mail

```json
{
  "datetime": "2026-02-27T11:30:00.000Z",
  "content": "Re: 週次報告の件\n\n来週の会議は水曜日に変更します。",
  "sender": "me",
  "permalink": "https://mail.google.com/mail/u/0/#all/18f9a2b3c4d5e6f7",
  "recipients": ["田中 一郎", "鈴木 花子"],
  "threadId": "18f9a2b3c4d5e6f7",
  "isThreadHead": false,
  "parent": {
    "datetime": "2026-02-26T09:00:00.000Z",
    "content": "週次報告の件\n\n来週の報告をお願いします。",
    "sender": "田中 一郎"
  }
}
```

#### drive

```json
{
  "datetime": "2026-02-27T10:30:00.000Z",
  "content": "週次報告書",
  "sender": "me",
  "permalink": "https://docs.google.com/document/d/xxx/edit",
  "mimeType": "application/vnd.google-apps.document",
  "isNew": true
}
```

詳細な仕様は [SPEC.md](SPEC.md) を参照。

## セットアップ

### 前提

- Google Workspace アカウント
- [clasp](https://github.com/google/clasp) のインストール（ pnpx 経由で使用）
- `clasp login` による認証済み

### デプロイ手順

```bash
# 1. GAS プロジェクト作成
pnpx @google/clasp create --type webapp --rootDir src --title "google-activity"

# 2. ソースを push
pnpx @google/clasp push

# 3. Web App としてデプロイ
pnpx @google/clasp deploy --description "v1.0.0"
```

### API トークンの設定

デプロイ後、GAS エディタで `setupApiToken` 関数を実行して API トークンを設定する。

1. `src/main.js` の `setupApiToken` 関数内の `token` 変数を任意の秘密文字列に変更してから push
2. GAS エディタで `setupApiToken` を選択して実行
3. Script Properties に `API_TOKEN` が保存されたことを確認

設定した文字列をリクエストの `token` フィールドで送信する。

### GCP API 有効化

GAS プロジェクトに紐づく Google Cloud Project で以下の API を有効化する必要がある。

- Google Calendar API
- Gmail API
- Google Drive API

## gcalntfy

GAS Web App の calendar API を定期ポーリングし、次の予定を 4 分前に Windows Toast 通知する常駐デーモン。

### 動作

- 起動すると常駐し、`gcalntfy.toml` の `schedule` に従って当日の Calendar イベントをポーリング
- 次のイベントの 4 分前に Windows Toast 通知を表示し、通知音（Opus 形式）を再生
- 日付が変わると通知済みセットをリセットして当日分を再取得

### 特徴

- **時間帯別ポーリング間隔**: `schedule` に 24 要素の配列（分単位）で設定。`0` の時間帯はポーリングしない
- **通知音**: exe に埋め込んだ Opus 音声を ffplay で再生。`gcalntfy.local.opus` を同フォルダに置くとカスタム音に上書き
- **BLE ヘッドホン対応**: `adelay=2000` で冒頭 2 秒の無音を挿入して接続遅延による冒頭切れを防止
- **ダッキング**: 通知音再生中に `duck_targets` で指定したプロセスをミュートし、終了後に自動復元
- **多重起動制御**: Job Object により新プロセス起動時に旧プロセスと子プロセス（ffplay）をまとめて終了
- **設定オーバーライド**: `gcalntfy.local.toml` がある場合はキー単位で優先して使用

### ビルド

```shell
task build
```

Visual Studio 2022 または Build Tools（C++20、MSVC）が必要。成果物は `out/gcalntfy.exe`。

### 設定

`gcalntfy.toml`（または `gcalntfy.local.toml`）を exe と同フォルダに配置する。

```toml
# GAS Web App のデプロイ URL
api_url = "https://script.google.com/macros/s/{DEPLOY_ID}/exec"
# API トークン（setupApiToken 関数で設定した値）
api_token = "YOUR_SECRET"
# 0時〜23時のポーリング間隔（分）。0=ポーリングしない
schedule = [60, 60, 60, 60, 60, 60, 60, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 20, 20, 20, 60, 60]
# 通知音再生中にミュートするプロセス名（空配列で無効）
# duck_targets = ["chrome.exe", "msedge.exe"]
```

## 技術スタック

### GAS Web App

- Google Apps Script (V8 ランタイム)
- Google Chat REST API v1
- Google Calendar REST API v3
- Gmail REST API v1
- Google Drive REST API v3
- People API v1 (REST 直呼び)
- GroupsApp (GAS 組み込みサービス)

### gcalntfy

- C++20（MSVC）
- WinRT Toast Notifications（Windows.UI.Notifications）
- WASAPI Core Audio API（ダッキング）
- WinHTTP（HTTPS ポーリング）
- toml++（TOML 設定パーサ）
- ffplay（通知音再生）
