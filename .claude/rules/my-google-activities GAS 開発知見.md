---
created: 2026-03-01 10:31:00
updated: 2026-03-02 02:32:31
tags:
  - "carecom/84/0"
  - "knowledge"
  - "project/my-google-activities"
project: my-google-activities
---
## 概要

Google Workspace 上の自分のアクティビティ（Chat・Calendar・Gmail・Drive の 4 メディア）を指定日付で収集する GAS Web App の開発知見。Advanced Service を使わず UrlFetchApp + OAuth トークンで REST API を直接叩く構成を採用。メディアごとの API 差異と並列バッチ取得・氏名解決の設計に多くの工夫が詰まっている。

## 背景と課題

「指定日に自分が発信した Google Chat メッセージを全スペースから収集したい」という要件から始まり、Calendar・Gmail・Drive へと対応メディアが拡大した。各メディアで以下の制約が重なって設計が複雑になった。

- Chat API はサーバー側で sender フィルタが効かないため全メッセージをクライアント側で絞り込む必要がある
- DM・グループチャットにはスペース名（displayName）がなく、メンバーリストから名前を解決しなければならない
- 自分が返信したスレッドの親メッセージが当日ではなく別日に投稿されている場合は個別取得が必要
- Gmail の日付フィルタは `YYYY/MM/DD` 形式が PST 基準になるため epoch 秒で範囲指定が必要
- Calendar の `eventTypes` クエリパラメータは繰り返しキー形式が必要で GAS の単純クエリ構築では使えない
- Drive の `modifiedByMeTime` は常に存在するとは限らず、フォールバック処理が必要
- スペース数・ファイル数が多い環境では逐次処理だと実行時間が GAS の制限（6 分）に近づく

## 実行方法

すべての機能は POST メソッドで提供する。`token`（Script Properties に設定済みの API トークン）と `media` は必須。

**GAS Web App への POST は curl -L では動かない。** リダイレクト先 URL を取得してから GET するパターンが必要（詳細は要注意点を参照）。

```bash
BASE="https://script.google.com/macros/s/{DEPLOY_ID}/exec"

# GAS POST ヘルパー関数（bash で使い回す）
gas_post() {
  local body="$1"
  local redirect=$(curl -s -o /dev/null -w "%{redirect_url}" -X POST \
    -H "Content-Type: application/json" --data-binary "$body" "$BASE")
  curl -s "$redirect"
}

# 個別取得
gas_post '{"token":"YOUR_SECRET","date":"2026-02-27","media":"chat"}'
gas_post '{"token":"YOUR_SECRET","date":"2026-02-27","media":"calendar"}'
gas_post '{"token":"YOUR_SECRET","date":"2026-02-27","media":"mail"}'
gas_post '{"token":"YOUR_SECRET","date":"2026-02-27","media":"drive"}'

# 全メディアまとめて取得
gas_post '{"token":"YOUR_SECRET","date":"2026-02-27","media":"all"}'

# 時刻指定（指定時刻から当日終わりまで）
gas_post '{"token":"YOUR_SECRET","date":"2026-02-27 16:00","media":"chat"}'

# 氏名解決（media=member）
gas_post '{"token":"YOUR_SECRET","media":"member","emails":["a@example.com","group@example.com"]}'
```

`media` パラメータは必須。`all` を指定すると全メディアをまとめて取得する。`member` を指定するとメールアドレス→氏名解決ができる。

レスポンスはメディア名をキーとした JSON 形式：

```json
{ "chat": [...] }         // 個別取得
{ "chat": [...], "calendar": [...], "mail": [...], "drive": [...] }  // media=all
{ "member": { "a@example.com": "山田太郎", "group@example.com": { "_expanded": true, "members": ["田中花子"] } } }  // media=member
```

### 事前準備

1. `pnpx @google/clasp create --type webapp --rootDir src --title "google-activity"` でプロジェクト作成
2. `pnpx @google/clasp push` でソースを push
3. GAS エディタで「サービス」から Chat API と People API を手動で有効化（appsscript.json に記述してもエディタ上での手動有効化が必要）
4. `pnpx @google/clasp deploy` でデプロイ
5. GAS エディタで `setupApiToken` 関数の `token` 変数を任意の秘密文字列に書き換えて実行（Script Properties に保存）

#### appsscript.json の重要設定

