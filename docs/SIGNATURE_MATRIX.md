# IL2CPP 签名矩阵

> 最后更新：2026-08-01 21:30

本表只服务当前游戏内 Mod 管理器。借卡、好友和网络 API 不属于当前实现范围，相关历史
研究不进入本表的 Release 调用路径。

状态含义：

- **A**：当前 DMM PC 实机日志和可见结果确认；
- **B**：当前 PC metadata/dump 已确认，尚未完成实机调用；
- **C**：跨平台导出或外部资料推断；
- **废弃**：已证实不适合当前实现。

未达到 A 的调用不能直接进入无降级保护的 Release UI 路径。

## 1. 当前已验证的菜单与全屏页面路径

### 1.1 游戏类和成员

| 用途 | Assembly / 类型 | 成员 | 当前 PC 证据 | 状态 |
|---|---|---|---|---|
| 菜单生命周期 | `Assembly-CSharp.dll` / `Campus.Common.MenuPresenter` | `SetEvent()` | Hook 进入并完成入口注入 | A |
| 菜单初始化结束 | 同上 | `OnAfterInitialize()` | `OutGameMenuPresenter` 日志 | A |
| 菜单 View | 同上 | `_commonView`，偏移 `0x58` | 实机解析非空，类型为 `MenuView` | A |
| 主页菜单 | `Campus.OutGame.OutGameMenuPresenter` | 继承 `MenuPresenter` | 主页可见入口 | A |
| 副按钮模板 | `Campus.Common.MenuView` | `GetSubButton(MenuButtonType)` | 值 40 返回 `MenuSubButtonView` | A |
| 菜单 Canvas | 同上 | `get_Canvas()` | 返回 `CampusCanvas` | A |
| 副按钮字典 | 同上 | `_subButtons`，偏移 `0x50` | 实机解析为 `SerializableDictionary` | A，只读 |
| 按钮组件 | `Campus.Common.MenuButtonViewBase` | `_button`，偏移 `0x38` | 克隆体返回 `CampusButton` | A |
| 文字组件 | 同上 | `_text`，偏移 `0x48` | 面板文字实机可见 | A |
| 自定义文字 | 同上 | `SetCustomText(String)` | “Mod 管理”和快照文本可见 | A |
| 点击入口 | `campus-submodule.Runtime.dll` / `Campus.Common.CampusButtonBase` | `OnClicked()` | 多次记录 `entry pressed` | A |
| 关闭菜单层 | `MenuView._closeButton` / `CampusButtonBase.OnClicked()` | 重复 Mod 导航和原位页面切换均记录 `menuClosed=1` | A |

### 1.2 Unity UI 方法

| 用途 | 类型/方法 | 实机结果 | 状态 |
|---|---|---|---|
| 带父节点克隆 | `UnityEngine.Object.Internal_CloneSingleWithParent(Object, Transform, bool)` | 克隆入口和面板 | A |
| Component Transform | `Component.get_transform()` | 返回 `RectTransform` | A |
| Component GameObject | `Component.get_gameObject()` | 返回面板 `GameObject` | A |
| 读取父节点 | `Transform.get_parent()` | 返回副按钮网格父节点 | A |
| 显隐 | `GameObject.SetActive(bool)` | 面板隐藏/重新显示 | A |
| 尺寸 | `RectTransform.set_sizeDelta(Vector2)` | 面板和文字区域生效 | A |
| 位置 | `RectTransform.set_anchoredPosition(Vector2)` | 面板居中可见 | A |
| 锚点 | `RectTransform.set_anchorMin/Max(Vector2)` | 修复不可见问题 | A |
| 轴心 | `RectTransform.set_pivot(Vector2)` | 当前面板可见 | A |
| 缩放 | `Transform.set_localScale(Vector3)` | 当前面板可见 | A |
| 层级 | `Transform.SetAsLastSibling()` | 当前面板显示在菜单内容之上 | A |

最新实机解析日志：

```text
commonView=0x58
subButtons=0x50
button=0x38
text=0x48
```

偏移必须继续由运行时 metadata 解析，不能因为当前值已验证就硬编码。

### 1.3 当前源码的设置模板全屏页

下列成员已由当前 PC metadata 确认并进入带空值检查、compiled-body 检查和 SEH 的受控路径；
18:20 起已取得全屏组合、固定行、分页、逐项开关和 Master 文案截图：

