# vi: ts=4 sw=4 ff=unix fenc=utf-8
# gcalntfy ビルドスクリプト
# DevShell モジュール経由で VC++ ビルド環境を初期化し、rc/cl でビルドする。
param([string]$Version = "0.0.0", [switch]$Release)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
# リリースワークフローはタグ名由来の値を渡すため、先頭の v を落として数値表記へ正規化する
$Version = $Version -replace '^v', ''

# vcpkg パス設定（VCPKG_INSTALLATION_ROOT 環境変数 → Scoop シムの優先順）
if ($env:VCPKG_INSTALLATION_ROOT) {
    $vcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
}
else {
    $vcpkgCmd = (Get-Command vcpkg -ErrorAction Stop).Source
    $vcpkgRoot = Split-Path $vcpkgCmd
    $shimFile = [System.IO.Path]::ChangeExtension($vcpkgCmd, ".shim")
    if (Test-Path $shimFile) {
        $vcpkgReal = (Get-Content $shimFile |
            Where-Object { $_ -match "^path" } |
            ForEach-Object { ($_ -split '"')[1] } |
            Select-Object -First 1)
        if ($vcpkgReal) {
            $vcpkgRoot = Split-Path $vcpkgReal
        }
    }
}
$vcpkgInclude = "$vcpkgRoot\installed\x64-windows-static\include"
$vcpkgLib     = "$vcpkgRoot\installed\x64-windows-static\lib"

# 依存ライブラリのインストール（未インストール時のみ実行）
& "$vcpkgRoot\vcpkg.exe" install libebur128:x64-windows-static
if ($LASTEXITCODE) { exit 1 }

# VC++ 開発環境の検出と初期化
# vswhere で Visual Studio / Build Tools のインストール先を特定し、DevShell モジュール経由で
# 開発シェルへ入る。-products '*' は既定の検索対象に含まれない Build Tools のスタンドアロン
# 構成を拾うために必要であり、外すと Build Tools のみの環境で検出に失敗する。
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Write-Error "vswhere.exe が見つからない: $vswhere"; exit 1 }
$vsPath = & $vswhere -products '*' -latest -property installationPath
if (-not $vsPath) { Write-Error "Visual Studio / Build Tools が見つからない"; exit 1 }

$devShellDll = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Import-Module $devShellDll
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64"

# バージョン定義ヘッダの生成
# version.rc.h は resource.rc 用、version.h は main.cpp 用。前者を rc 実行前に置く必要がある。
# $Version は "1.2.3-4-gabc1234-dirty" 等の形も取るため、数値版は先頭の major.minor.patch のみ採る。
# 第 4 数値は 0 固定。major.minor.patch を先頭に持たない場合（タグ未取得時など）は 0,0,0,0。
$verMatch = [regex]::Match($Version, '^(\d+)\.(\d+)\.(\d+)')
$verNum = if ($verMatch.Success) {
    "$($verMatch.Groups[1].Value),$($verMatch.Groups[2].Value),$($verMatch.Groups[3].Value),0"
}
else {
    "0,0,0,0"
}
@(
    "#define APP_VERSION_NUM $verNum",
    "#define APP_VERSION_STR `"$Version`""
) | Set-Content -Path out\version.rc.h -Encoding ascii
"#define APP_VERSION L`"$Version`"" | Set-Content -Encoding UTF8NoBOM out\version.h

# /c65001 は .rc を UTF-8 として解釈させる指定であり、コメントと文字列リテラルの双方に必須。
# 無指定では CP932 で MBCS 走査するため、行末バイトがリードバイト範囲に当たると次行が消え、
# VERSIONINFO の日本語文字列（FileDescription）も文字化けする。
rc /nologo /c65001 /I out\ /fo out\resource.res src\resource.rc
if ($LASTEXITCODE) { exit 1 }

# .env から OAuth クレデンシャルを読み込んで oauth.h を生成
$envFile = Join-Path $PSScriptRoot ".env"
if (-not (Test-Path $envFile)) { Write-Error ".env が見つからない（プロジェクトルートに .env を作成して GOOGLE_CLIENT_ID / GOOGLE_CLIENT_SECRET を設定してください）"; exit 1 }
$envVars = @{}
Get-Content $envFile | ForEach-Object {
    if ($_ -match '^\s*([^#=]+?)\s*=\s*(.+?)\s*$') { $envVars[$Matches[1]] = $Matches[2] }
}
if (-not $envVars['GOOGLE_CLIENT_ID'] -or -not $envVars['GOOGLE_CLIENT_SECRET']) {
    Write-Error ".env に GOOGLE_CLIENT_ID / GOOGLE_CLIENT_SECRET が未設定"; exit 1
}
@"
#define OAUTH_CLIENT_ID L"$($envVars['GOOGLE_CLIENT_ID'])"
#define OAUTH_CLIENT_SECRET L"$($envVars['GOOGLE_CLIENT_SECRET'])"
"@ | Set-Content -Encoding UTF8NoBOM out\oauth.h

# リリースモード時の追加フラグ
$clExtra   = if ($Release) { @('/DNDEBUG', '/GL', '/Gy') } else { @() }
$linkExtra = if ($Release) { @('/LTCG', '/OPT:REF', '/OPT:ICF') } else { @() }

cl /nologo /utf-8 /std:c++20 /EHsc /O2 @clExtra /I out\ /I "$vcpkgInclude" `
    /Foout\ /Feout\gcalntfy.exe `
    src\main.cpp out\resource.res `
    /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup @linkExtra `
    windowsapp.lib winhttp.lib shlwapi.lib shell32.lib propsys.lib bcrypt.lib ws2_32.lib gdi32.lib `
    "$vcpkgLib\ebur128.lib"
if ($LASTEXITCODE) { exit 1 }
