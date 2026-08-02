# 当前缺陷、修复状态与已排除结论

> 最后更新：2026-08-02
> 本文记录三个缺陷的现象、已经做过什么、**每次实机排除掉了什么**、当前修复状态和下一步。
>
> **当前只剩缺陷一。** 缺陷二（星形分隔）与缺陷三（橙条长度）的修复已由用户实机确认通过，
> 两节保留作排查记录，不要再当成待办。
>
> 目的是让接手者不要重复已经被证伪的假设。本轮修复已落到本地 Release 构建，
> 但本轮尚未启动游戏做最终验收；“已实现”不等于“已实机确认”。

## 缺陷一：热开关后主页服装颜色错误【未解，切一次页面即恢复】

### 现象

主页 → 菜单 → Mod 管理 → 开关 → **直接返回主页**：网格是 Mod 的，颜色是原版的。
打开换装页（颜色正常）后再回主页，主页也正常。用户确认「修了很多次都是这样」。

### 已排除（每条都有实机证据）

| # | 曾经的结论 | 如何被证伪 |
|---|---|---|
| 1 | 贴图覆盖不全 | manifest 只声明材质槽 0/1，日志 `slots=2` 是**完全覆盖** |
| 2 | 即时提交补丁没被调用 | `ApplyPersistentTextureOverrides` 确实在 `ApplySkinnedMeshReplacement` 内、贴图替换之后调用 |
| 3 | `Persistent material textures active` 的有无能说明问题 | 该日志被 `g_loggedPersistentRenderers` 按 renderer 去重，只打第一次 |
| 4 | **旧 `MaterialPropertyBlock` 覆盖克隆材质** | 探针加了「有 block 提交但无 mod 覆盖」的互补日志，整个会话 **0 条**；两个 `Renderer.SetPropertyBlock` 钩子确认已安装。**游戏在这些场景根本不调 SetPropertyBlock** |
| 5 | 游戏往我们的材质写贴图 | `Material.SetTexture` 钩子加一次性日志，**0 条** |

> 结论 4 曾被写进 `gakumas-mod-runtime` 的 `README.md`、`docs/roadmap.md`、
> `docs/runtime-api-v1.md` 和本仓库多处，**是错的**，已在各处标注。基于它的几轮
> Runtime 修复改的是一条从未执行的路径，这就是「修了很多次没有变化」的直接原因。

### 已确认的事实

```text
[ModAsset] Hot reapply target: object=… name="Root_Body"                   ← 场上角色
[ModAsset] Hot reapply target: object=… name="mdl_chr_atbm-cstm-0140_body" ← 缓存 Prefab
[ModAsset] Game reassigned materials on a patched renderer:
           renderer=000002659C6F1540 name="Geo_Body" via=set_sharedMaterials
```

**游戏在热重应用之后调用 `Renderer.set_sharedMaterials`，换掉带 Mod 贴图的私有材质。**
这是五轮里唯一被日志抓到的写入者。切换页面之所以恢复，是因为那条路重走了完整替换。

附带确认：`Renderer.set_materials` 与 `set_sharedMaterials` 解析到**同一个函数地址**
（第二次 `MH_CreateHook` 返回 `MH_ERROR_ALREADY_CREATED`），一个钩子即可覆盖。

### IDA 进一步确认（2026-08-02）

`dump.cs` 与 IDA 的调用链已经把“谁在写”缩小到游戏自己的材质数组提交：

```text
VLActorFaceModel.UpdateSharedMaterials()         0x0A88A6F4-0x0A88A754
  → sub_7ABADF0
  → UnityEngine.Renderer::SetMaterialArray_Injected

CampusActorModelParts.AddCombinedOpaqueSubMesh() 0x03A06770-0x03A06E4C
  → sub_A380B70 (call site 0x03A06DBC)
  → UnityEngine.Renderer::SetMaterialArray_Injected
```

