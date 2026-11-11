#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
一键改写 11 个键的功能码，并写入 DataFlash（不需要重烧固件）
==========================================================

原理：配置通道 = HID Feature Report 0x10（负载 63 字节，报告 ID + 63 = 64 单包）。
      CMD_SET_ALL(0x04) 一次性写全部键 → CMD_SAVE(0x09) 落盘 →
      CMD_RELOAD(0x21) 强制从 Flash 重读 → CMD_GET_KEYMAP(0x02) 回读比对。

用法：
    python set_keymap.py            写入内置键位表并校验
    python set_keymap.py --read     只读当前键位，不修改
    python set_keymap.py --key 3 0x011E   只改第 3 号键（0 基）为某个功能码
    python set_keymap.py --factory  恢复出厂默认并落盘

依赖：pip install hidapi
"""

import argparse
import os
import sys
import time

VID, PID = 0x1209, 0x0001
VENDOR_USAGE_PAGE = 0xFF00
REPORT_ID = 0x10
PLEN = 63                      # Feature 负载长度（不含报告 ID）

CMD_INFO = 0x01
CMD_GET_KEYMAP = 0x02
CMD_SET_KEY = 0x03
CMD_SET_ALL = 0x04
CMD_SAVE = 0x09
CMD_FACTORY = 0x0A
CMD_RELOAD = 0x21

# 编码：0x01xx = 普通键(HID usage)，0x02xx = 多媒体 usage，0x5MKK = 组合键
#      M = 修饰位(Ctrl1 Shift2 Alt4 Win8)，KK = usage
UP, SIX, SEVEN, EIGHT = 0x0701, 0x0123, 0x0124, 0x0125   # UP = 光标↑（鼠标相对移动 1）
ONE, TWO, THREE, FOUR = 0x011E, 0x011F, 0x0120, 0x0121
KEY_A, KEY_B = 0x0104, 0x0105
WIN_R = 0x5815

# 下标 = 键位 id = 配置器里的 K1..K11 = 板面丝印 CN1..CN11
#      上排  CN9=9  CN10=0  CN11=Backspace  [旋钮]
#      中排  CN5=5  CN6=6   CN7=7           CN8=8
#      下排  CN1=1  CN2=2   CN3=3           CN4=4
ONE_, TWO_, THREE_, FOUR_ = 0x011E, 0x011F, 0x0120, 0x0121
FIVE_, SIX_, SEVEN_, EIGHT_ = 0x0122, 0x0123, 0x0124, 0x0125
NINE_, ZERO_, BSPC = 0x0126, 0x0127, 0x012A

KEYMAP = [ONE_, TWO_, THREE_, FOUR_, FIVE_, SIX_, SEVEN_, EIGHT_, NINE_, ZERO_, BSPC]
LABEL = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 'Backspace']

# 给 2026-11-11 18:20 之前那版固件（矩阵 bit→键序还是错的蛇形表）用的补偿表。
# 顺序是"把 CN 序按旧固件的错位重新排一遍"，写完物理键位就对了。
# 新固件（CFG_VERSION=2，bit_to_key 已修正）不要用这个，用上面的 KEYMAP。
LEGACY_KEYMAP = [KEY_A, KEY_B, WIN_R, ONE, TWO, THREE, FOUR, UP, SIX, SEVEN, EIGHT]


def open_device():
    try:
        import hid
    except ImportError:
        print('[!] 缺少 hidapi：pip install hidapi')
        sys.exit(1)

    for d in hid.enumerate(VID, PID):
        if d.get('usage_page') == VENDOR_USAGE_PAGE:
            dev = hid.device()
            dev.open_path(d['path'])
            dev.set_nonblocking(1)
            return dev
    print('[!] 没找到配置通道（usage_page=0xFF00）。键盘插好了吗？')
    sys.exit(1)


def xfer(dev, payload):
    """一次 Feature 往返：payload 不含报告 ID，返回 (status, data)"""
    buf = bytearray(PLEN + 1)
    buf[0] = REPORT_ID
    buf[1:1 + len(payload[:PLEN])] = payload[:PLEN]
    if dev.send_feature_report(bytes(buf)) < 0:
        raise RuntimeError('send_feature_report 失败')
    time.sleep(0.05)                      # 给固件一点时间处理
    r = dev.get_feature_report(REPORT_ID, PLEN + 1)
    if not r:
        raise RuntimeError('get_feature_report 返回空')
    if r[0] not in (REPORT_ID, 0):
        raise RuntimeError('报告 ID 不符: %02X' % r[0])
    body = r[1:] if r[0] == REPORT_ID else r
    return body[1], body[2:]


def read_keymap(dev):
    st, data = xfer(dev, [CMD_GET_KEYMAP])
    if st != 0:
        raise RuntimeError('GET_KEYMAP 状态 %d' % st)
    return [data[i * 2] | (data[i * 2 + 1] << 8) for i in range(11)]


def show(codes, title):
    print(title)
    for i, c in enumerate(codes):
        print('  K%-2d  CN%-2d  0x%04X  %s' % (i + 1, i + 1, c, LABEL[i] if codes == KEYMAP else ''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--read', action='store_true', help='只读，不修改')
    ap.add_argument('--factory', action='store_true', help='恢复出厂默认并落盘')
    ap.add_argument('--key', nargs=2, metavar=('IDX', 'CODE'), help='只改一个键，如 --key 10 0x5815')
    ap.add_argument('--old-fw', action='store_true',
                    help='写"补偿表"，供 2026-11-11 18:20 之前那版（接线映射错位的）固件使用')
    args = ap.parse_args()

    dev = open_device()
    try:
        st, info = xfer(dev, [CMD_INFO])
        print('[i] 已连接：协议 v%d 固件 %d.%d  矩阵 %dx%d 键位 %d 灯珠 %d' %
              (info[0], info[1], info[2], info[3], info[4], info[5], info[6]))

        if args.read:
            codes = read_keymap(dev)
            for i, c in enumerate(codes):
                print('  K%-2d CN%-2d 0x%04X' % (i + 1, i + 1, c))
            return 0

        target = LEGACY_KEYMAP if args.old_fw else KEYMAP
        if args.factory:
            st, _ = xfer(dev, [CMD_FACTORY])
            print('[i] 已请求恢复出厂，状态 %d' % st)
        elif args.key:
            idx, code = int(args.key[0]), int(args.key[1], 0)
            st, _ = xfer(dev, [CMD_SET_KEY, idx, code & 0xFF, code >> 8])
            if st != 0:
                print('[!] SET_KEY 失败，状态 %d' % st)
                return 1
            print('[i] K%d 已设为 0x%04X' % (idx + 1, code))
        else:
            payload = [CMD_SET_ALL]
            for c in target:
                payload += [c & 0xFF, c >> 8]
            st, _ = xfer(dev, payload)
            if st != 0:
                print('[!] SET_ALL 失败，状态 %d' % st)
                return 1
            print('[i] 11 个键位已下发（%s）' % ('旧固件补偿表' if args.old_fw else 'CN 序标准表'))

        # 落盘（固件在主循环里真正写 Flash，需要一点时间）
        xfer(dev, [CMD_SAVE])
        time.sleep(0.6)

        # 强制从 Flash 重读，验证真的写进去了
        xfer(dev, [CMD_RELOAD])
        back = read_keymap(dev)
        print('[i] 回读（已经从 DataFlash 重新加载）:')
        for i, c in enumerate(back):
            print('  K%-2d CN%-2d 0x%04X' % (i + 1, i + 1, c))

        want = None if (args.key or args.factory) else target
        if want and back != want:
            print('[!] 回读与目标不一致，写入可能没落盘')
            return 1
        print('[✓] 完成')
        return 0
    finally:
        try:
            dev.close()
        except Exception:
            pass


if __name__ == '__main__':
    sys.exit(main())
