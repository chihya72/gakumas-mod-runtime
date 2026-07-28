# 当前状态与路线

## 已实现

- XInput 代理入口与独立日志；
- manifest v2 基础字段和优先级冲突处理；
- AssetBundle 懒加载；
- IL2CPP 线程 attach；
- 同步/异步单资源 hook；
- GameObject、Mesh、骨骼索引和贴图替换；
- Mesh patch 失败保护；
- source profile、Validator 和 Author Doctor。

当前 hook：

- `UnityEngine.AssetBundle::LoadAsset_Internal(System.String,System.Type)`；
- `UnityEngine.AssetBundle::LoadAssetAsync_Internal(System.String,System.Type)`；
- `UnityEngine.AssetBundleRequest::GetResult()`；
- `UnityEngine.AssetBundleRequest::get_asset()`。

当前不安装 `AssetBundleRequest::get_allAssets()` hook。

## 优先任务

1. 给 replacement map 与 load history 补齐并发保护；
2. 用真实 hair Mod 验证 `Geo_Hair` 与 `Geo_HairProp` 双 renderer；
3. 让 Validator/Doctor 离线核对 bundle 内 asset path；
4. 明确 `face` 的 profile、renderer 和材质规范；
5. 增加受控热重载与失败回退。

## 不承诺自动完成

- 修复错误蒙皮或判断美术质量；
- 让新增骨骼自动参与原 Animator；
- 自动转换 VRM/MMD 材质；
- 自动重展 UV 或烘焙全部游戏 shader 贴图；
- 自动注入复杂 prefab 逻辑组件。
