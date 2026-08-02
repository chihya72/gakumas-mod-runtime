# 游戏内 Mod 管理页面：当前流程与验证状态

> 最后更新：2026-08-01
> 本文描述当前源码和当前部署版。证据状态严格分为“已实机确认”和“已部署待复验”；
> 编译成功或日志中的局部调用不能替代可见实机结果。

## 1. 当前结论

管理器已经不是主页上的文字探针。当前主路径会借用游戏真实的 `SettingWindow` /
`SettingTopScreen`，把目标实例重构为完整的“Mod 管理”页面，并保留游戏自己的返回栈、
标题、滚动区、底部分页和开关组件。

已经由实机截图或同轮日志确认：

- `xinput9_1_0.dll` 自动加载，并与 `xinput1_3.dll` 的 Runtime API v1 握手；
- 主页菜单显示“Mod 管理”入口，点击保留原生动画；
- 页面标题、服装/发型分页、两个 `ScrollRect` 和原生 `SwitchButton` 可用；
- 每个 Mod 使用固定的“官方预览 / Mod 名称 / 游戏内选择目标 / 开关”行结构；
- 三个服装 Mod 的 Master 名称和官方服装缩略图可见；
- 发型资源键归一化后命中“月村手毬 · 公主皇冠”；
- 开关写回 Manifest，并更新 Runtime 当前会话；
- 原生开关在输入回调后发生二次反转的问题已经通过帧末校正确认修复；
- 标准服装替换可以在当前会话热关闭和热开启；
- Mod 页再次点 Mod 管理、系统设置页进入 Mod 管理、Mod 页进入系统设置三条导航分支
  已在上一部署版日志中执行成功。

当前最后三项修复已经编译，但尚未由用户重新启动游戏复验：

1. `SettingTopScreen.Reload()` 复用同一个 `MenuView` 地址时，主动失效入口注入缓存，避免
   从 Mod 管理返回系统设置后菜单中缺少“Mod 管理”；
2. `Renderer.set_sharedMaterials` / `set_materials` 或底层 `SetMaterialArray_Injected` 原始
   写入完成后，按可逆注册表恢复完整 Mod 材质数组，同时保留 Renderer 现有的每材质
   `MaterialPropertyBlock`；
3. 两栏 Mod 页直接设置 `SelectedBarRect.sizeDelta.x`，避免橙条继续按三栏比例绘制。

所以当前结论是：**完整页面和热开关主链已跑通，但最新菜单缓存、材质数组恢复、两栏橙条
修复以及完整生命周期矩阵仍待实机验收，MVP 尚未完成。**

## 2. 构建与部署基线

当前游戏目录中的管理器：

```text
<游戏目录>\xinput9_1_0.dll
大小：270848 字节
SHA-256：DBAE909DEA768F74C13D51944649554E75F28B6AD517EFF0A9E4912A350766DC
状态：已部署，最新入口缓存修复待实机复验
```

当前游戏目录中的 Runtime：

```text
<游戏目录>\xinput1_3.dll
大小：571904 字节
SHA-256：D262698D56293E624AF3613EB00F9ECD010C2893DD3F77B1FA0E2D6C9D7D1CE4
状态：已部署；底层材质钩子启动旁路、原生两参数 ABI 和已登记 Renderer 筛选待实机复验
```

联合替换前的备份：

```text
<游戏目录>\codex-backups\20260801-212145-menu-entry-mpb-refresh
```

历史可复现基线：

| 时间/构建 | 已确认结果 | 备注 |
|---|---|---|
| 12:23，`4AEF...F7DBD5` | 入口、点击、三条 Runtime 文本和显隐 | 仅 M1 文字探针，不是当前 UI |
| 18:20–18:21，`2217...77864` | 完整 Setting 页面、分页、滚动、开关写回 | 暴露重复导航、排序和行结构问题 |
| 后续部署版 | 固定行、Master 名称、服装缩略图、发型名称、开关帧末校正 | 截图与日志已确认 |
| 21:13–21:14 上一部署版 | 三种幂等导航分支执行成功 | Reload 后同地址菜单入口缓存缺陷被复现 |
| 当前部署版 | 入口缓存失效 + 热开启材质数组恢复 | 已部署，启动卡住修正版待实机复验 |

## 3. 当前源码流程

入口实现：`src/CampusUiProbe.cpp`。

