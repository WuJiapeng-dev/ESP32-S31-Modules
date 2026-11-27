#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""把根目录的 控制台.html 转成固件里的 C 字符串头文件。

用法：
    python tools/gen_page.py

产出：
    音色/main/page_html.h    （wifi_prov.c 里 #include 它）

要点：
- 逐字符转义，不会把 \\" 这类转义从中间切断（按行切断时最容易踩这个坑）
- 断掉 C 三字符组（??= ??/ 等）：'?' 后面再跟 '?' 时写成 \\?
- 生成后回读文件、反解义、与源文件逐字节比对，不一致直接报错
"""

import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..'))
SRC = os.path.join(REPO, '控制台.html')
DST = os.path.join(REPO, '音色', 'main', 'page_html.h')

LINE_MAX = 96          # 每行字符串的字节上限（留足余量给编译器）


def escape_char(ch):
    """把一个源字符转成 C 字符串里的写法（不含首尾引号）。"""
    if ch == '\\':
        return '\\\\'
    if ch == '"':
        return '\\"'
    if ch == '\n':
        return '\\n'
    if ch == '\r':
        return ''
    if ch == '\t':
        return '\\t'
    return ch


def escape(text):
    out = []
    prev_q = False
    for ch in text:
        if ch == '\r':
            prev_q = False
            continue
        if ch == '?' and prev_q:
            out.append('\\?')       # 断掉三字符组
        else:
            out.append(escape_char(ch))
        prev_q = (ch == '?')
    return ''.join(out)


def chunk(escaped):
    """按「完整转义单元」切行，避免把 \\" 切成两半。"""
    units = re.findall(r'\\u[0-9a-fA-F]{4}|\\.|.', escaped, re.S)
    lines, cur, size = [], [], 0
    for u in units:
        n = len(u.encode('utf-8'))
        if size + n > LINE_MAX and cur:
            lines.append(''.join(cur))
            cur, size = [], 0
        cur.append(u)
        size += n
    if cur:
        lines.append(''.join(cur))
    return lines


def c_unescape(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            out.append({'n': '\n', 't': '\t', '"': '"', '\\': '\\', '?': '?'}.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def main():
    if not os.path.exists(SRC):
        sys.exit('找不到源文件：%s' % SRC)

    html = io.open(SRC, encoding='utf-8').read()
    if '\r' in html:
        html = html.replace('\r', '')

    body = escape(html)
    lines = chunk(body)

    buf = []
    buf.append('/* 由 tools/gen_page.py 从 控制台.html 生成，请勿手改。')
    buf.append('   改界面：改 控制台.html → 重跑脚本 → 编译烧录。 */')
    buf.append('#ifndef PAGE_HTML_H')
    buf.append('#define PAGE_HTML_H')
    buf.append('')
    buf.append('static const char PAGE_HTML[] =')
    for l in lines:
        buf.append('    "%s"' % l)
    buf.append('    ;')
    buf.append('')
    buf.append('#endif /* PAGE_HTML_H */')
    buf.append('')

    out = '\n'.join(buf)
    io.open(DST, 'w', encoding='utf-8', newline='\n').write(out)

    # ---- 回读自检：把文件里的字符串拼回来、反解义，与源文件比对 ----
    back = io.open(DST, encoding='utf-8').read()
    literals = re.findall(r'^\s*"(.*)"\s*$', back, re.M)
    restored = c_unescape(''.join(literals))

    if restored != html:
        a, b = len(restored), len(html)
        print('自检失败：还原 %d 字节，源文件 %d 字节' % (a, b))
        for i in range(min(a, b)):
            if restored[i] != html[i]:
                print('第一处差异在第 %d 字符' % i)
                print('还原:', repr(restored[max(0, i - 30):i + 30]))
                print('源文:', repr(html[max(0, i - 30):i + 30]))
                break
        sys.exit(1)

    print('已生成 %s' % DST)
    print('源 %d 字节 → %d 行字符串，自检通过（逐字节一致）'
          % (len(html.encode('utf-8')), len(lines)))


if __name__ == '__main__':
    main()
