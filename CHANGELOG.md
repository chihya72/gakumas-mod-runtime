# Gakumas Mod Runtime 更新日志

## 未发布 — 组件装配收严：预检 fail-closed，不留半初始化组件

- **姿势驱动器改成"先预检、再 AddComponent"**（`AttachQuartzDriver`）：setting 类、四张参数表里
  每个字段名、每根引用骨，缺任一项就整体拒绝并写明缺什么，**prefab 一点不动**。
  以前是先挂组件再逐项写引用、缺什么只 warn 后继续 —— prefab 上于是留下一个半初始化的组件，
  它照样被 Instantiate、照样 OnEnable，然后带着空引用参与解算，日志里只有一行 warn；
- **预检过了还失败就回滚**：`setting` 建不出来、或写引用时字段/骨又没了，都会
  `DestroyComponentImmediate` 撤掉刚挂上的组件；
- **摇物组件初始化失败同样撤掉**：以前失败时组件留在 prefab 上、又没记进
  `g_createdActorSwingBoneNames`，后续清理找不到它；
- **驱动器挂不上时不静默替换成摇物**（两者二选一），只把"这根骨在游戏里不会动"写进日志 ——
  偷偷换一个求解器等于给作者一个"能动但不是他配的"结果；
- **可选方法找不到时不再记 ERROR。** `SkinnedMeshRenderer.ResetBounds` / `ResetLocalBounds`
  在这版游戏里被裁掉了（2026-08-18 实机日志坐实），而两处调用本来就判空、拿不到就跳过刷新
  包围盒 —— 每次启动刷两条 ERROR 会把"日志里有 error 就得看一眼"这条规矩磨钝。
  `Il2cppUtils::GetMethod` 新增 `optional` 参数，这类查找降为 Info；
- 纯判定逻辑拆到 `src/runtime/DriverPrecheck.hpp`，`mod_runtime_catalog_tests` 里新增
  `DriverPrecheckSmoke`：坏 sidecar（骨找不到 / 字段不存在 / 回调为空）必须报且点名，
  正常包一项都不报。
- **验证收口（2026-08-18）**：`tools/package.ps1 -Version dev` 构建并通过三个原生检查
  （`ModPresentationModelTests`、`DriverPrecheckSmoke`、`ModRuntimeCatalogSmoke`）后再打包。

## 1.0.0 — 独立运行时与游戏内 Mod 管理正式版

- 以 `xinput1_3.dll` 代理提供独立 AssetBundle Mod Runtime，统一扫描
  `gakumas-mod/mods/<mod-id>/mod.json`，支持 body、hair 与 `Geo_HairProp` 多 Renderer 替换；
- 内置游戏内 Mod 管理界面和 Runtime API v1，支持目录快照、启用状态持久化、同目标冲突保护，
  以及标准 `SkinnedMeshRenderer` Mod 的会话内热 OFF/ON；
- 热切换会保存并恢复原 Mesh、材质、骨绑定和 MaterialPropertyBlock；当前场景尚无目标时按
  `modId + source` 去重排队，在后续资源生命周期事件中重试；
- 支持 manifest v2、骨架 sidecar、运行时新建骨、摆动物理参数、碰撞体与 `ActorSwingChain`；
  `runtimeProtocol` / `buildId` 不匹配会在应用前被拒绝；
- 启动冲突采取 fail-closed：同一目标的多个启用 Mod 会整组关闭；即使 Manifest 写回失败，
  当前会话也不会静默选择某个优先级赢家；
- 将初始化移出 `DllMain` 的 loader lock，并在安装进程级 Hook 前固定模块；共享摇物状态改为
  锁内快照、锁外调用 Unity/IL2CPP，降低卸载竞态和重入死锁风险；
- Release x64 打包现在强制运行 `mod_presentation_tests` 与 `mod_runtime_catalog_tests`；Catalog
  测试覆盖有效/无效 Manifest、缺 Bundle、多目标、冲突、原子写回和写回失败回滚语义；
- 发布包包含代理 DLL、默认配置、安装说明、项目许可证和全部第三方许可证。

### 当前边界

- 新增摇物骨或链仍需重新进入场景，活体热 ON 不会为既有角色补建新的骨链；
- 整对象替换与附加式规则不承诺即时逆转；
- 冷路径创建的 Mesh/Material 克隆与未进入当前快照的 inactive prefab，仍需后续生命周期治理。
