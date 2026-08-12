# 游戏内 Mod 管理页面：当前流程与验证状态

> 最后更新：2026-08-12
> 本文描述当前源码和当前部署版。证据状态严格分为“已实机确认”和“已部署待复验”；
> 编译成功或日志中的局部调用不能替代可见实机结果。
> Release 只有 `xinput1_3.dll`；Runtime 与管理 UI 编译在同一模块中。

## 1. 当前结论

管理器已经不是主页上的文字探针。当前主路径会借用游戏真实的 `SettingWindow` /
`SettingTopScreen`，把目标实例重构为完整的“Mod 管理”页面，并保留游戏自己的返回栈、
标题、滚动区、底部分页和开关组件。

已经由实机截图或同轮日志确认：

- `xinput1_3.dll` 自动加载，Runtime 初始化后在同一模块内启动管理 UI，并通过 Runtime API v1
  读取和写入 Mod 状态；
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

2026-08-09 实机确认：热 ON 后直接回主页颜色即正确，同会话 12 轮 ON/OFF 的贴图还原、
网格还原和克隆释放全部配对成功。热开关主链和颜色问题都已收口。

2026-08-12 实机确认：如果管理页切换时当前场景没有目标 Renderer，ON 会以
`hotInstances=0` 进入延迟队列；`hmsz-fuyuko-icu` 返回主页时资源加载路径已先完成替换，
Renderer 生命周期回调随后以 `applied=0 alreadyPatched=1` 确认并清队列。此前“必须先进一次
换装页”的缺陷已经收口，但 `atbm-cstm-0140` 的同分支尚未单独复验。

仍待实机复验的一项：`SettingTopScreen.Reload()` 复用同一个 `MenuView` 地址时主动失效入口
注入缓存，避免从 Mod 管理返回系统设置后菜单中缺少“Mod 管理”。

所以当前结论是：**完整页面、直接热开关、颜色修复和零活体延迟重应用（hmsz 样本）均已跑通；
菜单缓存失效补丁、atbm 延迟分支与完整生命周期矩阵仍待实机验收。**

## 2. 构建与部署基线

游戏目录里只需要**一个**本项目 DLL：`xinput1_3.dll`，Runtime 与管理器 UI 编在一起；
Release 不生成或加载第二个管理器 DLL。

当前部署版的大小与 SHA-256 不在设计文档中手抄；以对应 release notes 或目标文件现场结果
为准。核对用
`Get-FileHash <游戏目录>\xinput1_3.dll -Algorithm SHA256`。

历史可复现基线：

| 时间/构建 | 已确认结果 | 备注 |
|---|---|---|
| 12:23，`4AEF...F7DBD5` | 入口、点击、三条 Runtime 文本和显隐 | 仅 M1 文字探针，不是当前 UI |
| 18:20–18:21，`2217...77864` | 完整 Setting 页面、分页、滚动、开关写回 | 暴露重复导航、排序和行结构问题 |
| 后续部署版 | 固定行、Master 名称、服装缩略图、发型名称、开关帧末校正 | 截图与日志已确认 |
| 21:13–21:14 上一部署版 | 三种幂等导航分支执行成功 | Reload 后同地址菜单入口缓存缺陷被复现 |
| 2026-08-09 部署版 | 法线/切线随顶点换空间、活体路径直写游戏材质、OFF 先注销再还原、热 OFF 释放并销毁克隆 | 12 轮 ON/OFF 实机确认，日志 `properties=3/3`、`destroyed=1` 全配对 |
| 2026-08-12 部署版 | 零活体 ON 延迟队列、多代原 Mesh 身份刷新、生命周期重试 | `hmsz-fuyuko-icu` 返回主页自动生效；`alreadyPatched=1` 正常清队列，无异常或重复应用 |

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
  → 发型把 master CostumeHead 作为 ICostume 交给 CostumeThumbnailView.Set
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
  → 标准 SkinnedMeshRenderer 规则恢复或重应用当前快照中的活体实例
  → ON 时若当前无目标 Renderer，按 modId + source 排队等待生命周期重试
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

- OFF：恢复当前 Renderer 快照中仍存活、且与可逆记录匹配的实例；不跨帧持有旧场景指针；
- ON：对当前目标资源子树重应用；当前无目标时排队，在 `RegisterBones` 或 Renderer 材质生命
  周期回调中重试；
- 整对象替换和附加式规则：不承诺可逆，继续按资源重新加载降级。

新增摇物骨/链只在 prefab graft 与角色初始化阶段建立；已经初始化的活体热 ON 只刷新网格、
材质、骨绑定和碰撞体，改变 swing 结构后必须重新进入场景。

热 ON 后的颜色问题已于 2026-08-09 解决：真因是换空间时只搬顶点、没搬法线和切线，
活体角色有真实旋转就会让法线与几何脱节。

> **两条已证伪的旧结论**，不得再作为实现依据：
> 「Renderer 现有 PropertyBlock 的旧贴图优先于材质贴图」（游戏从不调用
> `SetPropertyBlock`）；「游戏在热重应用之后写回材质数组换掉私有材质」（抓帧确认暗色帧
> 与正常帧的身体 draw 逐字节相同）。基于它们的几轮修复改的都是没执行或没问题的路径。

量化证据与其余教训见 [`../../docs/lessons-learned.md`](../../docs/lessons-learned.md)，
当前边界与未解项见 [`../../docs/roadmap.md`](../../docs/roadmap.md)。

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
2. 用 `atbm-cstm-0140` 复验“当前无目标 Renderer → ON 排队 → 返回主页自动生效”；`hmsz` 样本
   已通过，不再重复算作未验；
3. 打开发型页，截图确认“月村手毬 · 公主皇冠”的官方预览图实际可见；
4. 实机触发启动时冲突组全关，以及运行中新 Mod 被占用者拒绝的两种提示；
5. 验证返回主页、重登、重复打开、16:9/16:10、空列表、长名称和大量条目；
6. 完成写入失败、API 不兼容、Manifest 异常和安全 Shutdown 降级测试。

每轮实机测试必须同时记录管理器/Runtime DLL 哈希、日志时间、操作顺序和可见结果。
