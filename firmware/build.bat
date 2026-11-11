@echo off
REM ============================================================================
REM  MineKey CH552 固件编译（不依赖 make，直接调 sdcc）
REM
REM  用法：
REM    build.bat          带灯效的完整版本（默认）
REM    build.bat noled    不点灯的安全版本（怀疑灯带拖垮供电时用）
REM
REM  产物：firmware\minekey.hex
REM
REM  !!! 为什么要区分 !!!
REM  11 颗 WS2812 全白约 660mA，超过 USB 口 500mA 上限。WS2812 的位时序如果
REM  没标定准，灯珠会把 0 也读成 1，固件里的 400mA 限流就形同虚设（限流算的是
REM  led_buf，实际发出去的是全 1），整块板会被拖到欠压复位，连 BootLoader 都
REM  抓不到。时序已按 24MHz/1T 核算过，这里默认开启；怀疑供电时用 noled。
REM ============================================================================
setlocal
cd /d "%~dp0"

set LEDMODE=1
if /i "%~1"=="noled" set LEDMODE=0

REM ---------- 1. 找 SDCC：先看 PATH，再试常见安装目录 ----------
where sdcc >nul 2>nul
if not errorlevel 1 goto have_sdcc

for %%D in (
  "D:/Keyboard_Frame/SDCC/bin"
  "C:/Program Files/SDCC/bin"
  "C:/Program Files (x86)/SDCC/bin"
  "C:/SDCC/bin"
  "D:/SDCC/bin"
  "%LOCALAPPDATA%\SDCC\bin"
) do (
  if exist "%%~D\sdcc.exe" set "PATH=%%~D;%PATH%"
)

where sdcc >nul 2>nul
if errorlevel 1 (
  echo.
  echo [error] 找不到 sdcc.exe
  echo         已试过 PATH 和常见安装目录。
  echo         请把 SDCC 安装目录下的 bin 加进系统 PATH 后重开窗口。
  echo.
  pause
  exit /b 1
)

:have_sdcc
if "%LEDMODE%"=="1" (
  set EXTRAFLAG=
  echo [0/3] 模式：带灯效（默认）
) else (
  set EXTRAFLAG=-DNO_LED_OUTPUT
  echo [0/3] 模式：不点灯
)
sdcc -v 2>&1 | findstr /i "SDCC"

REM ---------- 2. 编译 ----------
REM  --xram-size 0x380 : 0x380..0x3FF 留给 USB 端点缓冲区（Ep0Buffer/Ep1Buffer）
REM  --code-size 0x3800: 0x3800 起是 WCH BootLoader，不许覆盖
REM  注意：sdcc 一次只能编译一个 .c，必须先各自 -c 成 .rel 再链接
set FLAGS=-mmcs51 --model-small --std-c99 --opt-code-size --xram-loc 0x0000 --xram-size 0x380 --iram-size 0x100 --code-size 0x3800 --Werror

if not exist build mkdir build
if not exist release mkdir release

echo [1/3] 编译 app.c / usb.c ...
sdcc %FLAGS% %EXTRAFLAG% -c -o build\app.rel src\app.c
if errorlevel 1 goto fail
sdcc %FLAGS% -c -o build\usb.rel src\usb.c
if errorlevel 1 goto fail

echo [2/3] 链接 ...
sdcc -mmcs51 --model-small --xram-loc 0x0000 --xram-size 0x380 --iram-size 0x100 --code-size 0x3800 -o build\minekey.ihx build\app.rel build\usb.rel
if errorlevel 1 goto fail

echo [3/3] 转 HEX / BIN ...
packihx build\minekey.ihx > release\minekey.hex
if errorlevel 1 goto fail
makebin -p build\minekey.ihx release\minekey.bin >nul 2>nul

echo.
echo ============ 编译完成 ============
dir /b release\minekey.hex release\minekey.bin 2>nul
echo.
echo 资源占用：
findstr /c:"ROM/EPROM/FLASH" /c:"EXTERNAL RAM" build\minekey.mem
echo.
echo 下一步：关掉所有烧录软件，双击 tools\flash_cli.bat 烧 release\minekey.hex
echo.
pause
exit /b 0

:fail
echo.
echo [error] 编译失败，看上面的报错。
echo         若是 "Could not get N consecutive bytes in internal RAM"，说明内部 RAM 不够，
echo         把大数组改成 __xdata（见 app.c 顶部注释）。
echo.
pause
exit /b 1
