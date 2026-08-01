# Gakumas In-Game Mod Manager

学园偶像大师 DMM 版的游戏内 AssetBundle Mod 管理器。

当前仓库已完成 M1 的入口加载与 Runtime API 握手验证，并在进行菜单入口和最小页面的实机验证。入口 DLL 使用游戏根目录 `d3d11.dll` 静态依赖的 `xinput9_1_0.dll`，独立于汉化的 `version.dll` 和 Mod Runtime 的 `xinput1_3.dll`。当前尚未证明完整管理页面稳定可用。第一版目标是：在游戏内查看服装与发型 Mod、显示它们对应的游戏内目标图标与名称，并修改下次启动时的启用状态。

完整规划见 [`docs/PROJECT_PLAN.md`](docs/PROJECT_PLAN.md)。

入口选择与导出转发证据见 [`docs/ENTRY_DLL_EVIDENCE.md`](docs/ENTRY_DLL_EVIDENCE.md)。

管理器启动日志写入游戏目录的 `gakumas-local\mod-manager.log`，同时发送到调试器输出。

## 当前阶段

截至 2026-08-01，已在目标游戏启动日志中确认：

- `xinput9_1_0.dll` 被游戏加载；
- 独立管理器成功找到 `xinput1_3.dll` 导出的 `GmrGetRuntimeApiV1`；
- Runtime API v1 握手成功；
- 管理器与汉化插件保持独立；
- 旧版 `HomeTopScreenPresenter.OpenNoticeSheetAsync` 探针因生命周期和调用安全问题已废弃；
- 菜单入口注入曾在 11:23 的构建中出现过成功日志，但 11:37 的最新构建未再次注入，当前不能视为稳定完成；
- 当前源码包含自建面板的实验路径，但尚无面板可见的最新实机证据；
- 服装/发型原生列表仍未接入。
- 管理器第一版按一个 Mod 一个逻辑目标处理；多条 `replacements[]` 不展开成多件服装，标记为配置异常。

- [x] 建立独立本地 Git 仓库
- [x] 完成产品与技术规划
- [x] 建立独立 `xinput9_1_0.dll` 入口、XInput 转发与 Runtime API 握手
- [x] 为 `gakumas-mod-runtime` 增加只读目录与启停 API
- [x] 在目标游戏进程中验证独立入口与 Runtime API v1 握手
- [ ] 验证游戏 UI 注入点与最小入口（历史构建曾成功，当前构建需修复回归）
- [ ] 制作管理器最小可用版本

借卡功能的 Campus 协议、好友列表和名片跳转研究记录在
[`docs/BORROW_LIBRARY_CASE_STUDY.md`](docs/BORROW_LIBRARY_CASE_STUDY.md)，不与本仓库的
Mod 管理 UI 验收混为一谈。
