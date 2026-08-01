@echo off
REM Build Banjo-Kazooie VR (Windows, Visual Studio + CMake).
REM Run this from a normal command prompt in the repo root.

setlocal
cd /d "%~dp0"

set GEN=Visual Studio 18 2026
set CFG=Release

REM A running exe locks the binary and the link fails with LNK1104/LNK1168.
taskkill /IM Lighthouse.exe /F >nul 2>&1

echo Configuring (vcpkg builds dependencies on first run, including openxr-loader)...
echo Full output -^> build_vr.log
cmake -S . -B build/x64 -G "%GEN%" -A x64 > build_vr.log 2>&1
if errorlevel 1 (
  echo.
  echo Configure failed. Error lines:
  findstr /I /C:"CMake Error" /C:"error" build_vr.log
  echo If "%GEN%" is not installed, change GEN above to your Visual Studio, e.g. "Visual Studio 17 2022".
  exit /b 1
)

echo.
echo Building (%CFG%)... appending to build_vr.log
cmake --build build/x64 --config %CFG% --parallel >> build_vr.log 2>&1

REM Do NOT trust the exit code here: a failed LINK has been observed returning 0.
REM Scan the log for the real failure markers instead.
findstr /I /C:"error C" /C:"error LNK" /C:"fatal error" build_vr.log >nul 2>&1
if not errorlevel 1 (
  echo.
  echo BUILD FAILED. Error lines:
  findstr /I /C:"error C" /C:"error LNK" /C:"fatal error" build_vr.log
  echo Full log: build_vr.log
  exit /b 1
)

if not exist "build\x64\Lighthouse.exe" (
  echo BUILD FAILED - no exe produced. Full log: build_vr.log
  exit /b 1
)

echo.
echo Build OK: build\x64\Lighthouse.exe
echo.
echo Asset archives must sit next to the exe (bring your own ROM):
echo   bk.o2r          - from your Banjo-Kazooie ROM
echo   lighthouse.o2r  - port assets
echo Generate them with:
echo   cmake --build build/x64 --target ExtractAssets --config %CFG%
echo   cmake --build build/x64 --target GeneratePortO2R --config %CFG%
echo.
echo NOTE: a CMake-generated bk.o2r carries no portVersion record, so the game deletes it on
echo launch and asks you to re-extract. Either use the in-game extraction wizard, or stamp it:
echo   python -c "import zipfile,struct; z=zipfile.ZipFile('build/x64/bk.o2r','a'); z.writestr('portVersion', struct.pack('>HHH',1,0,0)); z.close()"
echo.
echo Run with play_vr.bat (or Lighthouse.exe --vr / --novr).
endlocal
