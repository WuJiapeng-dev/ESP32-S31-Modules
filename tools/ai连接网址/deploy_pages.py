#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""一条命令把控制台页面重新发布到 GitHub Pages。

做的事：
  1. 调用 build_demo.py，从仓库根目录 控制台.html 重新生成 site/index.html
  2. 在临时目录检出 gh-pages 分支，用 site/ 的内容整体替换
  3. 提交并推送；GitHub Pages 会自动重建（通常 30~60 秒）

用法（在仓库任意位置）：
    python tools/ai连接网址/deploy_pages.py

为什么不走 GitHub Actions：当前 git 凭据是 OAuth App，没有 workflow 权限，
无法推送 .github/workflows/ 下的文件。所以改为直接推 gh-pages 分支 —— Pages
的来源就是该分支，效果一样，且不需要任何密钥。
"""

import io
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
SITE = os.path.join(HERE, 'site')
BRANCH = 'gh-pages'


def git(args, cwd=REPO, check=True):
    r = subprocess.run(['git'] + args, cwd=cwd, capture_output=True, text=True)
    if check and r.returncode != 0:
        sys.stderr.write((r.stdout or '') + (r.stderr or ''))
        raise SystemExit('git %s 失败' % ' '.join(args))
    return r


def main():
    if not os.path.isdir(SITE):
        raise SystemExit('缺少 site 目录，先检查 build_demo.py')

    # 1. 重新构建
    print('[1/4] 重新生成页面 ...')
    r = subprocess.run([sys.executable, os.path.join(HERE, 'build_demo.py')])
    if r.returncode != 0:
        raise SystemExit('构建失败，未发布')

    # 2. 检出 gh-pages 到临时目录
    print('[2/4] 准备 gh-pages 分支 ...')
    tmp = tempfile.mkdtemp(prefix='pages-')
    have = git(['rev-parse', '--verify', '--quiet', 'origin/' + BRANCH], check=False)
    try:
        if have.returncode == 0:
            git(['worktree', 'add', '--detach', tmp, 'origin/' + BRANCH])
        else:
            git(['worktree', 'add', '--detach', tmp, 'HEAD'])
            git(['checkout', '--orphan', BRANCH], cwd=tmp)
            git(['rm', '-rf', '.'], cwd=tmp, check=False)

        # 3. 用 site/ 内容替换
        print('[3/4] 同步站点文件 ...')
        for name in os.listdir(tmp):
            if name == '.git':
                continue
            p = os.path.join(tmp, name)
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
        for name in os.listdir(SITE):
            src = os.path.join(SITE, name)
            dst = os.path.join(tmp, name)
            shutil.copytree(src, dst) if os.path.isdir(src) else shutil.copy2(src, dst)
        # GitHub Pages 默认会跑 Jekyll，加这个文件跳过
        io.open(os.path.join(tmp, '.nojekyll'), 'w', encoding='utf-8').close()

        git(['add', '-A'], cwd=tmp)
        diff = git(['status', '--porcelain'], cwd=tmp, check=False).stdout.strip()
        if not diff:
            print('内容没有变化，无需发布。')
            return 0
        git(['-c', 'user.name=WuJiapeng-dev',
             '-c', 'user.email=WuJiapeng-dev@users.noreply.github.com',
             'commit', '-q', '-m', 'deploy(网页): 更新控制台页面'], cwd=tmp)

        print('[4/4] 推送 ...')
        git(['-c', 'http.sslVerify=false', 'push', 'origin', 'HEAD:refs/heads/' + BRANCH], cwd=tmp)
        print('\n已推送。Pages 重建约 30~60 秒后生效：')
        print('  https://wujiapeng-dev.github.io/ESP32-S31-Modules/')
        return 0
    finally:
        git(['worktree', 'remove', '--force', tmp], check=False)
        if os.path.isdir(tmp):
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
