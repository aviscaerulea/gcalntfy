# vi: ts=4 sw=4 ff=unix fenc=utf-8
# gcalntfy ビルドスクリプト
# DevShell モジュール経由で VC++ ビルド環境を初期化し、rc/cl でビルドする。
param([string]$Version = "0.0.0")

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

cl /nologo /utf-8 /std:c++20 /EHsc /O2 /I out\ `
    /Foout\ /Feout\gcalntfy.exe `
    src\main.cpp out\resource.res `
    /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup `
    windowsapp.lib winhttp.lib shlwapi.lib shell32.lib propsys.lib
if ($LASTEXITCODE) { exit 1 }
