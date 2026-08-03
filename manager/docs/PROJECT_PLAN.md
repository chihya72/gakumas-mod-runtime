# Gakumas 游戏内 Mod 管理器：完整规划

> 文档状态：M1 独立 DLL、Runtime 握手和主页入口已验证；真实 `SettingTopScreen` 全屏页、固定 Mod Cell、服装/发型分页、Master 名称、服装官方缩略图、原生开关写回及标准 `SkinnedMeshRenderer` 热开关主链均已实机运行。最新 Reload 入口缓存失效、材质数组写回后的 Mod 材质恢复和两栏橙条修复已完成本地 Release 构建，待实机复验
> 当前状态与已废弃做法见 `UI_FLOW.md`；调查数据源见 `RESEARCH_SOURCES.md`
> 建立日期：2026-08-01  
> 目标平台：学园偶像大师 DMM Windows 版（Unity IL2CPP / x64）  
> 项目仓库：本仓库的 `manager/` 子目录（原独立仓库 `gakumas-in-game-mod-manager` 已并入）
> 入口方式：**与 Runtime 同处 `xinput1_3.dll`**。规划期设计的独立 `xinput9_1_0.dll` 入口已在
>   DLL 合并后取消，下文凡按三个入口名展开的架构、加载顺序和握手章节均属规划期记录；
> 现有入口：`version.dll` 为独立汉化插件，`xinput1_3.dll` 为 Mod Runtime + 本管理器 UI

## 1. 项目摘要

本项目为 `gakumas-mod-runtime` 的游戏内管理界面。它不负责制作 Mod，也不直接实现 Mesh、材质或贴图替换；它负责把运行库已经扫描到的 Mod 转换成玩家能理解的列表，并允许玩家同时修改当前会话与下一次启动的启用状态。

第一版的核心体验是：

1. 玩家从游戏主页菜单进入“Mod 管理”；
2. 在“服装”和“发型”两个分页中查看 Mod；
3. 每个 Mod 使用游戏原本的目标服装或发型图标，告诉玩家应在游戏里选择什么；
4. 玩家使用开关修改 Mod 状态；
5. 标准服装/发型 Mod 在当前角色上即时开关，同时持久化下次启动状态。

管理器必须使用面向玩家的文字。资源 ID、`source`、`part`、文件路径、优先级和 Renderer 名称等技术字段不出现在普通界面中。

## 2. 已确认的产品决策

以下内容视为第一版的固定要求，不再作为开放问题：

- 一个 Mod 只能对应一个逻辑目标：一件服装或一个发型；
- 管理器第一版要求一个 Manifest 只提供一条逻辑 `replacement`；同一条记录可以包含多个
  renderer、材质槽或贴图规则，但不能借此表达多件服装或多个发型；
- `body` Mod 放入“服装”分页，`hair` Mod 放入“发型”分页；
- 管理入口只在游戏 Master 数据已经加载的主页阶段出现，因此不设计 Master 未加载占位流程；
- 目标解析失败时，使用 Manifest 原始 `source` 作为文字兜底，单个异常项不能阻断整个页面；
- 开关同时修改当前会话和下一次启动配置；标准 `SkinnedMeshRenderer` 规则必须支持当前角色热卸载与热恢复；
- 整对象替换或附加式规则无法安全逆转时，允许提示重新选择目标或重新进入场景；
- 正常玩家界面不显示服装 ID、发型资源 ID、替换部位或资源路径；
- 第一版只管理 `gakumas-mod-runtime` 的 AssetBundle Mod，不管理旧 3DMigoto Mod。

## 3. 目标与非目标

### 3.1 第一版目标

- 列出 Mod 根目录中的有效、禁用和可识别异常 Manifest；
- 显示 Mod 名称；
- 识别 Mod 属于服装还是发型；
- 显示对应的游戏角色、服装/发型名称与官方图标；
- 使用两个分页区分服装和发型；
- 启用或禁用 Mod，并原子写回 `mod.json` 的 `enabled`；
- 区分“本次启动实际状态”和“下次启动配置状态”；
- 在冲突、目标解析失败、Manifest 损坏或写入失败时给出玩家可理解的提示；
- 游戏升级导致 UI 签名失效时安全降级，不影响 Mod Runtime 的基本替换功能；
- 提供足够日志用于开发者排查，但不把技术信息塞进普通界面。

### 3.2 第一版不做

- 对整对象替换、附加式规则承诺无条件热卸载；
- 下载、更新、安装、删除或移动 Mod；
- Mod 市场、联网账号、评分或自动更新；
- Mod 作者工具和 Manifest 编辑器；
- 3DMigoto Mod 管理；
- `face` Mod 管理；
- 游戏内重启按钮；
- 向普通玩家展示完整日志、资源路径或 IL2CPP 调试信息；
- 自己维护一套游戏服装缩略图资源库；
- 提交或分发任何游戏提取资源。

