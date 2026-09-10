# Manifest v2

> 最后更新：2026-08-12。`enabled` 同时控制当前 Runtime 会话和下次启动配置；标准
> `SkinnedMeshRenderer` 替换支持热开关，不再默认要求重启游戏。

runtime 扫描：

```text
gakumas-mod/mods/<mod-id>/mod.json
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

这里的 `version` 是单个 Mod 包自己的版本号，不是 Runtime 插件版本；当前 Runtime
发布版本为 `1.0.0`。

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
| `renderers` | 原 renderer 与 Mod renderer 的明确配对。每项可带自己的 `skeleton`（该 renderer 的骨架 sidecar，1.2.1 起生效；发型 + 发饰同包时两份骨数不同，发饰必须用自己那份），不带则沿用顶层 `skeleton` |
| `replaceMaterials` | 是否直接替换材质；通常保持 `false` 并使用贴图规则 |
| `textures` | renderer、材质槽、shader property 与 Texture2D 资源的映射 |
| `skeleton` | 可选。bundle 内骨架 sidecar（`TextAsset`）的资源路径；声明后 runtime 按 sidecar 建真实骨链，不再走旧的按名 remap 分支。兼容旧别名 `skeletonAsset` |
| `transparentMaterials` | 可选。给渲染器追加原版没有的半透明材质段，见下一节 |

## 半透明材质段（`transparentMaterials`）

原版 body 渲染器只有 `m_bdy` / `m_bdyco` 两个材质槽。作者网格多切出来的半透明段（薄纱、罩裙）
没有原版槽位可对应，这个数组就是告诉 runtime「第 N 段是半透明段，贴图在哪、透明度多少」。
runtime 读到一条后：从 `gakumas-mod/gmi_shaders.bundle` 取 `Gmi/Transparent` 建一个新材质，
绑上三张贴图和参数，把 `sharedMaterials` 扩到 `materialSlot + 1` 并填进去。

缺 shader 包、缺 `asset` 贴图、材质建不出来，**整条拒绝并记 ERROR
（`Transparent materials refused`），不会静默回落成不透明**。`defMap` / `shadeMap` 缺失只 WARN。

```json
"transparentMaterials": [{
  "rendererName": "Geo_Body",
  "materialSlot": 2,
  "asset": "Assets/Mods/<id>/body_slot0_t0.png",
  "defMap": "Assets/Mods/<id>/body_slot0_t1.png",
  "shadeMap": "Assets/Mods/<id>/body_slot0_t4.png",
  "alpha": 0.5,
  "toonStrength": 1.0,
  "cull": 0.0, "zwrite": 0.0, "renderQueue": 3000,
  "props": { "_GmiBakedAfterDof": 1.0 }
}]
```

| 字段 | 说明 | 默认 |
|---|---|---|
| `rendererName` | 挂到哪个渲染器；省略时用所属 replacement 的 renderer | — |
| `materialSlot` | 追加的槽位，等于网格里那个 submesh 的序号（≥ 原版槽数）。必填 | — |
| `asset` | t0 基础色（bundle 内 Texture2D 路径）。必填；兼容别名 `texture`、`baseMap` | — |
| `defMap` / `shadeMap` | t1 / t4，卡通明暗用；兼容别名 `packedMask` / `shadeColor` | 空 |
| `type` | 贴图资源类型 | `Texture2D` |
| `alpha` | 整体不透明度 | `1.0` |
| `alphaFromTexture` | 1 = 再乘 t0.a；0 = 只用 `alpha` | `1.0` |
| `cull` | 0 关 / 1 剔前 / 2 剔后；薄纱是双面片，用 0 | `0.0` |
| `zwrite` | 颜色 pass 是否写深度 | `0.0` |
| `cutoff` | 低于此 alpha 的像素丢弃 | `0.004` |
| `toonStrength` `shadeDarken` `toonSoftness` `aoStrength` | 卡通明暗参数；没有 t1/t4 时 `toonStrength` 被强制为 0 | `1.0` `0.45` `0.08` `0.5` |
| `renderQueue` | 材质队列；-1 = 用 shader 自带 | `-1` |
| `props` | 任意 shader 浮点属性，原样 `SetFloat`。当前唯一需要的键见下 | 空 |

### `_GmiBakedAfterDof`（烘焙半透明）

`props` 里声明 `"_GmiBakedAfterDof": 1.0`，这块材质就走 **烘焙半透明** 路线（2026-09-07 实机定案）：

- runtime 自动把 `_ForwardEnable / _ActorTransparentEnable / _ZPrePassEnable / _DepthClaimEnable /
  _StencilWriteMask` 置 0、队列压到 3000 —— 材质不再进任何 SRP draw list；
- 每帧对渲染器 `BakeMesh`，在景深之后、bloom 之前用 `GmiBakedAfterDof` pass 显式补画主画面，
  遮挡用原生的编码深度（`_AdditionalInfoTexture`）；
- 游戏自己的平面镜（`PlanarReflectionUtility.RenderPlanarReflection`）画完后，用
  `GmiBakedReflection` pass 在镜面里补画一笔；
- 光照读角色灯表 `ShaderVariablesActorLighting`（按 `_ActorIndex` 掩码选灯）和场景全局的
  阴影色 / `_RampMap`，随场景灯光变化；
- 没有全局开关：有材质声明就画，没有就什么都不做。角色 rig 注册时立即登记渲染器，蒙皮数据
  自洽后约 3 帧开始画，与衣服其余部分基本同时出现。

不声明这个键就是旧的前向透明路径，已知在景深阶段没有深度附件、会穿透身体，**不要再用**。

已知边界：没有阴影贴图和环境高光；多层纱重叠处 alpha 会累积；镜面补绘不写原版 stencil，
镜子边缘可能溢出。缺 `defMap` / `shadeMap` 时只有平涂。


## 骨架 sidecar

`skeleton` 指向的 `TextAsset` 是一份 JSON。**它有三个硬性字段，缺任一或对不上，
该 replacement 直接失败**（`ModRuntime.cpp` 的 `runtimeProtocol is required` /
`buildId is required` / `bones array is required`）：

| 字段 | 要求 |
|---|---|
| `runtimeProtocol` | 整数，**必须等于 `1`**。对不上即判定为导出器与 runtime 版本不匹配 |
| `buildId` | 非空字符串，用于把 bundle 与日志对上号 |
| `bones` | 数组。每项必须有 `name`（字符串）与 `localPosition` / `localRotation` / `localScale`；`parentIndex` 可选（默认 `-1` 表示根）。可选 `swing` 对象，字段见下 |

可选的 `extraSwingBones` / `newBones` 数组用同样的 transform 字段，但用 `parentName`
而不是 `parentIndex` 挂接。**它们同样会被 runtime 建成 `ActorSwingDynamicBone`，所以
`swing` 该带的一项都不能少**——缺了就落进 `SetDefaultValues` 的惰性默认值。

可选的 `swingChains` 数组描述要新建的 `ActorSwingChain`：`host`（宿主骨名）、
`rootBones`（链根骨名数组）、`category` / `chainLength`（信息字段）。**必须按链长分组**，
长短链混在一条里会被 `UpdateChainInfo` 截到最短成员的长度。
写了 `swingChains` 但不是数组 = 整份 sidecar 报错（此前是静默当作"没有链"，包被验证器判坏
而实机看起来只是"不摆"）。`chainLength` 只进日志，类型错了由 `verify_ab_package.py` 报错，
运行时按缺省 0 处理、不会因此作废整份 sidecar。

### `swing` 字段

| 字段 | 说明 |
|---|---|
| `damping` / `stiffness` / `spring` / `mass` | 基本摆动参数 |
| `pendulum` / `pendulumRange` | 重力项与其作用范围。**`pendulumRange` 留 0 等于把重力项乘没了**，原版 84.6% 取 `1.0`（中位数 1.0） |
| `wind` / `useWindGlobalForce` | 风力。`useWindGlobalForce` 写 JSON 布尔或 0/1 都收 |
| `rootWeight` | 跟随链根的比例。留 `1.0`（`SetDefaultValues` 的默认）= 完全刚性、锁死不摆；原版 89.5% 取 `0.3` |
| `colliderRadius` / `colliderRadiusSub` / `colliderType` / `collisionMask` | 碰撞体。**也仍收旧的嵌套写法** `collider: {radius, type, collisionMask}`；两种都在时平铺的优先 |
| `useLimit` / `limitX` / `limitY` / `limitZ` | 每轴角度限位（度）。⚠️ 限位是**按骨轴**授权的：原版摇物骨的子骨一律在 local −X，而 MMD 等外部 rig 常在 local −Z，照搬原版限位会锁死真正的摆动轴。拿不准就写 `useLimit: 0` |
| `dynamicType` | `0`=Swing、`1`=Slide |

> **摆动链要带链尾 tip 骨。**这不是 runtime 的校验项，是数据完整性要求：**链深 N → N 层，
> 链尾也在层里**，少写 tip 就少一层、且末节朝向未定义。
> 无权重的 tip 骨不会出现在 `m_Bones` 里，导出器要显式补。
>
> ⚠️ 2026-08-11 修正：本节曾写「`UpdateChainInfo` 本就排除每条链的最后一根骨」。**排除那
> 半句是错的**——实测游戏自己的裙摆链 `layer[4]` 里就有 `LeftBackSkirt5_S_End`。要补 tip
> 这个结论不变，但理由是「多一节多一层」，不是「防止真正该摆的那根被当 tip 排除」。

`mod.json` 顶层也会带一份同值的 `runtimeProtocol` / `buildId`。顶层 `runtimeProtocol` 若存在，
runtime 会要求它是整数 `1`；缺失时兼容旧包。sidecar 中的 `runtimeProtocol` / `buildId` 是骨架
契约的硬校验，顶层 `buildId` 供离线校验工具与 sidecar 比对。

GakumasMI 插件导出时自动生成 sidecar；**手写 manifest 时这三个字段最容易漏**。

## 协议 2：来源代理骨架（实验能力，不是发布功能）

`runtimeProtocol: 2` 走 `BuildSourceProxyBoneArray()`：**每根骨都新建，不复用任何学马
Transform**，网格继续吃来源权重和来源 bindpose。发布版只接受协议 1，所以协议 2 的包在
发布 runtime 上是硬失败，不会静默退回混合 graft 产生误导性几何。

协议 1 用**一个 `bones` 数组同时**描述「要建的层级」和「Renderer 权重索引对应的骨数组」。
这两件事在无权重的根骨、无权重中间父节点和挂点上必然对不上——A-pose 那个包声明
`rootBone: "Hips"`，而 `Hips` 不带权重、不在 `m_Bones` 里，运行时找不到根骨就整体关闭，
实机表现是 `meshApplied=0 / materialApplied=1`（只换了材质）。协议 2 把两者拆开：

| 字段 | 要求 |
|---|---|
| `transforms` | **必需**。完整来源 Transform 树，含无权重祖先、辅助节点和挂点。每项字段与协议 1 的 `bones` 相同；`parentIndex` 允许**任意顺序**（父可以排在子后面），越界、自指或成环 = 整份作废 |
| `skinBones` | **必需**。指向 `transforms` 的索引数组，**顺序必须与网格权重索引和 bindpose 一致**。长度必须等于 Renderer 的骨数组长度 |
| `bindposeCount` | 可选。写了就必须等于 `skinBones` 长度。bindpose 本体在 bundle 的 Mesh 上，**不在 sidecar 里重复一份**（两份真值迟早对不上） |
| `rootTransform` | 可选。代理层级顶层，**必须是 `parentIndex < 0` 的节点** |
| `rootBone` | Renderer 的根骨。按名字在 `transforms` 里解析，**可以不是蒙皮骨**。不写且层级有多个根 = 失败 |
| `semanticMap` | 可选。`{学马人体语义: 来源 transform 名}`。任一取值找不到对应 transform = 整份作废。rest-only 阶段解析并校验但不使用——动画桥是后续阶段，提前要求这个字段等于假装它已经接上了 |
| `headSocket` | 可选。`{"transform": "<名字>"}`，同样按名字解析校验。保留游戏脸/头发的接合点，当前只校验引用 |
| `experimentalSourceProxy` | **必需**。`mode` 取 `"rest-only"`（只建骨架、停在 bind 姿势）、`"animation-bridge-minimal"`（驱动 20 根人体骨，无手指）或 `"animation-bridge"`（驱动全部已映射语义）。后两者**必须**带 `semanticMap`。物理三种模式下都关 |

### 动画桥（`animation-bridge*`）

每根被驱动的骨在建桥时算一个常量：

```text
correction = 游戏骨静止世界旋转⁻¹ · 代理骨静止世界旋转
每帧：       代理骨世界旋转 = 游戏骨世界旋转 · correction
```

展开后等于「actor 当前朝向 × 游戏骨相对自身静止的变化 × 来源骨自己的静止」——
**骨长、关节位置、局部轴向都不进公式**，这正是来源骨架能保住自己比例的原因。
两个静止姿势在**同一个世界帧**里采样，所以采样瞬间 actor 的朝向会自己抵消掉。

- 游戏骨的静止**从原版 bindpose 反解**（`rendererRot · rot(inv(bindpose))`），不读活体：
  mod 生效时 actor 可能已经在动，把摆着的姿势当静止会把它烙进之后每一帧。
  已在原版真值上验过：20 根被驱动骨的 bindpose 静止与节点静止完全一致
  （不一致的 50 根全是 `_S` 摇物骨，桥不碰）。
- 代理骨的静止**直接读活体**——它刚建好、还没人驱动，而且语义可能落在无权重的
  transform 上（这副 rip 的 `Hips` 就是），那种骨压根没有 bindpose。
- **只写世界旋转，不写位置**，所以来源骨长原样保留。
- 按 transform 下标升序写（导出器按深度优先写树 ⇒ 父先于子）。先写子会被随后写的父拖走。
- 挂在 `CampusActorController.LateUpdate`，**先调原函数再写**：游戏的 Animator、IK、
  关节限位、摇物 job 和这个函数自己的点头/视线修正都跑完了，且还没渲染。
- **桥按名字存，逐 actor 绑到活体上**。换装发生在 `AssetBundle` 加载钩子里，改的是
  **prefab 资产**；游戏渲染的是它的 `Instantiate()` 副本。建桥时能摸到的每一个
  transform（游戏骨和代理骨都是）都属于资产，往它们身上写世界旋转**永远不会有画面**。
  所以建桥只留 `(游戏骨名, 代理骨名, correction)`，每个 actor 第一次 LateUpdate 时
  在自己子树里按名字解析一次并缓存（`RegisterBones` 会清缓存，让「绑早了、部件还没挂上」
  的 actor 有第二次机会）。correction 是两个静止的比值，actor 朝向在它里面已经抵消，
  所以在资产上算、在实例上用是同一个值。
- **头靠 `headSocket` 接**：脸和头发是独立部件，骑在**原版 `Head` 骨**上，Animator 照旧
  把它停在原版头高，而身体自己的头在来源比例决定的位置——差值就是那 13cm。每帧把原版
  `Head` 的**世界位置**吸到来源头骨上即可闭合。**只写位置不写旋转**：旋转本来就对
  （`游戏骨世界旋转 = 代理骨世界旋转 · correction⁻¹` 是恒等式），而且留着游戏自己的
  点头/视线修正继续驱动脸。位置写的是绝对目标不是增量，所以就算 Animator 不重写这根骨
  的局部位置，重复写也只是幂等，不会累积漂移。
  `headSocket` 缺省（`-1`）时就用 `semanticMap` 里的 `Head`；声明了就以它为准——
  头骨位置尴尬的 rip 可以另指一根。
- 尚未做：Hips 位移（整体仍钉在静止高度）、手指、物理。

**没有向下兼容的单数组写法**：协议 2 缺 `transforms` 直接报错。留一条离线闸门拒绝、
运行时却照收的路径，等于给自己再造一次「日志全绿而画面不对」。

`transforms` 里的名字**必须唯一**——`rootBone`、`rootTransform`、`semanticMap`、`headSocket`
全按名字解析，重名会静默指到另一根骨上。运行时按 `__gmi_sp_<index>_<name>` 建 GameObject，
所以游戏侧任何「按名字找骨」的机制（`AttachQuartzDriver` 的 `resolveBone`、`RegisterBones`）
都够不到代理骨：**代理路线下的一切驱动必须由动画桥或我们自己的求解器写**。

离线闸门：`mod-workspace/experiments/source-rest-claymore-2026-08-16/check_source_proxy_sidecar.py`
（`--demo` 自检：1 个正常包不误报、10 个坏包全报）。

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
- OFF 时只恢复当前 Renderer 快照中仍存活、且与可逆记录匹配的实例；不跨帧保留场景对象指针；
- ON 时对当前目标资源子树重应用；当前无目标时按 `modId + source` 排队，等角色或 Renderer
  生命周期回调到来后重试。此时 `hotInstances=0` 是正常的延迟状态；
- 冷路径（资源加载时）的 Mod 贴图写入 Runtime 创建的私有材质；活体热路径直接写游戏自己的
  per-actor 材质，写前快照原贴图，OFF 时先注销 override 再写回；
- 已有 `MaterialPropertyBlock` 只作为兼容性快照保留，不是贴图写入者。实机探针确认这些场景
  从不调用 `Renderer.SetPropertyBlock`；
- `Renderer.set_sharedMaterials` / `set_materials` 的钩子保留兼容性诊断，并作为延迟热重应用的
  Renderer 生命周期触发器；「游戏写回材质数组导致热 ON 颜色错」这条归因仍已被抓帧证伪，
  真因是换空间时漏搬法线/切线，已修复；
- 新增摇物骨/链只在 prefab graft 与角色初始化阶段进入 Animation Rig；对已经初始化的活体直接
  热 ON 不会补建新链，必须重新进入场景。

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
