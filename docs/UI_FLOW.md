# M1：独立 DLL、菜单入口与最小 UI 状态

> 最后更新：2026-08-01 12:23
>
> 本文是当前 UI 实现的接手文档。只把当前 PC 日志和可见实机结果写成“已验证”；规划中
> 尚未实现的正式 Screen、列表、图标和开关单独列出。

## 1. 当前结论

M1 的核心链路已经通过实机验证：独立 DLL 加载、Runtime API 握手、主页入口注入、点击
识别、文本面板创建、Runtime 快照显示以及显隐切换均已成功。

当前 UI 仍是覆盖在主页菜单上的验证面板，不是最终管理器 Screen。它用于证明游戏 UI
调用层和 Runtime 数据通路可用，下一阶段必须更换正式容器和列表结构。

## 2. 已验证版本

```text
入口 DLL：xinput9_1_0.dll
构建时间：2026-08-01 12:21:11
大小：196608 字节
SHA-256：4AEFBEE12BC11509ACCFF62BA660D387AD395FA947B69AD0D9ECA19E82F7DBD5
日志：D:\Games\gakumas\gakumas-local\mod-manager.log
```

实机可见结果：

- 主页菜单副按钮区域出现“Mod 管理”；
- 点击后在菜单中央显示“共 3 个 Mod”和三条 Mod 状态；
- 再次点击隐藏，再次点击重新显示；
- 游戏没有卡死或崩溃；
- 按钮保留游戏原有按压动画。

## 3. 最新日志证据

### 3.1 符号和字段解析

```text
12:21:53.962 resolved commonView=1 subButtons=1 button=1 text=1
               getSubButton=1 customText=1 clone=1 transform=1 parent=1
               canvas=1 gameObject=1 setActive=1 sizeDelta=1 anchoredPos=1
               anchors=1/1 pivot=1 scale=1 sibling=1
12:21:53.962 field offsets commonView=0x58 subButtons=0x50 button=0x38 text=0x48
```

这些值与当前 PC 实际调用结果一致，当前构建不再存在 11:37 的字段未赋值回归。

### 3.2 入口注入

```text
12:23:06.869 step 2 view=MenuView dictionary=SerializableDictionary`2
12:23:06.871 step 3 template=MenuSubButtonView
12:23:06.871 canvas=CampusCanvas transform=RectTransform
12:23:06.873 step 6 clone=MenuSubButtonView
12:23:06.873 step 7 button=CampusButton
12:23:06.873 entry injected into MenuView
12:23:06.900 OnAfterInitialize on OutGameMenuPresenter
```

### 3.3 点击与面板生命周期

```text
12:23:07.646 entry pressed
12:23:07.647 panel created (MenuSubButtonView), object=GameObject
12:23:10.079 entry pressed
12:23:10.081 panel hidden
12:23:10.580 entry pressed
12:23:10.581 panel shown
```

日志和截图共同证明对象不是“只创建未显示”，而是实际可见并能切换显隐。

### 3.4 当前日志中的两个已知歧义

- `PluginMain.cpp` 仍输出 `UI hook is intentionally disabled`，但同一成功分支随后实际调用
  `StartCampusUiProbe()`；这是过期文案，不代表 UI 被禁用；
- 同一日志中出现了两组 `xinput9_1_0.dll loaded`，其中一组成功连接 Runtime，另一组最终
  超时。当前日志没有 PID/进程名，尚不能确认是启动链中的多个进程还是多个模块实例。

判断当前 UI 是否启用，应以同一时间段后的 `hooks installed`、`entry injected` 和
`panel created` 为准。下一次代码清理应修正文案，并给每条日志增加 PID/进程名。

## 4. 当前源码流程

入口文件：`src/CampusUiProbe.cpp`。

```text
PluginMain.BootstrapThread
  → RuntimeClient.Connect / IsReady
  → Runtime API v1 获取一次 1451 字节快照
  → StartCampusUiProbe
  → ProbeThread 等待 GameAssembly.dll
  → UnityResolve 初始化并附加线程
  → 精确解析字段、方法和 Unity UI API
  → Hook MenuPresenter.SetEvent / OnAfterInitialize
  → Hook CampusButtonBase.OnClicked

进入主页菜单
  → SetEventHook
  → EnsureEntry 读取 MenuPresenter._commonView
  → MenuView.GetSubButton(ClearCache) 取得模板
  → Object.Internal_CloneSingleWithParent 克隆入口
  → 读取克隆体 MenuButtonViewBase._button
  → SetCustomText("Mod 管理")

点击入口
  → PressHook 按 CampusButton 对象地址识别自定义入口
  → TogglePanel
  → 首次点击克隆第二个 MenuSubButtonView 作为文本面板
  → 设置根节点与文字节点 RectTransform
  → SetCustomText 写入 Runtime 快照摘要
  → 后续点击调用 GameObject.SetActive 切换显隐
