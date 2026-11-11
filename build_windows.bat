@echo off
rem Builds "NYC.exe" with Visual Studio Build Tools (MSVC) or MinGW g++.
rem You only need this if you change the code - a ready-made exe is already included.
setlocal
cd /d "%~dp0"
if not exist build mkdir build

where cl >nul 2>nul
if %errorlevel%==0 goto msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if defined VSDIR (
  call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
  goto msvc
)
where g++ >nul 2>nul
if %errorlevel%==0 goto mingw
echo.
echo No C++ compiler found. Install "Visual Studio Build Tools" (free) with the
echo "Desktop development with C++" workload, then run this again:
echo https://visualstudio.microsoft.com/visual-cpp-build-tools/
pause
exit /b 1

:msvc
echo Building with MSVC...
pushd src
rc /nologo /fo ..\build\app.res app.rc || goto fail
popd
cl /nologo /std:c++17 /O2 /EHsc /utf-8 /permissive- /DNDEBUG /Ivendor src\main.cpp src\platform_win.cpp build\app.res ^
   /Fo:build\ /Fe:"build\NYC.exe" /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup ^
   user32.lib gdi32.lib advapi32.lib ole32.lib shell32.lib shlwapi.lib version.lib dwmapi.lib uuid.lib windowscodecs.lib comdlg32.lib winhttp.lib || goto fail
goto done

:mingw
echo Building with MinGW...
pushd src
windres app.rc -O coff -o ..\build\app.res.o || goto fail
popd
g++ -std=c++17 -O2 -Ivendor -Ivendor\mingw src\main.cpp src\platform_win.cpp build\app.res.o -o "build\NYC.exe" ^
    -static -mwindows -s -lgdi32 -ldwmapi -ladvapi32 -lole32 -lshell32 -lshlwapi -luser32 -lversion -luuid -lwindowscodecs -lcomdlg32 -lwinhttp || goto fail
goto done

:fail
echo.
echo Build failed - scroll up to see the error.
pause
exit /b 1

:done
echo.
echo Done! build\NYC.exe
explorer build
pause
