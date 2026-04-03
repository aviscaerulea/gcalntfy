# vi: ts=4 sw=4 ff=unix fenc=utf-8
# gcalntfy ビルドスクリプト
# DevShell モジュール経由で VC++ ビルド環境を初期化し、rc/cl でビルドする。
param([string]$Version = "0.0.0")
$Version = $Version -replace '^v', ''

# VS 開発環境の初期化（DevShell モジュール経由、Build Tools 対応）
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -products '*' -latest -property installationPath
if (-not $vsPath) { Write-Error "Visual Studio / Build Tools が見つからない"; exit 1 }

$devShellDll = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
if (-not (Test-Path $devShellDll)) { Write-Error "DevShell.dll が見つからない: $devShellDll"; exit 1 }
Import-Module $devShellDll
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64"

rc /nologo /fo out\resource.res src\resource.rc
if ($LASTEXITCODE) { exit 1 }

"#define APP_VERSION L`"$Version`"" | Set-Content -Encoding UTF8NoBOM out\version.h

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

cl /nologo /utf-8 /std:c++20 /EHsc /O2 /I out\ `
    /Foout\ /Feout\gcalntfy.exe `
    src\main.cpp out\resource.res `
    /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup `
    windowsapp.lib winhttp.lib shlwapi.lib shell32.lib propsys.lib bcrypt.lib ws2_32.lib gdi32.lib
if ($LASTEXITCODE) { exit 1 }