```

Runtime JSON 在工作线程解析；Unity 对象的克隆、文字设置和 RectTransform 操作发生在游戏
主线程的点击回调中。

## 5. 关键安全决策

### 5.1 不写 `_subButtons` 字典

`_subButtons` 的键是 `MenuButtonSerializeType` 对象，不是整数 `MenuButtonType`。当前实现
只读取字典用于验证，不插入新键。入口通过克隆的 `CampusButton` 对象地址识别。

### 5.2 消费自定义入口点击

入口模板来自 `MenuButtonType.ClearCache`。当 `PressHook` 识别到自定义按钮时，处理完面板
后直接返回，不再调用原始 `CampusButtonBase.OnClicked`，避免触发模板的清缓存行为。

### 5.3 只有完整成功才登记 View

`g_injectedViews` 只在克隆和 `_button` 读取全部成功后登记。失败不会永久阻止同一 View
重试；克隆体缺少按钮时会被禁用，并停止本次会话继续注入。

### 5.4 失效安全

- 必需类、字段、方法或偏移缺失时不安装 UI Hook；
- 入口创建和面板切换有 SEH 边界；
- UI 失败不修改 Runtime 的 AssetBundle 替换状态；
- 不自动启动、关闭或控制游戏。

## 6. 当前面板的明确限制

- 面板本质仍是放大的 `MenuSubButtonView`；
- 它覆盖在原菜单格子上，文字与原图标发生视觉重叠；
- 没有独立遮罩、背景、滚动容器和正式关闭按钮；
- 没有进入游戏 Screen/Sheet 返回栈；
- 同一入口同时承担“打开”和“隐藏”；
- 没有正式列表 Cell、服装/发型分页、官方图标和开关；
- 当前文本在探针启动时生成，不会自动反映页面打开后的外部 Manifest 修改；
- 启动日志尚未包含 PID，多个加载序列混在同一文件中；
- Bootstrap 的“UI hook intentionally disabled”文字与当前行为不符；
- `g_injectedViews` 使用对象地址去重，地址复用只做了保守假设；
- 尚未验证离开主页后再进入、重登、长时间运行及多种窗口尺寸。

这些限制意味着 M1 核心验证成功，但不能把当前画面包装成 MVP。

## 7. 历史失败路线

以下路线已经废弃，不应重新接回当前实现：

- 使用 `Time.get_deltaTime` / `CampusActorController.LateUpdate` 每帧搜索 Presenter；
- 直接调用 `HomeTopScreenPresenter.OpenNoticeSheetAsync`；
- 借用 `ErrorSheetManager.OpenAsync` 并传入未经验证的托管委托；
- 把整数 `MenuButtonType` 写入 `MenuButtonSerializeType` 字段或字典；
- 点击自定义入口后继续转发 ClearCache 模板的原始 `OnClicked`。

历史时间线：

- 11:23：旧构建能克隆入口，但 `OpenAsync returned` 没有可见页面证据；
- 11:37：字段指针未赋值，日志为 `commonView=0 subButtons=0 button=0`；
- 12:17：字段回归修复，入口和点击成功，面板对象创建但锚点导致不可见；
- 12:23：补齐锚点、文字区域、缩放和层级后，面板实机可见。

## 8. 尚未完成的验收

M1 后续生命周期验收：

1. 离开主页再返回，不重复创建入口；
2. 连续打开/关闭 20 次，不残留或崩溃；
3. 登出重新登录后重新绑定新 View；
4. 16:9、16:10 和窗口模式下均能正确定位；
5. Runtime API 不存在或版本错误时不显示入口；
6. 汉化插件安装与否均不影响管理器。

MVP 尚需：

1. 独立、可关闭且不覆盖原菜单内容的正式管理容器；
2. 每次打开时刷新 Runtime 快照；
3. Presentation Model 和服装/发型分栏；
4. Master 名称解析与官方图标；
5. 开关写回和重启提示；
6. 异常、冲突和长列表处理。

诊断清理还需：修正 Bootstrap 过期日志文字，并在日志前缀加入 PID/进程名。

## 9. 接手后的第一项开发

不要继续美化当前放大按钮。下一步应先确定正式容器：

1. 复用一个生命周期简单、可关闭的原生 Screen/Sheet 容器，或构建独立 Canvas 子树；
2. 保留当前已验证的菜单入口和 `CampusButtonBase.OnClicked` 识别链；
3. 将 `BuildSheetBody()` 拆成 Runtime 快照读取、Presentation Model 和 View 绑定三层；
4. 先用文本列表完成打开、关闭、返回和刷新，再接官方 Cell 与图标。

如新容器失败，应回退到当前可见验证面板，而不是恢复 ErrorSheet 或字典插入路线。
