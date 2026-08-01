# M1：独立 DLL 注入与 UI 实验状态

本文只记录当前管理器源码、日志和实机证据。没有日志或可复现实机结果的内容只标为
“推测”或“待验证”，不写成已完成。

## 1. 当前事实（2026-08-01）

### 已验证

- 游戏加载独立入口 `xinput9_1_0.dll`；
- 管理器从 `xinput1_3.dll` 取得 `GmrGetRuntimeApiV1`；
- Runtime API v1 握手成功，并读到约 1451 字节的 Mod 快照；
- `MenuPresenter.SetEvent` 和 `OnAfterInitialize` 曾在主线程进入；
- 11:23 的旧构建曾记录模板克隆、文字替换和“entry injected into MenuView”。

### 当前未验收

- 最新 11:37 构建记录 `commonView=0 subButtons=0 button=0`，没有再次出现入口注入；
- `CampusButtonBase.OnClicked` 的当前构建尚无完整“点击入口 → 自建面板”证据；
- 自建面板没有最新的 `panel created` 或可见截图证据；
- 服装/发型分页、官方图标、开关和重启状态尚未接入。

因此当前 M1 状态是：**DLL/API 已完成，UI 入口和页面仍在修复与验证中。**

## 2. 当前源码路线

`src/CampusUiProbe.cpp` 当前不是旧版公告 Sheet 探针，而是一个菜单和自建面板实验：

```text
Runtime API 握手成功
  → 启动 ProbeThread
  → UnityResolve 初始化并取得 il2cpp 导出
  → Hook MenuPresenter.SetEvent / OnAfterInitialize
  → 进入 SetEvent 时读取 MenuView
  → 克隆清除缓存模板并改成“Mod 管理”
  → Hook CampusButtonBase.OnClicked，按按钮指针识别入口
  → 在 Unity 主线程克隆一个面板对象
  → 把后台读取的 Mod 快照文本写入面板
```

当前实现没有把克隆体写入 `_subButtons`，也没有使用 `ErrorSheetManager`。
它依赖按钮本身的点击 Hook，而不是让菜单的分支逻辑认识新的菜单类型。

## 3. 当前构建的已知阻塞

源码中以下字段目前只是声明和读取，尚未在初始化阶段赋值：

- `g_commonViewField` 对应 `MenuPresenter._commonView`；
- `g_subButtonsField` 对应 `MenuView._subButtons`；
- `g_buttonField` 对应 `MenuButtonViewBase._button`。

这与 11:37 日志中的三个 `0` 一致，会使 `EnsureEntry()` 直接返回。因此在重新实机
测试前，必须先补齐字段解析并给 `menuView`、`buttonViewBase`、`g_menuPresenter` 加空指针
保护。修复后才重新判断入口是否稳定。

## 4. 旧路线与证据边界

### 已废弃：`Time.get_deltaTime` / `LateUpdate` 轮询

旧版通过 Unity 每帧入口轮询 `FindObjectsOfType`，再寻找
`HomeTopScreenPresenter` 并调用 `OpenNoticeSheetAsync`。它与页面生命周期无关，并曾导致
启动或进主页崩溃，已从当前实现路线删除。

### 已废弃：ErrorSheet

旧构建曾调用 `ErrorSheetManager.OpenAsync`，日志只证明 `OpenAsync returned`，没有证明
Sheet 可见、文本正确或能安全关闭。因此它不能写成“页面已实现”，当前文档不再把它作为
实现方案。

### 仍需验证：自建面板

当前自建面板只是一种最小功能验证，复用了按钮模板，不等于最终管理器页面。必须实机确认：

- 克隆对象是否真的可见；
- `SetCustomText` 是否能显示多行内容；
- RectTransform 尺寸和布局是否正确；
- 再次点击是否能隐藏；
- 离开主页后对象指针是否失效；
- 重复进入主页是否会重复注入。

## 5. 实机日志时间线

### 11:23 旧构建

```text
step 2 view=MenuView
step 3 template=MenuSubButtonView
step 6 clone=MenuSubButtonView
entry injected into MenuView
entry pressed; opening the Mod sheet
OpenAsync returned
```

这只能证明旧构建走到了这些代码路径，不能证明页面成功显示。

### 11:37 当前构建

```text
resolved commonView=0 subButtons=0 button=0
hooks installed; open the home menu
OnAfterInitialize on OutGameMenuPresenter
```

没有 `step 2`、`entry injected` 或 `panel created`，说明当前构建尚未达到旧构建的
入口注入阶段。

## 6. 验收标准

在以下日志和截图出现前，不把 M1 UI 标记为完成：

1. 字段解析全部为非零且经过类型/偏移检查；
2. 主页第一次进入只出现一个“Mod 管理”入口；
3. 点击入口出现自建面板；
4. 面板能显示 Mod 快照文本并能关闭；
5. 离开并重新进入主页不会重复注入；
6. 失败时只记录日志，不导致游戏崩溃；
7. 汉化插件是否安装不影响入口和 Runtime 握手。

## 7. 与借卡功能的关系

借卡库不是当前 Mod 管理器 M1 的验收项。借卡功能另见
`BORROW_LIBRARY_CASE_STUDY.md`，其中的好友列表、`publicUserId`、服务端 API host、
Campus gRPC client 和 `papi.proto` 需要单独验证，不能用“Mod 管理入口成功”替代。
