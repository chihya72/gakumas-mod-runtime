# Gakumas 游戏内 Mod 管理器：当前产品与发布计划

> 最后更新：2026-08-09
> 本文只描述当前架构和仍有效的发布计划。已证伪结论和已撤回路线统一记录在
> `../../docs/lessons-learned.md`，不得再作为实现依据。

## 1. 产品摘要

管理器是 `gakumas-mod-runtime` 的内置 UI。Release 只部署一个 `xinput1_3.dll`：同一模块负责
扫描 AssetBundle Mod、应用替换、暴露 Runtime API v1，并在游戏主页注入“Mod 管理”入口。

Mod 根目录固定为：

```text
<游戏目录>\gakumas-mod\mods\<mod-id>\
  mod.json
  <mod-id>.bundle
```

不存在第二个管理器 DLL，不依赖汉化插件的 `version.dll`，也不读取汉化插件的数据目录。

## 2. 第一版产品范围

- 在主页菜单提供唯一“Mod 管理”入口；
- 复用游戏原生 `SettingWindow` / `SettingTopScreen`、返回栈、分页、滚动区和开关；
- 服装与发型分两页显示；
- 显示玩家可读的 Mod 名称、游戏内目标名称和可用的官方缩略图；
- 开关同步修改当前 Runtime 会话和 `mod.json`；
- 同一逻辑目标只能启用一个 Mod，冲突时安全拒绝或关闭冲突组；
- 单个损坏 Manifest、签名失效或 UI 组合失败不能导致游戏崩溃。

不在第一版范围：下载/更新 Mod、联网仓库、账号 API、自动修复美术资产、任意整对象替换的
无条件热逆转。

## 3. 当前架构

```text
xinput1_3.dll
├─ Runtime
│  ├─ 扫描 gakumas-mod\mods
│  ├─ Manifest v2 / Runtime API v1
│  ├─ AssetBundle 懒加载与替换
│  └─ 标准 SkinnedMeshRenderer 热 OFF/ON
└─ Manager UI
   ├─ RuntimeClient（同模块调用 Runtime API v1）
   ├─ RuntimeModSnapshot / ModPresentationModel
   └─ CampusUiProbe（菜单入口、原生设置页组合、开关与导航）
```

Runtime 初始化完成后调用 `GkmmInitialize()`。UI 和 Runtime 之间只通过
`src/runtime/ModRuntimeApi.h` 定义的 API v1 交换快照和开关结果，不复制目录扫描、冲突判断或
Manifest 写回逻辑。

## 4. 已验证能力

- 单 DLL 自动加载与 Runtime/UI 初始化；
- 主页入口、点击、完整设置模板页面、服装/发型分页和滚动；
- 固定 Mod 行、服装 Master 名称和缩略图、发型 Master 名称；
- 开关写回、当前会话热更新、帧末视觉校正和稳定排序；
- 启动冲突组关闭和运行中新项冲突拒绝的代码/离线契约；
- 标准服装 `SkinnedMeshRenderer` 的 Mesh、材质、骨骼和根骨快照及热 OFF/ON；
- Mod→Mod、设置→Mod、Mod→设置三种幂等导航的主分支。

详细证据等级和当前待复验项见 `UI_FLOW.md` 与 `SIGNATURE_MATRIX.md`。

## 5. 材质热开关的正确结论

「热 ON 后颜色错」跟材质写入者无关，2026-08-09 已定案：真因是网格换空间时只搬了顶点、
没搬法线和切线。冷路径两端都是 prefab 单位阵所以看不出来，活体角色有真实旋转就会让法线
和几何脱节。修复后热 ON 直接回主页颜色即正确。

同时作废的两条结论：`MaterialPropertyBlock` 覆盖（探针确认游戏从不调用
`SetPropertyBlock`），以及「游戏事后写回材质数组」（抓帧确认暗色帧和正常帧的身体 draw
逐字节相同）。底层 icall 实验 Hook 已撤回。详见
[`../../docs/lessons-learned.md`](../../docs/lessons-learned.md)。

活体路径现在直接把 Mod 贴图写进游戏自己的 per-actor 材质，不再克隆私有材质、不再灌
PropertyBlock；OFF 时先注销 override 再写回快照的原贴图（`Material.SetTexture` 的 Hook
会拦截还原写入）。

## 6. 发布验收矩阵

自动化：

- Release x64 构建；
- `mod_presentation_tests`；
- Python `unittest discover`；
- 9 个导出（8 个 XInput 代理导出 + `GmrGetRuntimeApiV1`）；
- release zip 的文件树、许可证和 SHA-256。

实机：

1. 启动、退出、重启，确认单 DLL 加载和 Manifest 状态保持；
2. 重复打开菜单，确认只有一个“Mod 管理”入口；
3. Mod→设置→菜单、设置→Mod、Mod→Mod，确认不叠页且 Reload 后入口可重建；
4. 标准服装 ON/OFF/ON，直接回主页记录 Mesh、材质、骨骼和颜色（不再需要进换装页面）；
5. hair + hairprop 双 Renderer 开关和发型预览；
6. 启动冲突组全关、运行中新项冲突拒绝；
7. 缺 bundle、损坏 Manifest、只读文件等安全降级；
8. 返回主页、重登、不同分辨率和长期重复打开。

颜色问题已解决，不得再把 PropertyBlock 或材质数组写回写成原因。仍需作为已知问题记录的是
无摆动声明的骨被挂 swing 组件导致的 `RegisterBones` 异常（见下）和冷路径克隆的累积。

## 7. 发布后优先级

1. 只给声明了摆动参数的骨挂 `ActorSwingDynamicBone`，替掉 `RegisterRigBonesGuarded` 的 SEH 兜底；
2. 完成发型官方预览和完整生命周期矩阵；
3. 完善 `appliedThisSession`、并发审计和离线 bundle asset-path 校验；
4. 为整对象替换与附加式规则提供受控重载或清晰的重新加载提示。

任何新结论都必须同时更新 `UI_FLOW.md`、`RESEARCH_SOURCES.md`、`SIGNATURE_MATRIX.md`、
根 `README.md`、`docs/manifest-v2.md` 与 `docs/roadmap.md`。
