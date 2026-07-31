# Runtime API v1

`gakumas-mod-runtime` 通过 `GmrGetRuntimeApiV1` 向独立的游戏内管理器 DLL 提供最小 C ABI。该接口只负责 Mod 目录状态和配置写回，不负责管理器 UI 注入。

## 导出

```cpp
GmrResult GMR_CALL GmrGetRuntimeApiV1(GmrRuntimeApiV1* output);
```

当前 Release 产物为 `xinput1_3.dll`。如果部署时文件名使用 `xinput3.dll`，导出内容保持一致；管理器通过配置的模块名查找，不会覆盖或改名现有代理。

## 快照

`getModsJson` 返回由 Runtime 分配的 UTF-8 JSON 缓冲区，调用方必须用同一 API 表中的 `freeBuffer` 释放。

快照包括：

- `configuredEnabled`：Manifest 当前保存的启用状态；
- `registeredThisSession`：本次启动是否注册为有效替换；
- `appliedThisSession`：当前 Runtime 是否记录到成功应用；
- `restartRequired`：配置和本次启动状态是否不同；
- `manifestState`：Manifest、Bundle 和单目标校验结果；
- `runtimeState`：启用、禁用、冲突或待重启状态；
- `target.kind/source/masterKey/part`：供 UI 解析官方名称和图标的内部目标信息；
- `conflict`：同一逻辑目标的冲突摘要。

Runtime 不在管理器打开前重新加载 Bundle，也不因获取快照而改变当前替换 map。

## 写回

`setModEnabled` 只接受已扫描的 Mod ID：

1. 重新读取目标 `mod.json`；
2. 检查文件摘要和 Mod ID 没有被外部修改；
3. 只更新顶层 `enabled`；
4. 写入同目录临时文件并 Flush；
5. 使用 `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` 原子替换；
6. 更新内存快照中的待重启状态。

该调用不热卸载、不热加载、不重建 replacement map。玩家需要重启游戏才能让切换后的状态进入 Runtime。

## 验证

- Runtime Release 构建已确认导出 `GmrGetRuntimeApiV1`；
- `tests/ModRuntimeCatalogSmoke.cpp` 覆盖启用、禁用、冲突、多目标、缺 Bundle 与原子启停写回；
- 管理器 M1 探针通过 `GetModuleHandleW` 查找 `xinput1_3.dll` 和部署别名 `xinput3.dll`，不依赖 `version.dll` 或汉化插件。
