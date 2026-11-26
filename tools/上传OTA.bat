@echo off
setlocal

rem ============================================================
rem  Double-click to compile + OTA upload to a manually set IP.
rem  IP is stored in ota_ip.txt (first line) next to this file.
rem ============================================================

cd /d "%~dp0.."

set "IP="
if exist "%~dp0ota_ip.txt" (
  for /f "usebackq tokens=1 delims= " %%a in ("%~dp0ota_ip.txt") do (
    if not defined IP set "IP=%%a"
  )
)

echo.
echo   Target IP: %IP%
echo.
set /p NEWIP=  New IP (press Enter to keep):
if not "%NEWIP%"=="" set "IP=%NEWIP%"

if "%IP%"=="" (
  echo   [ERROR] no IP
  pause
  exit /b 1
)

echo %IP%> "%~dp0ota_ip.txt"

echo.
echo   ===== build + OTA =====
echo.
set DSH_OTA_GBK=1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ota.ps1" -Ip %IP%
set RC=%ERRORLEVEL%

echo.
if "%RC%"=="0" (
  echo   ===== DONE =====
) else (
  echo   ===== FAILED ^(exit %RC%^) =====
  echo   Tip: device busy fetching data; just run again.
)
echo.
pause
endlocal