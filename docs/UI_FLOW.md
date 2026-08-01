# M1：独立 DLL 注入与 UI 探针流程

M1 的探针只验证入口加载、Runtime 握手和原生 UI 生命周期，不连接完整 Mod 列表。

```text
UnityPlayer.dll
  │
  ├─ 查找 UnityPlayer 内的 XInput 动态候选
  └─ 加载游戏目录中的 xinput1_4.dll
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
- `UnityPlayer.dll` 的 XInput 动态加载能成功加载 `xinput1_4.dll`；
- `version.dll` 不被替换、不被加载为管理器依赖；
- `xinput1_3.dll` 与部署别名 `xinput3.dll` 的映射可配置；
- Runtime API 不存在时，管理器只写日志，不显示入口；
- 主页首次进入只创建一个入口；
- 离开主页再进入不会重复创建；
- 关闭空白页面不会留下按钮事件或对象引用；
- 管理器 DLL 卸载时移除自己的 Hook；
- Runtime 的 AssetBundle 替换在探针失败时仍能工作。

## 备注

M1 不在本阶段修改 `version.dll` 或汉化插件工程。M1 的实际 UI 类名、Presenter、Cell 和 Toggle 方法，必须在目标游戏版本的 metadata/实机日志中补齐到 `SIGNATURE_MATRIX.md` 后，才能进入服装列表开发。
