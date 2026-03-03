# vi: ts=4 sw=4 ff=unix fenc=utf-8
# gcalntfy ビルドスクリプト
# DevShell モジュール経由で VC++ ビルド環境を初期化し、rc/cl でビルドする。

# VS 開発環境の初期化（DevShell モジュール経由）
$vsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
if (-not $vsPath) {
    Write-Error 'Visual Studio not found'
    exit 1
}
Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -Arch amd64 -HostArch amd64 *> $null

rc /nologo /fo out\resource.res src\resource.rc
if ($LASTEXITCODE) { exit 1 }

cl /nologo /utf-8 /std:c++20 /EHsc /O2 `
    /Foout\ /Feout\gcalntfy.exe `
    src\main.cpp out\resource.res `
    /link /SUBSYSTEM:CONSOLE `
    windowsapp.lib winhttp.lib shlwapi.lib shell32.lib propsys.lib
if ($LASTEXITCODE) { exit 1 }
