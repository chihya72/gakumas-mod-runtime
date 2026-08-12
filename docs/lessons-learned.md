# 反面教训汇总（Runtime + 管理器）

这份文档只记**做错过什么、为什么错、结论是什么**。证据、日志和逐轮排查过程留在各自的
原始文档里，这里不重复。接手前先读一遍，可以省掉已经交过的学费。

---

## 1. 排查方法

1. **「日志说做了」不等于「做成了」。** 三个缺陷各栽过一次：`menuClosed=1` 而菜单还开着、
   「悬空分隔符已隐藏」而隐藏的是备用按钮、`Restored mod materials` 因为查找不匹配而一条
   都没打。日志证明代码走到了那一行，不证明那一行达成了目的。
2. **去重日志不能当行为证据。** 按对象去重的一次性日志只能证明「第一次发生过」，不能证明
   「每次都发生」，更不能证明「没打就是没发生」。
3. **探针必须写互补面。** 只在「命中时记录」只能证明存在；加上「没命中时也记录」才能证伪
   整条路径。热开关颜色缺陷的关键转折就是补上了互补日志——一整个会话 0 条，直接判死了
   五轮修复所依赖的前提。
4. **改内容前先做极端值探针。** 连续五轮改贴图零反应，本身就是「问题不在贴图内容」的强
   信号，但直到第六轮才拿纯品红底图去验。任何「改 X 没反应」的排查，第一步应该是用一个
   不可能看错的值证明 X 确实在被读。
5. **先清点环境里所有能改渲染的东西。** 游戏目录里 3DMigoto 的 `d3d11.dll` 与本 runtime 的
   `xinput1_3.dll` 并存，而整个调查只盯着后者——真因在前者。
6. **隐藏 GameObject 不等于改变组件的逻辑集合。** 把按钮设为 inactive 不会改变原生组件从
   序列化字段算出来的 `ActiveTabButtons`，所以按它算的尺寸不会跟着变。
7. **抓帧要比内容 hash，不是文件名 hash。** 3DMigoto 文件名里的 `ps-cb0=a298507e` 是
   **资源创建时**的 hash；常量缓冲每帧 Map 重写，这个值永远不变。两帧的常量缓冲看着
   「一模一样」纯属错觉。真正的内容 hash 在 `log.txt` 的
   `Dumping ... -> FrameAnalysisDeduped\XXXX.buf` 里。这个坑让「两帧完全相同」的错误结论
   连续成立了两轮。
8. **输入全都一样而输出不同，说明还有没比到的输入。** 不要因此怀疑对方的操作。本轮就是
   把 `vb0` 的差异当成「姿势不同」放过了——它同时装着 POSITION/NORMAL/TANGENT，法线的
   差异就藏在里面。用户坚持两次抓帧确实是亮和暗，这个坚持是对的。
9. **需要看输出就把输出 dump 出来。** `analyse_options` 加 `dump_rt` 之后，一份抓帧就能
   回答「这个 draw 画完的瞬间已经错了没有」，比反复对比输入快一个数量级。验完记得去掉。
10. **自己装的 Hook 会拦自己的还原写入。** `Material.SetTexture` 的 Hook 会把参数换成已
    登记的 Mod 贴图，于是「还原原始贴图」的调用返回成功、日志打印成功、内容纹丝不动。
    还原前必须先注销 override。这是第 1 条的又一个实例。

---

## 2. 已证伪的技术结论

| 曾经的结论 | 实况 |
|---|---|
| Renderer 已有的每材质 `MaterialPropertyBlock` 旧贴图覆盖了我们的克隆材质 | **错**。游戏在这些场景**从不调用** `Renderer.SetPropertyBlock`，`Material.SetTexture` 也从不写我们的材质。基于它的几轮修复改的是一条从未执行的路径 |
| 热 ON 后颜色错误是贴图覆盖不全 | **错**。manifest 只声明了实际存在的材质槽，日志的 `slots=2` 是完全覆盖 |
| 即时提交补丁没被调用 | **错**。调用点确实在替换之后 |
| `SettingTopScreenPresenter.SetEvent()` 是接管设置页的唯一 Hook 点 | **错**。当前 PC 的真实创建流程绕过或内联了它，必须用主线程 `EventSystem.Update()` 兜底 |
| 3DMigoto 时代的「shader 变体重排贴图槽位」对 AB 路线同样成立 | **错**。Unity 按属性名绑定，与寄存器槽位无关 |
| 热 ON 颜色错误的真因是游戏在热重应用之后重新赋值材质数组 | **错**。抓帧对比证伪：暗色帧和正常帧的身体 draw 在 Mesh、8 张贴图、材质常量、shader 变体和渲染状态上逐字节相同。这条错误结论撑了五轮修复 |
| 热路径也该像冷路径那样克隆私有材质 | **错**。活体角色的材质是游戏自己维护的 per-actor 拷贝，换成克隆等于把 renderer 从游戏的更新链上摘下来。热路径应当直接写游戏那份，并快照原贴图供 OFF 还原 |
| 可以调 `CampusActorModelParts.InitializeCampusMaterials()` 刷新派生材质状态 | **错**。在活体 actor 上重跑会把整个材质数组换成 shader 无属性的空材质（审计打出 `keywords=[] floats=[]`），角色变洋红，后续开关全废。已从源码删除并留注释 |
| 放掉 gchandle 就等于释放了克隆的 Mesh | **错**。Mesh 是原生 Unity 对象，GC 只管托管包装。必须 `Object.Destroy`，否则每次热 ON 泄漏约 16 MB |
| `hotInstances=0` 表示热 ON 失败，只能让玩家进一次换装页 | **错**。切换发生在管理页时，目标 Renderer 可能尚未创建。当前实现按 `modId + source` 排队，并由 `RegisterBones`/Renderer 生命周期重试；资源路径若已先应用，会以 `alreadyPatched=1` 安全清队列 |

