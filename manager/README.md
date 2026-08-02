# Gakumas In-Game Mod Manager

学园偶像大师 DMM Windows 版的独立游戏内 AssetBundle Mod 管理器。

管理器是本仓库 `xinput1_3.dll` 的一部分，由 Runtime 在自己的初始化线程末尾调用
`GkmmInitialize()` 启动，通过 Runtime API v1 读取 Mod 目录和写入启用状态。它不使用
汉化插件注入，不替换 `version.dll`，也不承担 Mesh、材质或 AssetBundle 替换工作。

早期版本是独立的 `xinput9_1_0.dll`，靠 `GetModuleHandleW` 找 Runtime 并轮询最多 60 秒
等它就绪。合并进同一个 DLL 后加载顺序和就绪竞争都不存在，那套握手已删除；
`GmrGetRuntimeApiV1` 导出保留，仍是 UI 与 Runtime 之间唯一的接口。

## 当前状态

截至 2026-08-01，以下主链已经在目标游戏中完成实机验证：

```text
xinput1_3.dll 自动加载
  → Runtime 初始化完成后调用 GkmmInitialize
  → 读取 1451 字节 Runtime Mod 快照
  → Hook MenuPresenter.SetEvent / CampusButtonBase.OnClicked
  → 在主页菜单克隆“MOD 管理”入口
  → 点击入口进入游戏原生 SettingWindow 返回栈
  → 页面重构为“Mod 管理”及“服装 / 发型”分页
  → 原生 SwitchButton 显示 Mod、写回状态并热更新当前会话
```

当前实机截图和日志已经证明：

- 主页菜单会出现且只观察到一个“Mod 管理”入口；
- 点击按钮不会卡死，点击动画和插件点击 Hook 均正常；
- Runtime JSON 已被解析成稳定 `modId`、玩家文案和冲突状态；
- 全屏管理页、服装/发型分页、滚动区域和原生开关可见；
- 固定 Mod 行、服装官方缩略图、服装/发型 Master 名称已经可见；
- 开关写回成功，页面状态和 Runtime 当前会话会立即刷新；
- 管理器与 Mod Runtime 同处 `xinput1_3.dll`，与汉化 `version.dll` 保持独立。

12:23 文字探针的历史实机基线：

```text
build\bin\x64\Release\xinput9_1_0.dll
大小：196608 字节
SHA-256：4AEFBEE12BC11509ACCFF62BA660D387AD395FA947B69AD0D9ECA19E82F7DBD5
```

18:20–18:21 全屏页、分页和开关写回对应的 DLL：

```text
大小：240640 字节
SHA-256：221760876D257C45B37B22C10390BF9DCD6E197050E4D7BA0F4F3CBDD4377864
```

在上述实机基线上，当前源码和部署版还包括：

