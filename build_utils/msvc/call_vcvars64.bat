:: Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

:: Initializes the 64-bit MSVC environment for the build. This is a wrapper around vcvars64.bat
:: that locates Visual Studio through vswhere instead of a hardcoded path, so any edition and any
:: release is found the same way - Build Tools and Community, 2022 and 2026.
::
:: By default the newest installation and its default toolset are used. Set
:: NX_FORCED_VC_TOOLSET_VERSION to pin a specific toolset instead.
::
:: Errors are reported by jumping to a label rather than from inside an if(...) block. Every path
:: here contains "Program Files (x86)", and cmd expands variables when it parses a block, so a path
:: mentioned inside one closes it early - on the branch not taken as readily as on the branch taken.

@echo off

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

:: Re-initializing an environment that is already correct would be harmless but noisy, so it is
:: skipped - unless a specific toolset was asked for and the current one is not it.
if "%VCToolsVersion%" == "" goto :env_not_initialized
if "%NX_FORCED_VC_TOOLSET_VERSION%" == "" goto :already_initialized
if "%VCToolsVersion%" == "%NX_FORCED_VC_TOOLSET_VERSION%" goto :already_initialized
goto :env_not_initialized

:already_initialized

echo MSVC environment is already initialized for version %VCToolsVersion%.
echo:
goto :exit

:env_not_initialized

if not exist %VSWHERE% goto :no_vswhere

:: -products * is required because Build Tools is not one of the products vswhere returns by
:: default; -requires filters out an installation carrying no C++ compiler at all.
set VS_INSTALL_DIR=
for /f "usebackq delims=" %%I in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL_DIR=%%I"

if "%VS_INSTALL_DIR%" == "" goto :no_visual_studio

set "VCVARS_BAT=%VS_INSTALL_DIR%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS_BAT%" goto :no_vcvars

set VCVARS_VER_ARG=
if "%NX_FORCED_VC_TOOLSET_VERSION%" == "" goto :version_not_forced
echo Forcing MSVC version %NX_FORCED_VC_TOOLSET_VERSION%.
echo:
set VCVARS_VER_ARG=-vcvars_ver=%NX_FORCED_VC_TOOLSET_VERSION%

:version_not_forced

@echo on
call "%VCVARS_BAT%" %VCVARS_VER_ARG% || @goto :exit
@echo off

echo:
echo MSVC environment is initialized for version %VCToolsVersion%.
goto :exit

:no_vswhere

echo vswhere.exe was not found. No Visual Studio installation is present.
(call) %= Set ERRORLEVEL to 1 (failure). =%
goto :exit

:no_visual_studio

echo No Visual Studio installation with the C++ toolset was found.
(call) %= Set ERRORLEVEL to 1 (failure). =%
goto :exit

:no_vcvars

echo vcvars64.bat was not found under "%VS_INSTALL_DIR%".
(call) %= Set ERRORLEVEL to 1 (failure). =%
goto :exit

:exit
    @exit /b %ERRORLEVEL% %= Needed for a proper cmd.exe exit status. =%