```text
PluginMain.BootstrapThread
  → RuntimeClient.Connect / GetModsJson
  → Runtime API v1 就绪后启动 UI Probe
  → ProbeThread 等待 GameAssembly.dll
  → UnityResolve 初始化并 attach 当前线程
  → 解析当前 PC metadata 成员和 Unity UI 方法
  → Hook MenuPresenter.SetEvent / OnAfterInitialize
  → Hook CampusButtonBase.OnClicked
  → Hook EventSystem.Update
```

### 3.1 菜单入口

```text
MenuPresenter.SetEvent
  → EnsureEntry 读取 MenuPresenter._commonView
  → MenuView.GetSubButton(ClearCache) 取得副按钮模板
  → Internal_CloneSingleWithParent 克隆入口
  → 读取克隆体 MenuButtonViewBase._button
  → SetCustomText("Mod 管理")
  → 完整成功后把当前 MenuView 记入 g_injectedViews
```

`_subButtons` 的键是 `MenuButtonSerializeType` 对象，不是整数 `MenuButtonType`。当前实现只读
该字典，不写入新键；入口由克隆按钮的对象地址识别。

### 3.2 从普通页面进入 Mod 管理

```text
PressHook 识别 g_modButton
  → g_modScreenPending = true
  → MenuPresenter.set_SelectedButtonType(Setting)
  → OutGameMenuPresenter.OnSelected()
  → 游戏创建 SettingWindow / SettingTopScreen
  → EventSystem.UpdateHook 查找已初始化的 Setting CampusSimpleTab
  → 只接管第一页为 PreferenceTabPage 的新实例
  → ComposeModScreen
```

`SettingTopScreenPresenter.SetEvent()` Hook 仍是快速路径，但当前 PC 的真实创建流程会绕过或
内联它，因此 `EventSystem.Update()` 是必要的主线程兜底。

### 3.3 页面组合

```text
GetModsJson
  → RuntimeModSnapshot
  → ModPresentationModel（稳定 modId、分类、冲突与玩家文案）
  → 读取 Costume / CostumeHead / Character Master
  → 标题改为“Mod 管理”
  → CampusSimpleTab 改为“服装 / 发型”，隐藏第三页
  → 取得两页 ScrollRect.content
  → 复制设置页原生开关行，生成固定 Mod Cell
  → 服装调用官方 CostumeThumbnail 组件
  → 发型调用 CostumeHead.GetThumbAssetName + ThumbnailViewBase.Set
  → 保存活动 CampusSimpleTab 身份和各开关的 modId 绑定
```

发型名称已经实机命中。日志也确认 `img_cos_costume_head_ttmr-hair-0002_head` 已交给官方
缩略图组件；最终预览图在当前页面可见仍需单独截图验收，因此文档不把它写成已完成。

### 3.4 Mod 开关

```text
PressHook 识别克隆行内部 CampusButton
  → 按稳定 modId 调用 RuntimeClient.SetModEnabled
  → Runtime 检查同目标冲突
  → 更新当前会话有效 replacement map
  → 原子写回 mod.json 的 enabled
  → 标准 SkinnedMeshRenderer 规则恢复或重应用当前实例与缓存 Prefab
  → 管理器重读 Runtime 快照
  → 重建当前页文本并校正所有受影响开关
```

`EventSystem.UpdateHook` 在调用游戏原始输入处理后执行 `ReconcileToggleVisuals()`，避免
`SwitchButton` 自己的监听器在插件回调返回后再次反转 ON/OFF 外观。

冲突语义：

- 启动扫描发现同目标多个 Mod 同时为 ON：Runtime 把该冲突组全部关闭并持久化，页面提示
  “Mod 冲突……已自动关闭”；
- 当前会话已有同目标 Mod 为 ON：新 Mod 的开启请求返回 `GMR_E_TARGET_CONFLICT`，旧 Mod
  保持 ON，新 Mod 保持 OFF，并提示先检查和关闭占用者。

Presentation Model 和 Runtime catalog 测试已覆盖这些规则；冲突两条实机交互分支仍需验收。

### 3.5 热恢复与即时颜色刷新

Runtime 启动时注册全部有效候选，但 AssetBundle 仍懒加载。标准原地替换首次应用时记录
Renderer 的原 Mesh、材质、骨骼、根骨和已有每材质 `MaterialPropertyBlock`：

- OFF：恢复当前场景实例与缓存 Prefab；
- ON：只对目标服装/发型资源子树重应用，并刷新活动 Animation Rig；
- 整对象替换和附加式规则：不承诺可逆，继续按资源重新加载降级。