## 4. 玩家体验设计

### 4.1 入口

首选入口是在主页菜单中增加“Mod 管理”按钮。按钮只在以下条件同时满足时出现：

- `gakumas-mod-runtime` 初始化成功；
- Mod 管理器插件初始化成功；
- 已进入 Master 数据可用的主页阶段；
- 当前游戏版本的 UI 接入签名验证通过。

如果管理器初始化失败，不注入入口，不影响原菜单和 Mod Runtime。

### 4.2 页面层级

第一版使用游戏原生风格的 Sheet 或 Screen：

```text
Mod 管理
┌──────────────┬──────────────┐
│ 服装  (数量) │ 发型  (数量) │
└──────────────┴──────────────┘

[目标官方图标]  Mod 名称                         [开关]
                在换装中选择：角色名 · 游戏内名称
                冲突或错误说明（仅有异常时）
```

仅在 Runtime 返回不支持或恢复失败时显示页面级降级说明：

```text
当前对象未能即时刷新，请重选服装/发型或重新进入场景
```

普通条目不重复显示该说明。

### 4.3 卡片显示规则

普通状态只显示：

- 游戏的目标服装或发型图标；
- Mod 名称；
- “在换装中选择”或“在发型中选择”；
- 游戏内角色名和目标名称；
- 启用开关。

按需显示的状态文字：

- `加载失败`；
- `Mod 冲突：与其他 Mod 使用同一目标，已自动关闭`；
- `无法开启：已有其他 Mod 开启同一目标，请先检查并关闭它`；
- `无法识别对应服装`；
- `配置有误`。

普通界面禁止显示：

- `ttmr-cstm-0111` 等内部 ID；
- `mdl_chr_...` 等原始资源名，除非目标解析失败且需要兜底；
- `body`、`hair` 等内部枚举值；
- Bundle、Prefab、Renderer、材质槽和文件系统路径；
- 冲突优先级数字。

### 4.4 分类、排序和空状态

分类规则：

- 逻辑类别为 `costume` 的项目进入“服装”；
- 逻辑类别为 `hair` 的项目进入“发型”；
- 不支持或无法确认类别的 Manifest 不伪装成正常项目；它只通过页面级异常提示和日志报告。

默认排序不得因开关状态变化而移动：

1. 游戏角色顺序；
2. 游戏 Master 中的目标顺序；
3. Mod 名称；
4. 稳定 `modId` 作为最终决胜键。

当前实现已经解析角色与目标 Master 名称，但尚未接入 Master 的显示顺序键，因此暂用
`Mod 名称 + modId` 的稳定顺序；启用状态不参与排序，开关后条目不会换位。

空状态：

```text
暂未发现服装 Mod
```

或：

```text
暂未发现发型 Mod
```

第一版不提供搜索和复杂筛选；当实机列表规模证明有必要时再加入。

### 4.5 多 Mod 冲突

同一逻辑目标是互斥组。启动时若多个 Manifest 同时开启，Runtime 将该冲突组全部关闭并持久化。被自动关闭的条目显示：

```text
Mod 冲突：与“另一个 Mod 名称”使用同一服装，已自动关闭
```

运行中已有一个 Mod 开启时，再开启同目标 Mod 会被拒绝；原 Mod 保持开启，新 Mod 保持关闭并显示：

```text
无法开启：“已开启的 Mod 名称”已开启同一服装，请先检查并关闭它
```

玩家必须先关闭原 Mod，之后才能开启新 Mod。普通界面不展示优先级数值。

## 5. 当前基础与缺口

### 5.1 已有能力

`gakumas-mod-runtime` 当前已经具备：

- 通过 `xinput1_3.dll` 代理进入游戏进程；
- 扫描 `gakumas-local\local-files\mods\<mod-id>\mod.json`；
- 读取 Manifest v2 的 `name`、`id`、`priority`、`enabled` 和 `replacements[]`；
- 按 `source` 注册替换规则；
- 懒加载 AssetBundle；
- 处理 `body` 与 `hair` 的 Mesh、骨架和贴图替换；
- 记录运行日志与资源加载历史；
- 使用 UnityResolve 访问 IL2CPP 类与方法。

历史研究中已经验证了以下游戏调用路径可行。这里的代码只作为签名调查参考，管理器不会加载、调用、修改或依赖汉化插件：

- `MasterManager.get_CostumeMaster`；
- `CostumeMaster.GetAllWithSortByKey`；
- `Costume.get_Id`；
- `Costume.get_CostumeHeadId`；
- `Costume.get_DefaultCostumeHeadId`；
- `Costume.get_CostumeColorGroupId`；
- `PhotographyCostumeSettingListItemModel` 相关路径。

