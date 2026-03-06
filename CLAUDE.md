@README.md
@spec.md

## 開発環境

- clasp で GAS プロジェクトを管理
- ソースコードは `src/` 以下に配置し `pnpx @google/clasp push` でデプロイ
- clasp は `pnpx @google/clasp` で実行する（bunx は不安定なため使用しない）

## Advanced Service の有効化

`appsscript.json` に定義しているが、GAS エディタ上での手動有効化も必要。
push 後に GAS エディタを開き「サービス」から Chat API と People API が有効か確認すること。

## テスト方法

GAS エディタで `doGet` を選択し、イベントパラメータに以下を設定して実行する。

```json
{"parameter": {"date": "YYYY-MM-DD"}}
{"parameter": {"date": "YYYY-MM-DD", "medium": "calendar"}}
```

## 実装上の注意点

- Calendar API の `start.dateTime` は `+09:00` 形式で返る。Chat の `Z` 形式と混在すると `localeCompare` ソートが壊れるため `toISOString()` で UTC 正規化する
- Calendar API の `eventTypes` クエリパラメータは繰り返しキーが必要で、GAS の単純クエリ構築（`Object.entries`）では使えない → クライアント側フィルタで対応
- 新規 API モジュール追加時は `util.js` の `callApi(baseUrl, path, params)` を使う
