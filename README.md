# Gakumas In-Game Mod Manager

学园偶像大师 DMM 版的游戏内 AssetBundle Mod 管理器。

当前仓库已完成 M1 的入口加载与 Runtime API 握手验证，并进入 M2 数据通路和 UI 接入准备阶段。入口 DLL 使用游戏根目录 `d3d11.dll` 静态依赖的 `xinput9_1_0.dll`，独立于汉化的 `version.dll` 和 Mod Runtime 的 `xinput1_3.dll`。完整游戏内页面尚未接入。第一版目标是：在游戏内查看服装与发型 Mod、显示它们对应的游戏内目标图标与名称，并修改下次启动时的启用状态。

完整规划见 [`docs/PROJECT_PLAN.md`](docs/PROJECT_PLAN.md)。

入口选择与导出转发证据见 [`docs/ENTRY_DLL_EVIDENCE.md`](docs/ENTRY_DLL_EVIDENCE.md)。

管理器启动日志写入游戏目录的 `gakumas-local\mod-manager.log`，同时发送到调试器输出。

## 当前阶段

截至 2026-08-01，已在目标游戏启动日志中确认：

- `xinput9_1_0.dll` 被游戏加载；
- 独立管理器成功找到 `xinput1_3.dll` 导出的 `GmrGetRuntimeApiV1`；
- Runtime API v1 握手成功；
- 管理器与汉化插件保持独立；
- 当前仍未接入主页菜单、原生服装/发型列表或可见 UI。

- [x] 建立独立本地 Git 仓库
- [x] 完成产品与技术规划
- [x] 建立独立 `xinput9_1_0.dll` 入口、XInput 转发与 Runtime API 握手
- [x] 为 `gakumas-mod-runtime` 增加只读目录与启停 API
- [x] 在目标游戏进程中验证独立入口与 Runtime API v1 握手
- [ ] 验证游戏 UI 注入点与服装/发型原生格子
- [ ] 制作管理器最小可用版本
