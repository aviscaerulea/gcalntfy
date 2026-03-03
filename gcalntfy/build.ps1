# vi: ts=4 sw=4 ff=unix fenc=utf-8
# gcalntfy ビルドスクリプト
# DevShell モジュール経由で VC++ ビルド環境を初期化し、rc/cl でビルドする。

# VS 開発環境の初期化（DevShell モジュール経由、Build Tools 対応）
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -products '*' -latest -property installationPath
if (-not $vsPath) { Write-Error "Visual Studio / Build Tools が見つからない"; exit 1 }

$devShellDll = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
if (-not (Test-Path $devShellDll)) { Write-Error "DevShell.dll が見つからない: $devShellDll"; exit 1 }
Import-Module $devShellDll
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64"

# チャイム音 + 通知音を結合（ffmpeg がなければ通知音のみ使用）
$chime    = "src\chime.opus"
$notify   = "src\gcalntfy.opus"
$combined = "out\notify.opus"
if (-not (Test-Path $notify)) { Write-Error "notify sound not found: $notify"; exit 1 }
$merged   = $false
if ((Get-Command ffmpeg -ErrorAction SilentlyContinue) -and (Test-Path $chime)) {
    & ffmpeg -y -i $chime -i $notify `
        -filter_complex "[0:a][1:a]concat=n=2:v=0:a=1[out]" -map "[out]" `
        -c:a libopus -b:a 64k $combined
    $merged = -not $LASTEXITCODE
    if (-not $merged) { Write-Warning "ffmpeg concat failed, using notify sound only" }
}
if (-not $merged) {
    Copy-Item $notify $combined
}

rc /nologo /fo out\resource.res src\resource.rc
if ($LASTEXITCODE) { exit 1 }

cl /nologo /utf-8 /std:c++20 /EHsc /O2 `
    /Foout\ /Feout\gcalntfy.exe `
    src\main.cpp out\resource.res `
    /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup `
    windowsapp.lib winhttp.lib shlwapi.lib shell32.lib propsys.lib
if ($LASTEXITCODE) { exit 1 }
