# 调查数据源与证据等级

> 最后更新：2026-08-05

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

输入：游戏安装目录下的 `gakumas_Data\il2cpp_data\Metadata\global-metadata.dat`。
路径不写死在仓库里，用环境变量指过去：

```powershell
$env:GKMS_METADATA = "<游戏目录>\gakumas_Data\il2cpp_data\Metadata\global-metadata.dat"
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

输入：一份 iOS 版的 Il2CppInspector 导出，目录里含 `il2cpp.json` 和 `dump.cs` 两个
文件。它是外部产物，不在本仓库内，路径不写死，用环境变量指过去：

```powershell
$env:GKMS_INSPECTOR = "<导出目录>\il2cpp.json"
```

仓库工具：

```powershell
python tools\inspector_index.py -c CampusButtonBase --raw
```

`dump.cs` 不经工具，直接手翻——它就在 `il2cpp.json` 旁边。

`dump.cs` 提供字段类型、继承关系、方法参数和偏移线索。它帮助确认：

```text
MenuPresenter._commonView                    0x58
MenuView._subButtons                        0x50
MenuButtonViewBase._button                  0x38
MenuButtonViewBase._text                    0x48
MenuButtonViewBase._buttonType              MenuButtonSerializeType
MenuView.GetSubButton 参数                   MenuButtonType
SettingTopScreenPresenter._tab               CampusSimpleTab
PreferenceTabView._vsyncToggleButton         SwitchButton
SwitchButton._button                         CampusButton
SwitchButton.SetIsOn(bool, bool)              原生开关状态设置
CampusSimpleTab.GetPage/GetButton             底部分页和页面
UnityEngine.UI.ScrollRect.get_content         设置页滚动内容
```

这些输入来自 iOS 3.2.0，不得直接当作 PC 地址或布局。上述四个偏移之所以升级为 A 级，是
因为 2026-08-01 12:23 的当前 PC 日志和可见 UI 又完成了运行时验证。

`dump.cs`、`il2cpp.json`、IDA 数据库和游戏 metadata 不提交到本仓库。接手者必须能在自己
的目标版本重新生成或重新索引，不能依赖文档中的绝对路径长期不变。

## 4. 实机日志与截图

日志：

```text
<游戏目录>\gakumas-mod\mod-manager.log
```

当前 A 级 UI 证据：

- 12:21:53：全部必需字段、方法和 RectTransform API 解析成功；
- 12:23:06：克隆 `MenuSubButtonView` 和 `CampusButton` 成功；
- 12:23:07：点击进入插件并创建面板；
- 12:23:10：同一面板成功隐藏和重新显示；
- 用户截图：历史 M1 面板实际显示“共 3 个 Mod”及三条名称/类别/状态文字；
- 18:20–18:21：用户截图确认真实 `SettingTopScreen` 已重构为“Mod 管理”，服装/发型分页、
  原生开关及当时版本的“重启后停用”文案均工作；开关后按启用状态换位和重复点击误进原设置页
  也被复现。该文案属于热开关改造前的历史证据，不代表当前切换仍要求重启；
- 18:21 后日志确认重复点击时 EventSystem 在 1 帧内误选旧 Mod Tab；活动 Tab 身份和 pending
  去重随后修复该竞态；
- 后续截图确认三个服装行均显示固定结构、游戏目标名称和官方服装缩略图；发型资源键归一化
  后也显示“月村手毬 · 公主皇冠”；
- 同轮 Manifest 与日志确认开关写回成功；输入回调后原生组件二次反转滑块的问题已通过
  `EventSystem.Update()` 原始处理后的连续校正确认修复；
- 发型日志确认 `CostumeHead.GetThumbAssetName()` 返回
  `img_cos_costume_head_ttmr-hair-0002_head`，并交给官方 `ThumbnailViewBase.Set`；当前页面最终
  图像是否可见仍需截图，所以只把“资源提交”标 A，不把“最终预览”标 A；
- 21:13–21:14 日志确认三种导航：Mod→Mod 消费重复点击并 `menuClosed=1`、系统设置→Mod
  原位复用并 `menuClosed=1`、Mod→系统设置调用 Reload 并 `menuClosed=1`；
- 同轮又确认 Reload 后 `MenuView` 地址不变，旧 `g_injectedViews` 记录让 `SetEventHook` 直接
  返回，导致菜单入口消失。最新源码在 Reload 成功后失效当前 View 缓存；该补丁只有本地
  构建证据，尚未升级为 A；
- 用户确认标准服装热开关已经生效，但热 ON 后直接返回主页颜色错误，进入一次换装页面后恢复。
  Runtime 日志同时记录 `targets=2, applied=2, refreshedRigs=1`，说明重应用成功而显示提交不完整；
- ~~源码检查确认现有每材质 `MaterialPropertyBlock` 的旧贴图覆盖了克隆材质。~~
  **该结论已于 2026-08-02 被实机证伪**：互补探针显示游戏在这些场景从不调用
  `Renderer.SetPropertyBlock`，`Material.SetTexture` 也从不写我们的材质。真实写入者是
  `Renderer.set_sharedMaterials`。首个托管 setter 钩子在 04:04 热切换日志中没有触发，
  因此 Runtime 曾加入 IDA 已确认的 `Renderer.SetMaterialArray_Injected` 底层钩子。该实验及
  后续受限扫描连续导致启动卡住或 ON/OFF 崩溃，现已撤回；当前部署恢复为 IDA 调查前的
  热切换基线，直接回主页的颜色缺陷仍通过进入一次换装页面恢复；
  真因与下一步见 `../../docs/roadmap.md`「唯一未解缺陷」。

IDA 进一步确认底层写入链：`VLActorFaceModel.UpdateSharedMaterials()`
（`0x0A88A6F4`）进入 `sub_7ABADF0`，最终调用
`UnityEngine.Renderer::SetMaterialArray_Injected`；`CampusActorModelParts.AddCombinedOpaqueSubMesh()`
（`0x03A06770`，调用点 `0x03A06DBC`）也通过 `sub_A380B70` 写入同一路径。橙条方面，
`CampusSimpleTabButtonGroup.Initialize()`（`0x02459A7C`）才是把 `GetBarSize()` 写入
`SelectedBarRect.sizeDelta` 的位置，`SetSelectIndex()` 只改位置。

12:23 截图中的菜单图标重叠只描述已废弃文字探针；18:20 之后的全屏截图才是当前 UI 证据。

## 5. 源码与实机结论的对应关系

| 结论 | 源码位置 | 证据 |
|---|---|---|
| Runtime API 握手 | `src/PluginMain.cpp` / `RuntimeClient.cpp` | 1451 字节快照日志 |
| 入口注入 | `CampusUiProbe.cpp::EnsureEntry` | 入口截图和 step 2–7 日志 |
| 点击识别 | `CampusUiProbe.cpp::PressHook` | `entry pressed` |
| 历史文字探针 | 旧 `CampusUiProbe.cpp::TogglePanel` / `BuildSheetBody` | 12:23 `panel created/hidden/shown` 和截图；已退出当前源码主路径 |
| JSON 与玩家模型 | `RuntimeModSnapshot.cpp` / `ModPresentationModel.cpp` | 当前全屏行、分类、稳定排序和 Master 文案可见；独立测试覆盖冲突文案 |
| 设置模板全屏页 | 当前 `CampusUiProbe.cpp::EventSystemUpdateHook` / `ComposeModScreen` | 18:20 用户截图确认标题、分页、滚动内容和开关行 |
| 开关写回 | `RuntimeClient::SetModEnabled` / `CampusUiProbe.cpp::HandleModToggle` | Runtime 与 Manifest 写回、帧末视觉校正均已实机确认 |
| 幂等导航 | `g_activeModTab` / `PressHook` / `ReloadActiveModScreenAsSettings` | 21:13–21:14 三个分支日志为 A；Reload 后入口缓存失效为 B，待复验 |
| 稳定排序 | `ModPresentationModel.cpp::SortItems` | 本地测试确认开关状态互换后顺序保持 `名称 + modId` |
| Master 目标与缩略图 | `ResolveGameTarget` / `AttachOfficialGameThumbnail` | 服装名称与缩略图为 A；发型改走同一条 `CostumeThumbnailView.Set(ICostume)`（master `CostumeHead` 实现 `ICostume`，dump.cs 确认），空态标签与长按详情为 B，待实机复验 |
| Runtime 热恢复 | `ModRuntime.cpp::SetModEnabled` / 热恢复扫描 | 用户确认 OFF/ON 生效，Runtime 日志记录重应用和 Rig 刷新 |
| 即时颜色提交 | `ModRuntime.cpp::RestorePatchedMaterials` | **未修复**。旧 MPB 覆盖结论已被实机证伪（`SetPropertyBlock` 从不触发）；真实写入者是游戏调用 `set_sharedMaterials`。见 `../../docs/roadmap.md` |

源码中存在、但没有对应实机结果的路径不得仅凭编译成功标为 A。

## 6. Campus 仓库的范围

参考仓库：<https://github.com/vertesan/campus>

它可作为 protobuf、Master、资源索引和游戏结构的背景资料，但当前 Mod 管理器：

- 不实现借卡库；
- 不调用好友、资料页或关注 API；
- 不导出、复制或重放账号 token；
- 不把 Campus gRPC 当作 Runtime Mod JSON 的来源。

借卡、好友、名片和 Campus 网络 API 不在产品范围内，相关早期调研已随文档整理删除
（需要时从 git 历史取）。除非产品范围被用户明确修改，否则不得把这类任务加入开发计划
或签名矩阵。

## 7. 调查原则

1. 能从当前 PC metadata 确认的成员不靠多次崩溃试错；
2. 每次实机只验证一组明确假设，并记录时间、DLL 哈希和可见结果；
3. iOS 地址、偏移和 ABI 只能作为线索，必须由 PC 重新验证；
4. 重要重载按名称和参数数量精确解析，并检查 compiled body；
5. UI 对象只在 Unity 主线程操作；
6. UI 失败只关闭管理器，不影响 `gakumas-mod-runtime`；
7. 不提交游戏二进制、提取资产、账号凭据或机器专用数据库。
8. “上一部署版日志确认分支执行”与“最新补丁已部署”分开记录，不能用前者替代后者的复验。

## 8. 更新文档时的规则

每次实机进展后必须同步检查：

- `README.md` 的当前阶段和下一步；
- `PROJECT_PLAN.md` 的里程碑、缺口、开放项和执行顺序；
- `UI_FLOW.md` 的源码流程、时间线和已知限制；
- `SIGNATURE_MATRIX.md` 的 A/B/C 状态；
- 本文的证据时间和来源。
- 相邻 Runtime 的 `README.md`、`docs/runtime-api-v1.md`、`docs/manifest-v2.md`、
  `docs/roadmap.md` 与渲染调查记录。

旧失败记录可以保留，但必须明确标为历史，不能继续使用“当前构建”描述旧版本。
