# -*- coding: utf-8 -*-
r"""从 wifi_prov.c 里抽出 PAGE_HTML 常量，导出成可离线打开的预览页。

用法（在模块根目录执行）：
    python tools\gen_preview.py
生成两份：
  1) 模块内 preview_console.html —— 本机预览用，被 .gitignore 排除，不提交
  2) 上级（仓库根）控制台.html —— 对外展示的那一份，随仓库提交

网页这种展示物料一律放仓库根目录，只有代码才分模块。
用浏览器直接打开即可看界面（数据是假的，不连板子）。

关键点：C 字符串里的转义只处理 \" \\ \n \t \r 与 \xNN，
纯 ASCII 以外的字符一律原样保留（源码本身就是 UTF-8），
绝不能用 unicode_escape 走一遍 latin-1，否则中文会变成乱码。
"""
import io
import os
import re

SRC = os.path.join('main', 'wifi_prov.c')
OUT = 'preview_console.html'          # 本机预览（.gitignore 排除）
OUT_REPO = os.path.join('..', '控制台.html')   # 仓库根目录，随仓库提交

src = io.open(SRC, encoding='utf-8').read()

# 数组由连续多行字符串字面量隐式拼接而成，中间夹了空行用于分组，
# 所以边界判断要「跳过空行，遇到第一个非空且不以引号开头的行才停」。
lines = src.splitlines()
begin = next(i for i, l in enumerate(lines) if 'PAGE_HTML[]' in l)
finish = len(lines)
for i in range(begin + 1, len(lines)):
    stripped = lines[i].strip()
    if stripped == '':
        continue
    if not stripped.startswith('"'):
        finish = i
        break
block = '\n'.join(lines[begin:finish])


def unescape_c(s):
    out = []
    i = 0
    n = len(s)
    while i < n:
        ch = s[i]
        if ch == '\\' and i + 1 < n:
            nxt = s[i + 1]
            if nxt == 'n':
                out.append('\n'); i += 2; continue
            if nxt == 't':
                out.append('\t'); i += 2; continue
            if nxt == 'r':
                out.append('\r'); i += 2; continue
            if nxt == '"':
                out.append('"'); i += 2; continue
            if nxt == "'":
                out.append("'"); i += 2; continue
            if nxt == '\\':
                out.append('\\'); i += 2; continue
            if nxt == 'x' and i + 3 < n:
                try:
                    out.append(chr(int(s[i + 2:i + 4], 16)))
                    i += 4
                    continue
                except ValueError:
                    pass
            if nxt == 'u' and i + 5 < n:
                try:
                    out.append(chr(int(s[i + 2:i + 6], 16)))
                    i += 6
                    continue
                except ValueError:
                    pass
        out.append(ch)
        i += 1
    return ''.join(out)


parts = []
for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', block):
    parts.append(unescape_c(m.group(1)))

html = ''.join(parts)

# 加一段离线 mock：静态打开时伪造 /api/state，让界面能渲染出数据
mock = """
<script>
/* offline preview only: fake the device API so the UI renders without the board */
(function () {
  var _fetch = window.fetch;
  window.fetch = function (url, opt) {
    var s = String(url);
    if (s.indexOf('/api/state') >= 0) {
      return Promise.resolve({
        json: function () {
          return Promise.resolve({
            wifi: 1, ip: '192.168.1.86', ssid: 'MyHome', ble: 0, ready: 1,
            voice: 'stella', has_key: 1, busy: 0
          });
        }
      });
    }
    if (s.indexOf('/api/') >= 0) {
      return Promise.resolve({ json: function () { return Promise.resolve({ ok: 1 }); } });
    }
    return _fetch.apply(this, arguments);
  };
  document.addEventListener('DOMContentLoaded', function () {
    setTimeout(function () { if (typeof poll === 'function') { poll(); } }, 60);
  });
})();
</script>
"""

if '</body>' in html:
    html = html.replace('</body>', mock + '</body>')
else:
    html += mock

for path in (OUT, OUT_REPO):
    with io.open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(html)

print('片段数 %d' % len(parts))
print('输出字节 %d' % len(html.encode('utf-8')))
print('写入:', OUT)
print('写入:', os.path.abspath(OUT_REPO))
print('标题片段:', html[html.index('<title>'):html.index('</title>') + 8])
garbled = [w for w in ('锟', 'ï¿½', '鍢', '闊宠') if w in html]
print('乱码特征:', garbled if garbled else '无')
