# AssetBundle 暗色渲染贴图覆盖问题调查与 Runtime 修复总结

日期：2026-07-30

> ## ⚠️ 本文第 7、8 节的结论已作废
>
> **真因与 runtime 无关**：游戏目录 `ShaderFixes/` 里残留着 7 个 3DMigoto 研究期
> 留下的 `*_replace.txt`，其中 `f872756c910a6eb7-ps_replace.txt` 正是 body 的
> **「普通舞台光」pixel shader**。3DMigoto 会把它重新编译后顶替游戏原本的 DXBC，
> 导致舞台场景下整个身体的光照算术出错。移除后恢复正常。
>
> 本文第 1–6、9–11 节的测量数据仍然有效。第 8 节实现的四条 Unity 实例化 hook
> **已于同日回退**。证伪过程见第 12–13 节，真因见第 14 节，处置见第 15 节。
>
> 保留原文是为了记录「一条看似严密的推理链如何整条走偏」，不是为了保留结论。

## 1. 调查目标与约束

本次工作调查当前 AssetBundle 版本为什么重新出现旧 3DMigoto 方案中“暗色光照下
t0-t4 渲染槽发生变化”的问题，并判断能否通过新版 Blender 插件重新导出直接修复。
后续工作还包括更新 runtime，并根据游戏内测试截图判断修复效果。

调查和实现期间遵守以下约束：

- 前期研究只读取现有文档、Manifest、资源和日志；
- 不控制游戏或其他桌面程序；
- 不自动启动游戏；
- 不自动替换游戏目录中的 DLL；
- 不修改现有 AssetBundle、Blender 文件或 Blender 插件；
- 仓库整理工作尚未结束，因此不移动或删除现有文件；
- 允许在 workspace 同级创建独立临时调查目录。

## 2. 旧 3DMigoto 槽位问题与 AssetBundle 的区别

旧研究资料表明，3DMigoto 时代观察到的物理资源槽会随 shader variant 改变：

- 一种布局中 Base/Def/Shade 位于 t0/t1/t4；
- 另一种布局中可能移动到 t1/t2/t5；
- t4 还可能在部分 pass 中变成深度比较资源。

AssetBundle 方案不应继续依赖这些物理槽号。当前 Manifest 写入的是 Unity shader
property：

```text
_BaseMap
_DefMap
_ShadeMap
```

只要 runtime 通过 Unity Material API 设置这些语义属性，Unity 应负责根据当前
shader variant 将它们绑定到正确的物理资源槽。因此，AB 本身不需要理解当前 pass
中 `_ShadeMap` 最终对应 t4 还是 t5。

前期检查发现，原 runtime 只在替换 prefab 时调用一次 `Material.SetTexture`。游戏
之后仍可能通过以下路径改变材质状态：

- `MaterialPropertyBlock`；
- `Renderer.SetPropertyBlock`；
- Material clone；
- GameObject/prefab 实例化；
- 游戏自己的材质初始化或重设逻辑。

所以即使第一次写入 `_ShadeMap` 成功，live renderer 到暗色 pass 时仍可能重新使用
原角色材质中的贴图。

由此得到的第一阶段假设（**后续实机调查已推翻**）是：

- 不能依靠升级 Blender 插件、重新导出 AB 来通用修复；
- 现有 AB 的语义映射没有根本问题；
- 真正需要修复的是 runtime 对材质覆盖的生命周期管理；
- runtime 修复后，已有 AB 原则上不需要重新导出。

这组判断只代表调查中段的工作假设，不是本文最终结论。后续的品红探针和
`ShaderFixes/*_replace.txt` 清理证明，暗色异常的直接原因是 3DMigoto shader 替换残留，
不是 AssetBundle 或 Runtime 的材质生命周期。

## 3. 当前源码中的 PropertyBlock 修复路径

以下是当前源码保留的持久贴图覆盖路径。它已经通过编译和源码契约测试，但仍需目标
游戏版本的实机光照测试；这些代码是防止游戏回写贴图的防御路径，不能解释为已经修复
本文所述的暗色异常。

第一版 runtime 修复包含：

- 不再直接修改游戏共享材质，而是为目标 renderer 的每个材质槽创建私有 Material
  clone；
