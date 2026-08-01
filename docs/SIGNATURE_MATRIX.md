# IL2CPP 签名矩阵

本文只记录当前版本允许继续调查的签名。状态含义：

- **A：当前 PC 实机日志/结果确认**；
- **B：当前 PC metadata 已确认，尚未完成调用验证**；
- **C：iOS 导出、截图或第三方仓库推断**；
- **废弃：曾经尝试但不再作为实现路线**。

未达到 A 的方法不能直接进入 Release UI 路径。

## 1. Master 路径

| 用途 | Assembly | 类型/方法 | 状态 |
|---|---|---|---|
| 取得服装 Master | `Assembly-CSharp.dll` / `Campus.Common.Master.MasterManager` | `get_CostumeMaster()` | B，旧调用样例曾验证 |
| 遍历服装 | `Campus.Common.Master.CostumeMaster` | `GetAllWithSortByKey(int)` | B，泛型实例化方式未定 |
| 服装 ID | `Campus.Common.Proto.Client.Master.Costume` | `get_Id()` | B |
| 服装头部 ID | `Campus.Common.Proto.Client.Master.Costume` | `get_CostumeHeadId()` | B |
| 默认头部 ID | `Campus.Common.Proto.Client.Master.Costume` | `get_DefaultCostumeHeadId()` | B |
| 异色组 ID | `Campus.Common.Proto.Client.Master.Costume` | `get_CostumeColorGroupId()` | B |
| 公开时间 | `Campus.Common.Proto.Client.Master.Costume` | `get_ViewStartTime()` | B，需确认单位 |

这些不是当前 UI 已完成的证据。接入前仍需在目标 PC 版本检查返回对象和主线程时机。

## 2. 发型路径

| 用途 | 类型/方法 | 状态 |
|---|---|---|
| 取得 `CostumeHeadMaster` | `Campus.Common.Master.MasterManager.get_CostumeHeadMaster()` | B |
| 遍历头部 Master | `CostumeHeadMaster.GetAllWithSortByKey(int)` | B |
| 头部 ID | `CostumeHead.get_Id()` | B |
| 发型资源 ID | `CostumeHead.get_HairAssetId()` | B |

发型显示必须优先通过 `hairAssetId` 和官方发型格子确认，不能仅凭 source 字符串拼名称。

## 3. 菜单 UI

| 用途 | Assembly / 类型 | 成员 | 状态 |
|---|---|---|---|
| 菜单生命周期 | `Assembly-CSharp.dll` / `Campus.Common.MenuPresenter` | `_commonView`、`SetEvent()`、`OnAfterInitialize()` | A：曾进入实机 Hook |
| 主页菜单 | `Campus.OutGame.OutGameMenuPresenter` | `OnSelected()`、`IsLock(MenuButtonType)` | B/A：方法和顺序有实机日志，当前入口未稳定 |
| 菜单 View | `Campus.Common.MenuView` | `_canvas`、`_buttons`、`_subButtons`、`GetSubButton()` | B；旧构建曾取得对象 |
| 菜单格子 | `Campus.Common.MenuButtonViewBase` | `_buttonType`、`_button`、`SetCustomText(String)` | B；旧构建曾克隆 |
| Campus 按钮 | `campus-submodule.Runtime.dll` / `Campus.Common.CampusButtonBase` | `OnClicked()` | B；类已定位，当前点击链未完整确认 |
| 页面层管理器 | `Assembly-CSharp.dll` / `ScreenLayerManager` | `OpenAsync(...)` | B/C，未接入 |

11:23 的旧构建有入口克隆日志；11:37 当前构建的三个字段解析值为 0，因此菜单入口
当前标记为“回归待修”，不能写成完成。

## 4. 类型区别：不能混用

```csharp
MenuView._buttons    : SerializableDictionary<MenuButtonSerializeType, MenuButtonView>
MenuView._subButtons : SerializableDictionary<MenuButtonSerializeType, MenuSubButtonView>
MenuButtonViewBase._buttonType : MenuButtonSerializeType
MenuPresenter.SelectedButtonType: MenuButtonType
```

