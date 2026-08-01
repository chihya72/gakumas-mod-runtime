# 独立入口 DLL 证据

## 已确认的第三个入口

检查目标文件：

```text
D:\Games\gakumas\UnityPlayer.dll
```

`UnityPlayer.dll` 静态导入 `dwmapi.dll` 的 `DwmGetWindowAttribute`、`DwmSetWindowAttribute`。游戏目录内的 Chromium 网页组件也使用同一个系统 DLL，但整个目录实际需要转发的 DWM API 只有以下 5 个：

```text
DwmDefWindowProc
DwmExtendFrameIntoClientArea
DwmGetCompositionTimingInfo
DwmGetWindowAttribute
DwmSetWindowAttribute
```

该入口不参与汉化的 `version.dll`，也不参与 Mod Runtime 的 `xinput1_3.dll`，不会成为游戏登录、汉化和网页组件的公共网络链代理。

## 代理要求

管理器入口必须导出上述 5 个函数，并从系统目录的真实 `dwmapi.dll` 动态解析后转发。不能只导出管理器自己的初始化函数，否则 UnityPlayer 或网页组件会因为缺少导入符号而加载失败。

当前 Release 产物：

```text
build\bin\x64\Release\dwmapi.dll
```

## 重新验证

```powershell
dumpbin /imports D:\Games\gakumas\UnityPlayer.dll
dumpbin /exports build\bin\x64\Release\dwmapi.dll
```

如果游戏更新后的模块新增了 DWM API 导入，必须把对应的转发函数补齐后再部署。
