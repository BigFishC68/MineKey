# MineKey 配置器（软件分支）

> CH552 十一键 + EC11 旋钮键盘的**配套驱动**。硬件与固件已经做完 —— 你拿到的是一把能用的键盘，
> 这个分支只负责一件事：**在电脑上改键位、改灯效、设主机动作**，全部走 HID 写入键盘的 DataFlash，不用烧固件。

想做板、刷固件、打印外壳 → 看 [`hardware`](../../tree/hardware) 分支。
想要完整源码（硬件 + 固件 + 软件都在一个目录） → 看 [`main`](../../tree/main)。

---

## 这个分支有什么 / 没有什么

| 有 | 没有 |
|---|---|
| 桌面配置器（C# + WebView2，直接调 HID） | 固件源码与 `release/*.hex` |
| 网页配置器（纯 HTML/JS，Chrome/Edge 的 WebHID） | 原理图、嘉立创工程、外壳 STEP |
| 主机动作代理（按键开网页 / 开软件 / 执行命令） | 烧录工具（`ispprog`、`flash_cli.bat`） |
| 命令行改键 / 改灯脚本（`set_keymap.py`、`set_led.py`） | 固件构建与校验工具（`verify_hex.py` 等） |
| 使用说明书 `docs/03-使用说明.md`、协议说明 `docs/02-软件架构与协议.md` | —— |

---

## 目录结构

```
MineKey-software/
├── MineKey.bat                 桌面配置器启动器（首次运行自动构建）
├── tools/
│   ├── configurator/           配置器前端（HTML/JS/CSS，浏览器可直接打开）
│   ├── configurator-win/       桌面版外壳（C# + WebView2，HID 走原生 API）
│   │   ├── run.bat             增量构建 + 启动（需要 .NET 10 Desktop Runtime / SDK）
│   │   └── build-release.bat   打独立单文件 exe（自包含，约 90–140 MB）
│   ├── host-agent/             主机动作代理（Python + hidapi）
│   ├── set_keymap.py           命令行改键（写 DataFlash，不重烧）
│   ├── set_led.py              命令行改灯效
│   └── hidtest.ps1             Feature 通道往返自检
├── docs/
│   ├── 02-软件架构与协议.md     报文格式、功能码表（要自己写工具时看这份）
│   └── 03-使用说明.md           面向使用者的完整操作手册
└── assets/                     图标与界面截图
```

---

## 三步上手

### 1. 打开配置器

**方式 A（推荐，桌面版）**：双击根目录 `MineKey.bat`。
首次运行会自动 `dotnet build`，需要 **.NET 10 Desktop Runtime**（或 SDK）。
不想装 SDK，就用 `tools/configurator-win/build-release.bat` 打一个自包含 exe。

**方式 B（网页版）**：Chrome / Edge 打开 `tools/configurator/index.html`，走 WebHID，不需要装任何东西，
第一次会弹出设备授权，选中 `MineKey` 即可。

### 2. 改键 / 改灯

左边点键位选中，右边分「按键 / 动作 / 灯光 / 设备」四页：

- **按键**：普通键、多媒体、鼠标、光标移动、组合键、内部动作、主机动作
- **灯光**：8 种灯效（常亮 / 呼吸 / 彩虹 / 按压缩放 / 关闭 / 彩虹呼吸 / 单点扫描 / 星光），可给每颗灯单独配色
- **旋钮**：顺时针 / 逆时针 / 按下 三个方向分别设定

改完**自动写进键盘并落盘**（700 ms 去抖），拔电重插依然生效。**不需要重烧固件。**

### 3. 主机动作（可选，让键开网页 / 开软件）

```bash
cd tools/host-agent
pip install hidapi
python agent.py            # 或双击 start_agent.bat（带窗口，方便看日志）
```

首次运行会把 `actions.example.json` 复制成 `actions.json`（**这份是你自己的，不会提交**），
改完重跑即可。想开机自启就双击 `install_autostart.bat`（用 `pythonw.exe` 后台跑），
撤销用 `uninstall_autostart.bat`。

然后在配置器「动作」页填槽位 → 按键页把某个键设成「主机动作 + 槽位号」。

> 代理进程关掉 = 主机动作失效，这是设计如此：键盘只发动作编号，执行靠电脑上的进程。

---

## 默认键位

| 排 | 第 1 列 | 第 2 列 | 第 3 列 | 第 4 列 |
|---|---|---|---|---|
| 上排 | CN9 = **9** | CN10 = **0** | CN11 = **Backspace** | 旋钮 |
| 中排 | CN5 = **5** | CN6 = **6** | CN7 = **7** | CN8 = **8** |
| 下排 | CN1 = **1** | CN2 = **2** | CN3 = **3** | CN4 = **4** |

旋钮：顺时针 音量 +、逆时针 音量 −、**按下 静音**。出厂灯效：呼吸 / 亮度 30% / `#F77634` 全填。

命令行也能改（键盘插着就行，不重烧）：

```bash
python tools/set_keymap.py     # 交互式改键，写完回读校验
python tools/set_led.py        # 改灯效
```

---

## 设备与协议（写自己的工具时看）

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

完整帧格式见 [`docs/02-软件架构与协议.md`](docs/02-软件架构与协议.md)。

---

## 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| 配置器打开看不到设备 | 桌面版要有 HID 访问权限（普通用户即可）；网页版要 Chrome/Edge 并允许 WebHID，且同一设备不能被 WCHISPStudio 之类工具独占 |
| 改了设置，拔电又变回去 | 配置写入后要 `SAVE` 才落盘。配置器已自动存盘，自己写工具时记得补 `CMD_SAVE` |
| 主机动作没反应 | 代理没运行，或这台电脑的 `actions.json` 没配对应槽位；槽位 0 曾被当成"空"，固件已改用 `0xFF` 作哨兵，旧固件请先升级 |
| 组合键 / 光标移动行为怪 | 见 `docs/03-使用说明.md` 第五章（修饰键运行原理）与第七章（灯光电流限幅 400 mA） |
| 首次点 `MineKey.bat` 卡在构建 | 需要 .NET 10 SDK；构建报文件占用说明配置器还开着，关掉重跑 |

---

## 分支说明

| 分支 | 面向 | 内容 |
|---|---|---|
| `main` | 想一次看全的人 / 自己 fork 二次开发 | 硬件 + 固件 + 软件 + 全部文档 |
| **`software`（当前）** | **已经拿到成品键盘的用户** | **只含驱动 / 配置器 / 主机代理** |
| `hardware` | 想做板、烧固件、打印外壳 | 原理图、嘉立创工程、外壳与键帽 STEP、固件源码与 hex、烧录工具 |

---

## 授权

以 **GNU General Public License v3.0** 发布，详见 [LICENSE](LICENSE)。

字体 [Zpix 最像素](https://github.com/SolidZORO/zpix-pixel-font) 遵循其自身授权。