### 5.2 当前已经补齐的 Runtime 能力

相邻 `gakumas-mod-runtime` 仓库已经为管理器补齐：

- 独立于 replacement map 的完整 Mod 目录；
- 禁用 Mod、Manifest 异常、冲突和本次启动注册状态；
- 不可变 UTF-8 JSON 快照；
- 原子修改顶层 `enabled` 的 API；
- 带结构尺寸和版本号的 Runtime API v1 C ABI；
- `GmrGetRuntimeApiV1` 导出和跨 DLL 缓冲区释放函数；
- Runtime 与管理器握手日志。

2026-08-01 的目标游戏日志与截图已确认管理器读取快照、组合全屏分页、显示固定 Mod 行、
解析服装/发型 Master 名称、加载服装官方缩略图，并写回当前会话和 Manifest。当前剩余缺口是：

- `appliedThisSession` 与真实 AssetBundle 应用成功记录的完整对应；
- Manager/Runtime 共享头文件的单一来源整理；
- 最新 Reload 后同地址 `MenuView` 的入口缓存失效补丁实机复验；
- 热开启后拦截 `Renderer.set_sharedMaterials` / `set_materials` 并恢复完整 Mod 材质数组的实机复验；
- 发型 `CostumeHead` 官方预览图在最终行中可见的截图验收；
- 返回/重登/分辨率和长期重复生命周期验证；
- 冲突、配置异常、写入失败和大列表的实机覆盖；
- Manager/Runtime API 头文件的单一共享来源。

Runtime 已为标准原地替换记录 Renderer 级可逆快照：原 Mesh、共享材质、骨骼名称、根骨名称，以及 Renderer 相对资源根节点的层级深度。关闭时同时扫描当前场景和已缓存 Prefab，按 Mod Mesh/私有材质引用匹配并恢复；开启时只对对应服装/发型子树重应用，避免误改整名角色的其他 Renderer。整对象替换和附加式规则暂不进入该路径。

热开启在当前角色已完成动画初始化后发生，因此 Runtime 还会缓存活动 `CampusActorAnimationRig` 上下文；若本次重应用创建了新的动态骨或链，则把它们补入当前 `initializeData` 并重新调用原始 `RegisterBones`。缓存 Prefab 不匹配活动 Rig，不执行该步骤。

材质热恢复不能对所有槽直接调用 `SetPropertyBlock(null, slot)`，因为游戏自己的肤色、遮罩和光照参数也保存在每材质 `MaterialPropertyBlock` 中。Runtime 在合并 Mod 贴图前深拷贝游戏已有 Block；OFF 时恢复这些快照，只清除 Runtime 为原本空槽创建的 Block。

活跃 `SkinnedMeshRenderer` 在运行中更换 Mesh/骨骼后还持有内部渲染缓存；Runtime 会重置
Bounds、保持原 enabled 状态刷新 Renderer，并只对活动 Rig 的目标节点执行一次停用/启用。
热重应用后游戏会把材质数组换回原始值，所以用户必须切换页面才恢复正确颜色。IDA MCP 后的
底层写回钩子实验导致加载卡住或点击崩溃，已经撤回。当前部署恢复为调查前的同步热恢复/热重应用
路径；管理器全屏页面、橙条、分隔星、入口与导航修复全部保留。

## 6. 总体架构

```text
gakumas.exe
├─ version.dll（汉化插件，独立运行，不参与本项目）
├─ xinput1_3.dll（现有 Mod Runtime 代理）
│  ├─ 扫描全部 mod.json
│  ├─ 保存本次启动状态与下次启动配置
│  ├─ 注册并执行 AssetBundle 替换
│  └─ 导出 Runtime API v1
└─ xinput9_1_0.dll（本项目，d3d11.dll 的独立 XInput 依赖入口）
   ├─ 通过 Runtime API 获取 Mod 快照
   ├─ 通过 UnityResolve / RuntimeInvoke 访问游戏 UI 与 Master
   ├─ 解析服装和发型目标
   ├─ 复用游戏原生格子加载图标
   └─ 通过 Runtime API 修改 enabled
```

### 6.1 二进制边界

推荐产物：

```text
游戏根目录\xinput9_1_0.dll
```

`xinput9_1_0.dll` 是普通的 x64 XInput 代理 DLL。游戏根目录的 `d3d11.dll` 静态导入 `xinput9_1_0.dll` 的 XInput 1.0 导出，因此 Windows 加载图形模块时会自动加载它。管理器转发系统 XInput 9.1.0 的 4 个导出后启动工作线程。不占用汉化的 `version.dll`，不占用 Mod Runtime 的 `xinput1_3.dll`，也不由 Runtime 链式加载。

管理器 DLL 导出固定入口：

