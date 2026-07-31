# M1：IL2CPP 签名矩阵

本文档记录管理器允许使用的游戏方法签名。管理器不依赖汉化插件；已存在于其他项目的调用仅作为独立复核资料。任何未标记为“已验证”的方法，不能直接用于 Release 构建。

## 已验证的 Master 路径

| 用途 | Assembly | Namespace / Class | Method | 实例/静态 | 返回值 | 参数 | 状态 |
|---|---|---|---|---|---|---|---|
| 取得服装 Master | `Assembly-CSharp.dll` | `Campus.Common.Master.MasterManager` | `get_CostumeMaster` | 静态 getter | `CostumeMaster*` | 无 | 已验证 |
| 遍历服装 | `Assembly-CSharp.dll` | `Campus.Common.Master.CostumeMaster` | `GetAllWithSortByKey` | 实例 | `List<Costume*>*` | `int sortType` | 已验证，泛型调用需实际 `MethodInfo*` |
| 服装 ID | `Assembly-CSharp.dll` | `Campus.Common.Proto.Client.Master.Costume` | `get_Id` | 实例 | `Il2CppString*` | 无 | 已验证 |
| 服装头部 ID | `Assembly-CSharp.dll` | `Campus.Common.Proto.Client.Master.Costume` | `get_CostumeHeadId` | 实例 | `Il2CppString*` | 无 | 已验证 |
| 默认头部 ID | `Assembly-CSharp.dll` | `Campus.Common.Proto.Client.Master.Costume` | `get_DefaultCostumeHeadId` | 实例 | `Il2CppString*` | 无 | 已验证 |
| 异色组 ID | `Assembly-CSharp.dll` | `Campus.Common.Proto.Client.Master.Costume` | `get_CostumeColorGroupId` | 实例 | `Il2CppString*` | 无 | 已验证 |
| 公开时间 | `Assembly-CSharp.dll` | `Campus.Common.Proto.Client.Master.Costume` | `get_ViewStartTime` | 实例 | `int64_t` | 无 | 已验证，需处理秒/毫秒 |

以上签名来自本地已有的独立 IL2CPP 调用样例，实际接入管理器前仍需在目标游戏版本上打印地址、返回对象非空，并完成一次主页场景验证。

## 发型路径

| 用途 | Assembly | Namespace / Class | Method | 状态 |
|---|---|---|---|---|
| 取得 `CostumeHeadMaster` | `Assembly-CSharp.dll` | `Campus.Common.Master.MasterManager` | `get_CostumeHeadMaster` | 待实机确认 |
| 遍历头部 Master | `Assembly-CSharp.dll` | `Campus.Common.Master.CostumeHeadMaster` | `GetAllWithSortByKey` | 待实机确认 |
| 头部 ID | `Assembly-CSharp.dll` | `Campus.Common.Proto.Client.Master.CostumeHead` | `get_Id` | 待实机确认 |
| 发型资源 ID | `Assembly-CSharp.dll` | `Campus.Common.Proto.Client.Master.CostumeHead` | `get_HairAssetId` | 待实机确认 |

发型目标必须优先用 `hairAssetId` 与官方发型格子验证，不能因为 source 名称格式相似就直接拼接显示名称。

## 原生 UI 路径

| 用途 | Assembly | Class / Method | 状态 | 下一步 |
|---|---|---|---|---|
| 主页菜单入口 | `Assembly-CSharp.dll` | 具体 Home menu Presenter/View | 待实机确认 | 注入空按钮并验证重复进入 |
| 页面层管理器 | `Assembly-CSharp.dll` | Sheet/ScreenLayerManager | 待实机确认 | 打开空白管理页面 |
| 服装列表 Cell | `Assembly-CSharp.dll` | `PhotographyCostumeSettingListItemModel` 及对应 View/Presenter | 类名已出现，完整绑定待确认 | 绑定一个官方 Costume |
| 发型列表 Cell | `Assembly-CSharp.dll` | CostumeHead/发型选择列表对应 Model/View | 待实机确认 | 绑定一个官方 CostumeHead |
| Toggle 事件 | `Assembly-CSharp.dll` | 原生 Toggle/Button 绑定方法 | 待实机确认 | 验证开关只写配置 |

## 调用约束

- `RuntimeInvoke` 必须与此表中的 managed 参数和返回值一致；
- 泛型方法必须取得实际运行时类型的方法信息，不能猜一个静态地址；
- Master、List、Cell、Sprite 只在 Unity 主线程访问；
- 签名缺失时只关闭管理器功能，不影响 `gakumas-mod-runtime` 的资源替换；
- 每次实机验证记录游戏版本、GameAssembly 时间戳、方法地址和结果，不提交游戏二进制或提取资产。
