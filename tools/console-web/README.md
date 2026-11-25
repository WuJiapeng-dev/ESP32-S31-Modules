# 网页控制台（公网演示版）

把 ESP32-S31 语音合成控制台的**前端页面**发布成公网可访问的网站：单一网址，
浏览器打开即用，不需要任何本地文件或服务。

- 线上入口（免费、固定）：**https://wujiapeng-dev.github.io/ESP32-S31-Modules/**
- 备用入口（平台临时域名，带分享码）：`https://voice-console-26973.app.workbuddy.host/?sharecode=F7FSENPNAQ`
- 自定义域名（免费子域名，申请中）：`yueyueniao.is-a.dev` → PR [is-a-dev/register#62810](https://github.com/is-a-dev/register/pull/62810)
- 付费域名：`yueyueniao.com`（未注册；`.com` 没有免费途径）

## 自定义域名（零成本）

`is-a.dev` 免费提供 `*.is-a.dev` 子域名，用 GitHub PR 申请，适合本项目：

1. 申请文件 `domains/yueyueniao.json`（已在 fork 中提交，PR 待合并）：
   ```json
   {
     "owner": { "username": "WuJiapeng-dev", "email": "WuJiapeng-dev@users.noreply.github.com" },
     "records": { "CNAME": "wujiapeng-dev.github.io" }
   }
   ```
2. **PR 合并后**，一条命令完成绑定并等证书：
   ```bash
   python tools/console-web/bind_domain.py            # 绑定 + 等待 HTTPS
   python tools/console-web/bind_domain.py --check    # 只看状态
   ```
3. 绑定后 `github.io` 地址会重定向到新域名，属正常现象。

`bind_domain.py` 会在 DNS 未生效时**拒绝**绑定（否则会把现在能用的 github.io 地址重定向到打不开的域名），并且不落盘、不打印凭据。


## 结构

```
tools/console-web/
├── build_demo.py      # 从仓库根目录 控制台.html 生成 site/index.html
├── deploy_pages.py    # 构建 + 推 gh-pages，GitHub Pages 自动重建
├── site/              # 发布目录（只放可公开的静态文件）
│   └── index.html     # 由 build_demo.py 生成，勿手改
└── README.md
```

发布目录与构建脚本刻意分开：托管平台会把整个目录原样上传，脚本不该出现在公网。

## 生成与发布

```bash
# 一条命令：重新生成 + 推到 gh-pages 分支（Pages 来源即该分支）
python tools/console-web/deploy_pages.py
```

只改页面不发布时，单跑 `python tools/console-web/build_demo.py` 即可（输出 `site/index.html`）。

**为什么不走 GitHub Actions**：仓库当前 git 凭据是 OAuth App，没有 `workflow` 权限，
推不了 `.github/workflows/` 下的文件；改用直接推 `gh-pages` 分支，效果相同且零密钥。
若要换成 Actions，在 GitHub 网页端 Settings → Pages 把 Source 改成 `GitHub Actions`，
再自行推送一个 `deploy-pages` 工作流即可。

`build_demo.py` 对源页面的改动只有两类，功能逻辑一行未动：

1. **打开示例数据层**：只拦截文档保留地址 `192.0.2.x`，不会碰任何真实设备地址。
2. **消除混合内容冲突**：公网是 https，浏览器会拦截对 `http://192.168.x.x` 的请求。
   因此地址解析在 https 下默认补 `https://`，输入框与说明文案只引导 https 地址，
   并删掉了 `http://192.168.4.1` 这类示例。脚本末尾有兜底断言，残留相关片段会直接报错。

## 数据隔离

页面没有后端、没有登录、没有共享存储：设备列表与各页配置全部写入**当前浏览器的
localStorage**（键 `ksk.console.v1`）。不同电脑、不同浏览器、同一浏览器的隐私窗口
各自独立，互相看不到对方的数据；清除站点数据即恢复初始状态。

## 已知限制

- 公网 https 页面无法访问内网 http 板子（浏览器混合内容策略），所以本站在线时用的是
  示例数据。要控制真实板子，请直接打开板子自己发出的那个页面（同一局域网内）。
- 示例数据只对 `192.0.2.10` / `192.0.2.11` 生效，添加真实地址会走真实网络请求。
