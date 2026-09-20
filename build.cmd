@echo off
setlocal EnableExtensions

rem Canonical local build: the same CMake presets used by CI.
set "CMAKE=cmake"
where cmake >nul 2>nul
if not errorlevel 1 goto :run

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :missing_cmake
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT goto :missing_cmake

set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" goto :missing_cmake

:run
set "BUILD_DIR=%~dp0.build\windows-vs2022"
set "PACKAGE_DIR=%BUILD_DIR%\game-package"

"%CMAKE%" --preset windows-vs2022
if errorlevel 1 exit /b %errorlevel%

rem DO NOT PASS `--parallel` HERE, WITH OR WITHOUT A VALUE.
rem
rem The line used to read `--build --preset windows-relwithdebinfo --parallel`.  Under this
rem generator (Visual Studio 17 2022, MSBuild 17.14) that makes the build fail with NO output
rem beyond the MSBuild banner and return 1, so `build.cmd` reported "the canonical build is
rem broken" on a tree that builds cleanly -- and there was nothing in the log to read.
rem
rem `--parallel` is therefore never passed here.  CMake forwards it to MSBuild as `/m:<n>`, and
rem whatever MSBuild does with it under this generator, it does it before printing anything --
rem which is why the failure looked like a broken repository rather than a broken flag.  The
rem preset already builds in parallel by default, so dropping the flag loses nothing.
"%CMAKE%" --build --preset windows-relwithdebinfo
if errorlevel 1 exit /b %errorlevel%

rem Keep the existing package tree and replace only files owned by the install set.
"%CMAKE%" --install "%BUILD_DIR%" --config RelWithDebInfo --prefix "%PACKAGE_DIR%" --component GameRuntime
exit /b %errorlevel%

:missing_cmake
echo CMake was not found. Install CMake 3.22+ or Visual Studio 2022 C++ Build Tools.
exit /b 2
