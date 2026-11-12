#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
KeyForge 主机动作代理
=====================

键盘只能发 HID 键码，它没法自己"打开网页"或"启动软件"——
这必须由电脑上的一个小程序来执行。本脚本就是那个程序：

    键盘按键 → 固件发出 Report ID 5（1 字节动作编号）→ 本脚本读到 → 执行动作

依赖：
    pip install hidapi

用法：
    python agent.py                 # 用同目录的 actions.json
    python agent.py -c my.json      # 指定配置
    python agent.py --list          # 列出所有 HID 设备（找 VID/PID 用）
    python agent.py --dry-run       # 只打印不执行（调试用）
    python agent.py --demo 3        # 模拟触发 3 号动作，不连键盘

配置文件 actions.json：
{
  "vid": "0x1209",
  "pid": "0x0001",
  "actions": {
    "0": {"name": "打开 B 站",   "type": "url",    "target": "https://www.bilibili.com"},
    "1": {"name": "VS Code",     "type": "app",    "target": "C:/Users/me/AppData/Local/Programs/Microsoft VS Code/Code.exe"},
    "2": {"name": "工作目录",     "type": "folder", "target": "D:/work"},
    "3": {"name": "锁屏",        "type": "cmd",    "target": "rundll32.exe user32.dll,LockWorkStation"}
  }
}
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

REPORT_HOST_ACTION = 0x05
USAGE_PAGE_VENDOR = 0xFF00

DEFAULT_CONFIG = {
    "vid": "0x1209",
    "pid": "0x0001",
    "actions": {}
}


# --------------------------------------------------------------------------
# 配置
# --------------------------------------------------------------------------
def load_config(path):
    """读配置。没有 actions.json 就从 actions.example.json 拷一份，直接能跑。"""
    if not os.path.exists(path):
        example = os.path.join(os.path.dirname(os.path.abspath(path)), "actions.example.json")
        if os.path.exists(example):
            shutil.copyfile(example, path)
            print(f"[i] 没有 {os.path.basename(path)}，已从 actions.example.json 复制一份")
            print("    改成自己的动作后重跑即可")
        else:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(DEFAULT_CONFIG, f, ensure_ascii=False, indent=2)
            print(f"[!] 找不到配置文件，已生成空模板 {path}")
            return None

    with open(path, "r", encoding="utf-8") as f:
        cfg = json.load(f)

    for key, default in DEFAULT_CONFIG.items():
        cfg.setdefault(key, default)
    return cfg


def parse_id(v):
    if isinstance(v, int):
        return v
    return int(str(v), 0)


# --------------------------------------------------------------------------
# 动作执行
# --------------------------------------------------------------------------
def _split_target(target):
    """把 "C:/x/y.exe --flag" 拆成 (文件, 参数)。

    带引号就按引号拆；否则先看整串是不是一个真实存在的文件（路径里有空格的情况，
    比如 "D:\\Microsoft VS Code\\Code.exe"），最后才退化到按第一个空格拆。
    """
    t = target.strip()
    if t.startswith('"'):
        i = t.find('"', 1)
        if i > 0:
            return t[1:i], t[i + 1:].strip()
    if os.path.exists(t):
        return t, ""
    i = t.find(" ")
    if i > 0:
        return t[:i], t[i + 1:].strip()
    return t, ""


def shell_open(target, cwd=None):
    """用 ShellExecuteW 打开，和配置器「试跑」走同一条系统路径。

    之前 url 用 webbrowser、app 用 subprocess，行为和试跑不一致，
    而且 app 目标只要带空格就会被劈成两半（"D:\\Microsoft" + "VS Code/Code.exe"）。
    """
    if sys.platform != "win32":
        if os.path.exists(target):
            subprocess.Popen([target], cwd=cwd)
        else:
            import webbrowser
            webbrowser.open(target)
        return

    import ctypes
    exe, args = _split_target(target)
    # 是文件才拆参数；是网址/文件夹就整串传
    if not os.path.isfile(exe):
        exe, args = target.strip().strip('"'), ""
        cwd = None

    shell32 = ctypes.windll.shell32
    shell32.ShellExecuteW.restype = ctypes.c_void_p      # HINSTANCE 是指针，别用默认 c_int
    rc = shell32.ShellExecuteW(None, "open", exe, args if args else None, cwd, 1) or 0
    if int(rc) <= 32:
        raise OSError(f"ShellExecute 返回 {int(rc)}"
                      f"（2=找不到文件 3=找不到路径 5=拒绝访问 31=无关联程序）")