`sub_A380D00` / `sub_A380B70` 都是 `SetMaterialArray_Injected` 的包装；它们不是新的
覆盖机制，而是 `Renderer.set_sharedMaterials` 在 IL2CPP 里的实际底层写入路径。前者
没有静态 C++ caller（通过元数据/虚调用进入），后者的 `CampusActorModelParts` caller
已由 IDA 交叉引用确认。最直接的下一步是围绕 `VLActorFaceModel.UpdateSharedMaterials`
验证热重应用时序，并在该写入完成后补回 Mod 材质；继续只追 `SetPropertyBlock` 不会到达这条路径。

### 上一次修复为什么没生效

在钩子里用 `g_reversibleRendererPatches` 按 `patchedRenderer == renderer` 找补丁，
**没有匹配**，于是静默返回，`Restored mod materials` 一条都没打。而同一个 renderer
在 override 缓存里是**命中**的。所以两套注册表指向的 renderer 不是同一个——很可能
一套记的是 Prefab 上的，另一套是场景实例上的。当前构建已把两个查找结果都打进日志：

```text
Materials reassigned on renderer with overrides: … registryMatch=? keys=? patches=?
```

### 当前修复（待实机验收）

1. Runtime 现在钩住 `Renderer.set_sharedMaterials` 和 `Renderer.set_materials`，在原生写入后
   按 renderer、原材质数组或当前 Mod Mesh 匹配可逆注册表，并用完整的 Mod 材质数组恢复。
   本轮日志确认这两个托管 setter 没有在热重应用时触发，真实调用落在 IDA 已确认的
   `Renderer.SetMaterialArray_Injected`，因此最新构建又增加了该底层写回钩子；
2. 恢复调用对应的原始 setter，并用 `thread_local` 防止递归；
3. 04:13 的实机启动在首轮角色资源加载后停止，日志没有进入主页；该部署版新增的底层钩子
   会在启动阶段对每个 Renderer 执行扫描，且把 Unity 原生 icall 按托管方法的三参数 ABI
   调用，已判定为本轮加载卡住的回归点；
4. 当前部署修正为原生两参数签名，启动阶段完全旁路，只有用户实际执行热 ON/OFF 后才
   arm，并且先用已登记 Renderer 的指针做 O(1) 筛选，再进入材质恢复；
5. 04:28 实机点击 ON/OFF 后，WER 记录为 `UnityPlayer.dll` 的 `0x80000003`（Unity
   debug-break）；Manager 没有拿到 `setModEnabled` 返回日志，说明同步调用发生在
   `CampusButtonBase.OnClicked` 回调栈内，重入了游戏渲染更新。
6. 当前 Manager 修复为：按钮回调只把 `{modId, enabled}` 放入队列，先让原生点击回调返回，
   再由下一次 `EventSystem.Update` 完成 Runtime 热切换和 UI 状态回写；离开/重建设置页时
   会清空队列，避免使用失效的行控件指针。已编译并部署，待实机复测 ON/OFF。
7. 04:44 复测在进入热恢复后立即崩溃，新增日志确认崩溃发生在恢复函数内部，且尚未输出
   `Hot-restored renderer`。优先移除热恢复/热重应用的全场 Renderer 枚举，仅使用已登记
   Renderer、已记忆源节点和已捕获角色根节点；避免在 Unity 正在更新角色层级时调用全局对象扫描。
   已重新编译部署，待再次实机复测。
8. 05:39 复测已输出 `Hot restore renderer registry prepared: patches=1 renderers=1` 后崩溃，
   说明仅移除全场扫描仍不够，登记的 Prefab Renderer 指针不能直接作为热恢复目标。当前构建
   改为从已观察的 `ActiveAnimationRig` 角色根节点及记忆源节点收集 Renderer，不再直接触碰
   `patch.patchedRenderer`；同时保留逐 Renderer 诊断日志。已重新编译部署，待再次实机复测。
