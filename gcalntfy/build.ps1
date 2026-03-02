# vi: ts=4 sw=4 ff=unix fenc=utf-8
# gcalntfy ビルドスクリプト
# vcvarsall.bat 経由で VC++ ビルド環境を初期化し、rc/cl でビルドする。
# vswhere.exe の絶対パスを使用（PATH に含まれない環境を考慮）。

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe not found: $vswhere"
    exit 1
}
$vsPath = & $vswhere -latest -property installationPath
$vcvarsall = Join-Path $vsPath 'VC\Auxiliary\Build\vcvarsall.bat'

# vcvarsall.bat の環境変数を現在の PowerShell プロセスに取り込む
$envLines = cmd /c "`"$vcvarsall`" amd64 >nul 2>&1 && set"
$envLines | Where-Object { $_ -match '^([^=]+)=(.*)$' } | ForEach-Object {
    [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2])
}

rc /nologo /fo out\resource.res src\resource.rc
if ($LASTEXITCODE) { exit 1 }

cl /nologo /utf-8 /std:c++20 /EHsc /O2 `
    /Foout\ /Feout\gcalntfy.exe `
    src\main.cpp out\resource.res `
    /link /SUBSYSTEM:CONSOLE `
    windowsapp.lib winhttp.lib shlwapi.lib shell32.lib propsys.lib
if ($LASTEXITCODE) { exit 1 }
