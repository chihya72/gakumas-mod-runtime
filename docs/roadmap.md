# 当前状态与路线

> 最后更新：2026-08-12
> “已实现”不自动等于“已实机验收”。本页只写已经验过的和明确没验过的，别把编译通过
> 当成生效。

## 已实现

- XInput 1.3 代理入口与独立日志；
- Manifest v2 完整目录、禁用项、单逻辑目标校验和原子写回；
- Runtime API v1 JSON 快照、跨 DLL 缓冲区释放和当前会话启停；
- 启动时同目标多项开启则整组自动关闭，运行中拒绝开启被占用目标；
- AssetBundle 懒加载；
- IL2CPP 线程 attach；
- 同步/异步单资源 Hook；
- GameObject、Mesh、骨骼索引和贴图替换；
- Mesh patch 失败保护；
- 标准 `SkinnedMeshRenderer` 的可逆快照：Mesh、材质、骨骼、根骨和每材质 PropertyBlock；
- 换空间时同时搬运顶点、法线和切线（切线 w 保留副切线符号）；
- 热路径直接把 Mod 贴图写进游戏自己的 per-actor 材质，写前快照原贴图，OFF 时先注销
  override 再写回；活体路径不再克隆私有材质，也不再灌 PropertyBlock；
- 热 OFF 释放并 `Object.Destroy` 本次热 ON 克隆的 Mesh；
- OFF 只恢复当前 Renderer 快照中仍存活、且与可逆记录匹配的实例；不跨帧保存或解引用旧场景
  Renderer/GameObject 指针；
- 热重应用身份在每次资源加载时记录，不再依赖「先 OFF 过一次」；每个 source 保留有界的近期
  原 Mesh 身份并排除 Runtime 自己的克隆，角色重建后不会把新原 Mesh 当成陌生对象；
- ON 时若当前没有目标 Renderer，按 `modId + source` 进入延迟队列，并由 `RegisterBones` 与
  Renderer 材质生命周期回调重试；OFF 会清除同 Mod 的待处理请求；
- 新骨和摇物链在 prefab graft 阶段建立，`RegisterBones` 前只补齐链 layers，再由游戏原生初始化
  收走；旧的并行表追加和 `RegisterRigBonesGuarded` SEH 兜底已删除；
- OFF 清除该 Mod 的混合骨数组缓存，但故意保留带 `modId + sidecar 指纹` 的骨归属记录，避免
  ON→OFF→ON 每轮在层级里重复创建同名骨；失效对象在复用时做存活检查；
- Renderer/节点刷新和持久贴图覆盖；
- source profile、Validator 和 Author Doctor。

当前 Hook：

- `UnityEngine.AssetBundle::LoadAsset_Internal(System.String,System.Type)`；
- `UnityEngine.AssetBundle::LoadAssetAsync_Internal(System.String,System.Type)`；
- `UnityEngine.AssetBundleRequest::GetResult()`；
- `UnityEngine.AssetBundleRequest::get_asset()`；
- `UnityEngine.Renderer::set_sharedMaterials(Material[])`；
- `UnityEngine.Renderer::set_materials(Material[])`；
- `UnityEngine.Renderer::SetPropertyBlock(MaterialPropertyBlock)`；
- `UnityEngine.Renderer::SetPropertyBlock(MaterialPropertyBlock,int)`；
- `UnityEngine.Material::SetTexture(int/string,Texture)`。

`Renderer.set_sharedMaterials` / `set_materials` 除兼容性诊断外，也会把本次 observed renderer
交给延迟热重应用重试；该指针只在当前回调使用，不会进入待处理队列。两条 PropertyBlock Hook
保留兼容性快照/诊断。不能再把 `PropertyBlock`、`Material.SetTexture` 或材质数组写回描述为
热 ON 颜色问题的成因。

`Material.SetTexture` 的 Hook 会把参数换成已登记的 Mod 贴图，所以任何还原贴图的代码
必须**先注销 override 再写**，否则写入会「成功」但内容不变。