| 用途 | 类型/成员 | 当前源码行为 | 状态 |
|---|---|---|---|
| 原生导航 | `MenuPresenter.set_SelectedButtonType(Setting)` + `OutGameMenuPresenter.OnSelected()` | 走 Presenter 的原生设置分支，保留窗口返回栈 | A，18:12 已确认打开原设置页 |
| 接管时点 | `EventSystem.Update()` + 活跃 `CampusSimpleTab` 第一页为 `PreferenceTabPage` | 在主线程确认设置 Tab 已初始化后，仅在 pending 时重构 | A，18:20 已确认；`SettingTopScreenPresenter.SetEvent()` 已实机证伪为唯一 Hook 点 |
| 页面标题 | `OutGameScreenViewBase._overlayTitleView` / `OverlayTitleView.SetTitle(String)` | 标题改为“Mod 管理” | A |
| 底部分页 | `SettingTopScreenPresenter._tab` / `CampusSimpleTab` | 前两页改为服装/发型，隐藏第三页并禁用 Flick | A |
| 页签文字与宽度 | `CampusSimpleTabButton.SetText/SetForceWidth` | 两项各占一半目标宽度 | A |
| 页面容器 | `TabBase.GetPage(int)`、`ScrollRect.get_content()` | 取得两个原生可滚动页 | A |
| 行模板 | `PreferenceTabPage._view` → `PreferenceTabView._vsyncToggleButton` | 向上定位垂直同步设置块并复制为固定 Mod Cell | A，服装/发型截图确认 |
| 原生开关 | `SwitchButton._button/SetIsOn/SetDisabled` | 按内部按钮地址绑定 `modId`；原始输入结束后以 Runtime 配置校正视觉 | A，写回与帧末校正均确认 |
| 原生文字 | `CampusText.set_text(String)` | 写入 Mod 名称、目标说明、状态和空状态 | A，服装/发型 Master 文案已确认 |
| 列表重排 | `Transform.GetChild`、`LayoutRebuilder.ForceRebuildLayoutImmediate` | 隐藏原设置块并重建布局 | A |
| Mod→Mod | 活动 Mod `CampusSimpleTab` 身份 + `MenuView._closeButton` | 消费重复入口，只关闭菜单层 | A，21:14 日志 |
| 设置→Mod | `FindActiveSettingTab()` + `ComposeModScreen()` | 原位复用系统设置页，不压入第二个 Setting 页面 | A，21:13 日志 |
| Mod→设置 | `SettingTopScreenPresenter.Campus.ICampusScreen.Reload()` | 原位重载干净设置页并关闭菜单 | A，21:13 日志 |
| Reload 后入口重建 | `g_injectedViews.erase(g_menuViewInstance)` | 同地址 `MenuView` 的新生命周期允许重新注入 | B，已部署待实机复验 |

表中 B 项只有在 Reload 后重新打开菜单并看到唯一“Mod 管理”入口后才能升级为 A。

## 2. 两种菜单类型不能混用

```csharp
MenuView._buttons:
  SerializableDictionary<MenuButtonSerializeType, MenuButtonView>

MenuView._subButtons:
  SerializableDictionary<MenuButtonSerializeType, MenuSubButtonView>

MenuButtonViewBase._buttonType:
  MenuButtonSerializeType

MenuPresenter.SelectedButtonType:
  MenuButtonType

MenuView.GetSubButton(...):
  MenuButtonType
```

- `MenuButtonType` 是整数枚举，用于 `GetSubButton`、选择分支和 `SelectedButtonType`；
- `MenuButtonSerializeType` 是对象类型，用于序列化字段和字典键；
- 当前实现不修改 `_buttonType`，也不向 `_subButtons` 插入新键；
- 把整数写入 `MenuButtonSerializeType` 对象位置会破坏引用并可能导致游戏崩溃。

### 当前使用的 `MenuButtonType`

当前依赖：

```text
16 = Setting
40 = ClearCache
```

`ClearCache` 只用于获取稳定存在的副按钮模板；`Setting` 的真实按钮只用于确认当前菜单具备
该路由。自定义按钮点击不得转发模板自身的原始 `OnClicked`，也不能伪调用真实设置按钮的
`CampusButtonBase.OnClicked`：18:06 实机日志确认后者返回但不会触发导航。当前实现必须写入
`SelectedButtonType=Setting` 后调用当前 `OutGameMenuPresenter.OnSelected()`。

## 3. RuntimeInvoke 调用层约束

当前 `src/CampusUiProbe.cpp` 已验证以下规则：

1. 方法按类、名称和参数数量精确解析，并跳过开放泛型；
2. 调用前检查 `MethodInfo` 的 compiled body；
3. `il2cpp_runtime_invoke` 的引用参数直接放对象指针，值类型参数传本地值地址；
4. 调用后检查 managed exception；
5. Master、Unity UI、GameObject 和 RectTransform 只在 Unity 主线程访问；
6. 入口和全屏页面组合外层保留 SEH，错误只禁用管理器 UI；
7. `UniTask` 具有结构体返回 ABI，未经单独验证不能按普通指针返回函数 Hook。

`UnityResolve::Class::Get<Method>` 在参数类型不匹配时可能退回同名第一个方法，所以重要重载
仍使用当前的 `FindMethodInClass(name, argc)` 路径。

## 4. Master 与官方图标候选