- 按 shader property ID 设置 `_BaseMap/_DefMap/_ShadeMap`；
- 按当前调用点修正 Unity 6 IL2CPP managed 方法的 ABI，补上需要的 `MethodInfo*` 参数；
- 按 Material、renderer 和材质槽记录永久贴图覆盖；
- hook `Renderer.SetPropertyBlock(MaterialPropertyBlock)`；
- hook `Renderer.SetPropertyBlock(MaterialPropertyBlock, int)`；
- 合并游戏已有的 PropertyBlock，而不是覆盖其中的动态参数；
- 正确处理 per-material PropertyBlock 高于 renderer-wide PropertyBlock 的优先级；
- hook `Material.SetTexture(int, Texture)`；
- hook `Material.SetTexture(string, Texture)`；
- **未**保留 `Object.Internal_CloneSingle` 等实例化 hook；它们属于已回退实验，不是当前修复；
- 添加 `Cloned private material` 和 `Persistent material textures active` 等诊断日志；
- 更新 README、roadmap 和测试。

该阶段的历史构建记录：

```text
警告：0
错误：0
测试：2 项通过
XInput 导出：8 个
DLL 大小：485,888 字节
SHA-256：EAC43EFBEF605DB9BCAB51A1DED595AA60F0F2FEB9A3255C6C99DE641B1108C3
```

这版 DLL 没有由调查过程自动安装或启动测试，之后由用户手动安装并在游戏内验证。

## 4. 第一版游戏内测试截图分析

测试截图中：

- 图 1、图 2 是强暗色或舞台光照；
- 图 3 是正常室内光照；
- 脸部正常；
- 头发正常；
- 泳装格纹、花朵和服装图案仍然存在；
- 只有 mod 身体材质的大面积皮肤在暗面变成灰绿或卡其色；
- 正常光照下问题基本不暴露。

这些现象说明问题不是：

- 整个 framebuffer 的色调变化；
- t0 与完全无关的纹理直接互换；
- UV 或网格整体损坏；
- BaseMap 本身丢失。

异常只在 shader 暗面分支占主导时出现，最可疑的是 `_ShadeMap` 在 live renderer
上没有继续保持为 mod 贴图。

## 5. 初始资源对象误判与纠正

由于截图是粉色格纹泳装，最初先检查了：

```text
pm.ttmr.madoka-swimsuit
```

该包的 Manifest 使用：

```text
_BaseMap  -> body_slot0_t0
_DefMap   -> body_slot0_t1
_ShadeMap -> body_slot0_t4
```

对该包的直接检查结果：

- t0/t4 为 sRGB；
- t1 为 Linear；
- 三张贴图均为 4096×4096 RGBA32；
- bundle 内原始像素和源 DDS 在垂直翻转后逐字节一致；
- 最大像素差值为 0；
- 平均像素差值为 0；
- t4 的皮肤区域为暖色，不是灰绿色；
- 皮肤区域 t0 平均 RGB 约为 `250.25, 229.81, 217.01`；
- 皮肤区域 t4 平均 RGB 约为 `224.35, 205.31, 194.25`。

随后读取实际游戏日志，发现这次测试真正加载的不是 Madoka 包，而是：

```text
chisaki-swimsuit
mdl_chr_atbm-cstm-0140_body
```

因此停止沿错误对象继续分析，改为检查实际安装的 Chisaki 包。

## 6. 实际安装的 Chisaki 包验证

实际安装位置：

```text
<游戏目录>\gakumas-local\local-files\mods\chisaki-swimsuit
```

该包包含两个材质槽，共六张语义贴图：

```text
slot 0: _BaseMap / _DefMap / _ShadeMap
slot 1: _BaseMap / _DefMap / _ShadeMap
```

直接读取安装中的 bundle 和 `bundle-src` PNG 后确认：

- slot 0 为 4096×4096；
- slot 1 为 2048×2048；
- t0/t4 的 `m_ColorSpace = 1`，即 sRGB；
- t1 的 `m_ColorSpace = 0`，即 Linear；
- TextureFormat 为 RGBA32；
- bundle 内嵌数据和 `bundle-src` PNG 垂直翻转后逐像素一致；
- 最大差值为 0；
- 平均差值为 0。