```json
{
  "webapp": {
    "executeAs": "USER_DEPLOYING",
    "access": "ANYONE_ANONYMOUS"
  }
}
```

`USER_DEPLOYING` でデプロイ者の権限で実行し、`ANYONE_ANONYMOUS` で外部からアクセスを受け付ける。トークン認証は POST ボディ内で行う（GAS Web App はリクエストヘッダを受け取れないため URL に露出しない POST ボディが唯一のセキュアな手段）。

## 技術的アプローチ

### 認証方式の選択

Advanced Service（Chat API）ではなく `UrlFetchApp` + `ScriptApp.getOAuthToken()` の組み合わせを採用。Advanced Service は appsscript.json に定義するだけでなく GAS エディタ上での手動有効化も必要で、CI/CD との相性が悪い。REST API 直呼びのほうが依存が少なく再現性が高い。

### 自分のユーザーID 取得

Chat API の `sender.name` は `"users/{数値ID}"` 形式。この ID を `Session.getActiveUser()` では取得できないため、OAuth2 の userinfo エンドポイント（`https://www.googleapis.com/oauth2/v2/userinfo`）から取得している。

### 並列バッチ取得

`UrlFetchApp.fetchAll()` で全スペースのメッセージ取得と DM のメンバーリスト取得を 1 回のバッチに混合して並列実行。`FETCH_BATCH_SIZE = 50` で分割し、nextPageToken がある場合は次バッチに積んで全件取得する。

### スレッド構造の判定（Chat）

`message.threadReply` フラグが `false`（または存在しない）かつ `thread.name` があればスレッド先頭メッセージ。`threadId` は `thread.name`（`"spaces/{space}/threads/{threadId}"` 形式）の末尾から抽出する。

### スレッド先頭判定（Gmail）

`In-Reply-To` ヘッダの有無で判定する。ヘッダがなければ `isThreadHead: true`。これにより Thread API を使わず単一メッセージ取得で判定できる。

### Gmail の日付フィルタ

Gmail の `after:YYYY/MM/DD` 形式は PST 基準のため JST と最大 17 時間ずれが生じる。`after:{epoch秒}` 形式を使うことで UTC 基準の正確な範囲指定が可能。

### Calendar のイベントタイプフィルタ

`eventTypes` クエリパラメータは繰り返しキーが必要（`eventTypes=default&eventTypes=focusTime` 形式）で、GAS の `Object.entries` による単純なクエリ構築では送れない。`outOfOffice`・`workingLocation`・`focusTime` はクライアント側で除外するフィルタで対処する。

### Calendar のタイムゾーン正規化

Calendar API の `start.dateTime` は `+09:00` のようなオフセット付きで返る。Chat の `Z` 形式と混在すると `localeCompare` ソートが壊れるため、`toISOString()` で UTC に正規化してから格納する。

### Drive のクライアント側フィルタ

`modifiedByMeTime` は共有ドライブやサードパーティ編集では設定されないことがある。`modifiedByMeTime` が期間内に含まれるか、未設定かつ `lastModifyingUser.me === true` の場合を対象とするフォールバックで対処する。

### 共通アーキテクチャパターン

- `util.js` の `callApi(baseUrl, path, params)` で全メディアの API 呼び出しを統一
- `getDateRange(date)` で JST 基準の取得期間（UTC 変換済み）を共通生成
- `main.js` の `MEDIA_GETTERS` オブジェクトでメディア名と取得関数をマッピングし、`media=all` の場合は全エントリを実行

### 氏名解決モジュール（members.js）

Calendar 参加者・Gmail 宛先の氏名解決を共通化。People API 個人連絡先 → People API 組織ディレクトリ → ContactsApp の順にフォールバック。`Map` キャッシュで同一アドレスへの重複呼び出しを防ぐ。Calendar のグループアドレスは `GroupsApp` で個人に再帰展開（最大深度 3）。

内部用 `resolveEmail`（未解決時はメールアドレスにフォールバック）と外部 API 用 `resolveEmailForApi`（未解決時は null を返す）の 2 関数があり、共通の検索ロジックは `lookupName(email)` に集約している。

### POST 統一 API とトークン認証

全機能を `doPost(e)` に統一。POST ボディの `token` フィールドと Script Properties の `API_TOKEN` を照合する。`doGet` は使い方案内のエラーを返すだけ。トークンは `setupApiToken()` を GAS エディタから手動実行して設定する（プレースホルダ文字列のままでは例外を投げるガード条件あり）。

