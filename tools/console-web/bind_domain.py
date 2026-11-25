#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""is-a.dev PR 合并后，把 GitHub Pages 的自定义域名绑上（一步到位）。

前置条件：`is-a-dev/register` 的 PR 已被合并，DNS 已生效
（`nslookup yueyueniao.is-a.dev` 应指向 wujiapeng-dev.github.io 的同级地址）。

用法：
    python tools/console-web/bind_domain.py            # 绑定并等待 HTTPS
    python tools/console-web/bind_domain.py --check     # 只看状态，不改动

安全说明：
- 凭据不落盘、不打印，运行期从 git 凭据助手临时取用。
- 失败不会破坏现状：绑定失败时 Pages 仍按原样服务 github.io 地址。
"""

import json
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

OWNER = 'WuJiapeng-dev'
REPO = 'ESP32-S31-Modules'
DOMAIN = 'yueyueniao.is-a.dev'
API = 'https://api.github.com/repos/%s/%s' % (OWNER, REPO)
SITE = 'https://%s/' % DOMAIN


def token():
    """从 git 凭据助手取 token，不打印、不写文件。"""
    p = subprocess.run(
        ['git', 'credential', 'fill'],
        input='protocol=https\nhost=github.com\n\n',
        capture_output=True, text=True,
    )
    m = re.search(r'^password=(.+)$', p.stdout, re.M)
    if not m:
        sys.exit('没取到 GitHub 凭据，先在能 push 的终端里运行一次 git fetch')
    return m.group(1).strip()


def call(method, url, tok, payload=None):
    data = json.dumps(payload).encode('utf-8') if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header('Authorization', 'token ' + tok)
    req.add_header('Accept', 'application/vnd.github+json')
    if data:
        req.add_header('Content-Type', 'application/json')
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read().decode('utf-8') or '{}')
    except urllib.error.HTTPError as e:
        body = e.read().decode('utf-8', 'replace')
        try:
            return e.code, json.loads(body)
        except ValueError:
            return e.code, {'message': body}


def dns_ready():
    """is-a.dev 有泛解析，随便一个名字都会解析到 Cloudflare，
    所以不能只看“能不能解析”，要看 CNAME 是否指向 github.io，
    或者干脆看这个域名是不是已经在返回我们的页面。"""
    try:
        req = urllib.request.Request(
            'https://dns.google/resolve?name=%s&type=CNAME' % DOMAIN,
            headers={'Accept': 'application/dns-json'})
        with urllib.request.urlopen(req, timeout=20) as r:
            data = json.loads(r.read().decode('utf-8'))
    except Exception:
        data = {}

    for ans in data.get('Answer', []) or []:
        if 'github.io' in str(ans.get('data', '')):
            return True, 'CNAME 已指向 GitHub Pages'

    if html_is_ours():
        return True, '域名已在返回本站页面'
    return False, 'CNAME 还没指过来（PR 可能尚未合并）'


def html_is_ours():
    try:
        req = urllib.request.Request(SITE)
        with urllib.request.urlopen(req, timeout=20) as r:
            body = r.read(400000).decode('utf-8', 'replace')
        return 'ksk.console.v1' in body or '语音合成控制台' in body
    except Exception:
        return False


def site_up():
    try:
        req = urllib.request.Request(SITE, method='HEAD')
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status == 200
    except Exception:
        return False


def main():
    check_only = '--check' in sys.argv
    tok = token()

    status, pages = call('GET', API + '/pages', tok)
    if status != 200:
        sys.exit('读 Pages 配置失败：%s' % pages.get('message'))
    print('当前 Pages：%s ｜ cname=%s ｜ https_enforced=%s'
          % (pages.get('status'), pages.get('cname'), pages.get('https_enforced')))

    ok_dns, why = dns_ready()
    print('DNS：%s' % why)

    if check_only:
        print('站点 %s 可访问：%s' % (SITE, site_up()))
        return 0

    if not ok_dns:
        print('\nDNS 未就绪，先不改设置（否则 github.io 地址会被重定向到打不开的域名）。')
        print('等 is-a.dev 的 PR 合并、DNS 生效后再跑一次本脚本即可。')
        return 2

    if pages.get('cname') != DOMAIN:
        print('\n绑定自定义域名 %s ...' % DOMAIN)
        st, res = call('PUT', API + '/pages', tok,
                       {'cname': DOMAIN, 'source': {'branch': 'gh-pages', 'path': '/'}})
        if st not in (200, 204):
            sys.exit('绑定失败：%s' % res.get('message'))
        print('已提交绑定。')
    else:
        print('\n域名已绑定，等待证书。')

    print('等待 HTTPS 生效（最多 10 分钟）...')
    for i in range(40):
        if site_up():
            print('完成：%s 已可访问' % SITE)
            return 0
        time.sleep(15)
        print('  ... 第 %d 次检查' % (i + 1))
    print('还没生效。GitHub 签证书通常需要 15~60 分钟，稍后用 --check 再看。')
    return 1


if __name__ == '__main__':
    sys.exit(main())
