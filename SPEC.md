## 概要

指定日の Google Workspace 上のアクティビティ（ Chat：自分が送信したスペースの全メッセージ・ Calendar イベント・ Gmail 送信メール・ Drive 更新ファイル）を JSON で返す GAS Web App。
活動の振り返りや週次報告の材料収集（「誰と何をしたか」）を主な用途とする。

## エンドポイント

GAS Web App として公開。`executeAs: USER_DEPLOYING` でデプロイ者の権限で実行する。
すべての機能は POST メソッドで提供し、POST ボディの `token` フィールドで認証する。
GET リクエストは使い方案内のみ返す。

```
POST https://script.google.com/macros/s/{DEPLOY_ID}/exec
Content-Type: application/json
```

### POST ボディの共通フォーマット

```json
{
  "token": "YOUR_SECRET",
  "media": "chat | calendar | mail | drive | all | member",
  ...メディア固有パラメータ
}
```

### パラメータ

| パラメータ | 必須 | 説明 |
|---|---|---|
| `token` | 必須 | API トークン。Script Properties の `API_TOKEN` と照合する |
| `media` | 必須 | 取得対象。`chat` / `calendar` / `mail` / `drive` / `all` / `member` のいずれか |
| `date` | media が member 以外の場合に必須 | 取得対象日時（JST 基準）。`YYYY-MM-DD` で当日全体、`YYYY-MM-DD HH:MM` で指定時刻から当日終わりまで |
| `fields` | 任意 | 返却するフィールド名の配列。省略時は全フィールドを返す（後方互換）。`media=all` は無視される |
| `emails` | media="member" の場合に必須 | 解決するメールアドレスの配列 |
| `expandLimit` | media="member" の場合に任意 | グループ展開の上限数。`0`（デフォルト）で上限なし。`N>0` で展開後人数が N を超えるグループはグループ名を返す |

`media=all` を指定すると全アクティビティメディアをまとめて取得する。
GAS の実行時間制限（6 分）を考慮し、全メディア同時取得は個別取得より時間がかかる。

`fields` パラメータを使うと、必要なフィールドだけ取得して高コスト API 呼び出しをスキップできる。
calendar メディアの場合、`attendees` / `sender` を省略すると People API 呼び出しが発生しないため大幅に高速化する。

`date` パラメータの時刻指定について:
- `YYYY-MM-DD` のみ: JST 00:00:00 〜 翌日 00:00:00（当日全体）
- `YYYY-MM-DD HH:MM`: JST 指定時刻 〜 翌日 00:00:00（日付は跨がない）

### トークン認証

初回デプロイ後、GAS エディタで `setupApiToken` 関数を実行して API トークンを Script Properties に設定する。
設定した任意の秘密文字列を POST ボディの `token` フィールドで送信する。
トークンは URL に露出しない POST ボディのみで送信することで、GAS Web App の制約（リクエストヘッダ不可）の中で安全なトークン送信を実現する。

## レスポンス

### 個別取得成功時（例: media=chat）

```json
{
  "chat": [...]
}
```

メディア名をキーとし、アクティビティオブジェクトの配列を値として返す。
必須フィールドは `datetime`、`content`、`permalink` の 3 つ。その他はメディア別の固有フィールド。

### 全体取得成功時（media=all）

```json
{
  "chat": [...],
  "calendar": [...],
  "mail": [...],
  "drive": [...]
}
```

全メディアの結果をひとつの JSON で返す。各メディアの配列は datetime 昇順でソート済み。

### 氏名解決成功時（media=member）

```json
{
  "member": {
    "a@example.com": "山田太郎",
    "smallgroup@example.com": { "_expanded": true, "members": ["田中花子", "佐藤一郎"] },
    "biggroup@example.com": "全社メーリングリスト"
  }
}
```

個人アドレスは氏名文字列を返す。グループアドレスは `expandLimit` に応じて展開結果またはグループ名を返す。

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
| `content` | メッセージ本文 |
| `sender` | 自分: `"me"`、他人: 表示名（`resolveSenderName` で判定） |
| `spaceName` | スペース表示名（ DM ・グループチャットは参加者名をカンマ区切り） |
| `spaceType` | `SPACE` / `GROUP_CHAT` / `DIRECT_MESSAGE` |
| `permalink` | `chat.google.com` 形式のメッセージ直接リンク |
| `threadId` | スレッド識別子（`thread.name` の末尾 ID 部分） |
| `isThreadHead` | `true`: スレッドの最初のメッセージ、`false`: 返信 |

### Calendar

```json
{
  "datetime": "2026-02-27T10:00:00.000Z",
  "content": "週次定例ミーティング",
  "sender": "me",
  "permalink": "https://www.google.com/calendar/event?eid=...",
  "attendees": ["田中 一郎", "鈴木 花子"]
}
```

| フィールド | 説明 |
|---|---|
| `datetime` | イベント開始日時（ UTC RFC3339 形式）。終日イベントは JST 00:00:00 |
| `content` | イベントタイトル |
| `sender` | 自分が作成者: `"me"`、他人が主催者: 解決済み氏名（ People API 経由） |
| `permalink` | Google Calendar のイベントリンク |
| `attendees` | 参加者の氏名配列（自分・会議室・欠席者を除く、氏名解決済み） |

### Mail