9. 05:47 复测已进入活动角色 Renderer 列表，并在 `Hot restore inspecting renderer=...` 后崩溃，
   位置落在热路径的 `Object.IsNativeObjectAlive` 检查。当前构建移除该不稳定检查，只处理刚从
   `GetComponentsInChildren` 得到的组件指针，并继续记录后续匹配/恢复步骤。已重新编译部署。
10. 用户要求精确恢复 IDA MCP 调查前的 ON/OFF，而不是关闭热应用。已用相邻备份确认边界：
    `20260802-041055` 保存的 Runtime（原文件时间 03:58:44）没有
    `SetMaterialArray_Injected`，但包含 `Hot-restored renderer`、`Hot reapply target` 和
    `Session toggle applied`；下一份 `20260802-042339` 才首次出现底层钩子。当前已部署前者，
    并把源码恢复为原来的全场 Renderer 热恢复/热重应用；`kEnableLiveToggleApply`、底层 icall
    钩子、受限扫描和崩溃诊断日志均已移除。
11. Manager 只撤回 IDA 后为崩溃排查加入的帧末 ON/OFF 队列，恢复同步 `HandleModToggle`；
    全屏页面、橙条尺寸、星形分隔、缩略图、菜单入口和幂等导航修复全部保留。

## 缺陷二：底栏多一颗星形分隔图标【已修复，实机确认】

### 现象

设置页原本三栏两颗星。改成两栏后仍是两颗：一颗在「服装」「发型」之间但**偏左不居中**，
一颗贴在右边缘。

### 已排除

| # | 假设 | 证据 |
|---|---|---|
| 1 | 星形是 tab bar 的兄弟节点，夹在按钮之间 | 子节点只有 `Background`、四个 `CampusSimpleTabButton`、`Overlay`，**没有分隔符节点** |
| 2 | 隐藏被藏按钮前面那个兄弟即可 | 那个兄弟是 `CampusSimpleTabButton (4)`（备用按钮），隐藏它星形无变化，已撤销 |
| 3 | 星形在 `Background` 或 `Overlay` 里 | `Background` 无子节点；`Overlay` 只有 `SelectImage`（橙条本体） |

### 实机拿到的层级

```text
tab bar
├─ Background                      （无子节点）
├─ CampusSimpleTabButton      服装  ├─ TouchArea / ScaleRoot / Light
├─ CampusSimpleTabButton (1)  发型  ├─ TouchArea / ScaleRoot / Light
├─ CampusSimpleTabButton (4)  备用  ├─ TouchArea / ScaleRoot / Light
├─ CampusSimpleTabButton (2)  培育  ├─ TouchArea / ScaleRoot / Light   （已隐藏）
└─ Overlay                         └─ SelectImage                      （橙条）
```

### 当前最可能的原因

**每个按钮内部的 `Light` 就是那颗星。**它解释了全部现象：

- 三栏时：`基本设置.Light`、`声音.Light` 可见，`培育.Light` 被药丸右端裁掉 → 看到 2 颗；
- 两栏时：`服装.Light`、`发型.Light` 都落在可见区内 → 仍看到 2 颗，且第二颗贴右边缘；
- 第一颗偏左，是因为它跟着**按钮**走而不是跟着两栏分界走。

代码已在组合 Mod 页面时隐藏「发型」按钮（`CampusSimpleTabButton (1)`）的 `Light` 子节点
（`CampusUiProbe.cpp` 的 `HideTabButtonLight`）。**2026-08-02 用户实机确认星形已减少到一颗，本缺陷关闭。**

## 缺陷三：橙色选中条长度仍是三栏比例【已修复，实机确认】

### 现象

首次进入 Mod 管理页，橙条长度按三栏算。**手动点「发型」再点回「服装」后长度正确。**

### 已排除

| # | 假设 | 证据 |
|---|---|---|
| 1 | 没找到 `CampusSimpleTabButtonGroup` | `no tab button group matched` 从未出现，group 每次都找到 |
| 2 | `SetSelectIndex` 没被调用 | 连调 8 帧，日志确认执行 |
| 3 | 调用太早、布局未落地 | 延后到 `EventSystem.Update` 连续 8 帧重算，无变化 |
| 4 | 同索引早退，需要 0→1→0 | 改成先切 1 再切 0，无变化 |

