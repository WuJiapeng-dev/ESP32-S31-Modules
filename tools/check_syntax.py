#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""不启动完整构建，只对 main/ 下的 .c 做语法级编译检查。

为什么需要它：
  idf.py / cmake 在部分机器上会被安全软件拦在进程创建（CreateProcess failed），
  但交叉编译器本身是能跑的。这个脚本直接取 build 目录里 CMake 已经生成好的
  编译参数（compile_commands.json），把源文件换成 -fsyntax-only 跑一遍，
  能查出语法错、类型错、-Werror 下的告警——不产出 bin，也不用 ninja。

用法：
  python tools/check_syntax.py                 # 自动找 build 目录
  python tools/check_syntax.py --build build_j # 指定构建目录
  python tools/check_syntax.py main/ds_chat.c  # 只查某个文件

注意：
  中文、空格路径会让 -specs 的路径解析出错，脚本会把工程镜像到
  C:\\kk_syntax（纯英文）再编译，结束后自动清理。
"""

import argparse
import io
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                      # 仓库根
# 固件工程目录：默认 音色/，可用 --proj 指定（交付包里是 固件/）
PROJ = os.path.join(ROOT, '音色')
TMP = r'C:\kk_syntax'                             # 纯英文镜像目录


def resolve_proj(name):
    """按 --proj 找固件工程目录，支持相对/绝对路径。"""
    for cand in (name, os.path.join(ROOT, name), os.path.join(HERE, name)):
        p = os.path.abspath(cand)
        if os.path.isdir(os.path.join(p, 'main')):
            return p
    return None


def tokenize(s):
    """按 shell 规则分词，保留引号内的空格，并还原 \\" 转义。

    CMake 生成的命令里 -D 参数形如 -DMBEDTLS_CONFIG_FILE=\\"mbedtls/esp_config.h\\"，
    必须还原成带真实引号的一个参数，否则预处理器会报
    '#include' expects "FILENAME"。
    """
    out, cur, q = [], [], False
    i, n = 0, len(s)
    while i < n:
        ch = s[i]
        if ch == '\\' and i + 1 < n and s[i + 1] == '"':
            cur.append('"')
            i += 2
            continue
        if ch == '"':
            q = not q
            i += 1
            continue
        if ch in ' \t' and not q:
            if cur:
                out.append(''.join(cur))
                cur = []
            i += 1
            continue
        cur.append(ch)
        i += 1
    if cur:
        out.append(''.join(cur))
    return out


def pick_build(want=None):
    if want:
        p = want if os.path.isabs(want) else os.path.join(PROJ, want)
        return p if os.path.isfile(os.path.join(p, 'compile_commands.json')) else None
    cands = []
    for name in os.listdir(PROJ):
        if name.startswith('build'):
            cc = os.path.join(PROJ, name, 'compile_commands.json')
            if os.path.isfile(cc):
                cands.append((os.path.getmtime(cc), os.path.join(PROJ, name)))
    cands.sort(reverse=True)
    return cands[0][1] if cands else None


def mirror(build):
    """把 main/、build/config、build/specs、build/toolchain 拷到英文路径。

    为什么要整个镜像：IDF 6.x 会用 @<build>/toolchain/cflags 响应文件传公共参数，
    而响应文件里的路径一旦含中文（例如 音色/）就会写成坏字节，导致
    -specs 指向一个不存在的文件、编译器退回去用 newlib 的头。
    """
    if os.path.isdir(TMP):
        shutil.rmtree(TMP, ignore_errors=True)
    os.makedirs(TMP)
    shutil.copytree(os.path.join(PROJ, 'main'), os.path.join(TMP, 'main'),
                    ignore=shutil.ignore_patterns('*.o', '*.obj'))
    for sub in ('config', 'specs', 'toolchain'):
        src = os.path.join(build, sub)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(TMP, sub))


def expand_response_file(ref):
    """读 @xxx 响应文件，返回其中的参数；specs 一律改指镜像里的那份。"""
    if not os.path.isfile(ref):
        return []
    raw = io.open(ref, 'rb').read().decode('latin-1')
    out, seen = [], set()
    for t in tokenize(raw):
        if 'specs=' in t:
            t = '-specs=' + os.path.join(TMP, 'specs', 'picolibc.specs')
        if t in seen:
            continue
        seen.add(t)
        out.append(t)
    return out


def base_args(build, sample):
    """从 compile_commands 里取一条命令，去掉 -o/-c/源文件，路径改指镜像目录。"""
    data = json.load(io.open(os.path.join(build, 'compile_commands.json'), encoding='utf-8'))
    ent = [e for e in data if e['file'].replace('\\', '/').endswith(sample)]
    if not ent:
        ent = data
    ent = ent[0]
    toks = tokenize(ent.get('command') or ' '.join(ent['arguments']))

    proj_fwd = PROJ.replace('\\', '/')
    build_fwd = build.replace('\\', '/')
    tmp_fwd = TMP.replace('\\', '/')

    args, resp, i = [], [], 0
    while i < len(toks):
        t = toks[i]
        if t == '-o':
            i += 2
            continue
        if t == '-c':
            i += 1
            continue
        if t.replace('\\', '/').endswith(sample):
            i += 1
            continue
        t = t.replace(build_fwd, tmp_fwd)
        t = t.replace(proj_fwd, tmp_fwd)
        if t.startswith('@'):
            resp.append(t[1:])
            i += 1
            continue
        args.append(t)
        i += 1
    return args, resp


def main():
    global PROJ
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='*', help='要检查的 .c（默认 main/ 下全部）')
    ap.add_argument('--build', help='构建目录名，如 build')
    ap.add_argument('--proj', default='音色', help='固件工程目录，默认 音色')
    a = ap.parse_args()

    p = resolve_proj(a.proj)
    if not p:
        print('找不到固件工程目录（需要包含 main/ 子目录）：%s' % a.proj)
        return 2
    PROJ = p

    build = pick_build(a.build)
    if not build:
        print('找不到带 compile_commands.json 的构建目录，先跑一次 idf.py reconfigure')
        return 2
    print('构建目录:', build)

    files = a.files or sorted(
        os.path.join('main', f) for f in os.listdir(os.path.join(PROJ, 'main'))
        if f.endswith('.c'))
    sample = os.path.basename(files[0])

    mirror(build)
    args, resp = base_args(build, sample)

    # 把 @响应文件摊平到参数里；里面的路径已改指镜像目录
    extra = []
    for r in resp:
        extra += expand_response_file(r)
    if extra:
        args = args[:1] + extra + args[1:]
        print('已展开响应文件 %d 个，补入 %d 个参数' % (len(resp), len(extra)))

    # 兜底：万一响应文件里没有 specs，也要显式补，否则会用成 newlib 的头
    specs = os.path.join(TMP, 'specs', 'picolibc.specs')
    if os.path.isfile(specs) and not any('specs=' in a for a in args):
        args.insert(1, '-specs=' + specs)
        print('已补 -specs（picolibc）')
    print('编译参数 %d 个，源文件 %d 个\n' % (len(args), len(files)))

    bad = 0
    for rel in files:
        src = os.path.join(TMP, rel.replace('\\', '/').replace('main/', 'main/', 1))
        if not os.path.isfile(src):
            src = os.path.join(TMP, 'main', os.path.basename(rel))
        r = subprocess.run(args + ['-fsyntax-only', src],
                           capture_output=True, encoding='utf-8', errors='replace')
        out = ((r.stdout or '') + (r.stderr or '')).strip()
        # 把镜像路径换回原名，报错位置看得懂
        out = out.replace(TMP, PROJ)

        if r.returncode == 0 and not out:
            print('[ OK ] %s' % rel)
        elif r.returncode == 0:
            print('[WARN] %s' % rel)
            print(out[:4000])
        else:
            bad += 1
            print('[FAIL] %s' % rel)
            print(out[:4000])
        print()

    shutil.rmtree(TMP, ignore_errors=True)
    print('---- 失败 %d / %d ----' % (bad, len(files)))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