Chisaki slot 0 t4 alpha 统计：

```text
alpha 255：3,901,689 像素
alpha 0：12,875,527 像素
皮肤暗面分支覆盖率：约 23.26%
```

alpha 是干净的二值遮罩。可视化后，白色区域正确覆盖身体、手臂、腿部等皮肤 UV
岛。t4 RGB 也是正常的暖色皮肤和服装暗色，不包含测试截图中的统一灰绿色。

因此可以排除：

- AB 内写错 t4；
- PNG 转 bundle 时损坏；
- sRGB/Linear 设置错误；
- t4 alpha 被图像编辑器破坏；
- Blender 导出产生错误贴图；
- 重新导出 AB 即可修复。

## 7. Runtime 日志中的决定性证据（⚠️ 已作废，推理错误）

> 本节把「一个 hook 没装上」当成了「屏幕上贴图丢了」的证据，这两件事之间没有因果
> 关系。下面第 1–5 条推论全部不成立，理由见第 13 节。


检查的日志：

```text
<游戏目录>\gakumas-local\mod-plugin.log
```

并核对了游戏目录 DLL 和第一版构建 DLL 的哈希。

日志表明：

- 游戏中确实安装的是第一版 DLL；
- 游戏 DLL 和第一版构建 DLL 的 SHA-256 完全一致；
- 两个 `Renderer.SetPropertyBlock` hook 安装成功；
- 两个 `Material.SetTexture` hook 安装成功；
- runtime 成功创建了私有材质；
- runtime 成功加载并写入六张 t0/t1/t4；
- 但日志明确出现：

```text
Object.Internal_CloneSingle unavailable; cloned material tracking disabled.
```

并且整次运行没有出现：

```text
Persistent material textures active
```

这组旧日志只能证明当时的实验版状态，不能证明实例化 hook 是暗色问题的根因。当前结论是：

1. 贴图成功写到了被修改的 prefab renderer；
2. 当时的实例化登记 hook 没有触发；
3. 这不能单独证明 live renderer 的材质丢失，也不能证明 Runtime 是暗色异常根因；
4. 后续调查发现 `ShaderFixes/*_replace.txt` 中残留的 3DMigoto pixel shader 替换可以
   直接造成同样的暗色异常。

因此本文保留生命周期数据，但不再把它写成唯一因果结论。

## 8. 第二版 Runtime 修复（⚠️ 已回退）

> 本节描述的实现已于 2026-07-30 从 `src/runtime/ModRuntime.cpp` 移除。实机安装后
> 四条 hook 全部安装成功，但 `Restored persistent material textures on live clone`
> 与 `Persistent material textures active` 一次都没出现过 —— 它们从未触发，而且按
> 第 13 节的分析也不可能有用。


以下内容是已回退版本的历史设计，不是当前源码行为。

### 8.1 按 mod mesh 保存覆盖关系（历史）

当时的实验版在 prefab 完成 mesh 和贴图替换后，尝试保存：

```text
mod sharedMesh
    -> material slot
        -> property ID
            -> mod texture
```

这样后续恢复不再依赖 prefab Material 指针和 live Material 指针是否相同。

### 8.2 覆盖 Unity 6 的 managed 实例化路径（历史）

原实现只尝试通过 `il2cpp_resolve_icall` 获取：

```text
Object.Internal_CloneSingle
```

但 Unity 6 中实际可解析的是 managed IL2CPP wrapper。

检查游戏的 `global-metadata.dat` 后，确认存在：

```text
Internal_CloneSingle
Internal_CloneSingleWithParent
Internal_InstantiateSingle
Internal_InstantiateSingleWithParent
```

又使用 Unity 6000.0.67f1 的 `UnityEngine.CoreModule.dll` 核对了方法签名。

当时第二版覆盖：

```text
Internal_CloneSingle(Object)
Internal_CloneSingleWithParent(Object, Transform, bool)
Internal_InstantiateSingle(Object, Vector3, Quaternion)
Internal_InstantiateSingleWithParent(Object, Transform, Vector3, Quaternion)
```

所有 managed IL2CPP hook 均使用正确的 Unity 6 ABI，包括末尾的 `MethodInfo*`。

### 8.3 live GameObject 恢复流程（历史）

