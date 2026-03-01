## My Google Activities

指定日に自分が発信した Google Workspace 上のアクティビティを JSON で取得する GAS Web App。
活動の振り返りや週次報告の材料収集（「誰と何をしたか」）を主な用途とする。

Google Chat のメッセージ・ Google Calendar のイベント・ Gmail の送信メールに対応している。

## 機能

- 指定日に自分が送信した Google Chat メッセージを全スペースから収集
- DM ・グループチャット・名前付きスペースすべてに対応
- 指定日のプライマリカレンダーのイベントを収集（会議・タスク等）
- 指定日に自分が送信した Gmail メールを収集（件名・本文・宛先）
- Calendar イベントの参加者、Gmail メールの宛先を氏名解決して取得
- HTTP GET で取得できる Web API として公開

## 使い方

メディア種別（`media`）を必須パラメータとして個別にリクエストする。

```bash
curl "https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=2026-02-27&media=chat"
curl "https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=2026-02-27&media=calendar"
curl "https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=2026-02-27&media=mail"
```

## レスポンス形式

必須フィールドは `datetime`、`media`、`content`、`permalink` の 4 つ。

### Chat

```json
{
  "activities": [
    {
      "datetime": "2026-02-27T09:23:50.228Z",
      "media": "chat",
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

### Calendar

```json
{
  "activities": [
    {
      "datetime": "2026-02-27T10:00:00.000Z",
      "media": "calendar",
      "content": "週次定例ミーティング",
      "sender": "me",
      "permalink": "https://www.google.com/calendar/event?eid=...",
      "attendees": ["田中 一郎", "鈴木 花子"]
    }
  ]
}
```

### Mail

```json
{
  "activities": [
    {
      "datetime": "2026-02-27T11:30:00.000Z",
      "media": "mail",
      "content": "Re: 週次報告の件\n\n来週の会議は水曜日に変更します。\nよろしくお願いします。",
      "sender": "me",
      "permalink": "https://mail.google.com/mail/u/0/#all/18f9a2b3c4d5e6f7",
      "recipients": ["田中 一郎", "鈴木 花子"],
      "threadId": "18f9a2b3c4d5e6f7",
      "isThreadHead": false
    }
  ]
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

## 技術スタック

- Google Apps Script (V8 ランタイム)
- Google Chat REST API v1
- Google Calendar REST API v3
- Gmail REST API v1
- People API v1 (REST 直呼び)
- GroupsApp (GAS 組み込みサービス)
