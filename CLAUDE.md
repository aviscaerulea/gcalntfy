## 開発環境

- Visual Studio 2022 または Build Tools（C++20、MSVC）
- vcpkg（`VCPKG_INSTALLATION_ROOT` 環境変数、または PATH 上で解決する。libebur128 はビルド時に自動導入）
- Task、PowerShell 7（`pwsh`）
- `rm`、`cp`、`mkdir` を含む coreutils（Git Bash 等。Taskfile が使用する）

## ビルド方法

```shell
task build    # ビルド（out/gcalntfy.exe）
task release  # リリースビルド + zip 作成
```

ビルド前に `.env` を作成する。次に GCP の OAuth クライアント情報を設定する。

```
GOOGLE_CLIENT_ID=xxxxxxxxxxxx.apps.googleusercontent.com
GOOGLE_CLIENT_SECRET=GOCSPX-xxxxxxxx
```

## 参考

- README.md
