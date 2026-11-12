@echo off
chcp 936 >nul
cd /d "%~dp0"
title MineKey FLASH (safe mode, no config bits)

rem ASCII-only file. Uses the WCH command line programmer in noprotect mode:
rem it erases + programs + verifies the code area and NEVER touches the
rem config bits / code protection. The GUI tool's "write config" step is
rem what killed two chips, so prefer this for day to day flashing.

set TOOL=D:\Keyboard_Frame\WCHISPTool\WCHISPTool_CH54x-55x\ispprog\ispprog.exe

rem Default firmware lives in firmware\release\. Path is relative to this
rem script, so the repo can be moved anywhere.
set HEX=%~1
if "%HEX%"=="" set HEX=%~dp0..\firmware\release\minekey_led.hex

rem Allow a bare file name: "flash_cli.bat minekey.hex"
if not exist "%HEX%" if exist "%~dp0..\firmware\release\%HEX%" set HEX=%~dp0..\firmware\release\%HEX%

if not exist "%TOOL%" (
  echo [X] programmer not found: %TOOL%
  pause
  exit /b 1
)
if not exist "%HEX%" (
  echo [X] hex not found: %HEX%
  pause
  exit /b 1
)

echo ============================================================
echo  MineKey safe flash
echo  hex  : %HEX%
echo  mode : noprotect  ^(config bits untouched^)
echo ============================================================
echo.
echo  Close WCHISPStudio / WCHISPTool before running this!
echo  The chip must be sitting in BootLoader (power cycle it now).
echo.

"%TOOL%" "%HEX%"

echo.
echo If it says OK, unplug and replug the USB so the board runs.
pause >nul
