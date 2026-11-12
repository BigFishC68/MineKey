@echo off
cd /d "%~dp0"
title MineKey host-agent - remove autostart

rem ASCII only. Removes the startup shortcut and stops the background agent.

set "LNK=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\MineKey host-agent.lnk"

if exist "%LNK%" (
  del /f /q "%LNK%"
  echo [OK] Startup shortcut removed.
) else (
  echo [i] No startup shortcut found, nothing to remove.
)

echo.
echo Stopping any running background agent (pythonw)...
taskkill /f /im pythonw.exe >nul 2>nul
if errorlevel 1 (
  echo [i] No pythonw process was running.
) else (
  echo [OK] Stopped.
)

echo.
pause >nul
