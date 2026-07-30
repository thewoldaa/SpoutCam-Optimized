@echo off
rem Build the SpoutCam settings program.
rem
rem Usage:  build.cmd [x64|x86]      default x64
rem
rem Needs Visual Studio Build Tools with the C++ workload. There is no project
rem file on purpose - the program is four source files plus the SpoutDX ones,
rem which is under the weight where a .vcxproj earns its keep.

setlocal

set ARCH=%1
if "%ARCH%"=="" set ARCH=x64

if /i "%ARCH%"=="x86" (
    set VCARCH=x86
    set WVARCH=x86
    set EXENAME=SpoutCam32.exe
) else (
    set ARCH=x64
    set VCARCH=x64
    set WVARCH=x64
    set EXENAME=SpoutCam.exe
)

set ROOT=%~dp0
set SPOUTDX=%ROOT%..\SpoutCam\SpoutDX\source
set WEBVIEW=%ROOT%packages\WebView2\build\native
set OUT=%ROOT%build\%ARCH%

rem The finished program goes to the repository root, next to the SpoutCam
rem folder. Registration looks for the filter relative to the program, so this
rem is the one place it works without being told where anything is.
set EXEDIR=%ROOT%..

if not exist "%SPOUTDX%\SpoutDX.cpp" (
    echo ERROR: SpoutDX sources not found at "%SPOUTDX%"
    echo The SpoutCam repository must sit next to this folder.
    exit /b 1
)

if not exist "%WEBVIEW%\include\WebView2.h" (
    echo ERROR: WebView2 SDK not found at "%WEBVIEW%"
    echo Fetch it with:
    echo   nuget install Microsoft.Web.WebView2 -OutputDirectory packages
    exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Install Visual Studio Build Tools.
    exit /b 1
)

rem Read the install path through a temp file rather than a for /f backquote.
rem The default location contains "(x86)", and that closing bracket upsets the
rem parser inside a for ... in ( ) group.
set "VSPATH="
set "VSTMP=%TEMP%\spoutcamsettings_vspath.txt"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSTMP%" 2>nul
if exist "%VSTMP%" set /p VSPATH=<"%VSTMP%"
del "%VSTMP%" 2>nul

if not defined VSPATH (
    echo ERROR: No Visual Studio installation with the C++ tools was found.
    exit /b 1
)
echo Using %VSPATH%

rem vcvarsall prints its own harmless complaints to stderr on some installs
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvarsall.bat failed for %VCARCH%.
    exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

rc /nologo /fo "%OUT%\app.res" /i "%ROOT%src" "%ROOT%src\app.rc"
if errorlevel 1 exit /b 1

cl /nologo /EHsc /O2 /MT /W3 /std:c++17 /DUNICODE /D_UNICODE ^
   /I "%ROOT%src" /I "%WEBVIEW%\include" /I "%SPOUTDX%" ^
   /Fo"%OUT%\\" /Fe"%EXEDIR%\%EXENAME%" ^
   "%ROOT%src\main.cpp" ^
   "%ROOT%src\preview.cpp" ^
   "%SPOUTDX%\SpoutDX.cpp" ^
   "%SPOUTDX%\SpoutCopy.cpp" ^
   "%SPOUTDX%\SpoutDirectX.cpp" ^
   "%SPOUTDX%\SpoutFrameCount.cpp" ^
   "%SPOUTDX%\SpoutSenderNames.cpp" ^
   "%SPOUTDX%\SpoutSharedMemory.cpp" ^
   "%SPOUTDX%\SpoutUtils.cpp" ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED ^
   "%OUT%\app.res" ^
   "%WEBVIEW%\%WVARCH%\WebView2LoaderStatic.lib" ^
   d3d11.lib dxgi.lib msimg32.lib shell32.lib advapi32.lib user32.lib gdi32.lib ^
   ole32.lib oleaut32.lib version.lib

if errorlevel 1 exit /b 1

echo.
for %%p in ("%EXEDIR%\%EXENAME%") do echo Built %%~fp
endlocal