以下调用已进入当前源码；服装固定 Cell 和发型名称已取得实机截图，发型预览最终可见仍待复验。

### 4.1 服装

| 用途 | 类型/方法 | 状态 |
|---|---|---|
| 取得服装 Master | `Campus.Common.Master.MasterManager.get_CostumeMaster()` | A，日志确认 505 项 |
| 遍历服装 | `CostumeMaster.GetAllWithSortByKey(int)` | A |
| 服装 ID | `Costume.get_Id()` | A |
| 服装名称/角色 | `Costume.get_Name()` / `Costume.GetCharacter()` / `Character.get_Name()` | A，三个目标截图确认 |
| 服装头部 ID | `Costume.get_CostumeHeadId()` | B |
| 默认头部 ID | `Costume.get_DefaultCostumeHeadId()` | B |
| 异色组 ID | `Costume.get_CostumeColorGroupId()` | B |
| 公开时间 | `Costume.get_ViewStartTime()` | B，单位待确认 |

### 4.2 发型

| 用途 | 类型/方法 | 状态 |
|---|---|---|
| 取得头部 Master | `MasterManager.get_CostumeHeadMaster()` | A，日志确认 384 项 |
| 遍历头部 Master | `CostumeHeadMaster.GetAllWithSortByKey(int)` | A |
| 头部 ID | `CostumeHead.get_Id()` | A，当前 Master 解析链使用 |
| 发型资源 ID | `CostumeHead.get_HairAssetId()` | A，归一化后命中 `mdl_chr_ttmr-hair-0002_hair` |
| 发型名称/角色 | `CostumeHead.get_Name()` / `CostumeHead.GetCharacter()` | A，截图确认“月村手毬 · 公主皇冠” |
| 发型预览资源 | `CostumeHead.GetThumbAssetName()` | A/B：实机日志返回 `img_cos_costume_head_ttmr-hair-0002_head` 并交给 `ThumbnailViewBase.Set`；最终图像可见待确认 |

发型必须通过 `hairAssetId`、Costume 引用和游戏实际发型格子交叉确认，不能只根据 `source`
字符串拼接玩家名称。

## 5. 正式管理容器与列表候选

| 用途 | 候选 | 状态 |
|---|---|---|
| 正式页面层 | 真实 Setting 导航创建的 `SettingWindow` / `SettingTopScreen` 实例 | A，全屏截图及三种页面切换日志确认 |
| 当前内容容器 | 设置页 `CampusSimpleTab` + `ScrollRect` | A，18:20 截图确认 |
| 固定 Mod Cell | 设置行表面 + 缩略图区 + 两段文字 + `SwitchButton` | A，服装与发型页截图确认结构 |
| 官方缩略图 | 服装：`CostumeListCellThumbnailView.Set`；发型：`CostumeHead.GetThumbAssetName` + `ThumbnailViewBase.Set` | A：服装；A/B：发型资源提交有日志、最终图像待确认 |
| 服装完整格子 | 摄影或换装页面的 Cell/ItemModel/Presenter | B/C，必要时的下一层方案 |
| 发型完整格子 | CostumeHead 选择页面的 Cell/ItemModel/Presenter | B/C，必要时的下一层方案 |
| 分页 | `CampusSimpleTab` / `CampusSimpleTabButton` | A |
| 开关 | `Campus.Common.SwitchButton` | A，Runtime 写回已确认 |

12:23 已验证的放大 `MenuSubButtonView` 只是 M1 探针；随后实现的主页主卡片 Canvas 面板也已
撤出点击主路径。当前入口只打开设置模板全屏页。

## 6. 已废弃路径

- `Time.get_deltaTime` / `CampusActorController.LateUpdate` 每帧轮询 Presenter；
- `FindObjectsOfType` 搜索主页对象后直接调用异步 Sheet；
- `HomeTopScreenPresenter.OpenNoticeSheetAsync` 作为最终页面；
- `ErrorSheetManager.OpenAsync` 加未经验证的托管委托；
- 向 `_subButtons` 写整数键或直接调用泛型字典 `Add`；
- 修改 `_buttonType` 为整数 `MenuButtonType`；
- 自定义入口处理后继续调用 ClearCache 模板的原始点击逻辑。

## 7. 版本更新复核清单

游戏更新后按以下顺序复核：

1. 运行 `tools/metadata_index.py --selfcheck`；
2. 确认四个字段存在、类型匹配且偏移为非负值；
3. 确认上表所有 A 级方法存在 compiled body；
4. 只启用入口注入，确认主页出现单一按钮；
5. 再启用点击、全屏页面和三种幂等导航；
6. 记录游戏版本、DLL 哈希、日志时间和可见截图；
7. 执行 Mod→设置→菜单，确认 Reload 后入口重新注入且不重复；
8. 任一步失败都停止对应 UI 能力，不影响 Runtime 替换。
