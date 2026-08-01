# 独立入口 DLL 证据

## 已确认的第三个入口

检查目标文件：

```text
D:\Games\gakumas\UnityPlayer.dll
```

游戏目录中所有依赖 `WINHTTP.dll` 的模块合计使用以下导出：

```text
WinHttpAddRequestHeaders
WinHttpCloseHandle
WinHttpConnect
WinHttpCrackUrl
WinHttpCreateProxyResolver
WinHttpFreeProxyResult
WinHttpGetDefaultProxyConfiguration
WinHttpGetIEProxyConfigForCurrentUser
WinHttpGetProxyForUrl
WinHttpGetProxyForUrlEx
WinHttpGetProxyResult
WinHttpOpen
WinHttpOpenRequest
WinHttpQueryAuthSchemes
WinHttpQueryDataAvailable
WinHttpQueryHeaders
WinHttpQueryOption
WinHttpReadData
WinHttpReceiveResponse
WinHttpSendRequest
WinHttpSetCredentials
WinHttpSetOption
WinHttpSetStatusCallback
WinHttpSetTimeouts
WinHttpWriteData
```

其中 `UnityPlayer.dll` 使用基础的 4 个函数，汉化的 `version.dll` 和内置网页插件还会使用其余函数。游戏根目录没有系统原版 `winhttp.dll`，因此管理器可以使用这个独立入口，不覆盖汉化插件的 `version.dll`，也不覆盖 Mod Runtime 的 `xinput1_3.dll`。

## 代理要求

管理器入口必须导出上述所有函数，并从系统目录的真实 `winhttp.dll` 动态解析后转发。不能只导出管理器自己的初始化函数，否则任一依赖模块都会因为缺少导入符号而加载失败。

当前 Release 产物：

```text
build\bin\x64\Release\winhttp.dll
```

## 重新验证

```powershell
dumpbin /imports D:\Games\gakumas\UnityPlayer.dll
dumpbin /exports build\bin\x64\Release\winhttp.dll
```

如果游戏更新后的 `UnityPlayer.dll` 新增了 WinHTTP 导入，必须把对应的转发函数补齐后再部署。