热 ON 后直接返回主页颜色错误、切页面才恢复仍是已知缺陷。当前部署已恢复为 IDA MCP
调查前的热切换基线；底层材质数组实验钩子及其后续崩溃排查扫描已撤回。

> **已证伪**：曾记录的原因「Renderer 现有 PropertyBlock 的旧贴图优先于材质贴图」是错的。
> 2026-08-02 的互补探针显示游戏在这些场景**从不调用** `Renderer.SetPropertyBlock`，
> `Material.SetTexture` 也从不写我们的材质。基于该结论的几轮 Runtime 修复改的是一条
> 从未执行的路径。

实机抓到的写入链会在热重应用之后重新赋值材质数组，把带 Mod 贴图的私有材质换掉。
当前先保留“不崩溃且可热切换，必要时切页刷新”的行为；完整排除过程和下一步见
[`OPEN_DEFECTS.md`](OPEN_DEFECTS.md)。

## 4. 三种幂等导航

| 当前页面 | 点击 | 预期行为 | 当前证据 |
|---|---|---|---|
| Mod 管理 | Mod 管理 | 只关闭菜单层，不重复创建或重构页面 | 21:14 日志确认 |
| 系统设置 | Mod 管理 | 原位复用当前 `SettingTopScreen` 并组合为 Mod 页面 | 21:13 日志确认 |
| Mod 管理 | 系统设置 | 调用 `Campus.ICampusScreen.Reload()` 原位恢复干净设置页并关闭菜单 | 21:13 日志确认 |

最后一条会销毁并重建 Setting 内容，但游戏可能保留同一个 `MenuView` 地址。上一构建只按地址
去重，导致新菜单没有重新克隆入口。当前实现会在 Reload 成功后：

```text
g_activeModTab = nullptr
g_toggleBindings.clear()
g_injectedViews.erase(g_menuViewInstance)
g_modButton = nullptr
```

下一次 `MenuPresenter.SetEvent` 因此能够对同地址 View 重新注入。这个最终分支待实机复验。

## 5. 关键安全边界

- 所有 Master、Unity UI 和 GameObject 操作只在 Unity 主线程执行；
- 必需成员缺失时按能力降级，全屏能力失败不删除主页入口；
- `g_injectedViews` 只在入口克隆和按钮读取完整成功后登记；
- 页面组合、Reload 和 Runtime 开关有空值检查、compiled-body 检查与 SEH 边界；
- UI 失败不修改 Runtime 资源状态；Runtime 切换失败保持原状态并返回错误码；
- 管理器不控制游戏进程，不自动启动或关闭游戏；
- 不写 `_subButtons` 字典，不把整数写入 `MenuButtonSerializeType`；
- 不执行克隆模板原来的 ClearCache 点击逻辑。

## 6. 已废弃路线

以下只保留为历史，不得接回 Release 主路径：

- 在主页 Canvas 上放大 `MenuSubButtonView` 作为最终管理面板；
- `Time.get_deltaTime` / `CampusActorController.LateUpdate` 每帧搜索 Presenter；
- `HomeTopScreenPresenter.OpenNoticeSheetAsync`；
- `ErrorSheetManager.OpenAsync` 加未经验证的托管委托；
- 伪点击真实设置按钮的 `CampusButtonBase.OnClicked`；
- 把整数 `MenuButtonType` 写入 `_subButtons` 的对象键；
- 仅靠 `SettingTopScreenPresenter.SetEvent()` 接管页面；
- 通过启用状态排序列表，导致开关后条目换位。

## 7. 当前待验收项

按优先级执行：

1. 在用户手动启动游戏后，执行 Mod → 设置 → 菜单，确认“Mod 管理”入口仍存在；再执行
   设置 → Mod、Mod → Mod，确认没有卡住、叠页或错误启动；
2. 对同一标准服装 Mod 执行 ON → 返回主页、OFF → 返回主页、再 ON → 返回主页，确认 Mesh、
   材质、骨骼和颜色都即时正确，不需要切换其他游戏页面；
3. 打开发型页，截图确认“月村手毬 · 公主皇冠”的官方预览图实际可见；
4. 实机触发启动时冲突组全关，以及运行中新 Mod 被占用者拒绝的两种提示；
5. 验证返回主页、重登、重复打开、16:9/16:10、空列表、长名称和大量条目；
6. 完成写入失败、API 不兼容、Manifest 异常和安全 Shutdown 降级测试。

每轮实机测试必须同时记录管理器/Runtime DLL 哈希、日志时间、操作顺序和可见结果。
