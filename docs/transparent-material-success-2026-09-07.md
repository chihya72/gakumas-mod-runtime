# 2026-09-07 半透明实机成功记录与镜面后续

## 已验证的本地版本（未正式发布）

承接 modding 仓库 research/transparent-material 分支
research/transparent-material-2026-08-18.md 的 5.17、5.19。
旧记录证明后景深插入点存在，但注入 DrawRenderer 的姿势不正确。
本次组合使用实际场景 SkinnedMeshRenderer、当帧 BakeMesh、显式模型/视图/投影矩阵，
在 VLPostProcessPass.SetupVLBloom 之前补画，绕过已完成的景深。
该组合实机成功；没有单变量实验可以把旧失败唯一归因于蒙皮或 prefab 实例。

运行时代码：src/runtime/BakedTransparency.inl、ModRuntime.cpp；
着色器：packaging/shaders/GmiTransparent.shader 的 GmiBakedAfterDof。
从 m_MotionVectors（_AdditionalInfoTexture）的编码深度执行像素遮挡。
gakumas-mod/gmi-baked-transparency.on 启用；材质 _GmiBakedAfterDof=1，
_AlphaFromTexture=0；运行时关闭 Forward、ActorTransparent、ZPrePass、DepthClaim。

## 证据与产物

- D:\Games\gakumas\FrameAnalysis-2026-09-07-112715：hmsz-icu 薄纱显示成功，
  记录中的 draw 109/110，381/858 索引，运行时 draws=2。
- 长裙 cstm-0177 的 75% 测试验证连续透明，但服装本身缺少被遮住的身体，不能靠透明补齐几何。
- 短裙来源 D:\Games\gakumas\FrameAnalysis-2026-09-07-121712，cstm-0173。
- 已安装 hmsz-layered-dress50，目标 cstm-0177，buildId=f5871cf6b6aa4a8d。
  短裙全部 26271 三角形 + 长裙选定 3019 三角形；submesh 索引数 71763/7050/9057。
  第三槽 alpha=0.50。两套图集组合并对应重映射 UV，DDS 导入 PNG 需要垂直翻转。
  工程 D:\GIT\gakumas-modding\outputs\hmsz-dress75\authoring-layered.blend。
  构建目录同路径 export-layered\hmsz-layered-dress50。
  原 hmsz-dress75 已禁用并保留。包结构校验通过；摆动骨骼左右数量 12/16，动态质量仍需观察。
- D:\Games\gakumas\FrameAnalysis-2026-09-07-151234 与用户截图：主画面短裙外罩长纱裙成功。
  draw149/164 为 1081x1920 离屏目标上的短裙两槽；199/205 为 2162x3840 主画面两槽。
  长纱裙索引区间 first=78813,count=9057 出现在 222 和 242；242 是独立 baked 绘制，
  无 DSV，使用编码深度采样。不能把 222 的提交直接当作有效颜色贡献。
  镜中没有长纱裙：本次补绘没有覆盖反射输出。

## 已知边界与后续次序

1. 首先实现镜面绘制：定位真实反射渲染入口，在反射完成、纹理被消费前，
   使用该视图的矩阵及其颜色/深度目标补绘。不能仅凭同规格资源 hash 判断纹理身份。
2. 首次出现延迟放在下一步：当前场景扫描间隔 60 帧会产生发现延迟；
   单帧抓帧不能测得实际启动等待时间。同帧后景深绘制本身不会形成肉眼可见的跨帧加载延迟。
3. 当前路线尚未验证所有镜头、透明层排序、场景照明与原生角色特效，不等同通用正式交付。

首次 shader bundle 失败来自最小 Unity 项目缺 AssetBundle 模块，不能归因为 67/77 版本号。
原型游戏与 shader bundle 已工作；镜面改动须保留可回退产物，并由用户自行启动游戏验收。

## 后续 m_bdy 克隆方案复核（代码事实，未当作成功版本）

