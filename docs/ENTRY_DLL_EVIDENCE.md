# 独立入口 DLL 证据

## 已确认的第三个入口

检查目标文件：

```text
D:\Games\gakumas\UnityPlayer.dll
```

从该二进制的字符串表中确认存在以下 XInput 动态加载候选：

```text
xinput1_3.dll
xinput1_4.dll
xinput9_1_0.dll
```

游戏根目录当前只有现有 Mod Runtime 的 `xinput1_3.dll`，没有 `xinput1_4.dll`。因此管理器使用 `xinput1_4.dll`，与 `version.dll` 汉化入口和 `xinput1_3.dll` Runtime 入口分离。

## 代理要求

管理器入口按系统 `xinput1_4.dll` 的命名导出转发以下函数：

```text
XInputGetState
XInputSetState
XInputGetCapabilities
XInputEnable
XInputGetBatteryInformation
XInputGetKeystroke
XInputGetAudioDeviceIds
XInputGetStateEx (ordinal 100)
```

代理从系统目录的真实 `xinput1_4.dll` 动态解析后转发，再启动管理器工作线程。不能只导出管理器自己的初始化函数，否则 Unity 输入模块可能因为缺少 XInput 导入符号而加载失败。

当前 Release 产物：

```text
build\bin\x64\Release\xinput1_4.dll
```

## 重新验证

```powershell
dumpbin /exports $env:WINDIR\System32\xinput1_4.dll
dumpbin /exports build\bin\x64\Release\xinput1_4.dll
```
