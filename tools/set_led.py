#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
灯效配置写入（不需要重烧固件）
==============================

走 Feature 0x10 通道：CMD_SET_LED(0x08) 一次写全 8 项参数 + 11 颗灯珠颜色，
然后 CMD_SAVE(0x09) 落盘，最后 CMD_RELOAD(0x21) 强制从 DataFlash 重读校验。

用法：
    python set_led.py                       # 应用出厂默认：呼吸 / 30% / #F77634 全填
    python set_led.py --read                # 只读当前灯效
    python set_led.py --mode 1 --bright 30 --color F77634 --all
    python set_led.py --mode 2 --speed 80   # 只改动指定项，其余保持不变

模式：0 常亮  1 呼吸  2 彩虹  3 按压缩放  4 关灯  5 彩虹呼吸  6 单点扫描  7 星光
亮度/速度：0..100（亮度 100 档 = 物理亮度的 70%，固件 LED_FULL）

依赖：pip install hidapi
"""

import argparse
import sys

import set_keymap as K            # 复用同一套协议封装

CMD_GET_LED = 0x07
CMD_SET_LED = 0x08
CMD_SAVE = 0x09
CMD_RELOAD = 0x21

NL = 11
DEFAULT = dict(mode=1, bright=30, speed=50, rgb=(0xF7, 0x76, 0x34))


def read_led(dev):
    st, d = K.xfer(dev, [CMD_GET_LED])
    if st != 0:
        raise RuntimeError('GET_LED 状态 %d' % st)
    return {
        'mode': d[0], 'bright': d[1], 'speed': d[2],
        'rgb': (d[3], d[4], d[5]),
        'per_key': [tuple(d[6 + i * 3: 6 + i * 3 + 3]) for i in range(NL)],
    }


def hexcolor(rgb):
    return '#%02X%02X%02X' % tuple(rgb)


def show(c, title):
    print(title)
    print('  模式=%d 亮度=%d%% 速度=%d 主色=%s' % (c['mode'], c['bright'], c['speed'], hexcolor(c['rgb'])))
    same = all(tuple(p) == tuple(c['rgb']) for p in c['per_key'])
    if same:
        print('  11 颗灯珠颜色：全部 = 主色')
    else:
        print('  11 颗灯珠颜色：' + ' '.join(hexcolor(p) for p in c['per_key']))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--read', action='store_true')
    ap.add_argument('--mode', type=int)
    ap.add_argument('--bright', type=int)
    ap.add_argument('--speed', type=int)
    ap.add_argument('--color', help='RRGGBB 或 #RRGGBB')
    ap.add_argument('--all', action='store_true', help='把主色填到全部 11 颗灯珠')
    args = ap.parse_args()

    dev = K.open_device()
    try:
        cur = read_led(dev)
        if args.read:
            show(cur, '[i] 当前灯效：')
            return 0

        # 没给参数 = 应用出厂默认
        use_default = not any([args.mode is not None, args.bright is not None,
                               args.speed is not None, args.color, args.all])
        new = dict(cur)
        if use_default:
            new['mode'] = DEFAULT['mode']
            new['bright'] = DEFAULT['bright']
            new['speed'] = DEFAULT['speed']
            new['rgb'] = DEFAULT['rgb']
            new['per_key'] = [DEFAULT['rgb']] * NL
        else:
            if args.mode is not None:   new['mode'] = max(0, min(7, args.mode))
            if args.bright is not None: new['bright'] = max(0, min(100, args.bright))
            if args.speed is not None:  new['speed'] = max(0, min(100, args.speed))
            if args.color:
                s = args.color.strip().lstrip('#')
                if len(s) != 6:
                    print('[!] 颜色要 6 位十六进制，例如 F77634')
                    return 1
                new['rgb'] = tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))
            if args.all:
                new['per_key'] = [new['rgb']] * NL

        p = [CMD_SET_LED, new['mode'], new['bright'], new['speed'],
             new['rgb'][0], new['rgb'][1], new['rgb'][2]]
        for c in new['per_key']:
            p += list(c)
        st, _ = K.xfer(dev, p)
        if st != 0:
            print('[!] SET_LED 状态 %d' % st)
            return 1

        K.xfer(dev, [CMD_SAVE])
        import time
        time.sleep(0.7)
        K.xfer(dev, [CMD_RELOAD])
        show(read_led(dev), '[✓] 已写入并落盘，回读：')
        return 0
    finally:
        try:
            dev.close()
        except Exception:
            pass


if __name__ == '__main__':
    sys.exit(main())