当时的实验版在上述方法返回新的 GameObject 后，尝试：

1. 遍历 clone 下的 `SkinnedMeshRenderer`；
2. 读取每个 renderer 的 `sharedMesh`；
3. 用 sharedMesh 查找此前登记的 mod 覆盖；
4. 对各材质槽重新调用 `Material.SetTexture`；
5. 把 live Material 重新加入永久覆盖表；
6. 对 live renderer 写入 per-material `MaterialPropertyBlock`；
7. 保留游戏已有的动态 PropertyBlock 参数；
8. 让后续 `SetTexture` 和 `SetPropertyBlock` hook 继续保护贴图。

新增的成功日志：

```text
Restored persistent material textures on live clone
Persistent material textures active
```

## 9. 第二版构建结果（历史记录）

第二版重新编译成功：

```text
警告：0
错误：0
测试：2 项通过
git diff --check：通过
XInput 导出：8 个
```

新 DLL：

```text
<仓库>\build\bin\x64\Release\xinput1_3.dll
```

属性：

```text
大小：492,544 字节
SHA-256：165418ECCC7D9D4D8784A4204669B397C5A951CDBFEE1815E9DE9F4CF431DE0E
```

调查结束时，游戏目录仍安装第一版 DLL：

```text
EAC43EFBEF605DB9BCAB51A1DED595AA60F0F2FEB9A3255C6C99DE641B1108C3
```

第二版只完成了静态验证和构建；它随后被回退，不能作为当前 DLL 或当前修复结果。

## 10. 修改的文件

当时修复提交直接修改了 `gakumas-mod-runtime` 中的：

```text
README.md
docs/roadmap.md
src/runtime/ModRuntime.cpp
tests/test_tools.py
```

并新增本文档：

```text
AB_DARK_RENDERING_INVESTIGATION.md
```

这些修改当前没有提交。（同日的后续回退又动了同一批文件，见第 15 节。）

没有修改：

- Chisaki AB；
- Madoka AB；
- Blender 文件；
- Blender 插件；
- AB exporter；
- `gakumas-modding` 主仓库代码；
- 游戏安装包内的 mod 文件。

## 11. 临时调查目录

按照允许的范围，在 workspace 同级创建了：

```text
<仓库同级>\gakumas-ab-dark-research
```

其中保存了：

- bundle 三张贴图的预览；
- t0/t1/t4 对照图；
- Chisaki slot 0 t4 alpha 可视化。

没有删除或移动现有仓库文件。

## 12. 第二版实机结果：修复未命中

用户手动安装第二版 DLL 后实机测试。日志（`gakumas-local/mod-plugin.log`）显示：

- 当时游戏目录 DLL 哈希 = 第二版构建产物，确认装对；
- 四条实例化 hook **全部安装成功**；
- 但 `Restored persistent material textures on live clone` 与
  `Persistent material textures active` **一次都没有出现**；
- 画面与第一版完全一致，暗面依旧。

即：第二版新增的代码路径从未执行，相对第一版等于什么都没做。

## 13. 证伪：生命周期方向本身就是错的

第 7 节的推理有一个逻辑漏洞，在拿到实机结果前就该发现：

**runtime 是就地改游戏原 prefab 的 renderer**（日志里的 `Replaced asset in-place`），
而 Unity 的 `Instantiate` 复制 GameObject 与 Component，但 Mesh / Material / Texture
是**引用共享，不克隆**。所以 live 实例指向的就是那两个已经带 mod 贴图的私有 Material，
没有任何东西需要"恢复"。

更硬的一条：六张贴图是**同一次调用、写进同两个 Material**。若 live 实例真丢了登记，
`_BaseMap` 会和 `_ShadeMap` 一起丢 —— 正常光照下的底色和服装图案就该是错的。而所有
截图都显示正常光照完全正常。**不可能只丢 `_ShadeMap` 而保住 `_BaseMap`。**

第 7 节把「`Object.Internal_CloneSingle` 这个 icall 在 Unity 6 上解析不到」当成了
「贴图在 live renderer 上丢失」的证据。前者只是一条 hook 没装上，与屏幕上画了什么
没有因果关系。

## 14. 真因：ShaderFixes 里的残留 shader dump

### 14.1 纯色底图探针

