# MineKey 硬件分支

> CH552G 十一键 + EC11 旋钮键盘的**硬件与固件**：原理图、嘉立创工程、外壳与键帽 3D 模型、固件源码与烧录工具。
>
> 只要改键 / 改灯 / 用主机动作 —— 不用碰这个分支，去 [`software`](../../tree/software) 分支下配置器即可，
> **两者用的是同一套固件协议，software 分支不需要重新烧录**。

![原理图](hardware/keyboard_SCH.jpg)

---

## 规格

| 项目 | 说明 |
|---|---|
| 主控 | CH552G（SOP-16，E8051，24 MHz，16 KB Flash / 1 KB xRAM） |
| 矩阵 | 3 行 × 4 列，行准双向输入 + 列推挽拉低，每键串 1N4148 |
| 旋钮 | EC11 编码器（A = P3.2，B = P3.4，按压在矩阵 R1C4） |
| 灯珠 | 11 颗 WS2812 蛇形串联，数据脚 P3.3，400 mA 电流限幅 |
| 接口 | USB 2.0 全速 Type-C（MC-311S），D+ = P3.6，D− = P3.7（内部 1.5 k 上拉，无需外部电阻） |
| 固件占用 | 约 9.5 KB（`0x0000–0x2558`），用户区上限 `0x3800`，之后是 BootLoader |

**接线映射（踩过坑，勿随意改）**：`ROW1 = 上排`、`ROW3 = 下排`，`COL1..COL4` 左→右。
灯珠串联是蛇形：`CN11 → CN10 → CN9 → CN5 → CN6 → CN7 → CN8 → CN4 → CN3 → CN2 → CN1`。

引脚表见 `firmware/src/board.h` 顶部注释；改板核对记录见 `docs/01-原理图审查.md`。

---

## 目录结构

```
MineKey-hardware/
├── hardware/
│   ├── keyboard_SCH.jpg        原理图（CH552G + 3×4 矩阵 + 11×WS2812 + EC11 + Type-C）
│   ├── keyboard_v1.epro2       嘉立创 EDA 工程（v1，可直接打开改板）
│   ├── keyboard-Body.step      外壳主体 3D 模型（FreeCAD 导出，可打印）
│   ├── keyboard-CAP.step       单颗键帽模型
│   ├── keyboard-CAP-full.step  全套键帽（11 颗）
│   └── CH552DS1.PDF            WCH CH552/CH551 数据手册
├── firmware/
│   ├── src/
│   │   ├── app.c               矩阵扫描、去抖、灯效、配置存取、Feature 协议
│   │   ├── usb.c               USB 描述符与端点（含 HID 报告描述符）
│   │   ├── board.h             板级定义：引脚、矩阵尺寸、默认键位 / 灯效
│   │   ├── ch552.h             寄存器定义
│   │   └── app.h / usb.h
│   ├── test/                   主机侧单元测试桩
│   ├── build.bat               一键构建（需要 SDCC）
│   ├── Makefile
│   └── release/
│       ├── minekey.hex         无灯安全版
│       ├── minekey_led.hex     完整版，带灯效（推荐）
│       ├── minekey_diag.hex    诊断版：烧入后点两颗红灯做里程碑
│       └── minekey.bin
├── tools/
│   ├── ispprog/                命令行烧录器源码（需自备 WCH 官方 DLL，见其 README）
│   ├── flash_cli.bat           安全烧录入口（noprotect，绝不修改配置位）
│   └── verify_hex.py           hex 结构校验（地址范围 / 校验和 / 与 bin 比对）
└── docs/
    ├── 01-原理图审查.md        改板核对记录（P1–P4）
    ├── 02-软件架构与协议.md    报文格式、功能码表、协议细节
    └── 04-固件校验报告.md      每版固件的校验数据 + 真机调试过程
```

---

## 从零做一把

1. **打板**：`hardware/keyboard_v1.epro2` 直接导入嘉立创 EDA，或者照着 `keyboard_SCH.jpg` 自己画。
   器件清单与参数核对见 `docs/01-原理图审查.md`（编码器消抖电容、D+ 上拉、灯珠去耦都记在里面）。
2. **打印外壳**：`keyboard-Body.step` 打主体，`keyboard-CAP-full.step` 打全套键帽
   （只想改单个键的造型就用 `keyboard-CAP.step`）。STEP 是 FreeCAD 导出的标准件，导入 Cura / Bambu Studio 即可切片。