用户反馈 Claude 部署的 vanilla-lit 路线严重破坏效果。读取实际代码确认：
GmiCloneVanillaLit 使用 One/OneMinusSrcColor，且将 _BaseColor 设置为 alpha。
即使假设 shader 输出确实等于 a*C，合成也为 a*C+B*(1-a*C)，并非 a*C+B*(1-a)。
例如 a=0.5,C=(0.2,0.4,1)，背景系数是 (0.9,0.8,0.5)，而正确 alpha 应全为 0.5。
材质白色不代表灯光处理后的颜色恒为 (1,1,1)，此方案不适合可换场景的纱裙。

同时它关闭 DrawGmiBakedTransparency，改在 RenderActor 返回后提交 DrawRenderer，
猜测带 gbuffer 名称的 pass（找不到兜底 0），并假定 _ColorMask/1 对应输出附件。
这同时改变几何提交、绘制时机、pass、MRT 写入与混合，不能当成纯光照实验。
GmiSubmitVanillaLit 在刷新前检查空清单并返回；原主画面扫描又被关闭，
会依赖其他入口先初始化清单。没有逐相机限制的 lastFrame 也不适合多视图。

新的可调查入口已经在本地 iOS 3.2.3 元数据找到：Campus.Rendering.CampusActorParameterPass。
它具有 _MatCapMainLight、_MatCapParam、_ShadeMultiplyColor、_ShadeAdditiveColor、
_GlobalLightParameter、_LitColorMultiplier 等静态属性 ID，且有 UpdateActorCommand。
这些是候选语义入口，尚不能直接宣称等于 PC cb0[148..161]。
可通过 PC 元数据/同帧参数值核对 ID 与 cb 偏移，或提取 SerializedShader 做精确映射；
无需因为材质属性表缺少全局量就放弃既有 baked 几何路线。
下一版应保留 BakeMesh、显式矩阵、真 alpha 和深度处理，只替换共享光照计算，
主相机和镜面调用同一光照函数，各自保留独立目标与深度。

## 镜面：入口已定，补绘已实现（待实机验收）

上一节第 1 条「定位真实反射渲染入口」有答案了，两处证据：

抓帧 FrameAnalysis-2026-09-07-151234。1081x1920 的 378bc99e 是一张图集，上下两格各一个
反射视角（下格上下翻转），draws 47..172 全部画在其中，含 mod 的不透明 submesh（149/164，
索引 71763），不含长纱裙。镜子是重画几何，不是屏幕空间贴图。对比 draw 149 与主画面 199：
per-draw 常量缓冲完全相同，只有 VP 那一段不同且首行整行变号。注意 3Dmigoto 的 cb 文件名
哈希按资源标识，不按内容，只能比 buf 内容。

il2cpp 反编译（iOS 3.3.0 UnityFramework）。
DeferredLights.ExecutePlanarReflection @0x0A35D8C8 取一条 CommandBuffer，按
_planarReflectionCount（上限 MaxPlanarReflection=3）循环调用
VL.Rendering.PlanarReflectionUtility.RenderPlanarReflection @0x0A2C3BA8，循环后才
context.ExecuteCommandBuffer。RenderPlanarReflection 内部顺序：cmd 画 stencil/mask 盒、
设 _PlanarReflection_* 全局、SetInvertCulling、SetViewProjectionMatrices(反射 V、
CalculateObliqueMatrix 得到的 P)；context.Cull；context.ExecuteCommandBuffer(cmd)；
cmd.Clear()；context.DrawRenderers 两到三趟（基础 tag、颜色 tag、每面镜子自带的额外
tag，过滤条件取自 PlanarReflectionCachedChunk）；随后把「恢复剔除、恢复主相机 VP、画
resetPass」排进 cmd —— 这几条要等下一次执行才生效。