### 外部 API 用の氏名解決（media=member）

`media: "member"` で `emails` 配列を POST すると、各メールアドレスを氏名解決して返す。グループアドレスは `expandGroupMembers` で個人に展開し、`expandLimit` を超える場合はグループ表示名を返す（`GroupsApp.getGroupByEmail(email).getName()`）。

## 知見

### 成功した方法

#### lastActiveTime によるスペース事前フィルタ（Chat）

全スペースを対象にメッセージ取得すると不要なリクエストが大量に発生するが、`space.lastActiveTime` が取得対象日の開始時刻より前のスペースは確実にメッセージがないため除外できる。ISO 8601 形式同士なのでレキシカル比較（文字列比較）がそのまま時系列順を保証する。

#### 外部スレッド親メッセージの追加取得（Chat/Gmail）

当日返信したが親が別日のスレッドも文脈付きで取れるようになった。Chat はスレッド先頭のメッセージ名 `{spaceName}/messages/{threadId}.{threadId}` 形式で推定取得。Gmail は `threads.get(format=MINIMAL)` でスレッド先頭 ID を取得後 `messages.get(format=FULL)` で本文取得。いずれも 404 やエラーを `muteHttpExceptions: true` + ステータスコードチェックでグレースフルに処理する。

#### members の名前を Map に蓄積してから結合（Chat）

ページネーションがある DM でも全メンバー名を正確に取れる。バッチ処理中に逐次結合すると途中バッチのデータが欠ける。

#### Gmail の epoch 秒フィルタ

PST 誤差を完全に排除できた。`after:YYYY/MM/DD` は PST 午前 0 時基準なので JST との差が大きく、日付付近のメールが抜け落ちる。

#### グループアドレスの再帰展開（Calendar）

参加者に配布リストが含まれていても個人名で解決できるようになった。`GroupsApp.getGroupByEmail()` で個人アドレスに展開して People API に渡す。深度上限 3 を設けて無限ループを防ぐ。

#### Chat API の `users/{id}` と People API の `people/{id}` が同一の数値IDを共有する

`members.list` で `displayName` が空だったユーザーを `people:batchGet` で一括補完する方法が有効だった。`userNameMap` にマージすることで既存の `resolveSenderName()` フォールバックチェーンを変更せずに対応できた。

#### lookupName による共通検索ロジックの集約

内部用 `resolveEmail` と外部 API 用 `resolveEmailForApi` で People API + ContactsApp の検索処理が重複していた。共通部分を `lookupName(email)` に切り出し、フォールバック挙動（email に返すか null に返すか）だけを呼び出し側で制御するパターンにしたことで保守性が向上した。

### 失敗した方法

#### Chat API の filter パラメータで sender フィルタを試みた

Chat API はサーバー側の sender フィルタを現時点でサポートしていない（400 エラー）。`createTime` の範囲フィルタのみ使えるため、sender 絞り込みはクライアント側で行う設計に変更した。

#### Advanced Service を使おうとした

`appsscript.json` の `advancedServices` 定義だけでは不十分で GAS エディタ上での手動有効化も必要。push/deploy の自動化フローに組み込みにくいため、REST API 直呼びに切り替えた。

#### Calendar の `eventTypes` を URL パラメータで指定しようとした

GAS の `Object.entries` で構築するクエリ文字列は同一キーの繰り返しをサポートしない。クライアント側フィルタに切り替えた。

### 要注意点

#### GAS Web App の HTTP ステータスコード制御不可

`ContentService` はステータスコードを指定する手段を持たないため、エラー時も HTTP 200 が返る。クライアント側はレスポンスボディの `error` フィールドで判定する設計が必要。

#### curl での GAS POST テストはリダイレクト → GET のパターンが必要

GAS Web App の POST は処理後にレスポンスをリダイレクト URL に埋め込んで 302 を返す。curl `-L` でそのままリダイレクトを追うと 411 (Length Required) になる。正しい手順は「POST してリダイレクト URL だけ取得 → そのリダイレクト URL に GET」：

```bash
redirect=$(curl -s -o /dev/null -w "%{redirect_url}" -X POST \
  -H "Content-Type: application/json" --data-binary "$body" "$BASE")
curl -s "$redirect"
```