3. **焊接**：11 个热插拔轴座 + 11 颗 WS2812 + EC11 + Type-C + 最小系统（CH552G 只需 VCC/GND/D+/D− 与 D+ 内部上拉）。
   灯珠方向按丝印，串错一颗后面全不亮；蛇形顺序见上方"接线映射"。
4. **烧固件**：见下节。
5. **改键**：去 [`software`](../../tree/software) 分支拿配置器，插上就能改，不用再烧。

---

## 烧固件

需要 **SDCC**（`sdcc` + `packihx` + `makebin`）和一根直连主板的 USB 线。

**现成固件直接烧**：双击 `tools\flash_cli.bat`（默认烧 `firmware\release\minekey_led.hex`），
也可以把任意 hex 拖到它上面。

它调用 WCH 官方命令行烧录器，走 `noprotect` 模式 —— **只擦写代码区，绝不碰配置位 / 保护位**。

> ⚠️ 先关掉 WCHISPStudio / WCHISPTool，它们会独占 HID 设备，导致命令行报 `FlashProgramB -> 0`。

烧完工具不会让芯片自动运行，**拔插一次 USB** 即可。

**从源码构建**：

```bash
cd firmware
build.bat            # 需要 SDCC，产物落在 firmware/ 根目录
```

构建后建议校验一次（**必须 < 0x3800**，越界会覆盖 BootLoader）：

```bash
python tools/verify_hex.py firmware/release/minekey.hex
```

`tools/ispprog` 调用的 `WCH55xISPDLL.dll` 等动态库版权归南京沁恒，**不在本仓库内**，
需自行从 WCHISPTool 安装目录取，编译方法见 `tools/ispprog/README.md`。

---

## 协议速查

一个 HID 接口、四个集合，用 Report ID 区分：

| Report ID | 方向 | 长度 | 用途 |
|---|---|---|---|
| 1 | IN | 8 | 键盘（修饰键 + 6 键） |
| 2 | IN | 2 | 多媒体（Consumer usage 16 位） |
| 3 | IN | 9 | 状态（矩阵位图 + 旋钮刻度） |
| 4 | IN | 4 | 鼠标（按键 + 滚轮） |
| 5 | IN | 1 | 主机动作编号 |
| 0x10 | Feature | 63 | 配置通道（命令 + 参数） |

功能码：`0x01xx` 普通键、`0x02xx` 多媒体、`0x03xx` 鼠标、`0x04xx` 灯效动作、
`0x5MKK` 组合键（M = 修饰位）、`0x06xx` 主机动作、`0x07XY` 光标相对移动。

Feature 负载 63 字节（+ 报告 ID = 64，正好卡在 EP0 单包上限），控制传输不分包。
完整帧格式见 [`docs/02-软件架构与协议.md`](docs/02-软件架构与协议.md)。

---

## 已知坑（都踩过了，改代码前先看）

1. **GUI 的「下载」会去写配置位，某些芯片上会卡在「配置位校验失败」**
   日志里没有「擦除中 / 编程中」就说明代码区没被碰、芯片是好的，**重试或换回 GUI 通常还能烧**
   （2026-11-16 实测：一次失败后原地重试即成功，不要当成芯片报废）。
2. **拿 `0` 当哨兵值的地方** `host_action_pending == 0` 曾被当成"没有待发动作"，
   导致槽位 0 永远发不出报文（表现为"文件夹能用、开网页没反应"）。已改用 `0xFF`。
3. **动作 / 主机动作必须占住 `active_sw`**，否则"按下沿"判定失效，按住不放会每 1 ms 触发一次。
4. **写完配置必须 `SAVE` 才会断电保留**，只写 RAM 的话界面显示成功、拔电回旧值。
5. **救援后门**：长按旋钮 3 秒无条件跳回 BootLoader，固件跑飞也能重烧。

---

## 分支说明

| 分支 | 面向 | 内容 |
|---|---|---|
| `main` | 想一次看全的人 / 自己 fork 二次开发 | 硬件 + 固件 + 软件 + 全部文档 |
| `software` | 已经拿到成品键盘的用户 | 只含驱动 / 配置器 / 主机代理，不含固件与烧录 |
| **`hardware`（当前）** | **想做板、刷固件、打印外壳** | **原理图、嘉立创工程、外壳与键帽 STEP、固件源码与 hex、烧录工具** |

---

## 授权

以 **GNU General Public License v3.0** 发布，详见 [LICENSE](LICENSE)。

`tools/ispprog/` 调用的 WCH 官方 ISP 动态库版权归南京沁恒微电子，**不包含在本仓库内**。
