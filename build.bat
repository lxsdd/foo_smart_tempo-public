@echo off
setlocal enabledelayedexpansion

rem Normalize PATH casing to avoid duplicate Path/PATH keys in inherited env blocks.
set "__FOOBPM_ORIG_PATH=%PATH%"
set "PATH="
set "Path=%__FOOBPM_ORIG_PATH%"
set "__FOOBPM_ORIG_PATH="

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo vswhere.exe not found at !VSWHERE!
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
    set "MSBUILD=%%i"
    goto :found
)

for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -all -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
    set "MSBUILD=%%i"
    goto :found
)

:found
if not defined MSBUILD (
    echo MSBuild.exe not found.
    exit /b 1
)

echo Using MSBuild: !MSBUILD!

set "BUILD_DIR=%~dp0build"
set "BUILD_OK_MARKER=!BUILD_DIR!\last_build_success.marker"
set "EXPERIMENTAL_BUILD_MARKER=!BUILD_DIR!\experimental_build.marker"
set "BUILD_TARGET=Rebuild"
if defined SMART_TEMPO_INCREMENTAL_BUILD set "BUILD_TARGET=Build"
if exist "!BUILD_OK_MARKER!" del /f /q "!BUILD_OK_MARKER!" >nul 2>nul
if not exist "!BUILD_DIR!" mkdir "!BUILD_DIR!" >nul 2>nul

set "EXPERIMENTAL_MSBUILD_PROPS="
if defined SMART_TEMPO_EXPERIMENTAL_DEFINES (
    echo Experimental Smart Tempo defines enabled: !SMART_TEMPO_EXPERIMENTAL_DEFINES!
    set "EXPERIMENTAL_MSBUILD_PROPS=/p:SmartTempoExperimentalDefines=!SMART_TEMPO_EXPERIMENTAL_DEFINES!"
    > "!EXPERIMENTAL_BUILD_MARKER!" echo !SMART_TEMPO_EXPERIMENTAL_DEFINES!
) else if defined SMART_TEMPO_EXPERIMENTAL_HEADER_ACTIVE (
    echo Experimental Smart Tempo header overrides enabled: !SMART_TEMPO_EXPERIMENTAL_HEADER_ACTIVE!
    > "!EXPERIMENTAL_BUILD_MARKER!" echo header: !SMART_TEMPO_EXPERIMENTAL_HEADER_ACTIVE!
) else (
    if exist "!EXPERIMENTAL_BUILD_MARKER!" del /f /q "!EXPERIMENTAL_BUILD_MARKER!" >nul 2>nul
)

echo Building Win32 Solution...
"!MSBUILD!" foo_smart_tempo.sln /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v145 /p:CppLanguageStandard=cpp23 !EXPERIMENTAL_MSBUILD_PROPS! /t:!BUILD_TARGET!
if errorlevel 1 (
    echo Win32 build failed.
    exit /b 1
)

echo Building x64 Solution...
"!MSBUILD!" foo_smart_tempo.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:CppLanguageStandard=cpp23 !EXPERIMENTAL_MSBUILD_PROPS! /t:!BUILD_TARGET!
if errorlevel 1 (
    echo x64 build failed.
    exit /b 1
)

if not exist "!BUILD_DIR!\foo_smart_tempo.dll" (
    echo Win32 output missing: !BUILD_DIR!\foo_smart_tempo.dll
    exit /b 1
)
if not exist "!BUILD_DIR!\x64\foo_smart_tempo.dll" (
    echo x64 output missing: !BUILD_DIR!\x64\foo_smart_tempo.dll
    exit /b 1
)

> "!BUILD_OK_MARKER!" echo Build succeeded at %DATE% %TIME%

echo Build successful.
exit /b 0