一路改贴图内容（t1.A、RampAdd、按原版比例重建 t4）**五轮全部零反应**之后，才做了本该
第一个做的实验：把 `_BaseMap` 整张刷成纯品红 `(255,0,255)`，t1/t4 保持原样。

结果：正常场景和坏场景**都显示品红**。

这一步同时判死两件事：

- mod 贴图在坏场景里**确实被采样**（绑定正常，第 7/8 节的方向彻底出局）；
- 坏场景里品红渲染成**暗掉、去饱和的梅子紫** —— 没有色相偏移，是整个身体被均匀压暗。
  所以正常肤色 → 卡其、白裙 → 橄榄灰、粉格纹 → 暗红，全都是同一个成因。

而脸和头发是 runtime 从未触碰的原版 renderer，始终正常 —— 断层就在脖子上。

### 14.2 元凶

游戏目录 `ShaderFixes/` 里躺着 **7 个 3DMigoto 研究期残留的 shader 替换**：

```text
f872756c910a6eb7-ps_replace.txt   ← body「普通舞台光」PS，元凶
f8f8f50ef76b02d5-ps_replace.txt
fe50b7a82b0f37be-vs_replace.txt   ← body 主 surface VS（仓库里出厂是 .disabled）
1150ec987484fd3e-vs_replace.txt
fbc9a56c0dae45b3-vs_replace.txt
e2b8a17e9cf78d4f-ps_replace.txt
ee805d96cf434bb2-ps_replace.txt
```

`f872756c910a6eb7` 与 `ShaderCache/` 里的原始反编译**字节完全相同**（53,285 bytes）——
它不是一份手改过的 fix，是**一份原样的 dump 被放进了 override 目录**。

这恰恰是最坑的一种。`[Rendering] override_directory=ShaderFixes` 会让 3DMigoto 用
d3dcompiler 把这份 HLSL **重新编译**，顶替游戏原本的 DXBC。重编译不可能与原始字节码
等价：反编译丢掉了 precise / fast-math 标志与精度限定，浮点重结合改变了算术结果。
于是贴图对、UV 对、几何对，只有**光照算术**不一样。

它只对被 dump 过的那个 hash 生效，所以只在用该变体的场景犯（房间场景走
`9ab6fcdf2237a70a` 亮主光，无覆盖 → 正常）；只影响 body（只有 body 的 shader 被
dump 过）；且完全不需要任何 Mods 包 —— 这就是为什么移除了 3DMigoto 的千咲 mod 之后
问题依旧。

来源是 `[Hunting] hunting=1` + `marking_actions = clipboard regex hlsl asm ...`：
研究期间每 mark 一次 shader，3DMigoto 就往 override 目录写一份 dump，下次 F10 或重启
即生效，此后永久存在。

### 14.3 为什么它能活过每一次重装

`mod-manager/installer/GakumasModManager.iss` 里：

```text
Source: "{#StageDir}\ShaderFixes\*"; DestDir: "{app}\ShaderFixes"; Flags: ignoreversion ...
```

`ignoreversion` 只覆盖清单里那 9 个原装文件，**从不清理目录里多出来的东西**；卸载时
Inno 也只删自己装过的文件。所以残留跨每一次装卸存活。

同一份脚本里 `d3dx.ini` 用的是 `onlyifdoesntexist`，导致 0.6.0 把默认值从
`hunting=1` 改成更安全的 `hunting=2`（HUD 默认关，也就断掉了继续产生残留的源头）
这个改进，对已有安装等于没生效。

这两条已单独立项跟进。

## 15. 处置

**已完成：**

- 7 个残留 `*_replace.txt` 与 2 个实验 `.hlsl` 移出到 `ShaderFixes-stray-backup/`；
- 第 8 节的四条 Unity 实例化 hook 及其配套设施（`InheritPersistentTextureOverridesForClone`、
  `RestorePersistentTextureOverridesOnRenderer/OnGameObject`、`RegisterRendererTextureOverridesForMesh`、
  `g_meshTextureOverrides`）已从 `src/runtime/ModRuntime.cpp` 移除，
  `tests/test_tools.py` 增加了防止重新引入的守卫测试；
- `docs/roadmap.md` 的 hook 清单已同步。

