# 发布流程

产物是一个 zip，解压到游戏根目录即可：

```text
xinput1_3.dll
gakumas-mod\config.json
README.txt
LICENSE
third-party-notices.md
licenses\
```

`.dll` **不在 Gitea 附件白名单里**（只允许 `.zip` / `.gz` / `.tgz` 等），所以发布物
必须是压缩包，不能直接传裸 DLL。

## 手动发布

不依赖 CI，任何时候都能跑：

```powershell
.\tools\package.ps1 -Version v1.0.0
```

它会依次运行仓库内 Premake、MSBuild Release x64、离线测试，然后在 `dist\` 下生成
`gakumas-mod-runtime-1.0.0.zip`，附带项目和第三方许可证，并打印 zip 和 DLL 的 SHA-256。任一步失败就中断，
不会产出半成品包。

已经编译过、只想重新打包：

```powershell
.\tools\package.ps1 -Version v1.0.0 -SkipBuild
```

拿到 zip 后网页上传，或者用脚本创建 release：

```powershell
$env:GITEA_TOKEN = "<个人访问令牌，需要 write:repository>"
.\tools\publish-release.ps1 -Tag v1.0.0 `
    -Zip dist\gakumas-mod-runtime-1.0.0.zip `
    -ApiBase https://git.chinosk6.cn/api/v1 `
    -Repository chihya72/gakumas-mod-runtime
```

## GitHub Actions 自动发布

`.github/workflows/release.yml` 使用 GitHub 托管的 `windows-latest` runner：推送到 `main`
或手动运行时只构建并上传 artifact；推送 `v*` 标签时构建、上传 artifact 并创建 GitHub Release。
手动运行用于验证流水线，不创建 Release。

```powershell
git push github main
git tag v1.0.0
git push github v1.0.0
```

## Gitea 自动发布

`.gitea/workflows/release.yml`：推 `v*` 标签触发，构建 → 打包 → 建 release →
传 zip。`workflow_dispatch` 手动触发时只构建并上传 artifact，**不**创建 release，
方便在不浪费标签的情况下验证流水线。

```powershell
git tag v1.0.0
git push origin v1.0.0
```

### 前提：一台 Windows runner

这是唯一的门槛。本项目用 premake + MSBuild 编 x64 原生 DLL，**Linux runner 跑不了
这个 job**，容器也不行。需要在一台装了 VS2022（或 Build Tools，勾选「使用 C++ 的桌面
开发」）的 Windows 机器上注册 `act_runner`：

1. 从 <https://gitea.com/gitea/act_runner/releases> 下载 Windows 版；
2. 在仓库 `设置 → Actions → Runners` 取注册令牌；
3. 注册时**标签必须包含 `windows`**，workflow 的 `runs-on: windows` 靠它匹配：

```powershell
.\act_runner.exe register --no-interactive `
    --instance https://git.chinosk6.cn `
    --token <注册令牌> `
    --name win-build `
    --labels windows:host
```

`:host` 后缀表示直接在宿主机上跑而不是容器——Windows 上必须这样，job 才能用到本机的
MSBuild。注册完 `.\act_runner.exe daemon` 常驻。

没有 runner 时 workflow 不会报错，只会一直排队，所以先去 Runners 页面确认它在线。

### 关于 `actions/checkout`

Gitea 默认从 github.com 拉取 action。如果实例访问不了 GitHub，`actions/checkout@v4`
会失败；此时可以在实例配置里把 `[actions] DEFAULT_ACTIONS_URL` 指向可访问的镜像，
或者把 checkout 换成手写的 `git clone`。手动发布这条路不受影响。

## 版本号

`GKMS_VERSION` 在 premake **生成阶段**读取（不是编译阶段），`package.ps1` 会在运行
`tools\premake5.exe vs2022` 之前设好。它被编进 DLL，启动时写进日志：

```text
[BOOT] GakumasMod: [ModAsset] gakumas-mod-runtime v1.0.0 loaded. logLevel=error ...
```

本地不设就是 `dev`。用户报问题时让他们贴这一行，比对 DLL 哈希省事得多。
