## My Google Activities

指定日に自分が発信した Google Workspace 上のアクティビティを JSON で取得する GAS Web App。
活動の振り返りや週次報告の材料収集（「誰と何をしたか」）を主な用途とする。

現在は Google Chat のメッセージ取得と Google Calendar のイベント取得に対応している。

## 機能

- 指定日に自分が送信した Google Chat メッセージを全スペースから収集
- DM・グループチャット・名前付きスペースすべてに対応
- 指定日のプライマリカレンダーのイベントを収集（会議・タスク等）
- Calendar イベントの参加者を氏名解決して取得（グループアドレスの個人展開に対応）
- スペース名・種別・パーマリンク付きの JSON を返却
- HTTP GET で取得できる Web API として公開

## 使い方

```bash
curl "https://script.google.com/macros/s/{DEPLOY_ID}/exec?date=2026-02-27"
```

詳細な仕様は [SPEC.md](SPEC.md) を参照。

## セットアップ

### 前提

- Google Workspace アカウント
- [clasp](https://github.com/google/clasp) のインストール（pnpx 経由で使用）
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

## 技術スタック

- Google Apps Script (V8 ランタイム)
- Google Chat REST API v1
- Google Calendar REST API v3
- People API v1 (REST 直呼び)
- GroupsApp (GAS 組み込みサービス)