因此补绘接在该函数返回处：此时反射 VP、反转剔除、图集这张带深度附件的 RT 都还在，
自带一条 CommandBuffer 立即 context.ExecuteCommandBuffer 即落在镜像里，运行时不需要
自己算任何矩阵，也不需要主画面那套编码深度采样。实现见 src/runtime/BakedReflection.inl
与 GmiTransparent.shader 的 GmiBakedReflection pass（Blend One OneMinusSrcAlpha、
ZTest LEqual、常规 UnityObjectToClipPos）。开关 gakumas-mod/gmi-baked-reflection.on，
删掉即回到只有主画面的行为。

未验证项，实机看到什么就记什么：本次补绘不写 stencil，而原版反射几何是带 RenderStateBlock
（含 stencil）画的，镜子边缘或另一格图集可能出现溢出；三趟 DrawRenderers 用的 ShaderTagId
与队列区间没有读取，也没有依赖；首次显示延迟仍未处理。

## 18:18 场景光照试验：恢复真 alpha，接入原版场景参数（待实机验收）

本次依据用户“按你说的来做”实施。未启动游戏。

- 关闭部署的 gmi-vanilla-lit.on，使主画面恢复 GmiBakedAfterDof；保留原来的 BakeMesh、
  显式矩阵、编码深度遮挡、Blend One/OneMinusSrcAlpha。未改变 mod 网格、贴图与 alpha=0.5。
- 主画面和镜面共用 GmiActorCloth。依据原版 f872756c910a6eb7 的寄存器运算，
  实现 Def.r 调节 Ramp 横坐标、Ramp.a/阴影权重、Shade.rgb×场景阴影色、Shade.a 分支。
  不再将额外光除以亮度归一化；改用场景额外光缩放，并乘在着色结果上。
- 使用候选全局名 _MatCapMainLight、_MatCapParam、_ShadeMultiplyColor、
  _MatCapLightColor、_ShadeAdditiveColor、_GlobalLightParameter。
  这些名字确实存在于 PC global-metadata.dat 与 CampusActorParameterPass 元数据，
  **名字存在不等于已证明 cb0 下标映射**。下一次抓帧须比较这些输入与原生 body 同帧的
  cb0[147..154]，尤其 148 的光方向、149 的阈值/阴影权重、150 阴影色、154.y 额外光缩放。
- 每次提交从当前 renderer 的原生 slot0 材质读取 _RampMap，通过 MPB 绑定；
  不克隆原生 shader，也不保留一份永久冻结的 Ramp。无 Ramp 时使用近似渐变并清除 ready 标志。
- 主画面显式提供光照视图矩阵，镜面使用其当前矩阵。当前 viewNormal 仍为视图空间近似；
  原版使用与观察方向有关的基，透视及镜面下尚不能宣称逐像素一致。

离线验证：Release x64 DLL 构建成功；Unity 6000.0.67f1 D3D11 shader 构建与
AssetBundle 重载成功（GMI_BAKED_LOAD_OK assets=1 passes=7）。原版 Ramp/Shade
寄存器表达式与实现的 lerp 表达式做 1500 个随机通道数值比较通过。
该数值检查不验证全局绑定、GPU 提交时机或最终画面。不能据此标记场景光照修复成功。

未实现：逐物体灯光筛选、原版 SH 环境光、真实阴影贴图、反射立方图、逐光高光、
材质专用饱和度/倍率分支；不能以此版声称完全复刻原生光照。首次出现延迟仍留下一步。
验收重点是：主画面恢复 50% 透纱且贴图正确；暖光/蓝光下阴影色是否跟随；
镜中仍能看见长纱裙。若偏色或偏亮，先核对上述绑定值，再补缺失算法，不先调曝光遮掩。

部署前的 DLL、shader bundle 和 vanilla-lit 开关已备份到
build/actor-lighting-backup-20260907-181816。
若需回到上一份自定义 shader 光照，恢复其中 DLL/bundle 并保持 vanilla-lit 开关关闭；
把备份开关也恢复会重新启用用户报告失败的克隆路线。