- `MenuButtonType` 是整数枚举，用于选择分支和 `SelectedButtonType`；
- `MenuButtonSerializeType` 是对象类型，用于字段和字典键；
- 往 `_buttonType` 写整数会破坏对象引用；
- `GetSubButton` 的真实参数必须以当前 PC metadata 为准，不能从字段类型反推；
- 当前源码没有把克隆体写回 `_subButtons`，因此不能把“字典插入”写成已实现。

### 已由实机观察到的 `MenuButtonType` 值

以下只记录菜单调查用枚举，不代表可以随意把值写入对象字段：

| 值 | 名称 | 值 | 名称 | 值 | 名称 |
|---:|---|---:|---|---:|---|
| 0 | None | 15 | Help | 30 | SeminarRetire |
| 1 | Profile | 16 | Setting | 31 | Inquiry |
| 2 | Meishi | 17 | Title | 32 | TowerRankMission |
| 3 | Friend | 18 | Predownload | 33 | TowerHighScore |
| 4 | Guild | 19 | DataLinkage | 34 | TowerRetire |
| 5 | Achievement | 20 | Other | 35 | TowerRetry |
| 6 | Item | 21 | ProduceSchedule | 36 | PhotographyPrepare |
| 7 | Shop | 22 | ProduceLog | 37 | ProduceRanking |
| 8 | PictureBookProduceCollection | 23 | ProduceSetting | 38 | TourRetire |
| 9 | Costume | 24 | ProduceToHome | 39 | TourToHome |
| 10 | PictureBookIdolSelect | 25 | ProduceAchievement | 40 | ClearCache |
| 11 | PhotoAlbum | 26 | ProduceLiveCostume | 41 | CompetitionRetire |
| 12 | Music | 27 | ProduceHelp | 42 | CompetitionToHome |
| 13 | Roster | 28 | ProduceRetire | 43 | CompetitionUnitDetail |
| 14 | Media | 29 | SeminarRetry | | |

## 5. 借卡候选路径

以下来自 Campus 仓库、metadata 和第三方截图的交叉推断，全部为 C/B 级候选，尚未完成
当前 PC 调用验证：

| 目标 | 候选类型/方法 |
|---|---|
| 好友列表 | `Campus.OutGame.FriendTopScreenPresenter` |
| 用户资料 | `MoveToProfileOthersScreen` |
| 支援卡详情 | `MoveToSupportCardDetailScreen` |
| 回忆详情 | `MoveToMemoryDetailScreen` |
| 好友操作 | `FriendFollowAsync` / `FriendUnfollowAsync` |
| 列表数据 | `FriendListPresenter.SetItemModels` |
| 行模型 | `FriendListItemModel` |

这些候选不能因为截图相似就标成已实现。借卡研究详见 `BORROW_LIBRARY_CASE_STUDY.md`。

## 6. 调用约束

1. `Class::Get<Method>(name, args)` 参数类型匹配失败时可能返回第一个同名方法；重要调用
   必须按名字和参数个数精确解析，并检查 compiled body。
2. `RuntimeInvoke` 的引用类型参数不能一律按值类型取地址；调用点必须自己确认 params
   数组布局。
3. `UniTask` 返回值存在结构体返回 ABI，不能按普通 `void` 或单槽函数直接 Hook。
4. 泛型方法必须取得实际运行时类型的 `MethodInfo`；开放泛型不能直接调用。
5. Master、List、Cell、Sprite 和 Unity UI 对象只能在 Unity 主线程访问。
6. 签名缺失或对象为空时只关闭管理器 UI 功能，不能让游戏退出。
7. 每次 A 级实机验证记录游戏版本、GameAssembly 时间戳、日志时间和可见结果；不提交
   游戏二进制或提取资产。

## 7. 已废弃方法

- 通过 `Time.get_deltaTime` 或 `CampusActorController.LateUpdate` 轮询
  `FindObjectsOfType` 找 Presenter；
- 直接调用 `HomeTopScreenPresenter.OpenNoticeSheetAsync` 作为最终页面；
- 依赖 `ErrorSheetManager` 传入未经验证的托管委托；
- 把整数 `MenuButtonType` 写入 `MenuButtonSerializeType` 字段或字典。