- Runtime JSON 解析已从 `CampusUiProbe.cpp` 拆到独立快照模块；
- Presentation Model 保留稳定 `modId`，并生成服装/发型、自动互斥冲突和配置异常状态；
- 从普通页面进入“Mod 管理”时写入 `SelectedButtonType=Setting` 并调用 `OutGameMenuPresenter.OnSelected()`，由游戏创建新的 `SettingTopScreen` 窗口实例；若当前已经是系统设置页则原位复用，不再重复压入同类页面；
- 只对这次导航重构设置页：标题改为“Mod 管理”，底部改为“服装 / 发型”两个分页；
- 页面复用设置页的 `ScrollRect`、分页控件和“垂直同步”原生 `SwitchButton` 行；
- 每个 Mod 都保留稳定 `modId`，开关通过 Runtime API v1 写回并在成功后重新读取快照；
- 开关会热更新 Runtime 当前会话并写回 Manifest；写入失败时恢复滑块并显示错误；
- 同一服装或发型只允许一个 Mod 开启；已有 Mod 开启时拒绝新项并显示占用者，启动时若多项冲突则将冲突组全部关闭；
- Release x64 构建和独立 Presentation Model 测试已通过；
- 日志前缀已加入 PID/进程名，Runtime 模块只接受 `xinput1_3.dll`；
- 共享 UI 类型按完整托管名称跨程序集解析；全屏页能力与主页入口 Hook 已解耦，页面符号失败不再删除入口。
- 18:12 实机确认 Presenter 路径会打开原设置页，但 `SettingTopScreenPresenter.SetEvent()` 未实际经过 Hook；当前版本改由主线程 `EventSystem.Update()` 发现已初始化的设置 Tab 后执行重构。
- 18:20–18:21 实机确认全屏组合、分页和逐项开关可用，同时暴露了重复点击误进设置页、开关后按启用状态换位以及临时行缺少目标信息的问题；当前源码已修复重复导航和稳定排序，并接入 Master 目标名称及正式固定行结构。
- 固定行优先克隆游戏已加载的官方服装缩略图组件；组件尚未进入当前资源上下文时使用游戏字体的“服 / 发”占位，避免退回纯文字布局。
- 后续截图已确认固定行、服装 Master 名称和官方服装缩略图可见；同时确认开关写入成功后视觉状态会被原生组件二次反转，发型的完整资源名未能按逻辑 ID 命中 `CostumeHead`。
- 当前源码在 `EventSystem.Update()` 原始输入处理结束后统一校正开关视觉状态，并归一化发型逻辑 ID、`mdl_chr_` 前缀、资源路径和 `_hair` 后缀；后续实机日志和截图已确认开关连续校正、发型名称“月村手毬 · 公主皇冠”命中。
- 发型预览走和服装同一条 `CostumeThumbnailView.Set(ICostume)`：master `CostumeHead` 实现 `ICostume`。早先只调基类 `ThumbnailViewBase.Set(assetName)` 的写法会留下未初始化的空态标签并丢失长按详情，已删除。
- Runtime 启动时注册全部有效替换候选但仍保持 AssetBundle 懒加载；标准 `SkinnedMeshRenderer` 规则会保存替换前的 Mesh、材质和骨骼绑定。关闭和重新开启的热切换主链已经实机生效。IDA MCP 后为即时颜色加入的底层材质数组钩子和崩溃排查扫描已全部撤回，当前恢复为“不崩溃、热切换生效、直接回主页颜色可能错误、切页恢复”的调查前基线。整对象替换和附加式规则仍按重新加载资源降级。
- 导航现在按页面身份幂等处理：Mod 页再次点“Mod 管理”只关闭菜单层；系统设置页点“Mod 管理”原位重构；Mod 页点系统设置则调用游戏的 `ICampusScreen.Reload()` 原位恢复干净设置页，避免多个 `SettingTopScreen` 交叉压栈。Reload 同时失效当前 `MenuView` 的注入缓存，使同地址菜单重新创建“Mod 管理”入口。

当前已部署、待实机验证的合并构建（2026-08-03，Runtime + 管理器同一个 DLL）：
`673280` 字节，SHA-256
`95D1BE29642388CB79A3DF2F32D99E09F40F870D91E3FE04C816484DD9670D49`。

> 上面的历史基线是**证据记录**，对应当时的截图和日志，不要改；只有这一行"当前"需要
> 随重编译更新。核对用 `Get-FileHash <游戏目录>\xinput1_3.dll -Algorithm SHA256`。

## 当前 UI 的边界

12:23 实机验证的是 M1 文本探针；18:20 起的截图已经验证真实全屏管理页。当前边界是：

- 12:23 历史基线由 `MenuSubButtonView` 放大而成，确实只有文字而没有合格的面板背景；该路径已废弃；
- 当前页面进入游戏原生设置窗口和返回栈，不再在主页 Canvas 上覆盖文字；
- 原生滚动区域、服装/发型分页、逐项开关和写回后刷新已经实机确认；
- 新固定行、服装 Master 名称、官方服装缩略图和发型 Master 名称已由截图确认；发型官方预览图直连路径待复验；
- Runtime 写回、开关视觉校正和标准替换热开关主链均已由实机确认；当前已撤回即时颜色实验，
  保留切页恢复的已知缺陷；
- 底栏星形分隔与橙条长度两个 UI 缺陷已实机确认修复（2026-08-02），**未解的只剩热开关后
  直接回主页颜色错误**，切换一次页面即恢复；
- 三种幂等导航分支已由上一部署版日志确认；最新的 Reload 后菜单入口缓存失效补丁已部署，仍需复验；
- 返回主页、重登、不同分辨率和长期重复打开的完整生命周期矩阵尚未验收。
- 当前源码已补充 PID/进程名并清除 Bootstrap 过期文案，但尚待新实机日志确认。

因此当前结论是：**M1 的入口和最小可见 UI 核心链路已通过，MVP 尚未完成。**

## 产品要求

- 第一版只管理 `gakumas-mod-runtime` 的 AssetBundle Mod；
- “服装”和“发型”分栏或分页显示；
- 一个 Mod 只对应一件服装或一个发型；
- 使用游戏内官方名称和官方图标；
- 普通玩家界面不显示资源 ID、`source`、`part`、Renderer 或文件路径；
- 开关同时修改当前 Runtime 会话与下次启动配置；标准服装/发型替换应即时恢复或应用到已实例化角色，失败时保留日志并允许通过重选或重进场景降级；
- 单个异常 Mod 或 UI 签名失效不能导致游戏崩溃。

