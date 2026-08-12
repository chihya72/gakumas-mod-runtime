# Gakumas Mod Runtime 已知风险与排查索引

更新：2026-08-12
适用：`gakumas-mod-runtime` 1.0.0 之后的当前工作树

这是本仓库 Runtime、游戏内管理器和运行时 manifest 契约独立审计的**唯一长期台账**。遇到游戏
崩溃、换模异常、热开关异常或管理器问题时，先按本文的现象和 ID 排查。

本文只记录 `gakumas-mod-runtime` 仓库自身的问题，不记录 Blender 插件、素材制作或离线导出器
自身的问题。保留原始审计编号方便从日志与历史反查；原审计 P5 的根因位于本仓 Runtime，因此
在本文登记为 X1（原 P5）。所有条目当前均按决定只记录，不表示计划立即修改。

## 当前总体结论

1. **当前已验证的 Runtime 和成品没有已知必现阻断问题。** Runtime 1.0.0 的构建与测试通过；
   当前实机成品和 `ttmr-hair-0002` 双 Renderer 发型没有命中已知必现失败条件。
2. **F1、F2、R6、R7、R8、R10、R11 主要是游戏、Unity 或 IL2CPP metadata 更新后的兼容风险。**
   当前没有实测故障；更新后可能表现为解析失败、错误内存写入、网格变形或崩溃，首轮兼容测试
   必须优先复核。
3. **X1 对特定双 Renderer 发型是确定缺陷，不是极小概率。** 已有完整双 skeleton 的 30 组中，
   19 组按最终骨序不等价；使用这些目标并同时替换 `Geo_HairProp` 时，当前 Runtime 会读取 Hair
   顶层 sidecar，导致 HairProp 因骨数不匹配被跳过。`amao-hair-0002` 与 `ttmr-hair-0002` 已确认
   最终骨架等价，不受影响。
4. **R3、R4、R9、R12 是尚未量化的偶发风险，不能称为已经证明的“极小概率”。** 当前测试未
   复现；R4 只影响实验性 attach 路线，R3/R9 应在偶发加载崩溃或换场景后异常时优先检查。
5. **其余多数条目影响维护性、性能、诊断或极端规模，**不会让已验证的常规换模路径必然失败。

因此，当前已验证内容可以继续使用，但这不等于 Runtime 零缺陷，也不等于全部剩余风险都只是
极小概率。游戏更新后先查兼容性项；新双 Renderer 发型先查 X1；未知偶发崩溃先查并发与生命周期项。

## 先查：按现象排查

| 现象 | 优先检查 |
|---|---|
| 游戏更新后启动、换装或加载 mod 崩溃 | F1、F2、R6、R8、R10、R11 |
| 偶发崩溃，单次测试难复现；并行加载时更容易出现 | R3、R4、R9、R12 |
| `Geo_Hair` 正常但 `Geo_HairProp` 没换、报 sidecar count mismatch | X1 |
| 网格严重变形、骨骼跟错或 bindpose 整体错误 | X1、R7、R15 |
| 热 OFF/ON 或换场景后网格、材质异常 | R9、R11 |
| 新增骨很多时加载卡顿 | R5 |
| Mod 管理页条目、按钮或分组异常 | M1、M2、M3、M4、M5 |
| 日志文件异常增长、调试输出不完整或路径错误 | N1、N2、N3 |

## X1（原 P5）：副 Renderer 的独立 skeleton 未被读取

Runtime 的 `LocalModRendererRule` 只解析 `rendererId/targetRenderer/modRenderer`，不保存 renderer
级 `skeleton`。对 `Geo_Hair + Geo_HairProp` 两个 Renderer 做 graft 时，两者都读取 replacement
顶层、也就是 Hair 的 sidecar。

### `mdl_chr_amao-hair-0002_hair` 的结论

**按当前标准制作流程可以使用，不会因为 X1 出错。** 原始 skeleton 虽有 4 个
`weightedIndex` 成对互换，但导出后的 Hair/HairProp 都是 77 根骨，同名、同序、同父子关系、
同局部变换，root 都是 `Head_Hair`。因此 Runtime 使用 Hair 顶层 sidecar 时仍与 HairProp 等价。

这个结论以最终 sidecar 契约为准，不能仅根据 AssetStudio 原始索引顺序判断。如果两个 Renderer
刻意携带不同的自定义新增骨，仍需重新比较最终 sidecar。

### 已检查资源

- 378 组原始 Hair/HairProp Mesh 中，240 组骨数不同，是确定的 count mismatch 候选；
- 另有 58 组骨数相同但原始哈希数组不同，必须生成完整 skeleton 后比较最终骨序，不能直接判坏；
- 已有完整双 skeleton 的 30 组按最终骨序复算：11 组等价，19 组不等价；
- 19 组不等价资源的最终骨数均不同，触发时 Runtime 会记录 `sidecar count mismatch` 并跳过
  HairProp 网格，而不是让整个游戏必然崩溃；
- 当前成品 `ttmr-hair-madoka-swimsuit` 使用 `ttmr-hair-0002`，属于等价资源。

已确认不等价的 19 组：

`amao-hair-0006/0008/0014/0023`、`hmsz-cstm-0053/0054`、`hrnm-cstm-0063`、
`hski-hair-0001/0006/0023`、`hume-cstm-0063`、`hume-hair-0001/0002`、
`jsna-hair-0001/0009`、`kcna-hair-0013`、`ttmr-cstm-0084`、
`ttmr-hair-0008/0025`。

