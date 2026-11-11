@echo off
REM Build a standalone single-file EXE (no .NET runtime needed on the target PC).
REM Output: dist\MineKey.exe  (about 90-140 MB, WebView2 runtime still required by Windows)
setlocal
cd /d "%~dp0"

dotnet publish -c Release -r win-x64 --self-contained true ^
  -p:PublishSingleFile=true ^
  -p:IncludeNativeLibrariesForSelfExtract=true ^
  -p:EnableCompressionInSingleFile=true ^
  -o dist --nologo

if errorlevel 1 (
  echo Publish failed.
  pause
  exit /b 1
)

echo.
echo Done: dist\MineKey.exe
echo Note: the www\ folder must stay next to the exe.
if exist "dist\www\index.html" (echo www\ OK) else (echo WARNING: www\ missing)
pause
