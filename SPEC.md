## 概要

指定日に自分が発信した Google Workspace 上のアクティビティ（Chat メッセージ・Calendar イベント等）を JSON で返す GAS Web App。
活動の振り返りや週次報告の材料収集（「誰と何をしたか」）を主な用途とする。

## エンドポイント

GAS Web App として公開。`executeAs: USER_ACCESSING` でアクセス者自身の権限で実行する。

```
GET https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=YYYY-MM-DD
GET https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=YYYY-MM-DD&medium=chat
GET https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=YYYY-MM-DD&medium=calendar
```

### クエリパラメータ

| パラメータ | 必須 | 説明 |
|---|---|---|
| `date` | 必須 | 取得対象日（`YYYY-MM-DD` 形式、JST 基準） |
| `medium` | 任意 | 取得対象メディア。省略時は全対象。`chat` / `calendar` に対応 |

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
      "datetime": "2026-02-27T10:00:00.000Z",
      "medium": "calendar",
      "content": "週次定例ミーティング",
      "sender": "me",
      "spaceName": "二階 隆幸",
      "spaceType": "CALENDAR",
      "permalink": "https://www.google.com/calendar/event?eid=...",
      "attendees": ["田中 一郎", "鈴木 花子"]
    }
  ]
}
```

### アクティビティフィールド

| フィールド | 説明 |
|---|---|
| `datetime` | 発信日時（UTC RFC3339 形式） |
| `medium` | メディア種別（`chat` / `calendar` / 将来: `gmail`） |
| `content` | コンテンツ。Chat: メッセージ本文 / Calendar: イベントタイトル |
| `sender` | 自分が発信者・作成者の場合は `"me"`、他人は表示名 |
| `spaceName` | Chat: スペース表示名（DM・グループチャットは参加者名をカンマ区切り） / Calendar: カレンダー名 |
| `spaceType` | Chat: `SPACE` / `GROUP_CHAT` / `DIRECT_MESSAGE` / Calendar: `CALENDAR` |
| `permalink` | Chat: `chat.google.com` 形式のメッセージ直接リンク / Calendar: Google Calendar のイベントリンク |
| `threadId` | Chat のみ。スレッド識別子（`thread.name` の末尾 ID 部分） |
| `isThreadHead` | Chat のみ。`true`: スレッドの最初のメッセージ、`false`: 返信 |
| `attendees` | Calendar のみ。参加者の氏名配列（自分・会議室・欠席者を除く） |

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
| Google Calendar | 実装済み | プライマリカレンダー対象 |
| Gmail | 未実装（将来追加予定） | - |

### Chat アクティビティ取得の仕様

- 対象: 自分が参加している全スペース
- フィルタ: `createTime` による日付範囲（JST 当日 00:00 〜 翌日 00:00 を UTC 変換）
- 送信者フィルタ: Chat API がサーバー側での sender フィルタに非対応のため、クライアント側で `sender.name` を照合
- 自分のユーザー ID: OAuth2 userinfo エンドポイントから取得した数値 ID を `users/{id}` 形式で使用
- スペース名解決: `displayName` が空の場合（DM・グループチャット）は `Members.list` で参加者名を取得。メッセージ取得と同一 `fetchAll` バッチで並列実行
- パーマリンク: `message.name` の spaceId・messageId から構築（DM は `dm/`、その他は `room/` パス）
- スレッド構造: `message.thread.name` と `message.threadReply` から `threadId` / `isThreadHead` を導出
- 外部親メッセージ: 自分が返信したスレッドの親メッセージ（別日投稿を含む）を結果に追加することで文脈を保持し、`sender` で送信者を識別可能にする

### Calendar アクティビティ取得の仕様

- 対象: プライマリカレンダー（`calendars/primary`）のみ
- フィルタ: `timeMin` / `timeMax` による日付範囲（JST 当日 00:00 〜 翌日 00:00 を UTC 変換）
- イベントタイプフィルタ: `outOfOffice`（不在）・`workingLocation`（勤務場所）・`focusTime`（サイレントモード）をクライアント側で除外
- 欠席フィルタ: `attendees` の自分のエントリの `responseStatus === "declined"` のイベントを除外
- 繰り返しイベント: `singleEvents=true` により個別イベントとして展開
- 終日イベント: `start.date` のみの場合は JST 00:00:00 に変換して `datetime` に格納
- 送信者: `event.creator.self === true` なら `"me"`、他人が主催者なら主催者の表示名
- パーマリンク: `event.htmlLink` をそのまま使用
- 参加者解決: `attendees` から会議室・自分・欠席者を除外し、グループアドレスを個人に再帰展開した上で People API / ContactsApp で氏名解決

### 参加者解決の仕様

- 会議室（`resource: true`）・自分（`self: true`）・欠席者（`responseStatus === "declined"`）を除外
- グループアドレスは `GroupsApp` で個人メンバーに再帰展開（最大深度: `MAX_GROUP_DEPTH = 3`）
- 氏名解決の優先順:
  1. People API `people:searchContacts`（個人連絡先）
  2. People API `people:searchDirectoryPeople`（組織ディレクトリ）
  3. `ContactsApp.getContactsByEmailAddress()`（GAS 組み込み）
  4. Calendar API の `displayName`
  5. メールアドレス（最終手段）
- キャッシュ（`Map`）により同一メールアドレスへの重複 API 呼び出しを防ぐ

### 必要な GAS 設定

#### 認証方式

UrlFetchApp + `ScriptApp.getOAuthToken()`（Advanced Service 不使用）

#### OAuth スコープ

- `chat.spaces.readonly`
- `chat.messages.readonly`
- `chat.memberships.readonly`
- `userinfo.profile`
- `calendar.readonly`
- `contacts.readonly`（People API 個人連絡先）
- `directory.readonly`（People API 組織ディレクトリ）
- `groups`（GroupsApp グループ展開）

## ファイル構成

```
src/
├── appsscript.json   GAS マニフェスト
├── main.js           Web App エントリポイント（doGet）
├── chat.js           Chat アクティビティ取得モジュール
├── calendar.js       Calendar アクティビティ取得モジュール
├── members.js        参加者解決モジュール
├── config.js         定数定義
└── util.js           共通ユーティリティ
```
