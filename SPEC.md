## 概要

指定日に自分が発信した Google Workspace 上のアクティビティ（ Chat メッセージ・ Calendar イベント・ Gmail 送信メール）を JSON で返す GAS Web App。
活動の振り返りや週次報告の材料収集（「誰と何をしたか」）を主な用途とする。

## エンドポイント

GAS Web App として公開。`executeAs: USER_ACCESSING` でアクセス者自身の権限で実行する。

```
GET https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=YYYY-MM-DD&media=chat
GET https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=YYYY-MM-DD&media=calendar
GET https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=YYYY-MM-DD&media=mail
```

### クエリパラメータ

| パラメータ | 必須 | 説明 |
|---|---|---|
| `date` | 必須 | 取得対象日（`YYYY-MM-DD` 形式、JST 基準） |
| `media` | 必須 | 取得対象メディア。`chat` / `calendar` / `mail` のいずれか |

`media` パラメータは必須。1 回のリクエストで取得できるのは 1 メディア種別のみ。
GAS の実行時間制限（ 6 分）を考慮した分割設計とする。

## レスポンス

### 成功時

```json
{
  "activities": [...]
}
```

必須フィールドは `datetime`、`media`、`content`、`permalink` の 4 つ。その他はメディア別の固有フィールド。

### エラー時

```json
{
  "error": "エラーメッセージ"
}
```

GAS Web App では HTTP ステータスコードを制御できないため、エラー時も HTTP 200 が返る。

## メディア別アクティビティスキーマ

### Chat

```json
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
```

| フィールド | 説明 |
|---|---|
| `datetime` | 発信日時（ UTC RFC3339 形式） |
| `media` | `"chat"` |
| `content` | メッセージ本文 |
| `sender` | 自分: `"me"`、他人: 表示名 |
| `spaceName` | スペース表示名（ DM ・グループチャットは参加者名をカンマ区切り） |
| `spaceType` | `SPACE` / `GROUP_CHAT` / `DIRECT_MESSAGE` |
| `permalink` | `chat.google.com` 形式のメッセージ直接リンク |
| `threadId` | スレッド識別子（`thread.name` の末尾 ID 部分） |
| `isThreadHead` | `true`: スレッドの最初のメッセージ、`false`: 返信 |

### Calendar

```json
{
  "datetime": "2026-02-27T10:00:00.000Z",
  "media": "calendar",
  "content": "週次定例ミーティング",
  "sender": "me",
  "permalink": "https://www.google.com/calendar/event?eid=...",
  "attendees": ["田中 一郎", "鈴木 花子"]
}
```

| フィールド | 説明 |
|---|---|
| `datetime` | イベント開始日時（ UTC RFC3339 形式）。終日イベントは JST 00:00:00 |
| `media` | `"calendar"` |
| `content` | イベントタイトル |
| `sender` | 自分が作成者: `"me"`、他人が主催者: 解決済み氏名（ People API 経由） |
| `permalink` | Google Calendar のイベントリンク |
| `attendees` | 参加者の氏名配列（自分・会議室・欠席者を除く、氏名解決済み） |

### Mail