## 185607 抓帧：发白的可复现原因与角色灯光表修正

用户反馈上一版仍发白、看起来不透明。抓帧证据：

- 主画面长裙补绘 draw755，PS c4791285c2ea795a。cb0[4]=(0.5,0,0.004,0)，
  shader 最终 o0.w=0.5，RGB 也乘 0.5。不能把“不透明”的观感直接解释成 alpha 被改成 1。
- draw755 cb0[44..49] 对应候选场景参数，逐值与原版 draw729/730 的 cb0[147..154]
  对照一致。由此确认上一版这些参数名的映射；问题不在参数名称。
- 原版 body 从独立 cb2 读取角色灯光，不是 URP _AdditionalLights* 全局数组！
  原版 cb2[0] 的 uint4=(7,0,0,117440512)，本次 actorIndex=0，8-bit mask=7，
  只启用角色表的灯 0、1、2。角色表灯0位于 (-0.033,8.172,6.095)，
  通用表灯0却位于 (0,5,0)，连灯光来源都不同，不只是漏了筛选。
- 用 draw755 的 9057 个唯一顶点、世界矩阵、法线重算漫反射额外光：
  旧表 RGB 中位数=(0.990924,1.446523,1.421949)，角色表加掩码后
  =(0.301986,0.293298,0.286100)。这里只比较额外光乘数，不等于最终像素颜色。
- 解码 draw754/755 的 R11G11B10_FLOAT 同一目标，在变化大于0.05的像素中，
  draw755 RGB 中位数=(0.625,0.898438,0.96875)，90分位=(0.734375,1,1.0625)。
  尚未进入后续 Bloom 已接近/超过白点，可解释强烈发光和背景细节不明显。
  重叠纱片的多次 alpha 累积仍可能影响局部透明观感，不据此断言所有像素均只混合一次。

修正：按 Campus.Rendering.Shaders.ShaderVariablesActorLighting 元数据的精确布局声明
同名 CBUFFER：uint4 _LightData @0、float4[8] _LightPositions @16、_LightColors @144、
_LightAttenuations @272、_LightDirections @400，总计528字节。PC 元数据同样存在该缓冲名。
不再读取通用 URP 灯表。运行时从原生 slot0 材质及 renderer MPB 读取 _ActorIndex，
传到自定义 pass 后解码对应 byte，只计算 mask 允许的灯。主画面和镜面共用此实现。
未修改 alpha、曝光、几何、贴图或绘制时机。

验证：Release DLL 成功；D3D11 shader 构建和 bundle 重载成功，passes=7。
用 UnityPy 读取编译后的 Shader，主画面与镜面的角色缓冲均为528字节，字段偏移匹配。
仍须用户下一次抓帧确认运行时确实绑定了角色缓冲，不能把离线布局验证当成实机成功。
本轮部署前备份位于 build/actor-light-buffer-185607/backup。未启动游戏。

## 收口（2026-09-07 晚）

- 不再有 `gmi-baked-transparency.on` / `gmi-baked-reflection.on`：材质声明 `_GmiBakedAfterDof` 即启用，
  镜面跟随主画面；相关钩子无条件安装。上文提到开关文件的地方以本节为准。
- 光照按 `ShaderVariablesActorLighting`（`_ActorIndex` 掩码选灯）+ 场景全局阴影色 + `_RampMap`，
  用户实机确认暖光 / 蓝光舞台与带镜子的房间都正常。
- 「克隆原版材质改混合状态」对照实验删除（原版 PS 的 o1.w 硬写 1，混合出不了固定 alpha）。
- 刚出现的渲染器留 30 帧稳定期再烘（换装页当场新建角色、同帧 BakeMesh 曾原生崩溃）；扫描间隔 20 帧。
- 契约见 `manifest-v2.md` 的 `transparentMaterials` 一节；作者侧只需在插件里选「自建半透明」。
