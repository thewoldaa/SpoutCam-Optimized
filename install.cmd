@echo off
rem Point Windows at the SpoutCam filter in this folder.
rem
rem   install.cmd        register
rem   install.cmd /u     unregister
rem
rem This only has to be run once. Registration records the path of the .ax
rem file, not its contents, so rebuilding over the same path is picked up
rem without registering again. What does have to happen after a rebuild is
rem closing whatever had the camera open, because a loaded DLL is locked and
rem the build cannot overwrite it.

setlocal

set "ROOT=%~dp0"
set "AX64=%ROOT%SpoutCam\binaries\SPOUTCAM\SpoutCam64\SpoutCam64.ax"
set "AX32=%ROOT%SpoutCam\binaries\SPOUTCAM\SpoutCam32\SpoutCam32.ax"

set "MODE=register"
set "FLAG="
if /i "%~1"=="/u" (
    set "MODE=unregister"
    set "FLAG=/u"
)

rem Ask for administrator rights and come back
net session >nul 2>&1
if errorlevel 1 (
    echo Administrator rights are needed to %MODE% a DirectShow filter.
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '%~1' -Verb RunAs"
    exit /b
)

if not exist "%AX64%" (
    echo ERROR: %AX64%
    echo not found. Build the filter first:
    echo     msbuild SpoutCam\SpoutCamDX.sln /p:Configuration=Release /p:Platform=x64
    pause
    exit /b 1
)

echo %MODE%ing 64 bit filter
regsvr32 /s %FLAG% "%AX64%"
if errorlevel 1 (echo   FAILED) else (echo   ok)

rem 32 bit hosts need their own build. Not every checkout will have one.
if exist "%AX32%" (
    echo %MODE%ing 32 bit filter
    regsvr32 /s %FLAG% "%AX32%"
    if errorlevel 1 (echo   FAILED) else (echo   ok)
) else (
    echo skipping 32 bit, not built
)

echo.
echo Registered path is now:
reg query "HKLM\SOFTWARE\Classes\CLSID\{8E14549A-DB61-4309-AFA1-3578E927E933}\InprocServer32" /ve 2>nul
if errorlevel 1 echo   not registered

echo.
echo Close and reopen any program using the camera before testing.
pause
endlocal