def run_action(action, dry_run=False):
    name = action.get("name", "(未命名)")
    kind = action.get("type", "")
    target = action.get("target", "")

    print(f"[>] {name}  ←  {kind}: {target}")
    if dry_run:
        return

    try:
        if kind in ("url", "app", "folder"):
            if kind == "url" and target and not re.match(r"^[a-z][a-z0-9+.\-]*://", target, re.I):
                target = "https://" + target      # 和配置器「试跑」的补全规则一致
            cwd = None
            if kind == "app":
                exe, _ = _split_target(target)
                if os.path.isfile(exe):
                    cwd = os.path.dirname(exe) or None   # 用程序自己的目录当工作目录
            shell_open(target, cwd)

        elif kind == "cmd":
            subprocess.Popen(target, shell=True)

        else:
            print(f"[!] 未知类型 {kind!r}")

    except Exception as e:                     # noqa: BLE001
        print(f"[!] 执行失败：{e}")


# --------------------------------------------------------------------------
# 设备
# --------------------------------------------------------------------------
def list_devices():
    try:
        import hid
    except ImportError:
        print("[!] 缺少 hidapi：pip install hidapi")
        return

    for d in hid.enumerate():
        print(f"VID={d['vendor_id']:#06x} PID={d['product_id']:#06x}  "
              f"usage_page={d.get('usage_page', 0):#06x}  "
              f"{d.get('manufacturer_string', '')} {d.get('product_string', '')}")


def find_device(vid, pid):
    import hid

    # 先按 VID/PID + Vendor HID usage page 找
    for d in hid.enumerate(vid, pid):
        if d.get("usage_page", 0) == USAGE_PAGE_VENDOR:
            return d
    # 退回：只按 VID/PID
    found = hid.enumerate(vid, pid)
    return found[0] if found else None


def _load_actions(path):
    """只读 actions 表，供热重载用（配置器自动改写 json 后调用）"""
    with open(path, "r", encoding="utf-8") as f:
        cfg = json.load(f)
    return {int(k): v for k, v in cfg.get("actions", {}).items()}


def run_forever(cfg, dry_run, cfg_path=None):
    try:
        import hid
    except ImportError:
        print("[!] 缺少 hidapi：pip install hidapi")
        print("    Windows 下如果编译失败，可以试： pip install hidapi --only-binary :all:")
        return 1

    vid, pid = parse_id(cfg["vid"]), parse_id(cfg["pid"])
    actions = {int(k): v for k, v in cfg["actions"].items()}

    print(f"[i] 已载入 {len(actions)} 个动作（按键发来的编号 → 这里的槽位号）：")
    for i in sorted(actions):
        a = actions[i]
        print(f"    {i}: {a.get('name', '?')}  [{a.get('type', '?')}]  {a.get('target', '')}")

    print(f"[i] 查找设备 {vid:#06x}:{pid:#06x} …")
    print("[i] Ctrl+C 退出")

    # 动作表热重载：配置器会自动改写 actions.json，这里盯着 mtime，改完立刻生效
    last_mtime = 0.0
    if cfg_path and os.path.exists(cfg_path):
        try:
            last_mtime = os.path.getmtime(cfg_path)
        except OSError:
            pass
    last_check = time.time()

    dev = None
    while True:
        if dev is None:
            info = find_device(vid, pid)
            if info:
                try:
                    dev = hid.device()
                    dev.open_path(info["path"])
                    # 非阻塞轮询：阻塞式 ReadFile 在配置器/其它进程同时打开同一个
                    # HID 集合时会互相抢占，表现为刷屏的 "read error" 重连；
                    # 改成 10ms 轮询后两边可以共存。
                    dev.set_nonblocking(1)
                    print(f"[✓] 已连接：{info.get('product_string') or 'HID 设备'}")
                except Exception as e:         # noqa: BLE001
                    print(f"[!] 打开失败：{e}")
                    dev = None
            else:
                time.sleep(1.0)
                continue

        try:
            data = dev.read(64, 0)             # 非阻塞，立即返回
        except Exception as e:                 # noqa: BLE001
            print(f"[!] 读失败，重连：{e}")
            try:
                dev.close()
            except Exception:                  # noqa: BLE001
                pass
            dev = None
            time.sleep(1.0)
            continue

        if not data:
            time.sleep(0.01)                   # 10ms 轮询，别把 CPU 跑满
            # 每秒看一眼配置文件有没有被改（配置器改完槽位会自动写它）
            if cfg_path and time.time() - last_check > 1.0:
                last_check = time.time()
                try:
                    mt = os.path.getmtime(cfg_path)
                    if mt != last_mtime:
                        last_mtime = mt
                        actions = _load_actions(cfg_path)
                        print(f"[↻] actions.json 变了，已重新载入 {len(actions)} 个动作")
                except OSError:
                    pass
            continue

        report_id = data[0]
        if report_id != REPORT_HOST_ACTION:
            continue

        idx = data[1]
        action = actions.get(idx)
        if action:
            run_action(action, dry_run)
        else:
            print(f"[!] 收到主机动作 {idx}，但 actions.json 里没有槽位 {idx}"
                  f"（配置器里改完槽位后要「导出 actions.json」覆盖到这里）")


