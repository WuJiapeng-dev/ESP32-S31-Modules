#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从仓库根目录的 控制台.html 生成公网演示版页面（tools/ai连接网址/index.html）。

改动只涉及两点，其余功能原样保留：
  1. 打开「示例数据」层（只拦截文档保留地址 192.0.2.x，不碰任何真实地址）
  2. 与 https 冲突的地址处理与文案：默认补 https、只接受 https 设备、去掉
     http://192.168.x.x 这类会在 https 页面上被混合内容策略拦截的示例

用法（仓库根目录）：
    python tools/ai连接网址/build_demo.py
"""
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.join(ROOT, '控制台.html')
DST = os.path.join(HERE, 'site', 'index.html')

REPLACEMENTS = [
    # 标题
    ('<title>语音合成控制台</title>',
     '<title>语音合成控制台 · 演示</title>'),

    # 示例数据层常开（仅 192.0.2.x 生效）
    ("  var demo = (p === 'file:' || h === 'localhost' || h === '127.0.0.1');",
     "  var demo = true;   /* 演示版构建：始终启用示例数据（只拦截 192.0.2.x） */"),

    # 任意托管环境都播种示例设备
    ("""    var preview = (location.protocol === 'file:' ||
      location.hostname === 'localhost' || location.hostname === '127.0.0.1');""",
     "    var preview = true;"),

    # 不写协议时，https 页面补 https，避免自己去请求 http 被拦
    ("  if (!/^https?:\\/\\//i.test(u)) { u = 'http://' + u; }",
     "  if (!/^https?:\\/\\//i.test(u)) {\n"
     "    u = (location.protocol === 'https:' ? 'https://' : 'http://') + u;  /* https 页面默认补 https */\n"
     "  }"),

    # 地址输入框提示
    ('placeholder="192.168.4.1 或 http://192.168.1.50"',
     'placeholder="https://设备地址"'),

    ('<p class="hint">只填 IP、带端口、带协议、粘贴整条链接都行；不写协议时按 http 处理。</p>',
     '<p class="hint">本页是 https 站点，只能添加 https 设备地址；内网 http 地址会被浏览器的混合内容策略拦截。</p>'),

    # 说明卡片标题与导语
    ("""      <h2>跨设备是怎么连上的</h2>
      <p class="desc">固件没有开放跨域头，所以跨设备不能直连。本页用两种通道：</p>""",
     """      <h2>连接方式与数据位置</h2>
      <p class="desc">公网 https 站点能访问的设备必须也是 https 或与本页同源；页面的数据完全留在这个浏览器里。</p>"""),

    ("""          <b>同源</b>：直接打开设备自己的页面（例如 <code>http://192.168.4.1</code>），页面与设备同源，直连读写。<br>""",
     """          <b>同源</b>：直接打开设备自己的页面（<code>https://…</code>），页面与设备同源，直连读写。<br>"""),

    ("""          <b>https 页面访问 http 设备会被浏览器拦</b>（混合内容），这种情况请用本地文件或 http 部署打开。""",
     """          <b>本页是 https</b>，浏览器会拦截对 <code>http://192.168.x.x</code> 的请求（混合内容），所以这里只接受 https 设备地址；要控制内网板子，请直接打开板子自己发出的那个页面。"""),

    # 第二张说明：改为数据位置（隔离性）
    ("""      <details class="tip">
        <summary>能用几台</summary>
        <div class="bd">
          每台中继设备会占用一个后台 iframe 和一条常驻连接，建议不超过 <b>6 台</b>。设备与手机／电脑要在同一局域网，跨网段、跨 VLAN 一般不通。
        </div>
      </details>""",
     """      <details class="tip">
        <summary>数据存在哪里</summary>
        <div class="bd">
          设备列表和各页配置只写入<b>当前浏览器的 localStorage</b>，不上传服务器，也不会与其他电脑或浏览器共享；换一台设备打开就是一份全新的默认数据。清除该站点的浏览数据即可恢复初始状态。
        </div>
      </details>"""),

    # 删掉「为什么不用 https」卡片（本页本身就是 https 站点）
    ("""      <details class="tip">
        <summary>为什么不用 https</summary>
        <div class="bd">
          https 页面会被浏览器<b>禁止</b>访问 <code>http://192.168.x.x</code>（混合内容），所以本页必须用 <b>http</b> 打开才能控制板子；板子固件本身也只提供 http，没有证书。<br>
          想让别的电脑用固定网址打开：在局域网内任意一台机器上进到本文件所在目录，执行 <code>python -m http.server 8080</code>，别人的浏览器打开 <code>http://那台机器IP:8080/</code> 即可；不在同一网络则连不到板子。
        </div>
      </details>
""", ''),
]

DEMO_BAR_CSS = """.demo-bar{max-width:820px;margin:10px auto 0;padding:9px 12px;border-radius:10px;
  background:#fff8ec;color:#8a4b06;border:1px solid #f7e3c3;font-size:12px;line-height:1.65}
.demo-bar b{font-weight:700}
.demo-bar code{background:#fff;border-color:#f0dcc0;color:#8a4b06}
.ft{max-width:820px;"""

DEMO_BAR = """
<div class="demo-bar">
  <b>在线演示</b> · 页面里的设备是示例数据，可以随便点，改不坏；清除站点数据即可重置。
  <br>公网 https 页面无法访问内网 <code>http://192.168.x.x</code> 设备（浏览器混合内容策略），要控制真实板子请直接打开板子自己发出的页面。
</div>
"""


def main():
    if not os.path.exists(SRC):
        sys.exit('找不到源文件: %s' % SRC)
    with io.open(SRC, encoding='utf-8') as f:
        s = f.read()

    for old, new in REPLACEMENTS:
        if old not in s:
            sys.exit('锚点缺失，源页面已改动，请更新 build_demo.py：\n%s' % old[:80])
        s = s.replace(old, new, 1)

    if '.ft{max-width:820px;' not in s:
        sys.exit('锚点缺失：CSS 插入点')
    s = s.replace('.ft{max-width:820px;', DEMO_BAR_CSS, 1)

    if '</header>' not in s:
        sys.exit('锚点缺失：</header>')
    s = s.replace('</header>', '</header>\n' + DEMO_BAR, 1)

    # 兜底：演示版不应残留任何 http 设备示例地址
    for bad in ('http://192.168.4.1', 'http://192.168.1.50', "u = 'http://' + u"):
        if bad in s:
            sys.exit('残留了会触发混合内容的片段: %s' % bad)

    with io.open(DST, 'w', encoding='utf-8', newline='\n') as f:
        f.write(s)
    print('已生成 %s（%d 字节）' % (DST, len(s.encode('utf-8'))))


if __name__ == '__main__':
    main()