```cpp
extern "C" __declspec(dllexport)
bool GkmmInitialize();

extern "C" __declspec(dllexport)
void GkmmShutdown();
```

加载后，管理器通过 `GetModuleHandleW(L"xinput1_3.dll")` 在当前进程中查找现有 Runtime 的导出函数。找不到 Runtime API 时，管理器记录原因并不显示入口，不自行伪造 Mod 状态。

管理器入口固定为 `xinput9_1_0.dll`，不占用 `version.dll` 或 `xinput1_3.dll`，也不要求安装汉化插件。

`RuntimeClient.cpp` 已删除历史口误产生的 `xinput3.dll` 兜底候选，只接受已经验证的
`xinput1_3.dll`。该清理已通过本地 Release 构建，待下一轮实机握手复验。

入口加载、Runtime 握手和停止状态写入 `gakumas-local\mod-manager.log`；日志同时使用 `OutputDebugStringA` 输出，便于用 DebugView 观察。

### 6.2 独立入口加载流程

首选流程：

1. Windows 加载游戏根目录的 `d3d11.dll` 时，根据其导入表加载 `xinput9_1_0.dll`；
2. `xinput9_1_0.dll` 的 `DllMain` 只创建工作线程，不执行 Unity 或文件扫描；
3. 工作线程等待 Runtime API 和 `GameAssembly.dll` 就绪；
4. 管理器完成签名检查后，等待主页 UI 创建并注入入口；
5. 退出、进程结束或加载失败时，管理器清理自己的 Hook 和 UI 订阅。

本项目不需要额外 Loader，也不需要外部 DLL 注入器；入口由 `d3d11.dll` 的导入关系触发。

现有 DLL 边界：

- 不覆盖、不改名、不向 `version.dll` 写入管理器代码；汉化插件继续按原方式独立加载；
- 不覆盖、不改名、不向 `xinput1_3.dll` 写入管理器代码；它继续作为唯一受支持的 Mod Runtime 模块并导出 Runtime API；
- 不为 Runtime 引入部署别名或模糊匹配；管理器只接受已经验证的 `xinput1_3.dll`；
- 新管理器只使用自己的文件名 `xinput9_1_0.dll`，由 `d3d11.dll` 的导入关系自动进入进程。

### 6.3 依赖原则

- `gakumas-mod-runtime` 是 Mod 状态的唯一事实来源，但不负责管理器 UI 注入；
- 管理器不能直接修改 Runtime 内部容器；
- 管理器只通过版本化 C ABI 读写状态；
- `gkms-localify-dmm` 与本项目没有编译、加载、调用和 Hook 依赖；
- 管理器拥有自己的 UI Hook 生命周期；它可以静态链接自己的 MinHook，但不得 Hook 汉化插件目标或 Runtime 资源 Hook；
- 跨 DLL 不传 `std::string`、`std::vector`、异常或 Unity 对象；
- Unity 对象只存在于管理器 UI 层，不能写入持久化目录或跨线程长期裸持有。

## 7. 模块划分

### 7.1 Runtime 侧目录服务

职责：

- 扫描所有一级 Mod 目录；
- 无论 `enabled` 为真或假，都生成目录记录；
- 将 Manifest 解析和 replacement 注册分开；
- 校验一个 Mod 只有一个逻辑目标；
- 计算冲突和实际注册状态；
- 提供快照；
- 原子更新 `enabled`；
- 记录本次启动状态，不在切换时修改已经注册的 replacement map。

该部分改动属于 `gakumas-mod-runtime` 仓库，不放入本仓库。

### 7.2 Manager Runtime Client

职责：

- 校验 Runtime API 版本；
- 获取并解析快照 JSON；
- 调用启停 API；
- 把 Runtime 错误码转换为玩家提示；
- 保证 Runtime 不可用时管理器安全退出。

### 7.3 Target Resolver

职责：

- 将 replacement 的 `source` 转换为逻辑服装或发型目标；
- 查询游戏 Master；
- 得到角色与游戏内显示名称；
- 为 UI 构造原生格子所需的数据；
- 失败时返回可显示的降级结果，而不是抛出到页面层。

### 7.4 Game UI Bridge

职责：

- 解析并验证 IL2CPP 类与方法；
- 注入主页菜单入口；
- 打开/关闭管理页面；
- 创建分页和可复用列表；
- 复用游戏原生服装/发型 Cell；
- 绑定点击与开关事件；
- 在游戏主线程上操作 Unity 对象。

### 7.5 Presentation Model

职责：

- 合并 Runtime 状态与 Master 解析结果；
- 生成玩家可见标题、副标题、图标和状态文案；
- 排序和分页；
- 管理热切换、自动互斥和错误状态；
- 不向 View 暴露文件路径或原始 Manifest 结构。

