# 独立入口 DLL 证据

## 已确认的第三个入口

检查目标文件：

```text
<游戏目录>\d3d11.dll
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

## 实机验证基线

2026-08-01 12:23 的目标游戏测试使用：

```text
大小：196608 字节
SHA-256：4AEFBEE12BC11509ACCFF62BA660D387AD395FA947B69AD0D9ECA19E82F7DBD5
```

`dumpbin /exports` 已确认继续导出：

```text
XInputGetCapabilities
XInputGetDSoundAudioDeviceGuids
XInputGetState
XInputSetState
```

同一构建已在游戏内显示“Mod 管理”入口和最小文本面板，因此入口证据不再只停留在 PE
导入表；自动加载、工作线程、Runtime API 握手和 UI Hook 均有实机日志。

此后完整 `SettingTopScreen` 管理页、固定 Mod 行和导航逻辑继续沿用同一
`xinput9_1_0.dll` 入口，已取得实机截图与日志。当前游戏目录部署版为：

```text
<游戏目录>\xinput9_1_0.dll
大小：270848 字节
SHA-256：DBAE909DEA768F74C13D51944649554E75F28B6AD517EFF0A9E4912A350766DC
```

该版本的 Release 构建和 XInput 转发约束未改变；新增的是 Reload 后失效同地址菜单 View
注入缓存。它已经部署，但这项最新行为仍待下一次实机复验。12:23 哈希继续保留为最小入口
链路的历史可复现基线，不能误写成当前游戏目录文件。

## 重新验证

```powershell
dumpbin /imports <游戏目录>\d3d11.dll
dumpbin /exports build\bin\x64\Release\xinput9_1_0.dll
```
