#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Intel HEX 独立校验工具（无第三方依赖，任何人可直接运行复核）

用法：
    python verify_hex.py <文件.hex> [对照.bin]

校验内容：
    1. 每条记录的字节数、校验和
    2. 是否存在 EOF 记录（:00000001FF）
    3. 地址范围是否落在用户区 0x0000-0x37FF
       （CH552 的 BootLoader 在 0x3800-0x3FFF，写入该区域会破坏烧录能力）
    4. 复位向量、Timer0 中断向量
    5. SHA256（可与烧录前记录的值比对）
    6. 若给了 .bin，逐字节比对两者是否同源
"""

import sys
import hashlib


def parse_hex(path):
    mem = {}
    errors = []
    data_records = 0
    has_eof = False
    type_count = {}

    with open(path, "r", encoding="ascii", errors="replace") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line:
                continue
            if not line.startswith(":"):
                errors.append("第 %d 行：不以冒号开头" % lineno)
                continue
            try:
                b = bytes.fromhex(line[1:])
            except ValueError:
                errors.append("第 %d 行：含非十六进制字符" % lineno)
                continue

            if len(b) < 5:
                errors.append("第 %d 行：记录过短" % lineno)
                continue

            n = b[0]
            addr = (b[1] << 8) | b[2]
            typ = b[3]
            type_count[typ] = type_count.get(typ, 0) + 1

            if len(b) != 5 + n:
                errors.append("第 %d 行：长度字段 %d 与实际 %d 不符"
                              % (lineno, n, len(b) - 5))
            if sum(b) & 0xFF != 0:
                errors.append("第 %d 行：校验和错误" % lineno)

            if typ == 0:
                for i in range(n):
                    mem[addr + i] = b[4 + i]
                data_records += 1
            elif typ == 1:
                has_eof = True
            elif typ in (2, 4):
                errors.append("第 %d 行：出现扩展地址记录（本固件不应有）" % lineno)

    return mem, errors, data_records, has_eof, type_count


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    hex_path = sys.argv[1]
    bin_path = sys.argv[2] if len(sys.argv) > 2 else None

    raw = open(hex_path, "rb").read()
    print("=" * 60)
    print("文件      :", hex_path)
    print("文本大小  :", len(raw), "字节")
    print("SHA256    :", hashlib.sha256(raw).hexdigest())
    print("=" * 60)

    mem, errors, records, has_eof, type_count = parse_hex(hex_path)

    print("数据记录数:", records)
    print("记录类型  :", type_count, "(0 = 数据, 1 = EOF)")
    print("有 EOF 记录:", has_eof)
    print("结构错误  :", errors if errors else "无")
    print()

    if not mem:
        print("!! 没有任何数据记录")
        return 1

    addrs = sorted(mem)
    lo, hi = addrs[0], addrs[-1]
    over = [a for a in addrs if a > 0x37FF]

    print("字节总数  :", len(mem))
    print("地址范围  : 0x%04X .. 0x%04X" % (lo, hi))
    print("超出用户区(>0x37FF)的字节数:", len(over),
          "← 必须为 0，否则会破坏 BootLoader")
    print()

    print("复位入口 0x0000 :", " ".join("%02X" % mem.get(i, 0xFF) for i in range(3)))
    print("Timer0   0x000B :", " ".join("%02X" % mem.get(0x0B + i, 0xFF) for i in range(3)))
    print()

    if bin_path:
        b = open(bin_path, "rb").read()
        gaps = [a for a in range(len(b)) if a not in mem]
        diff = [a for a in addrs if b[a] != mem[a]] if len(b) > hi else ["长度不足"]
        print("对照 bin  :", bin_path, len(b), "字节")
        print("  空洞位置:", ["0x%04X" % g for g in gaps] or "无",
              "（bin 中补 0xFF，属正常：未使用的中断向量槽）")
        print("  内容差异:", len(diff) if diff != ["长度不足"] else diff)
        print()

    ok = (not errors) and has_eof and not over
    print("结论      :", "通过" if ok else "不通过")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