状态：**仅记录**。HairProp 不替换时优先查本项。

## Runtime 已知风险

| ID | 等级 | 触发条件与影响 | 当前决定 |
|---|---|---|---|
| F1 | 高 | IL2CPP `GetValue/SetValue` 直接解引用字段；游戏更新导致内部字段改名时会硬崩，而不是只让 mod 失效 | 不处理；字段几乎不会改名。游戏更新后若崩先查 |
| F2 | 中 | 精确方法签名解析失败后可能退回同名错误重载；可能静默失败、调用错误或崩溃 | 仅记录 |
| R1 | 维护 | `Shutdown()` 在模块被 pin 后没有调用者，约 95 行清理代码不可达；不影响正常进程生命周期 | 仅记录 |
| R2 | 维护 | `LoadAssetBundleFromMemoryFile` 及依赖链无人调用；实际走文件加载 | 仅记录 |
| R3 | 高 | `g_dumpedProfiles` 的 `contains+emplace` 无锁；并发资产加载可能造成容器数据竞争、内存破坏或偶发崩溃 | 仅记录；偶发加载崩溃优先查 |
| R4 | 中 | 实验性 `attachToOriginal` 的 `attachAsset/attachSourceMeshes` 跨线程读写无锁；可能崩溃或挂错对象 | 仅记录；普通 hair/body 路径不受影响 |
| R5 | 中 | `FindClassByName` 每次扫描全部 assembly/class，且在 `createBone` 中逐骨调用；大量新骨时产生加载卡顿 | 仅记录 |
| R6 | 中/更新高危 | `ActorSwingCollider` 与 `LimitInfo` 按固定偏移裸写；布局变化会写坏托管对象，可能随机或延迟崩溃 | 仅记录；游戏/metadata 更新后复核偏移 |
| R7 | 中 | Unity `Matrix4x4.op_Multiply` 解析失败才进入手写兜底；兜底把列主序按行主序计算，相当于交换乘法次序，bindpose 会整体错误 | 仅记录；只有兜底实际出现后再处理 |
| R8 | 中 | 两处 `Object.Instantiate/Destroy` 原生函数指针调用未传 `MethodInfo*`；当前非泛型实现通常不读取它 | 不处理 |
| R9 | 中 | 热切换记录中的原 Mesh/Material 是未 pin 的托管指针；若跨场景后仍用于恢复，可能还原错误、访问失效对象或崩溃 | 仅记录 |
| R10 | 低/更新风险 | 四个 Transform 变换函数对 assembly/class 解析结果无空检查；Unity API 漂移时可能空解引用 | 仅记录 |
| R11 | 低/更新风险 | `IsNativeObjectAlive` 解析失败时 fail-open 返回 true，会让失效对象防护失效 | 仅记录 |
| R12 | 低 | profile dump 用的静态 Mesh 属性缓存 map 无锁；并发 dump 时有数据竞争风险 | 仅记录 |
| R13 | 维护 | dynamicCollider 邻近注释互相矛盾，与实际 new 实例的代码不一致 | 仅记录 |
| R14 | 低/安全 | manifest 的 bundle 路径未阻止绝对路径或 `..`，本地 mod 可引用 mod 目录外文件 | 仅记录 |
| R15 | 低 | swing 的浮点参数写成字符串时整份 sidecar 解析失败，网格替换被跳过；标准导出包正常写数字 | 仅记录 |
| X1（原 P5） | 高/契约 | renderer 级 skeleton 未解析；特定双 Renderer 发型的 HairProp 会因骨数不匹配被跳过 | 仅记录；详见上方专节 |

## 日志、路径与管理器风险

| ID | 等级 | 触发条件与影响 | 当前决定 |
|---|---|---|---|
| N1 | 低 | 日志只追加不轮转；长期使用 info 级别会持续占用磁盘 | 仅记录 |
| N2 | 低 | `OutputDebugStringA` 的 4096 字节缓冲会截断超长调试行；日志文件仍完整 | 仅记录 |
| N3 | 低 | Runtime 根目录使用相对路径；宿主不是 `gakumas.exe` 时跳过 chdir，日志/mods 会落到任意 CWD | 仅记录；正常游戏宿主无影响 |
| M1 | 低 | 发型资源名规范化取第一个 `_hair`；异常命名中更早出现 `_hair` 时会截断过头、分组错误 | 仅记录 |
| M2 | 低 | 管理器托管调用参数固定 8 个，超过时静默截断；现有调用未超过 | 仅记录 |
| M3 | 低 | `ReadManagedList` 超过 4096 项会整段降级；未来 master 数据增长后列表可能缺失 | 仅记录 |
| M4 | 低 | UI probe Hook 没有 target 去重；两个托管方法落到同一原生目标时部分 Hook 安装失败 | 仅记录 |
| M5 | 低 | SEH 捕获异常时不执行 C++ 析构，faulted 路径可能泄漏克隆或局部资源 | 仅记录 |

## 使用与维护规则

1. 游戏客户端、Unity 或 IL2CPP metadata 更新后，优先复核 F1、F2、R6、R7、R8、R10、R11。
2. 新双 Renderer 发型出现 HairProp 不替换时先查 X1；不能只比较原始骨索引或哈希顺序。
3. 偶发加载崩溃、并行时更易复现或换场景后异常时，优先检查 R3、R4、R9、R12。
4. 新发现只在本文增加条目或更新状态；不要用临时 scratchpad、聊天结论或外仓文档代替本台账。
5. 若修复某项，保留条目并标注修复提交、测试范围和是否有实机证据。
