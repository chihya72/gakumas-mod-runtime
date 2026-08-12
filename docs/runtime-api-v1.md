# Runtime API v1

`gakumas-mod-runtime` 通过 `GmrGetRuntimeApiV1` 提供版本化 C ABI。管理器 UI 现在与
Runtime 同处 `xinput1_3.dll`，但仍只经由这张函数表与 Runtime 通信，不直接调用内部符号；
导出同时保留给外部调用方。API 负责完整 Mod 快照、当前会话启停和 Manifest 持久化；
不负责管理器 UI 注入。

> 当前语义：标准 `SkinnedMeshRenderer` 替换支持热开关，不再要求每次切换后重启游戏。
> 整对象替换、附加式规则或热恢复失败时仍可能需要重新选择资源、重进场景或重启。

## 1. 导出与版本协商

```cpp
GmrResult GMR_CALL GmrGetRuntimeApiV1(GmrRuntimeApiV1* output);
```

`output` 由调用方分配，并填写自身可接受的 `structSize`。Runtime 返回：

- `apiVersion = GMR_API_VERSION_1`；
- `getModsJson`；
- `freeBuffer`；
- `setModEnabled`；
- `writeLog`。

当前 Release 产物和部署模块名都是 `xinput1_3.dll`。不加载、覆盖或改名汉化插件的
`version.dll`。

## 2. 所有权与线程约束

`getModsJson` 返回 Runtime 分配的 UTF-8 JSON 缓冲区。调用方必须使用同一 API 表中的
`freeBuffer` 释放，不能跨 DLL 直接 `free`。

目录容器由 Runtime 锁保护；JSON 是深拷贝，不向调用方暴露内部引用。管理器当前从 Unity
主线程触发开关，以便热恢复安全访问场景对象。调用方不得在 Runtime Shutdown 后保存函数表
或继续调用。

## 3. 快照

根对象包含 `schemaVersion` 和 `mods[]`。每个 Mod 的主要字段：

| 字段 | 含义 |
|---|---|
| `id` / `name` | 稳定 Mod ID 与作者名称 |
| `configuredEnabled` | 当前 Manifest 保存的 `enabled` |
| `registeredThisSession` | 当前会话有效 replacement map 是否注册该 Mod |
| `appliedThisSession` | Runtime 是否记录到本次会话应用；当前仍需补齐所有真实应用点 |
| `restartRequired` | 热路径无法同步时的兼容字段；标准热切换成功后为 `false` |
| `manifestState` | `valid`、损坏、缺 Bundle、多逻辑目标或不支持部位等目录状态 |
| `runtimeState` | `active`、`disabled`、冲突或无效状态 |
| `target.kind` | `costume` 或 `hair` |
| `target.source/masterKey/part` | 管理器解析官方 Master 名称和预览的内部键 |
| `conflict` | 冲突对象、名称、是否启动时自动关闭或运行中被拒绝 |

获取快照不会重扫 Mod 目录、加载 AssetBundle 或改变当前 replacement map。AssetBundle 仍按
游戏实际资源请求懒加载。

## 4. `setModEnabled`

```cpp
GmrResult setModEnabled(const char* modIdUtf8, uint8_t enabled);
```

只接受初始化扫描得到的稳定 Mod ID，不接受文件路径。成功路径：

1. 在目录锁内唯一定位 Mod，并确认 Manifest 状态有效；
2. 开启请求先检查相同 `target.kind + target key` 是否已有启用项；
3. 调用 Runtime 会话层更新候选状态、重建有效 replacement map，并对支持的当前对象执行
   热恢复或热重应用；
4. 重新读取并校验目标 `mod.json` 的时间戳、摘要和 ID，检测外部并发修改；
5. 只修改顶层 `enabled`；
6. 在同目录写临时文件、Flush，并通过
   `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` 原子替换；
7. 清理同目标的旧冲突提示并重新计算快照状态。

如果持久化失败，Runtime 尝试把当前会话回滚到调用前状态，再把文件错误返回给管理器。

### 4.1 同目标冲突

- 启动扫描发现同一目标有多个 Manifest 同时 `enabled=true`：Runtime 将整组当前会话和
  Manifest 全部关闭，并在快照记录自动冲突信息；不按 `priority` 偷偷保留一个；