リダイレクト URL は 1 リクエストごとに生成される使い捨て URL（`user_content_key` に実行結果が埋め込まれている）なので再利用は不可。

#### `executeAs: USER_DEPLOYER` と `USER_ACCESSING` の違い

デプロイ者固定の `USER_DEPLOYER` だと、他ユーザーがアクセスしてもデプロイ者のデータが返る。自分専用の Web App として使う場合でも `USER_ACCESSING` にしておかないと、別アカウントで curl したときに意図しないデータが返ってくる可能性がある。

#### ソロスペース（自分だけのメモ用スペース）の除外

`membershipCount.joinedDirectHumanUserCount === 1` のスペースは自分だけのソロスペース。これを除外しないと不要なリクエストが増える。ただし `membershipCount` が存在しないスペースもあるため `|| 0` でフォールバックが必要。

#### スレッド先頭メッセージの判定は `threadReply` フラグに依存する（Chat）

`message.thread.name` だけでは先頭か返信かを判別できない。`threadReply: false`（またはフィールドなし）が先頭の条件。

#### 外部スレッド親メッセージの `sender` フィールドに注意（Chat）

他人が投稿したスレッド先頭メッセージは `sender.name !== myUserId` になる。`resolveSenderName()` でユーザーID と比較して `"me"` か表示名かを振り分けることで、レスポンスの `sender` フィールドを一貫した仕様にできる。外部親取得時は `myUserId` を必ずスコープに渡すこと。

#### 外部スレッド親のメッセージ名推定形式（Chat）

スレッド先頭メッセージのリソース名は `spaces/{space}/messages/{threadId}.{threadId}` というドット区切りの重複形式になる。通常の返信メッセージは `{threadId}.{messageId}` 形式なので、先頭だけ同じ ID が 2 つ並ぶ点が特徴的。実装では `${threadId}.${threadId}` として URL を組み立てる。404 が返ることもあるため `muteHttpExceptions: true` + ステータスコードチェックで必ずグレースフルに処理する。

#### Gmail の署名除去

`text/plain` の本文から `-- ` 行（メール署名区切り）より後ろを削除する。正規表現は `/^-- $/m`（行末に改行が続く形式）を使うこと。これにより宛先に長大な署名が渡るのを防げる。

#### Drive の `modifiedByMeTime` 未設定問題

共有ドライブのファイルや Google 以外のクライアントで編集したファイルはこのフィールドが存在しないことがある。`lastModifyingUser.me === true` をフォールバック条件として使い、自分が最後に編集したファイルを取り込む。

#### Drive 共有ドライブの取得には `supportsAllDrives=true` と `includeItemsFromAllDrives=true` が必要

この 2 つを省略するとマイドライブのみ対象になる。

#### Chat API ユーザー認証での `displayName` 空問題

`members.list` の `member.displayName` も同様に空になる。`people:batchGet` で補完できるが、外部ドメイン・削除済み・Bot ユーザーは People API でも解決不可なため、最終フォールバックとして `userId`（`users/{id}` 形式）をそのまま返す設計が必要。

#### `people:batchGet` の `resourceNames` は繰り返しキーで渡す必要がある

`callApi` の通常クエリ構築（`Object.entries` ベース）では対応できないため、`?resourceNames=people/A&resourceNames=people/B` のように手動で URL を組み立てる必要がある。Calendar API の `eventTypes` と同じ制約。

## 関連リソース

- [[my-google-activities]] - プロジェクト本体
- [Google Chat REST API - Messages: list](https://developers.google.com/workspace/chat/api/reference/rest/v1/spaces.messages/list)
- [OAuth2 userinfo endpoint](https://developers.google.com/identity/protocols/oauth2/openid-connect#obtainuserinfo)
- [Google Calendar REST API - Events: list](https://developers.google.com/calendar/api/v3/reference/events/list)
- [Gmail REST API - Messages: list](https://developers.google.com/gmail/api/reference/rest/v1/users.messages/list)
- [Google Drive REST API - Files: list](https://developers.google.com/drive/api/reference/rest/v3/files/list)
- [People API - searchContacts](https://developers.google.com/people/api/rest/v1/people/searchContacts)
- [Google Chat - Identify and reference users](https://developers.google.com/workspace/chat/identify-reference-users)
