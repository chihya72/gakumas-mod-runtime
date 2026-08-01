# 调查数据源与证据等级

> 最后更新：2026-08-01 12:23

本文说明当前 Mod 管理器的签名从哪里来、哪些结论可以作为实现依据，以及接手者如何在
游戏更新后重新验证。外部绝对路径是当前调查机输入，不是仓库运行依赖。

## 1. 证据等级

| 等级 | 含义 | 能否进入当前实现 |
|---|---|---|
| A | 当前 DMM PC 日志与可见实机结果直接证明 | 可以，但仍需空值和版本降级 |
| B | 当前 PC metadata/dump 已确认成员，尚未调用 | 只能进入受控探针 |
| C | iOS 导出、外部仓库或第三方截图推断 | 不能直接进入 Release 路径 |
| D | 仅名称、外观或经验猜测 | 只能作为搜索线索 |

“方法返回”不等于“功能成功”。UI 必须有可见结果或明确生命周期日志；网络/异步方法必须
观察到完成或产生的页面状态。

## 2. 当前 PC metadata

输入：

```text
D:\Games\gakumas\gakumas_Data\il2cpp_data\Metadata\global-metadata.dat
```

仓库工具：

```powershell
python tools\metadata_index.py MenuView
python tools\metadata_index.py -m SetCustomText
python tools\metadata_index.py --selfcheck
```

用途：

- 搜索类、命名空间、字段和方法；
- 检查参数数量和 metadata 基本结构；
- 游戏更新后快速发现关键成员缺失。

限制：单独使用 metadata 不能证明方法体可调用，也不能证明页面生命周期正确。

## 3. Il2CppInspector 导出和 `dump.cs`

当前调查机资料：

```text
D:\GIT\gkms-localify-ios\workspace\3.2.0\inspector\il2cpp.json
D:\GIT\gkms-localify-ios\workspace\3.2.0\inspector\dump.cs
```

仓库工具：

```powershell
python tools\inspector_index.py -c CampusButtonBase --raw
```

`dump.cs` 提供字段类型、继承关系、方法参数和偏移线索。它帮助确认：

```text
MenuPresenter._commonView                    0x58
MenuView._subButtons                        0x50
MenuButtonViewBase._button                  0x38
MenuButtonViewBase._text                    0x48
MenuButtonViewBase._buttonType              MenuButtonSerializeType
MenuView.GetSubButton 参数                   MenuButtonType
```

这些输入来自 iOS 3.2.0，不得直接当作 PC 地址或布局。上述四个偏移之所以升级为 A 级，是
因为 2026-08-01 12:23 的当前 PC 日志和可见 UI 又完成了运行时验证。

`dump.cs`、`il2cpp.json`、IDA 数据库和游戏 metadata 不提交到本仓库。接手者必须能在自己
的目标版本重新生成或重新索引，不能依赖文档中的绝对路径长期不变。

## 4. 实机日志与截图

日志：

```text
D:\Games\gakumas\gakumas-local\mod-manager.log
```

当前 A 级 UI 证据：

- 12:21:53：全部必需字段、方法和 RectTransform API 解析成功；
- 12:23:06：克隆 `MenuSubButtonView` 和 `CampusButton` 成功；
- 12:23:07：点击进入插件并创建面板；
- 12:23:10：同一面板成功隐藏和重新显示；
- 用户截图：面板实际显示“共 3 个 Mod”及三条名称/类别/状态文字。

截图同时证明当前布局仍与原菜单图标重叠，所以它只能验证技术链路，不能证明最终 UI
设计完成。

## 5. 源码与实机结论的对应关系

| 结论 | 源码位置 | 证据 |
|---|---|---|
| Runtime API 握手 | `src/PluginMain.cpp` / `RuntimeClient.cpp` | 1451 字节快照日志 |
| 入口注入 | `CampusUiProbe.cpp::EnsureEntry` | 入口截图和 step 2–7 日志 |
| 点击识别 | `CampusUiProbe.cpp::PressHook` | `entry pressed` |
| 面板创建 | `CampusUiProbe.cpp::TogglePanel` | `panel created` 和截图 |
| 显隐 | 同上 | `panel hidden` / `panel shown` |
| 文本 JSON 映射 | `CampusUiProbe.cpp::BuildSheetBody` | 3 条 Mod 文本可见 |

源码中存在、但没有对应实机结果的路径不得仅凭编译成功标为 A。

## 6. Campus 仓库的范围

参考仓库：<https://github.com/vertesan/campus>

它可作为 protobuf、Master、资源索引和游戏结构的背景资料，但当前 Mod 管理器：

- 不实现借卡库；
- 不调用好友、资料页或关注 API；
- 不导出、复制或重放账号 token；
- 不把 Campus gRPC 当作 Runtime Mod JSON 的来源。

早期借卡讨论保存在 `BORROW_LIBRARY_CASE_STUDY.md`，只作为历史案例。除非产品范围被用户
明确修改，否则不得把其中任务加入当前开发计划或签名矩阵。

## 7. 调查原则

1. 能从当前 PC metadata 确认的成员不靠多次崩溃试错；
2. 每次实机只验证一组明确假设，并记录时间、DLL 哈希和可见结果；
3. iOS 地址、偏移和 ABI 只能作为线索，必须由 PC 重新验证；
4. 重要重载按名称和参数数量精确解析，并检查 compiled body；
5. UI 对象只在 Unity 主线程操作；
6. UI 失败只关闭管理器，不影响 `gakumas-mod-runtime`；
7. 不提交游戏二进制、提取资产、账号凭据或机器专用数据库。

## 8. 更新文档时的规则

每次实机进展后必须同步检查：

- `README.md` 的当前阶段和下一步；
- `PROJECT_PLAN.md` 的里程碑、缺口、开放项和执行顺序；
- `UI_FLOW.md` 的源码流程、时间线和已知限制；
- `SIGNATURE_MATRIX.md` 的 A/B/C 状态；
- 本文的证据时间和来源。

旧失败记录可以保留，但必须明确标为历史，不能继续使用“当前构建”描述旧版本。
