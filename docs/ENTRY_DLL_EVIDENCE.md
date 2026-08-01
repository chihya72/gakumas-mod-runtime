# 独立入口 DLL 证据

## 已确认的第三个入口

检查目标文件：

```text
D:\Games\gakumas\d3d11.dll
```

该模块的 PE 导入表明确包含：

```text
XINPUT9_1_0.dll
  XInputGetCapabilities
  XInputGetDSoundAudioDeviceGuids
  XInputGetState
  XInputSetState
```

因此 `xinput9_1_0.dll` 是由现有游戏图形模块自动带入进程的独立入口，不占用汉化的 `version.dll`，也不占用 Mod Runtime 的 `xinput1_3.dll`。游戏目录原本没有 `xinput9_1_0.dll`。

## 代理要求

管理器入口按系统 `xinput9_1_0.dll` 的命名导出转发上述 4 个函数，并从系统目录的真实 DLL 动态解析。不能只导出管理器自己的初始化函数，否则 `d3d11.dll` 会因缺少 XInput 导入符号而加载失败。

当前 Release 产物：

```text
build\bin\x64\Release\xinput9_1_0.dll
```

## 重新验证

```powershell
dumpbin /imports D:\Games\gakumas\d3d11.dll
dumpbin /exports build\bin\x64\Release\xinput9_1_0.dll
```
