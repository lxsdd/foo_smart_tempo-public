@echo off
setlocal EnableExtensions

set "PROJECT_NAME=foo_smart_tempo"
set "PROJECT_VERSION=2.2.0"

if not "%~1"=="" (
    set "ROOT_DIR=%~1"
) else (
    set "ROOT_DIR=%~dp0"
)
for %%I in ("%ROOT_DIR%") do set "ROOT_DIR=%%~fI"
if not "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR%\"

set "BUILD_DIR=%ROOT_DIR%build"
set "STAGE_DIR=%BUILD_DIR%\package_tmp"
set "COMPONENT_FILE=%BUILD_DIR%\%PROJECT_NAME%_%PROJECT_VERSION%.fb2k-component"
set "BUILD_OK_MARKER=%BUILD_DIR%\last_build_success.marker"

if not exist "%BUILD_OK_MARKER%" (
    echo [FEHLER] Kein gueltiger Build-Marker gefunden.
    echo         Bitte zuerst .\build.bat erfolgreich ausfuehren.
    exit /b 1
)
if not exist "%BUILD_DIR%\foo_smart_tempo.dll" (
    echo [FEHLER] Win32 DLL fehlt: %BUILD_DIR%\foo_smart_tempo.dll
    exit /b 1
)
if not exist "%BUILD_DIR%\x64\foo_smart_tempo.dll" (
    echo [FEHLER] x64 DLL fehlt: %BUILD_DIR%\x64\foo_smart_tempo.dll
    exit /b 1
)

echo [0/4] Pruefe Build-Frische...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "$markerPath = '%BUILD_OK_MARKER%';" ^
  "$sourceRoots = @((Join-Path '%ROOT_DIR%' 'src\\foo_smart_tempo'));" ^
  "$latestSource = $null;" ^
  "foreach($root in $sourceRoots){ if(Test-Path $root){ $cand = Get-ChildItem -Path $root -Recurse -File | Where-Object { $_.Extension -in '.cpp','.c','.h','.hpp','.rc','.inl','.inc' } | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1; if($cand -ne $null -and ($latestSource -eq $null -or $cand.LastWriteTimeUtc -gt $latestSource.LastWriteTimeUtc)){ $latestSource = $cand } } }" ^
  "$latestBuildInput = $latestSource;" ^
  "$metaFiles = @((Join-Path '%ROOT_DIR%' 'foo_smart_tempo.sln'), (Join-Path '%ROOT_DIR%' 'build.bat'), (Join-Path '%ROOT_DIR%' 'src\\foo_smart_tempo\\foo_smart_tempo.vcxproj'));" ^
  "foreach($m in $metaFiles){ if(Test-Path $m){ $fi = Get-Item $m; if($latestBuildInput -eq $null -or $fi.LastWriteTimeUtc -gt $latestBuildInput.LastWriteTimeUtc){ $latestBuildInput = $fi } } }" ^
  "if($latestBuildInput -ne $null){ $markerTime = (Get-Item $markerPath).LastWriteTimeUtc; if($latestBuildInput.LastWriteTimeUtc -gt $markerTime){ Write-Host ('[FEHLER] Build ist veraltet. Neuester Input: ' + $latestBuildInput.FullName); Write-Host '         Bitte zuerst .\\build.bat erneut ausfuehren.'; exit 2 } }"
if errorlevel 1 exit /b 1

echo [1/4] Bereite Ordnerstruktur vor...
if exist "%STAGE_DIR%" rd /s /q "%STAGE_DIR%" || exit /b 1
mkdir "%STAGE_DIR%" || exit /b 1
mkdir "%STAGE_DIR%\x64" || exit /b 1

echo [2/4] Kopiere Dateien...
copy "%BUILD_DIR%\foo_smart_tempo.dll" "%STAGE_DIR%\" >nul || exit /b 1
copy "%ROOT_DIR%licenses\kissfft_license.txt" "%STAGE_DIR%\" >nul || exit /b 1
copy "%BUILD_DIR%\x64\foo_smart_tempo.dll" "%STAGE_DIR%\x64\" >nul || exit /b 1

echo [3/4] Erstelle fb2k-component Archiv (via 7-Zip)...
if exist "%COMPONENT_FILE%" del /f /q "%COMPONENT_FILE%" || exit /b 1

pushd "%STAGE_DIR%" || exit /b 1
if exist "C:\Program Files\7-Zip\7z.exe" (
    "C:\Program Files\7-Zip\7z.exe" a -tzip "%COMPONENT_FILE%" * >nul || (popd & exit /b 1)
) else if exist "C:\Program Files (x86)\7-Zip\7z.exe" (
    "C:\Program Files (x86)\7-Zip\7z.exe" a -tzip "%COMPONENT_FILE%" * >nul || (popd & exit /b 1)
) else (
    echo [WARNUNG] 7-Zip wurde nicht gefunden! Nutze tar-Fallback...
    tar -a -c -f "%COMPONENT_FILE%" * >nul 2>nul
    if errorlevel 1 (
        popd
        echo [FEHLER] Weder 7-Zip noch funktionierendes tar gefunden.
        exit /b 1
    )
)
popd

echo [4/4] Raeume temporaere Dateien auf...
rd /s /q "%STAGE_DIR%" || exit /b 1
del /f /q "%BUILD_OK_MARKER%" >nul 2>nul

echo ---------------------------------------------------
echo ERFOLG! Datei %COMPONENT_FILE% ist jetzt 100%% foobar-kompatibel.
echo ---------------------------------------------------
exit /b 0

