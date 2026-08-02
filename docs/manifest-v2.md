# Manifest v2

> 最后更新：2026-08-01。`enabled` 现在同时控制当前 Runtime 会话和下次启动配置；标准
> `SkinnedMeshRenderer` 替换支持热开关，不再默认要求重启游戏。

runtime 扫描：

```text
gakumas-local/local-files/mods/<mod-id>/mod.json
```

最小示例：

```json
{
  "schemaVersion": 2,
  "id": "example-body-mod",
  "name": "Example Body Mod",
  "version": "0.1.0",
  "author": "author",
  "priority": 0,
  "enabled": true,
  "replacements": [
    {
      "source": "mdl_chr_example-cstm-0000_body",
      "part": "body",
      "priority": 0,
      "bundle": "example-body.bundle",
      "asset": "Assets/Mods/example/mdl_chr_example-cstm-0000_body.prefab",
      "type": "GameObject",
      "renderers": [
        {
          "rendererId": "body",
          "targetRenderer": "Geo_Body",
          "modRenderer": "Geo_Body"
        }
      ],
      "replaceMaterials": false,
      "textures": [
        {
          "rendererName": "Geo_Body",
          "materialSlot": 0,
          "property": "_BaseMap",
          "asset": "Assets/Mods/example/body_t0.png",
          "type": "Texture2D"
        }
      ]
    }
  ]
}
```

## 字段

| 字段 | 说明 |
|---|---|
| `enabled` | 是否在当前会话注册该 Mod，并持久化为下次启动状态 |
| `priority` | 低层 replacement 兼容字段；管理器不会用它在同一服装/发型中自动挑选赢家 |
| `source` | 游戏原始资源名；兼容旧别名 `from`、`target` |
| `part` | `face`、`hair` 或 `body` |
| `bundle` | 相对当前 Mod 目录的 AssetBundle 路径 |
| `asset` | bundle 内资源路径 |
| `type` | 当前主路径为 `GameObject` |
| `renderers` | 原 renderer 与 Mod renderer 的明确配对 |
| `replaceMaterials` | 是否直接替换材质；通常保持 `false` 并使用贴图规则 |
| `textures` | renderer、材质槽、shader property 与 Texture2D 资源的映射 |
| `skeleton` | 可选。bundle 内骨架 sidecar（`TextAsset`）的资源路径；声明后 runtime 按 sidecar 建真实骨链，不再走旧的按名 remap 分支。兼容旧别名 `skeletonAsset` |

## 骨架 sidecar

`skeleton` 指向的 `TextAsset` 是一份 JSON。**它有三个硬性字段，缺任一或对不上，
该 replacement 直接失败**（`ModRuntime.cpp` 的 `runtimeProtocol is required` /
`buildId is required` / `bones array is required`）：

| 字段 | 要求 |
|---|---|
| `runtimeProtocol` | 整数，**必须等于 `1`**。对不上即判定为导出器与 runtime 版本不匹配 |
| `buildId` | 非空字符串，用于把 bundle 与日志对上号 |
| `bones` | 数组。每项必须有 `name`（字符串）与 `localPosition` / `localRotation` / `localScale`；`parentIndex` 可选（默认 `-1` 表示根）。可选 `swing` 对象带 `damping` / `stiffness` / `spring` / `mass` / `rootWeight` / `pendulum` / `useWindGlobalForce` 与 `collider`（`radius` / `type` / `collisionMask`） |

可选的 `extraSwingBones` / `newBones` 数组用同样的 transform 字段，但用 `parentName`
而不是 `parentIndex` 挂接。

> **摆动链要带链尾 tip 骨。**这不是 runtime 的校验项，是数据完整性要求：游戏的
> `UpdateChainInfo` 本就排除每条链的最后一根骨，sidecar 少写 tip 就等于少一节摆动。
> 无权重的 tip 骨不会出现在 `m_Bones` 里，导出器要显式补。

`mod.json` 顶层也会带一份同值的 `runtimeProtocol` / `buildId`（由导出器写入，便于离线
校验工具比对），但 runtime 只强制校验 sidecar 里的那份。

GakumasMI 插件导出时自动生成 sidecar；**手写 manifest 时这三个字段最容易漏**。

## 部位约定

| part | renderer |
|---|---|
| `body` | 只允许单个 `Geo_Body` |
| `hair` | 单个 `Geo_Hair`，或 `Geo_Hair` + `Geo_HairProp` |
| `face` | 尚未固化公开 renderer 约定，必须按 source profile 单独验证 |

一个 bundle 可以包含多个部位，但每个 prefab/FBX 只对应一个部位；各部位使用独立 replacement。
runtime 可以重排已存在骨骼的 skinning 数据，但不会自动修权重、创建 Animator 驱动或转换材质。

> 管理器第一版的产品约束更窄：一个 Mod Manifest 只显示一个逻辑目标（一个 `body` 服装
> 或一个 `hair` 发型）。Runtime 仍按兼容性保留 `replacements[]` 数组并逐项解析；如果
> 管理器发现多条 replacement，必须将该 Mod 标记为配置异常，不在玩家界面展开成多件服装。

## 逻辑目标与冲突

Runtime 通过标准化后的 `part + source/masterKey` 建立玩家能理解的逻辑目标：

- `body` → 一件服装；
- `hair` → 一个发型；
- 同一逻辑目标只允许一个 Mod 启用。

冲突不再依赖 `priority` 静默选择：

- 启动扫描发现同目标多个 Manifest 都为 `enabled=true`，整组全部自动写回 `false`，并在
  Runtime 快照保存冲突对象和“已自动关闭”状态；
- 当前会话已有同目标 Mod 启用时，另一个 Mod 的开启请求被拒绝，现有 Mod 保持 ON，
  新 Mod 保持 OFF；
- 玩家先关闭现有 Mod 后，才能开启同目标的另一个 Mod。

## 当前会话热开关

API 切换成功时先更新当前会话有效 replacement map，再原子写回 Manifest。标准原地
`SkinnedMeshRenderer` 规则会：

- 首次应用时保存原 Mesh、材质、骨骼、根骨和每材质 `MaterialPropertyBlock`；
- OFF 时恢复当前场景实例与缓存 Prefab；
- ON 时对目标资源子树重应用并刷新活动 Animation Rig；
- 立即把 Mod 贴图合并到 Renderer 已有的 PropertyBlock，保留游戏自己的其他属性。
- Runtime 保留 `Renderer.set_sharedMaterials` / `set_materials` 的诊断与恢复路径；游戏当前
  实际使用的底层材质数组写回仍可能让主页颜色在热 ON 后暂时错误，切换页面会重新走完整
  替换路径并恢复。IDA 后的底层 icall 实验钩子已因崩溃撤回。

整对象替换和附加式规则暂不保证即时逆转；它们可能需要重新选择资源、重进场景或重启。
热开关改变的是 Manifest 状态与 Runtime 会话，不会把 AssetBundle 改成启动时全部预加载；
资源仍按请求懒加载。

## 管理器兼容约束

为获得完整游戏内 UI，一个 Manifest 应满足：

- `id` 唯一且稳定；
- 只有一条逻辑 replacement；
- `part` 为 `body` 或 `hair`；
- `source` 能归一化到 Costume 或 CostumeHead Master；
- Bundle 文件存在且路径不逃逸 Mod 目录；
- 玩家名称写在 `name`，不要把资源 ID 当作显示名称。

不满足这些约束的 Manifest 仍可被目录扫描并报告错误，但管理器不会把它伪装成正常服装或发型。