## 8. Runtime API v1 规划（已删除）

> **§8 Runtime API v1 规划已删除。**当前 API 的权威定义是 [`../../docs/runtime-api-v1.md`](../../docs/runtime-api-v1.md)，规划稿与它不一致，留着只会误导。

## 9. Manifest 与逻辑目标规则（已删除）

> **§9 Manifest 与逻辑目标规则已删除。**当前规范见 [`../../docs/manifest-v2.md`](../../docs/manifest-v2.md)。

## 10. Master 与官方图标

### 10.1 Master 访问

管理入口在主页阶段创建，按产品约束可认为 Master 已经加载。页面打开时同步建立只读索引：

```text
Costume.Id -> Costume 对象
CostumeHead.Id -> CostumeHead 对象
CostumeHead.HairAssetId -> CostumeHead 对象
Character.Id -> Character 显示信息
```

索引的生命周期限定在当前登录会话。登出、回到标题或用户数据重建时清空并重新建立。

### 10.2 RuntimeInvoke 使用规范

`RuntimeInvoke` 不是无签名的动态调用。每个调用点必须登记并验证：

- Assembly；
- Namespace；
- Class；
- Method；
- 参数数量；
- 静态或实例方法；
- C++ 返回类型；
- 每个参数的 managed 类型；
- 是否为泛型方法；
- 是否需要隐藏 `MethodInfo*`；
- 必须运行在什么线程；
- 已验证的游戏版本。

调用策略：

- 普通、签名明确的方法优先使用 UnityResolve `Method::Invoke`；
- 需要走 managed invocation、装箱或异常边界时使用 `RuntimeInvoke`；
- 共享泛型方法必须取得实际运行时类型的 `MethodInfo`，按已验证签名调用；
- 任一签名缺失时跳过相应 UI 功能并记录日志，禁止用猜测的函数指针继续执行。

仓库后续维护 `docs\SIGNATURE_MATRIX.md`，把所有游戏方法签名集中记录，避免散落在 Hook 代码中。

### 10.3 图标策略

首选方案是复用游戏摄影/换装页面的原生 Cell、Model 和 Presenter：

- 服装分页绑定对应 Costume；
- 发型分页绑定对应 CostumeHead 或游戏实际使用的头部选择模型；
- 由游戏自己的异步资源流程加载 Sprite；
- 管理器不猜测缩略图 AssetBundle 路径；
- 管理器不缓存或分发提取后的游戏图标。

如果无法直接复用完整 Cell，则第二选择是复用它内部已验证的图标加载方法。只有前两种方案都不可行时才研究资源命名规则。

## 11. UI 接入规划

### 11.1 原生 UI 优先

优先复用游戏现有：

- 页面/Sheet 层级管理器；
- 分页控件；
- Scroll/List Presenter；
- 可复用 Cell；
- Toggle；
- Toast/Dialog；
- 关闭和返回行为；
- 字体、颜色、边距与动画。

不在第一版引入 ImGui 覆盖层。ImGui 会产生输入、缩放、视觉一致性和移动端式界面适配问题，也无法自然复用官方服装格子。

M1 早期验证的放大 `MenuSubButtonView` 只用于证明入口、点击、Runtime 快照和
RectTransform 调用链，已经退出当前主路径。当前页面使用游戏真实设置窗口，不再覆盖主页。

当前源码已经停止创建主页 Canvas 面板。Mod 入口写入当前菜单 Presenter 的
`SelectedButtonType=Setting` 并调用 `OutGameMenuPresenter.OnSelected()`，让游戏自行创建
`SettingWindow` / `SettingTopScreen` 并维护返回栈；仅对 pending 的该次设置
页面，在主线程 `EventSystem.Update()` 中发现第一页为 `PreferenceTabPage` 的活跃 Tab 后，
重构标题、分页、滚动内容和开关行。17:54 的首次部署确认旧解析把
`CampusSimpleTab`、`SwitchButton` 和 `OverlayTitleView` 错误限定在单一程序集，因启动检查失败
连带禁用了入口。当前实现已改为跨程序集解析，并把入口 Hook 与全屏能力检查解耦；修正版已
于 18:20–18:21 取得全屏页、分页和开关写回截图。后续部署又实机确认固定行、Master 名称、
服装缩略图、稳定排序和开关帧末校正。21:13–21:14 日志确认 Mod→Mod、设置→Mod、Mod→设置
三种幂等导航分支；同轮复现 Reload 后复用 `MenuView` 地址造成入口丢失。当前部署版已在
Reload 时失效该 View 的注入缓存，待实机复验。

### 11.2 接入调查顺序

