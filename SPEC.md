## 概要

指定日に自分が発信した Google Workspace 上のアクティビティ（Chat メッセージ等）を JSON で返す GAS Web App。

## エンドポイント

GAS Web App として公開。`executeAs: USER_ACCESSING` でアクセス者自身の権限で実行する。

```
GET https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=YYYY-MM-DD
GET https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=YYYY-MM-DD&medium=chat
```

### クエリパラメータ

| パラメータ | 必須 | 説明 |
|---|---|---|
| `date` | 必須 | 取得対象日（`YYYY-MM-DD` 形式、JST 基準） |
| `medium` | 任意 | 取得対象メディア。省略時は全対象。現在は `chat` のみ対応 |

## レスポンス

### 成功時

```json
{
  "activities": [
    {
      "datetime": "2026-02-27T09:23:50.228255Z",
      "medium": "chat",
      "content": "@全員 MFA を有効にしましょう...",
      "sender": "me",
      "spaceName": "開発G情報共有",
      "spaceType": "SPACE",
      "permalink": "https://chat.google.com/room/AAAASextXjU/WZZ04XGAHno.WZZ04XGAHno",
      "threadId": "WZZ04XGAHno",
      "isThreadHead": true
    },
    {
      "datetime": "2026-02-27T09:29:43.158543Z",
      "medium": "chat",
      "content": "サイトでMFAを登録する際...",
      "sender": "me",
      "spaceName": "開発G情報共有",
      "spaceType": "SPACE",
      "permalink": "https://chat.google.com/room/AAAASextXjU/WZZ04XGAHno.dINiYwPrnRc",
      "threadId": "WZZ04XGAHno",
      "isThreadHead": false
    }
  ]
}
```

### アクティビティフィールド

| フィールド | 説明 |
|---|---|
| `datetime` | 発信日時（UTC RFC3339 形式） |
| `medium` | メディア種別（`chat` / 将来: `calendar`, `gmail`） |
| `content` | メッセージ本文 |
| `sender` | 自分のメッセージは `"me"`、他人のメッセージは送信者の表示名 |
| `spaceName` | スペース表示名。DM・グループチャットは参加者名（自分以外）をカンマ区切りで表示 |
| `spaceType` | スペース種別（`SPACE` / `GROUP_CHAT` / `DIRECT_MESSAGE`） |
| `permalink` | `chat.google.com` 形式のメッセージ直接リンク |
| `threadId` | スレッド識別子（`thread.name` の末尾 ID 部分） |
| `isThreadHead` | `true`: スレッドの最初のメッセージ、`false`: 返信 |

### エラー時

```json
{
  "error": "エラーメッセージ"
}
```

## 実装詳細

### 対応メディア

| メディア | 実装状況 | 備考 |
|---|---|---|
| Google Chat | 実装済み | 全スペース（DM, グループ, スペース）対象 |
| Google Calendar | 未実装（将来追加予定） | - |
| Gmail | 未実装（将来追加予定） | - |

### Chat アクティビティ取得の仕様

- 対象: 自分が参加している全スペース
- フィルタ: `createTime` による日付範囲（JST 当日 00:00 〜 翌日 00:00 を UTC 変換）
- 送信者フィルタ: Chat API がサーバー側での sender フィルタに非対応のため、クライアント側で `sender.name` を照合
- 自分のユーザーID: OAuth2 userinfo エンドポイントから取得した数値IDを `users/{id}` 形式で使用
- スペース名解決: `displayName` が空の場合（DM・グループチャット）は `Members.list` で参加者名を取得。メッセージ取得と同一 `fetchAll` バッチで並列実行
- パーマリンク: `message.name` の spaceId・messageId から構築（DM は `dm/`、その他は `room/` パス）
- スレッド構造: `message.thread.name` と `message.threadReply` から `threadId` / `isThreadHead` を導出
- 外部親メッセージ: 自分が返信したスレッドの親メッセージ（別日投稿を含む）を結果に追加し、`sender` で送信者を識別可能にする

### 必要な GAS 設定

**認証方式:** UrlFetchApp + `ScriptApp.getOAuthToken()`（Advanced Service 不使用）

**OAuth スコープ:**
- `chat.spaces.readonly`
- `chat.messages.readonly`
- `chat.memberships.readonly`
- `userinfo.profile`

## ファイル構成

```
src/
├── appsscript.json   GAS マニフェスト
├── main.js           Web App エントリポイント（doGet）
├── chat.js           Chat アクティビティ取得モジュール
├── config.js         定数定義
└── util.js           共通ユーティリティ
```
