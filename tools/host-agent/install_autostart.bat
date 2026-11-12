@echo off
cd /d "%~dp0"
title MineKey host-agent - install autostart

rem ASCII only. Registers a startup shortcut so the host-action agent
rem runs at login in the background (pythonw = no console window).

set "PY=D:\Python\python.exe"
if not exist "%PY%" set "PY=python"
for %%I in ("%PY%") do set "PYDIR=%%~dpI"
set "PYW=%PYDIR%pythonw.exe"
if not exist "%PYW%" set "PYW=pythonw.exe"

if not exist "actions.json" (
  echo [i] actions.json not found, copying from actions.example.json
  copy /y "actions.example.json" "actions.json" >nul
)

set "STARTUP=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"
set "LNK=%STARTUP%\MineKey host-agent.lnk"

echo ============================================================
echo  Install autostart for the host-action agent
echo   python : %PYW%
echo   script : %CD%\agent.py
echo   link   : %LNK%
echo ============================================================
echo.

powershell -NoProfile -ExecutionPolicy Bypass -Command "$W=New-Object -ComObject WScript.Shell; $s=$W.CreateShortcut('%LNK%'); $s.TargetPath='%PYW%'; $s.Arguments='agent.py -c actions.json --log agent.log'; $s.WorkingDirectory='%CD%'; $s.WindowStyle=7; $s.Description='MineKey host action agent'; $s.Save()"

if exist "%LNK%" (
  echo [OK] Startup shortcut created.
  echo      It will start automatically next time you log in.
) else (
  echo [X] Failed to create the shortcut.
  pause
  exit /b 1
)

echo.
echo Starting it now (background, no window)...
start "" "%PYW%" agent.py -c actions.json --log agent.log

echo.
echo Log file: %CD%\agent.log
echo To stop it:  taskkill /f /im pythonw.exe   (or remove the shortcut)
echo To undo  :  run uninstall_autostart.bat
pause >nul
