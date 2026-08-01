# M1：独立 DLL 注入与 UI 探针流程

M1 的探针只验证入口加载、Runtime 握手和原生 UI 生命周期，不连接完整 Mod 列表。

## 当前进度（2026-08-01）

- 已验证游戏加载独立入口 `xinput9_1_0.dll`；
- 已验证管理器从 `xinput1_3.dll` 获取并调用 `GmrGetRuntimeApiV1`；
- 已在目标游戏日志中看到 `M1 probe connected to Runtime API v1`；
- Campus 原生 Sheet 探针实验代码曾尝试在 Unity 主线程定位 `HomeTopScreenPresenter` 并通过 `RuntimeInvoke` 调用 `OpenNoticeSheetAsync`；当前已禁用，等待更安全的主线程调度方案；
- 原生主页菜单、Sheet/Screen 和服装/发型 Cell 尚未接入。

```text
UnityPlayer.dll
  │
  ├─ 加载游戏根目录的 d3d11.dll
  └─ 根据导入表加载 xinput9_1_0.dll
       │
       ├─ DllMain：只创建工作线程
       ├─ 查找 xinput1_3.dll（部署别名可为 xinput3.dll）
       ├─ GetProcAddress(GmrGetRuntimeApiV1)
       ├─ 检查 Runtime API v1
       ├─ 等待主页菜单对象
       └─ 注入“Mod 管理”空入口
```

## 探针验收

- 汉化插件是否安装，不影响管理器 DLL 的加载与卸载；
- `d3d11.dll` 的导入关系能成功加载 `xinput9_1_0.dll`；
- `version.dll` 不被替换、不被加载为管理器依赖；
- `xinput1_3.dll` 与部署别名 `xinput3.dll` 的映射可配置；
- Runtime API 不存在时，管理器只写日志，不显示入口；
- 主页首次进入只创建一个入口；
- 离开主页再进入不会重复创建；
- 关闭空白页面不会留下按钮事件或对象引用；
- 管理器 DLL 卸载时移除自己的 Hook；
- Runtime 的 AssetBundle 替换在探针失败时仍能工作。

已实机通过：

- 独立入口加载；
- Runtime API v1 握手；
- 管理器与汉化插件无加载依赖。

待实机验证（当前不自动执行）：

- `Campus.OutGame.HomeTopScreenPresenter` 实例查找；
- `OpenNoticeSheetAsync` 的 RuntimeInvoke 调用；
- 原生 Sheet 能否在管理器自己的生命周期中安全打开和关闭。

## 备注

M1 不在本阶段修改 `version.dll` 或汉化插件工程。M1 的实际 UI 类名、Presenter、Cell 和 Toggle 方法，必须在目标游戏版本的 metadata/实机日志中补齐到 `SIGNATURE_MATRIX.md` 后，才能进入服装列表开发。