**保留：** 私有 Material 克隆、`Renderer.SetPropertyBlock` 与 `Material.SetTexture`
两组 hook 保持不动。它们是第一版的产物，开销低，且作为防止游戏回写贴图的兜底仍然合理。

## 16. 顺带查实的三条贴图管线缺陷

以下与本次渲染问题**无关**，是排查过程中用原版资产逐通道对照量出来的独立缺陷。原版
真值取自 `all_body/mdl_chr_atbm-cstm-0140_body`（AssetStudio CLI 导出 `bdy_col/def/sdw/rma`
+ `t_chr_atbm-base-0000_rmp`），按网格 UV 采样、以顶点 COLOR 的 `G_low` 分材质类。

**① ShadeMap 布料区大面积纯黑**

`t4.RGB` 恰好等于 0 的比例：slot0 布料 **57.67%**、slot1 布料 **81.83%**（皮肤区 0%）。
根因在 `05_finalize_textures.py`：`darken` 是逐 texel 的图（`type_aux.png` 的 G 通道），
材质分类没覆盖到的地方缺省 0，暗面直接算成纯黑。

**② 顶点 COLOR 的 G 字节被整字节覆盖**

`G_low` 选 `_RampAddMap` 的 LUT 行（该图 16 行分 6 组，各组内容完全不同：灰 / 紫 /
暖白 / 全零）。原版按材质分布在 `0:25.7% 3:21.2% 6:24.6% 9:11.5% 12:2.9% 15:14.0%`，
mod 网格 **`G_low` 全为 0（100%）**。`research/hair-shader-analysis.md` §10.1 明确警告过
不能为统一描边色覆盖整个 G 字节。

**③ 皮肤校准目标偏黄**

`04_bake_and_sample.py` 用启发式 mask 从一张抓帧 DDS 采样：

```python
mask = (r > 0.6) & (r > g) & (g > b) & ((r - b) > 0.02) & ((r - b) < 0.35) & (g > 0.4)
```

`g > b` 与 `r - b > 0.02` 两个条件按定义排除了所有 `b >= g` 的皮肤 texel，同时捞进了
大量暖色布料。结果 `target_srgb × 255 = (236.6, 220.3, 203.2)`，G−B = 17；原版身体
皮肤为 `(226, 206, 199)`，G−B = 7。

**原版实测真值表**（供修 ①③ 用，`t4/t0` 为线性比）：

| 材质类 (G_low) | t0 RGB | t4/t0 (R,G,B) | t1 R/G/B/A | t4.A |
|---|---|---|---|---|
| 皮肤 (0) | 226, 206, 199 | 0.843, 0.847, 0.875 | .370 / .534 / .056 / .043 | 0.74 |
| 深紫布 (3) | 74, 64, 125 | 0.326, 0.337, 0.385 | .216 / .363 / .016 / .044 | 0 |
| 棕 (6) | 159, 125, 83 | 0.449, 0.389, 0.377 | .231 / .595 / .381 / .261 | 0 |
| 灰 (9) | 131, 128, 133 | 0.325, 0.342, 0.406 | .135 / .437 / .030 / .037 | 0 |
| 紫 (12) | 63, 49, 113 | 0.345, 0.420, 0.465 | .154 / .600 / .235 / .151 | 0 |
| 白料 (15) | 171, 170, 171 | 0.351, 0.361, 0.457 | .273 / .556 / .025 / .112 | 0 |
| **mod 现用** | — | **0.45 / 0.45 / 0.45（标量）** | 皮肤 .45/.13/0/0 · 布料 .38/.17/0/0 | 1 / 0 |

两点系统性偏差：原版暗面是**冷的**（B 比例约为 R 的 1.2–1.3 倍），mod 用纯标量
`darken`（`hueShiftDeg=0 / satScale=1`）产出中性灰；原版 smoothness 在 0.36–0.60，
mod 全身只有 0.13–0.17。

## 17. 方法论教训

1. **改内容前先做纯色探针。** 五轮贴图实验零反应，本身就是「问题不在贴图内容」的
   强信号，但直到第六轮才去验。任何「改 X 没反应」的排查，第一步应该是用一个不可能
   看错的极端值证明 X 确实在被读。
