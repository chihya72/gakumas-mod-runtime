# IL2CPP 签名矩阵

> 最后更新：2026-08-01 12:23

本表只服务当前游戏内 Mod 管理器。借卡、好友和网络 API 不属于当前实现范围，相关历史
研究不进入本表的 Release 调用路径。

状态含义：

- **A**：当前 DMM PC 实机日志和可见结果确认；
- **B**：当前 PC metadata/dump 已确认，尚未完成实机调用；
- **C**：跨平台导出或外部资料推断；
- **废弃**：已证实不适合当前实现。

未达到 A 的调用不能直接进入无降级保护的 Release UI 路径。

## 1. 当前已验证的菜单和最小面板路径

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

当前只依赖：

```text
40 = ClearCache
```

它只用于 `GetSubButton(40)` 获取一个稳定存在的副按钮模板。自定义按钮点击被插件消费，
不得继续转发模板的原始 `CampusButtonBase.OnClicked`。

## 3. RuntimeInvoke 调用层约束

当前 `src/CampusUiProbe.cpp` 已验证以下规则：

1. 方法按类、名称和参数数量精确解析，并跳过开放泛型；
2. 调用前检查 `MethodInfo` 的 compiled body；
3. `il2cpp_runtime_invoke` 的引用参数直接放对象指针，值类型参数传本地值地址；
4. 调用后检查 managed exception；
5. Master、Unity UI、GameObject 和 RectTransform 只在 Unity 主线程访问；
6. 入口和面板操作外层保留 SEH，错误只禁用管理器 UI；
7. `UniTask` 具有结构体返回 ABI，未经单独验证不能按普通指针返回函数 Hook。

`UnityResolve::Class::Get<Method>` 在参数类型不匹配时可能退回同名第一个方法，所以重要重载
仍使用当前的 `FindMethodInClass(name, argc)` 路径。

## 4. Master 与官方图标候选

以下是下一阶段需要验证的调用，不是当前可见 UI 已使用的路径。

### 4.1 服装

| 用途 | 类型/方法 | 状态 |
|---|---|---|
| 取得服装 Master | `Campus.Common.Master.MasterManager.get_CostumeMaster()` | B，旧样例曾调用 |
| 遍历服装 | `CostumeMaster.GetAllWithSortByKey(int)` | B，泛型实例化待验证 |
| 服装 ID | `Costume.get_Id()` | B |
| 服装头部 ID | `Costume.get_CostumeHeadId()` | B |
| 默认头部 ID | `Costume.get_DefaultCostumeHeadId()` | B |
| 异色组 ID | `Costume.get_CostumeColorGroupId()` | B |
| 公开时间 | `Costume.get_ViewStartTime()` | B，单位待确认 |

### 4.2 发型

| 用途 | 类型/方法 | 状态 |
|---|---|---|
| 取得头部 Master | `MasterManager.get_CostumeHeadMaster()` | B |
| 遍历头部 Master | `CostumeHeadMaster.GetAllWithSortByKey(int)` | B |
| 头部 ID | `CostumeHead.get_Id()` | B |
| 发型资源 ID | `CostumeHead.get_HairAssetId()` | B |

发型必须通过 `hairAssetId`、Costume 引用和游戏实际发型格子交叉确认，不能只根据 `source`
字符串拼接玩家名称。

## 5. 正式管理容器与列表候选

| 用途 | 候选 | 状态 |
|---|---|---|
| 正式页面层 | `ScreenLayerManager` / 简单 Screen 或 Sheet | B/C，尚未选定 |
| 服装格子 | 摄影或换装页面的 Cell/ItemModel/Presenter | B/C |
| 发型格子 | CostumeHead 选择页面的 Cell/ItemModel/Presenter | B/C |
| 分页 | 游戏现有 Tab 组件 | B/C |
| 开关 | 游戏现有 Toggle/CampusButton 组合 | B/C |

当前放大的 `MenuSubButtonView` 只是 M1 探针，不应继续扩展成最终长列表。

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
5. 再启用点击和面板；
6. 记录游戏版本、DLL 哈希、日志时间和可见截图；
7. 任一步失败都停止 UI 接入，不影响 Runtime 替换。
