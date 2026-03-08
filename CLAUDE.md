@README.md

## 開発環境

Visual Studio 2022 または Build Tools（C++20、MSVC）が必要。

```shell
task build    # ビルド（out/gcalntfy.exe）
task release  # リリースビルド + zip 作成
```

ビルド前に `.env` を作成して GCP の OAuth クライアント情報を設定すること。

## .env の設定

```
GOOGLE_CLIENT_ID=xxxxxxxxxxxx.apps.googleusercontent.com
GOOGLE_CLIENT_SECRET=GOCSPX-xxxxxxxx
```

## 参考

@.claude/rules/README_gcalntfy.md
