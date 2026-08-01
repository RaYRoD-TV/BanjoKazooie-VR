@echo off
REM Launch Banjo-Kazooie in VR.
REM
REM Start your VR runtime FIRST (Quest Link / Air Link / Virtual Desktop / SteamVR) and make sure the
REM headset is AWAKE - a sleeping headset reports XR_ERROR_FORM_FACTOR_UNAVAILABLE and the game just
REM runs flat, which looks identical to VR being broken.
REM
REM Pass --novr to force the flat game. With no flag at all the game probes for a headset and enables
REM VR only if one answers, so the same exe works either way.

setlocal
cd /d "%~dp0build\x64"

if not exist "Lighthouse.exe" (
  echo Lighthouse.exe not found - run build_vr.bat first.
  exit /b 1
)
if not exist "bk.o2r" (
  echo bk.o2r not found next to the exe. Generate it from your ROM ^(see build_vr.bat^).
  exit /b 1
)

start "" "Lighthouse.exe" --vr %*
endlocal
