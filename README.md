## My Google Activities

指定日に自分が発信した Google Workspace 上のアクティビティを JSON で取得する GAS Web App。
活動の振り返りや週次報告の材料収集（「誰と何をしたか」）を主な用途とする。

Google Chat のメッセージ・ Google Calendar のイベント・ Gmail の送信メール・ Google Drive の更新ファイルに対応している。

## 機能

- 指定日に自分がメッセージを送信したスペースの全 Chat メッセージを収集（他者含む）
- DM ・グループチャット・名前付きスペースすべてに対応
- 指定日のプライマリカレンダーのイベントを収集（会議・タスク等）
- 指定日に自分が送信した Gmail メールを収集（件名・本文・宛先）
- Calendar イベントの参加者、Gmail メールの宛先を氏名解決して取得
- 指定日に自分が作成・更新した Google Drive ファイルのメタデータを収集（マイドライブ・共有ドライブ対象）
- 全メディアをまとめて取得する `media=all` をサポート
- HTTP GET で取得できる Web API として公開

## 使い方

メディア種別（`media`）を必須パラメータとして指定する。個別取得と全体取得の両方に対応。

```bash
# 個別取得
curl "https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=2026-02-27&media=chat"
curl "https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=2026-02-27&media=calendar"
curl "https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=2026-02-27&media=mail"
curl "https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=2026-02-27&media=drive"

# 全メディアまとめて取得
curl "https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=2026-02-27&media=all"

# 時刻指定（16:00 以降のみ）
curl "https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=2026-02-27%2016%3A00&media=chat"
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

各メディアのフィールド定義:

**calendar:**
```json
{
  "datetime": "2026-02-27T10:00:00.000Z",
  "content": "週次定例ミーティング",
  "sender": "me",
  "permalink": "https://www.google.com/calendar/event?eid=...",
  "attendees": ["田中 一郎", "鈴木 花子"]
}
```

**mail:**
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

**drive:**
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
pnpx @google/clasp create --type webapp --rootDir src --title "My Google Activities"

# 2. ソースを push
pnpx @google/clasp push

# 3. Web App としてデプロイ
pnpx @google/clasp deploy --description "v1.0.0"
```

### GCP API 有効化

GAS プロジェクトに紐づく Google Cloud Project で以下の API を有効化する必要がある。

- Google Calendar API
- Gmail API
- Google Drive API

## 技術スタック

- Google Apps Script (V8 ランタイム)
- Google Chat REST API v1
- Google Calendar REST API v3
- Gmail REST API v1
- Google Drive REST API v3
- People API v1 (REST 直呼び)
- GroupsApp (GAS 組み込みサービス)
