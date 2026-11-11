@echo off
cd /d "%~dp0"
title MineKey Host Agent

rem ASCII only: cmd.exe mangles UTF-8 Chinese in .bat files
set PY=D:\Python\python.exe
if not exist "%PY%" set PY=python

echo ============================================
echo  MineKey host-action agent
echo  Press a key mapped to "host action" to run it
echo  Actions are defined in actions.json (0..N)
echo  Close this window to stop the agent.
echo ============================================
echo.

"%PY%" agent.py -c actions.json

echo.
echo [exited] press any key to close
pause >nul