### 当前最可能的原因（来自 `dump.cs`，尚未实机验证）

```csharp
public abstract class CampusTabButtonGroupBase<T, TTabButton> : QuaUIBehaviour {
    protected TTabButton _firstTabButton;
    protected TTabButton _lastTabButton;
    protected List<TTabButton> _middleTabButtons;
    public IReadOnlyList<TTabButton> ActiveTabButtons { get; private set; }
    public override void Initialize();
}

public class CampusSimpleTabButtonGroup : CampusTabButtonGroupBase<…> {
    private float _barMargin;                       // 0x50
    private UnityEngine.UI.Image _selectedBarImage; // 0x58
    public RectTransform SelectedBarRect { get; }
    public float GetBarSize();
    public Vector3 GetButtonBarPosition(int index);
}
```

**把第三个按钮的 GameObject 设为 inactive，并不会改变 `ActiveTabButtons`。**
该列表由 `Initialize()` 从 `_firstTabButton` / `_middleTabButtons` / `_lastTabButton`
计算，是序列化结构而不是运行时可见性。因此 `GetBarSize()` 很可能仍按三（或四）栏算，
无论我们调多少次 `SetSelectIndex` 都是同一个结果——这与「四种调用方式全部无效」吻合。

### IDA 已确认（2026-08-02）

- `CampusSimpleTabButtonGroup.Initialize()`：`0x02459A7C-0x02459C88`；先调用基类初始化，
  然后对 `SelectedBarRect` 执行 `RectTransform.set_sizeDelta_Injected`。
- `CampusSimpleTabButtonGroup.SelectedBarRect` getter：`0x024599C0-0x02459A7C`。
- `CampusSimpleTabButtonGroup.GetBarSize()`：`0x02459C88-0x02459D78`，从 `a1 + 0x48`
  的活动按钮集合取宽度，再扣 `2 * _barMargin`（`_barMargin` 在 `0x50`）。
- `SetSelectIndex()` 的辅助链最终调用 `Transform.set_position_Injected`，不改
  `SelectedBarRect.sizeDelta`；因此继续调用它或延后调用都不能修复橙条长度。

这把修法从“重算原生 group”收敛为：取得 `SelectedBarRect`，读取现有 `sizeDelta`，只改 x
为两栏布局宽度，并保留 y。初始化时机仍需在布局完成后验证一次。

### 当前修复（已实机确认）

> 2026-08-02 用户实机确认橙条长度已按两栏正确显示，本缺陷关闭。


1. 已绕开 `GetBarSize()`：布局完成后取得 `SelectedBarRect`，读取现有 `sizeDelta`，只改 x
   为两栏宽度 `492 - 2 × 56 = 380`，保留 y；随后重新调用一次 `SetSelectIndex(0)`，
   修正首次进入时仍按三栏位置计算的左偏；连续刷新只用于等待布局稳定。
2. 启动游戏首次进入 Mod 管理页，确认橙条不再按三栏比例，并确认切换服装/发型后仍保持正确。

## 通用教训

1. **「日志说做了」不等于「做成了」。**本文三个缺陷各有一次栽在这上面：
   `menuClosed=1` 而菜单还开着、`dangling tab separator hidden` 而隐藏的是备用按钮、
   `Restored mod materials` 因为查找不匹配而一条都没打。
2. **去重日志不能当作行为证据。**按对象去重的一次性日志只能证明「第一次发生过」。
3. **写探针要写互补面。**「有覆盖时记录」只能证明存在，加上「无覆盖时也记录」才能
   证伪整条路径。缺陷一的关键转折正是补上了互补日志。
4. 隐藏 GameObject 不等于改变组件的逻辑集合（缺陷三）。