## 仓库关系

```text
version.dll       汉化插件，独立，不参与本项目
xinput1_3.dll     Runtime（Mod 扫描、替换、Runtime API）+ 本目录的游戏内管理 UI
```

[`docs/ENTRY_DLL_EVIDENCE.md`](docs/ENTRY_DLL_EVIDENCE.md) 记录的是独立
`xinput9_1_0.dll` 时期的入口选择和 XInput 转发证据，DLL 合并后仅作历史参考。

## 文档入口

- [`docs/PROJECT_PLAN.md`](docs/PROJECT_PLAN.md)：产品要求、架构、里程碑和完整验收标准；
- [`docs/UI_FLOW.md`](docs/UI_FLOW.md)：当前源码流程、最新实机证据、限制和接手步骤；
- [`docs/SIGNATURE_MATRIX.md`](docs/SIGNATURE_MATRIX.md)：当前 PC 已验证/待验证的 IL2CPP 成员；
- [`docs/RESEARCH_SOURCES.md`](docs/RESEARCH_SOURCES.md)：metadata、dump 和日志的证据等级；
- [`docs/OPEN_DEFECTS.md`](docs/OPEN_DEFECTS.md)：缺陷排查记录；三个里两个已实机关闭，只剩热开关后颜色；
- [`docs/ENTRY_DLL_EVIDENCE.md`](docs/ENTRY_DLL_EVIDENCE.md)：独立 `xinput9_1_0.dll` 时期的加载依据（历史）。

`BORROW_LIBRARY_CASE_STUDY.md` 只是早期 Campus UI/API 背景研究，不属于当前 Mod 管理器的
产品范围或开发路线。

## 构建与实机日志

管理器是 `gakumas-mod-runtime` 仓库下的子目录 `manager/`，和 Runtime 编进同一个
`xinput1_3.dll`（根 `premake5.lua` 的 `gakumas_mod_runtime` 项目），不携带第三方源码：

| 依赖 | 来自 | 用途 |
|---|---|---|
| `deps/minhook` | 仓库根 | UI hook 与 Runtime 共用同一份 |
| `src/deps/nlohmann/json.hpp` | 同上 | `src/RuntimeModSnapshot.cpp` 直接 `#include` |
| `src/deps/UnityResolve/UnityResolve.hpp` | 同上 | `src/CampusUiProbe.cpp` 直接 `#include` |
| `src/runtime/ModRuntimeApi.h` | 同上 | Runtime API v1 的**唯一**定义，不再有子目录副本 |

### 生成 sln 并编译

Visual Studio 2022 / MSBuild Release x64：

在**仓库根**执行，管理器没有自己的 sln：

```powershell
.\generate.bat
msbuild build\gakumas_mod_runtime.sln /m /p:Configuration=Release /p:Platform=x64
```

`mod_presentation_tests` 是不依赖游戏的离线测试，直接运行
`build\bin\x64\Release\mod_presentation_tests.exe` 即可。

产物：

```text
build\bin\x64\Release\xinput1_3.dll
```

部署到：

```text
D:\Games\gakumas\xinput1_3.dll
```

游戏必须由用户手动启动。管理器日志位于：

```text
D:\Games\gakumas\gakumas-mod\mod-manager.log
```

日志等级由 `gakumas-mod/config.json` 的 `logLevel` 控制，与 Runtime 共用，默认
`"error"`：本文件里那些 `Mod menu: ...` 诊断行属于 info，默认不写。排查时把它改成
`"info"`。只有会关闭功能的行（入口未注入、注入 faulted、页面组合 faulted 等）保留在
error 级，默认也能看到。

## 下一步

1. 执行 Mod → 设置 → 菜单，确认 Reload 后“Mod 管理”入口仍存在；再验证设置 → Mod 与 Mod → Mod 不叠页；
2. 连续执行同一标准服装 Mod 的 ON/OFF/ON，并每次直接返回主页，确认颜色不再依赖切换其他页面刷新；
3. 打开发型分页，确认“月村手毬 · 公主皇冠”通过 CostumeHead 自身资源显示官方预览图；
4. 实机触发启动时冲突组全关和运行中新 Mod 拒绝开启两种提示；
5. 完成返回主页、重登、分辨率、异常降级和长期重复打开测试。