1. **已完成：**确认 `OutGameMenuPresenter`、`MenuPresenter.SetEvent` 和 `MenuView`；
2. **已完成：**克隆副按钮并通过 `CampusButtonBase.OnClicked` 识别自定义入口；
3. **已完成：**创建可见文本面板并显示 Runtime 快照；
4. **已实机验证：**通过真实设置导航创建全屏页面，复用标题、返回栈、Tab 和 Scroll；
5. **已实机验证：**绑定服装/发型分页、临时设置行和 Runtime 开关写回；
6. **已实机验证：**稳定排序、Master 目标文案、固定图标位、服装官方缩略图和三种幂等导航分支；
7. **已部署待复验：**Reload 后菜单入口缓存失效，以及热开启后的即时颜色刷新；
8. 验证返回主页、重登、重复进入和随后原设置页不会被污染；
9. 验证发型官方预览图最终可见；需要时再研究完整专用 Cell。

### 11.3 UI 生命周期

- 主菜单创建时注入入口，并用实例标记防止重复注入；设置页 Reload 会主动失效当前 View 的标记，以允许同地址新生命周期重新注入；
- 当前源码在每次全屏页面组合和开关写回成功后拉取 Runtime 快照；该语义已由 18:20–18:21 实机日志和截图确认；
- 在游戏主线程解析 Master、创建和绑定 Unity 对象；
- 关闭页面时释放管理器持有的事件订阅、GCHandle 和临时列表；
- 返回标题或登出时清空 Master 索引；
- Runtime Shutdown 时先关闭管理页面，再移除 Hook 和释放 DLL。

## 12. 开关与持久化

### 12.1 状态模型

页面开关绑定 `configuredEnabled`，成功写入时也同步更新当前 Runtime 会话。

| 状态 | 开关 | 玩家显示 |
|---|---|---|
| 当前会话启用 | 开 | 普通目标说明 |
| 当前会话关闭 | 关 | 普通目标说明 |
| 启动时同目标多项冲突 | 关 | `Mod 冲突：与“Mod 名称”使用同一服装/发型，已自动关闭` |
| 运行中开启被占用目标 | 关 | `无法开启：“Mod 名称”已开启同一服装/发型，请先检查并关闭它` |
| 配置无效 | 禁用 | `配置有误` |

所有有效替换规则在启动时注册为候选，AssetBundle 仍按实际资源请求懒加载。开关会重建有效 replacement map，后续加载立即使用新状态。标准原地替换在首次应用前保存可逆 Renderer 快照；OFF 恢复当前实例和缓存 Prefab 的 Mesh、材质、骨骼与根骨，已注册的动态骨基础设施保留为不可见休眠状态，避免破坏游戏初始化列表；ON 对已恢复的资源子树和当前实例重新应用并复用这些动态骨。详细的 Registered/Applied/Hot-restored 状态留在日志和内部快照。整对象替换和附加式规则仍以重新加载资源作为降级路径。

### 12.2 写入流程

`SetModEnabled(modId, enabled)` 必须：

1. 通过 Runtime 目录按 `modId` 找到已扫描 Manifest，调用方不能传任意路径；
2. 重新读取目标文件并确认仍是同一个 Mod；
3. 解析 JSON；
4. 只修改顶层 `enabled`；
5. 在同一目录写入临时文件；
6. Flush 后使用 Windows 原子替换；
7. 开启时查找同一 `targetKind + targetKey` 的已开启项；若存在则保持双方状态不变并返回 `GMR_E_TARGET_CONFLICT`；
8. 无冲突时更新当前会话候选状态并重建有效 replacement map；
9. 原子写回目标 Manifest，成功后更新 `configuredEnabled` 与 `registeredThisSession`；
10. 启动扫描发现同组多项开启时，以组为单位全部关闭；任一步失败时尽力回滚整组会话状态与 Manifest；
11. 其他失败同样保留原状态并返回明确错误码。

为检测外部编辑，目录记录保存 Manifest 的最后写入时间与内容摘要。写入前不一致时重新解析；如果目标身份改变则返回 `GMR_E_CONCURRENT_CHANGE`。

### 12.3 点击反馈

- 写入成功：立即更新开关和当前会话资源；仅在热恢复不支持或失败时显示重新加载提示；
- 写入失败：恢复开关，并显示“保存失败，请查看日志”；
- 快速连续点击：写入进行中时临时禁用该开关，防止并发写同一 Manifest；
- 页面关闭后重新打开：以 Runtime 最新快照为准。

## 13. 线程与并发

- Unity、IL2CPP UI、Master 对象与 Sprite 只在游戏主线程访问；
- Runtime 目录、replacement map 和 load history 必须分别有明确的锁策略；
- API 返回深拷贝 JSON 快照，不向管理器暴露容器引用；
- Manifest 文件很小，第一版允许点击时同步原子写入，但不得在主线程递归扫描目录或读取 Bundle；
- Runtime 初始化扫描在现有工作线程完成；
- 管理器入口仅在目录快照就绪后开放；
- Shutdown 先阻止新调用，再等待活动 API 调用结束，然后释放管理器。

