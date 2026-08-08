# 当前状态与路线

> 最后更新：2026-08-09
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
- OFF 恢复当前实例与缓存 Prefab，ON 对目标资源子树热重应用；
- 热重应用身份在资源加载时记录，不再依赖「先 OFF 过一次」；
- `CampusActorAnimationRig.RegisterBones` 抛异常时用 SEH 兜住，不打断整个 toggle；
- 活动 `CampusActorAnimationRig` 的动态骨补齐与刷新；
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

后三条保留为兼容性快照/诊断 Hook；当前已验证场景没有调用它们写回 Mod 材质，不能再把
`PropertyBlock`、`Material.SetTexture` 或材质数组写回描述为热 ON 颜色问题的成因。

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

> 另有两个 UI 缺陷（底栏多一颗星形分隔、橙条按三栏比例）已于 2026-08-02 实机确认修复。

部署版的大小与 SHA-256 不在文档里抄写——两处手抄的哈希曾经同时过期。release 的哈希在
release notes 里，本机部署版用 `Get-FileHash` 自己算。

### 已知未解

1. **无摆动声明的骨也被挂上 `ActorSwingDynamicBone`**。`createBone` 不看 sidecar 有没有给
   摆动参数，一律加组件。已发布包的 sidecar `newBones` 是空的（`chisaki-swimsuit`、
   `hmsz-fuyuko-icu` 都是 0），于是 22 根源专属骨被做成 22 条长度 1 的链，
   `CampusActorAnimationRig.RegisterBones` 抛 `ArgumentOutOfRangeException`。目前只有
   `RegisterRigBonesGuarded` 的 SEH 兜底，toggle 不再被打断，但异常每次都发生。
   **运行时这半的修法**：只有 sidecar 声明了摆动参数的骨才挂组件。
   > 摆动链本身能不能跑（导出器要产出 `newBones`：摆动参数 + 链尾 tip）不属于本仓库，
   > 规范见 `gakumas-modding/research/ab-route-notes.md` §3，进度在同仓库
   > `research/current-status-and-roadmap.md`「逐项验证等级」第 4 项。
2. **冷路径的克隆没人销毁**。游戏每次重新 `LoadAsset` body prefab 都会克隆一份 Mesh 和
   两份私有材质，它们跟着 prefab 走。频率低于热开关，但长会话会累积；要动得先确定
   prefab 何时真的不再被引用。
3. **Mod 关闭后已加载的 prefab 仍是打过补丁的**，所以关掉之后再进换装页面可能仍显示 Mod。

## 下一步优先级

1. 只给 sidecar 声明了摆动参数的骨挂 `ActorSwingDynamicBone`，消掉每次热重应用都发生的
   `RegisterBones` 异常；摆动链能否真的摆是 `gakumas-modding` 的导出器课题，不在本仓库；
2. 实机覆盖启动时冲突组全部关闭、运行中新 Mod 被拒绝的两条冲突分支；
3. 用真实 hair Mod 验证 `Geo_Hair` 与 `Geo_HairProp` 双 Renderer 的热恢复；
4. 把 `appliedThisSession` 接到所有真实应用和热恢复结果，而不是仅保留目录字段；
5. 给 replacement map 与 load history 完成统一并发审计；
6. 让 Validator/Doctor 离线核对 Bundle 内 asset path；
7. 明确 `face` 的 profile、Renderer 和材质规范；
8. 为整对象替换与附加式规则设计受控热重载或明确的重新加载提示。

## 发布前验收

- Release x64 构建、9 个导出（8 个 XInput 代理 + `GmrGetRuntimeApiV1`）和当前 API 结构尺寸；
- Python 测试、`mod_presentation_tests` 和源码契约测试（`tests/ModRuntimeCatalogSmoke.cpp`
  尚未接进 premake，不在这条里）；
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
