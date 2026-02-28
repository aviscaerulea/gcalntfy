@README.md
@SPEC.md

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
```