2. **「某个 hook 没装上」不是画面证据。** 日志里缺一行和屏幕上少一张贴图之间需要一条
   完整的因果链，第 7 节跳过了这一步。
3. **先清点环境里所有能改渲染的东西。** 游戏目录里 `d3d11.dll`（3DMigoto）与
   `xinput1_3.dll`（AB runtime）并存，整个调查却只盯着后者。
4. **同一个平台上的旧结论要分清哪一半可转移。** 3DMigoto 时代的槽位重排结论对 AB
   路线不成立（Unity 按属性名绑定，品红探针已实测证明），但它底下那张
   「body 有约 28 个 PS 变体、按光照切换」的事实是这次定位的关键线索。

## 18. 2026-08-01 热开关后的颜色刷新问题

这是后续 Runtime 热开关改造暴露的**独立问题**，不能与第 14 节的 3DMigoto ShaderFixes
残留混为一谈。

### 18.1 现象与边界

- Mod 在启动时已经开启，第一次进入主页显示正常；
- 游戏内 OFF/ON 热切换本身生效，Mesh、材质和目标资源重新应用；
- 热 ON 后直接返回主页时颜色错误；
- 再切换一次游戏页面，画面恢复正常；
- Runtime 日志记录热重应用 `targets=2, applied=2, refreshedRigs=1`。

这些证据把问题限制在“已实例化 Renderer 的状态提交”，而不是 Bundle 未加载、replacement
map 未更新或 Mesh 没有重新应用。页面切换能恢复也说明游戏自身的下一轮 Renderer 初始化会
补上缺少的状态。

### 18.2 已确认原因

实机互补探针已经证伪“旧 `MaterialPropertyBlock` 覆盖克隆材质”：这些场景没有调用
`Renderer.SetPropertyBlock`，`Material.SetTexture` 也没有向 Mod 材质写入。真实写入者是游戏
在热重应用之后调用 `Renderer.set_sharedMaterials` / `set_materials`，把带 Mod 贴图的私有
材质数组换回原始数组。页面切换之所以恢复，是因为下一轮页面初始化重新走了完整替换路径。

旧修复只做了：

- Bounds 重置；
- Renderer enabled 状态刷新；
- 活动 Rig 目标节点停用/启用；
- 动态骨重新注册。

这些操作足以刷新 Mesh 和骨骼，但无法阻止游戏随后写回原始材质数组。

### 18.3 当前处置

当前 `ModRuntime.cpp` 保留每个原 Renderer 完成热重应用后的持久贴图覆盖逻辑：

```text
ApplyPersistentTextureOverrides(pair.originalRenderer)
```

它先读取并保留游戏现有 Block 的肤色、遮罩、光照等字段，再只覆盖 Manifest 声明的 Texture
property，并按材质槽提交回 Renderer。不能用 `SetPropertyBlock(null, slot)` 清空整个 Block，
否则会破坏游戏自己的角色外观参数。

IDA 后加入的底层材质数组钩子及后续受限 Renderer 扫描连续导致加载卡住或 ON/OFF 崩溃，
现已撤回，源码恢复为 `20260802-041055` 备份里的调查前实现（该备份的 Runtime 为 `570880`
字节、SHA-256 `F88C74F5FFB5114FE20B622AD82A1C877C6461725C95031419EF4E6A8065A969`——那是**当时
那份备份**的哈希，不是当前部署版的）。基线是热切换不崩溃，但直接回主页的颜色仍可能需要
切换页面刷新。当前部署版的哈希见 [`README.md`](README.md)。

### 18.4 与旧暗色问题的区分

| 问题 | 根因 | 是否依赖热开关 | 页面切换是否恢复 | 当前处置 |
|---|---|---:|---:|---|
| 第 14 节场景暗色 | 3DMigoto `ShaderFixes` 中重编译的残留 Shader | 否 | 否 | 残留已移出 |
| 本节热 ON 颜色错误 | 游戏材质数组写回换回原始材质 | 是 | 是 | 即时修复实验已撤回，保留切页刷新基线 |

今后出现颜色异常时，先记录是否经过热开关、页面切换能否恢复、Renderer/MPB 日志，再决定
进入哪条调查路线，不能因为两者都表现为“颜色不对”就复用同一结论。
