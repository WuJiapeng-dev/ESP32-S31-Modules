# AI 连接网址

把 ESP32-S31 语音控制台发布成**一个网址**：浏览器打开即用，不需要任何本地文件、不需要装软件、不需要服务器。

- 线上地址（免费、固定）：**https://wujiapeng-dev.github.io/ESP32-S31-Modules/**
- 备用地址（平台临时域名）：`https://voice-console-26973.app.workbuddy.host/?sharecode=F7FSENPNAQ`
- 自定义域名（免费子域名，申请中）：`yueyueniao.is-a.dev` → PR [is-a-dev/register#62810](https://github.com/is-a-dev/register/pull/62810)

## 结构

```
tools/ai连接网址/
├── build_demo.py            # 从仓库根 控制台.html 生成公网版 site/index.html
├── deploy_pages.py          # 构建 + 推 gh-pages，GitHub Pages 自动重建
├── bind_domain.py           # is-a.dev 合并后绑定自定义域名并等 HTTPS
├── domains/
│   └── yueyueniao.json      # is-a.dev 域名申请文件（已提交 PR）
├── site/
│   └── index.html           # 发布产物，由 build_demo.py 生成，勿手改
└── README.md
```

发布目录与脚本刻意分开：托管平台只看到 `site/`，构建脚本不暴露到公网。

## 常用命令

```bash
# 改完根目录 控制台.html 后，一条命令发布上线
python tools/ai连接网址/deploy_pages.py

# 只重新生成页面、不发布
python tools/ai连接网址/build_demo.py

# 域名 PR 合并后绑定（--check 只看状态）
python tools/ai连接网址/bind_domain.py
```

`deploy_pages.py` 是幂等的：内容没变化时直接跳过，不产生空提交。

## build_demo.py 改了什么

对源页面只做两类改动，**功能逻辑一行未动**：

1. **打开示例数据层**：只拦截文档保留地址 `192.0.2.x`，真实地址照常走网络。
2. **消除混合内容冲突**：公网是 https，浏览器会拦截对 `http://192.168.x.x` 的请求，因此地址解析在 https 下默认补 `https://`，示例与文案改成 https，并删掉 `http://192.168.4.1` 这类硬编码。脚本末尾有兜底断言，残留相关片段会直接报错。

## 自定义域名（零成本）

`is-a.dev` 免费提供 `*.is-a.dev` 子域名，用 GitHub PR 申请。申请文件 `domains/yueyueniao.json`：

```json
{
  "owner": { "username": "WuJiapeng-dev", "email": "WuJiapeng-dev@users.noreply.github.com" },
  "records": { "CNAME": "wujiapeng-dev.github.io" }
}
```

**PR 合并后**执行 `python tools/ai连接网址/bind_domain.py` 完成绑定并等证书；此前不要手动改 Pages 的自定义域名，否则 `github.io` 会被重定向到打不开的域名（脚本已内置该保护）。

`.com` 无免费途径（Freenom 免费域名已停服），要 `yueyueniao.com` 只能付费，首年通常 ¥30~80。

## 为什么不用 GitHub Actions

仓库当前 git 凭据是 OAuth App，没有 `workflow` 权限，推不了 `.github/workflows/` 下的文件；GitHub 连接器同样 403。所以改为直接推 `gh-pages` 分支——Pages 的来源就是该分支，效果一样，且不需要任何密钥。

## 数据隔离

没有后端、没有登录、没有共享存储：设备列表与各页配置全部写入**当前浏览器的 localStorage**（键 `ksk.console.v1`）。不同电脑、不同浏览器、隐私窗口各自独立，互相看不到对方的数据。

## 已知限制

- 公网 https 页面无法访问内网 http 板子（浏览器混合内容策略，无开关可绕）。所以线上跑的是示例数据；要控制真实板子，用板子自己发出的页面（`http://192.168.4.1`），或 `tools/public-link.bat` 的隧道。
- 示例数据只对 `192.0.2.10` / `192.0.2.11` 生效。
- `is-a.dev` 靠人工审 PR，合并时间由维护者决定。

详细过程与验证记录见 [报告.md](报告.md)。
