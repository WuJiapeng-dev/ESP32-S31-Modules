# -*- coding: utf-8 -*-
"""
生成 GB2312 -> Unicode 反查表（Unicode -> GB2312 双字节码）

为什么需要：SYN6288 只认 GB2312/GBK 编码的文本，而手机 BLE 发来的是 UTF-8。
ESP-IDF 的 newlib 未启用 iconv，没有现成的 UTF-8 -> GB2312 转换，所以把码表
编译进固件，运行时二分查找。

数据来源：Python 标准库 codecs 的 'gb2312' 编解码器（即 CP936 的 GB2312 子集），
与 SYN6288 手册的“GB2312”编码格式一致。

用法：
    python tools/gen_gb2312_table.py
输出：
    main/gb2312_table.c
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, 'main', 'gb2312_table.c')

pairs = {}
for b1 in range(0xA1, 0xF8):          # GB2312 区
    for b2 in range(0xA1, 0xFF):
        raw = bytes([b1, b2])
        try:
            ch = raw.decode('gb2312')
        except UnicodeDecodeError:
            continue
        if len(ch) != 1:
            continue
        cp = ord(ch)
        if cp > 0xFFFF:               # GB2312 全在 BMP
            continue
        pairs.setdefault(cp, (b1 << 8) | b2)

items = sorted(pairs.items())
print('GB2312 字符数: %d' % len(items))

lines = []
lines.append('/*')
lines.append(' * gb2312_table.c - 自动生成，请勿手工编辑')
lines.append(' *')
lines.append(' * 生成脚本: tools/gen_gb2312_table.py')
lines.append(' * 数据来源: Python codecs "gb2312" 编解码器（GB2312 字符集）')
lines.append(' * 编码方式: 每项 (unicode << 16) | gb2312_dualbyte，按 unicode 升序')
lines.append(' * 条目数  : %d' % len(items))
lines.append(' */')
lines.append('')
lines.append('#include <stdint.h>')
lines.append('#include <stddef.h>')
lines.append('')
lines.append('const uint32_t g_gb2312_map[] = {')
row = []
for cp, code in items:
    row.append('0x%08X' % ((cp << 16) | code))
    if len(row) == 8:
        lines.append('    ' + ', '.join(row) + ',')
        row = []
if row:
    lines.append('    ' + ', '.join(row) + ',')
lines.append('};')
lines.append('')
lines.append('const size_t g_gb2312_map_len = sizeof(g_gb2312_map) / sizeof(g_gb2312_map[0]);')
lines.append('')

with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(lines))

print('已写入: %s  (%d 字节)' % (OUT, os.path.getsize(OUT)))