当前不安装 `AssetBundleRequest::get_allAssets()` Hook，也不安装：

- `Object::Internal_CloneSingle`；
- `Object::Internal_CloneSingleWithParent`；
- `Object::Internal_InstantiateSingle`；
- `Object::Internal_InstantiateSingleWithParent`。

四条 Instantiate 热路径 Hook 曾在实验版出现，2026-07-30 已回退。原因见
[`lessons-learned.md`](lessons-learned.md)。

## 实机状态

2026-08-09 已经确认（同一会话连续 12 轮 ON/OFF）：

- 管理器通过 Runtime API 读取目录并写回开关；
- Manifest 和当前会话状态同步改变；
- 标准服装 Mod 热 OFF/ON 生效，**热 ON 后直接回主页颜色即正确**；
- 每轮 OFF 的贴图还原是 `properties=3/3`，网格还原到原 Mesh；
- 每轮 ON 的克隆都有配对的释放（`destroyed=1`），克隆地址被复用，内存不再增长。

### 已解决：热 ON 后直接回主页颜色错误

现象曾经是：主页 → 菜单 → Mod 管理 → 开关 → **直接返回主页**，网格是 Mod 的、颜色不对；
进入一次换装页面再回主页就正常。

**真因**：`TransformModMeshVerticesToOriginalRendererSpace` 把 Mod 网格搬到目标 renderer
空间时只搬顶点，法线和切线留在原地。冷路径的两个 renderer 都是 prefab 单位阵，这个变换
等于空操作，所以从来没暴露；热路径的目标是场景里有真实旋转的活体角色，顶点转过去而法线
没转，法线与几何脱节，着色结果就错。进入换装页面之所以能恢复，是那条路重新加载资源走冷
路径，变换又变回单位阵。

抓帧量化（`FrameAnalysis` + 索引缓冲算面法线）：

```text
冷路径（正常）  mean(面法线 · 顶点法线) = +0.967   对齐比例 100%
热路径（发暗）  mean(面法线 · 顶点法线) = -0.233   对齐比例  73%
```

同一对帧的 Mesh、8 张贴图、材质常量缓冲、shader 变体和渲染状态逐字节相同——所以此前
「游戏事后写回材质数组」的归因是错的，见 [`lessons-learned.md`](lessons-learned.md)。

### 已解决：管理页开启后回主页仍不生效

现象是：在 Mod 管理页开启 Mod 后，日志写 `hotInstances=0`；直接返回主页仍是原版，必须先进
一次换装页再回来才生效。根因有两层：

1. 管理页切换时场景里可能暂时没有该资源的 Renderer，旧实现没有保存“等目标出现再应用”的
   状态；
2. 角色重建后原 Mesh 指针会变化，旧的单一身份记录会把新一代原 Mesh 当成不匹配对象。

当前实现按 `modId + source` 去重保存延迟请求，在 `RegisterBones` 和 Renderer 材质生命周期回调
中重试；同时保存有界的近期原 Mesh 身份、跳过 Runtime 自己创建的 Mesh 克隆，并允许生命周期
Hook 把尚未出现在 `FindObjectsByType` 快照中的当前 Renderer 直接交给本次匹配。

2026-08-12 `hmsz-fuyuko-icu` 实机日志确认：

```text
Deferred hot reapply queued: ...
Session toggle applied: ... enabled=1 ... hotInstances=0
Deferred hot reapply satisfied: trigger=Renderer.set_sharedMaterials ... applied=0 alreadyPatched=1
```

最后一行不是失败：新角色的资源加载路径已经先完成 Mesh/材质/骨架替换，生命周期重试看到该
Renderer 已使用补丁 Mesh，于是只清除待处理项，不重复应用。该轮同时有正常的 prefab 建链、
`RegisterBones` 和 15 根 `hmsz` live bone 日志；没有异常、崩溃或重复重应用。

