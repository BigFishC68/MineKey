@echo off
REM MineKey 配置器 - 启动前先增量构建一次，保证跑的是最新代码
REM （框架依赖版，需要 .NET 10 Desktop Runtime；WebView2 运行时 Win10/11 自带）
setlocal
cd /d "%~dp0"

echo Building...
dotnet publish -c Release -o bin\app --nologo
if errorlevel 1 (
  echo.
  echo [warn] 构建失败 —— 多半是配置器还开着，文件被占用。
  echo        关掉配置器窗口后重跑本脚本即可；这次先用现有版本启动。
  echo.
)

if not exist "bin\app\MineKey.exe" (
  echo [error] bin\app\MineKey.exe 不存在，构建没成功。
  pause
  exit /b 1
)

start "" "bin\app\MineKey.exe"
exit /b 0
