# Gakumas In-Game Mod Manager

学园偶像大师 DMM Windows 版的独立游戏内 AssetBundle Mod 管理器。

管理器以 `xinput9_1_0.dll` 进入游戏进程，通过 `gakumas-mod-runtime` 的 Runtime API v1
读取 Mod 目录和写入启用状态。它不使用汉化插件注入，不替换 `version.dll`，也不承担
Mesh、材质或 AssetBundle 替换工作。

## 当前状态

截至 2026-08-01 12:23，以下链路已在目标游戏中完成实机验证：

```text
xinput9_1_0.dll 自动加载
  → 找到 xinput1_3.dll 的 GmrGetRuntimeApiV1
  → 读取 1451 字节 Runtime Mod 快照
  → Hook MenuPresenter.SetEvent / CampusButtonBase.OnClicked
  → 在主页菜单克隆“Mod 管理”入口
  → 点击入口创建自建文本面板
  → 面板显示 3 个 Mod 的名称、类别和当前状态
  → 再次点击可隐藏/重新显示
```

当前实机截图和日志已经证明：

- 主页菜单会出现且只观察到一个“Mod 管理”入口；
- 点击按钮不会卡死，点击动画和插件点击 Hook 均正常；
- Runtime JSON 已被解析成玩家可读的三条文本记录；
- 自建面板对象可见，并能通过同一入口切换显隐；
- 管理器、汉化 `version.dll` 和 Mod Runtime `xinput1_3.dll` 保持独立。

本次通过实机验证的 DLL：

```text
build\bin\x64\Release\xinput9_1_0.dll
大小：196608 字节
SHA-256：4AEFBEE12BC11509ACCFF62BA660D387AD395FA947B69AD0D9ECA19E82F7DBD5
```

## 当前 UI 的边界

现在显示的是 M1 功能验证面板，不是最终管理页面：

- 面板由 `MenuSubButtonView` 克隆后放大，仍覆盖在原菜单内容之上；
- 没有独立背景、正式列表、滚动、分页、关闭按钮或返回栈；
- 当前只显示文本，没有官方服装/发型图标；
- 没有服装/发型分栏；
- 没有接入启用开关；
- Runtime 快照在 UI 探针启动时构建，尚未在每次打开页面时刷新；
- 返回主页、重登、不同分辨率和长期重复打开的完整生命周期矩阵尚未验收。
- 当前日志缺少 PID，启动链中的多组 DLL 加载记录会混在同一文件；Bootstrap 仍有一条
  “UI hook is intentionally disabled”的过期文案，但成功分支实际会启动 UI Probe。

因此当前结论是：**M1 的入口和最小可见 UI 核心链路已通过，MVP 尚未完成。**

## 产品要求

- 第一版只管理 `gakumas-mod-runtime` 的 AssetBundle Mod；
- “服装”和“发型”分栏或分页显示；
- 一个 Mod 只对应一件服装或一个发型；
- 使用游戏内官方名称和官方图标；
- 普通玩家界面不显示资源 ID、`source`、`part`、Renderer 或文件路径；
- 开关只修改下次启动配置，并提示“重启游戏后生效”；
- 单个异常 Mod 或 UI 签名失效不能导致游戏崩溃。

## 仓库关系

```text
version.dll       汉化插件，独立，不参与本项目
xinput1_3.dll     gakumas-mod-runtime，负责 Mod 扫描、替换和 Runtime API
xinput9_1_0.dll   本项目，负责游戏内管理 UI
```

入口选择和 XInput 转发证据见 [`docs/ENTRY_DLL_EVIDENCE.md`](docs/ENTRY_DLL_EVIDENCE.md)。

## 文档入口

- [`docs/PROJECT_PLAN.md`](docs/PROJECT_PLAN.md)：产品要求、架构、里程碑和完整验收标准；
- [`docs/UI_FLOW.md`](docs/UI_FLOW.md)：当前源码流程、最新实机证据、限制和接手步骤；
- [`docs/SIGNATURE_MATRIX.md`](docs/SIGNATURE_MATRIX.md)：当前 PC 已验证/待验证的 IL2CPP 成员；
- [`docs/RESEARCH_SOURCES.md`](docs/RESEARCH_SOURCES.md)：metadata、dump 和日志的证据等级；
- [`docs/ENTRY_DLL_EVIDENCE.md`](docs/ENTRY_DLL_EVIDENCE.md)：`xinput9_1_0.dll` 加载依据。

`BORROW_LIBRARY_CASE_STUDY.md` 只是早期 Campus UI/API 背景研究，不属于当前 Mod 管理器的
产品范围或开发路线。

## 构建与实机日志

Visual Studio 2022 / MSBuild Release x64：

```powershell
msbuild build\gakumas_in_game_mod_manager.sln /m /t:xinput9_1_0_manager /p:Configuration=Release /p:Platform=x64
```

产物：

```text
build\bin\x64\Release\xinput9_1_0.dll
```

部署到：

```text
D:\Games\gakumas\xinput9_1_0.dll
```

游戏必须由用户手动启动。管理器日志位于：

```text
D:\Games\gakumas\gakumas-local\mod-manager.log
```

## 下一步

1. 把覆盖式验证面板替换成拥有独立背景、关闭行为和稳定层级的管理容器；
2. 将 Runtime 快照转换成正式 Presentation Model；
3. 完成服装/发型分栏；
4. 接 Costume/CostumeHead Master 和官方图标；
5. 接入启用开关及“重启后生效”反馈；
6. 完成返回主页、重登、分辨率和异常降级测试。
