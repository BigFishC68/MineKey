# ispprog · 命令行烧录器

调 WCH 官方 ISP 动态库的极简命令行烧录工具：只做「擦除 → 编程 → 校验」，
内部 `SetIspOption(1, noprotect)` —— **完全不碰配置位/保护位**。

## 为什么需要它

WCHISPTool 的 GUI 流程是「**写配置位** → 擦除 → 编程」，配置位那一步在部分
CH552 上会失败并直接中止；反复重试会把芯片拖进异常状态（本项目烧坏过两颗，
日志都停在「写配置位失败」，代码区一行都没写过）。

这条路绕开那一步，是日常烧固件的推荐方式。

## 编译

> ⚠️ 本仓库**不包含 WCH 的二进制库**（版权归南京沁恒微电子，不随本项目开源分发）。
> 需要自己从已安装的 WCHISPTool 目录里拷到本文件夹：

```
WCH55xISPDLL.dll
WCHISPAPI.dll
CH343PT.DLL
CH375DLL.dll
```

通常位于 `C:\Program Files (x86)\WCHISPTool\` 或 `WCHISPTool_CH54x-55x\` 下。

然后：

```bash
dotnet build -c Release
```

## 用法

```bash
ispprog.exe <固件.hex>
```

前提条件：

- **所有 GUI 烧录软件（WCHISPStudio / WCHISPTool）必须先关掉**，否则会报
  `FlashProgramB -> 0`（设备被独占，能枚举但拿不到编程句柄）
- 芯片停在 BootLoader（上电即可，冷启动时 BootLoader 有几秒等待窗口）
- 烧完工具**不会**让芯片自动运行，**拔插一次 USB** 才会跑用户程序

输出示例：

```
[1] SetIspOption -> 1 (mode=noprotect)
[2] EnumDevices -> 1 台  dev0: index=0 mcu=0x00 (cold)
[3] 载入 minekey_led.hex  27264 字节（HEX 文本）
[4] FlashProgramB -> 1
结果：成功
```

> `FlashVerifyB` 的返回值没有计入结论，所以烧完仍建议实测（例如确认设备是否
> 枚举成 `1209:0001`）。

## 仓库根目录的快捷方式

`tools/flash_cli.bat` 已经把这个工具包了一层，双击即烧 `firmware/release/minekey_led.hex`，
也可以把其它 hex 拖到它上面。
