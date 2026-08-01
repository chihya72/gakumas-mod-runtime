# Gakumas In-Game Mod Manager

学园偶像大师 DMM 版的游戏内 AssetBundle Mod 管理器。

当前仓库已进入 M1/M2 初步制作阶段。入口 DLL 使用游戏 `UnityPlayer.dll` 已导入的 `dwmapi.dll` 名称，独立于汉化的 `version.dll` 和 Mod Runtime 的 `xinput1_3.dll`。完整游戏内页面尚未接入。第一版目标是：在游戏内查看服装与发型 Mod、显示它们对应的游戏内目标图标与名称，并修改下次启动时的启用状态。

完整规划见 [`docs/PROJECT_PLAN.md`](docs/PROJECT_PLAN.md)。

入口选择与导出转发证据见 [`docs/ENTRY_DLL_EVIDENCE.md`](docs/ENTRY_DLL_EVIDENCE.md)。

## 当前阶段

- [x] 建立独立本地 Git 仓库
- [x] 完成产品与技术规划
- [x] 建立独立 `dwmapi.dll` 入口、DWM API 转发与 Runtime API 握手
- [x] 为 `gakumas-mod-runtime` 增加只读目录与启停 API
- [ ] 验证游戏 UI 注入点与服装/发型原生格子
- [ ] 制作管理器最小可用版本