同轮 `registration coverage=15/37` 里的 22 个 missing 名字来自已关闭的
`chisaki-swimsuit` 全局骨名记录，不是 `hmsz` 应注册却丢失的骨；`hmsz` 自己的 15 根 live bone
均已读回。启动阶段的 `SkinnedMeshRenderer.ResetBounds` / `ResetLocalBounds` “Method not found”
是当前游戏版本没有这两个可选刷新方法；实现仍会用 Renderer enabled false→true 刷新，不影响
延迟队列或 prefab 建链。它们是待降低日志噪声的兼容性探针，不是本次缺陷复发。

> 另有两个 UI 缺陷（底栏多一颗星形分隔、橙条按三栏比例）已于 2026-08-02 实机确认修复。

部署版的大小与 SHA-256 不在文档里抄写——两处手抄的哈希曾经同时过期。release 的哈希在
release notes 里，本机部署版用 `Get-FileHash` 自己算。

### 当前边界与未解项

1. **冷路径的克隆没人销毁**。游戏每次重新 `LoadAsset` body prefab 都会克隆一份 Mesh 和
   两份私有材质，它们跟着 prefab 走。频率低于热开关，但长会话会累积；要动得先确定
   prefab 何时真的不再被引用。
2. **当前快照发现不到的 inactive/缓存 prefab 不会被主动遍历还原**。replacement map 已更新，
   后续新资源请求会按 OFF 状态处理，但已经缓存、又未进入当前 Renderer 快照的对象仍需实机
   明确其生命周期。
3. **活体热 ON 不补建新增摇物链**。骨/链属于 prefab graft 与角色初始化阶段；切换的 Mod 若改变
   了 swing 结构，必须重新进入场景。已有活体仍可即时刷新 Mesh、材质、骨绑定与碰撞体。
4. **延迟热 ON 的实机样本还不完整**。`hmsz-fuyuko-icu` 已确认；`atbm-cstm-0140` 在最新日志中
   只执行了 OFF，不能据此宣称它的“零活体后再 ON”分支也已验证。

## 下一步优先级

1. 用 `atbm-cstm-0140` 单独复验“当前无目标 Renderer → ON 排队 → 返回主页自动满足”的分支；
2. 实机覆盖启动时冲突组全部关闭、运行中新 Mod 被拒绝的两条冲突分支；
3. 用真实 hair Mod 验证 `Geo_Hair` 与 `Geo_HairProp` 双 Renderer 的热恢复；
4. 确定冷路径 Mesh/Material 克隆与 inactive prefab 的所有权和销毁时机；
5. 把 `appliedThisSession` 接到所有真实应用和热恢复结果，而不是仅保留目录字段；
6. 给 replacement map、身份记录与 load history 完成统一并发审计；
7. 让 Validator/Doctor 离线核对 Bundle 内 asset path；
8. 明确 `face` 的 profile、Renderer 和材质规范；
9. 为整对象替换与附加式规则设计受控热重载或明确的重新加载提示。

## 发布前验收

- Release x64 构建、9 个导出（8 个 XInput 代理 + `GmrGetRuntimeApiV1`）和当前 API 结构尺寸；
- Python 测试、`mod_presentation_tests` 和 `mod_runtime_catalog_tests`；
- 正常退出、强制关闭、重启后的 Manifest 一致性；
- 文件无权限、外部并发修改、缺 Bundle 和损坏 Manifest；
- 服装/发型混合、同目标冲突和大量 Mod；
- 热开关失败时不破坏当前 Renderer，并能安全降级为重新加载资源。

## 不承诺自动完成

- 修复错误蒙皮或判断美术质量；
- 让任意新增骨骼自动参与原 Animator；
- 自动转换 VRM/MMD 材质；
- 自动重展 UV 或烘焙全部游戏 Shader 贴图；
- 自动注入复杂 Prefab 逻辑组件；
- 对任意整对象替换保证无条件可逆热卸载。
