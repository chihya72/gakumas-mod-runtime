# Gakumas Mod Runtime

这是学园偶像大师本地 AssetBundle Mod 的独立游戏运行时。它通过 `xinput1_3.dll` 代理入口加载，
只负责扫描本地 Mod、拦截游戏资源加载并替换 Mesh、骨架映射和贴图。

`DllMain` 只关闭线程回调；首次 XInput 或 Runtime API 调用会在 loader lock 之外启动初始化线程。
由于运行时安装了进程级 Hook 并拥有后台探针线程，启动后会固定 DLL 到进程结束，不支持中途
`FreeLibrary` 热卸载。

当前发布版本：`1.0.0`。版本变化见 [CHANGELOG.md](CHANGELOG.md)。

本仓库不保存游戏提取资产、成品 Mod、旧测试 AB 包或生成报告。

## 当前能力

- 扫描 `gakumas-mod/mods/<mod-id>/mod.json`；
- 支持 manifest v2 的 `priority`、`part`、`renderers` 和贴图规则；
- 支持 `body=Geo_Body`；
- 支持 `hair=Geo_Hair` 或 `Geo_Hair+Geo_HairProp`；
- 从本地 AssetBundle 懒加载替换资源；
- 启动时注册全部有效替换候选，并通过 Runtime API 在当前会话热切换有效规则；
- 标准 `SkinnedMeshRenderer` 替换保存原 Mesh、材质和骨骼绑定；OFF 只恢复当前快照中仍存活的
  场景实例，不跨帧持有场景对象指针；ON 对当前目标资源子树重新应用，目标尚未创建时进入
  按 `modId + source` 去重的延迟队列；
- 整对象替换与附加式规则暂不承诺即时逆转，仍按后续资源加载状态处理；
- 同一服装/发型目标只允许一个启用 Mod；运行中拒绝冲突开启，启动时若同组多项开启则自动持久化关闭整组；
- 在原始 `SkinnedMeshRenderer` 上替换克隆后的 Mesh；
- 按骨骼名重排 skinning 数据，失败时保留原始 Mesh；
- 换空间时同时搬运顶点、法线和切线；
- 按 renderer、材质槽和 shader property 替换贴图：资源加载路径写进私有材质克隆，
  活体热路径直接写游戏自己的 per-actor 材质并快照原贴图，OFF 时写回；
- 热 OFF 释放并销毁本次热 ON 克隆的 Mesh，多轮开关不涨内存；
- 保留 `set_sharedMaterials` / `set_materials` / `MaterialPropertyBlock` 的兼容性路径；Renderer
  材质生命周期 Hook 同时用于唤醒延迟热重应用，但不是 2026-08-09 颜色问题的成因；
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

2026-08-09 实机确认：**热 ON 之后直接返回主页颜色即正确**，不再需要进入换装页面刷新。
同一会话连续 12 轮 ON/OFF，每轮的贴图还原与网格还原都完整，内存不再随开关增长。

2026-08-12 实机确认：在 Mod 管理页切换时场景中可能暂时没有目标 Renderer；这时 ON 会先写
`hotInstances=0` 并排队，而不是丢掉请求。`hmsz-fuyuko-icu` 返回主页时由新角色/Renderer 的
生命周期完成替换并清队列；日志中的 `applied=0 alreadyPatched=1` 表示资源加载路径已经先打好
补丁，延迟路径只确认结果，没有重复应用。`atbm-cstm-0140` 的这条“零活体后再开启”分支尚未
单独复验，不能借用 `hmsz` 的证据代替。

> **已证伪：**「热 ON 后颜色错误是游戏事后写回材质数组，换掉带 Mod 贴图的私有材质」。
> 抓帧对比推翻了它：暗色帧和正常帧的身体 draw 在 Mesh、8 张贴图、材质常量、shader 变体
> 和渲染状态上逐字节相同。真因是
> `TransformModMeshVerticesToOriginalRendererSpace` 只把顶点搬到目标 renderer 空间，
> 法线和切线留在原地——冷路径两端都是 prefab 单位阵所以看不出来，活体角色有真实旋转就
> 会让法线与几何脱节。完整推导与其余已证伪结论见
> [`docs/lessons-learned.md`](docs/lessons-learned.md)。

手抄的部署版大小与哈希在这里连续过期过两次，已经删掉：release 的哈希由
`publish-release.ps1` 写进 release notes，本机部署版自己算：

```powershell
Get-FileHash <游戏目录>\xinput1_3.dll -Algorithm SHA256
```

## 构建

要求 Visual Studio 2022 与 C++ 桌面开发组件。

```powershell
.\tools\package.ps1 -Version dev
```

产物：

```text
build/bin/x64/Release/xinput1_3.dll
```

打包脚本直接调用仓库内固定的 Premake 5.0.0-beta1，并通过 `vswhere` 定位 MSBuild；构建、
离线测试或打包任一步失败都会中止。第三方组件和许可证见
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

当前 Release 构建会强制运行 `mod_presentation_tests` 与 `mod_runtime_catalog_tests`；另有 Python
`unittest discover` 源码契约测试。
延迟热重应用状态及多代原 Mesh 身份刷新已有原生/源码契约回归。完整验收还应核对
`xinput1_3.def` 的 9 个导出（8 个 XInput 代理 + `GmrGetRuntimeApiV1`），并在目标游戏复验热 ON/OFF/ON：
直接回主页确认 Mesh、材质、骨骼和颜色都正确，多轮开关后内存不增长。新增摇物骨或链仍需
重新进入场景，不能把活体热开关当作 prefab graft 的替代。
