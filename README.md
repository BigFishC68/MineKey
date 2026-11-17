# MineKey 配置器

给 MineKey（CH552G · 11 键 + EC11 旋钮）改键位、灯效、主机动作的工具。
改动通过 HID 写进键盘的 DataFlash，断电保留，不需要烧固件。

## 打开

- **桌面版**：双击 `MineKey.bat`，首次会自动构建，需要 .NET 10 SDK
- **网页版**：Chrome / Edge 打开 `tools/configurator/index.html`，走 WebHID，不用装东西

左边点键位选中，右边分「按键 / 动作 / 灯光 / 设备」四页。
改完自动存盘（停手 0.7 秒后写入），拔电重插依然生效。

## 主机动作

让按键开网页、开软件、开文件夹、执行命令。

```bash
cd tools/host-agent
pip install hidapi
python agent.py          # 或双击 start_agent.bat，带窗口方便看日志
```

首次运行会把 `actions.example.json` 复制成 `actions.json`，改成自己的动作后重跑。
想开机自启就双击 `install_autostart.bat`（后台运行，无窗口），撤销用 `uninstall_autostart.bat`。

配置器「动作」页填槽位，按键页把某个键设成「主机动作 + 槽位号」。
代理不运行，主机动作就不会触发。

## 命令行

```bash
python tools/set_keymap.py    # 交互式改键
python tools/set_led.py       # 改灯效
```

## 默认键位

| 排 | 第 1 列 | 第 2 列 | 第 3 列 | 第 4 列 |
|---|---|---|---|---|
| 上 | CN9 = 9 | CN10 = 0 | CN11 = Backspace | 旋钮 |
| 中 | CN5 = 5 | CN6 = 6 | CN7 = 7 | CN8 = 8 |
| 下 | CN1 = 1 | CN2 = 2 | CN3 = 3 | CN4 = 4 |

旋钮顺时针音量 +、逆时针音量 −、按下静音。出厂灯效呼吸 30% `#F77634`。

## 常见问题

- **看不到设备**：网页版要允许 WebHID；桌面版不能被 WCHISPStudio 之类的工具独占设备。
- **改了又变回去**：配置要 `SAVE` 才落盘。配置器已自动处理，自己写工具时记得补 `CMD_SAVE`。
- **主机动作没反应**：代理没运行，或 `actions.json` 里没有这个槽位。

## 协议

写自己的工具时看。一个 HID 接口、四个集合，用 Report ID 区分：

| Report ID | 方向 | 长度 | 用途 |
|---|---|---|---|
| 1 | IN | 8 | 键盘，修饰键 + 6 键 |
| 2 | IN | 2 | 多媒体 |
| 3 | IN | 9 | 状态，矩阵位图 + 旋钮刻度 |
| 4 | IN | 5 | 鼠标，按键 + X/Y + 滚轮 |
| 5 | IN | 1 | 主机动作编号 |
| 0x10 | Feature | 64 | 配置通道 |

功能码：`0x01xx` 普通键、`0x02xx` 多媒体、`0x03xx` 鼠标、`0x04xx` 灯效、`0x5MKK` 组合键、
`0x06xx` 主机动作、`0x07XY` 光标移动（X 速度档，Y 方向）。细节见 `docs/02-软件架构与协议.md`。

## 许可

GPL-3.0，见 [LICENSE](LICENSE)。