- 当前会话已有同目标 Mod 启用：开启新 Mod 返回 `GMR_E_TARGET_CONFLICT`，现有 Mod 保持
  ON，新 Mod 保持 OFF；玩家必须先关闭旧项。

### 4.2 热切换范围

标准原地 `SkinnedMeshRenderer` 规则：

- OFF 恢复当前 Renderer 快照中仍存活且与可逆记录匹配的实例；旧场景对象指针不会跨帧保存
  或解引用；
- ON 对当前目标服装/发型资源子树重应用 Mesh、材质、骨骼和贴图；如果当前场景没有目标，
  Runtime 仍返回成功并按 `modId + source` 排队，在 `RegisterBones` 或 Renderer 材质生命周期
  回调中重试；
- `hotInstances=0` 在上述情形表示“当前无目标，已延迟”，不是持久化失败。若资源加载路径已经
  先完成替换，延迟完成日志会是 `applied=0 alreadyPatched=1`；
- Runtime 刷新 Renderer 和目标节点；热 ON 后直接回主页颜色即正确；
- 新增摇物骨/链是在 prefab graft 阶段建立并由游戏初始化收走的，直接热 ON 只刷新已有活体的
  网格、材质与碰撞体；要让新增链生效仍需重新进入场景。

> 早期文档把热 ON 的颜色错误先后归因于 `MaterialPropertyBlock` 覆盖和
> `Renderer.set_sharedMaterials` 写回材质数组，两条都已被证伪。真因是换空间时只搬顶点、
> 没搬法线和切线，2026-08-09 修复并实机验证。见
> [`lessons-learned.md`](lessons-learned.md)。

整对象替换和附加式规则不保证即时逆转。此类规则的配置状态仍会写回，但已实例化对象可能
需要重新加载资源或场景。

## 5. 结果码

| 结果 | 值 | 含义 |
|---|---:|---|
| `GMR_OK` | 0 | 成功，或请求状态已经一致 |
| `GMR_E_INVALID_ARGUMENT` | 1 | 空 ID、空输出或结构参数错误 |
| `GMR_E_API_VERSION` | 2 | API 版本/结构尺寸不兼容 |
| `GMR_E_NOT_INITIALIZED` | 3 | Runtime 目录尚未就绪或正在关闭 |
| `GMR_E_MOD_NOT_FOUND` | 4 | ID 不在已扫描目录 |
| `GMR_E_MANIFEST_INVALID` | 5 | 该 Mod 不能作为有效单目标规则管理 |
| `GMR_E_ACCESS_DENIED` | 6 | Manifest 无写权限 |
| `GMR_E_IO` | 7 | 其他文件 I/O 失败 |
| `GMR_E_CONCURRENT_CHANGE` | 8 | 重复 ID或外部修改改变目标身份 |
| `GMR_E_INTERNAL` | 9 | 会话热切换或内部操作失败 |
| `GMR_E_TARGET_CONFLICT` | 10 | 同目标已有启用 Mod，拒绝新开启 |

## 6. 当前验证状态

- Release x64 已构建并确认导出 `GmrGetRuntimeApiV1`；
- `tests/ModRuntimeCatalogSmoke.cpp` 覆盖有效/无效目录、冲突、多目标、缺 Bundle、原子写回，
  以及启动冲突写回失败时的会话 fail-closed 语义；它由 `premake5.lua` 编译为
  `mod_runtime_catalog_tests`，`package.ps1` 打包前强制运行；
- 另有 `mod_presentation_tests` 与 Python `unittest discover` 的源码契约测试；
- 目标游戏已确认快照、Manifest 写回和标准服装热 OFF/ON 生效；
- 标准热 OFF/ON 生效、不崩溃，热 ON 后直接返回主页颜色即正确（2026-08-09，12 轮验证）；
- 零活体目标时的延迟 ON 已用 `hmsz-fuyuko-icu` 实机确认（2026-08-12）：返回主页后资源路径
  已完成替换，生命周期 Hook 以 `alreadyPatched=1` 清除队列；`atbm-cstm-0140` 的同分支尚未
  单独复验；
- API 头文件 `src/runtime/ModRuntimeApi.h` 现在只有一份，Manager 直接包含它。