## 14. 兼容性与安全降级

### 14.1 游戏版本变化

管理器启动时验证关键签名：

- 主页菜单接入点；
- 页面层管理器；
- Costume/CostumeHead Master getter；
- 官方服装 Cell；
- 官方发型 Cell；
- Toggle 和事件绑定方法。

按能力分级：

- 入口签名失败：完全不显示管理器；
- 服装 Cell 失败：服装项使用占位图，但仍可管理开关；
- 发型 Cell 失败：发型项使用占位图，但仍可管理开关；
- Master 目标失败：显示原始 `source` 兜底；
- Runtime API 版本不兼容：不注入入口；
- 单个 Mod 异常：只影响该项目。

### 14.2 文件系统安全

- 只操作 Runtime 已扫描的一级 Mod 目录；
- 对实际路径做规范化并确认仍位于 Mod 根目录内；
- 不接受 UI 传入的绝对路径；
- 不跟随逃逸到 Mod 根目录外的重解析点；
- 不覆盖 Bundle 或其他 Mod 文件；
- 不删除文件；
- 日志避免输出用户令牌等无关敏感信息。

### 14.3 崩溃隔离

- DLL 入口不执行复杂逻辑；
- 初始化在 Runtime 工作线程完成；
- 所有导出函数检查结构尺寸、空指针和版本；
- 管理器独立登记自己的 UI Hook，Runtime 不负责管理这些 Hook；
- 原游戏对象的 Hook 默认保持原调用链；唯一例外是自定义入口克隆自 ClearCache 模板，
  插件识别到自己的 `CampusButton` 后必须消费点击，不能继续触发模板原行为；
- 未识别版本默认关闭功能，而不是强行调用猜测签名。

## 15. 日志与诊断

管理器使用独立日志文件：

```text
<游戏目录>\gakumas-mod\mod-manager.log
```

当前统一前缀为 `[GakumasModManager]`，同时输出到 `OutputDebugStringA`。进入正式模块化后可在
消息正文增加 `RuntimeClient`、`TargetResolver`、`UI` 和 `Persistence` 子标签，不再新建
第二份日志。

必须记录：

- 管理器版本、Runtime API 版本与游戏版本；
- PID、进程名和 DLL 实例加载时间，用于区分启动链中的多组加载记录；
- 关键签名验证结果；
- 扫描到的 Mod 数量及服装/发型/异常计数；
- 目标解析成功或失败原因；
- 图标 Cell 绑定失败；
- 开关写入结果；
- 冲突摘要；
- Shutdown 清理结果。

普通日志不输出完整 JSON。开发构建可提供受控的诊断快照，但 Release 默认关闭。

## 18. 测试计划

### 18.1 Runtime 单元测试

- 有效启用 body Mod；
- 有效禁用 body Mod；
- 有效 hair Mod；
- 一个 Manifest 含多条 replacement，即使指向同一逻辑目标，也判定为管理器配置异常；
- 一个 Manifest 指向两个服装，判定异常；
- 一个 Manifest 同时包含 body 与 hair，判定异常；
- `face` 判定为不支持；
- 缺失 `enabled` 时使用兼容默认值；
- Manifest JSON 损坏；
- Bundle 缺失；
- 重复 ID；
- 同 source 冲突；
- 不同优先级与相同优先级冲突；
- 原子写入成功；
- 无写权限；
- 外部并发修改；
- 路径逃逸与重解析点；
- API 缓冲区申请和释放；
- API v1 结构尺寸兼容。

### 18.2 Resolver 测试

- 标准 `cstm` body source；
- `othr` body source；
- 标准 hair source；
- Master 不存在目标；
- 一个 hairAssetId 对应多个 Costume；
- 隐藏或未来 Costume 的选择规则；
- 角色名和服装名为空；
- 原始 source 兜底。

### 18.3 实机测试矩阵

- 16:9、16:10 和常用窗口分辨率；
- 首次进入主页；
- 从其他页面返回主页；
- 连续多次打开/关闭管理器；
- 登出后重新登录；
- 无 Mod；
- 只有禁用 Mod；
- 服装与发型混合列表；
- 长 Mod 名称；
- 30、100 个列表项的滚动；
- 切换后正常退出并重启；
- 游戏被强制关闭后 Manifest 仍有效；
- Runtime 存在但管理器 DLL 缺失；
- 管理器存在但 Runtime API 版本不兼容；
- 汉化插件已安装与未安装两种环境，确认二者互不依赖。

## 19. 第一版验收标准

只有同时满足以下条件，才认为 MVP 完成：

