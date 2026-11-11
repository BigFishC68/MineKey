@echo off
REM MineKey 启动器 —— 双击即可
setlocal
set "EXE=%~dp0tools\configurator-win\bin\app\MineKey.exe"
if not exist "%EXE%" (
  echo 首次运行，正在构建...
  call "%~dp0tools\configurator-win\run.bat"
  exit /b
)
start "" "%EXE%"
