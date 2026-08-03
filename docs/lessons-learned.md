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

---

## 2. 已证伪的技术结论

| 曾经的结论 | 实况 |
|---|---|
| Renderer 已有的每材质 `MaterialPropertyBlock` 旧贴图覆盖了我们的克隆材质 | **错**。游戏在这些场景**从不调用** `Renderer.SetPropertyBlock`，`Material.SetTexture` 也从不写我们的材质。基于它的几轮修复改的是一条从未执行的路径 |
| 热 ON 后颜色错误是贴图覆盖不全 | **错**。manifest 只声明了实际存在的材质槽，日志的 `slots=2` 是完全覆盖 |
| 即时提交补丁没被调用 | **错**。调用点确实在替换之后 |
| `SettingTopScreenPresenter.SetEvent()` 是接管设置页的唯一 Hook 点 | **错**。当前 PC 的真实创建流程绕过或内联了它，必须用主线程 `EventSystem.Update()` 兜底 |
| 3DMigoto 时代的「shader 变体重排贴图槽位」对 AB 路线同样成立 | **错**。Unity 按属性名绑定，与寄存器槽位无关 |

真正的写入者是游戏在热重应用之后重新赋值材质数组，把带 Mod 贴图的私有材质换掉。
这条至今未修，当前保留「切一次页面即恢复」的基线。

---

## 3. 已撤回的实现尝试

都编译通过、都部署过，都因为让游戏更糟而回滚：

| 尝试 | 后果 |
|---|---|
| Hook `Renderer.SetMaterialArray_Injected` 底层 icall | 启动阶段对每个 Renderer 扫描 + 按托管三参数 ABI 调原生 icall → 加载卡住 |
| 改成原生两参数签名 + 受限扫描 | 点击 ON/OFF 直接崩 |
| 热恢复时枚举全场 Renderer | 在 Unity 更新角色层级期间做全局扫描 → 崩 |
| 直接拿已登记的 Prefab Renderer 指针做热恢复目标 | 崩。指针不能当场景实例用 |
| 热路径里调 `Object.IsNativeObjectAlive` 做存活检查 | 崩 |
| 管理器把 ON/OFF 放进帧末队列规避重入 | 只是为排查崩溃临时加的，问题不在这里，已撤回 |
| 四条 `Object::Internal_Clone/Instantiate` 热路径 Hook | 与暗色渲染问题无关（真因是环境残留），同日回退 |

**结论**：这条链上凡是「在 Unity 正在更新角色层级时做全局对象扫描」或「跨帧持有 Unity
对象指针」的做法都会崩。当前实现只使用刚从 `GetComponentsInChildren` 拿到的组件指针。

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
   互相打架。现在只有 `manager/README.md` 维护一行「当前」，release 的哈希由发布脚本写进
   release notes，其余文档一律不抄。
4. **同一事实不要在多份文档各存一份。** 实机证据时间线一度存在三份副本，改一处漏两处。

---

原始记录：热开关三缺陷的逐轮排查见
[`../manager/docs/OPEN_DEFECTS.md`](../manager/docs/OPEN_DEFECTS.md)；暗色渲染误判的完整
过程见 [`../AB_DARK_RENDERING_INVESTIGATION.md`](../AB_DARK_RENDERING_INVESTIGATION.md)；
UI 流程与安全边界见 [`../manager/docs/UI_FLOW.md`](../manager/docs/UI_FLOW.md)。
