# 当前状态与路线

> 最后更新：2026-08-02
> “已实现”不自动等于“已实机验收”。最新材质数组恢复与两栏橙条补丁已完成本地 Release
> 构建，仍需由用户手动启动游戏复验。

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
- OFF 恢复当前实例与缓存 Prefab，ON 对目标资源子树热重应用；
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

当前不安装 `AssetBundleRequest::get_allAssets()` Hook，也不安装：

- `Object::Internal_CloneSingle`；
- `Object::Internal_CloneSingleWithParent`；
- `Object::Internal_InstantiateSingle`；
- `Object::Internal_InstantiateSingleWithParent`。

四条 Instantiate 热路径 Hook 曾在实验版出现，2026-07-30 已回退。原因和证据见
[`../AB_DARK_RENDERING_INVESTIGATION.md`](../AB_DARK_RENDERING_INVESTIGATION.md)。

## 实机状态

已经确认：

- 管理器通过 Runtime API 读取目录并写回开关；
- Manifest 和当前会话状态同步改变；
- 标准服装 Mod 热 OFF/ON 生效；
- 热重应用日志记录 `targets=2, applied=2, refreshedRigs=1`。

已知缺陷：热 ON 后直接返回主页颜色错误，切换游戏页面后恢复。当前部署已恢复为 IDA MCP
调查前“不崩溃且热切换生效”的基线，暂不承诺即时颜色修复。

> **已证伪：**曾记录的原因「Renderer 已有 `MaterialPropertyBlock` 的旧贴图覆盖克隆材质」
> 是错的——实机确认游戏在这些场景从不调用 `Renderer.SetPropertyBlock`。

2026-08-02 确认的真实写入者是游戏调用 `Renderer.set_sharedMaterials` 替换了我们的私有材质。
排除过程与下一步见 [`../manager/docs/OPEN_DEFECTS.md`](../manager/docs/OPEN_DEFECTS.md)。

部署版的大小与 SHA-256 不在文档里抄写——两处手抄的哈希曾经同时过期。release 的哈希在
release notes 里，本机部署版用 `Get-FileHash` 自己算。

IDA 后实验性的 `SetMaterialArray_Injected` 钩子、按钮帧末队列和受限 Renderer 扫描均已
撤回；ON/OFF 调用链恢复为调查前的同步热恢复/热重应用实现。管理器 UI 修复不在回退范围内。

## 下一步优先级

1. 实机执行同一标准服装 Mod 的 ON/OFF/ON，确认已恢复“不崩溃、热切换生效、直接回主页
   颜色可能错误、切页恢复”的基线；
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