```json
{
  "datetime": "2026-02-27T11:30:00.000Z",
  "media": "mail",
  "content": "Re: 週次報告の件\n\n来週の会議は水曜日に変更します。\nよろしくお願いします。",
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

| フィールド | 説明 |
|---|---|
| `datetime` | 送信日時（ UTC RFC3339 形式） |
| `media` | `"mail"` |
| `content` | 件名 + `\n\n` + 本文（署名区切り `-- ` 以降を除去） |
| `sender` | 常に `"me"`（`from:me` フィルタ済み） |
| `permalink` | Gmail のメッセージ直接リンク |
| `recipients` | To 宛先の氏名配列（氏名解決済み） |
| `threadId` | スレッド識別子（`message.threadId`） |
| `isThreadHead` | `true`: スレッド先頭メール、`false`: 返信（`In-Reply-To` ヘッダの有無で判定） |
| `parent` | 返信メールのみ。スレッド先頭メッセージの `{ datetime, content, sender }`。自分がスレッドを開始した場合は省略 |

## 実装詳細

### 対応メディア

| メディア | 実装状況 | 備考 |
|---|---|---|
| Google Chat | 実装済み | 全スペース（ DM, グループ, スペース）対象 |
| Google Calendar | 実装済み | プライマリカレンダー対象 |
| Gmail | 実装済み | 送信メール（ SENT ラベル相当）対象 |

### Chat アクティビティ取得の仕様

- 対象: 自分が参加している全スペース
- フィルタ: `createTime` による日付範囲（ JST 当日 00:00 〜 翌日 00:00 を UTC 変換）
- 送信者フィルタ: Chat API がサーバー側での sender フィルタに非対応のため、クライアント側で `sender.name` を照合
- 自分のユーザー ID: OAuth2 userinfo エンドポイントから取得した数値 ID を `users/{id}` 形式で使用
- スペース名解決: `displayName` が空の場合（ DM ・グループチャット）は `Members.list` で参加者名を取得。メッセージ取得と同一 `fetchAll` バッチで並列実行
- パーマリンク: `message.name` の spaceId ・ messageId から構築（ DM は `dm/`、その他は `room/` パス）
- スレッド構造: `message.thread.name` と `message.threadReply` から `threadId` / `isThreadHead` を導出
- 外部親メッセージ: 自分が返信したスレッドの親メッセージ（別日投稿を含む）を結果に追加することで文脈を保持し、`sender` で送信者を識別可能にする

### Calendar アクティビティ取得の仕様

- 対象: プライマリカレンダー（`calendars/primary`）のみ
- フィルタ: `timeMin` / `timeMax` による日付範囲（ JST 当日 00:00 〜 翌日 00:00 を UTC 変換）
- イベントタイプフィルタ: `outOfOffice`（不在）・`workingLocation`（勤務場所）・`focusTime`（サイレントモード）をクライアント側で除外
- 欠席フィルタ: `attendees` の自分のエントリの `responseStatus === "declined"` のイベントを除外
- 繰り返しイベント: `singleEvents=true` により個別イベントとして展開
- 終日イベント: `start.date` のみの場合は JST 00:00:00 に変換して `datetime` に格納
- 主催者: `event.creator.self === true` なら `"me"`、他人が主催者なら People API で氏名解決
- パーマリンク: `event.htmlLink` をそのまま使用
- 参加者解決: `attendees` から会議室・自分・欠席者を除外し、グループアドレスを個人に再帰展開した上で People API / ContactsApp で氏名解決

### Gmail アクティビティ取得の仕様

- 対象: 自分が送信したメール（`from:me` フィルタ）
- 日付フィルタ: `after:{epoch秒}` / `before:{epoch秒}` で UTC 基準の正確な範囲指定（ PST 誤差を避けるため epoch 秒形式を使用）
- メッセージ取得: `list` で ID 一覧を取得後、`fetchAll` で並列バッチ `get`（`format=FULL`）
- 本文抽出: `text/plain` パートから署名区切り（`/^-- $/m`）より前の部分を抽出
- content 構成: 件名 + `\n\n` + 本文（ text/plain なければ件名のみ）
- 宛先解決: To ヘッダをパースしてメールアドレス抽出後、People API / ContactsApp で氏名解決
- パーマリンク: `https://mail.google.com/mail/u/0/#all/{messageId}` 形式
- スレッド構造: `message.threadId` を `threadId` として格納。`In-Reply-To` ヘッダが存在しない場合を `isThreadHead: true` とする
- 外部スレッド親: 返信メール（`isThreadHead: false`）のうち、スレッド先頭が当日の自分の送信メール内にない場合、`threads.get（format=MINIMAL）` でスレッド先頭メッセージ ID を取得後、`messages.get（format=FULL）` で本文を取得して `parent` オブジェクトとして付与する。自分が開始したスレッドへの返信には `parent` を付与しない

### 参加者・宛先解決の仕様（ Calendar/Gmail 共通）

- 氏名解決の優先順:
  1. People API `people:searchContacts`（個人連絡先）
  2. People API `people:searchDirectoryPeople`（組織ディレクトリ）
  3. `ContactsApp.getContactsByEmailAddress()`（ GAS 組み込み）
  4. Calendar API の `displayName` / メールヘッダの表示名
  5. メールアドレス（最終手段）
- キャッシュ（`Map`）により同一メールアドレスへの重複 API 呼び出しを防ぐ
- Calendar のグループアドレスは `GroupsApp` で個人に再帰展開（最大深度 3 ）
- Gmail の宛先はグループ展開を行わない（メールは個人アドレスに届くため不要）

#### 必要な GAS 設定

**認証方式:** UrlFetchApp + `ScriptApp.getOAuthToken()`（ Advanced Service 不使用）

#### OAuth スコープ

- `chat.spaces.readonly`
- `chat.messages.readonly`
- `chat.memberships.readonly`
- `userinfo.profile`
- `calendar.readonly`
- `gmail.readonly`
- `contacts.readonly`（ People API 個人連絡先）
- `directory.readonly`（ People API 組織ディレクトリ）
- `groups`（ GroupsApp グループ展開）

## ファイル構成

```
src/
├── appsscript.json   GAS マニフェスト
├── main.js           Web App エントリポイント（doGet）
├── chat.js           Chat アクティビティ取得モジュール
├── calendar.js       Calendar アクティビティ取得モジュール
├── gmail.js          Gmail アクティビティ取得モジュール
├── members.js        参加者・宛先の氏名解決モジュール
├── config.js         定数定義
└── util.js           共通ユーティリティ
```