```json
{
  "datetime": "2026-02-27T11:30:00.000Z",
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
| `content` | 件名 + `\n\n` + 本文（署名区切り `-- ` 以降を除去） |
| `sender` | 常に `"me"`（`from:me` フィルタ済み） |
| `permalink` | Gmail のメッセージ直接リンク |
| `recipients` | To 宛先の氏名配列（氏名解決済み） |
| `threadId` | スレッド識別子（`message.threadId`） |
| `isThreadHead` | `true`: スレッド先頭メール、`false`: 返信（`In-Reply-To` ヘッダの有無で判定） |
| `parent` | 返信メールのみ。スレッド先頭メッセージの `{ datetime, content, sender }`。自分がスレッドを開始した場合は省略 |

### Drive

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

| フィールド | 説明 |
|---|---|
| `datetime` | ファイル更新日時（ UTC RFC3339 形式。`modifiedByMeTime` 優先、なければ `modifiedTime`） |
| `content` | ファイル名 |
| `sender` | 常に `"me"` |
| `permalink` | `webViewLink`（なければ `https://drive.google.com/file/d/{id}/view`） |
| `mimeType` | ファイルの MIME タイプ |
| `isNew` | `true`: 対象日に自分が作成、`false`: 既存ファイルの更新 |

### Member（氏名解決）

```json
{
  "member": {
    "a@example.com": "山田太郎",
    "smallgroup@example.com": { "_expanded": true, "members": ["田中花子", "佐藤一郎"] },
    "biggroup@example.com": "全社メーリングリスト"
  }
}
```

| フィールド | 説明 |
|---|---|
| キー | 入力メールアドレス（リクエストの `emails` 配列の各要素） |
| 値（個人） | 氏名文字列（解決不可の場合はメールアドレスをそのまま返す） |
| 値（グループ・展開） | `{ _expanded: true, members: ["氏名", ...] }`（`expandLimit` 内に収まる場合） |
| 値（グループ・上限超過） | グループ表示名の文字列（`expandLimit` を超える場合） |

## 実装詳細

### 対応メディア

| メディア | 実装状況 | 備考 |
|---|---|---|
| Google Chat | 実装済み | 全スペース（ DM, グループ, スペース）対象 |
| Google Calendar | 実装済み | プライマリカレンダー対象 |
| Gmail | 実装済み | 送信メール（ SENT ラベル相当）対象 |
| Google Drive | 実装済み | マイドライブ・共有ドライブ対象 |
| 氏名解決（member） | 実装済み | メールアドレス→氏名変換、グループ展開対応 |

### Chat アクティビティ取得の仕様

- 対象: 指定日に自分がメッセージを送信したスペース（自分がメッセージを送信していないスペースは除外）
- メッセージ出力: 対象スペース内の期間内の全メッセージ（他者のメッセージを含む）
- フィルタ: `createTime` による日付範囲（ JST 当日 00:00 〜 翌日 00:00 を UTC 変換）
- 送信者判定: `resolveSenderName()` で全メッセージの sender を `"me"` または表示名に変換。`msg.sender.displayName` が空の場合は全スペースのメンバーリストから構築した `userId → displayName` マップでフォールバック解決し、それでも未解決のユーザーは People API `people:batchGet` で一括補完する
- 自分のユーザー ID: OAuth2 userinfo エンドポイントから取得した数値 ID を `users/{id}` 形式で使用
- スペース名解決: `displayName` が空の場合（ DM ・グループチャット）は `Members.list` で参加者名を取得。全スペースのメンバーリストも同一 `fetchAll` バッチで並列取得して `userId → displayName` マップに蓄積する
- パーマリンク: `message.name` の spaceId ・ messageId から構築（ DM は `dm/`、その他は `room/` パス）
- スレッド構造: `message.thread.name` と `message.threadReply` から `threadId` / `isThreadHead` を導出
- 外部親メッセージ: 全返信メッセージのうちスレッド先頭が当日データにないものを別途取得して文脈を保持する

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

### Drive アクティビティ取得の仕様

- 対象: マイドライブ・共有ドライブのすべてのファイル（フォルダ・ゴミ箱を除く）
- サーバー側フィルタ: `modifiedTime` の範囲・フォルダ除外・ゴミ箱除外を `q` パラメータで指定
- クライアント側フィルタ: `modifiedByMeTime` が期間内、または `modifiedByMeTime` 未設定かつ `lastModifyingUser.me === true`
- datetime: `modifiedByMeTime` を優先し、なければ `modifiedTime` を使用
- isNew 判定: `createdTime` が対象日範囲内 かつ `owners[].me === true`
- パーマリンク: `webViewLink` を優先し、なければ `https://drive.google.com/file/d/{id}/view`
- 氏名解決: 不要（ sender は常に `"me"`）

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

##### 認証方式

UrlFetchApp + `ScriptApp.getOAuthToken()`（ Advanced Service 不使用）

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
- `drive.metadata.readonly`（ Drive ファイルメタデータ読み取り）

## ファイル構成

```
src/
├── appsscript.json   GAS マニフェスト
├── main.js           Web App エントリポイント（doPost / doGet）
├── chat.js           Chat アクティビティ取得モジュール
├── calendar.js       Calendar アクティビティ取得モジュール
├── gmail.js          Gmail アクティビティ取得モジュール
├── drive.js          Drive アクティビティ取得モジュール
├── members.js        参加者・宛先の氏名解決モジュール（外部 API 用 resolveEmailForApi 含む）
├── config.js         定数定義
└── util.js           共通ユーティリティ
```