**真因（2026-08-09 定案）**：`TransformModMeshVerticesToOriginalRendererSpace` 换空间时
只搬顶点，法线和切线留在原地。冷路径两端都是 prefab 单位阵，空操作；热路径的目标是有真实
旋转的活体角色，于是法线与几何脱节。量化见 [`roadmap.md`](roadmap.md)。

---

## 3. 已撤回的实现尝试

都编译通过、都部署过，都因为让游戏更糟而回滚：

| 尝试 | 后果 |
|---|---|
| Hook `Renderer.SetMaterialArray_Injected` 底层 icall | 启动阶段对每个 Renderer 扫描 + 按托管三参数 ABI 调原生 icall → 加载卡住 |
| 改成原生两参数签名 + 受限扫描 | 点击 ON/OFF 直接崩 |
| 旧版热恢复在 Unity 更新角色层级期间扫描并继续使用缓存对象 | 与跨帧缓存的 Renderer/GameObject 指针混用 → 崩；不能由此推导“任何当前快照枚举都不安全” |
| 直接拿已登记的 Prefab Renderer 指针做热恢复目标 | 崩。指针不能当场景实例用 |
| 热路径里调 `Object.IsNativeObjectAlive` 做存活检查 | 崩 |
| 管理器把 ON/OFF 放进帧末队列规避重入 | 只是为排查崩溃临时加的，问题不在这里，已撤回 |
| 四条 `Object::Internal_Clone/Instantiate` 热路径 Hook | 与暗色渲染问题无关（真因是环境残留），同日回退 |
| 热重应用后调 `CampusActorModelParts.InitializeCampusMaterials()` | 材质数组被换成空 shader 材质，角色洋红，之后所有开关失效 |
| 活体路径继续灌每材质 `MaterialPropertyBlock` | 贴图已经写在材质上，这层多余；已只在冷路径保留 |

**结论**：不能跨帧信任缓存的 Unity 场景对象指针。当前实现每次只使用
`FindObjectsByType<SkinnedMeshRenderer>` 返回的当前快照；生命周期 Hook 传入的 observed renderer
也只在本次调用中检查，绝不放进延迟队列。另一个边界是：**不要在活体 actor 上重跑游戏自己的
初始化方法**——它们假定的是构建期上下文；新增摇物链应在 prefab graft 阶段完成。

---

## 4. 已废弃的 UI 路线

以下不得接回 Release 主路径：

- 在主页 Canvas 上放大 `MenuSubButtonView` 当作管理面板（只有文字、没有合格背景）；
- `Time.get_deltaTime` / `CampusActorController.LateUpdate` 每帧搜索 Presenter；
- 借 `HomeTopScreenPresenter.OpenNoticeSheetAsync` 或 `ErrorSheetManager.OpenAsync` 挂未经
  验证的托管委托；
- 伪点击真实设置按钮的 `CampusButtonBase.OnClicked`；
- 把整数 `MenuButtonType` 写进 `_subButtons` 的对象键（键是 `MenuButtonSerializeType` 对象）；
- 仅靠 `SettingTopScreenPresenter.SetEvent()` 接管页面；
- 按启用状态排序列表——开关一下条目就换位。

---

## 5. 环境与工程陷阱

1. **游戏目录 `ShaderFixes/` 里的残留会静默换掉 shader。** 3DMigoto 研究期留下的 7 个
   `*_replace.txt` 会被重新编译后顶替游戏原本的 DXBC，其中一个正是 body 的主舞台光
   pixel shader——表现为整身被均匀压暗，而脸和头发（runtime 从不触碰）正常，断层在脖子上。
   **渲染诡异且改贴图零反应时，先去数游戏目录的 ShaderFixes。**
2. **IL2CPP 重载必须按参数类型名解析。** 按「名字 + 参数个数」找会取到错误的重载并静默
   失败（同 argc 的重载很常见）。
3. **手抄的 DLL 大小和 SHA-256 一定会过期。** 这个仓库为此栽过三次，两份文档的哈希还能
   互相打架。当前值只以 release notes 或目标文件现场 `Get-FileHash` 为准，其余文档一律不抄。
4. **同一事实不要在多份文档各存一份。** 实机证据时间线一度存在三份副本，改一处漏两处。
5. **不要用 SEH 把数据结构不变量错误包装成“可恢复”。** 旧实现向已经初始化好的
   `swingDynamicBones/swingChains` 追加数据，却没有同步 `initialTransforms` 等并行表，导致
   `RegisterBones` 越界；SEH 只能阻止整次 toggle 中断，不能让链成立。当前修法是在 prefab graft
   阶段建骨建链，让游戏初始化一次性收走，相关 SEH 兜底已删除。
6. **Unity 原生对象不归 GC 管。** Mesh、Material 这些 `Instantiate` 出来的对象，放掉
   gchandle 只让托管包装可回收，原生内存要 `Object.Destroy` 才还。

---

已解决缺陷的真因与当前边界见 [`roadmap.md`](roadmap.md)；UI 流程与安全边界见
[`../manager/docs/UI_FLOW.md`](../manager/docs/UI_FLOW.md)。逐轮排查日志、暗色渲染误判的
完整过程和独立入口 DLL 的证据已随本文合并删除，需要时从 git 历史取。
