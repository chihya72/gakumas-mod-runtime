# Gakumas Mod Runtime

这是学园偶像大师本地 AssetBundle Mod 的独立游戏运行时。它通过 `xinput1_3.dll` 代理入口加载，
只负责扫描本地 Mod、拦截游戏资源加载并替换 Mesh、骨架映射和贴图。

本仓库不保存游戏提取资产、成品 Mod、旧测试 AB 包或生成报告。

## 当前能力

- 扫描 `gakumas-mod/mods/<mod-id>/mod.json`；
- 支持 manifest v2 的 `priority`、`part`、`renderers` 和贴图规则；
- 支持 `body=Geo_Body`；
- 支持 `hair=Geo_Hair` 或 `Geo_Hair+Geo_HairProp`；
- 从本地 AssetBundle 懒加载替换资源；
- 启动时注册全部有效替换候选，并通过 Runtime API 在当前会话热切换有效规则；
- 标准 `SkinnedMeshRenderer` 替换保存原 Mesh、材质和骨骼绑定；OFF 恢复当前实例与缓存 Prefab，ON 对对应资源子树重新应用；
- 整对象替换与附加式规则暂不承诺即时逆转，仍按后续资源加载状态处理；
- 同一服装/发型目标只允许一个启用 Mod；运行中拒绝冲突开启，启动时若同组多项开启则自动持久化关闭整组；
- 在原始 `SkinnedMeshRenderer` 上替换克隆后的 Mesh；
- 按骨骼名重排 skinning 数据，失败时保留原始 Mesh；
- 按 renderer、材质槽和 shader property 替换贴图；
- 为贴图覆盖创建私有材质；保留 `set_sharedMaterials` / `set_materials` 诊断与恢复路径，
  同时保留当前 `MaterialPropertyBlock` 参数；
- 写入 `gakumas-mod/mod-plugin.log`；
- 输出 source profile，并提供离线 Validator 与 Author Doctor。

manifest 格式见 [docs/manifest-v2.md](docs/manifest-v2.md)，当前限制和后续任务见
[docs/roadmap.md](docs/roadmap.md)，做错过什么见
[docs/lessons-learned.md](docs/lessons-learned.md)。

## 当前验证状态

2026-08-01 实机已经确认：

- 管理器通过 `GmrGetRuntimeApiV1` 读取快照和切换 Mod；
- Manifest 写回和当前会话有效 replacement map 同步更新；
- 标准服装替换可以热关闭、再热开启；
- 热重应用日志记录目标、应用数和活动 Animation Rig 刷新。

当前已恢复到 IDA MCP 调查前的已验证基线：标准服装 OFF/ON 热切换不崩溃，但热 ON 后
直接返回主页仍可能颜色错误；切换一次游戏页面后恢复正常。

> **已证伪：**「Renderer 已有的每材质 `MaterialPropertyBlock` 旧贴图覆盖克隆材质」这一
> 结论是错的。实机探针确认游戏在这些场景**从不调用 `Renderer.SetPropertyBlock`**，
> 基于该结论的几轮修复改的是一条从未执行的路径。

2026-08-02 探针确认的真实写入者是：游戏在热重应用之后调用材质数组写回，换掉带 Mod
贴图的私有材质。IDA 后加入的底层 `SetMaterialArray_Injected` 实验钩子及其后续受限扫描
连续造成加载卡住或点击崩溃，现已从源码和部署版移除。完整的排除过程、证据和下一步见
[`manager/docs/OPEN_DEFECTS.md`](manager/docs/OPEN_DEFECTS.md)。

手抄的部署版大小与哈希在这里连续过期过两次，已经删掉：release 的哈希由
`publish-release.ps1` 写进 release notes，本机部署版自己算：

```powershell
Get-FileHash <游戏目录>\xinput1_3.dll -Algorithm SHA256
```

## 构建

要求 Visual Studio 2022 与 C++ 桌面开发组件。

```bat
generate.bat
msbuild build\gakumas_mod_runtime.sln /p:Configuration=Release /p:Platform=x64
```

产物：

```text
build/bin/x64/Release/xinput1_3.dll
```

`generate.bat` 使用仓库内固定的 Premake 5.0.0-beta1。第三方组件和许可证见
[third-party-notices.md](third-party-notices.md)。

## 安装

从 [Releases](https://git.chinosk6.cn/chihya72/gakumas-mod-runtime/releases) 下载 zip，
解压到游戏根目录（`gakumas.exe` 所在处）。也可以自己构建，见
[`docs/release.md`](docs/release.md)。

每个 Mod 放在：

```text
gakumas-mod/mods/<mod-id>/
  mod.json
  your-mod.bundle
```

## gakumas-mod/config.json

两个键，都可省略：

```json
{
  "modManagerUi": true,
  "logLevel": "error"
}
```

- `modManagerUi`：游戏内 Mod 管理界面开关，默认开。只有明确写 `false` 才关闭
  （Mod 替换照常工作）；文件不存在、JSON 写错或键名打错都按开启处理。
- `logLevel`：`"info"` / `"warn"` / `"error"`，默认 `"error"`。默认只记录错误，
  排查问题时改成 `"info"` 拿完整 trace。`mod-plugin.log` 和 `mod-manager.log`
  共用这一个等级。

无论等级如何，启动都会写一行 `[BOOT]` 记录当前生效的等级——所以日志文件永远存在，
"没有日志"只可能意味着插件没被加载。

## 作者诊断

```bat
python tools\gakumas_mod_validator.py <mod-dir> --profile <source-profile.json>
python tools\gakumas_mod_doctor.py <mod-dir> --profile <source-profile.json>
```

默认报告会写入 `<mod-dir>\reports`。工具只检查 manifest、文件、renderer、材质槽和贴图声明，
目前不离线解析 AssetBundle 内部对象。

## 验证

```bat
python -m unittest discover -s tests -v
```

当前 Release 构建已通过，Python `unittest discover` 的 4 项测试通过。完整验收还应核对
`xinput1_3.def` 的 9 个导出（8 个 XInput 代理 + `GmrGetRuntimeApiV1`），并在目标游戏复验热 ON/OFF/ON 后无需切换页面即可得到正确
Mesh、材质、骨骼和颜色。旧的 3DMigoto 暗色调查与本次热开关 MPB 提交缺陷是两个独立问题，
见 [AB_DARK_RENDERING_INVESTIGATION.md](AB_DARK_RENDERING_INVESTIGATION.md)。
