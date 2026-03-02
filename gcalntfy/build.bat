@echo off
rem vi: ts=4 sw=4 ff=dos fenc=cp932
rem gcalntfy build script
rem
rem Run Enable-VSDev in PowerShell before this script:
rem   Enable-VSDev
rem   .\gcalntfy\build.bat
rem
rem Place gcalntfy.opus in this directory before building.

setlocal
cd /d "%~dp0"

echo [1/2] Resource compile...
rc /nologo resource.rc
if errorlevel 1 (
    echo ERROR: rc.exe failed
    exit /b 1
)

echo [2/2] C++ compile + link...
cl /nologo /utf-8 /std:c++20 /EHsc /O2 /Fegcalntfy.exe main.cpp resource.res ^
    /link /SUBSYSTEM:CONSOLE ^
    windowsapp.lib winhttp.lib shlwapi.lib shell32.lib propsys.lib
if errorlevel 1 (
    echo ERROR: cl.exe failed
    exit /b 1
)

echo Build SUCCESS: gcalntfy.exe
endlocal