1. 游戏主页存在稳定的“Mod 管理”入口；
2. 页面能列出启用和禁用的有效 AssetBundle Mod；
3. 服装与发型明确分栏或分页；
4. 一个 Mod 只显示一个逻辑目标；
5. body Mod 显示正确的游戏服装图标；
6. hair Mod 显示正确的游戏发型图标，或经过确认的官方头部格子；
7. 正常项只展示玩家可理解的名称，不展示技术 ID；
8. 解析失败只影响单项，并以原始 source 兜底；
9. 开关能可靠写回 `enabled`；
10. 标准服装/发型 Mod 在当前角色上能可靠热开启和热关闭；
11. 重启后 Runtime 实际状态仍与上次选择一致；
12. 冲突、文件损坏和写入失败不会导致页面或游戏崩溃；
13. 游戏 UI 签名不兼容时管理器安全关闭，Runtime 仍正常工作；
14. 发布包不含任何提取的游戏资源。

## 20. 风险与应对

| 风险 | 影响 | 应对 |
|---|---|---|
| 游戏更新改变 UI 类或签名 | 入口或页面失效 | 签名矩阵、启动验证、按能力降级 |
| 原生 Cell 构造依赖复杂 | 图标不能直接显示 | 先复用完整 Presenter；再退到已验证图标加载方法 |
| hair 资源与 CostumeHead 不是一对一 | 发型名称或图标匹配错误 | 通过 hairAssetId、Costume 引用和官方格子三重验证 |
| Runtime 只记录启用项 | 禁用 Mod 无法列出 | 新建完整目录模型，不再从 replacement map 反推 |
| 热关闭后对象仍被引用 | 外观残留、黑屏或崩溃 | 保存 Renderer 原状态，按 Mod 资源引用恢复场景实例与缓存 Prefab；不支持的规则降级为重新加载 |
| Runtime 与管理器各自管理 Hook | 卸载或目标冲突 | 明确 Hook 目标边界，管理器只负责 UI，Runtime 只负责资源替换 |
| Manifest 写入中断 | Mod 配置损坏 | 同目录临时文件、Flush、原子替换 |
| 玩家手动编辑 Manifest | 覆盖外部修改 | 时间戳/摘要检测，冲突时重新解析或拒绝写入 |
| 技术信息污染界面 | 玩家难以理解 | Presentation Model 统一转换，原始字段仅异常兜底 |
| 列表项过多 | 卡顿和内存增长 | 使用游戏可复用 List/Cell，不一次实例化全部卡片 |

## 21. 开放调查项

以下是需要通过实机和 IL2CPP metadata 确认的实现细节，不改变产品方向：

- 最新 Reload 入口缓存失效在返回主页、重登和 View 重建时是否稳定；
- 当前 SettingWindow 容器在关闭、返回、遮罩、输入阻断和不同分辨率下是否完整；
- 服装 Cell 的完整 Model/View/Presenter 签名；
- 发型选择实际使用的 Cell 与数据模型；
- CostumeHead 官方预览图在当前固定行中的最终加载与显示时序；
- 游戏角色显示名的最佳读取路径；
- 官方 Cell 是否能够在非原页面上下文独立加载图标；
- 登录/登出时最可靠的缓存清理事件；
- Runtime 现有 load history 如何稳定映射到 `appliedThisSession`；
- 管理器 DLL 的最终卸载顺序和 UI Hook 清理方式。

这些项目应记录在后续 `SIGNATURE_MATRIX.md` 和调查日志中。未验证前不把猜测写成硬编码。

## 22. 下一步执行顺序

从当前已验证状态继续：

1. **已部署待复验：**执行 Mod → 设置 → 菜单，确认 Reload 后“Mod 管理”入口仍存在；再验证设置 → Mod 与 Mod → Mod；
2. **已部署待复验：**对同一服装 Mod 执行 ON/OFF/ON 并直接返回主页，确认颜色不再需要切换游戏页面；
3. 实机确认发型 `GetThumbAssetName()` + `ThumbnailViewBase.Set()` 的官方预览图最终可见；
4. 实机触发启动时冲突组全关和运行中新 Mod 被拒绝的两种提示；
5. 补完返回主页、重登、重复打开、分辨率和长期运行生命周期；
6. 根据大列表实机结果决定是否把固定设置行升级为专用可复用 Cell；
7. 将 Runtime/Manager API 头文件整理为单一共享 SDK；
8. 接入 `appliedThisSession` 真实记录并补齐异常/兼容测试；
9. 完成发布构建、安装和卸载验证。

当前最优先的是最新入口缓存和即时颜色补丁的实机闭环，然后完成全屏设置模板生命周期与
异常矩阵；不是继续维护旧文字探针，也不是开始借卡或网络 API 功能。