# --------------------------------------------------------------------------
class _Tee:
    """把输出同时写到文件和原 stdout。

    pythonw.exe（无控制台）下 sys.stdout 是 None，直接 print() 会抛
    AttributeError 把进程干掉 —— 开机自启必须走这条路，所以这里统一接住。
    """

    def __init__(self, path):
        if os.path.exists(path) and os.path.getsize(path) > 1024 * 1024:
            os.remove(path)                       # 超过 1MB 就重来，别无限长
        self.f = open(path, "a", encoding="utf-8", errors="replace", buffering=1)

    def write(self, s):
        try:
            self.f.write(s)
        except Exception:
            pass
        try:
            if sys.__stdout__:
                sys.__stdout__.write(s)
        except Exception:
            pass

    def flush(self):
        try:
            self.f.flush()
        except Exception:
            pass


def acquire_single_instance():
    """同名互斥体：防止开机自启 + 手动启动同时跑两份。

    两份同时跑会各自执行一次动作（一次按键开两个窗口），必须拦住。
    """
    if sys.platform != "win32":
        return True
    try:
        import ctypes
        handle = ctypes.windll.kernel32.CreateMutexW(None, False, "Global\\MineKeyHostAgent")
        return ctypes.windll.kernel32.GetLastError() != 183   # ERROR_ALREADY_EXISTS
    except Exception:
        return True


def main():
    ap = argparse.ArgumentParser(description="KeyForge 主机动作代理")
    ap.add_argument("-c", "--config", default=os.path.join(os.path.dirname(__file__), "actions.json"))
    ap.add_argument("-l", "--log", help="日志同时写入文件（配合 pythonw 后台运行）")
    ap.add_argument("--list", action="store_true", help="列出 HID 设备")
    ap.add_argument("--dry-run", action="store_true", help="只打印不执行")
    ap.add_argument("--demo", type=int, metavar="N", help="手动触发 N 号动作")
    args = ap.parse_args()

    if args.log:
        tee = _Tee(args.log)
        sys.stdout = tee
        sys.stderr = tee
        print(f"[i] 日志：{os.path.abspath(args.log)}")

    if not acquire_single_instance():
        print("[i] 已有一个代理在运行（互斥体占用），本次退出")
        return 0

    if args.list:
        list_devices()
        return 0

    cfg = load_config(args.config)
    if cfg is None:
        print("    改好 actions.json 再运行一次")
        return 1

    if args.demo is not None:
        a = cfg["actions"].get(str(args.demo))
        if not a:
            print(f"[!] 配置里没有 {args.demo} 号动作")
            return 1
        run_action(a, args.dry_run)
        return 0

    try:
        return run_forever(cfg, args.dry_run, args.config) or 0
    except KeyboardInterrupt:
        print("\n[i] 已退出")
        return 0


if __name__ == "__main__":
    sys.exit(main())
