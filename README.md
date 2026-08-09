# gcalntfy

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/gcalntfy)](https://github.com/aviscaerulea/gcalntfy/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/gcalntfy)](LICENSE)
[![Build](https://github.com/aviscaerulea/gcalntfy/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/gcalntfy/actions/workflows/release.yml)

タスクトレイから Google カレンダーの通知と一覧表示を行う軽量常駐アプリです。

実測での物理メモリ使用量は約 10MB 以下です。

姉妹ツールとして、Redmine のチケットの更新を通知する [redntfy](https://github.com/aviscaerulea/redntfy) もあります。

![トレイアイコンから開いた予定一覧](docs/images/event-list.png)

## 機能

- 予定の通知：Google カレンダーをポーリングし、予定開始前や変更を Windows 通知と音声で知らせる
- システムトレイ：予定一覧の表示や設定をトレイアイコンから操作できる
  - 過去予定：表示するか ON/OFF 切り替え可能
  - 次の予定：太字で表示（設定時間以内なら赤文字になる）
  - ブラウザ表示：予定やフッターをクリックするとブラウザで開く
  - 通知抑制（通知の停止）：予定項目を右クリックするとその予定の通知を止め、取り消し線で表示する
- 複数カレンダー対応：メインのカレンダーに加え、外部カレンダーの予定もまとめて扱える

### システムトレイ

トレイアイコンは、これから始まる予定があると右下へバッジを表示します。

予定一覧の各項目には、開始までの残り時間を「（n時間n分後）」形式で表示します。開始が迫った予定は赤文字、次の予定は太字で強調します。フッタークリックで Google カレンダーの当日ページを開けます。

各種設定は、トレイアイコンの右クリックで開くトレイメニューから操作できます。

### Google タスクの通知対応（制限事項）

| 種別 | 対象 |
|---|:---:|
| 時刻を指定した繰り返さないタスク | ○ |
| 繰り返しタスク | — |

繰り返しタスクは Google Calendar API のイベント一覧に一切現れないため、取得段階で対応できません。
Google カレンダーの画面上の「集中タイム（サイレント モード）」予定はタスクと同じ内部種別（focusTime）で返りますが、判別して通知対象から除外します。

### 通知のタイミング

| タイミング | Windows 通知 | 音声通知 | 条件 |
|---|:---:|:---:|---|
| 予定の開始前（デフォルト 5 分前） | ✓ | ✓ | すべての予定で通知 |
| Google カレンダー側で設定した通知の時刻 | ✓ | ✓ | 予定にポップアップ通知を設定している場合のみ |
| 予定の変更・キャンセル・追加を見つけたとき | ✓ | — | 前回の確認時から内容が変わった場合（開始から 1 時間以上過ぎた予定だけの変更は通知しない） |
| 「今すぐ更新」を実行した後 | ✓ | — | トレイメニューから実行した場合のみ（成功なら本日これから始まる予定の件数、失敗なら理由を表示） |

予定のキャンセルを見つけたときの通知例：

![予定のキャンセルを見つけたときの Windows 通知](docs/images/cancel-toast.png)

「今すぐ更新」の成功時の通知例：

![「今すぐ更新」の成功時の Windows 通知](docs/images/update-toast.png)

### 音声通知の有無

| 条件 | 音声通知 |
|---|:---:|
| 音声ファイル（`sound.wav`）が exe と同フォルダに存在する | あり |
| 音声ファイルが存在しない | なし |
| トレイメニューで「音声通知」を OFF に設定している | なし |
| 「マイク/カメラ使用中は無効」が ON かつマイク/カメラ使用中 | なし |

## インストール

### 動作要件

- Windows 10/11
- 初回起動時に Google アカウントでの OAuth 2.0 認証が必要

### 手順

任意フォルダで zip を展開してください。次に `gcalntfy.exe` を実行してください。

または、[Scoop](https://scoop.sh/) でインストールできます。

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install gcalntfy
```

## 使い方

```powershell
gcalntfy
```

起動するとシステムトレイに常駐し、Google カレンダーを定期的に確認して予定を通知します。

初回だけ Google アカウントとの連携が必要です。Windows 通知からブラウザが開くので「許可」をクリックしてください。

## 設定

`gcalntfy.toml` を exe と同フォルダに配置してください。設定できる項目と意味は、同ファイルのコメントに記載しています。（変更の反映はアプリ再起動時）

同フォルダに `gcalntfy.local.toml` を併置すると、そちらの記述をキー単位で優先適用します。変更したい項目だけをこちらに書いておけば、バージョンアップで `gcalntfy.toml` を入れ替えても設定を移し替えずに済みます。

通知音を鳴らすには `sound.wav`（16bit PCM WAV）も exe と同フォルダに配置してください。
