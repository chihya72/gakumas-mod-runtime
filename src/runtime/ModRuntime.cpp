#include "ModRuntime.hpp"
#include "DriverPrecheck.hpp"

#include "ModIl2cppUtils.hpp"
#include "ModLog.hpp"
#include "ModPaths.hpp"
#include "ReapplyState.hpp"
#include "RuntimeBootstrap.hpp"
#include "ModRuntimeCatalog.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <array>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GakumasMod::Runtime {
    using Il2cppString = UnityResolve::UnityType::String;

    namespace {
        template <typename T>
        using UnityArray = UnityResolve::UnityType::Array<T>;
        using Il2CppGCHandle = void*;

        DWORD WINAPI StartRuntimeAfterModuleLoad(LPVOID) noexcept {
            GakumasMod::Bootstrap::EnsureStarted();
            return 0;
        }

        struct RuntimeLoadTrigger {
            RuntimeLoadTrigger() noexcept {
                wchar_t executablePath[MAX_PATH]{};
                const auto length = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
                if (length == 0 || length >= MAX_PATH) return;

                const wchar_t* executableName = executablePath;
                for (const auto* cursor = executablePath; *cursor; ++cursor) {
                    if (*cursor == L'\\' || *cursor == L'/') executableName = cursor + 1;
                }
                if (CompareStringOrdinal(executableName, -1, L"gakumas.exe", -1, TRUE)
                    != CSTR_EQUAL) {
                    return;
                }

                // This constructor runs while the DLL is being attached.  Windows does not
                // begin the new thread until DLL attach notifications have completed, so the
                // real bootstrap still runs outside the loader lock. EnsureStarted() is guarded
                // by call_once and remains safe if an XInput/API entry wins the race.
                if (const auto thread = CreateThread(
                        nullptr, 0, StartRuntimeAfterModuleLoad, nullptr, 0, nullptr)) {
                    CloseHandle(thread);
                }
            }
        };

        RuntimeLoadTrigger g_runtimeLoadTrigger{};

        struct LocalModMaterialTextureReplacement {
            std::string rendererName;
            int materialSlot{ -1 };
            std::string propertyName;
            std::string assetName;
            std::string typeName{ "Texture2D" };
        };

        struct LocalModMaterialColorReplacement {
            std::string rendererName;
            int materialSlot{ -1 };
            std::string propertyName;
            float r{ 1.0f };
            float g{ 1.0f };
            float b{ 1.0f };
            float a{ 1.0f };
        };

        struct LocalModMaterialFloatReplacement {
            std::string rendererName;
            int materialSlot{ -1 };
            std::string propertyName;
            float value{};
        };

        // 自建半透明材质：游戏自己的 Campus/Actor/Default 只有不透明与镂空两档，
        // 真半透明得用我们随插件发布的 gmi_shaders.bundle 里的 Gmi/Transparent
        // （URP 透明队列，在延迟光照与角色合成之后画，所以不参与 coverage 判定和 SSAO）。
        struct LocalModTransparentMaterial {
            std::string rendererName;
            int materialSlot{ -1 };
            std::string assetName;                 // bundle 内的 t0（带 alpha）
            std::string defMapAsset;               // t1 PackedMask：r=toon 阈值 a=AO
            std::string shadeMapAsset;             // t4：rgb=暗面色 a=分支 mask
            std::string typeName{ "Texture2D" };
            float alpha{ 1.0f };                   // 整体不透明度，与 t0.a 相乘
            float alphaFromTexture{ 1.0f };        // 0 = 忽略 t0.a，只用 alpha
            float cull{ 0.0f };                    // 0=双面 1=剔除正面 2=剔除背面
            float zwrite{ 0.0f };
            float cutoff{ 0.004f };
            float toonStrength{ 1.0f };            // 0 = 纯 unlit（上一版的样子）
            float shadeDarken{ 0.45f };            // 布料暗面 = base × 这个系数
            float toonSoftness{ 0.08f };
            float aoStrength{ 0.5f };
            // -1 = 用 shader 自己的队列(Transparent 3000)。<=2500 会让 URP 在**不透明阶段**
            // 画它 —— 景深/角色遮罩这类后处理读的是那一阶段的深度，透明队列进不去。
            int renderQueue{ -1 };
            // 任意 shader 浮点属性直通（_StencilRef 之类）：调参不用重编 shader
            std::vector<std::pair<std::string, float>> extraFloats{};
            // 对照实验用：不建自己的材质，改克隆游戏槽 0 的不透明材质来画这一段。
            // 结果是"游戏眼里的普通衣服"，用来把「糊」归因到管线还是归因到我们的 shader。
            bool vanillaMaterial{ false };
            // true = 队列由游戏自己的 VL.VLRenderQueue.GBufferTransparentRange 决定，
            // 落进原生 G-buffer 阶段那一趟（配合 shader 的 UniversalGBufferActor pass）。
            bool gbufferQueue{ false };
        };

        struct LocalModUnityColor {
            float r{ 1.0f };
            float g{ 1.0f };
            float b{ 1.0f };
            float a{ 1.0f };
        };

        struct LocalModMaterialSlotCopy {
            std::string rendererName;
            int fromSlot{ -1 };
            int toSlot{ -1 };
        };

        struct LocalModRendererRule {
            std::string rendererId;
            std::string targetRenderer;
            std::string modRenderer;
        };

        struct LocalModAssetReplacement {
            std::string modId;
            std::string modName;
            std::string manifestPath;
            std::string sourceName;
            std::string part;
            std::string assetName;
            std::string skeletonAssetName;
            std::string bundlePath;
            std::string typeName;
            bool replaceWholeObject{};
            bool attachToOriginal{};
            std::string rendererName;
            int priority{};
            bool sessionEnabled{true};
            bool replaceMaterials{};
            Il2CppGCHandle bundleHandle{};
            std::vector<LocalModRendererRule> rendererRules{};
            std::vector<LocalModMaterialSlotCopy> materialCopies{};
            std::vector<LocalModMaterialTextureReplacement> materialTextures{};
            std::vector<LocalModMaterialColorReplacement> materialColors{};
            std::vector<LocalModMaterialFloatReplacement> materialFloats{};
            std::vector<LocalModTransparentMaterial> transparentMaterials{};
            void* attachAsset{};
            std::vector<void*> attachSourceMeshes{};
        };

        struct LocalModRendererPair {
            void* originalRenderer{};
            void* modRenderer{};
            size_t originalIndex{};
            size_t modIndex{};
        };

        struct PersistentMaterialTextureOverride {
            int propertyId{};
            std::string propertyName;
            void* texture{};
            // What the property held before the Mod wrote over it.  Only the
            // live-instance path needs it: there the write lands on a material
            // the game owns, so turning the Mod off has to put the original
            // texture back rather than swap the material out.
            void* previousTexture{};
        };

        struct RendererSlotTextureOverrides {
            int materialIndex{};
            std::vector<PersistentMaterialTextureOverride> textures{};
        };

        struct ReversibleRendererPatch {
            std::string modId;
            std::string sourceName;
            std::string rendererName;
            void* patchedRenderer{};
            void* patchedMesh{};
            void* originalMesh{};
            int sourceRootDepth{};
            // Keep the complete Mod material array.  patchedMaterialKeys is
            // useful for identity checks, but cannot reconstruct slot order
            // when only some slots differ from the original.
            std::vector<void*> patchedMaterials{};
            std::vector<void*> patchedMaterialKeys{};
            std::vector<void*> originalMaterials{};
            std::vector<std::string> originalBoneNames{};
            std::string originalRootBoneName;
        };

        using ReapplyRendererIdentity = Detail::ReapplyRendererIdentity;

        struct ActiveAnimationRigContext {
            void* rig{};
            void* initializeData{};
            void* rootTransform{};
            void* rootGameObject{};
        };

        struct RendererPropertyBlockSnapshot {
            void* block{};
            Il2CppGCHandle handle{};
            bool empty{};
        };

        struct LocalModBoneWeight {
            float weight0{};
            float weight1{};
            float weight2{};
            float weight3{};
            int boneIndex0{};
            int boneIndex1{};
            int boneIndex2{};
            int boneIndex3{};
        };

        // The swing params the source bundle authors per bone. Everything else the
        // runtime bone exposes (rootWeight, pendulum, wind...) is computed rather than
        // authored — source m_Weight is 1.0 on every bone while live base bones read
        // rootWeight=0.3 — so we only carry these and leave the rest to the game.
        // P3 的驱动器数据结构与"必需引用预检"都在 DriverPrecheck.hpp（那段纯逻辑要离线测；
        // 只对新建骨有意义 —— humanoid 肢体上那 16 个驱动器由原版 prefab 自带，530 套里 528 套
        // 都有，AB 路线继承宿主，缺的是落在那些骨上的权重，见 P1）。

        struct LocalIpBoneSwing {
            float damping{};
            float stiffness{};
            float spring{};
            float mass{};
            bool useWindGlobalForce{};
            // rootWeight = 跟随根骨的比例。1.0 = 完全刚性跟随 = 锁死在 rest 姿态、不下垂。
            // SetDefaultValues 给 1.0，而实测 base 裙摆是 0.3。（护士服那轮我曾断言
            // 「m_Weight 不是 rootWeight」，依据是 rui 源全 1.0 而 base 运行时 0.3 —— 那个
            // 对比无效：rui 是偶像荣耀的服装、base 是学马的，不同服装本就可以不同授权值。）
            // pendulum = 单摆项(朝重力)，base 裙摆 0.001，SetDefaultValues 给 0 → 不下垂。
            // 负数 = sidecar 没提供，不要动。
            float rootWeight{ -1.0f };
            float pendulum{ -1.0f };
            // pendulumRange = pendulum 的作用范围。530 套原版实测 84.6% 取 1.0；留 0（也就是
            // SetDefaultValues 的值）等于把重力项乘没了 —— 骨"参数齐全"却不下垂。
            // wind 同理，原版 84.6% 取 1.0，不写就是 0 = 不受风。负数 = sidecar 没提供、不要动。
            float pendulumRange{ -1.0f };
            float wind{ -1.0f };
            int dynamicType{ -1 };   // 0=Swing 1=Slide，袖类原版用 Slide
            // 每轴角度限位（度）。原版 88.3% 开着；不开的话摆动没有边界，容易穿模。
            int useLimit{ -1 };
            int limit[3][2]{};
            // 碰撞：摇物骨的 dynamicCollider 与身体骨的 staticCollider 配对。身体骨
            // (Head/Neck/Spine*/Hips/Pelvis/Left|RightArm/ForeArm/Leg) 的 staticCollider 是
            // 游戏自带的，我们只需给自己的骨一个半径；不给的话 SetDefaultValues 留下的是
            // 空碰撞体 → 手臂直接穿过裙子。radius<0 表示 sidecar 没提供、不要动。
            float colliderRadius{ -1.0f };
            float colliderRadiusSub{ 0.05f };
            int colliderType{};
            int collisionMask{ -1 };
        };

        struct LocalIpBone {
            std::string name;
            int parentIndex{ -1 };
            UnityResolve::UnityType::Vector3 localPosition{};
            UnityResolve::UnityType::Quaternion localRotation{};
            UnityResolve::UnityType::Vector3 localScale{};
            std::optional<LocalIpBoneSwing> swing{};
            std::optional<LocalQuartzDriver> driver{};
        };

        // The unweighted tip of each swing chain. Skinning doesn't need them so they
        // never reach the mesh's bone array, but the sim does — a chain's last segment
        // is defined by its tip. They stay out of LocalIpBone because that list must
        // remain index-aligned with the mod mesh's bones, so they attach by parent name.
        struct LocalIpExtraBone {
            std::string name;
            std::string parentName;
            UnityResolve::UnityType::Vector3 localPosition{};
            UnityResolve::UnityType::Quaternion localRotation{};
            UnityResolve::UnityType::Vector3 localScale{};
            std::optional<LocalIpBoneSwing> swing{};
            std::optional<LocalQuartzDriver> driver{};
        };

        // 一条要新建的 ActorSwingChain：挂在哪根骨上、哪些新骨是它的链根。
        // 建不建链、按什么分组由导出器决定（见 core.build_swing_chains），运行时照单执行。
        struct LocalIpSwingChain {
            std::string host;
            std::string category;
            int chainLength{};
            std::vector<std::string> rootBones;
            // 环形碰撞（ChainLayerInfo.around）。-1 = sidecar 没说、保持默认 false。
            // 原版 40% 的层开着，但逐链手调无规律，所以不猜——见 ConfigureModChainLayers。
            int around{ -1 };
        };

        // Protocol 2 is deliberately test-runtime-only.  A release runtime only accepts
        // protocol 1, so an experimental source-proxy package fails closed instead of
        // silently falling back to the hybrid graft and producing misleading geometry.
        struct LocalIpSidecarOptions {
            int runtimeProtocol{ 1 };
            bool sourceProxyRestOnly{};
            std::string sourceProxyRootBoneName;
            // Protocol 2 splits what protocol 1 crammed into one array.  `transforms`
            // (parsed into the shared bone vector) is the complete source hierarchy —
            // unweighted ancestors, sockets and all.  `skinBones` indexes into it in
            // renderer bone order.  The A-pose package that only applied materials died
            // exactly here: its declared rootBone `Hips` is a real source transform but
            // not a weighted bone, so a skin-bone-only array could never contain it.
            bool sourceProxyHasTransforms{};
            std::vector<int> sourceProxySkinBones;
            std::string sourceProxyRootTransformName;
            // Gakumas human semantic -> index into the transform tree.  Unused while the
            // mode is rest-only; validated now so the animation bridge stage inherits a
            // resolved mapping instead of re-parsing names at 60fps.
            std::vector<std::pair<std::string, int>> sourceProxySemanticMap;
            int sourceProxyHeadSocket{ -1 };
            // 0 = rest-only (the probe that answered "can the source rig render at all").
            // 1 = minimal bridge: the roadmap's step 3 set, no fingers.
            // 2 = every mapped semantic.
            int sourceProxyAnimationMode{};
        };

        // One driven bone, by NAME — the replacement runs on the loaded PREFAB, so every
        // transform reachable while arming belongs to the asset, and the game renders an
        // Instantiate() of it.  Writing to the asset's own transforms is invisible by
        // construction; only the correction survives the copy (it is a ratio of two rests,
        // so the instance's actor rotation cancels out of it exactly as the capture
        // frame's did).
        struct SourceProxyBoneLink {
            std::string gameBoneName;    // the vanilla bone, named after its semantic
            std::string proxyBoneName;   // "__gmi_sp_<index>_<source name>"
            // gameRestWorld^-1 * proxyRestWorld, captured in one world frame so the
            // actor's orientation at capture time cancels out of it.
            UnityResolve::UnityType::Quaternion correction{};
        };

        struct SourceProxyBridge {
            void* renderer{};
            std::string sourceName;
            std::vector<SourceProxyBoneLink> links;   // parents before children
            // The head socket runs the OTHER way: the face and hair parts ride the vanilla
            // `Head` bone, so that bone is snapped onto the source rig's head each frame.
            std::string headGameName;
            std::string headProxyName;
            // Humanoid puts locomotion on the Hips POSITION channel, and the bridge only
            // ever writes rotations — which is why the source body played every walk and run
            // on the spot.  Followed as a DELTA from each rig's own rest, never as an
            // absolute copy: the source hips sit 12cm lower than the game's, so copying the
            // position outright would hang the body in the air by that much.
            std::string hipsGameName;
            std::string hipsProxyName;
            // proxyRestLocal - gameRestLocal, in the shared parent frame (both hips hang off
            // `Reference`, and the proxy container sits there with an identity transform).
            UnityResolve::UnityType::Vector3 hipsRestDelta{};
        };

        std::vector<SourceProxyBridge> g_sourceProxyBridges;
        // 无锁快路。桥是 protocol=2 的逐包 opt-in，普通包一条都不会有，但
        // `LateUpdate` 的 hook 是无条件装的 —— 没有这个标志，每帧、每个 actor 都要去抢
        // `g_swingStateMutex`，而那把锁在后台线程做 graft 时会被占很久，换装那一瞬间
        // 主线程就被挡在这里。只在装桥处置位，其余时间一次 relaxed 读就返回。
        std::atomic_bool g_sourceProxyBridgesArmed{};

        // The same link resolved against one live actor's own copies of those transforms.
        struct SourceProxyLiveLink {
            UnityResolve::UnityType::Transform* gameBone{};
            UnityResolve::UnityType::Transform* proxyBone{};
            UnityResolve::UnityType::Quaternion correction{};
        };
        struct SourceProxyLiveBridge {
            std::vector<SourceProxyLiveLink> links;
            UnityResolve::UnityType::Transform* headGameBone{};
            UnityResolve::UnityType::Transform* headProxyBone{};
            UnityResolve::UnityType::Transform* hipsGameBone{};
            UnityResolve::UnityType::Transform* hipsProxyBone{};
            UnityResolve::UnityType::Vector3 hipsRestDelta{};
        };
        // key = CampusActorController.  Empty links is a cached "not a modded actor";
        // RegisterBones clears the map, so an actor that ticked before its body was
        // attached gets another chance instead of being written off forever.
        std::unordered_map<void*, SourceProxyLiveBridge> g_sourceProxyLiveBridges;

        struct ActorSwingInitialTransform {
            UnityResolve::UnityType::Vector3 localPosition{};
            UnityResolve::UnityType::Quaternion localRotation{};
            UnityResolve::UnityType::Vector3 position{};
            UnityResolve::UnityType::Quaternion rotation{};
        };

        using AssetBundleLoadAssetFn = void* (*)(void*, Il2cppString*, void*);
        using AssetBundleLoadAssetAsyncFn = void* (*)(void*, Il2cppString*, void*);
        using AssetBundleRequestGetResultFn = void* (*)(void*);
        using AssetBundleRequestGetAssetFn = void* (*)(void*);
        using CampusActorAnimationRigRegisterBonesFn = void (*)(void*, void*);
        using CampusActorControllerLateUpdateFn = void (*)(void*, void*);
        // These are managed IL2CPP methods, not native icalls. Unity 6 method
        // pointers include the trailing MethodInfo* argument.
        using RendererSetPropertyBlockFn = void (*)(void*, void*, void*);
        using RendererSetPropertyBlockMaterialIndexFn = void (*)(void*, void*, int, void*);
        using MaterialSetTextureFn = void (*)(void*, int, void*, void*);
        using MaterialSetTextureStringFn = void (*)(void*, Il2cppString*, void*, void*);

        AssetBundleLoadAssetFn AssetBundle_LoadAsset_Orig{};
        AssetBundleLoadAssetAsyncFn AssetBundle_LoadAssetAsync_Orig{};
        AssetBundleRequestGetResultFn AssetBundleRequest_GetResult_Orig{};
        AssetBundleRequestGetAssetFn AssetBundleRequest_get_asset_Orig{};
        CampusActorAnimationRigRegisterBonesFn CampusActorAnimationRig_RegisterBones_Orig{};
        CampusActorControllerLateUpdateFn CampusActorController_LateUpdate_Orig{};
        using CampusActorControllerBuildModelFn = void (*)(void*, void*, void*);
        CampusActorControllerBuildModelFn CampusActorController_BuildModel_Orig{};
        RendererSetPropertyBlockFn Renderer_SetPropertyBlock_Orig{};
        RendererSetPropertyBlockMaterialIndexFn Renderer_SetPropertyBlockMaterialIndex_Orig{};
        MaterialSetTextureFn Material_SetTexture_Orig{};
        MaterialSetTextureStringFn Material_SetTextureString_Orig{};

        std::atomic_bool g_initialized{};
        std::vector<void*> g_hookTargets{};
        std::mutex g_historyMutex;
        std::mutex g_bundleMutex;
        std::mutex g_materialOverrideMutex;
        std::mutex g_propertyBlockScratchMutex;
        std::mutex g_propertyBlockSnapshotMutex;
        std::mutex g_reversiblePatchMutex;
        std::mutex g_animationRigMutex;
        std::mutex g_pendingReapplyMutex;
        std::unordered_map<void*, std::string> g_loadHistory{};

        std::unordered_map<std::string, Il2CppGCHandle> g_bundleHandleMap{};
        using LocalModAssetReplacementPtr = std::shared_ptr<LocalModAssetReplacement>;
        std::shared_mutex g_replacementMutex;
        std::vector<LocalModAssetReplacementPtr> g_registeredReplacements{};
        std::unordered_map<std::string, LocalModAssetReplacementPtr> g_replacementMap{};
        std::unordered_map<std::string, Il2CppGCHandle> g_loadedAssetHandleMap{};
        std::unordered_set<void*> g_transformedMeshSet{};
        // Names (GameObject names) of mod-created ActorSwingDynamicBone. Matched by name
        // (not pointer) because the graft runs on the loaded prefab; the game then
        // Instantiates it, so the scene clone's bones are different pointers with the
        // same names. See HasModBonesUnder / BuildLayersForModChains.
        std::unordered_set<std::string> g_createdActorSwingBoneNames{};
        // 上面那张集合是**进程级**的，只够用来回答"这根骨是不是某个 mod 建的"（诊断、门控）。
        // 判断"能不能复用一根已存在的骨"必须按**归属**分开，key = modId|sidecar指纹|source：
        // body 包和 hair 包挂在同一个角色层级下，两个 mod 也可能先后换同一个部位 —— 同名自
        // 定义骨若只按 source 隔离，后来者会直接复用前者、跳过自己的父级/变换/物理参数。
        // 指纹进 key 是为了原地更新（modId 不变、骨名不变、只改了 TRS/摆参/链）也不复用旧骨。
        std::unordered_map<std::string, std::unordered_set<std::string>> g_createdBonesByOwner{};
        // Keyed by clone so a hot OFF can drop the one it just detached.  Every
        // hot ON clones the Mod mesh again (~16 MB for a 170k-vertex body), and
        // a pinned handle the runtime never releases turned each toggle into a
        // permanent leak.  Asset loads run off the main thread, so this needs
        // its own lock.
        std::mutex g_runtimeMeshHandleMutex;
        std::unordered_map<void*, Il2CppGCHandle> g_runtimeMeshHandles{};
        std::vector<Il2CppGCHandle> g_runtimeBoneHandles{};
        std::vector<Il2CppGCHandle> g_runtimeMaterialHandles{};
        // 缓存必须带身份：只按 renderer + 骨数量判命中的话，换成另一个骨数相同的 mod 会
        // 直接拿到上一个 mod 的骨数组。按骨名逐个比也不够——同一个 mod 原地更新、骨名一字
        // 未改而 TRS/摆动参数/swingChains 全变了照样命中旧缓存。ownerKey 里带的是 sidecar
        // **全文指纹**（含 buildId），改一个字节就换一份缓存。
        struct HybridBoneCache {
            std::string ownerKey;
            std::vector<void*> bones;
        };
        std::unordered_map<void*, HybridBoneCache> g_hybridBonesByRenderer{};
        // 资产加载会离开主线程（见 g_runtimeMeshHandles 的注释），body 和 hair 可能并发
        // graft —— 这些容器全是裸的 unordered_map/set/vector，同时读写是 UB。
        std::mutex g_swingStateMutex;
        std::unordered_map<void*, std::vector<PersistentMaterialTextureOverride>> g_materialTextureOverrides{};

        std::unordered_set<std::string> SnapshotCreatedActorSwingBoneNames() {
            std::lock_guard lock(g_swingStateMutex);
            return g_createdActorSwingBoneNames;
        }
        // 宿主骨名 → sidecar 声明的 `around`（-1 = 没声明）。建链时记下，等
        // BuildLayersForModChains 在 RegisterBones 钩子里跑到时才用得上——那里拿不到 sidecar。
        // 用的是同一把 g_swingStateMutex：graft 在后台线程，裸读是数据竞争。
        std::unordered_map<std::string, int> g_modChainAroundByHost{};
        std::unordered_map<std::string, int> SnapshotModChainAround() {
            std::lock_guard lock(g_swingStateMutex);
            return g_modChainAroundByHost;
        }
        std::unordered_map<void*, std::vector<RendererSlotTextureOverrides>> g_rendererTextureOverrideCache{};
        // Internal Runtime material assignments (initial apply and OFF restore)
        // must not be mistaken for the game's post-toggle write that the hook
        // is meant to repair.
        thread_local bool t_internalMaterialAssignment = false;
        std::unordered_set<void*> g_privateMaterials{};
        std::unordered_set<void*> g_loggedPersistentRenderers{};
        // Renderers the game submits property blocks for that carry no mod
        // overrides -- diagnostic only, see ApplyPersistentTextureOverrides.
        std::unordered_set<void*> g_loggedUnmanagedRenderers{};
        std::unordered_set<void*> g_loggedGameTextureWrites{};
        std::unordered_map<void*, std::unordered_set<int>> g_runtimeOwnedPropertyBlockSlots{};
        std::unordered_map<void*, std::unordered_map<int, RendererPropertyBlockSnapshot>>
            g_rendererPropertyBlockSnapshots{};
        std::vector<ReversibleRendererPatch> g_reversibleRendererPatches{};
        std::unordered_map<std::string, std::vector<ReapplyRendererIdentity>>
            g_reapplyRendererIdentities{};
        std::vector<ActiveAnimationRigContext> g_activeAnimationRigs{};
        std::vector<Detail::PendingReapplyRequest> g_pendingReapplies{};
        Il2CppGCHandle g_propertyBlockScratchHandle{};
        UnityResolve::Method* g_rendererSetPropertyBlockMethod{};
        UnityResolve::Method* g_rendererSetPropertyBlockMaterialIndexMethod{};
        UnityResolve::Method* g_rendererGetPropertyBlockMethod{};
        UnityResolve::Method* g_rendererGetPropertyBlockMaterialIndexMethod{};
        UnityResolve::Method* g_materialPropertyBlockSetTextureMethod{};
        UnityResolve::Method* g_materialPropertyBlockIsEmptyMethod{};
        UnityResolve::Method* g_shaderPropertyToIdMethod{};
        UnityResolve::Method* g_materialSetTextureMethod{};
        UnityResolve::Method* g_materialSetTextureStringMethod{};
        std::unordered_set<std::string> g_dumpedProfiles{};
        std::atomic_bool g_rigRegisterObserved{};
        std::atomic_bool g_nativeChainValidation{};
        std::atomic_bool g_hasPendingReapplies{};
        std::atomic_bool g_pendingReapplyInFlight{};
        std::unordered_set<void*> g_nativeChainAttachedRoots{};

        bool AttachNativeChainToLiveRoot(UnityResolve::UnityType::Transform* rootTransform);
        void RestoreRendererPropertyBlockSnapshots(void* renderer, size_t materialCount);
        void ApplyPersistentTextureOverrides(void* renderer);
        void RetryPendingLiveReapplies(
            const char* trigger,
            void* observedRenderer = nullptr);

        std::string ToLowerAscii(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::string NormalizeAssetName(std::string name) {
            std::replace(name.begin(), name.end(), '\\', '/');
            return ToLowerAscii(std::move(name));
        }

        std::string SanitizeFileName(std::string value) {
            for (auto& c : value) {
                if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
                    c = '_';
                }
            }
            return value.empty() ? "unknown" : value;
        }

        const char* GetUnityObjectClassName(void* obj) {
            const auto klass = Il2cppUtils::get_class_from_instance(obj);
            return klass && klass->name ? klass->name : "null";
        }

        std::string GetUnityObjectNameString(void* obj) {
            if (!obj) return {};
            static auto Object_get_name = reinterpret_cast<Il2cppString * (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Object", "get_name"));
            if (Object_get_name) {
                if (const auto name = Object_get_name(obj)) {
                    if (const auto value = name->ToString(); !value.empty()) {
                        return value;
                    }
                }
            }

            static auto Object_GetName = reinterpret_cast<Il2cppString * (*)(void*)>(
                Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Object::GetName(UnityEngine.Object)"));
            const auto name = Object_GetName ? Object_GetName(obj) : nullptr;
            return name ? name->ToString() : std::string{};
        }

        bool IsNativeObjectAlive(void* obj) {
            if (!obj) return false;
            static auto IsNativeObjectAliveFn = reinterpret_cast<bool (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Object", "IsNativeObjectAlive"));
            return IsNativeObjectAliveFn ? IsNativeObjectAliveFn(obj) : true;
        }

        void LogAssetTrace(const char* hookName, const std::string& name, void* result = nullptr, void* requestType = nullptr) {
            const auto lowered = ToLowerAscii(name);
            if (lowered.find("mdl_chr_") == std::string::npos
                && lowered.find("geo_body") == std::string::npos
                && lowered.find("t_chr_") == std::string::npos) {
                return;
            }

            Log::InfoFmt("[ModAssetTrace] %s name=\"%s\" requestType=%p result=%p resultType=%s",
                hookName,
                name.c_str(),
                requestType,
                result,
                GetUnityObjectClassName(result));
        }

        std::optional<std::string> GetJsonString(const nlohmann::json& data, const char* key) {
            if (!data.contains(key) || !data[key].is_string()) return std::nullopt;
            return data[key].get<std::string>();
        }

        std::optional<std::string> GetFirstJsonString(const nlohmann::json& data, std::initializer_list<const char*> keys) {
            for (const auto key : keys) {
                if (const auto value = GetJsonString(data, key)) {
                    return value;
                }
            }
            return std::nullopt;
        }

        int GetJsonInt(const nlohmann::json& data, const char* key, const int fallback) {
            return data.contains(key) && data[key].is_number_integer()
                ? data[key].get<int>()
                : fallback;
        }

        float GetJsonFloat(const nlohmann::json& data, const char* key, const float fallback) {
            return data.contains(key) && data[key].is_number()
                ? data[key].get<float>()
                : fallback;
        }

        std::string InferPartFromAssetName(const std::string& sourceName) {
            const auto normalized = NormalizeAssetName(sourceName);
            if (normalized.find("_face") != std::string::npos || normalized.ends_with("-face")) return "face";
            if (normalized.find("_hair") != std::string::npos || normalized.ends_with("-hair")) return "hair";
            if (normalized.find("_body") != std::string::npos || normalized.ends_with("-body")) return "body";
            return {};
        }

        bool IsValidPart(const std::string& part) {
            return part.empty() || part == "face" || part == "hair" || part == "body";
        }

        bool AttachIl2cppThread(const HMODULE gameAssembly) {
            using DomainGetFn = void* (*)();
            using ThreadAttachFn = void* (*)(void*);

            const auto domainGet = reinterpret_cast<DomainGetFn>(GetProcAddress(gameAssembly, "il2cpp_domain_get"));
            const auto threadAttach = reinterpret_cast<ThreadAttachFn>(GetProcAddress(gameAssembly, "il2cpp_thread_attach"));
            if (!domainGet || !threadAttach) {
                Log::Error("[ModAsset] Cannot resolve il2cpp thread attach exports.");
                return false;
            }

            const auto domain = domainGet();
            if (!domain) {
                Log::Error("[ModAsset] il2cpp_domain_get returned null.");
                return false;
            }

            const auto thread = threadAttach(domain);
            if (!thread) {
                Log::Error("[ModAsset] il2cpp_thread_attach returned null.");
                return false;
            }
            return true;
        }

        UnityResolve::Method* FindMethodByNameAndArgCount(UnityResolve::Class* klass,
            const std::string& methodName,
            const size_t argCount) {
            if (!klass) return nullptr;
            for (const auto method : klass->methods) {
                if (!method || method->name != methodName || method->args.size() != argCount) continue;
                return method;
            }
            return nullptr;
        }

        UnityResolve::Class* FindClassByName(const std::string& className) {
            for (const auto assembly : UnityResolve::assembly) {
                if (!assembly) continue;
                for (const auto klass : assembly->classes) {
                    if (klass && klass->name == className) return klass;
                }
            }
            return nullptr;
        }

        void* AddComponentByClass(UnityResolve::UnityType::GameObject* gameObject,
            UnityResolve::Class* componentClass) {
            if (!gameObject || !componentClass) return nullptr;
            static auto gameObjectClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
            static auto addComponent = FindMethodByNameAndArgCount(gameObjectClass, "AddComponent", 1);
            return addComponent
                ? addComponent->Invoke<void*>(gameObject, componentClass->GetType())
                : nullptr;
        }

        // 按名字 + **参数类型**找方法。只比参数个数是不够的：`GameObject.GetComponent` 有
        // `(Type)` 和 `(string)` 两个单参重载，`FindMethodByNameAndArgCount` 取的是先遍历到的
        // 那个，顺序不确定。拿错重载的后果是把 `Il2CppReflectionType*` 当 `System.String*` 传
        // —— 而这里要用它做 INV-1 的闸门，**一个静默失效的闸门比没有闸门更糟**。
        UnityResolve::Method* FindMethodByArgType(UnityResolve::Class* klass,
            const std::string& methodName, const std::string& argTypeName) {
            if (!klass) return nullptr;
            for (const auto method : klass->methods) {
                if (!method || method->name != methodName || method->args.size() != 1) continue;
                const auto arg = method->args[0];
                if (arg && arg->pType && arg->pType->name.find(argTypeName) != std::string::npos) {
                    return method;
                }
            }
            return nullptr;
        }

        // 查一根骨上有没有某个组件。INV-1（一根骨一个求解器）的闸门要它 —— 挂之前先看，
        // 不看的后果实测过：12 根 `*_H` 各挂两个驱动器，游戏走到 BuildAvatar 就停。
        void* GetComponentByClass(UnityResolve::UnityType::GameObject* gameObject,
            UnityResolve::Class* componentClass) {
            if (!gameObject || !componentClass) return nullptr;
            static auto gameObjectClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
            static auto getComponent = FindMethodByArgType(gameObjectClass, "GetComponent", "Type");
            if (!getComponent) {
                // 解析不出正确重载就**当作"已被占用"**处理（调用方会拒绝挂载），
                // 而不是返回 nullptr 让闸门放行。
                Log::Warn("[ModAsset] 找不到 GameObject.GetComponent(Type)，驱动器闸门无法判断，按占用处理");
                return gameObject;
            }
            return getComponent->Invoke<void*>(gameObject, componentClass->GetType());
        }

        // Reliable managed List<T>.Add. UnityResolve's List::Add calls the un-inflated
        // generic List`1::Add and faults ("Add Invoke Error"); resolve the INFLATED Add
        // from the list's actual runtime class and go through il2cpp_runtime_invoke, which
        // sets up the generic RGCTX correctly. Returns false if the call raised.
        bool ListAddManaged(void* listObj, void* item) {
            if (!listObj) return false;
            const auto klass = Il2cppUtils::get_class_from_instance(listObj);
            if (!klass) return false;
            const auto method = UnityResolve::Invoke<void*>(
                "il2cpp_class_get_method_from_name", klass, "Add", 1);
            if (!method) return false;
            void* args[1] = { item };
            void* exc = nullptr;
            UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", method, listObj, args, &exc);
            return exc == nullptr;
        }

        // Allocate a fresh managed object of the same class as templateObj (for a generic
        // List, the same instantiation — e.g. reuse an existing chain's rootBones as the
        // List<ActorSwingDynamicBone> type template; also used to clone a ChainLayerInfo).
        // il2cpp_object_new + parameterless ctor. Returns nullptr on failure.
        void* CreateObjectLike(void* templateObj) {
            if (!templateObj) return nullptr;
            const auto klass = Il2cppUtils::get_class_from_instance(templateObj);
            if (!klass) return nullptr;
            const auto obj = UnityResolve::Invoke<void*>("il2cpp_object_new", klass);
            if (!obj) return nullptr;
            const auto ctor = UnityResolve::Invoke<void*>(
                "il2cpp_class_get_method_from_name", klass, ".ctor", 0);
            if (ctor) {
                void* exc = nullptr;
                UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", ctor, obj, nullptr, &exc);
                if (exc) return nullptr;
            }
            return obj;
        }

        // prefab 上销毁一个组件。用 DestroyImmediate —— prefab 不在场景里，Destroy 的延迟
        // 销毁要等下一帧，而这里必须在 Instantiate 之前就把它去掉。
        void DestroyComponentImmediate(void* component) {
            static auto method = FindMethodByNameAndArgCount(
                Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Object"),
                "DestroyImmediate", 1);
            if (component && method) method->Invoke<void>(component);
        }

        // 按类名新建一个托管对象。`Class::address` 就是 Il2CppClass*，所以不像 CreateObjectLike
        // 那样需要现成实例当模板 —— 这正是 dynamicCollider / limitInfo 这两个"建骨时还是 null"
        // 的引用字段所缺的东西。
        void* CreateManagedObject(const char* className) {
            const auto klass = FindClassByName(className);
            if (!klass || !klass->address) return nullptr;
            const auto obj = UnityResolve::Invoke<void*>("il2cpp_object_new", klass->address);
            if (!obj) return nullptr;
            if (const auto ctor = UnityResolve::Invoke<void*>(
                "il2cpp_class_get_method_from_name", klass->address, ".ctor", 0)) {
                void* exc = nullptr;
                UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", ctor, obj, nullptr, &exc);
                if (exc) return nullptr;
            }
            return obj;
        }

        // 往托管对象的**引用字段**写值，走 GC write barrier。
        //
        // 裸指针直写对 float/int 那些值类型没问题，但写引用会绕过增量 GC 的写屏障 ——
        // GC 不知道这个新引用存在，可能在下一轮把对象回收掉，表现是跑一阵之后随机失效或崩。
        // `il2cpp_gc_wbarrier_set_field` 拿不到时退回直写并告警：直写至少还能工作，
        // 静默什么都不做才是最坏的。
        void SetManagedReferenceField(void* object, std::uintptr_t fieldOffset, void* value) {
            const auto slot = reinterpret_cast<void**>(
                reinterpret_cast<std::uintptr_t>(object) + fieldOffset);
            static const auto barrier = reinterpret_cast<void*>(GetProcAddress(
                GetModuleHandleW(L"GameAssembly.dll"), "il2cpp_gc_wbarrier_set_field"));
            if (barrier) {
                UnityResolve::Invoke<void>("il2cpp_gc_wbarrier_set_field", object, slot, value);
                return;
            }
            static bool warned = false;
            if (!warned) {
                warned = true;
                Log::Warn("[ModAsset] il2cpp_gc_wbarrier_set_field 不可用，托管引用改为直写"
                    "（绕过 GC 写屏障，长时间运行可能丢引用）");
            }
            *slot = value;
        }

        // 取（必要时新建）某个引用字段指向的对象。
        void* EnsureReferenceField(void* component, UnityResolve::Class* componentClass,
            const char* fieldName, const char* className) {
            const auto field = componentClass->Get<UnityResolve::Field>(fieldName);
            if (!field) return nullptr;
            const auto slot = reinterpret_cast<void**>(
                reinterpret_cast<std::uintptr_t>(component) + field->offset);
            if (!*slot) {
                if (const auto created = CreateManagedObject(className)) {
                    SetManagedReferenceField(component, field->offset, created);
                }
            }
            return *slot;
        }

        // P3：把学马自己的姿势驱动器挂到一根**我们新建的**衣物骨上。
        //
        // INV-1（一根骨只能有一个写它的求解器）在这里强制：原版 530 套里 327 个裙摆驱动器
        // 与 ActorSwing 组件**零重叠**，而违反它的那一版直接把加载搞崩了（2026-08-15，12 根
        // `*_H` 各挂两个驱动器 → 走到 BuildAvatar 就停）。所以先查再挂，撞上就拒绝并记日志。
        //
        // setting 是**引用字段**（`public class ...Setting`，不是 struct），`SetDefaultValues`
        // 不会建它 —— 和 dynamicCollider / limitInfo 一样，得自己 new 完再写。
        bool AttachQuartzDriver(UnityResolve::UnityType::GameObject* gameObject,
            const LocalQuartzDriver& driver,
            const std::function<UnityResolve::UnityType::GameObject*(const std::string&)>& resolveBone) {
            if (!gameObject || driver.type.empty()) return false;
            const auto componentName = "ActorAnimationQuartzDriver" + driver.type + "Bone";
            const auto settingName = "ActorAnimationQuartzDriver" + driver.type + "Setting";
            const auto componentClass = FindClassByName(componentName.c_str());
            if (!componentClass) {
                Log::WarnFmt("[ModAsset] 未知驱动器类型 %s（找不到 %s），跳过",
                    driver.type.c_str(), componentName.c_str());
                return false;
            }
            // INV-1 闸门：这根骨上已经有摇物或别的驱动器就不挂。
            for (const char* occupied : { "ActorSwingDynamicBone", "ActorSwingStaticBone" }) {
                if (const auto other = FindClassByName(occupied)) {
                    if (GetComponentByClass(gameObject, other)) {
                        Log::WarnFmt("[ModAsset] %s 上已有 %s，拒绝再挂 %s（一根骨只能有一个求解器）",
                            gameObject->GetName().c_str(), occupied, componentName.c_str());
                        return false;
                    }
                }
            }
            if (GetComponentByClass(gameObject, componentClass)) {
                Log::WarnFmt("[ModAsset] %s 上已有 %s，不重复挂",
                    gameObject->GetName().c_str(), componentName.c_str());
                return false;
            }

            // 预检**在 AddComponent 之前**：缺任一必需引用就整体拒绝，什么都不挂。
            //
            // 旧写法是先挂组件、再逐项写引用，失败只 warn 后 continue/return —— prefab 上于是
            // 留下一个半初始化的组件：它照样被 Instantiate、照样 OnEnable，然后按空引用跑
            // （日志里只有一行 warn）。这类"日志说没成，画面上组件却在跑"的洞正是这一版要消灭的。
            const auto settingClass = FindClassByName(settingName.c_str());
            if (!settingClass) {
                Log::ErrorFmt("[ModAsset] 找不到 %s，拒绝挂 %s（预检失败，未改动 prefab）",
                    settingName.c_str(), componentName.c_str());
                return false;
            }
            const auto missing = MissingDriverReferences(settingName, driver,
                [&](const std::string& name) {
                    return settingClass->Get<UnityResolve::Field>(name) != nullptr;
                },
                [&](const std::string& boneName) {
                    return resolveBone && resolveBone(boneName) != nullptr;
                });
            if (!missing.empty()) {
                Log::ErrorFmt("[ModAsset] %s 上拒绝挂 %s：缺 %zu 项必需引用（%s）。"
                    "未改动 prefab —— 半挂上去的驱动器会按空引用跑，比不挂更坏",
                    gameObject->GetName().c_str(), componentName.c_str(),
                    missing.size(), JoinMissingReferences(missing).c_str());
                return false;
            }

            const auto component = AddComponentByClass(gameObject, componentClass);
            if (!component) return false;
            const auto setting = EnsureReferenceField(component, componentClass, "setting",
                settingName.c_str());
            if (!setting) {
                // 预检过了还失败 = 真的建不出对象。已经挂上的组件必须撤掉，不能留半成品。
                Log::ErrorFmt("[ModAsset] %s 的 setting 建不出来，已撤掉刚挂的组件",
                    componentName.c_str());
                DestroyComponentImmediate(component);
                return false;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(setting);
            // 预检已经保证每个字段都在。这里再缺就是游戏侧状态与预检那一刻不一致 ——
            // 那种情况整体撤掉，不留半成品（见下方 `wired`）。
            bool wired = true;
            const auto slot = [&](const std::string& name) -> std::uintptr_t {
                const auto field = settingClass->Get<UnityResolve::Field>(name);
                if (!field) {
                    wired = false;
                    return 0;
                }
                return base + field->offset;
            };
            for (const auto& [name, value] : driver.ints) {
                if (const auto at = slot(name)) *reinterpret_cast<int*>(at) = value;
            }
            for (const auto& [name, value] : driver.floats) {
                if (const auto at = slot(name)) *reinterpret_cast<float*>(at) = value;
            }
            for (const auto& [name, value] : driver.vectors) {
                if (const auto at = slot(name)) {
                    for (int axis = 0; axis < 3; ++axis) {
                        *reinterpret_cast<float*>(at + static_cast<std::uintptr_t>(axis) * 4) = value[axis];
                    }
                }
            }
            for (const auto& [name, boneName] : driver.bones) {
                const auto field = settingClass->Get<UnityResolve::Field>(name);
                const auto target = resolveBone ? resolveBone(boneName) : nullptr;
                if (!field || !target) {
                    wired = false;
                    continue;
                }
                SetManagedReferenceField(setting, field->offset, target);
            }
            if (!wired) {
                Log::ErrorFmt("[ModAsset] %s 上的 %s 预检通过、写引用时又缺了，已撤掉刚挂的组件"
                    "（一个引用为空的驱动器会照样跑，比不挂更坏）",
                    gameObject->GetName().c_str(), componentName.c_str());
                DestroyComponentImmediate(component);
                return false;
            }
            Log::InfoFmt("[ModAsset] 驱动器 %s ← %s（int %zu float %zu vec %zu bone %zu）",
                gameObject->GetName().c_str(), componentName.c_str(),
                driver.ints.size(), driver.floats.size(), driver.vectors.size(), driver.bones.size());
            return true;
        }

        bool InitializeActorSwingDynamicBone(void* component, UnityResolve::Class* componentClass,
            const std::optional<LocalIpBoneSwing>& swing) {
            if (!component || !componentClass) return false;
            auto transform = reinterpret_cast<UnityResolve::UnityType::Component*>(component)->GetTransform();
            if (!transform) return false;

            if (const auto setDefaults = componentClass->Get<UnityResolve::Method>("SetDefaultValues")) {
                setDefaults->Invoke<void>(component);
            }
            // SetDefaultValues leaves a bone inert (mass=0, spring=0), which is right only
            // for a chain's anchor — the source authors those very values on e.g. Wing1_S,
            // but Wing2_S wants mass=0.5/spring=0.3 and never swung while stuck on defaults.
            if (swing) {
                const auto setFloat = [&](const char* name, float value) {
                    if (const auto f = componentClass->Get<UnityResolve::Field>(name))
                        *reinterpret_cast<float*>(
                            reinterpret_cast<std::uintptr_t>(component) + f->offset) = value;
                };
                setFloat("damping", swing->damping);
                setFloat("stiffness", swing->stiffness);
                setFloat("spring", swing->spring);
                setFloat("mass", swing->mass);
                if (swing->rootWeight >= 0.0f) setFloat("rootWeight", swing->rootWeight);
                if (swing->pendulum >= 0.0f) setFloat("pendulum", swing->pendulum);
                if (swing->pendulumRange >= 0.0f) setFloat("pendulumRange", swing->pendulumRange);
                if (swing->wind >= 0.0f) setFloat("wind", swing->wind);
                if (swing->dynamicType >= 0) {
                    if (const auto f = componentClass->Get<UnityResolve::Field>("dynamicType"))
                        *reinterpret_cast<int*>(
                            reinterpret_cast<std::uintptr_t>(component) + f->offset) = swing->dynamicType;
                }
                if (const auto f = componentClass->Get<UnityResolve::Field>("useWindGlobalForce"))
                    *reinterpret_cast<bool*>(
                        reinterpret_cast<std::uintptr_t>(component) + f->offset) = swing->useWindGlobalForce;

                // dynamicCollider / limitInfo 是引用字段，SetDefaultValues 不建它们 —— 以前
                // 因此改成在活体克隆上按**骨名**补写，那要靠一张全局 name→params 表，两个 mod
                // 用了同名骨就会互相覆盖参数（也可能把碰巧同名的原版骨当成 mod 骨改写）。
                // 现在自己 new 出来当场写完，那张全局表和整趟活体补写都不需要了。
                // 布局取自 il2cpp：ActorSwingCollider = type@16 collisionMask@20 vec3_A@24
                // vec3_B@36 float_A@48 float_B@52；LimitInfo = useLimit@16 axisX@20 axisY@28 axisZ@36。
                if (swing->colliderRadius >= 0.0f) {
                    if (const auto collider = EnsureReferenceField(
                        component, componentClass, "dynamicCollider", "ActorSwingDynamicCollider")) {
                        const auto at = reinterpret_cast<std::uintptr_t>(collider);
                        *reinterpret_cast<int*>(at + 16) = swing->colliderType;
                        *reinterpret_cast<int*>(at + 20) = swing->collisionMask;
                        *reinterpret_cast<float*>(at + 48) = swing->colliderRadius;
                        *reinterpret_cast<float*>(at + 52) = swing->colliderRadiusSub;
                    }
                    else {
                        Log::Warn("[ModAsset] ActorSwing dynamicCollider unavailable; 该骨没有碰撞体");
                    }
                }
                if (swing->useLimit >= 0) {
                    if (const auto limitInfo = EnsureReferenceField(
                        component, componentClass, "limitInfo", "LimitInfo")) {
                        const auto at = reinterpret_cast<std::uintptr_t>(limitInfo);
                        *reinterpret_cast<int*>(at + 16) = swing->useLimit;
                        for (int axis = 0; axis < 3; ++axis) {
                            *reinterpret_cast<int*>(at + 20 + axis * 8) = swing->limit[axis][0];
                            *reinterpret_cast<int*>(at + 24 + axis * 8) = swing->limit[axis][1];
                        }
                    }
                }
                // dynamicCollider 是引用字段，SetDefaultValues 已经建好实例（护士服字段 dump
                // 里非空），所以只填它的字段，不用自己 new。float_A=半径、float_B=次半径
                // (真实裙摆授权 float_A 0.024~0.03 / float_B 0.05)。
                // 碰撞体这里写不了：dynamicCollider 此刻还是 null（SetDefaultValues 不建它）。
            }

            const ActorSwingInitialTransform initial{
                transform->GetLocalPosition(), transform->GetLocalRotation(),
                transform->GetPosition(), transform->GetRotation() };
            if (const auto modelingTransform = componentClass->Get<UnityResolve::Field>("modelingTransform")) {
                *reinterpret_cast<ActorSwingInitialTransform*>(
                    reinterpret_cast<std::uintptr_t>(component) + modelingTransform->offset) = initial;
            }
            if (const auto updateInitial = componentClass->Get<UnityResolve::Method>("UpdateInitialTransform")) {
                updateInitial->Invoke<void>(component, initial);
            }
            if (const auto updateDepth = componentClass->Get<UnityResolve::Method>("UpdateHierarchyDepth")) {
                updateDepth->Invoke<void>(component);
            }
            return true;
        }

        // 给活体骨写 dynamicCollider。摇物骨的 dynamicCollider 与身体骨的 staticCollider 配对；
        // 身体骨(Head/Neck/Spine*/Hips/Left|RightArm/ForeArm/Leg)的碰撞体是游戏自带的，我们只
        // 需给自己的骨半径和分组。不写 → 默认 mask=-1/半径0.05 → 手臂直接穿过裙子。
        //
        // 必须在**活体克隆**上写：建骨时 dynamicCollider 还是 null。
        // 这个角色身上有没有 mod 建的摇物骨。没有就整套后续（建层、诊断日志）都别跑：
        // 未改装的角色走原版路径，逐层遍历它全部的链只会白白撑大日志。
        bool HasModBonesUnder(void* rootTransform) {
            const auto boneClass = FindClassByName("ActorSwingDynamicBone");
            const auto rootGameObject = rootTransform
                ? reinterpret_cast<UnityResolve::UnityType::Component*>(rootTransform)->GetGameObject()
                : nullptr;
            if (!boneClass || !rootGameObject || !IsNativeObjectAlive(rootGameObject)) return false;
            const auto modBoneNames = SnapshotCreatedActorSwingBoneNames();
            for (const auto bone : rootGameObject->GetComponentsInChildren<void*>(boneClass, true)) {
                if (bone && IsNativeObjectAlive(bone)
                    && modBoneNames.count(GetUnityObjectNameString(bone))) {
                    return true;
                }
            }
            return false;
        }

        // 整个 graft-时建骨方案的地基假设：ActorSwingDynamicBone 实现 IActorAnimationBone，
        // 所以游戏自己的 CampusActorAnimation.Initialize() 会把我们长在 prefab 上的骨
        // GetComponentsInChildren 收进 initializeData，两张并行表由它保证同长。
        // 这行就是量它 —— mod 骨没进 swingDynamicBones 的话，参数写得再全也不会有人模拟它。
        void LogSwingRegistrationCoverage(void* initializeData) {
            const auto initDataClass = FindClassByName("CampusActorAnimationInitializeData");
            if (!initDataClass || !initializeData) return;
            const auto dynamicBones = initDataClass->GetValue<UnityResolve::UnityType::List<void*>*>(
                initializeData, "swingDynamicBones");
            const auto initialTransforms = initDataClass->GetValue<UnityResolve::UnityType::List<void*>*>(
                initializeData, "initialTransforms");
            if (!dynamicBones) return;
            const auto modBoneNames = SnapshotCreatedActorSwingBoneNames();
            size_t mine = 0;
            std::string missing;
            std::unordered_set<std::string> present;
            if (dynamicBones->pList) {
                for (int i = 0; i < dynamicBones->size; ++i) {
                    const auto bone = dynamicBones->pList->At(static_cast<unsigned int>(i));
                    if (!bone) continue;
                    const auto name = GetUnityObjectNameString(bone);
                    if (modBoneNames.count(name)) { ++mine; present.insert(name); }
                }
            }
            for (const auto& name : modBoneNames) {
                if (!present.count(name)) missing += name + " ";
            }
            Log::InfoFmt("[ModAsset] ActorSwing registration coverage: swingDynamicBones=%d initialTransforms=%d modBonesRegistered=%zu/%zu missing=%s",
                dynamicBones->size, initialTransforms ? initialTransforms->size : -1,
                mine, modBoneNames.size(),
                missing.empty() ? "(none)" : missing.c_str());
        }

        // 把活体 mod 骨的字段原样读回来。建骨写的是 prefab，中间隔着 Instantiate 的深拷贝
        // 和游戏自己的 Initialize —— 参数有没有活到这一步，只有读回来才知道，别拿"我写过"
        // 当"它有"。hierarchyDepth 一并读：它是 0 的话这根骨在 job 里根本排不进链。
        void LogModSwingBoneFields(void* rootTransform) {
            const auto boneClass = FindClassByName("ActorSwingDynamicBone");
            const auto rootGameObject = rootTransform
                ? reinterpret_cast<UnityResolve::UnityType::Component*>(rootTransform)->GetGameObject()
                : nullptr;
            if (!boneClass || !rootGameObject || !IsNativeObjectAlive(rootGameObject)) return;
            const auto modBoneNames = SnapshotCreatedActorSwingBoneNames();
            const auto readFloat = [&](void* bone, const char* name) {
                const auto field = boneClass->Get<UnityResolve::Field>(name);
                return field ? *reinterpret_cast<float*>(
                    reinterpret_cast<std::uintptr_t>(bone) + field->offset) : -999.0f;
            };
            for (const auto bone : rootGameObject->GetComponentsInChildren<void*>(boneClass, true)) {
                if (!bone || !IsNativeObjectAlive(bone)) continue;
                const auto name = GetUnityObjectNameString(bone);
                if (!modBoneNames.count(name)) continue;
                int depth = -1, useLimit = -1, limits[6]{};
                if (const auto field = boneClass->Get<UnityResolve::Field>("<hierarchyDepth>k__BackingField")) {
                    depth = *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(bone) + field->offset);
                }
                if (const auto field = boneClass->Get<UnityResolve::Field>("limitInfo")) {
                    if (const auto limitInfo = *reinterpret_cast<void**>(
                        reinterpret_cast<std::uintptr_t>(bone) + field->offset)) {
                        const auto at = reinterpret_cast<std::uintptr_t>(limitInfo);
                        useLimit = *reinterpret_cast<int*>(at + 16);
                        for (int i = 0; i < 6; ++i) limits[i] = *reinterpret_cast<int*>(at + 20 + i * 4);
                    }
                }
                float colliderRadius = -1.0f;
                int collisionMask = 0;
                if (const auto field = boneClass->Get<UnityResolve::Field>("dynamicCollider")) {
                    if (const auto collider = *reinterpret_cast<void**>(
                        reinterpret_cast<std::uintptr_t>(bone) + field->offset)) {
                        const auto at = reinterpret_cast<std::uintptr_t>(collider);
                        collisionMask = *reinterpret_cast<int*>(at + 20);
                        colliderRadius = *reinterpret_cast<float*>(at + 48);
                    }
                }
                Log::InfoFmt("[ModAsset] ActorSwing live bone %s: depth=%d damping=%.3f stiffness=%.4f spring=%.3f pendulum=%.4f pendulumRange=%.3f mass=%.3f wind=%.3f rootWeight=%.3f useLimit=%d limit=[%d,%d][%d,%d][%d,%d] colliderRadius=%.4f collisionMask=%d",
                    name.c_str(), depth, readFloat(bone, "damping"), readFloat(bone, "stiffness"),
                    readFloat(bone, "spring"), readFloat(bone, "pendulum"),
                    readFloat(bone, "pendulumRange"), readFloat(bone, "mass"),
                    readFloat(bone, "wind"), readFloat(bone, "rootWeight"), useLimit,
                    limits[0], limits[1], limits[2], limits[3], limits[4], limits[5],
                    colliderRadius, collisionMask);
            }
        }

        // 逐层打印这个角色身上所有 ActorSwingChain 的层参数。游戏自己的裙摆链就在同一份
        // 输出里，天然是对照组：原版裙摆层实测 around=1、radius 0.015~0.04（bundle 里授权
        // 的序列化值），而我们的链是运行时新建的 —— UpdateChainInfo 会不会去设 around 这个
        // 授权字段，只有读回来才知道。around=0 的链没有环形碰撞，等于白建。
        // 给运行时新建的层补授权字段。
        //
        // UpdateChainInfo 只建层的**成员**（哪根骨在第几层），`active` 和 `radius` 是序列化
        // 授权值：游戏自己的链从 bundle 反序列化出来就带着，我们运行时建的层拿到的是
        // ChainLayerInfo() 的默认值（active=0、radius=0.05）—— active=0 基本等于这条链不被
        // 模拟，实测我们的层全是 0 而同一帧游戏自己的层是 1。
        //
        // 取值来自 1539 条原版链的实测（tools/scan_vanilla_swing_bones.py）：
        //   layer[0]  active=0 (1539/1539)  radius=0.05 (无一例外) —— 锚定层，设计上永不激活
        //   layer[1+] active=1 (约 89%)     radius 中位逐层递增
        // around 原版是混的（60% 关 / 40% 开），逐链手调、无规律可循，不动它。
        // ponytail: 按层序号取中位数，够用；真要逐部件类别调再走 sidecar。
        //
        // 2026-08-15 用 IDA 把 `UpdateChainInfo`(sub_1316A88) 读通之后，上面几段可以说得更死：
        // 它重建 ChainInfo 时，对**老 ChainInfo 里已存在的同序号层**做的是
        //     layer[16] = old[16]                 ← 只有 1 个字节，即 `active`
        //     *(u64*)(layer+20) = *(u64*)(old+20) ← 8 字节，即 `radius`(@20) + `smoothing`(@24)
        // 老层不存在时只写死 `*(u32*)(layer+20) = 0x3D4CCCCD`（float 0.05f）。
        // ChainLayerInfo 布局（il2cpp）：active@0x10 around@0x11 radius@0x14 smoothing@0x18。
        // 由此得到三条：
        //   1. 我们在 UpdateChainInfo **之后**写 active/radius 是对的，而且**重入安全** ——
        //      再调一次 UpdateChainInfo 会把它们当老值继承回来。
        //   2. `smoothing` 不用写：174 条原版链逐层实测中位数**全是 0.0000**，等于默认值。
        //   3. **`around`(@0x11) 不在拷贝范围内** —— 重跑一次 UpdateChainInfo 就会被打回默认
        //      (false)。原版 40% 的层开着它（环形碰撞），逐链手调、无规律，所以不猜默认值，
        //      改成由 sidecar 显式指定；没指定就保持 false（与现状一致，不改变任何已有成品）。
        size_t ConfigureModChainLayers(UnityResolve::UnityType::List<void*>* layers,
            int around = -1) {
            static constexpr float kRadiusByLayer[] = {
                0.05f, 0.010f, 0.015f, 0.025f, 0.030f, 0.033f, 0.030f, 0.050f };
            const auto layerClass = FindClassByName("ChainLayerInfo");
            if (!layers || !layers->pList || !layerClass) return 0;
            const auto activeField = layerClass->Get<UnityResolve::Field>("active");
            const auto radiusField = layerClass->Get<UnityResolve::Field>("radius");
            if (!activeField || !radiusField) return 0;

            size_t configured = 0;
            for (int i = 0; i < layers->size; ++i) {
                const auto layer = layers->pList->At(static_cast<unsigned int>(i));
                if (!layer) continue;
                const auto at = reinterpret_cast<std::uintptr_t>(layer);
                const auto radius = kRadiusByLayer[
                    std::min<size_t>(static_cast<size_t>(i), std::size(kRadiusByLayer) - 1)];
                *reinterpret_cast<bool*>(at + activeField->offset) = i > 0;
                *reinterpret_cast<float*>(at + radiusField->offset) = radius;
                if (around >= 0) {
                    if (const auto aroundField = layerClass->Get<UnityResolve::Field>("around"))
                        *reinterpret_cast<bool*>(at + aroundField->offset) = around != 0;
                }
                ++configured;
            }
            return configured;
        }

        // 给我们建的链建层。
        //
        // graft 时在 prefab 上 AddComponent 建的链只有 rootBones，`chains.layers` 是空的 ——
        // 实测日志里我们那两条 host=Hips 的链一层都没有，所以链完全没参与模拟（"改成裙摆档"
        // 和"飘带档"表现几乎一样就是这么来的）。原以为 Instantiate 触发的 OnEnable 会建层，
        // 那是错的：游戏自己的链在 bundle 里**就有序列化好的 layers**（active/around/radius
        // 都是授权值），本来就不需要运行时建，OnEnable 自然也不负责。
        //
        // 这跟笔记里禁止的"手搭 layer"是两回事：那是自己 new ChainLayerInfo 硬凑层数，会和
        // 游戏产出的层重复、同一根骨被模拟两次；这里是让游戏自己的 UpdateChainInfo 去建。
        // 只碰 rootBones 里含 mod 骨、且当前没有层的链，绝不动游戏自己的。
        size_t BuildLayersForModChains(void* rootTransform) {
            const auto chainClass = FindClassByName("ActorSwingChain");
            const auto chainInfoClass = FindClassByName("ChainInfo");
            const auto rootGameObject = rootTransform
                ? reinterpret_cast<UnityResolve::UnityType::Component*>(rootTransform)->GetGameObject()
                : nullptr;
            if (!chainClass || !chainInfoClass || !rootGameObject
                || !IsNativeObjectAlive(rootGameObject)) return 0;
            const auto updateChainInfo = chainClass->Get<UnityResolve::Method>("UpdateChainInfo");
            if (!updateChainInfo) return 0;
            // 先把名字集合抄一份：后台线程正在 graft 时这张表会被写，裸读是数据竞争；
            // 而下面每轮都要调 UpdateChainInfo（托管调用），不能一直攥着锁。
            const auto modBoneNames = SnapshotCreatedActorSwingBoneNames();
            const auto aroundByHost = SnapshotModChainAround();

            size_t built = 0;
            for (const auto chain : rootGameObject->GetComponentsInChildren<void*>(chainClass, true)) {
                if (!chain || !IsNativeObjectAlive(chain)) continue;
                const auto rootBones = chainClass->GetValue<UnityResolve::UnityType::List<void*>*>(
                    chain, "rootBones");
                if (!rootBones || !rootBones->pList) continue;
                bool mine = false;
                for (int i = 0; i < rootBones->size && !mine; ++i) {
                    mine = modBoneNames.count(
                        GetUnityObjectNameString(rootBones->pList->At(static_cast<unsigned int>(i)))) > 0;
                }
                if (!mine) continue;
                const auto chainInfo = chainClass->GetValue<void*>(chain, "chains");
                const auto layers = chainInfo
                    ? chainInfoClass->GetValue<UnityResolve::UnityType::List<void*>*>(chainInfo, "layers")
                    : nullptr;
                if (layers && layers->pList && layers->size > 0) continue;   // 已经有层，别重建
                updateChainInfo->Invoke<void>(chain);
                // UpdateChainInfo 可能整个换掉 ChainInfo 实例，必须重新读，别拿调用前的指针
                // （上一版就是这么把 layers 报成 0 的，其实建出来了）。
                const auto rebuilt = chainClass->GetValue<void*>(chain, "chains");
                const auto after = rebuilt
                    ? chainInfoClass->GetValue<UnityResolve::UnityType::List<void*>*>(rebuilt, "layers")
                    : nullptr;
                const auto hostName = GetUnityObjectNameString(
                    reinterpret_cast<UnityResolve::UnityType::Component*>(chain)->GetGameObject());
                const auto declared = aroundByHost.find(hostName);
                const auto configured = ConfigureModChainLayers(
                    after, declared == aroundByHost.end() ? -1 : declared->second);
                Log::InfoFmt("[ModAsset] ActorSwing UpdateChainInfo on mod chain: host=%s layers=%d configured=%zu",
                    GetUnityObjectNameString(
                        reinterpret_cast<UnityResolve::UnityType::Component*>(chain)->GetGameObject()).c_str(),
                    after ? after->size : -1, configured);
                ++built;
            }
            return built;
        }

        void LogSwingChainLayers(void* rootTransform) {
            const auto chainClass = FindClassByName("ActorSwingChain");
            const auto chainInfoClass = FindClassByName("ChainInfo");
            const auto layerClass = FindClassByName("ChainLayerInfo");
            const auto rootGameObject = rootTransform
                ? reinterpret_cast<UnityResolve::UnityType::Component*>(rootTransform)->GetGameObject()
                : nullptr;
            if (!chainClass || !chainInfoClass || !layerClass
                || !rootGameObject || !IsNativeObjectAlive(rootGameObject)) return;
            const auto modBoneNames = SnapshotCreatedActorSwingBoneNames();
            for (const auto chain : rootGameObject->GetComponentsInChildren<void*>(chainClass, true)) {
                if (!chain || !IsNativeObjectAlive(chain)) continue;
                const auto chainInfo = chainClass->GetValue<void*>(chain, "chains");
                const auto layers = chainInfo
                    ? chainInfoClass->GetValue<UnityResolve::UnityType::List<void*>*>(chainInfo, "layers")
                    : nullptr;
                if (!layers || !layers->pList) continue;
                const auto host = GetUnityObjectNameString(
                    reinterpret_cast<UnityResolve::UnityType::Component*>(chain)->GetGameObject());
                for (int i = 0; i < layers->size; ++i) {
                    const auto layer = layers->pList->At(static_cast<unsigned int>(i));
                    if (!layer) continue;
                    const auto bones = layerClass->GetValue<UnityResolve::UnityType::List<void*>*>(layer, "bones");
                    std::string first = "(none)";
                    bool mine = false;
                    if (bones && bones->pList && bones->size > 0) {
                        first = GetUnityObjectNameString(bones->pList->At(0));
                        for (int b = 0; b < bones->size && !mine; ++b) {
                            mine = modBoneNames.count(
                                GetUnityObjectNameString(bones->pList->At(static_cast<unsigned int>(b)))) > 0;
                        }
                    }
                    Log::InfoFmt("[ModAsset] ActorSwing chain layer host=%s mod=%d layer[%d] active=%d around=%d radius=%.4f smoothing=%.4f bones=%d first=%s",
                        host.c_str(), mine ? 1 : 0, i,
                        layerClass->GetValue<bool>(layer, "active") ? 1 : 0,
                        layerClass->GetValue<bool>(layer, "around") ? 1 : 0,
                        layerClass->GetValue<float>(layer, "radius"),
                        layerClass->GetValue<float>(layer, "smoothing"),
                        bones ? bones->size : -1, first.c_str());
                }
            }
        }

        void LogActorSwingChainStats(void* rootTransform, const char* label) {
            if (!rootTransform) return;
            const auto chainClass = FindClassByName("ActorSwingChain");
            const auto chainInfoClass = FindClassByName("ChainInfo");
            const auto layerClass = FindClassByName("ChainLayerInfo");
            if (!chainClass || !chainInfoClass || !layerClass) return;

            const auto rootGameObject = reinterpret_cast<UnityResolve::UnityType::Component*>(rootTransform)
                ->GetGameObject();
            if (!rootGameObject || !IsNativeObjectAlive(rootGameObject)) return;
            const auto chains = rootGameObject->GetComponentsInChildren<void*>(chainClass, true);
            for (const auto chain : chains) {
                if (!chain || !IsNativeObjectAlive(chain)) continue;
                const auto chainInfo = chainClass->GetValue<void*>(chain, "chains");
                const auto layers = chainInfo
                    ? chainInfoClass->GetValue<UnityResolve::UnityType::List<void*>*>(chainInfo, "layers")
                    : nullptr;
                int totalBones = 0;
                if (layers && layers->pList) {
                    for (int i = 0; i < layers->size; ++i) {
                        if (const auto layer = layers->pList->At(static_cast<unsigned int>(i))) {
                            if (const auto bones = layerClass->GetValue<UnityResolve::UnityType::List<void*>*>(layer, "bones"))
                                totalBones += bones->size;
                        }
                    }
                }
                const auto stat = layers && layers->pList
                    ? std::to_string(layers->size) + "layers/" + std::to_string(totalBones) + "bones"
                    : (chainInfo ? "noLayers" : "noChains");
                Log::InfoFmt("[ModAsset] %s ActorSwing chain layers: object=%s stats=%s",
                    label,
                    GetUnityObjectNameString(chain).c_str(),
                    stat.c_str());
            }
        }

        size_t AddActorSwingChainsToAnimationData(void* rootTransform, void* initializeData) {
            const auto chainClass = FindClassByName("ActorSwingChain");
            const auto initializeDataClass = FindClassByName("CampusActorAnimationInitializeData");
            if (!rootTransform || !initializeData || !chainClass || !initializeDataClass) return 0;
            const auto rootGameObject = reinterpret_cast<UnityResolve::UnityType::Component*>(rootTransform)->GetGameObject();
            const auto swingChains = initializeDataClass->GetValue<UnityResolve::UnityType::List<void*>*>(
                initializeData, "swingChains");
            if (!rootGameObject || !swingChains) return 0;

            size_t added = 0;
            for (const auto chain : rootGameObject->GetComponentsInChildren<void*>(chainClass, true)) {
                bool exists = false;
                if (swingChains->pList) {
                    for (int i = 0; i < swingChains->size; ++i) {
                        if (swingChains->pList->At(static_cast<unsigned int>(i)) == chain) {
                            exists = true;
                            break;
                        }
                    }
                }
                if (!exists && ListAddManaged(swingChains, chain)) ++added;
            }
            return added;
        }

        void RememberActiveAnimationRig(
            void* rig,
            void* rootTransform,
            void* initializeData) {
            const auto rootGameObject = rootTransform
                ? reinterpret_cast<UnityResolve::UnityType::Component*>(rootTransform)->GetGameObject()
                : nullptr;
            if (!rig || !rootTransform || !rootGameObject || !initializeData) return;

            std::lock_guard lock(g_animationRigMutex);
            const auto existing = std::find_if(
                g_activeAnimationRigs.begin(), g_activeAnimationRigs.end(),
                [rig, rootGameObject](const auto& context) {
                    return context.rig == rig || context.rootGameObject == rootGameObject;
                });
            const ActiveAnimationRigContext context{
                rig,
                initializeData,
                rootTransform,
                rootGameObject,
            };
            if (existing == g_activeAnimationRigs.end()) {
                g_activeAnimationRigs.push_back(context);
            }
            else {
                *existing = context;
            }
        }

        void CampusActorAnimationRig_RegisterBones_Hook(void* self, void* initializeData) {
            {
                // A part prefab was just instantiated onto a live actor, so every cached
                // "this actor has no bridge" may now be wrong.  Taken first: the calls
                // below reach for this same (non-recursive) mutex.
                std::lock_guard swingStateLock(g_swingStateMutex);
                g_sourceProxyLiveBridges.clear();
            }
            const auto initializeDataClass = FindClassByName("CampusActorAnimationInitializeData");
            auto rootTransform = initializeDataClass
                ? initializeDataClass->GetValue<UnityResolve::UnityType::Transform*>(initializeData, "root")
                : nullptr;
            if (!rootTransform && self) {
                rootTransform = reinterpret_cast<UnityResolve::UnityType::Component*>(self)->GetTransform();
            }
            if (!g_rigRegisterObserved.exchange(true)) {
                Log::InfoFmt("[ModAsset] CampusActorAnimationRig.RegisterBones observed: self=%p root=%p data=%p",
                    self, rootTransform, initializeData);
            }
            RememberActiveAnimationRig(self, rootTransform, initializeData);
            const auto nativeChainAttached = AttachNativeChainToLiveRoot(rootTransform);
            // 骨和链都已在 prefab 上就位，游戏自己收走了 —— 这里不再改 initializeData。
            //
            // 建链和补碰撞体是**互相独立**的两件事，别把后者的返回值当前者的开关：sidecar
            // 只要没有 collider/limit 字段（或那两个引用字段当时还没建好），链就会永远停在
            // 空层状态。各自判断自己该不该跑。
            // Unconditional: the coverage probe below only runs for bones this runtime CREATED, so
            // on the whole-object route — where the swing rig arrives inside the package — nothing
            // has ever measured whether the game picked those bones up at all.  Three rounds of
            // parameter work went by on that blind spot.  atbm-cstm-0140 registers 106 of its own;
            // a number near that means the game found ours too, a number near zero means it did not.
            if (const auto initDataClass = FindClassByName("CampusActorAnimationInitializeData")) {
                const auto bones = initDataClass->GetValue<UnityResolve::UnityType::List<void*>*>(
                    initializeData, "swingDynamicBones");
                const auto transforms = initDataClass->GetValue<UnityResolve::UnityType::List<void*>*>(
                    initializeData, "initialTransforms");
                // Only fields this class actually declares.  `swingChainLayers` lives on
                // `IActorAnimationRigData`, not here — reading it by name returned a garbage
                // pointer and dereferencing it crashed the game during loading.  `GetValue` by
                // name cannot fail loudly, so the field list has to be checked against the dump
                // before the call, not after the crash.
                Log::WarnFmt("[ModAsset] Swing lists the game collected: swingDynamicBones=%d initialTransforms=%d",
                    bones ? bones->size : -1, transforms ? transforms->size : -1);
            }
            const auto hasModBones = HasModBonesUnder(rootTransform);
            if (hasModBones) {
                // 必须在 orig 之前：RegisterBones 会把各链的 layers 收进 rigData._swingChainLayers，
                // 层是空的就等于这条链没注册进去。
                BuildLayersForModChains(rootTransform);
                LogSwingRegistrationCoverage(initializeData);
            }
            if (nativeChainAttached) {
                const auto chainsAdded = AddActorSwingChainsToAnimationData(rootTransform, initializeData);
                Log::InfoFmt("[ModAsset] Native ActorSwing chain registered before CampusActorAnimationRig.RegisterBones: added=%zu",
                    chainsAdded);
            }
            CampusActorAnimationRig_RegisterBones_Orig(self, initializeData);
            // 未改装的角色不打这些 —— 逐层遍历原版全部链，一个角色几十上百行，
            // 白白撑大日志还占加载期开销。
            if (hasModBones) {
                LogModSwingBoneFields(rootTransform);
                LogSwingChainLayers(rootTransform);
            }
            if (g_nativeChainValidation) {
                LogActorSwingChainStats(rootTransform, "native");
            }
            RetryPendingLiveReapplies("RegisterBones");
        }

        void LogAssetBundleLoadMethodsOnce() {
            static bool dumped = false;
            if (dumped) return;
            dumped = true;

            const auto assetBundleClass = Il2cppUtils::GetClass(
                "UnityEngine.AssetBundleModule.dll", "UnityEngine", "AssetBundle");
            if (!assetBundleClass) return;

            for (const auto method : assetBundleClass->methods) {
                if (!method) continue;
                const auto methodName = method->name;
                if (methodName.find("LoadFrom") == std::string::npos
                    && methodName.find("LoadAsset") == std::string::npos) {
                    continue;
                }

                std::string args;
                for (size_t i = 0; i < method->args.size(); ++i) {
                    if (i) args += ", ";
                    args += method->args[i] && method->args[i]->pType
                        ? method->args[i]->pType->name
                        : "<unknown>";
                }
                Log::InfoFmt("[ModAsset] AssetBundle method: %s(%s) return=%s fn=%p",
                    methodName.c_str(),
                    args.c_str(),
                    method->return_type ? method->return_type->name.c_str() : "<unknown>",
                    method->function);
            }
        }

        UnityResolve::Class* GetSystemByteClass() {
            static UnityResolve::Class* byteClass = nullptr;
            if (byteClass) return byteClass;

            const char* assemblies[] = {
                "mscorlib.dll",
                "System.Private.CoreLib.dll",
                "netstandard.dll",
            };
            for (const auto assemblyName : assemblies) {
                const auto assembly = UnityResolve::Get(assemblyName);
                if (!assembly) continue;
                byteClass = assembly->Get("Byte", "System");
                if (byteClass) return byteClass;
            }

            Log::Error("[ModAsset] Cannot resolve System.Byte class for AssetBundle.LoadFromMemory.");
            return nullptr;
        }

        UnityArray<std::uint8_t>* ReadFileToManagedByteArray(const std::filesystem::path& path) {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream.is_open()) {
                Log::ErrorFmt("[ModAsset] Cannot open mod asset bundle file: %s", path.string().c_str());
                return nullptr;
            }

            const auto size = stream.tellg();
            if (size <= 0) {
                Log::ErrorFmt("[ModAsset] Mod asset bundle file is empty: %s", path.string().c_str());
                return nullptr;
            }
            stream.seekg(0, std::ios::beg);

            std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
            if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
                Log::ErrorFmt("[ModAsset] Cannot read mod asset bundle file: %s", path.string().c_str());
                return nullptr;
            }

            const auto byteClass = GetSystemByteClass();
            if (!byteClass) return nullptr;

            const auto array = UnityArray<std::uint8_t>::New(byteClass, bytes.size());
            if (!array) {
                Log::ErrorFmt("[ModAsset] Cannot allocate managed byte array for bundle: %s size=%zu",
                    path.string().c_str(),
                    bytes.size());
                return nullptr;
            }
            array->Insert(bytes.data(), bytes.size());
            return array;
        }

        void* LoadAssetBundleFromMemoryFile(const std::filesystem::path& absolutePath) {
            const auto bytes = ReadFileToManagedByteArray(absolutePath);
            if (!bytes) return nullptr;

            using LoadFromMemoryInternalFn = void* (*)(UnityArray<std::uint8_t>*, uint32_t);
            static auto LoadFromMemoryInternal = reinterpret_cast<LoadFromMemoryInternalFn>(
                Il2cppUtils::il2cpp_resolve_icall(
                    "UnityEngine.AssetBundle::LoadFromMemory_Internal(System.Byte[],System.UInt32)"));
            if (LoadFromMemoryInternal) {
                if (auto bundle = LoadFromMemoryInternal(bytes, 0)) {
                    Log::InfoFmt("[ModAsset] Loaded mod asset bundle via LoadFromMemory_Internal icall: %s",
                        absolutePath.string().c_str());
                    return bundle;
                }
            }

            const auto assetBundleClass = Il2cppUtils::GetClass(
                "UnityEngine.AssetBundleModule.dll", "UnityEngine", "AssetBundle");
            static auto LoadFromMemoryInternalMethod = FindMethodByNameAndArgCount(
                assetBundleClass, "LoadFromMemory_Internal", 2);
            if (LoadFromMemoryInternalMethod) {
                if (auto bundle = LoadFromMemoryInternalMethod->Invoke<void*>(
                    bytes,
                    static_cast<uint32_t>(0))) {
                    Log::InfoFmt("[ModAsset] Loaded mod asset bundle via managed LoadFromMemory_Internal: %s",
                        absolutePath.string().c_str());
                    return bundle;
                }
            }

            LogAssetBundleLoadMethodsOnce();
            Log::ErrorFmt("[ModAsset] Cannot load mod asset bundle via LoadFromMemory_Internal: %s",
                absolutePath.string().c_str());
            return nullptr;
        }

        void* LoadAssetBundleFromFile(const std::string& path) {
            const auto absolutePath = std::filesystem::absolute(path).lexically_normal();
            const auto normalizedPath = absolutePath.string();
            const auto bundlePath = Il2cppString::New(normalizedPath);
            if (!bundlePath) return nullptr;

            static auto LoadFromFileAsync = Il2cppUtils::GetMethod(
                "UnityEngine.AssetBundleModule.dll",
                "UnityEngine",
                "AssetBundle",
                "LoadFromFileAsync");
            static auto AssetBundleCreateRequest_get_assetBundle = Il2cppUtils::GetMethod(
                "UnityEngine.AssetBundleModule.dll",
                "UnityEngine",
                "AssetBundleCreateRequest",
                "get_assetBundle");
            if (!LoadFromFileAsync || !AssetBundleCreateRequest_get_assetBundle) {
                Log::ErrorFmt("[ModAsset] Cannot resolve font-style AssetBundle loader methods: %s",
                    normalizedPath.c_str());
                return nullptr;
            }

            const auto request = LoadFromFileAsync->Invoke<void*>(bundlePath);
            if (!request) {
                Log::ErrorFmt("[ModAsset] AssetBundle.LoadFromFileAsync returned null: %s",
                    normalizedPath.c_str());
                return nullptr;
            }

            const auto bundle = AssetBundleCreateRequest_get_assetBundle->Invoke<void*>(request);
            if (!bundle) {
                Log::ErrorFmt("[ModAsset] AssetBundleCreateRequest.get_assetBundle returned null: %s",
                    normalizedPath.c_str());
                return nullptr;
            }

            Log::InfoFmt("[ModAsset] Loaded mod asset bundle via font-style LoadFromFileAsync: %s",
                normalizedPath.c_str());
            return bundle;
        }

        Il2CppGCHandle LoadLocalModAssetBundle(const std::filesystem::path& bundlePath) {
            const auto normalizedPath = bundlePath.lexically_normal().string();
            {
                std::lock_guard bundleLock(g_bundleMutex);
                if (const auto iter = g_bundleHandleMap.find(normalizedPath); iter != g_bundleHandleMap.end()) {
                    return iter->second;
                }
            }

            // 装载走的是托管调用，会重入我们自己的 LoadAsset hook —— 锁只圈这张表，
            // 绝不跨过装载本身。
            const auto assetBundle = LoadAssetBundleFromFile(normalizedPath);
            if (!assetBundle) {
                Log::ErrorFmt("[ModAsset] Failed to load mod asset bundle: %s", normalizedPath.c_str());
                return nullptr;
            }

            const auto bundleHandle = UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", assetBundle, false);
            if (!bundleHandle) {
                Log::ErrorFmt("[ModAsset] Failed to create mod asset bundle GCHandle: %s", normalizedPath.c_str());
                return nullptr;
            }

            {
                // 并发装同一个包时后到的那份让路：句柄放掉，用先登记的，别把表里那条覆盖成
                // 野句柄（两个句柄指向的是同一个 AssetBundle 对象）。
                std::lock_guard bundleLock(g_bundleMutex);
                const auto [iter, inserted] = g_bundleHandleMap.emplace(normalizedPath, bundleHandle);
                if (!inserted) {
                    UnityResolve::Invoke<void>("il2cpp_gchandle_free", bundleHandle);
                    return iter->second;
                }
            }
            Log::InfoFmt("[ModAsset] Loaded mod asset bundle: %s", normalizedPath.c_str());
            return bundleHandle;
        }

        void LoadLocalModManifest(const std::filesystem::path& manifestPath) {
            std::ifstream stream(manifestPath);
            if (!stream.is_open()) {
                Log::ErrorFmt("[ModAsset] Cannot open mod manifest: %s", manifestPath.string().c_str());
                return;
            }

            nlohmann::json manifest;
            try {
                stream >> manifest;
            }
            catch (const std::exception& e) {
                Log::ErrorFmt("[ModAsset] Cannot parse mod manifest %s: %s",
                    manifestPath.string().c_str(),
                    e.what());
                return;
            }

            if (!manifest.contains("replacements") || !manifest["replacements"].is_array()) {
                Log::ErrorFmt("[ModAsset] Manifest has no replacements array: %s", manifestPath.string().c_str());
                return;
            }

            // sidecar 的 runtimeProtocol 一直是硬校验，mod.json 的却从来没查过 —— 导出器写它、
            // 验证器把不等于 1 判成 error，只有运行时照单全收。写了就必须对得上；没写的是
            // 协议号之前的老包，放行（真正的骨架契约还有 sidecar 那道硬闸）。
            if (manifest.contains("runtimeProtocol")) {
                constexpr int kManifestRuntimeProtocol = 1;
                const auto& value = manifest["runtimeProtocol"];
                if (!value.is_number_integer() || value.get<int>() != kManifestRuntimeProtocol) {
                    Log::ErrorFmt("[ModAsset] Manifest runtimeProtocol mismatch, mod skipped: %s expected=%d",
                        manifestPath.string().c_str(), kManifestRuntimeProtocol);
                    return;
                }
            }

            const auto manifestDir = manifestPath.parent_path();
            const auto modId = GetJsonString(manifest, "id")
                .value_or(manifestPath.parent_path().filename().string());
            const auto modName = GetJsonString(manifest, "name")
                .or_else([&] { return GetJsonString(manifest, "id"); })
                .value_or(manifestPath.stem().string());
            const auto manifestPriority = GetJsonInt(manifest, "priority", 0);
            const auto sessionEnabled = !manifest.contains("enabled")
                || !manifest["enabled"].is_boolean()
                || manifest["enabled"].get<bool>();
            int loadedCount = 0;

            for (const auto& item : manifest["replacements"]) {
                if (!item.is_object()) continue;

                const auto sourceName = GetFirstJsonString(item, { "from", "source", "target" });
                const auto bundleName = GetFirstJsonString(item, { "bundle", "assetBundle", "assetbundle" });
                if (!sourceName || !bundleName) {
                    Log::ErrorFmt("[ModAsset] Invalid replacement in %s: from/source/target and bundle are required.",
                        manifestPath.string().c_str());
                    continue;
                }

                const auto assetName = GetFirstJsonString(item, { "asset", "to", "name" }).value_or(*sourceName);
                const auto skeletonAssetName = GetFirstJsonString(item, { "skeleton", "skeletonAsset" }).value_or("");
                const auto typeName = GetJsonString(item, "type").value_or("GameObject");
                const auto replaceWholeObject = item.contains("replaceWholeObject")
                    && item["replaceWholeObject"].is_boolean()
                    && item["replaceWholeObject"].get<bool>();
                const auto attachToOriginal = item.contains("attachToOriginal")
                    && item["attachToOriginal"].is_boolean()
                    && item["attachToOriginal"].get<bool>();
                const auto rendererName = GetJsonString(item, "rendererName").value_or("");
                auto part = GetJsonString(item, "part").value_or(InferPartFromAssetName(*sourceName));
                if (!IsValidPart(part)) {
                    Log::ErrorFmt("[ModAsset] Invalid replacement part in %s: source=%s part=%s expected=face/hair/body",
                        manifestPath.string().c_str(), sourceName->c_str(), part.c_str());
                    continue;
                }
                const auto priority = GetJsonInt(item, "priority", manifestPriority);
                const auto replaceMaterials = item.contains("replaceMaterials")
                    && item["replaceMaterials"].is_boolean()
                    && item["replaceMaterials"].get<bool>();

                std::vector<LocalModRendererRule> rendererRules{};
                if (item.contains("renderers") && item["renderers"].is_array()) {
                    for (const auto& rendererItem : item["renderers"]) {
                        if (!rendererItem.is_object()) continue;
                        const auto targetRenderer = GetJsonString(rendererItem, "targetRenderer");
                        const auto modRenderer = GetJsonString(rendererItem, "modRenderer");
                        if (!targetRenderer || !modRenderer) {
                            Log::ErrorFmt("[ModAsset] Invalid renderer rule in %s: targetRenderer and modRenderer are required.",
                                manifestPath.string().c_str());
                            continue;
                        }

                        rendererRules.emplace_back(LocalModRendererRule{
                            GetJsonString(rendererItem, "rendererId").value_or(""),
                            *targetRenderer,
                            *modRenderer,
                        });
                    }
                }

                std::vector<LocalModMaterialTextureReplacement> materialTextures{};
                std::vector<LocalModMaterialSlotCopy> materialCopies{};
                if (item.contains("materialCopies") && item["materialCopies"].is_array()) {
                    for (const auto& copyItem : item["materialCopies"]) {
                        if (!copyItem.is_object()) continue;

                        materialCopies.emplace_back(LocalModMaterialSlotCopy{
                            GetJsonString(copyItem, "rendererName").value_or(rendererName),
                            GetJsonInt(copyItem, "fromSlot", GetJsonInt(copyItem, "sourceSlot", -1)),
                            GetJsonInt(copyItem, "toSlot", GetJsonInt(copyItem, "targetSlot", -1)),
                        });
                    }
                }

                std::vector<LocalModMaterialColorReplacement> materialColors{};
                if (item.contains("materialColors") && item["materialColors"].is_array()) {
                    for (const auto& colorItem : item["materialColors"]) {
                        if (!colorItem.is_object()) continue;

                        const auto propertyName = GetFirstJsonString(colorItem, { "property", "shaderProperty", "name" });
                        if (!propertyName) {
                            Log::ErrorFmt("[ModAsset] Invalid material color replacement in %s: property is required.",
                                manifestPath.string().c_str());
                            continue;
                        }

                        float r = GetJsonFloat(colorItem, "r", 1.0f);
                        float g = GetJsonFloat(colorItem, "g", 1.0f);
                        float b = GetJsonFloat(colorItem, "b", 1.0f);
                        float a = GetJsonFloat(colorItem, "a", 1.0f);
                        if (colorItem.contains("value") && colorItem["value"].is_array()) {
                            const auto& value = colorItem["value"];
                            if (value.size() > 0 && value[0].is_number()) r = value[0].get<float>();
                            if (value.size() > 1 && value[1].is_number()) g = value[1].get<float>();
                            if (value.size() > 2 && value[2].is_number()) b = value[2].get<float>();
                            if (value.size() > 3 && value[3].is_number()) a = value[3].get<float>();
                        }

                        materialColors.emplace_back(LocalModMaterialColorReplacement{
                            GetJsonString(colorItem, "rendererName").value_or(rendererName),
                            GetJsonInt(colorItem, "materialSlot", -1),
                            *propertyName,
                            r,
                            g,
                            b,
                            a,
                        });
                    }
                }

                std::vector<LocalModMaterialFloatReplacement> materialFloats{};
                if (item.contains("materialFloats") && item["materialFloats"].is_array()) {
                    for (const auto& floatItem : item["materialFloats"]) {
                        if (!floatItem.is_object()) continue;

                        const auto propertyName = GetFirstJsonString(floatItem, { "property", "shaderProperty", "name" });
                        if (!propertyName) {
                            Log::ErrorFmt("[ModAsset] Invalid material float replacement in %s: property is required.",
                                manifestPath.string().c_str());
                            continue;
                        }

                        materialFloats.emplace_back(LocalModMaterialFloatReplacement{
                            GetJsonString(floatItem, "rendererName").value_or(rendererName),
                            GetJsonInt(floatItem, "materialSlot", -1),
                            *propertyName,
                            GetJsonFloat(floatItem, "value", 0.0f),
                        });
                    }
                }

                std::vector<LocalModTransparentMaterial> transparentMaterials{};
                if (item.contains("transparentMaterials") && item["transparentMaterials"].is_array()) {
                    for (const auto& transparentItem : item["transparentMaterials"]) {
                        if (!transparentItem.is_object()) continue;

                        std::vector<std::pair<std::string, float>> extraFloats{};
                        if (transparentItem.contains("props") && transparentItem["props"].is_object()) {
                            for (const auto& [key, value] : transparentItem["props"].items()) {
                                if (value.is_number()) extraFloats.emplace_back(key, value.get<float>());
                            }
                        }
                        const auto textureAssetName = GetFirstJsonString(transparentItem, { "asset", "texture", "baseMap" });
                        const auto slot = GetJsonInt(transparentItem, "materialSlot", -1);
                        if (!textureAssetName || slot < 0) {
                            Log::ErrorFmt("[ModAsset] Invalid transparent material in %s: materialSlot(>=0) and asset are required.",
                                manifestPath.string().c_str());
                            continue;
                        }

                        transparentMaterials.emplace_back(LocalModTransparentMaterial{
                            GetJsonString(transparentItem, "rendererName").value_or(rendererName),
                            slot,
                            *textureAssetName,
                            GetFirstJsonString(transparentItem, { "defMap", "packedMask" }).value_or(""),
                            GetFirstJsonString(transparentItem, { "shadeMap", "shadeColor" }).value_or(""),
                            GetJsonString(transparentItem, "type").value_or("Texture2D"),
                            GetJsonFloat(transparentItem, "alpha", 1.0f),
                            GetJsonFloat(transparentItem, "alphaFromTexture", 1.0f),
                            GetJsonFloat(transparentItem, "cull", 0.0f),
                            GetJsonFloat(transparentItem, "zwrite", 0.0f),
                            GetJsonFloat(transparentItem, "cutoff", 0.004f),
                            GetJsonFloat(transparentItem, "toonStrength", 1.0f),
                            GetJsonFloat(transparentItem, "shadeDarken", 0.45f),
                            GetJsonFloat(transparentItem, "toonSoftness", 0.08f),
                            GetJsonFloat(transparentItem, "aoStrength", 0.5f),
                            GetJsonInt(transparentItem, "renderQueue", -1),
                            std::move(extraFloats),
                            transparentItem.value("vanillaMaterial", false),
                            transparentItem.value("gbufferQueue", false),
                        });
                    }
                }

                if (item.contains("textures") && item["textures"].is_array()) {
                    for (const auto& textureItem : item["textures"]) {
                        if (!textureItem.is_object()) continue;

                        const auto propertyName = GetFirstJsonString(textureItem, { "property", "shaderProperty", "name" });
                        const auto textureAssetName = GetFirstJsonString(textureItem, { "asset", "texture", "to" });
                        if (!propertyName || !textureAssetName) {
                            Log::ErrorFmt("[ModAsset] Invalid texture replacement in %s: property and asset are required.",
                                manifestPath.string().c_str());
                            continue;
                        }

                        materialTextures.emplace_back(LocalModMaterialTextureReplacement{
                            GetJsonString(textureItem, "rendererName").value_or(rendererName),
                            GetJsonInt(textureItem, "materialSlot", -1),
                            *propertyName,
                            *textureAssetName,
                            GetJsonString(textureItem, "type").value_or("Texture2D"),
                        });
                    }
                }

                const auto bundlePath = (manifestDir / *bundleName).lexically_normal();
                if (!std::filesystem::is_regular_file(bundlePath)) {
                    Log::ErrorFmt("[ModAsset] Mod bundle not found: %s", bundlePath.string().c_str());
                    continue;
                }

                auto replacement = std::make_shared<LocalModAssetReplacement>(
                    LocalModAssetReplacement{
                    modId,
                    modName,
                    manifestPath.string(),
                    *sourceName,
                    part,
                    assetName,
                    skeletonAssetName,
                    bundlePath.string(),
                    typeName,
                    replaceWholeObject,
                    attachToOriginal,
                    rendererName,
                    priority,
                    sessionEnabled,
                    replaceMaterials,
                    0,
                    std::move(rendererRules),
                    std::move(materialCopies),
                    std::move(materialTextures),
                    std::move(materialColors),
                    std::move(materialFloats),
                    std::move(transparentMaterials),
                });
                g_registeredReplacements.emplace_back(replacement);
                ++loadedCount;
                Log::InfoFmt("[ModAsset] Registered replacement candidate: %s -> %s (%s) modId=%s mod=%s enabled=%d part=%s priority=%d wholeObject=%d skeleton=%s rendererRules=%zu materialCopies=%zu textures=%zu colors=%zu floats=%zu",
                    sourceName->c_str(),
                    assetName.c_str(),
                    bundlePath.string().c_str(),
                    modId.c_str(),
                    modName.c_str(),
                    replacement->sessionEnabled ? 1 : 0,
                    replacement->part.c_str(),
                    replacement->priority,
                    replacement->replaceWholeObject ? 1 : 0,
                    replacement->skeletonAssetName.c_str(),
                    replacement->rendererRules.size(),
                    replacement->materialCopies.size(),
                    replacement->materialTextures.size(),
                    replacement->materialColors.size(),
                    replacement->materialFloats.size());
                if (!replacement->transparentMaterials.empty()) {
                    std::string slots;
                    for (const auto& transparent : replacement->transparentMaterials) {
                        slots += (slots.empty() ? "" : ", ") + std::to_string(transparent.materialSlot)
                            + ":" + transparent.assetName
                            + " alpha=" + std::to_string(transparent.alpha);
                    }
                    Log::InfoFmt("[ModAsset] Transparent materials declared: %s count=%zu [%s]",
                        sourceName->c_str(), replacement->transparentMaterials.size(), slots.c_str());
                }
            }

            Log::InfoFmt("[ModAsset] Manifest loaded: %s, replacements=%d", modName.c_str(), loadedCount);
        }

        void RebuildActiveReplacementMapLocked(const bool logConflicts) {
            g_replacementMap.clear();
            for (const auto& replacement : g_registeredReplacements) {
                if (!replacement || !replacement->sessionEnabled) continue;
                const auto replacementKey = NormalizeAssetName(replacement->sourceName);
                const auto existing = g_replacementMap.find(replacementKey);
                if (existing != g_replacementMap.end()) {
                    if (replacement->priority < existing->second->priority) {
                        if (logConflicts) {
                            Log::WarnFmt("[ModAsset] Active replacement conflict skipped by priority: source=%s newMod=%s newPriority=%d existingMod=%s existingPriority=%d",
                                replacement->sourceName.c_str(),
                                replacement->modName.c_str(),
                                replacement->priority,
                                existing->second->modName.c_str(),
                                existing->second->priority);
                        }
                        continue;
                    }
                    if (logConflicts) {
                        Log::WarnFmt("[ModAsset] Active replacement conflict overridden: source=%s newMod=%s newPriority=%d existingMod=%s existingPriority=%d",
                            replacement->sourceName.c_str(),
                            replacement->modName.c_str(),
                            replacement->priority,
                            existing->second->modName.c_str(),
                            existing->second->priority);
                    }
                }
                g_replacementMap[replacementKey] = replacement;
            }
        }

        void LoadLocalModManifests() {
            const auto modRoot = Paths::Mods();
            std::unique_lock replacementLock(g_replacementMutex);
            g_replacementMap.clear();
            g_registeredReplacements.clear();

            if (!std::filesystem::exists(modRoot)) {
                Log::InfoFmt("[ModAsset] Mod directory not found, skipped: %s", modRoot.string().c_str());
                return;
            }

            std::vector<std::filesystem::path> manifestPaths{};
            for (const auto& entry : std::filesystem::directory_iterator(modRoot)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    manifestPaths.emplace_back(entry.path());
                    continue;
                }

                if (!entry.is_directory()) continue;
                const auto manifestPath = entry.path() / "mod.json";
                if (std::filesystem::is_regular_file(manifestPath)) {
                    manifestPaths.emplace_back(manifestPath);
                }
            }
            std::sort(manifestPaths.begin(), manifestPaths.end());

            for (const auto& manifestPath : manifestPaths) {
                LoadLocalModManifest(manifestPath);
            }

            RebuildActiveReplacementMapLocked(true);
            Log::InfoFmt("[ModAsset] Registered mod asset replacement candidates: %zu active=%zu",
                g_registeredReplacements.size(), g_replacementMap.size());
        }

        LocalModAssetReplacementPtr FindLocalModAssetReplacement(const std::string& sourceName) {
            std::shared_lock replacementLock(g_replacementMutex);
            const auto iter = g_replacementMap.find(NormalizeAssetName(sourceName));
            return iter == g_replacementMap.end() ? nullptr : iter->second;
        }

        UnityResolve::Class* GetLocalModUnityClass(const std::string& typeName) {
            const auto typeKey = ToLowerAscii(typeName);
            if (typeKey == "gameobject" || typeKey == "unityengine.gameobject") {
                return Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
            }
            if (typeKey == "mesh" || typeKey == "unityengine.mesh") {
                return Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Mesh");
            }
            if (typeKey == "material" || typeKey == "unityengine.material") {
                return Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Material");
            }
            if (typeKey == "texture2d" || typeKey == "unityengine.texture2d") {
                return Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Texture2D");
            }
            if (typeKey == "textasset" || typeKey == "unityengine.textasset") {
                return Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "TextAsset");
            }
            if (typeKey == "sprite" || typeKey == "unityengine.sprite") {
                return Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Sprite");
            }
            if (typeKey == "shader" || typeKey == "unityengine.shader") {
                return Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Shader");
            }

            Log::ErrorFmt("[ModAsset] Unsupported replacement type \"%s\", fallback to GameObject.", typeName.c_str());
            return Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
        }

        Il2cppUtils::Il2CppReflectionType* GetLocalModReflectionType(const std::string& typeName) {
            const auto klass = GetLocalModUnityClass(typeName);
            if (!klass) return nullptr;

            const auto il2cppType = UnityResolve::Invoke<void*>("il2cpp_class_get_type", klass->address);
            return il2cppType
                ? UnityResolve::Invoke<Il2cppUtils::Il2CppReflectionType*>("il2cpp_type_get_object", il2cppType)
                : nullptr;
        }

        void* LoadLocalModAssetFromBundle(const Il2CppGCHandle bundleHandle,
            const std::string& bundlePath,
            const std::string& assetName,
            const std::string& typeName) {
            const auto cacheKey = NormalizeAssetName(bundlePath + "|" + assetName + "|" + typeName);
            {
                std::lock_guard bundleLock(g_bundleMutex);
                if (const auto iter = g_loadedAssetHandleMap.find(cacheKey); iter != g_loadedAssetHandleMap.end()) {
                    auto cachedAsset = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", iter->second);
                    if (cachedAsset && IsNativeObjectAlive(cachedAsset)) {
                        return cachedAsset;
                    }
                    UnityResolve::Invoke<void>("il2cpp_gchandle_free", std::exchange(iter->second, nullptr));
                    g_loadedAssetHandleMap.erase(iter);
                }
            }

            const auto assetBundle = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", bundleHandle);
            if (!assetBundle) {
                Log::ErrorFmt("[ModAsset] Mod bundle target is null: %s", bundlePath.c_str());
                return nullptr;
            }

            const auto reflectionType = GetLocalModReflectionType(typeName);
            if (!reflectionType) {
                Log::ErrorFmt("[ModAsset] Cannot resolve mod asset type: %s", typeName.c_str());
                return nullptr;
            }

            static auto AssetBundle_LoadAsset = Il2cppUtils::GetMethod(
                "UnityEngine.AssetBundleModule.dll",
                "UnityEngine",
                "AssetBundle",
                "LoadAsset_Internal",
                { "System.String", "System.Type" });
            if (!AssetBundle_LoadAsset) {
                Log::Error("[ModAsset] Cannot resolve AssetBundle.LoadAsset_Internal managed method for replacement.");
                return nullptr;
            }

            auto modAsset = AssetBundle_LoadAsset->Invoke<void*>(
                assetBundle,
                Il2cppString::New(assetName),
                reflectionType);
            if (!modAsset) {
                Log::ErrorFmt("[ModAsset] Failed to load mod asset: %s type=%s bundle=%s",
                    assetName.c_str(),
                    typeName.c_str(),
                    bundlePath.c_str());
                return nullptr;
            }

            {
                std::lock_guard bundleLock(g_bundleMutex);
                auto& slot = g_loadedAssetHandleMap[cacheKey];
                if (slot) UnityResolve::Invoke<void>("il2cpp_gchandle_free", slot);
                slot = UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", modAsset, false);
            }
            Log::InfoFmt("[ModAsset] Loaded mod asset: %s type=%s result=%p resultType=%s",
                assetName.c_str(),
                typeName.c_str(),
                modAsset,
                GetUnityObjectClassName(modAsset));
            return modAsset;
        }

        void* LoadLocalModReplacementAsset(LocalModAssetReplacement& replacement) {
            if (!replacement.bundleHandle) {
                replacement.bundleHandle = LoadLocalModAssetBundle(replacement.bundlePath);
            }
            if (!replacement.bundleHandle) {
                Log::ErrorFmt("[ModAsset] Replacement bundle unavailable, keeping original asset: %s bundle=%s",
                    replacement.sourceName.c_str(),
                    replacement.bundlePath.c_str());
                return nullptr;
            }

            return LoadLocalModAssetFromBundle(
                replacement.bundleHandle,
                replacement.bundlePath,
                replacement.assetName,
                replacement.typeName);
        }

        std::string GetTextAssetText(void* asset) {
            static auto TextAsset_get_text = reinterpret_cast<Il2cppString* (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "TextAsset", "get_text"));
            const auto text = asset && TextAsset_get_text ? TextAsset_get_text(asset) : nullptr;
            return text ? text->ToString() : std::string{};
        }

        void* CloneUnityObject(void* obj, const std::string& sourceName, const size_t rendererIndex) {
            if (!obj) return nullptr;

            using CloneFn = void* (*)(void*);
            static auto Object_InternalCloneSingle = reinterpret_cast<CloneFn>(
                Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Object::Internal_CloneSingle(UnityEngine.Object)"));

            auto clone = Object_InternalCloneSingle ? Object_InternalCloneSingle(obj) : nullptr;
            if (!clone) {
                static auto Object_Instantiate = reinterpret_cast<CloneFn>(
                    Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Object",
                        "Instantiate", { "UnityEngine.Object" }));
                clone = Object_Instantiate ? Object_Instantiate(obj) : nullptr;
            }
            if (!clone) {
                Log::ErrorFmt("[ModAsset] Failed to clone mod mesh: %s renderer=%zu sourceMesh=%p",
                    sourceName.c_str(),
                    rendererIndex,
                    obj);
                return nullptr;
            }

            {
                std::lock_guard lock(g_runtimeMeshHandleMutex);
                g_runtimeMeshHandles[clone] =
                    UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", clone, false);
            }
            Log::InfoFmt("[ModAsset] Cloned mod mesh before patch: %s renderer=%zu sourceMesh=%p clonedMesh=%p",
                sourceName.c_str(),
                rendererIndex,
                obj,
                clone);
            return clone;
        }

        bool IsRuntimeOwnedMesh(void* mesh) {
            if (!mesh) return false;
            std::lock_guard lock(g_runtimeMeshHandleMutex);
            return g_runtimeMeshHandles.contains(mesh);
        }

        UnityArray<void*>* GetSkinnedMeshRendererBones(void* renderer) {
            static auto SkinnedMeshRenderer_get_bones = reinterpret_cast<UnityArray<void*>* (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer", "get_bones"));
            return renderer && SkinnedMeshRenderer_get_bones ? SkinnedMeshRenderer_get_bones(renderer) : nullptr;
        }

        void* GetSkinnedMeshRendererSharedMesh(void* renderer) {
            static auto SkinnedMeshRenderer_get_sharedMesh = reinterpret_cast<void* (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer", "get_sharedMesh"));
            return renderer && SkinnedMeshRenderer_get_sharedMesh ? SkinnedMeshRenderer_get_sharedMesh(renderer) : nullptr;
        }

        bool SetSkinnedMeshRendererSharedMesh(void* renderer, void* mesh) {
            static auto SkinnedMeshRenderer_set_sharedMesh = reinterpret_cast<void (*)(void*, void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer", "set_sharedMesh"));
            if (!renderer || !SkinnedMeshRenderer_set_sharedMesh) return false;
            SkinnedMeshRenderer_set_sharedMesh(renderer, mesh);
            return true;
        }

        void SetSkinnedMeshRendererBones(void* renderer, UnityArray<void*>* bones) {
            static auto SkinnedMeshRenderer_set_bones = reinterpret_cast<void (*)(void*, UnityArray<void*>*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer", "set_bones"));
            if (renderer && bones && SkinnedMeshRenderer_set_bones) SkinnedMeshRenderer_set_bones(renderer, bones);
        }

        void* GetSkinnedMeshRendererRootBone(void* renderer) {
            static auto SkinnedMeshRenderer_get_rootBone = reinterpret_cast<void* (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer", "get_rootBone"));
            return renderer && SkinnedMeshRenderer_get_rootBone ? SkinnedMeshRenderer_get_rootBone(renderer) : nullptr;
        }

        bool SetSkinnedMeshRendererRootBone(void* renderer, void* rootBone) {
            static auto SkinnedMeshRenderer_set_rootBone = reinterpret_cast<void (*)(void*, void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer", "set_rootBone"));
            if (!renderer || !SkinnedMeshRenderer_set_rootBone) return false;
            SkinnedMeshRenderer_set_rootBone(renderer, rootBone);
            return true;
        }

        void RefreshSkinnedMeshRendererState(void* renderer) {
            if (!renderer || !IsNativeObjectAlive(renderer)) return;

            static auto getEnabled = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer", "get_enabled");
            static auto setEnabled = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer", "set_enabled",
                { "System.Boolean" });
            // 这两个在**这版游戏里被裁掉了**（2026-08-18 实机日志坐实）。下面每处调用都判空、
            // 拿不到就跳过刷新包围盒 —— 所以按 optional 查，别每次启动刷两条 ERROR。
            static auto resetBounds = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer", "ResetBounds",
                {}, /*optional=*/true);
            static auto resetLocalBounds = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer", "ResetLocalBounds",
                {}, /*optional=*/true);

            bool wasEnabled = true;
            if (getEnabled && getEnabled->function) {
                using GetEnabledFn = bool (*)(void*, void*);
                wasEnabled = reinterpret_cast<GetEnabledFn>(getEnabled->function)(
                    renderer, getEnabled->address);
            }
            if (wasEnabled && setEnabled && setEnabled->function) {
                using SetEnabledFn = void (*)(void*, bool, void*);
                reinterpret_cast<SetEnabledFn>(setEnabled->function)(
                    renderer, false, setEnabled->address);
            }
            if (resetBounds && resetBounds->function) {
                using ResetFn = void (*)(void*, void*);
                reinterpret_cast<ResetFn>(resetBounds->function)(
                    renderer, resetBounds->address);
            }
            if (resetLocalBounds && resetLocalBounds->function) {
                using ResetFn = void (*)(void*, void*);
                reinterpret_cast<ResetFn>(resetLocalBounds->function)(
                    renderer, resetLocalBounds->address);
            }
            if (wasEnabled && setEnabled && setEnabled->function) {
                using SetEnabledFn = void (*)(void*, bool, void*);
                reinterpret_cast<SetEnabledFn>(setEnabled->function)(
                    renderer, true, setEnabled->address);
            }
            Log::InfoFmt(
                "[ModAsset] Refreshed active SkinnedMeshRenderer state: renderer=%s wasEnabled=%d resetBounds=%d resetLocalBounds=%d",
                GetUnityObjectNameString(renderer).c_str(),
                wasEnabled ? 1 : 0,
                resetBounds && resetBounds->function ? 1 : 0,
                resetLocalBounds && resetLocalBounds->function ? 1 : 0);
        }

        int GetMeshVertexCount(void* mesh) {
            static auto Mesh_get_vertexCount = reinterpret_cast<int (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "get_vertexCount"));
            return mesh && Mesh_get_vertexCount ? Mesh_get_vertexCount(mesh) : -1;
        }

        int GetMeshIntProperty(void* mesh, const char* propertyName) {
            static std::unordered_map<std::string, int (*)(void*)> accessors;
            if (!mesh) return -1;
            auto& fn = accessors[propertyName];
            if (!fn) {
                fn = reinterpret_cast<int (*)(void*)>(
                    Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", propertyName));
            }
            return fn ? fn(mesh) : -1;
        }

        UnityArray<UnityResolve::UnityType::Vector3>* GetMeshVertices(void* mesh) {
            static auto Mesh_get_vertices = reinterpret_cast<UnityArray<UnityResolve::UnityType::Vector3>* (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "get_vertices"));
            return mesh && Mesh_get_vertices ? Mesh_get_vertices(mesh) : nullptr;
        }

        void SetMeshVertices(void* mesh, UnityArray<UnityResolve::UnityType::Vector3>* vertices) {
            static auto Mesh_set_vertices = reinterpret_cast<void (*)(void*, UnityArray<UnityResolve::UnityType::Vector3>*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "set_vertices"));
            if (mesh && vertices && Mesh_set_vertices) Mesh_set_vertices(mesh, vertices);
        }

        void RecalculateMeshBounds(void* mesh) {
            static auto Mesh_RecalculateBounds = reinterpret_cast<void (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "RecalculateBounds"));
            if (mesh && Mesh_RecalculateBounds) Mesh_RecalculateBounds(mesh);
        }

        UnityResolve::UnityType::Transform* GetComponentTransform(void* component) {
            static auto Component_get_transform = reinterpret_cast<UnityResolve::UnityType::Transform * (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Component", "get_transform"));
            return component && Component_get_transform ? Component_get_transform(component) : nullptr;
        }

        UnityResolve::UnityType::Transform* GetHierarchyRoot(void* component) {
            auto current = GetComponentTransform(component);
            if (!current) return nullptr;
            while (const auto parent = current->GetParent()) {
                if (!IsNativeObjectAlive(parent)) break;
                current = parent;
            }
            return current;
        }

        void* GetHierarchyRootGameObject(void* component) {
            const auto root = GetHierarchyRoot(component);
            return root ? root->GetGameObject() : nullptr;
        }

        int GetComponentDepthFromRoot(void* component, void* rootGameObject) {
            if (!component || !rootGameObject) return 0;
            auto current = GetComponentTransform(component);
            const auto root = reinterpret_cast<UnityResolve::UnityType::GameObject*>(
                rootGameObject)->GetTransform();
            if (!current || !root) return 0;

            int depth = 0;
            while (current && current != root) {
                current = current->GetParent();
                ++depth;
            }
            return current == root ? depth : 0;
        }

        void* GetSourceRootGameObject(void* component, const int sourceRootDepth) {
            auto current = GetComponentTransform(component);
            if (!current) return nullptr;
            for (int index = 0; index < sourceRootDepth; ++index) {
                const auto parent = current->GetParent();
                if (!parent || !IsNativeObjectAlive(parent)) return nullptr;
                current = parent;
            }
            return current->GetGameObject();
        }

        UnityResolve::UnityType::Vector3 InverseTransformPoint(void* transform, const UnityResolve::UnityType::Vector3& position) {
            static auto method = UnityResolve::Get("UnityEngine.CoreModule.dll")
                ->Get("Transform")
                ->Get<UnityResolve::Method>("InverseTransformPoint");
            return transform && method ? method->Invoke<UnityResolve::UnityType::Vector3>(transform, position) : UnityResolve::UnityType::Vector3{};
        }

        UnityResolve::UnityType::Vector3 TransformPoint(void* transform, const UnityResolve::UnityType::Vector3& position) {
            static auto method = UnityResolve::Get("UnityEngine.CoreModule.dll")
                ->Get("Transform")
                ->Get<UnityResolve::Method>("TransformPoint");
            return transform && method ? method->Invoke<UnityResolve::UnityType::Vector3>(transform, position) : UnityResolve::UnityType::Vector3{};
        }

        UnityResolve::UnityType::Vector3 InverseTransformDirection(
            void* transform, const UnityResolve::UnityType::Vector3& direction) {
            static auto method = UnityResolve::Get("UnityEngine.CoreModule.dll")
                ->Get("Transform")
                ->Get<UnityResolve::Method>("InverseTransformDirection");
            return transform && method
                ? method->Invoke<UnityResolve::UnityType::Vector3>(transform, direction)
                : UnityResolve::UnityType::Vector3{};
        }

        UnityResolve::UnityType::Vector3 TransformDirection(
            void* transform, const UnityResolve::UnityType::Vector3& direction) {
            static auto method = UnityResolve::Get("UnityEngine.CoreModule.dll")
                ->Get("Transform")
                ->Get<UnityResolve::Method>("TransformDirection");
            return transform && method
                ? method->Invoke<UnityResolve::UnityType::Vector3>(transform, direction)
                : UnityResolve::UnityType::Vector3{};
        }

        UnityArray<UnityResolve::UnityType::Vector3>* GetMeshNormals(void* mesh) {
            static auto fn = reinterpret_cast<UnityArray<UnityResolve::UnityType::Vector3>* (*)(void*)>(
                Il2cppUtils::GetMethodPointer(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "get_normals"));
            return mesh && fn ? fn(mesh) : nullptr;
        }

        void SetMeshNormals(void* mesh, UnityArray<UnityResolve::UnityType::Vector3>* normals) {
            static auto fn = reinterpret_cast<void (*)(void*, UnityArray<UnityResolve::UnityType::Vector3>*)>(
                Il2cppUtils::GetMethodPointer(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "set_normals"));
            if (mesh && normals && fn) fn(mesh, normals);
        }

        UnityArray<UnityResolve::UnityType::Vector4>* GetMeshTangents(void* mesh) {
            static auto fn = reinterpret_cast<UnityArray<UnityResolve::UnityType::Vector4>* (*)(void*)>(
                Il2cppUtils::GetMethodPointer(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "get_tangents"));
            return mesh && fn ? fn(mesh) : nullptr;
        }

        void SetMeshTangents(void* mesh, UnityArray<UnityResolve::UnityType::Vector4>* tangents) {
            static auto fn = reinterpret_cast<void (*)(void*, UnityArray<UnityResolve::UnityType::Vector4>*)>(
                Il2cppUtils::GetMethodPointer(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "set_tangents"));
            if (mesh && tangents && fn) fn(mesh, tangents);
        }

        UnityResolve::UnityType::Matrix4x4 GetTransformLocalToWorldMatrix(void* transform) {
            static auto Transform_get_localToWorldMatrix = reinterpret_cast<UnityResolve::UnityType::Matrix4x4(*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Transform", "get_localToWorldMatrix"));
            return transform && Transform_get_localToWorldMatrix ? Transform_get_localToWorldMatrix(transform) : UnityResolve::UnityType::Matrix4x4{};
        }

        UnityResolve::UnityType::Matrix4x4 GetTransformWorldToLocalMatrix(void* transform) {
            static auto Transform_get_worldToLocalMatrix = reinterpret_cast<UnityResolve::UnityType::Matrix4x4(*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Transform", "get_worldToLocalMatrix"));
            return transform && Transform_get_worldToLocalMatrix ? Transform_get_worldToLocalMatrix(transform) : UnityResolve::UnityType::Matrix4x4{};
        }

        UnityResolve::UnityType::Matrix4x4 MultiplyMatrix4x4(const UnityResolve::UnityType::Matrix4x4& left,
            const UnityResolve::UnityType::Matrix4x4& right) {
            static auto Matrix4x4_op_Multiply = reinterpret_cast<UnityResolve::UnityType::Matrix4x4(*)(UnityResolve::UnityType::Matrix4x4, UnityResolve::UnityType::Matrix4x4)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Matrix4x4", "op_Multiply",
                    { "UnityEngine.Matrix4x4", "UnityEngine.Matrix4x4" }));
            if (Matrix4x4_op_Multiply) return Matrix4x4_op_Multiply(left, right);

            UnityResolve::UnityType::Matrix4x4 result{};
            for (int row = 0; row < 4; ++row) {
                for (int col = 0; col < 4; ++col) {
                    float value = 0.0f;
                    for (int i = 0; i < 4; ++i) value += left.m[row][i] * right.m[i][col];
                    result.m[row][col] = value;
                }
            }
            return result;
        }

        UnityResolve::UnityType::Matrix4x4 GetBindposeRendererSpaceAdjustment(void* originalRenderer, void* modRenderer) {
            const auto originalTransform = GetComponentTransform(originalRenderer);
            const auto modTransform = GetComponentTransform(modRenderer);
            if (!originalTransform || !modTransform) return {};

            return MultiplyMatrix4x4(
                GetTransformWorldToLocalMatrix(modTransform),
                GetTransformLocalToWorldMatrix(originalTransform));
        }

        UnityArray<UnityResolve::UnityType::Matrix4x4>* GetMeshBindposes(void* mesh) {
            static auto Mesh_get_bindposes = reinterpret_cast<UnityArray<UnityResolve::UnityType::Matrix4x4>* (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "get_bindposes"));
            return mesh && Mesh_get_bindposes ? Mesh_get_bindposes(mesh) : nullptr;
        }

        void SetMeshBindposes(void* mesh, UnityArray<UnityResolve::UnityType::Matrix4x4>* bindposes) {
            static auto Mesh_set_bindposes = reinterpret_cast<void (*)(void*, UnityArray<UnityResolve::UnityType::Matrix4x4>*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "set_bindposes"));
            if (mesh && bindposes && Mesh_set_bindposes) Mesh_set_bindposes(mesh, bindposes);
        }

        UnityArray<LocalModBoneWeight>* GetMeshBoneWeights(void* mesh) {
            static auto Mesh_get_boneWeights = reinterpret_cast<UnityArray<LocalModBoneWeight>* (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "get_boneWeights"));
            return mesh && Mesh_get_boneWeights ? Mesh_get_boneWeights(mesh) : nullptr;
        }

        void SetMeshBoneWeights(void* mesh, UnityArray<LocalModBoneWeight>* boneWeights) {
            static auto Mesh_set_boneWeights = reinterpret_cast<void (*)(void*, UnityArray<LocalModBoneWeight>*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "set_boneWeights"));
            if (mesh && boneWeights && Mesh_set_boneWeights) Mesh_set_boneWeights(mesh, boneWeights);
        }

        std::unordered_map<std::string, size_t> BuildBoneNameIndexMap(UnityArray<void*>* bones) {
            std::unordered_map<std::string, size_t> result;
            if (!bones) return result;

            for (std::uintptr_t i = 0; i < bones->max_length; ++i) {
                const auto name = GetUnityObjectNameString(bones->At(static_cast<unsigned int>(i)));
                if (!name.empty() && !result.contains(name)) {
                    result.emplace(name, static_cast<size_t>(i));
                }
            }
            return result;
        }

        bool LoadIpBoneSidecar(const LocalModAssetReplacement& replacement, std::vector<LocalIpBone>& bones,
            std::vector<LocalIpExtraBone>& extraBones, std::vector<LocalIpSwingChain>& swingChains,
            std::string& fingerprint, LocalIpSidecarOptions& options) {
            if (replacement.skeletonAssetName.empty()) return false;

            // Test packages may pair an already-built Unity bundle with a sidecar on disk.
            // The exact prefix plus protocol-2 validation below keeps this from becoming a
            // general production file loader.  Only a bare filename beside mod.json is
            // accepted: no absolute path and no directory traversal.
            constexpr std::string_view kExperimentalFilePrefix = "experimental-file:";
            std::string text;
            if (replacement.skeletonAssetName.starts_with(kExperimentalFilePrefix)) {
                const auto filename = replacement.skeletonAssetName.substr(kExperimentalFilePrefix.size());
                const auto relative = std::filesystem::path(filename);
                if (filename.empty() || relative.is_absolute() || relative.filename() != relative) {
                    Log::ErrorFmt("[ModAsset][EXPERIMENT] Invalid external sidecar filename: %s",
                        replacement.skeletonAssetName.c_str());
                    return false;
                }
                const auto sidecarPath = std::filesystem::path(replacement.manifestPath).parent_path() / relative;
                std::ifstream stream(sidecarPath, std::ios::binary);
                if (!stream) {
                    Log::ErrorFmt("[ModAsset][EXPERIMENT] External sidecar is unavailable: %s",
                        sidecarPath.string().c_str());
                    return false;
                }
                text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
                Log::WarnFmt("[ModAsset][EXPERIMENT] Loaded protocol-2 sidecar beside manifest: %s",
                    sidecarPath.string().c_str());
            }
            else {
                const auto asset = LoadLocalModAssetFromBundle(
                    replacement.bundleHandle,
                    replacement.bundlePath,
                    replacement.skeletonAssetName,
                    "TextAsset");
                text = GetTextAssetText(asset);
            }
            if (text.empty()) {
                Log::ErrorFmt("[ModAsset] IP skeleton sidecar is empty or unavailable: %s asset=%s",
                    replacement.sourceName.c_str(), replacement.skeletonAssetName.c_str());
                return false;
            }

            try {
                const auto document = nlohmann::json::parse(text);
                constexpr int kAbRuntimeProtocol = 1;
                constexpr int kExperimentalSourceProxyProtocol = 2;
                if (!document.contains("runtimeProtocol") || !document["runtimeProtocol"].is_number_integer()) {
                    throw std::runtime_error("runtimeProtocol is required (exporter/runtime mismatch)");
                }
                const auto runtimeProtocol = document["runtimeProtocol"].get<int>();
                if (runtimeProtocol != kAbRuntimeProtocol
                    && runtimeProtocol != kExperimentalSourceProxyProtocol) {
                    throw std::runtime_error(
                        "unsupported runtimeProtocol=" + std::to_string(runtimeProtocol)
                        + ", expected=1 or experimental 2");
                }
                options = {};
                options.runtimeProtocol = runtimeProtocol;
                const auto hasExperimentalSourceProxy = document.contains("experimentalSourceProxy");
                if (runtimeProtocol == kAbRuntimeProtocol && hasExperimentalSourceProxy) {
                    throw std::runtime_error(
                        "experimentalSourceProxy requires runtimeProtocol=2");
                }
                if (runtimeProtocol == kExperimentalSourceProxyProtocol) {
                    if (!hasExperimentalSourceProxy || !document["experimentalSourceProxy"].is_object()) {
                        throw std::runtime_error(
                            "runtimeProtocol=2 requires experimentalSourceProxy object");
                    }
                    const auto& experiment = document["experimentalSourceProxy"];
                    if (!experiment.contains("mode") || !experiment["mode"].is_string()) {
                        throw std::runtime_error("experimentalSourceProxy.mode is required");
                    }
                    const auto mode = experiment["mode"].get<std::string>();
                    if (mode == "rest-only") options.sourceProxyAnimationMode = 0;
                    else if (mode == "animation-bridge-minimal") options.sourceProxyAnimationMode = 1;
                    else if (mode == "animation-bridge") options.sourceProxyAnimationMode = 2;
                    else {
                        throw std::runtime_error(
                            "experimentalSourceProxy.mode must be rest-only, "
                            "animation-bridge-minimal or animation-bridge");
                    }
                    if (options.sourceProxyAnimationMode != 0 && !document.contains("semanticMap")) {
                        throw std::runtime_error("an animation bridge needs a semanticMap");
                    }
                    options.sourceProxyRestOnly = true;
                    options.sourceProxyRootBoneName = document.contains("rootBone")
                        && document["rootBone"].is_string()
                        ? document["rootBone"].get<std::string>() : std::string{};
                    options.sourceProxyRootTransformName = document.contains("rootTransform")
                        && document["rootTransform"].is_string()
                        ? document["rootTransform"].get<std::string>() : std::string{};
                    // No fallback to the old single-array shape.  It could not express an
                    // unweighted root, and keeping it alive would mean a package the
                    // offline gate rejects still loads in game.
                    if (!document.contains("transforms") || !document["transforms"].is_array()) {
                        throw std::runtime_error("runtimeProtocol=2 requires a transforms array");
                    }
                    options.sourceProxyHasTransforms = true;
                }
                if (!document.contains("buildId") || !document["buildId"].is_string()
                    || document["buildId"].get<std::string>().empty()) {
                    throw std::runtime_error("buildId is required for bundle/log correlation");
                }
                // 指纹 = buildId + 全文哈希。buildId 单独不够：它是导出期算的，作者手改
                // sidecar（或换了导出器版本而 buildId 没滚）时不动，而缓存/骨复用两处都靠
                // 它判"还是不是同一份骨架"。
                fingerprint = document["buildId"].get<std::string>() + "/"
                    + std::to_string(std::hash<std::string>{}(text));
                Log::InfoFmt("[ModAsset] IP skeleton sidecar protocol=%d buildId=%s fingerprint=%s source=%s",
                    runtimeProtocol, document["buildId"].get<std::string>().c_str(),
                    fingerprint.c_str(), replacement.sourceName.c_str());
                if (options.sourceProxyRestOnly) {
                    Log::WarnFmt("[ModAsset][EXPERIMENT] Source-proxy mode=%d enabled: source=%s; physics is still disabled (0=rest-only, 1=minimal bridge, 2=full bridge)",
                        options.sourceProxyAnimationMode, replacement.sourceName.c_str());
                }
                if (!options.sourceProxyHasTransforms
                    && (!document.contains("bones") || !document["bones"].is_array())) {
                    throw std::runtime_error("bones array is required");
                }
                // Protocol 2 with `transforms` describes the hierarchy there; `bones` is
                // then meaningless and must not be half-read into the same vector.
                const auto& boneArray = options.sourceProxyHasTransforms
                    ? document["transforms"] : document["bones"];

                const auto parseVector3 = [](const nlohmann::json& value) {
                    if (!value.is_array() || value.size() < 3) throw std::runtime_error("Vector3 array is invalid");
                    return UnityResolve::UnityType::Vector3(
                        value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
                };
                const auto parseQuaternion = [](const nlohmann::json& value) {
                    if (!value.is_array() || value.size() < 4) throw std::runtime_error("Quaternion array is invalid");
                    return UnityResolve::UnityType::Quaternion(
                        value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>());
                };

                const auto parseSwing = [](const nlohmann::json& item) -> std::optional<LocalIpBoneSwing> {
                    if (!item.contains("swing") || !item["swing"].is_object()) return std::nullopt;
                    const auto& s = item["swing"];
                    // 布尔位两种写法都收：游戏侧是 bool，但源模型和原版基准表统计出来的都是
                    // 1/0。`value(key, false)` 遇到数字会抛 type_error.302，而那一抛就是
                    // **整份 sidecar 作废、骨架 graft 整个跳过**（表现是网格没换、只有贴图
                    // 生效）—— 一个标志位的写法不该有这种爆炸半径。
                    const auto flag = [&s](const char* key, bool fallback) {
                        if (!s.contains(key)) return fallback;
                        const auto& value = s[key];
                        if (value.is_boolean()) return value.get<bool>();
                        if (value.is_number()) return value.get<double>() != 0.0;
                        return fallback;
                    };
                    LocalIpBoneSwing swing{
                        s.value("damping", 0.0f), s.value("stiffness", 0.0f),
                        s.value("spring", 0.0f), s.value("mass", 0.0f),
                        flag("useWindGlobalForce", false) };
                    swing.rootWeight = s.value("rootWeight", -1.0f);
                    swing.pendulum = s.value("pendulum", -1.0f);
                    swing.pendulumRange = s.value("pendulumRange", -1.0f);
                    swing.wind = s.value("wind", -1.0f);
                    swing.dynamicType = s.value("dynamicType", -1);
                    swing.useLimit = s.value("useLimit", -1);
                    const auto parseLimit = [&](const char* key, int (&out)[2]) {
                        if (!s.contains(key) || !s[key].is_array() || s[key].size() < 2) return;
                        out[0] = s[key][0].get<int>();
                        out[1] = s[key][1].get<int>();
                    };
                    parseLimit("limitX", swing.limit[0]);
                    parseLimit("limitY", swing.limit[1]);
                    parseLimit("limitZ", swing.limit[2]);
                    // 碰撞体字段平铺在 swing 对象里（导出器按原版 ActorSwingDynamicBone 的
                    // 字段名写）。同时仍收 manifest-v2 里写过的嵌套写法
                    // `swing.collider.{radius,type,collisionMask}` —— 只认平铺的话，按旧文档
                    // 产出的包会**静默丢掉整套碰撞体配置**，表现是手臂穿过装饰件而日志全绿。
                    swing.colliderRadius = s.value("colliderRadius", -1.0f);
                    swing.colliderRadiusSub = s.value("colliderRadiusSub", 0.05f);
                    swing.colliderType = s.value("colliderType", 0);
                    swing.collisionMask = s.value("collisionMask", -1);
                    if (s.contains("collider") && s["collider"].is_object()) {
                        const auto& c = s["collider"];
                        if (swing.colliderRadius < 0.0f) swing.colliderRadius = c.value("radius", -1.0f);
                        if (!s.contains("colliderType")) swing.colliderType = c.value("type", 0);
                        if (!s.contains("collisionMask")) swing.collisionMask = c.value("collisionMask", -1);
                    }
                    return swing;
                };

                // P3：`"driver": {"type":"Skirt", "ints":{...}, "floats":{...},
                //                 "vectors":{"innerCoefficient":[0,0.1,0.1]}, "bones":{"referenceBone":"Hips"}}`
                // 四张表分开是为了让类型显式 —— JSON 的 0 既可能是 int 也可能是 float，
                // 靠形状猜会把 `rotationOrder` 写成浮点、把枚举写坏，而且这种错在日志里看不出来。
                const auto parseDriver = [](const nlohmann::json& item) -> std::optional<LocalQuartzDriver> {
                    if (!item.contains("driver") || !item["driver"].is_object()) return std::nullopt;
                    const auto& d = item["driver"];
                    if (!d.contains("type") || !d["type"].is_string()) {
                        throw std::runtime_error("driver needs a type");
                    }
                    LocalQuartzDriver driver{};
                    driver.type = d["type"].get<std::string>();
                    if (d.contains("ints") && d["ints"].is_object()) {
                        for (const auto& [key, value] : d["ints"].items()) {
                            if (value.is_number()) driver.ints[key] = value.get<int>();
                        }
                    }
                    if (d.contains("floats") && d["floats"].is_object()) {
                        for (const auto& [key, value] : d["floats"].items()) {
                            if (value.is_number()) driver.floats[key] = value.get<float>();
                        }
                    }
                    if (d.contains("vectors") && d["vectors"].is_object()) {
                        for (const auto& [key, value] : d["vectors"].items()) {
                            if (!value.is_array() || value.size() < 3) continue;
                            driver.vectors[key] = { value[0].get<float>(), value[1].get<float>(),
                                                    value[2].get<float>() };
                        }
                    }
                    if (d.contains("bones") && d["bones"].is_object()) {
                        for (const auto& [key, value] : d["bones"].items()) {
                            if (value.is_string()) driver.bones[key] = value.get<std::string>();
                        }
                    }
                    return driver;
                };

                bones.clear();
                bones.reserve(boneArray.size());
                for (const auto& item : boneArray) {
                    if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) {
                        throw std::runtime_error("bone name is required");
                    }
                    LocalIpBone bone{};
                    bone.name = item["name"].get<std::string>();
                    bone.parentIndex = item.value("parentIndex", -1);
                    bone.localPosition = parseVector3(item.at("localPosition"));
                    bone.localRotation = parseQuaternion(item.at("localRotation"));
                    bone.localScale = parseVector3(item.at("localScale"));
                    bone.swing = parseSwing(item);
                    bone.driver = parseDriver(item);
                    bones.emplace_back(std::move(bone));
                }

                if (options.sourceProxyHasTransforms) {
                    // Everything below fails closed.  A protocol-2 package whose indices
                    // or semantics do not resolve must stop the renderer, not degrade to
                    // "applied successfully" with a mesh hanging off the wrong bones.
                    if (bones.empty()) throw std::runtime_error("transforms array is empty");
                    for (size_t index = 0; index < bones.size(); ++index) {
                        const auto parent = bones[index].parentIndex;
                        if (parent < -1 || parent >= static_cast<int>(bones.size())
                            || parent == static_cast<int>(index)) {
                            throw std::runtime_error("transforms[" + std::to_string(index)
                                + "] has an out-of-range parentIndex");
                        }
                    }
                    const auto indexOfTransform = [&bones](const std::string& name) {
                        const auto found = std::find_if(bones.begin(), bones.end(),
                            [&name](const LocalIpBone& bone) { return bone.name == name; });
                        return found == bones.end()
                            ? -1 : static_cast<int>(std::distance(bones.begin(), found));
                    };

                    if (!document.contains("skinBones") || !document["skinBones"].is_array()) {
                        throw std::runtime_error("transforms requires a skinBones array");
                    }
                    for (const auto& item : document["skinBones"]) {
                        if (!item.is_number_integer()) {
                            throw std::runtime_error("skinBones entries must be integers");
                        }
                        const auto index = item.get<int>();
                        if (index < 0 || index >= static_cast<int>(bones.size())) {
                            throw std::runtime_error("skinBones index " + std::to_string(index)
                                + " is outside transforms");
                        }
                        options.sourceProxySkinBones.emplace_back(index);
                    }
                    if (options.sourceProxySkinBones.empty()) {
                        throw std::runtime_error("skinBones array is empty");
                    }
                    // The bindposes live on the bundle mesh; duplicating them in JSON would
                    // just create a second truth.  Declaring the count keeps the exporter
                    // honest, and the renderer-side count check runs against the real mesh.
                    if (document.contains("bindposeCount")
                        && document["bindposeCount"].is_number_integer()
                        && document["bindposeCount"].get<int>()
                            != static_cast<int>(options.sourceProxySkinBones.size())) {
                        throw std::runtime_error("bindposeCount does not match skinBones");
                    }

                    if (!options.sourceProxyRootBoneName.empty()
                        && indexOfTransform(options.sourceProxyRootBoneName) < 0) {
                        throw std::runtime_error("rootBone \"" + options.sourceProxyRootBoneName
                            + "\" is not in transforms");
                    }
                    if (!options.sourceProxyRootTransformName.empty()
                        && indexOfTransform(options.sourceProxyRootTransformName) < 0) {
                        throw std::runtime_error("rootTransform \""
                            + options.sourceProxyRootTransformName + "\" is not in transforms");
                    }

                    if (document.contains("semanticMap")) {
                        if (!document["semanticMap"].is_object()) {
                            throw std::runtime_error("semanticMap must be an object");
                        }
                        for (const auto& [semantic, value] : document["semanticMap"].items()) {
                            if (!value.is_string()) {
                                throw std::runtime_error("semanticMap." + semantic
                                    + " must name a source transform");
                            }
                            const auto index = indexOfTransform(value.get<std::string>());
                            if (index < 0) {
                                throw std::runtime_error("semanticMap." + semantic + " -> \""
                                    + value.get<std::string>() + "\" is not in transforms");
                            }
                            options.sourceProxySemanticMap.emplace_back(semantic, index);
                        }
                    }

                    // Parsed and reference-checked now, consumed by the head/face stage.
                    if (document.contains("headSocket")) {
                        const auto& socket = document["headSocket"];
                        if (!socket.is_object() || !socket.contains("transform")
                            || !socket["transform"].is_string()) {
                            throw std::runtime_error("headSocket needs a transform name");
                        }
                        options.sourceProxyHeadSocket =
                            indexOfTransform(socket["transform"].get<std::string>());
                        if (options.sourceProxyHeadSocket < 0) {
                            throw std::runtime_error("headSocket.transform \""
                                + socket["transform"].get<std::string>() + "\" is not in transforms");
                        }
                    }

                    Log::InfoFmt("[ModAsset][EXPERIMENT] Source-proxy protocol 2 sidecar: transforms=%zu skinBones=%zu semanticMap=%zu rootTransform=%s rootBone=%s headSocket=%d",
                        bones.size(), options.sourceProxySkinBones.size(),
                        options.sourceProxySemanticMap.size(),
                        options.sourceProxyRootTransformName.c_str(),
                        options.sourceProxyRootBoneName.c_str(),
                        options.sourceProxyHeadSocket);
                }

                extraBones.clear();
                for (const auto* field : { "extraSwingBones", "newBones" }) {
                    if (!document.contains(field) || !document[field].is_array()) continue;
                    for (const auto& item : document[field]) {
                        if (!item.is_object() || !item.contains("name") || !item.contains("parentName")) {
                            throw std::runtime_error("extra swing bone needs name and parentName");
                        }
                        LocalIpExtraBone bone{};
                        bone.name = item["name"].get<std::string>();
                        bone.parentName = item["parentName"].get<std::string>();
                        bone.localPosition = parseVector3(item.at("localPosition"));
                        bone.localRotation = parseQuaternion(item.at("localRotation"));
                        bone.localScale = parseVector3(item.at("localScale"));
                        bone.swing = parseSwing(item);
                        bone.driver = parseDriver(item);
                        extraBones.emplace_back(std::move(bone));
                    }
                }

                swingChains.clear();
                if (document.contains("swingChains") && !document["swingChains"].is_null()) {
                    // 容器类型错了就报错，别静默当"没有链"：导出侧验证器把它判成 error，
                    // 运行时却照常 graft，结果是包被判坏而实机"看起来正常，只是不摆"。
                    if (!document["swingChains"].is_array()) {
                        throw std::runtime_error("swingChains must be an array");
                    }
                    for (const auto& item : document["swingChains"]) {
                        if (!item.is_object() || !item.contains("host")
                            || !item.contains("rootBones") || !item["rootBones"].is_array()) {
                            throw std::runtime_error("swing chain needs host and rootBones");
                        }
                        LocalIpSwingChain chain{};
                        chain.host = item["host"].get<std::string>();
                        chain.category = item.value("category", "");
                        // 只进日志、不参与建链。类型错了由导出侧验证器报错就够，运行时
                        // 别为一个日志字段抛 type_error.302 把整份 sidecar 作废
                        //（useWindGlobalForce 那次的爆炸半径见上面 parseSwing 的注释）。
                        chain.chainLength = item.contains("chainLength")
                            && item["chainLength"].is_number_integer()
                            ? item["chainLength"].get<int>() : 0;
                        for (const auto& name : item["rootBones"]) {
                            chain.rootBones.emplace_back(name.get<std::string>());
                        }
                        chain.around = item.contains("around") && item["around"].is_boolean()
                            ? (item["around"].get<bool>() ? 1 : 0) : -1;
                        swingChains.emplace_back(std::move(chain));
                    }
                }
                return !bones.empty();
            }
            catch (const std::exception& e) {
                Log::ErrorFmt("[ModAsset] Cannot parse IP skeleton sidecar: %s asset=%s error=%s",
                    replacement.sourceName.c_str(), replacement.skeletonAssetName.c_str(), e.what());
                bones.clear();
                options = {};
                return false;
            }
        }

        bool SetTransformParent(UnityResolve::UnityType::Transform* child,
            UnityResolve::UnityType::Transform* parent) {
            static auto Transform_SetParent = FindMethodByNameAndArgCount(
                Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Transform"),
                "SetParent", 2);
            if (!child || !parent || !Transform_SetParent) return false;
            Transform_SetParent->Invoke<void>(child, parent, false);
            return true;
        }

        bool AttachNativeChainToLiveRoot(UnityResolve::UnityType::Transform* rootTransform) {
            if (!rootTransform) return false;
            const auto rootGameObject = rootTransform->GetGameObject();
            if (!rootGameObject) return false;
            {
                std::lock_guard swingStateLock(g_swingStateMutex);
                if (g_nativeChainAttachedRoots.contains(rootGameObject)) return true;
            }

            const auto rendererClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer");
            if (!rendererClass) return false;
            const auto renderers = rootGameObject->GetComponentsInChildren<void*>(rendererClass, true);

            std::vector<LocalModAssetReplacementPtr> activeReplacements;
            {
                std::shared_lock replacementLock(g_replacementMutex);
                activeReplacements.reserve(g_replacementMap.size());
                for (const auto& [key, replacement] : g_replacementMap) {
                    (void)key;
                    activeReplacements.emplace_back(replacement);
                }
            }
            for (const auto& replacementPtr : activeReplacements) {
                if (!replacementPtr) continue;
                auto& replacement = *replacementPtr;
                if (!replacement.attachToOriginal || !replacement.attachAsset) continue;
                bool matched = false;
                void* matchedMesh = nullptr;
                for (const auto renderer : renderers) {
                    const auto mesh = GetSkinnedMeshRendererSharedMesh(renderer);
                    if (mesh && std::find(replacement.attachSourceMeshes.begin(), replacement.attachSourceMeshes.end(), mesh)
                        != replacement.attachSourceMeshes.end()) {
                        matched = true;
                        matchedMesh = mesh;
                        break;
                    }
                }
                if (!matched) {
                    Log::InfoFmt("[ModAsset] Native chain live-root mismatch: root=%s target=%s renderers=%zu sourceMeshes=%zu",
                        GetUnityObjectNameString(rootGameObject).c_str(), replacement.sourceName.c_str(),
                        renderers.size(), replacement.attachSourceMeshes.size());
                    continue;
                }

                const auto subtreeClone = CloneUnityObject(replacement.attachAsset, replacement.sourceName, 0);
                const auto cloneGameObject = reinterpret_cast<UnityResolve::UnityType::GameObject*>(subtreeClone);
                const auto attached = cloneGameObject
                    && SetTransformParent(cloneGameObject->GetTransform(), rootTransform);
                if (!attached) return false;
                // ponytail: the §4 probe has one attach subtree per actor; key by
                // (root, replacement) when manifests need multiple native subtrees.
                {
                    std::lock_guard swingStateLock(g_swingStateMutex);
                    g_nativeChainAttachedRoots.emplace(rootGameObject);
                }
                Log::InfoFmt("[ModAsset] Attached native chain subtree to live actor: root=%s source=%s matchedMesh=%s clone=%p",
                    GetUnityObjectNameString(rootGameObject).c_str(), replacement.sourceName.c_str(),
                    GetUnityObjectNameString(matchedMesh).c_str(), subtreeClone);
                return true;
            }
            return false;
        }

        // 按 sidecar 的 swingChains 建 ActorSwingChain —— 在 graft 时、prefab 上完成。
        //
        // 时机：prefab 上 AddComponent 不触发 OnEnable，游戏 Instantiate 时才触发，届时
        // ActorSwingChain.OnEnable 自己调 UpdateChainInfo 建层；同时
        // CampusActorAnimation.Initialize() 把链和骨（都实现 IActorAnimationBone）一起
        // GetComponentsInChildren 收进 CampusActorAnimationInitializeData，两张并行表由它
        // 自己保证同长。所以这里只把组件放对位置，建层/注册/初始变换全部不碰。
        //
        // 建不建、挂哪、怎么分组全由导出器算好（它离线可测）：530 套原版实测裙类 94% 挂链、
        // 飘带绳结类只有 2.6%（蝴蝶结在原版里就是裸 ActorSwingDynamicBone，链多带一层
        // around/radius 的环形碰撞解算，那是裙摆专用的）；而链必须按长度分组，否则
        // UpdateChainInfo 会把整条链截到最短成员的长度。这里不做任何启发式。
        size_t AttachSwingChainsToGraftedSkeleton(
            void* originalRenderer, const std::vector<LocalIpSwingChain>& swingChains,
            const std::unordered_map<std::string, UnityResolve::UnityType::Transform*>& graftedBones) {
            if (swingChains.empty()) return 0;
            const auto chainClass = FindClassByName("ActorSwingChain");
            const auto dynamicBoneClass = FindClassByName("ActorSwingDynamicBone");
            const auto transformClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
            const auto hierarchyRoot = GetHierarchyRootGameObject(originalRenderer);
            if (!chainClass || !dynamicBoneClass || !transformClass || !hierarchyRoot) {
                Log::WarnFmt("[ModAsset] ActorSwing chain skipped: chainClass=%p boneClass=%p root=%p",
                    chainClass, dynamicBoneClass, hierarchyRoot);
                return 0;
            }
            const auto rootGameObject = reinterpret_cast<UnityResolve::UnityType::GameObject*>(hierarchyRoot);

            // 宿主骨是**游戏骨**，按名字在层级里找（游戏骨名在一个角色内唯一）。
            std::unordered_map<std::string, UnityResolve::UnityType::GameObject*> byName;
            for (const auto item : rootGameObject->GetComponentsInChildren<void*>(transformClass, true)) {
                const auto gameObject = reinterpret_cast<UnityResolve::UnityType::Transform*>(item)->GetGameObject();
                if (gameObject) byName.emplace(GetUnityObjectNameString(gameObject), gameObject);
            }
            // 链根只认**本次 graft 自己建/复用的那批骨**。按名字全层级找会串包：body 和 hair
            // 各有同名新骨时，后者虽然建了独立 Transform，建链时却可能拿到先出现的前者 ——
            // 要么被 alreadyRooted 排除掉不建链，要么建出来的链去驱动别人的骨。
            // 已经在某条链的 rootBones 里的骨不再重复挂（同一 prefab 重复 graft、body+hair
            // 两个 renderer 共用一套骨架时都会走到这里）。顺便借一个 List 实例当类型模板。
            std::unordered_set<void*> alreadyRooted;
            void* templateRootList = nullptr;
            for (const auto chain : rootGameObject->GetComponentsInChildren<void*>(chainClass, true)) {
                const auto rootBones = chainClass->GetValue<void*>(chain, "rootBones");
                if (!rootBones) continue;
                if (!templateRootList) templateRootList = rootBones;
                const auto list = reinterpret_cast<UnityResolve::UnityType::List<void*>*>(rootBones);
                if (!list->pList) continue;
                for (int i = 0; i < list->size; ++i)
                    alreadyRooted.insert(list->pList->At(static_cast<unsigned int>(i)));
            }

            size_t built = 0;
            for (const auto& spec : swingChains) {
                const auto host = byName.find(spec.host);
                if (host == byName.end()) {
                    Log::WarnFmt("[ModAsset] ActorSwing chain host not in skeleton, skipped: host=%s roots=%zu",
                        spec.host.c_str(), spec.rootBones.size());
                    continue;
                }
                std::vector<void*> roots;
                for (const auto& name : spec.rootBones) {
                    const auto bone = graftedBones.find(name);
                    const auto gameObject = bone == graftedBones.end()
                        ? nullptr : bone->second->GetGameObject();
                    const auto component = gameObject
                        ? gameObject->GetComponent<void*>(dynamicBoneClass) : nullptr;
                    if (!component) {
                        Log::WarnFmt("[ModAsset] ActorSwing chain root missing its bone, skipped: %s",
                            name.c_str());
                        continue;
                    }
                    if (!alreadyRooted.count(component)) roots.emplace_back(component);
                }
                if (roots.empty()) continue;

                const auto chain = AddComponentByClass(host->second, chainClass);
                if (!chain) continue;
                if (spec.around >= 0) {
                    std::lock_guard swingStateLock(g_swingStateMutex);
                    g_modChainAroundByHost[spec.host] = spec.around;
                }
                auto rootBones = chainClass->GetValue<void*>(chain, "rootBones");
                if (!rootBones && templateRootList) {
                    rootBones = CreateObjectLike(templateRootList);
                    if (rootBones) {
                        if (const auto field = chainClass->Get<UnityResolve::Field>("rootBones"))
                            SetManagedReferenceField(chain, field->offset, rootBones);
                    }
                }
                size_t added = 0;
                if (rootBones) {
                    for (const auto bone : roots)
                        if (ListAddManaged(rootBones, bone)) { alreadyRooted.insert(bone); ++added; }
                }
                // 一根都没挂上的链是空壳：它照样会被 CampusActorAnimation.Initialize() 收走、
                // 进 rigData，然后什么也不驱动。销毁掉，别留在 prefab 上。
                if (added == 0) {
                    Log::ErrorFmt("[ModAsset] ActorSwing chain has no usable rootBones, destroyed: host=%s roots=%zu list=%p",
                        spec.host.c_str(), spec.rootBones.size(), rootBones);
                    DestroyComponentImmediate(chain);
                    continue;
                }
                ++built;
                Log::InfoFmt("[ModAsset] ActorSwing chain built on prefab: host=%s category=%s chainLength=%d roots=%zu/%zu",
                    spec.host.c_str(), spec.category.c_str(), spec.chainLength, added, spec.rootBones.size());
            }
            return built;
        }

        // Experimental protocol 2 path.  Unlike BuildHybridBoneArray, this deliberately
        // reuses no game Transform: every weighted source bone is rebuilt from the source
        // local rest data and the renderer continues to use the source weights/bindposes.
        //
        // This first stage is rest-only by design.  It answers one narrow question in the
        // client: can the runtime preserve the source rig and render the mesh in its own
        // bind pose?  Driving these proxies from the live Humanoid pose is a separate stage;
        // mixing that into this probe would make a bad pose impossible to attribute.
        // COLLECT only — the caller destroys these AFTER the renderer has been switched to the
        // new proxy tree.  Destroying here crashed the game: mod OFF→ON re-applies to the LIVE
        // actor, not the prefab, so the container found here is the one whose bones are still
        // in that renderer's bone array, and freeing them mid-frame makes the game's own
        // LateUpdate walk a destroyed transform.
        std::vector<void*> CollectStaleSourceProxyContainers(void* proxyParent) {
            const auto transformClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
            const auto parentObject = proxyParent
                ? reinterpret_cast<UnityResolve::UnityType::Transform*>(proxyParent)->GetGameObject()
                : nullptr;
            std::vector<void*> stale;
            if (!parentObject || !transformClass) return stale;

            for (const auto transform :
                parentObject->GetComponentsInChildren<void*>(transformClass, true)) {
                if (GetUnityObjectNameString(transform).starts_with("__gmi_source_proxy_rest__")) {
                    stale.emplace_back(reinterpret_cast<UnityResolve::UnityType::Transform*>(
                        transform)->GetGameObject());
                }
            }
            return stale;
        }

        // DestroyImmediate, not Destroy: a prefab is not in a scene, so it can be instantiated
        // again before a deferred destroy would ever run — the orphan would be copied into the
        // next actor with the same bone names.
        void DestroyRetiredSourceProxyContainers(const std::vector<void*>& containers) {
            for (const auto object : containers) {
                if (object && IsNativeObjectAlive(object)) DestroyComponentImmediate(object);
            }
        }

        UnityArray<void*>* BuildSourceProxyBoneArray(void* originalRenderer,
            UnityArray<void*>* originalBones,
            const std::vector<LocalIpBone>& sidecarBones,
            const LocalIpSidecarOptions& options,
            const std::string& sourceName,
            const std::string& modId,
            const std::string& sidecarFingerprint,
            const size_t rendererIndex,
            size_t& semanticMatches,
            size_t& createdBones,
            void*& sourceRootBone,
            std::vector<void*>& proxyTree,
            std::vector<void*>& retiredContainers) {
            sourceRootBone = nullptr;
            proxyTree.clear();
            retiredContainers.clear();
            if (!originalRenderer || !originalBones || sidecarBones.empty()) return nullptr;

            const auto transformClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
            const auto gameObjectClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
            if (!transformClass || !gameObjectClass) return nullptr;

            const auto& skinBones = options.sourceProxySkinBones;
            if (skinBones.empty()) return nullptr;

            const auto indexOfBone = [&sidecarBones](const std::string& name) {
                const auto found = std::find_if(sidecarBones.begin(), sidecarBones.end(),
                    [&name](const LocalIpBone& bone) { return bone.name == name; });
                return found == sidecarBones.end()
                    ? static_cast<size_t>(-1)
                    : static_cast<size_t>(std::distance(sidecarBones.begin(), found));
            };

            size_t hierarchyRootIndex = sidecarBones.size();
            size_t rootCount = 0;
            for (size_t index = 0; index < sidecarBones.size(); ++index) {
                if (sidecarBones[index].parentIndex < 0) {
                    if (hierarchyRootIndex == sidecarBones.size()) hierarchyRootIndex = index;
                    ++rootCount;
                }
            }
            if (!options.sourceProxyRootTransformName.empty()) {
                hierarchyRootIndex = indexOfBone(options.sourceProxyRootTransformName);
                if (hierarchyRootIndex == static_cast<size_t>(-1)
                    || sidecarBones[hierarchyRootIndex].parentIndex >= 0) {
                    Log::ErrorFmt("[ModAsset][EXPERIMENT] Declared rootTransform is not a hierarchy top: %s rootTransform=%s",
                        sourceName.c_str(), options.sourceProxyRootTransformName.c_str());
                    return nullptr;
                }
            }
            if (hierarchyRootIndex == sidecarBones.size()) {
                Log::ErrorFmt("[ModAsset][EXPERIMENT] Source proxy has no root transform: %s renderer=%zu",
                    sourceName.c_str(), rendererIndex);
                return nullptr;
            }

            // The renderer root is a Transform, not necessarily a skinned bone.  Resolving
            // it against the full tree is the whole point of splitting the arrays: the
            // A-pose package declared `Hips`, which is a real transform but carries no
            // weights, and the skin-bone-only lookup could not find it.
            size_t rootIndex = hierarchyRootIndex;
            if (!options.sourceProxyRootBoneName.empty()) {
                rootIndex = indexOfBone(options.sourceProxyRootBoneName);
                if (rootIndex == static_cast<size_t>(-1)) {
                    Log::ErrorFmt("[ModAsset][EXPERIMENT] Declared source proxy rootBone is absent: %s root=%s",
                        sourceName.c_str(), options.sourceProxyRootBoneName.c_str());
                    return nullptr;
                }
            }
            else if (rootCount != 1) {
                Log::ErrorFmt("[ModAsset][EXPERIMENT] Source proxy has %zu hierarchy roots and no declared rootBone: %s",
                    rootCount, sourceName.c_str());
                return nullptr;
            }

            const auto originalBoneIndexMap = BuildBoneNameIndexMap(originalBones);
            semanticMatches = 0;
            for (const auto& bone : sidecarBones) {
                if (originalBoneIndexMap.contains(bone.name)) ++semanticMatches;
            }
            createdBones = sidecarBones.size();

            // The renderer array is a projection of the tree, not the tree itself.
            const auto makeRendererArray = [&](const std::vector<void*>& tree) {
                auto result = UnityArray<void*>::New(transformClass, skinBones.size());
                for (size_t index = 0; index < skinBones.size(); ++index) {
                    result->At(static_cast<unsigned int>(index)) =
                        tree[static_cast<size_t>(skinBones[index])];
                }
                return result;
            };

            // Keep the proxy and normal hybrid caches disjoint even for the same sidecar.
            const auto ownerKey = modId + "|" + sidecarFingerprint + "|" + sourceName
                + "|source-proxy-rest-only";
            std::vector<void*> candidateBones;
            {
                std::lock_guard swingStateLock(g_swingStateMutex);
                if (const auto cached = g_hybridBonesByRenderer.find(originalRenderer);
                    cached != g_hybridBonesByRenderer.end()
                    && cached->second.ownerKey == ownerKey) {
                    candidateBones = cached->second.bones;
                }
            }
            const auto cacheIsAlive = candidateBones.size() == sidecarBones.size()
                && std::all_of(candidateBones.begin(), candidateBones.end(),
                    [](void* bone) { return bone && IsNativeObjectAlive(bone); });
            if (cacheIsAlive) {
                sourceRootBone = candidateBones[rootIndex];
                proxyTree = candidateBones;
                Log::InfoFmt("[ModAsset][EXPERIMENT] Reused source proxy rest skeleton: %s renderer=%zu transforms=%zu skinBones=%zu root=%s",
                    sourceName.c_str(), rendererIndex, candidateBones.size(), skinBones.size(),
                    sidecarBones[rootIndex].name.c_str());
                return makeRendererArray(candidateBones);
            }
            if (!candidateBones.empty()) {
                std::lock_guard swingStateLock(g_swingStateMutex);
                if (const auto cached = g_hybridBonesByRenderer.find(originalRenderer);
                    cached != g_hybridBonesByRenderer.end()
                    && cached->second.ownerKey == ownerKey
                    && cached->second.bones == candidateBones) {
                    g_hybridBonesByRenderer.erase(cached);
                }
            }

            auto originalRoot = reinterpret_cast<UnityResolve::UnityType::Transform*>(
                GetSkinnedMeshRendererRootBone(originalRenderer));
            auto proxyParent = originalRoot ? originalRoot->GetParent() : nullptr;
            if (!proxyParent) proxyParent = GetHierarchyRoot(originalRenderer);
            if (!proxyParent) {
                Log::ErrorFmt("[ModAsset][EXPERIMENT] Cannot locate source proxy parent: %s renderer=%zu",
                    sourceName.c_str(), rendererIndex);
                return nullptr;
            }

            // One renderer gets exactly one proxy container.  Mod OFF→ON, an in-place package
            // update and any cache miss all reach this build path again, while the previous
            // container is still parented here carrying the SAME bone names — so the actor ends
            // up with two `__gmi_sp_6_Hips`, and the bridge binds by name and can pick the
            // orphan, which stops driving the body entirely.  Matched by prefix rather than
            // exact name on purpose: an in-place package update changes the sidecar
            // fingerprint, so the stale container's name no longer matches ours.
            retiredContainers = CollectStaleSourceProxyContainers(proxyParent);

            auto containerObject = gameObjectClass->New<UnityResolve::UnityType::GameObject>();
            if (!containerObject) return nullptr;
            const auto containerName = "__gmi_source_proxy_rest__"
                + std::to_string(std::hash<std::string>{}(ownerKey))
                + "_" + std::to_string(rendererIndex);
            UnityResolve::UnityType::GameObject::Create(containerObject, containerName);
            const auto containerTransform = containerObject->GetTransform();
            if (!containerTransform || !SetTransformParent(containerTransform, proxyParent)) {
                Log::ErrorFmt("[ModAsset][EXPERIMENT] Cannot parent source proxy container: %s renderer=%zu",
                    sourceName.c_str(), rendererIndex);
                return nullptr;
            }
            containerTransform->SetLocalPosition(UnityResolve::UnityType::Vector3(0.0f, 0.0f, 0.0f));
            containerTransform->SetLocalRotation(UnityResolve::UnityType::Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
            containerTransform->SetLocalScale(UnityResolve::UnityType::Vector3(1.0f, 1.0f, 1.0f));

            std::vector<void*> proxyBones(sidecarBones.size());
            std::vector<unsigned char> states(sidecarBones.size());
            std::function<bool(size_t)> buildBone;
            buildBone = [&](const size_t index) {
                if (index >= sidecarBones.size()) return false;
                if (states[index] == 2) return true;
                if (states[index] == 1) return false;
                states[index] = 1;

                const auto& sourceBone = sidecarBones[index];
                auto parent = containerTransform;
                if (sourceBone.parentIndex >= 0) {
                    const auto parentIndex = static_cast<size_t>(sourceBone.parentIndex);
                    if (parentIndex >= sidecarBones.size() || !buildBone(parentIndex)) return false;
                    parent = reinterpret_cast<UnityResolve::UnityType::Transform*>(proxyBones[parentIndex]);
                }

                auto gameObject = gameObjectClass->New<UnityResolve::UnityType::GameObject>();
                if (!gameObject) return false;
                const auto proxyName = "__gmi_sp_" + std::to_string(index) + "_" + sourceBone.name;
                UnityResolve::UnityType::GameObject::Create(gameObject, proxyName);
                const auto transform = gameObject->GetTransform();
                if (!transform || !SetTransformParent(transform, parent)) return false;
                transform->SetLocalPosition(sourceBone.localPosition);
                transform->SetLocalRotation(sourceBone.localRotation);
                transform->SetLocalScale(sourceBone.localScale);
                proxyBones[index] = transform;
                states[index] = 2;
                return true;
            };

            for (size_t index = 0; index < sidecarBones.size(); ++index) {
                if (!buildBone(index)) {
                    Log::ErrorFmt("[ModAsset][EXPERIMENT] Cannot build source proxy hierarchy: %s renderer=%zu bone=%s index=%zu",
                        sourceName.c_str(), rendererIndex, sidecarBones[index].name.c_str(), index);
                    return nullptr;
                }
            }

            {
                std::lock_guard swingStateLock(g_swingStateMutex);
                g_runtimeBoneHandles.emplace_back(UnityResolve::Invoke<Il2CppGCHandle>(
                    "il2cpp_gchandle_new", containerObject, false));
                for (const auto proxy : proxyBones) {
                    const auto gameObject = reinterpret_cast<UnityResolve::UnityType::Transform*>(
                        proxy)->GetGameObject();
                    g_runtimeBoneHandles.emplace_back(UnityResolve::Invoke<Il2CppGCHandle>(
                        "il2cpp_gchandle_new", gameObject, false));
                }
                g_hybridBonesByRenderer[originalRenderer] = { ownerKey, proxyBones };
            }

            sourceRootBone = proxyBones[rootIndex];
            proxyTree = proxyBones;
            Log::WarnFmt("[ModAsset][EXPERIMENT] Built source proxy skeleton: %s renderer=%zu transforms=%zu skinBones=%zu semanticMatches=%zu semanticMap=%zu root=%s parent=%s; do not judge animation in this build",
                sourceName.c_str(), rendererIndex, proxyBones.size(), skinBones.size(),
                semanticMatches, options.sourceProxySemanticMap.size(),
                sidecarBones[rootIndex].name.c_str(), GetUnityObjectNameString(proxyParent).c_str());
            return makeRendererArray(proxyBones);
        }

        // The roadmap's step 3 set: hips, spine, neck, head, both arms, both legs.
        // Fingers are deliberately out — this rip's fingers rest 172-180 degrees round
        // from stock, so including them would make a bad frame impossible to attribute
        // between "the bridge is wrong" and "the fingers were always going to need
        // their own answer".
        bool IsMinimalBridgeSemantic(const std::string& semantic) {
            static const std::unordered_set<std::string> kMinimal = {
                "Hips", "Spine", "Spine1", "Spine2", "Neck", "Head",
                "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand",
                "RightShoulder", "RightArm", "RightForeArm", "RightHand",
                "LeftUpLeg", "LeftLeg", "LeftFoot",
                "RightUpLeg", "RightLeg", "RightFoot",
            };
            return kMinimal.contains(semantic);
        }

        UnityResolve::UnityType::Quaternion MultiplyQuaternion(
            const UnityResolve::UnityType::Quaternion& left,
            const UnityResolve::UnityType::Quaternion& right) {
            return {
                left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
                left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
                left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
                left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
            };
        }

        UnityResolve::UnityType::Vector3 RotateVectorByQuaternion(
            const UnityResolve::UnityType::Quaternion& rotation,
            const UnityResolve::UnityType::Vector3& value) {
            const auto tx = 2.0f * (rotation.y * value.z - rotation.z * value.y);
            const auto ty = 2.0f * (rotation.z * value.x - rotation.x * value.z);
            const auto tz = 2.0f * (rotation.x * value.y - rotation.y * value.x);
            return {
                value.x + rotation.w * tx + rotation.y * tz - rotation.z * ty,
                value.y + rotation.w * ty + rotation.z * tx - rotation.x * tz,
                value.z + rotation.w * tz + rotation.x * ty - rotation.y * tx,
            };
        }

        UnityResolve::UnityType::Quaternion InvertQuaternion(
            const UnityResolve::UnityType::Quaternion& value) {
            return { -value.x, -value.y, -value.z, value.w };
        }

        // Unity's Matrix4x4 is column-major, so this struct's m[a][b] holds Unity's
        // m[b][a] — each C++ row is a Unity column, which is exactly the basis vector
        // LookRotation wants.  Calling Unity's own converter here rather than
        // hand-rolling one keeps the handedness and the degenerate cases its problem.
        UnityResolve::UnityType::Quaternion RotationOfMatrix(
            const UnityResolve::UnityType::Matrix4x4& matrix) {
            static auto Quaternion_LookRotation = reinterpret_cast<UnityResolve::UnityType::Quaternion(*)(
                UnityResolve::UnityType::Vector3, UnityResolve::UnityType::Vector3)>(
                    Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine",
                        "Quaternion", "LookRotation", { "UnityEngine.Vector3", "UnityEngine.Vector3" }));
            if (!Quaternion_LookRotation) return { 0.0f, 0.0f, 0.0f, 1.0f };
            const auto normalize = [](float x, float y, float z) {
                const auto length = std::sqrt(x * x + y * y + z * z);
                return length > 1.0e-8f
                    ? UnityResolve::UnityType::Vector3(x / length, y / length, z / length)
                    : UnityResolve::UnityType::Vector3(0.0f, 0.0f, 0.0f);
            };
            const auto up = normalize(matrix.m[1][0], matrix.m[1][1], matrix.m[1][2]);
            const auto forward = normalize(matrix.m[2][0], matrix.m[2][1], matrix.m[2][2]);
            return Quaternion_LookRotation(forward, up);
        }

        // Retarget constant, derived once per bone.
        //
        //   proxyWorld(t) = gameWorld(t) * correction,  correction = gameRest^-1 * proxyRest
        //
        // Substituting gameWorld(t) = actor(t)*chainGame(t) and the two rests, both taken
        // in the SAME world frame, gives actor(t) * [chainGame(t)*chainGame(rest)^-1] *
        // chainSource(rest): the game bone's own change from its own rest, applied to the
        // source bone's own rest.  Bone lengths, joint positions and local axes never
        // enter it, which is the whole reason the source rig can keep its own proportions.
        //
        // The game bone's rest is read from the VANILLA bindposes, not from the live
        // transform: by the time a mod applies, the actor may already be posed, and
        // sampling a posed skeleton as "rest" bakes that pose into every frame after.
        size_t BuildSourceProxyAnimationBridge(void* originalRenderer,
            UnityArray<void*>* originalBones,
            UnityArray<UnityResolve::UnityType::Matrix4x4>* originalBindposes,
            const std::vector<void*>& proxyTree,
            const std::vector<LocalIpBone>& sidecarBones,
            const LocalIpSidecarOptions& options,
            const std::string& sourceName) {
            if (options.sourceProxyAnimationMode == 0) return 0;
            if (!originalBones || !originalBindposes || proxyTree.empty()) return 0;
            // A renderer gets patched more than once (the game re-applies on its own
            // lifecycle callbacks).  On every pass after the first, this renderer's bone
            // array is already OUR proxy array, so every vanilla-name lookup below would
            // miss and the arming would "fail" on a bridge that is in fact fine.  Keep the
            // one built from the real vanilla bones.
            {
                std::lock_guard swingStateLock(g_swingStateMutex);
                const auto existing = std::find_if(g_sourceProxyBridges.begin(),
                    g_sourceProxyBridges.end(),
                    [&](const SourceProxyBridge& item) { return item.renderer == originalRenderer; });
                if (existing != g_sourceProxyBridges.end() && !existing->links.empty()) {
                    Log::InfoFmt("[ModAsset][EXPERIMENT] Animation bridge already armed for this renderer, kept: %s driven=%zu",
                        sourceName.c_str(), existing->links.size());
                    return existing->links.size();
                }
            }
            if (originalBindposes->max_length != originalBones->max_length) {
                Log::ErrorFmt("[ModAsset][EXPERIMENT] Animation bridge needs matching vanilla bone/bindpose counts: %s bones=%zu bindposes=%zu",
                    sourceName.c_str(), static_cast<size_t>(originalBones->max_length),
                    static_cast<size_t>(originalBindposes->max_length));
                return 0;
            }
            const auto rendererTransform = GetComponentTransform(originalRenderer);
            if (!rendererTransform) return 0;
            const auto rendererRotation = reinterpret_cast<UnityResolve::UnityType::Transform*>(
                rendererTransform)->GetRotation();

            const auto gameBoneIndex = BuildBoneNameIndexMap(originalBones);
            // Ascending transform index means parents before children (the exporter writes
            // the tree depth-first).  Setting a parent's world rotation drags its children,
            // so a child processed first would be dragged back out of place.
            std::vector<std::pair<int, const std::string*>> ordered;
            for (const auto& [semantic, transformIndex] : options.sourceProxySemanticMap) {
                if (options.sourceProxyAnimationMode == 1 && !IsMinimalBridgeSemantic(semantic)) continue;
                ordered.emplace_back(transformIndex, &semantic);
            }
            std::sort(ordered.begin(), ordered.end(),
                [](const auto& left, const auto& right) { return left.first < right.first; });

            SourceProxyBridge bridge{ originalRenderer, sourceName, {} };
            std::string mapping;
            size_t unmatched = 0;
            size_t restDisagreements = 0;
            struct RestCandidate {
                const std::string* semantic;
                std::string proxyName;
                UnityResolve::UnityType::Quaternion transformRest;
                UnityResolve::UnityType::Quaternion bindposeRest;
                UnityResolve::UnityType::Quaternion proxyRest;
            };
            std::vector<RestCandidate> candidates;
            UnityResolve::UnityType::Transform* hipsGameBone = nullptr;
            UnityResolve::UnityType::Transform* hipsProxyBone = nullptr;
            UnityResolve::UnityType::Vector3 hipsBindposeRestLocal{};
            bool hipsBindposeRestKnown = false;
            for (const auto& [transformIndex, semantic] : ordered) {
                const auto found = gameBoneIndex.find(*semantic);
                if (found == gameBoneIndex.end()) {
                    ++unmatched;
                    Log::WarnFmt("[ModAsset][EXPERIMENT] Bridge semantic has no vanilla bone: %s semantic=%s",
                        sourceName.c_str(), semantic->c_str());
                    continue;
                }
                const auto proxyBone = reinterpret_cast<UnityResolve::UnityType::Transform*>(
                    proxyTree[static_cast<size_t>(transformIndex)]);
                const auto gameBone = reinterpret_cast<UnityResolve::UnityType::Transform*>(
                    originalBones->At(static_cast<unsigned int>(found->second)));
                if (!proxyBone || !gameBone) { ++unmatched; continue; }

                // Two candidate rests for the game bone, and neither is trustworthy alone:
                //
                //   transform  — the node rest, EXACTLY right when the skeleton is at rest,
                //                garbage when it is not.  The cold path patches the loaded
                //                prefab (never animated) so it is right there; the hot path
                //                (mod toggled ON) patches a LIVE actor mid-animation, and
                //                sampling that bakes the current pose into every later frame.
                //   bindpose   — a property of the asset, immune to pose, but measured 33.46
                //                degrees off on six bones of `atbm-cstm-0140` (both thumbs,
                //                all three joints), which is exactly a constant thumb offset.
                //
                // So decide per ARMING, not per bone: the two agree on 6/52 bones when the
                // skeleton is at rest and on ~52/52 when it is posed — the regimes are not
                // close, and the log carried the evidence both times before this existed.
                const auto transformRest = gameBone->GetRotation();
                auto bindposeRest = transformRest;
                if (originalBindposes) {
                    bindposeRest = MultiplyQuaternion(rendererRotation, InvertQuaternion(
                        RotationOfMatrix(originalBindposes->At(static_cast<unsigned int>(found->second)))));
                    const auto dot = std::fabs(bindposeRest.x * transformRest.x
                        + bindposeRest.y * transformRest.y + bindposeRest.z * transformRest.z
                        + bindposeRest.w * transformRest.w);
                    if (2.0f * std::acos(dot > 1.0f ? 1.0f : dot) * 57.2957795f > 1.0f) {
                        ++restDisagreements;
                    }
                }
                // The proxy was built moments ago and nothing drives it yet, so its rotation
                // is its rest — and it must come from the transform regardless, because a
                // semantic may map to an unweighted transform with no bindpose at all (this
                // rip's own `Hips` is exactly that).
                candidates.emplace_back(RestCandidate{
                    semantic, GetUnityObjectNameString(proxyBone),
                    transformRest, bindposeRest, proxyBone->GetRotation() });

                if (*semantic == "Hips") {
                    hipsGameBone = gameBone;
                    hipsProxyBone = proxyBone;
                    // The bindpose carries the rest POSITION as well as the rest rotation —
                    // `inverse(bindpose)` is the bone's rest transform in renderer space — so
                    // the posed case needs no special rule, just the same fallback the
                    // rotations already use.  Converted to the hips' parent frame here so the
                    // per-frame code can stay a plain local-position delta.
                    const auto parent = gameBone->GetParent();
                    if (originalBindposes && parent) {
                        // No general inverse needed: a bindpose is rigid, so its inverse has
                        // translation -R^T*t.  This struct's ROW is Unity's COLUMN (see
                        // RotationOfMatrix), which puts R at m[c][r] and t at m[3][0..2].
                        const auto& bindpose = originalBindposes->At(
                            static_cast<unsigned int>(found->second));
                        UnityResolve::UnityType::Vector3 restInRenderer{};
                        float* const axis[3] = { &restInRenderer.x, &restInRenderer.y, &restInRenderer.z };
                        for (int i = 0; i < 3; ++i) {
                            float sum = 0.0f;
                            for (int r = 0; r < 3; ++r) sum += bindpose.m[i][r] * bindpose.m[3][r];
                            *axis[i] = -sum;
                        }
                        hipsBindposeRestLocal = InverseTransformPoint(
                            parent, TransformPoint(rendererTransform, restInRenderer));
                        hipsBindposeRestKnown = true;
                    }
                }
                mapping += (mapping.empty() ? "" : ", ") + *semantic + "->"
                    + sidecarBones[static_cast<size_t>(transformIndex)].name;
            }

            // A quarter of the driven bones disagreeing is nowhere near either regime (6/52
            // at rest, 52/52 posed), so it separates them without pretending to be precise.
            const auto skeletonIsPosed = !candidates.empty()
                && restDisagreements * 4 > candidates.size();
            for (const auto& candidate : candidates) {
                bridge.links.emplace_back(SourceProxyBoneLink{
                    *candidate.semantic, candidate.proxyName,
                    MultiplyQuaternion(
                        InvertQuaternion(skeletonIsPosed
                            ? candidate.bindposeRest : candidate.transformRest),
                        candidate.proxyRest) });
            }
            if (bridge.links.empty()) {
                Log::ErrorFmt("[ModAsset][EXPERIMENT] Animation bridge resolved no bones: %s",
                    sourceName.c_str());
                return 0;
            }

            // Face and hair are separate parts riding the VANILLA `Head` bone, which the
            // Animator still parks at the vanilla head height — while the body's own head
            // is wherever the source rig's proportions put it.  Snapping that bone onto the
            // source head closes the gap.  Position only: rotation already agrees, because
            // gameWorld = proxyWorld * correction^-1 holds by construction, and leaving it
            // alone keeps the game's own nod/look-at corrections driving the face.
            for (const auto& link : bridge.links) {
                if (link.gameBoneName != "Head") continue;
                bridge.headGameName = link.gameBoneName;
                bridge.headProxyName = link.proxyBoneName;
                break;
            }
            // A declared socket wins over the head joint itself: a rip whose head bone sits
            // somewhere unhelpful can name the transform the game's head should ride.
            if (!bridge.headGameName.empty()
                && options.sourceProxyHeadSocket >= 0
                && static_cast<size_t>(options.sourceProxyHeadSocket) < proxyTree.size()) {
                bridge.headProxyName = GetUnityObjectNameString(
                    proxyTree[static_cast<size_t>(options.sourceProxyHeadSocket)]);
            }
            const auto headMapping = bridge.headGameName.empty()
                ? std::string("none") : bridge.headGameName + "->" + bridge.headProxyName;

            // Locomotion, as a delta from each rig's own rest — same shape as the rotations,
            // and the posed case takes the same fallback rather than a rule of its own: a
            // bindpose carries the rest position too, so there is nothing here that the
            // rotation path did not already have to solve.
            if (hipsGameBone && hipsProxyBone && (!skeletonIsPosed || hipsBindposeRestKnown)) {
                const auto gameRestLocal = skeletonIsPosed
                    ? hipsBindposeRestLocal : hipsGameBone->GetLocalPosition();
                const auto proxyRestLocal = hipsProxyBone->GetLocalPosition();
                bridge.hipsGameName = "Hips";
                bridge.hipsProxyName = GetUnityObjectNameString(hipsProxyBone);
                bridge.hipsRestDelta = UnityResolve::UnityType::Vector3(
                    proxyRestLocal.x - gameRestLocal.x,
                    proxyRestLocal.y - gameRestLocal.y,
                    proxyRestLocal.z - gameRestLocal.z);
            }

            const auto driven = bridge.links.size();
            {
                std::lock_guard swingStateLock(g_swingStateMutex);
                std::erase_if(g_sourceProxyBridges,
                    [&](const SourceProxyBridge& item) { return item.renderer == originalRenderer; });
                g_sourceProxyBridges.emplace_back(std::move(bridge));
                g_sourceProxyBridgesArmed.store(true, std::memory_order_relaxed);
                // Actors that already decided they had no bridge get to look again.
                g_sourceProxyLiveBridges.clear();
            }
            Log::WarnFmt("[ModAsset][EXPERIMENT] Animation bridge armed: %s mode=%d driven=%zu unmatched=%zu restDisagreements=%zu restSource=%s hipTranslation=%d head=%s physics=0 map=[%s]",
                sourceName.c_str(), options.sourceProxyAnimationMode, driven, unmatched,
                restDisagreements, skeletonIsPosed ? "bindpose(actor was posed)" : "transform",
                bridge.hipsGameName.empty() ? 0 : 1,
                headMapping.c_str(), mapping.c_str());
            return driven;
        }

        // Resolve every armed bridge against one live actor's own transforms.  Called once
        // per actor and cached; the walk is the expensive part, the lookups are not.
        SourceProxyLiveBridge BindSourceProxyBridgesToActor(void* actor) {
            SourceProxyLiveBridge live;
            const auto transformClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
            if (!transformClass) return live;

            // ponytail: first name wins.  Proxy names are unique by construction, and
            // within one actor the humanoid names only exist on the body part's skeleton.
            std::unordered_map<std::string, UnityResolve::UnityType::Transform*> byName;
            // A duplicate `__gmi_sp_*` name means an orphaned proxy skeleton is still in the
            // hierarchy, and first-wins would silently bind the bridge to the copy nobody
            // renders.  Counted, not tolerated quietly.
            size_t duplicateProxyNames = 0;
            const auto collect = [&](void* rootObject) {
                byName.clear();
                duplicateProxyNames = 0;
                if (!rootObject) return;
                for (const auto transform :
                    reinterpret_cast<UnityResolve::UnityType::GameObject*>(rootObject)
                        ->GetComponentsInChildren<void*>(transformClass, true)) {
                    auto name = GetUnityObjectNameString(transform);
                    const auto isProxy = name.starts_with("__gmi_sp_");
                    if (!byName.emplace(std::move(name),
                            reinterpret_cast<UnityResolve::UnityType::Transform*>(transform)).second
                        && isProxy) {
                        ++duplicateProxyNames;
                    }
                }
            };
            const auto proxiesIn = [&] {
                size_t found = 0;
                for (const auto& bridge : g_sourceProxyBridges) {
                    for (const auto& link : bridge.links) {
                        if (byName.contains(link.proxyBoneName)) ++found;
                    }
                }
                return found;
            };

            const auto actorTransform = GetComponentTransform(actor);
            collect(actorTransform ? actorTransform->GetGameObject() : nullptr);
            const char* scope = "actor";
            auto proxiesFound = proxiesIn();
            if (proxiesFound == 0) {
                // The parts may hang above the controller rather than under it; widening
                // once (and saying which scope answered) beats a silent nothing.
                collect(GetHierarchyRootGameObject(actor));
                scope = "root";
                proxiesFound = proxiesIn();
            }
            if (proxiesFound == 0) return live;   // not a modded actor

            size_t expected = 0;
            for (const auto& bridge : g_sourceProxyBridges) {
                for (const auto& link : bridge.links) {
                    ++expected;
                    const auto proxy = byName.find(link.proxyBoneName);
                    const auto game = byName.find(link.gameBoneName);
                    if (proxy == byName.end() || game == byName.end()) continue;
                    live.links.emplace_back(SourceProxyLiveLink{
                        game->second, proxy->second, link.correction });
                }
                if (!bridge.hipsGameName.empty() && !live.hipsGameBone) {
                    const auto hipsProxy = byName.find(bridge.hipsProxyName);
                    const auto hipsGame = byName.find(bridge.hipsGameName);
                    if (hipsProxy != byName.end() && hipsGame != byName.end()) {
                        live.hipsGameBone = hipsGame->second;
                        live.hipsProxyBone = hipsProxy->second;
                        live.hipsRestDelta = bridge.hipsRestDelta;
                    }
                }
                if (bridge.headGameName.empty() || live.headGameBone) continue;
                const auto headProxy = byName.find(bridge.headProxyName);
                const auto headGame = byName.find(bridge.headGameName);
                if (headProxy == byName.end() || headGame == byName.end()) continue;
                live.headGameBone = headGame->second;
                live.headProxyBone = headProxy->second;
            }
            Log::WarnFmt("[ModAsset][EXPERIMENT] Animation bridge bound to live actor: actor=%p scope=%s transforms=%zu driven=%zu proxies=%zu expected=%zu head=%d duplicateProxyNames=%zu",
                actor, scope, byName.size(), live.links.size(), proxiesFound, expected,
                live.headGameBone ? 1 : 0, duplicateProxyNames);
            if (duplicateProxyNames) {
                Log::ErrorFmt("[ModAsset][EXPERIMENT] Actor carries an orphaned proxy skeleton: actor=%p duplicateProxyNames=%zu — the bridge may be driving bones nothing renders",
                    actor, duplicateProxyNames);
            }
            return live;
        }

        // Runs from the actor's LateUpdate, after the game's own animation, IK and
        // corrections have written the human bones for this frame.
        void DriveSourceProxyBridges(void* actor) {
            if (!g_sourceProxyBridgesArmed.load(std::memory_order_relaxed)) return;
            std::lock_guard swingStateLock(g_swingStateMutex);
            // An unloaded bundle leaves its template behind; the prefab renderer it was
            // keyed by is gone and a new asset can land on that address.
            std::erase_if(g_sourceProxyBridges, [](const SourceProxyBridge& bridge) {
                return !IsNativeObjectAlive(bridge.renderer);
            });
            g_sourceProxyBridgesArmed.store(!g_sourceProxyBridges.empty(),
                std::memory_order_relaxed);
            if (!actor || g_sourceProxyBridges.empty()) return;

            auto found = g_sourceProxyLiveBridges.find(actor);
            if (found == g_sourceProxyLiveBridges.end()) {
                found = g_sourceProxyLiveBridges.emplace(
                    actor, BindSourceProxyBridgesToActor(actor)).first;
            }
            auto& live = found->second;
            if (live.links.empty()) return;
            // A destroyed actor hands its address to the next one ([N] gets reused), so
            // writing through a stale binding is a use-after-free.
            if (!IsNativeObjectAlive(live.links.front().proxyBone)) {
                g_sourceProxyLiveBridges.erase(found);
                return;
            }
            // Locomotion first: this moves the whole proxy chain, and the rotations written
            // below are absolute world rotations, so they do not care when it happens — but
            // the head snap at the end reads the proxy head's POSITION, which does.
            if (live.hipsGameBone && live.hipsProxyBone) {
                const auto gameLocal = live.hipsGameBone->GetLocalPosition();
                live.hipsProxyBone->SetLocalPosition(UnityResolve::UnityType::Vector3(
                    gameLocal.x + live.hipsRestDelta.x,
                    gameLocal.y + live.hipsRestDelta.y,
                    gameLocal.z + live.hipsRestDelta.z));
            }
            for (auto& link : live.links) {
                link.proxyBone->SetRotation(
                    MultiplyQuaternion(link.gameBone->GetRotation(), link.correction));
            }
            // After the whole chain is written: the proxy head's position is only correct
            // once every ancestor rotation for this frame has landed.  Absolute target,
            // not a delta, so re-running it on a bone the Animator never rewrites is a
            // no-op rather than a drift.
            if (live.headGameBone && live.headProxyBone) {
                live.headGameBone->SetPosition(live.headProxyBone->GetPosition());
            }
        }

        UnityArray<void*>* BuildHybridBoneArray(void* originalRenderer,
            UnityArray<void*>* originalBones,
            UnityArray<void*>* modBones,
            const std::vector<LocalIpBone>& sidecarBones,
            const std::vector<LocalIpExtraBone>& extraBones,
            const std::vector<LocalIpSwingChain>& swingChains,
            const std::string& sourceName,
            const std::string& modId,
            const std::string& sidecarFingerprint,
            const size_t rendererIndex,
            size_t& matchedBones,
            size_t& createdBones,
            std::vector<void*>& createdDynamicBones) {
            if (!originalRenderer || !originalBones || !modBones || sidecarBones.size() != modBones->max_length) return nullptr;
            // 这根骨属于谁：同一个 mod + 同一份 sidecar + 同一个 source 才算自己人。
            const auto ownerKey = modId + "|" + sidecarFingerprint + "|" + sourceName;
            // 锁只圈住共享容器本身。建 GameObject / AddComponent / 建链都是托管调用，
            // 会走回我们自己的 hook —— 在锁里做那些事既堵热路径又有重入死锁的风险。
            std::vector<void*> cachedBones;
            std::vector<void*> candidateBones;
            {
                std::lock_guard swingStateLock(g_swingStateMutex);
                if (const auto cached = g_hybridBonesByRenderer.find(originalRenderer);
                    cached != g_hybridBonesByRenderer.end()
                    && cached->second.ownerKey == ownerKey) {
                    candidateBones = cached->second.bones;
                }
            }
            const auto cacheIsAlive = !candidateBones.empty()
                && std::all_of(candidateBones.begin(), candidateBones.end(),
                    [](void* bone) { return bone && IsNativeObjectAlive(bone); });
            if (cacheIsAlive) {
                cachedBones = candidateBones;
            }
            else if (!candidateBones.empty()) {
                std::lock_guard swingStateLock(g_swingStateMutex);
                if (const auto cached = g_hybridBonesByRenderer.find(originalRenderer);
                    cached != g_hybridBonesByRenderer.end()
                    && cached->second.ownerKey == ownerKey
                    && cached->second.bones == candidateBones) {
                    g_hybridBonesByRenderer.erase(cached);
                }
            }

            const auto transformClass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
            const auto gameObjectClass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
            if (!transformClass || !gameObjectClass) return nullptr;
            const auto originalBoneIndexMap = BuildBoneNameIndexMap(originalBones);

            if (!cachedBones.empty()) {
                auto result = UnityArray<void*>::New(transformClass, cachedBones.size());
                for (size_t i = 0; i < cachedBones.size(); ++i) result->At(static_cast<unsigned int>(i)) = cachedBones[i];
                matchedBones = 0;
                for (const auto& bone : sidecarBones) if (originalBoneIndexMap.contains(bone.name)) ++matchedBones;
                createdBones = sidecarBones.size() - matchedBones;
                return result;
            }

            std::unordered_set<std::string> ownNames;
            {
                std::lock_guard swingStateLock(g_swingStateMutex);
                if (const auto owned = g_createdBonesByOwner.find(ownerKey); owned != g_createdBonesByOwner.end()) {
                    ownNames = owned->second;
                }
            }
            std::unordered_map<std::string, UnityResolve::UnityType::Transform*> existingCreatedBones;
            if (const auto hierarchyRoot = GetHierarchyRootGameObject(originalRenderer)) {
                const auto hierarchyTransforms = reinterpret_cast<UnityResolve::UnityType::GameObject*>(
                    hierarchyRoot)->GetComponentsInChildren<void*>(transformClass, true);
                for (const auto item : hierarchyTransforms) {
                    // 死对象不能复用：mod 关掉/换场景后名字还在记录里，Transform 已经销毁，
                    // 复用它等于把新网格蒙到一根不存在的骨上。
                    if (!item || !IsNativeObjectAlive(item)) continue;
                    const auto name = GetUnityObjectNameString(item);
                    // 只复用**本归属自己**建过的骨；别的 mod/别的构建的同名骨要另建一根
                    if (!ownNames.contains(name)) continue;
                    existingCreatedBones.emplace(
                        name,
                        reinterpret_cast<UnityResolve::UnityType::Transform*>(item));
                }
            }
            const auto hips = originalBoneIndexMap.find("Hips");
            size_t fallbackParentIndex = 0;
            if (hips != originalBoneIndexMap.end()) {
                fallbackParentIndex = hips->second;
            }
            else {
                const auto root = std::find_if(sidecarBones.begin(), sidecarBones.end(),
                    [&](const LocalIpBone& bone) {
                        return bone.parentIndex < 0 && originalBoneIndexMap.contains(bone.name);
                    });
                if (root != sidecarBones.end()) {
                    fallbackParentIndex = originalBoneIndexMap.at(root->name);
                }
                else if (originalBones->max_length == 0) {
                    Log::ErrorFmt("[ModAsset] Cannot build IP skeleton: original renderer has no root bone: %s renderer=%zu",
                        sourceName.c_str(), rendererIndex);
                    return nullptr;
                }
            }

            // 驱动器 setting 里的骨引用（Skirt/Rotation 的 `referenceBone`、Waist 的两个 offset 骨）
            // 按名字解到**原版活体骨架**上 —— 原版自己就是这么接的（`referenceWaistOffsetBone`
            // 指向的是身体骨）。找不到就返回空，AttachQuartzDriver 会记一条日志而不是静默带着
            // 空引用跑（那样只会在游戏里表现成"这块布不动"，谁也查不出为什么）。
            const auto resolveDriverBone =
                [&](const std::string& boneName) -> UnityResolve::UnityType::GameObject* {
                const auto found = originalBoneIndexMap.find(boneName);
                if (found == originalBoneIndexMap.end() || !originalBones) return nullptr;
                const auto item = originalBones->At(static_cast<unsigned int>(found->second));
                if (!item) return nullptr;
                return reinterpret_cast<UnityResolve::UnityType::Transform*>(item)->GetGameObject();
            };

            const auto createBone = [&](const std::string& name, UnityResolve::UnityType::Transform* parent,
                const UnityResolve::UnityType::Vector3& localPosition,
                const UnityResolve::UnityType::Quaternion& localRotation,
                const UnityResolve::UnityType::Vector3& localScale,
                const std::optional<LocalIpBoneSwing>& swing,
                const std::optional<LocalQuartzDriver>& driver) -> UnityResolve::UnityType::Transform* {
                auto gameObject = gameObjectClass->New<UnityResolve::UnityType::GameObject>();
                if (!gameObject) return nullptr;
                UnityResolve::UnityType::GameObject::Create(gameObject, name);
                auto transform = gameObject->GetTransform();
                if (!transform || !SetTransformParent(transform, parent)) return nullptr;
                transform->SetLocalPosition(localPosition);
                transform->SetLocalRotation(localRotation);
                transform->SetLocalScale(localScale);
                // 摇物和姿势驱动器**二选一**：原版 530 套里 327 个裙摆驱动器与 ActorSwing 组件
                // 零重叠，两个求解器同帧写一根骨没有先例（INV-1）。声明了 driver 就不挂摇物。
                if (driver) {
                    // 挂不上就是**这根骨没有任何求解器**（不静默替换成摇物：驱动器与摇物二选一，
                    // 偷偷换一个求解器等于给作者一个"能动但不是他配的"的结果）。日志要说清。
                    if (!AttachQuartzDriver(gameObject, *driver, resolveDriverBone)) {
                        Log::ErrorFmt("[ModAsset] %s 的驱动器没挂上，这根骨在游戏里不会动"
                            "（没有替换成摇物：两者二选一）", name.c_str());
                    }
                    const auto driverHandle = UnityResolve::Invoke<Il2CppGCHandle>(
                        "il2cpp_gchandle_new", gameObject, false);
                    {
                        std::lock_guard swingStateLock(g_swingStateMutex);
                        g_runtimeBoneHandles.emplace_back(driverHandle);
                    }
                    return transform;
                }
                void* dynamicBone = nullptr;
                if (const auto dynamicBoneClass = FindClassByName("ActorSwingDynamicBone")) {
                    const auto component = AddComponentByClass(gameObject, dynamicBoneClass);
                    if (component && InitializeActorSwingDynamicBone(component, dynamicBoneClass, swing)) {
                        dynamicBone = component;
                    }
                    else if (component) {
                        // 初始化没成的组件必须撤掉：它照样被 Instantiate、照样 OnEnable，
                        // 然后带着 SetDefaultValues 都没跑完的状态参与解算，而我们这边
                        // 又没把它记进 g_createdActorSwingBoneNames —— 后续清理也找不到它。
                        Log::ErrorFmt("[ModAsset] %s 的摇物组件初始化失败，已撤掉（不留半成品）",
                            name.c_str());
                        DestroyComponentImmediate(component);
                    }
                }
                const auto handle = UnityResolve::Invoke<Il2CppGCHandle>(
                    "il2cpp_gchandle_new", gameObject, false);
                {
                    std::lock_guard swingStateLock(g_swingStateMutex);
                    if (dynamicBone) {
                        g_createdActorSwingBoneNames.emplace(name);
                        g_createdBonesByOwner[ownerKey].emplace(name);
                    }
                    g_runtimeBoneHandles.emplace_back(handle);
                }
                if (dynamicBone) createdDynamicBones.emplace_back(dynamicBone);
                return transform;
            };

            std::vector<void*> hybridBones(sidecarBones.size());
            std::vector<unsigned char> states(sidecarBones.size());
            std::function<bool(size_t)> buildBone;
            buildBone = [&](const size_t index) {
                if (index >= sidecarBones.size()) return false;
                if (states[index] == 2) return true;
                if (states[index] == 1) return false;
                states[index] = 1;

                const auto& sidecarBone = sidecarBones[index];
                if (const auto original = originalBoneIndexMap.find(sidecarBone.name); original != originalBoneIndexMap.end()) {
                    hybridBones[index] = originalBones->At(static_cast<unsigned int>(original->second));
                    ++matchedBones;
                }
                else if (const auto existing = existingCreatedBones.find(sidecarBone.name);
                    existing != existingCreatedBones.end()) {
                    hybridBones[index] = existing->second;
                    ++createdBones;
                }
                else {
                    auto parent = reinterpret_cast<UnityResolve::UnityType::Transform*>(
                        originalBones->At(static_cast<unsigned int>(fallbackParentIndex)));
                    if (sidecarBone.parentIndex >= 0) {
                        if (static_cast<size_t>(sidecarBone.parentIndex) >= sidecarBones.size()
                            || !buildBone(static_cast<size_t>(sidecarBone.parentIndex))) return false;
                        parent = reinterpret_cast<UnityResolve::UnityType::Transform*>(hybridBones[static_cast<size_t>(sidecarBone.parentIndex)]);
                    }

                    const auto transform = createBone(sidecarBone.name, parent, sidecarBone.localPosition,
                        sidecarBone.localRotation, sidecarBone.localScale, sidecarBone.swing,
                        sidecarBone.driver);
                    if (!transform) return false;
                    hybridBones[index] = transform;
                    ++createdBones;
                }

                states[index] = 2;
                return hybridBones[index] != nullptr;
            };

            for (size_t i = 0; i < sidecarBones.size(); ++i) {
                if (!buildBone(i)) {
                    Log::ErrorFmt("[ModAsset] Cannot build IP skeleton hierarchy: %s renderer=%zu bone=%s index=%zu",
                        sourceName.c_str(), rendererIndex, sidecarBones[i].name.c_str(), i);
                    return nullptr;
                }
            }

            // Attach each chain's tip. These carry no skin weights so they stay out of
            // hybridBones, but the sim needs them: without a tip the last segment is
            // undefined and the chain diverges instead of swinging.
            size_t extraCreated = 0;
            std::unordered_map<std::string, UnityResolve::UnityType::Transform*> extraTransforms;
            for (const auto& extra : extraBones) {
                if (const auto existing = existingCreatedBones.find(extra.name);
                    existing != existingCreatedBones.end()) {
                    extraTransforms[extra.name] = existing->second;
                    continue;
                }
                const auto parent = std::find_if(sidecarBones.begin(), sidecarBones.end(),
                    [&](const LocalIpBone& bone) { return bone.name == extra.parentName; });
                UnityResolve::UnityType::Transform* parentTransform = nullptr;
                if (parent != sidecarBones.end()) {
                    parentTransform = reinterpret_cast<UnityResolve::UnityType::Transform*>(
                        hybridBones[static_cast<size_t>(std::distance(sidecarBones.begin(), parent))]);
                }
                else if (const auto created = extraTransforms.find(extra.parentName); created != extraTransforms.end()) {
                    parentTransform = created->second;
                }
                if (!parentTransform) {
                    Log::WarnFmt("[ModAsset] Chain tip has no parent in the mesh skeleton, skipped: %s parent=%s",
                        extra.name.c_str(), extra.parentName.c_str());
                    continue;
                }
                const auto createdTransform = parentTransform ? createBone(extra.name, parentTransform,
                    extra.localPosition, extra.localRotation, extra.localScale, extra.swing,
                    extra.driver) : nullptr;
                if (createdTransform) {
                    extraTransforms[extra.name] = createdTransform;
                    ++extraCreated;
                }
            }
            if (!extraBones.empty()) {
                Log::InfoFmt("[ModAsset] Chain tips attached: %zu/%zu renderer=%zu",
                    extraCreated, extraBones.size(), rendererIndex);
            }

            // 链在这里建：骨和 tip 都已就位，而 prefab 还没被 Instantiate。
            // 把本次产出的骨映射直接传下去，别让它再按名字全层级找一遍。
            auto graftedBones = extraTransforms;
            for (size_t i = 0; i < sidecarBones.size(); ++i) {
                graftedBones[sidecarBones[i].name] =
                    reinterpret_cast<UnityResolve::UnityType::Transform*>(hybridBones[i]);
            }
            AttachSwingChainsToGraftedSkeleton(originalRenderer, swingChains, graftedBones);

            {
                std::lock_guard swingStateLock(g_swingStateMutex);
                g_hybridBonesByRenderer[originalRenderer] = { ownerKey, hybridBones };
            }
            auto result = UnityArray<void*>::New(transformClass, hybridBones.size());
            for (size_t i = 0; i < hybridBones.size(); ++i) result->At(static_cast<unsigned int>(i)) = hybridBones[i];
            return result;
        }

        void UpdateMaxBoneIndex(int& currentMax, const int boneIndex) {
            if (boneIndex > currentMax) currentMax = boneIndex;
        }

        void AddBoneWeightStat(std::vector<double>& totals, const int boneIndex, const float weight) {
            if (weight <= 0.0f || boneIndex < 0 || static_cast<size_t>(boneIndex) >= totals.size()) return;
            totals[static_cast<size_t>(boneIndex)] += static_cast<double>(weight);
        }

        std::string FormatTopBoneWeightStats(const std::vector<double>& totals, UnityArray<void*>* bones, const size_t limit) {
            std::vector<size_t> indices;
            indices.reserve(totals.size());
            for (size_t i = 0; i < totals.size(); ++i) {
                if (totals[i] > 0.000001) indices.push_back(i);
            }

            std::sort(indices.begin(), indices.end(), [&](const size_t left, const size_t right) {
                return totals[left] > totals[right];
            });

            std::string result;
            const auto count = indices.size() < limit ? indices.size() : limit;
            for (size_t i = 0; i < count; ++i) {
                const auto boneIndex = indices[i];
                if (!result.empty()) result += ", ";
                const auto boneName = bones && boneIndex < bones->max_length
                    ? GetUnityObjectNameString(bones->At(static_cast<unsigned int>(boneIndex)))
                    : std::string{};
                result += boneName.empty() ? "#" + std::to_string(boneIndex) : boneName;
                result += ":";
                result += std::to_string(totals[boneIndex]);
            }
            return result;
        }

        void RemapBoneInfluence(int& boneIndex, float& weight, const int fallbackBoneIndex,
            const std::vector<int>& modToOriginalBoneIndex, size_t& remappedCount, size_t& droppedInfluences) {
            if (weight <= 0.0f) {
                boneIndex = fallbackBoneIndex;
                return;
            }

            if (boneIndex < 0 || static_cast<size_t>(boneIndex) >= modToOriginalBoneIndex.size()) {
                boneIndex = fallbackBoneIndex;
                weight = 0.0f;
                ++droppedInfluences;
                return;
            }

            const auto originalBoneIndex = modToOriginalBoneIndex[static_cast<size_t>(boneIndex)];
            if (originalBoneIndex < 0) {
                boneIndex = fallbackBoneIndex;
                weight = 0.0f;
                ++droppedInfluences;
                return;
            }

            if (originalBoneIndex != boneIndex) ++remappedCount;
            boneIndex = originalBoneIndex;
        }

        bool NormalizeBoneWeight(LocalModBoneWeight& weight, const int fallbackBoneIndex, size_t& fallbackVertices) {
            const auto total = weight.weight0 + weight.weight1 + weight.weight2 + weight.weight3;
            if (total > 0.000001f) {
                const auto invTotal = 1.0f / total;
                weight.weight0 *= invTotal;
                weight.weight1 *= invTotal;
                weight.weight2 *= invTotal;
                weight.weight3 *= invTotal;
                return true;
            }

            weight.weight0 = 1.0f;
            weight.weight1 = 0.0f;
            weight.weight2 = 0.0f;
            weight.weight3 = 0.0f;
            weight.boneIndex0 = fallbackBoneIndex;
            weight.boneIndex1 = fallbackBoneIndex;
            weight.boneIndex2 = fallbackBoneIndex;
            weight.boneIndex3 = fallbackBoneIndex;
            ++fallbackVertices;
            return false;
        }

        bool TransformModMeshVerticesToOriginalRendererSpace(void* originalRenderer, void* modRenderer, void* modMesh,
            const std::string& sourceName, const size_t rendererIndex) {
            if (!originalRenderer || !modRenderer || !modMesh) return false;
            {
                std::lock_guard meshLock(g_runtimeMeshHandleMutex);
                if (g_transformedMeshSet.contains(modMesh)) return true;
            }

            const auto originalTransform = GetComponentTransform(originalRenderer);
            const auto modTransform = GetComponentTransform(modRenderer);
            const auto vertices = GetMeshVertices(modMesh);
            if (!originalTransform || !modTransform || !vertices) {
                Log::ErrorFmt("[ModAsset] Cannot transform mod mesh vertices: %s renderer=%zu originalTransform=%p modTransform=%p vertices=%zu",
                    sourceName.c_str(),
                    rendererIndex,
                    originalTransform,
                    modTransform,
                    vertices ? static_cast<size_t>(vertices->max_length) : 0);
                return false;
            }

            for (std::uintptr_t i = 0; i < vertices->max_length; ++i) {
                const auto modLocal = vertices->At(static_cast<unsigned int>(i));
                const auto world = TransformPoint(modTransform, modLocal);
                vertices->At(static_cast<unsigned int>(i)) = InverseTransformPoint(originalTransform, world);
            }

            // Normals and tangents live in the same space as the positions, so
            // they have to make the same trip.  On the cold path the two
            // transforms are both prefab-identity and skipping this is
            // invisible; on a live actor the rotation is real, and leaving the
            // normals behind is what rendered a hot-applied costume dark
            // (frame analysis, 2026-08-09: face-normal agreement +0.97 on the
            // cold path, -0.23 on the hot path, with identical textures).
            size_t normalCount = 0;
            if (const auto normals = GetMeshNormals(modMesh)) {
                for (std::uintptr_t i = 0; i < normals->max_length; ++i) {
                    const auto world = TransformDirection(
                        modTransform, normals->At(static_cast<unsigned int>(i)));
                    normals->At(static_cast<unsigned int>(i)) =
                        InverseTransformDirection(originalTransform, world);
                }
                SetMeshNormals(modMesh, normals);
                normalCount = static_cast<size_t>(normals->max_length);
            }

            size_t tangentCount = 0;
            if (const auto tangents = GetMeshTangents(modMesh)) {
                for (std::uintptr_t i = 0; i < tangents->max_length; ++i) {
                    auto& tangent = tangents->At(static_cast<unsigned int>(i));
                    const UnityResolve::UnityType::Vector3 direction{
                        tangent.x, tangent.y, tangent.z };
                    const auto world = TransformDirection(modTransform, direction);
                    const auto local = InverseTransformDirection(originalTransform, world);
                    // w carries the bitangent sign, not a coordinate.
                    tangent = { local.x, local.y, local.z, tangent.w };
                }
                SetMeshTangents(modMesh, tangents);
                tangentCount = static_cast<size_t>(tangents->max_length);
            }

            SetMeshVertices(modMesh, vertices);
            RecalculateMeshBounds(modMesh);
            {
                std::lock_guard meshLock(g_runtimeMeshHandleMutex);
                g_transformedMeshSet.emplace(modMesh);
            }
            Log::InfoFmt("[ModAsset] Transformed mod mesh vertices to original renderer space: %s renderer=%zu vertices=%zu normals=%zu tangents=%zu originalRenderer=\"%s\" modRenderer=\"%s\"",
                sourceName.c_str(),
                rendererIndex,
                static_cast<size_t>(vertices->max_length),
                normalCount,
                tangentCount,
                GetUnityObjectNameString(originalRenderer).c_str(),
                GetUnityObjectNameString(modRenderer).c_str());
            return true;
        }

        bool PatchModMeshSkinningLosslessly(void* originalRenderer, void* modRenderer, void* originalMesh, void* modMesh,
            const LocalModAssetReplacement& replacement, const std::string& sourceName, const size_t rendererIndex) {
            const auto matrixClass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Matrix4x4");
            const auto originalBones = GetSkinnedMeshRendererBones(originalRenderer);
            const auto modBones = GetSkinnedMeshRendererBones(modRenderer);
            const auto modBindposes = GetMeshBindposes(modMesh);
            const auto modBoneWeights = GetMeshBoneWeights(modMesh);
            if (!matrixClass || !originalBones || !modBones || !modBindposes || !modBoneWeights) return false;

            if (modBindposes->max_length != modBones->max_length) {
                Log::ErrorFmt("[ModAsset] Lossless IP skeleton requires matching bone/bindpose counts: %s renderer=%zu modBones=%zu modBindposes=%zu",
                    sourceName.c_str(), rendererIndex,
                    static_cast<size_t>(modBones->max_length), static_cast<size_t>(modBindposes->max_length));
                return false;
            }

            std::vector<LocalIpBone> sidecarBones;
            std::vector<LocalIpExtraBone> extraSwingBones;
            std::vector<LocalIpSwingChain> swingChains;
            std::string sidecarFingerprint;
            LocalIpSidecarOptions sidecarOptions{};
            if (!LoadIpBoneSidecar(replacement, sidecarBones, extraSwingBones, swingChains,
                    sidecarFingerprint, sidecarOptions)) {
                return false;
            }
            // Protocol 2 with a full tree counts skin bones; everything else still has one
            // array doing both jobs.
            const auto declaredSkinBoneCount = sidecarOptions.sourceProxyHasTransforms
                ? sidecarOptions.sourceProxySkinBones.size() : sidecarBones.size();
            if (declaredSkinBoneCount != modBones->max_length) {
                Log::ErrorFmt("[ModAsset] Lossless IP skeleton sidecar count mismatch: %s renderer=%zu transforms=%zu skinBones=%zu modBones=%zu",
                    sourceName.c_str(), rendererIndex, sidecarBones.size(),
                    declaredSkinBoneCount, static_cast<size_t>(modBones->max_length));
                return false;
            }

            // The graft (BuildHybridBoneArray) builds the whole skeleton from the sidecar
            // JSON by name/order and creates missing bones live; it never reads the mod
            // SMR's bone names. Requiring modBones[i].name == sidecar[i].name only forced
            // the exporter to embed synthesized Transforms into the bundle, which Unity 6
            // native LoadAsset crashes on. Validate the sidecar hierarchy alone.
            // A protocol-2 transform tree is validated at parse time and may list a parent
            // after its child; only the protocol-1 array is required to be topological.
            for (size_t i = 0; i < sidecarBones.size() && !sidecarOptions.sourceProxyHasTransforms; ++i) {
                if (sidecarBones[i].parentIndex < -1
                    || sidecarBones[i].parentIndex >= static_cast<int>(i)) {
                    Log::ErrorFmt("[ModAsset] Lossless IP skeleton sidecar hierarchy invalid: %s renderer=%zu index=%zu sidecar=\"%s\" parent=%d",
                        sourceName.c_str(), rendererIndex, i, sidecarBones[i].name.c_str(), sidecarBones[i].parentIndex);
                    return false;
                }
            }

            for (std::uintptr_t i = 0; i < modBoneWeights->max_length; ++i) {
                const auto& weight = modBoneWeights->At(static_cast<unsigned int>(i));
                const auto validBoneIndex = [&](const int boneIndex) {
                    return boneIndex >= 0 && static_cast<std::uintptr_t>(boneIndex) < modBones->max_length;
                };
                if (!validBoneIndex(weight.boneIndex0) || !validBoneIndex(weight.boneIndex1)
                    || !validBoneIndex(weight.boneIndex2) || !validBoneIndex(weight.boneIndex3)) {
                    Log::ErrorFmt("[ModAsset] Lossless IP skeleton found an invalid source weight index: %s renderer=%zu vertex=%zu",
                        sourceName.c_str(), rendererIndex, static_cast<size_t>(i));
                    return false;
                }
            }

            size_t matchedBones = 0;
            size_t createdBones = 0;
            std::vector<void*> createdDynamicBones;
            void* sourceProxyRootBone = nullptr;
            std::vector<void*> sourceProxyTree;
            std::vector<void*> retiredProxyContainers;
            UnityArray<void*>* graftedBones = nullptr;
            if (sidecarOptions.sourceProxyRestOnly) {
                graftedBones = BuildSourceProxyBoneArray(
                    originalRenderer, originalBones, sidecarBones, sidecarOptions,
                    sourceName, replacement.modId, sidecarFingerprint, rendererIndex,
                    matchedBones, createdBones, sourceProxyRootBone, sourceProxyTree,
                    retiredProxyContainers);
                if (!extraSwingBones.empty() || !swingChains.empty()) {
                    Log::WarnFmt("[ModAsset][EXPERIMENT] Rest-only source proxy ignores physics metadata: %s renderer=%zu extraBones=%zu swingChains=%zu",
                        sourceName.c_str(), rendererIndex, extraSwingBones.size(), swingChains.size());
                }
            }
            else {
                graftedBones = BuildHybridBoneArray(
                    originalRenderer, originalBones, modBones, sidecarBones, extraSwingBones, swingChains,
                    sourceName, replacement.modId, sidecarFingerprint, rendererIndex,
                    matchedBones, createdBones, createdDynamicBones);
            }
            if (!graftedBones) return false;

            auto adjustedBindposes = UnityArray<UnityResolve::UnityType::Matrix4x4>::New(matrixClass, modBindposes->max_length);
            const auto bindposeSpaceAdjustment = GetBindposeRendererSpaceAdjustment(originalRenderer, modRenderer);
            for (std::uintptr_t i = 0; i < modBindposes->max_length; ++i) {
                adjustedBindposes->At(static_cast<unsigned int>(i)) = MultiplyMatrix4x4(
                    modBindposes->At(static_cast<unsigned int>(i)), bindposeSpaceAdjustment);
            }

            SetMeshBindposes(modMesh, adjustedBindposes);
            SetSkinnedMeshRendererBones(originalRenderer, graftedBones);
            if (sidecarOptions.sourceProxyRestOnly
                && (!sourceProxyRootBone
                    || !SetSkinnedMeshRendererRootBone(originalRenderer, sourceProxyRootBone))) {
                SetSkinnedMeshRendererBones(originalRenderer, originalBones);
                Log::ErrorFmt("[ModAsset][EXPERIMENT] Cannot assign source proxy rootBone; renderer bones restored: %s renderer=%zu",
                    sourceName.c_str(), rendererIndex);
                return false;
            }

            const auto originalRootName = GetUnityObjectNameString(GetSkinnedMeshRendererRootBone(originalRenderer));
            const auto modRootName = GetUnityObjectNameString(GetSkinnedMeshRendererRootBone(modRenderer));
            // The normal lossless graft reuses live game Transforms, so matching renderer
            // roots remain a necessary guard there.  Protocol 2 deliberately creates the
            // complete source hierarchy from the sidecar and assigns its declared root;
            // applying the old same-name guard before selecting that route rejects exactly
            // the external rigs this path exists to preserve.
            if (!sidecarOptions.sourceProxyRestOnly
                && (originalRootName.empty() || originalRootName != modRootName)) {
                Log::ErrorFmt("[ModAsset] Lossless IP skeleton requires matching roots: %s renderer=%zu originalRoot=\"%s\" modRoot=\"%s\"",
                    sourceName.c_str(), rendererIndex,
                    originalRootName.c_str(), modRootName.c_str());
                return false;
            }
            RecalculateMeshBounds(modMesh);
            if (sidecarOptions.sourceProxyRestOnly && !retiredProxyContainers.empty()) {
                // Only now is the previous container genuinely unused: the renderer's bone
                // array and rootBone above already point at the new tree.  Retiring it any
                // earlier is what crashed the game inside its own LateUpdate on mod OFF→ON.
                DestroyRetiredSourceProxyContainers(retiredProxyContainers);
                Log::WarnFmt("[ModAsset][EXPERIMENT] Retired previous source proxy containers: %s renderer=%zu containers=%zu",
                    sourceName.c_str(), rendererIndex, retiredProxyContainers.size());
            }
            if (sidecarOptions.sourceProxyRestOnly) {
                // Armed after the renderer is fully swapped: a bridge whose renderer never
                // took the mesh would drive bones nothing renders.
                const auto driven = BuildSourceProxyAnimationBridge(
                    originalRenderer, originalBones, GetMeshBindposes(originalMesh),
                    sourceProxyTree, sidecarBones, sidecarOptions, sourceName);
                Log::WarnFmt("[ModAsset][EXPERIMENT] Applied source-proxy skinning: %s renderer=%zu semanticMatches=%zu createdBones=%zu bones=%zu boneWeights=%zu sourceWeights=1 sourceBindposes=1 animationBridge=%zu physics=0",
                    sourceName.c_str(), rendererIndex, matchedBones, createdBones,
                    static_cast<size_t>(graftedBones->max_length),
                    static_cast<size_t>(modBoneWeights->max_length), driven);
            }
            else {
                Log::InfoFmt("[ModAsset] Applied lossless IP skeleton graft: %s renderer=%zu matchedBones=%zu createdBones=%zu bones=%zu boneWeights=%zu swingPrepared=%zu droppedInfluences=0 fallbackVertices=0",
                    sourceName.c_str(), rendererIndex, matchedBones, createdBones,
                    static_cast<size_t>(graftedBones->max_length), static_cast<size_t>(modBoneWeights->max_length),
                    createdDynamicBones.size());
            }
            return true;
        }

        bool PatchModMeshSkinningToOriginalOrder(void* originalRenderer, void* modRenderer, void* originalMesh, void* modMesh,
            const LocalModAssetReplacement& replacement, const std::string& sourceName, const size_t rendererIndex) {
            if (!replacement.skeletonAssetName.empty()) {
                return PatchModMeshSkinningLosslessly(
                    originalRenderer, modRenderer, originalMesh, modMesh, replacement, sourceName, rendererIndex);
            }
            const auto boneWeightClass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "BoneWeight");
            const auto matrixClass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Matrix4x4");
            if (!boneWeightClass || !matrixClass) return false;

            const auto originalBones = GetSkinnedMeshRendererBones(originalRenderer);
            const auto modBones = GetSkinnedMeshRendererBones(modRenderer);
            const auto originalBindposes = GetMeshBindposes(originalMesh);
            const auto modBindposes = GetMeshBindposes(modMesh);
            const auto modBoneWeights = GetMeshBoneWeights(modMesh);
            if (!originalBones || !modBones || !originalBindposes || !modBindposes || !modBoneWeights) {
                Log::ErrorFmt("[ModAsset] Cannot patch skinning to original order: %s renderer=%zu originalBones=%zu modBones=%zu originalBindposes=%zu modBindposes=%zu modBoneWeights=%zu",
                    sourceName.c_str(),
                    rendererIndex,
                    originalBones ? static_cast<size_t>(originalBones->max_length) : 0,
                    modBones ? static_cast<size_t>(modBones->max_length) : 0,
                    originalBindposes ? static_cast<size_t>(originalBindposes->max_length) : 0,
                    modBindposes ? static_cast<size_t>(modBindposes->max_length) : 0,
                    modBoneWeights ? static_cast<size_t>(modBoneWeights->max_length) : 0);
                return false;
            }

            const auto originalBoneIndexMap = BuildBoneNameIndexMap(originalBones);
            std::vector<int> modToOriginalBoneIndex(modBones->max_length, -1);
            size_t matchedBones = 0;
            for (std::uintptr_t i = 0; i < modBones->max_length; ++i) {
                const auto modBoneName = GetUnityObjectNameString(modBones->At(static_cast<unsigned int>(i)));
                if (const auto iter = originalBoneIndexMap.find(modBoneName); iter != originalBoneIndexMap.end()) {
                    modToOriginalBoneIndex[static_cast<size_t>(i)] = static_cast<int>(iter->second);
                    ++matchedBones;
                }
            }

            int fallbackBoneIndex = 0;
            if (const auto iter = originalBoneIndexMap.find("Hips"); iter != originalBoneIndexMap.end()) {
                fallbackBoneIndex = static_cast<int>(iter->second);
            }

            auto remappedBoneWeights = UnityArray<LocalModBoneWeight>::New(boneWeightClass, modBoneWeights->max_length);
            auto remappedBindposes = UnityArray<UnityResolve::UnityType::Matrix4x4>::New(matrixClass, originalBones->max_length);
            size_t remappedIndices = 0;
            size_t remappedBindposeCount = 0;
            size_t droppedInfluences = 0;
            size_t fallbackVertices = 0;
            int maxOriginalBoneIndex = -1;
            int maxModBoneIndex = -1;
            std::vector<double> modBoneWeightTotals(modBones->max_length, 0.0);
            std::vector<double> originalBoneWeightTotals(originalBones->max_length, 0.0);
            const auto originalRootName = GetUnityObjectNameString(GetSkinnedMeshRendererRootBone(originalRenderer));
            const auto modRootName = GetUnityObjectNameString(GetSkinnedMeshRendererRootBone(modRenderer));
            const auto useModBindposes = !originalRootName.empty() && originalRootName == modRootName && modBindposes->max_length > 0;
            const auto bindposeMode = useModBindposes ? "mod-remapped" : "original";
            const auto bindposeSpaceAdjustment = GetBindposeRendererSpaceAdjustment(originalRenderer, modRenderer);

            for (std::uintptr_t i = 0; i < originalBones->max_length; ++i) {
                remappedBindposes->At(static_cast<unsigned int>(i)) =
                    i < originalBindposes->max_length
                        ? originalBindposes->At(static_cast<unsigned int>(i))
                        : UnityResolve::UnityType::Matrix4x4{};
                if (i < originalBindposes->max_length) ++remappedBindposeCount;
            }

            if (useModBindposes) {
                remappedBindposeCount = 0;
                for (std::uintptr_t i = 0; i < modBones->max_length && i < modBindposes->max_length; ++i) {
                    const auto originalBoneIndex = modToOriginalBoneIndex[static_cast<size_t>(i)];
                    if (originalBoneIndex >= 0 && static_cast<std::uintptr_t>(originalBoneIndex) < remappedBindposes->max_length) {
                        remappedBindposes->At(static_cast<unsigned int>(originalBoneIndex)) =
                            MultiplyMatrix4x4(modBindposes->At(static_cast<unsigned int>(i)), bindposeSpaceAdjustment);
                        ++remappedBindposeCount;
                    }
                }
            }

            for (std::uintptr_t i = 0; i < modBoneWeights->max_length; ++i) {
                auto weight = modBoneWeights->At(static_cast<unsigned int>(i));
                UpdateMaxBoneIndex(maxModBoneIndex, weight.boneIndex0);
                UpdateMaxBoneIndex(maxModBoneIndex, weight.boneIndex1);
                UpdateMaxBoneIndex(maxModBoneIndex, weight.boneIndex2);
                UpdateMaxBoneIndex(maxModBoneIndex, weight.boneIndex3);
                AddBoneWeightStat(modBoneWeightTotals, weight.boneIndex0, weight.weight0);
                AddBoneWeightStat(modBoneWeightTotals, weight.boneIndex1, weight.weight1);
                AddBoneWeightStat(modBoneWeightTotals, weight.boneIndex2, weight.weight2);
                AddBoneWeightStat(modBoneWeightTotals, weight.boneIndex3, weight.weight3);

                RemapBoneInfluence(weight.boneIndex0, weight.weight0, fallbackBoneIndex, modToOriginalBoneIndex, remappedIndices, droppedInfluences);
                RemapBoneInfluence(weight.boneIndex1, weight.weight1, fallbackBoneIndex, modToOriginalBoneIndex, remappedIndices, droppedInfluences);
                RemapBoneInfluence(weight.boneIndex2, weight.weight2, fallbackBoneIndex, modToOriginalBoneIndex, remappedIndices, droppedInfluences);
                RemapBoneInfluence(weight.boneIndex3, weight.weight3, fallbackBoneIndex, modToOriginalBoneIndex, remappedIndices, droppedInfluences);
                NormalizeBoneWeight(weight, fallbackBoneIndex, fallbackVertices);

                UpdateMaxBoneIndex(maxOriginalBoneIndex, weight.boneIndex0);
                UpdateMaxBoneIndex(maxOriginalBoneIndex, weight.boneIndex1);
                UpdateMaxBoneIndex(maxOriginalBoneIndex, weight.boneIndex2);
                UpdateMaxBoneIndex(maxOriginalBoneIndex, weight.boneIndex3);
                AddBoneWeightStat(originalBoneWeightTotals, weight.boneIndex0, weight.weight0);
                AddBoneWeightStat(originalBoneWeightTotals, weight.boneIndex1, weight.weight1);
                AddBoneWeightStat(originalBoneWeightTotals, weight.boneIndex2, weight.weight2);
                AddBoneWeightStat(originalBoneWeightTotals, weight.boneIndex3, weight.weight3);
                remappedBoneWeights->At(static_cast<unsigned int>(i)) = weight;
            }

            SetMeshBindposes(modMesh, remappedBindposes);
            SetMeshBoneWeights(modMesh, remappedBoneWeights);
            RecalculateMeshBounds(modMesh);
            Log::InfoFmt("[ModAsset] Patched mod mesh skinning to original order: %s renderer=%zu matchedBones=%zu originalBones=%zu modBones=%zu boneWeights=%zu remappedIndices=%zu remappedBindposes=%zu bindposeMode=%s originalRoot=\"%s\" modRoot=\"%s\" droppedInfluences=%zu fallbackVertices=%zu fallbackBoneIndex=%d maxModBoneIndex=%d maxOriginalBoneIndex=%d bindposes=%zu",
                sourceName.c_str(),
                rendererIndex,
                matchedBones,
                static_cast<size_t>(originalBones->max_length),
                static_cast<size_t>(modBones->max_length),
                static_cast<size_t>(modBoneWeights->max_length),
                remappedIndices,
                remappedBindposeCount,
                bindposeMode,
                originalRootName.c_str(),
                modRootName.c_str(),
                droppedInfluences,
                fallbackVertices,
                fallbackBoneIndex,
                maxModBoneIndex,
                maxOriginalBoneIndex,
                static_cast<size_t>(remappedBindposes->max_length));
            Log::InfoFmt("[ModAsset] Weighted bone diagnostics: %s renderer=%zu modTop=[%s] originalTop=[%s]",
                sourceName.c_str(),
                rendererIndex,
                FormatTopBoneWeightStats(modBoneWeightTotals, modBones, 16).c_str(),
                FormatTopBoneWeightStats(originalBoneWeightTotals, originalBones, 16).c_str());
            return matchedBones > 0;
        }

        void LogSkinnedMeshRendererDiagnostics(const std::string& sourceName, const size_t rendererIndex,
            void* originalRenderer, void* modRenderer, void* originalMesh, void* modMesh, const char* stage) {
            const auto originalBones = GetSkinnedMeshRendererBones(originalRenderer);
            const auto modBones = GetSkinnedMeshRendererBones(modRenderer);
            const auto originalBindposes = GetMeshBindposes(originalMesh);
            const auto modBindposes = GetMeshBindposes(modMesh);
            const auto originalRootBone = GetSkinnedMeshRendererRootBone(originalRenderer);
            const auto modRootBone = GetSkinnedMeshRendererRootBone(modRenderer);
            Log::InfoFmt("[ModAsset] Mesh diagnostics %s: %s renderer=%zu originalRenderer=\"%s\" modRenderer=\"%s\" originalMesh=\"%s\" modMesh=\"%s\" originalVertices=%d modVertices=%d originalBones=%zu modBones=%zu originalBindposes=%zu modBindposes=%zu originalRoot=\"%s\" modRoot=\"%s\"",
                stage,
                sourceName.c_str(),
                rendererIndex,
                GetUnityObjectNameString(originalRenderer).c_str(),
                GetUnityObjectNameString(modRenderer).c_str(),
                GetUnityObjectNameString(originalMesh).c_str(),
                GetUnityObjectNameString(modMesh).c_str(),
                GetMeshVertexCount(originalMesh),
                GetMeshVertexCount(modMesh),
                originalBones ? static_cast<size_t>(originalBones->max_length) : 0,
                modBones ? static_cast<size_t>(modBones->max_length) : 0,
                originalBindposes ? static_cast<size_t>(originalBindposes->max_length) : 0,
                modBindposes ? static_cast<size_t>(modBindposes->max_length) : 0,
                GetUnityObjectNameString(originalRootBone).c_str(),
                GetUnityObjectNameString(modRootBone).c_str());
        }

        std::optional<size_t> FindRendererIndexByName(const std::vector<void*>& renderers,
            const std::string& rendererName,
            const std::unordered_set<size_t>* usedIndices = nullptr) {
            if (rendererName.empty()) return std::nullopt;

            for (size_t i = 0; i < renderers.size(); ++i) {
                if (usedIndices && usedIndices->contains(i)) continue;
                if (GetUnityObjectNameString(renderers[i]) == rendererName) return i;
            }
            return std::nullopt;
        }

        std::vector<LocalModRendererPair> BuildRendererPairs(const std::vector<void*>& originalRenderers,
            const std::vector<void*>& modRenderers,
            const LocalModAssetReplacement& replacement) {
            std::vector<LocalModRendererPair> pairs;
            std::unordered_set<size_t> usedOriginalIndices;
            std::unordered_set<size_t> usedModIndices;

            const auto addPair = [&](const size_t originalIndex, const size_t modIndex) {
                if (originalIndex >= originalRenderers.size() || modIndex >= modRenderers.size()) return;
                if (usedOriginalIndices.contains(originalIndex) || usedModIndices.contains(modIndex)) return;
                pairs.emplace_back(LocalModRendererPair{ originalRenderers[originalIndex], modRenderers[modIndex], originalIndex, modIndex });
                usedOriginalIndices.emplace(originalIndex);
                usedModIndices.emplace(modIndex);
            };

            if (!replacement.rendererRules.empty()) {
                for (const auto& rule : replacement.rendererRules) {
                    auto originalIndex = FindRendererIndexByName(originalRenderers, rule.targetRenderer, &usedOriginalIndices);
                    auto modIndex = FindRendererIndexByName(modRenderers, rule.modRenderer, &usedModIndices);
                    if (!originalIndex && originalRenderers.size() == 1 && !usedOriginalIndices.contains(0)) {
                        originalIndex = 0;
                        Log::WarnFmt("[ModAsset] Renderer rule target not found, fallback to only original renderer: %s rendererId=\"%s\" targetRenderer=\"%s\" actual=\"%s\"",
                            replacement.sourceName.c_str(),
                            rule.rendererId.c_str(),
                            rule.targetRenderer.c_str(),
                            GetUnityObjectNameString(originalRenderers[0]).c_str());
                    }
                    if (!modIndex && modRenderers.size() == 1 && !usedModIndices.contains(0)) {
                        modIndex = 0;
                        Log::WarnFmt("[ModAsset] Renderer rule mod not found, fallback to only mod renderer: %s rendererId=\"%s\" modRenderer=\"%s\" actual=\"%s\"",
                            replacement.sourceName.c_str(),
                            rule.rendererId.c_str(),
                            rule.modRenderer.c_str(),
                            GetUnityObjectNameString(modRenderers[0]).c_str());
                    }
                    if (originalIndex && modIndex) {
                        addPair(*originalIndex, *modIndex);
                        continue;
                    }

                    Log::ErrorFmt("[ModAsset] Renderer rule pair not found: %s rendererId=\"%s\" targetRenderer=\"%s\" modRenderer=\"%s\" originalRenderers=%zu modRenderers=%zu",
                        replacement.sourceName.c_str(),
                        rule.rendererId.c_str(),
                        rule.targetRenderer.c_str(),
                        rule.modRenderer.c_str(),
                        originalRenderers.size(),
                        modRenderers.size());
                }
                return pairs;
            }

            if (!replacement.rendererName.empty()) {
                auto originalIndex = FindRendererIndexByName(originalRenderers, replacement.rendererName);
                auto modIndex = FindRendererIndexByName(modRenderers, replacement.rendererName);

                if (!originalIndex && originalRenderers.size() == 1) {
                    originalIndex = 0;
                    Log::WarnFmt("[ModAsset] Original rendererName not found, fallback to only original renderer: %s rendererName=\"%s\" actual=\"%s\"",
                        replacement.sourceName.c_str(), replacement.rendererName.c_str(), GetUnityObjectNameString(originalRenderers[0]).c_str());
                }
                if (!modIndex && modRenderers.size() == 1) {
                    modIndex = 0;
                    Log::WarnFmt("[ModAsset] Mod rendererName not found, fallback to only mod renderer: %s rendererName=\"%s\" actual=\"%s\"",
                        replacement.sourceName.c_str(), replacement.rendererName.c_str(), GetUnityObjectNameString(modRenderers[0]).c_str());
                }
                if (originalIndex && modIndex) addPair(*originalIndex, *modIndex);
                else {
                    Log::ErrorFmt("[ModAsset] RendererName pair not found: %s rendererName=\"%s\" originalRenderers=%zu modRenderers=%zu",
                        replacement.sourceName.c_str(), replacement.rendererName.c_str(), originalRenderers.size(), modRenderers.size());
                }
                return pairs;
            }

            for (size_t originalIndex = 0; originalIndex < originalRenderers.size(); ++originalIndex) {
                const auto originalName = GetUnityObjectNameString(originalRenderers[originalIndex]);
                if (originalName.empty()) continue;
                if (const auto modIndex = FindRendererIndexByName(modRenderers, originalName, &usedModIndices)) {
                    addPair(originalIndex, *modIndex);
                }
            }

            size_t originalIndex = 0;
            size_t modIndex = 0;
            while (originalIndex < originalRenderers.size() && modIndex < modRenderers.size()) {
                while (originalIndex < originalRenderers.size() && usedOriginalIndices.contains(originalIndex)) ++originalIndex;
                while (modIndex < modRenderers.size() && usedModIndices.contains(modIndex)) ++modIndex;
                if (originalIndex < originalRenderers.size() && modIndex < modRenderers.size()) {
                    addPair(originalIndex, modIndex);
                    ++originalIndex;
                    ++modIndex;
                }
            }
            return pairs;
        }

        void* GetRendererSharedMaterials(void* renderer) {
            static auto method = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer", "get_sharedMaterials");
            if (!renderer || !method || !method->function) return nullptr;
            using Fn = void* (*)(void*, void*);
            return reinterpret_cast<Fn>(method->function)(renderer, method->address);
        }

        bool SetRendererSharedMaterials(void* renderer, void* materialsObject) {
            static auto method = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer", "set_sharedMaterials",
                { "UnityEngine.Material[]" });
            if (!renderer || !materialsObject || !method || !method->function) return false;
            using Fn = void (*)(void*, void*, void*);
            t_internalMaterialAssignment = true;
            reinterpret_cast<Fn>(method->function)(renderer, materialsObject, method->address);
            t_internalMaterialAssignment = false;
            {
                std::lock_guard lock(g_materialOverrideMutex);
                g_rendererTextureOverrideCache.erase(renderer);
            }
            return true;
        }

        int GetShaderPropertyId(const std::string& propertyName) {
            if (!g_shaderPropertyToIdMethod) {
                g_shaderPropertyToIdMethod = Il2cppUtils::GetMethod(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "Shader", "PropertyToID",
                    { "System.String" });
            }
            if (!g_shaderPropertyToIdMethod || !g_shaderPropertyToIdMethod->function) return -1;
            using Fn = int (*)(Il2cppString*, void*);
            return reinterpret_cast<Fn>(g_shaderPropertyToIdMethod->function)(
                Il2cppString::New(propertyName),
                g_shaderPropertyToIdMethod->address);
        }

        bool SetMaterialTexture(void* material, const int propertyId, void* texture) {
            if (!g_materialSetTextureMethod) {
                g_materialSetTextureMethod = Il2cppUtils::GetMethod(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "Material", "SetTexture",
                    { "System.Int32", "UnityEngine.Texture" });
            }
            if (!material || propertyId < 0 || !texture
                || !g_materialSetTextureMethod || !g_materialSetTextureMethod->function) {
                return false;
            }
            using Fn = void (*)(void*, int, void*, void*);
            reinterpret_cast<Fn>(g_materialSetTextureMethod->function)(
                material, propertyId, texture, g_materialSetTextureMethod->address);
            return true;
        }

        // Shader name, keywords and every scalar the shader declares.  Colours
        // and vectors are left out on purpose: their 16-byte return would need
        // the hidden-pointer ABI, and a stale-derived-state difference shows up
        // in a keyword or a scalar first.
        std::string DescribeMaterialState(void* material) {
            if (!material) return {};
            static auto Material_get_shader = reinterpret_cast<void* (*)(void*)>(
                Il2cppUtils::GetMethodPointer(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "Material", "get_shader"));
            static auto Material_get_shaderKeywords =
                reinterpret_cast<UnityArray<Il2cppString*>* (*)(void*)>(
                    Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine",
                        "Material", "get_shaderKeywords"));
            static auto Material_GetFloat = reinterpret_cast<float (*)(void*, int)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine",
                    "Material", "GetFloat", { "System.Int32" }));
            static auto Shader_GetPropertyCount = reinterpret_cast<int (*)(void*)>(
                Il2cppUtils::GetMethodPointer(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "Shader", "GetPropertyCount"));
            static auto Shader_GetPropertyName = reinterpret_cast<Il2cppString* (*)(void*, int)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine",
                    "Shader", "GetPropertyName", { "System.Int32" }));
            static auto Shader_GetPropertyType = reinterpret_cast<int (*)(void*, int)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine",
                    "Shader", "GetPropertyType", { "System.Int32" }));

            std::string result;
            if (Material_get_shaderKeywords) {
                if (const auto keywords = Material_get_shaderKeywords(material)) {
                    result += " keywords=[";
                    for (std::uintptr_t i = 0; i < keywords->max_length; ++i) {
                        if (const auto keyword = keywords->At(static_cast<unsigned int>(i))) {
                            if (i) result += ",";
                            result += keyword->ToString();
                        }
                    }
                    result += "]";
                }
            }
            const auto shader = Material_get_shader ? Material_get_shader(material) : nullptr;
            if (!shader || !Shader_GetPropertyCount || !Shader_GetPropertyName
                || !Shader_GetPropertyType || !Material_GetFloat) {
                return result;
            }
            result += " floats=[";
            bool first = true;
            const auto count = Shader_GetPropertyCount(shader);
            for (int i = 0; i < count; ++i) {
                // ShaderPropertyType: 0 Color, 1 Vector, 2 Float, 3 Range, 4 Texture.
                const auto type = Shader_GetPropertyType(shader, i);
                if (type != 2 && type != 3) continue;
                const auto name = Shader_GetPropertyName(shader, i);
                if (!name) continue;
                const auto propertyName = name->ToString();
                if (!first) result += ",";
                first = false;
                result += Log::Format("%s=%.4f", propertyName.c_str(),
                    Material_GetFloat(material, GetShaderPropertyId(propertyName)));
            }
            result += "]";
            return result;
        }

        void* GetMaterialTexture(void* material, const int propertyId) {
            static auto method = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Material", "GetTexture",
                { "System.Int32" });
            if (!material || propertyId < 0 || !method || !method->function) return nullptr;
            using Fn = void* (*)(void*, int, void*);
            return reinterpret_cast<Fn>(method->function)(material, propertyId, method->address);
        }

        void RegisterPersistentMaterialTextureOverride(void* material, const int propertyId,
            const std::string& propertyName, void* texture, void* previousTexture) {
            if (!material || propertyId < 0 || !texture) return;
            std::lock_guard lock(g_materialOverrideMutex);
            auto& overrides = g_materialTextureOverrides[material];
            if (const auto iter = std::find_if(overrides.begin(), overrides.end(),
                [propertyId](const auto& entry) { return entry.propertyId == propertyId; });
                iter != overrides.end()) {
                iter->propertyName = propertyName;
                iter->texture = texture;
                // Keep the first snapshot: the second write would record our own
                // texture as the thing to restore.
            }
            else {
                if (previousTexture) {
                    // Nothing else may reference it once the material stops doing
                    // so, and a restore into a collected texture is a native crash.
                    if (const auto handle = UnityResolve::Invoke<Il2CppGCHandle>(
                            "il2cpp_gchandle_new", previousTexture, false)) {
                        g_runtimeMaterialHandles.emplace_back(handle);
                    }
                }
                overrides.emplace_back(PersistentMaterialTextureOverride{
                    propertyId,
                    propertyName,
                    texture,
                    previousTexture,
                });
            }
            g_rendererTextureOverrideCache.clear();
        }

        std::vector<PersistentMaterialTextureOverride> GetRegisteredMaterialTextureOverrides(void* material) {
            if (!material) return {};
            std::lock_guard lock(g_materialOverrideMutex);
            if (const auto iter = g_materialTextureOverrides.find(material);
                iter != g_materialTextureOverrides.end()) {
                return iter->second;
            }
            return {};
        }

        std::vector<RendererSlotTextureOverrides> CollectRendererTextureOverrides(void* renderer) {
            if (!renderer) return {};
            {
                std::lock_guard lock(g_materialOverrideMutex);
                if (const auto iter = g_rendererTextureOverrideCache.find(renderer);
                    iter != g_rendererTextureOverrideCache.end()) {
                    return iter->second;
                }
            }

            std::vector<RendererSlotTextureOverrides> result;
            const auto materials = reinterpret_cast<UnityArray<void*>*>(GetRendererSharedMaterials(renderer));
            if (!materials) return result;

            for (std::uintptr_t i = 0; i < materials->max_length; ++i) {
                auto overrides = GetRegisteredMaterialTextureOverrides(
                    materials->At(static_cast<unsigned int>(i)));
                if (!overrides.empty()) {
                    result.emplace_back(RendererSlotTextureOverrides{
                        static_cast<int>(i),
                        std::move(overrides),
                    });
                }
            }
            {
                std::lock_guard lock(g_materialOverrideMutex);
                g_rendererTextureOverrideCache[renderer] = result;
            }
            return result;
        }

        void* ClonePrivateMaterial(void* material, const std::string& sourceName,
            const size_t rendererIndex, const size_t materialIndex) {
            if (!material) return nullptr;

            using CloneFn = void* (*)(void*);
            static auto Object_InternalCloneSingle = reinterpret_cast<CloneFn>(
                Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Object::Internal_CloneSingle(UnityEngine.Object)"));
            auto clone = Object_InternalCloneSingle ? Object_InternalCloneSingle(material) : nullptr;

            if (!clone) {
                static auto Object_Instantiate = Il2cppUtils::GetMethod(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "Object", "Instantiate",
                    { "UnityEngine.Object" });
                if (Object_Instantiate && Object_Instantiate->function) {
                    using InstantiateFn = void* (*)(void*, void*);
                    clone = reinterpret_cast<InstantiateFn>(Object_Instantiate->function)(
                        material, Object_Instantiate->address);
                }
            }
            if (!clone) {
                Log::ErrorFmt("[ModAsset] Failed to clone private material: %s renderer=%zu slot=%zu material=%p",
                    sourceName.c_str(), rendererIndex, materialIndex, material);
                return nullptr;
            }

            const auto handle = UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", clone, false);
            {
                std::lock_guard lock(g_materialOverrideMutex);
                g_privateMaterials.emplace(clone);
                if (handle) g_runtimeMaterialHandles.emplace_back(handle);
            }
            Log::InfoFmt("[ModAsset] Cloned private material: %s renderer=%zu slot=%zu source=%p clone=%p",
                sourceName.c_str(), rendererIndex, materialIndex, material, clone);
            return clone;
        }

        bool EnsureRendererPrivateMaterials(void* renderer, void* materialsObject,
            const LocalModAssetReplacement& replacement, const size_t rendererIndex) {
            const auto materials = reinterpret_cast<UnityArray<void*>*>(materialsObject);
            if (!renderer || !materials) return false;

            bool changed = false;
            for (std::uintptr_t i = 0; i < materials->max_length; ++i) {
                const auto material = materials->At(static_cast<unsigned int>(i));
                if (!material) continue;

                bool alreadyPrivate = false;
                {
                    std::lock_guard lock(g_materialOverrideMutex);
                    alreadyPrivate = g_privateMaterials.contains(material);
                }
                if (alreadyPrivate) continue;

                if (const auto clone = ClonePrivateMaterial(
                    material, replacement.sourceName, rendererIndex, static_cast<size_t>(i))) {
                    materials->At(static_cast<unsigned int>(i)) = clone;
                    changed = true;
                }
            }

            if (changed && !SetRendererSharedMaterials(renderer, materialsObject)) {
                Log::ErrorFmt("[ModAsset] Failed to assign private materials: %s renderer=%zu",
                    replacement.sourceName.c_str(), rendererIndex);
                return false;
            }
            return changed;
        }

        bool ApplyMaterialTextureReplacements(void* renderer, void* materialsObject,
            const LocalModAssetReplacement& replacement, const size_t rendererIndex) {
            const auto materials = reinterpret_cast<UnityArray<void*>*>(materialsObject);
            if (!materials) return false;

            const auto activeRendererName = GetUnityObjectNameString(renderer);
            size_t applied = 0;
            for (const auto& textureReplacement : replacement.materialTextures) {
                if (!textureReplacement.rendererName.empty()
                    && !activeRendererName.empty()
                    && textureReplacement.rendererName != activeRendererName) {
                    continue;
                }
                if (textureReplacement.materialSlot < 0
                    || static_cast<std::uintptr_t>(textureReplacement.materialSlot) >= materials->max_length) {
                    continue;
                }

                const auto textureAsset = LoadLocalModAssetFromBundle(
                    replacement.bundleHandle,
                    replacement.bundlePath,
                    textureReplacement.assetName,
                    textureReplacement.typeName);
                if (!textureAsset) {
                    Log::ErrorFmt("[ModAsset] Material texture load failed: %s property=%s",
                        textureReplacement.assetName.c_str(),
                        textureReplacement.propertyName.c_str());
                    continue;
                }

                const auto material = materials->At(static_cast<unsigned int>(textureReplacement.materialSlot));
                if (!material) continue;
                const auto propertyId = GetShaderPropertyId(textureReplacement.propertyName);
                const auto previousTexture = GetMaterialTexture(material, propertyId);
                if (!SetMaterialTexture(material, propertyId, textureAsset)) {
                    Log::ErrorFmt("[ModAsset] Material.SetTexture failed: %s renderer=%zu slot=%d property=%s",
                        replacement.sourceName.c_str(),
                        rendererIndex,
                        textureReplacement.materialSlot,
                        textureReplacement.propertyName.c_str());
                    continue;
                }
                RegisterPersistentMaterialTextureOverride(
                    material, propertyId, textureReplacement.propertyName, textureAsset,
                    previousTexture);
                ++applied;
                Log::InfoFmt("[ModAsset] Applied material texture: %s renderer=%zu rendererName=\"%s\" slot=%d property=%s texture=%s result=%p",
                    replacement.sourceName.c_str(),
                    rendererIndex,
                    activeRendererName.c_str(),
                    textureReplacement.materialSlot,
                    textureReplacement.propertyName.c_str(),
                    textureReplacement.assetName.c_str(),
                    textureAsset);
            }

            if (applied > 0) {
                Log::InfoFmt("[ModAsset] Material texture replacement finished: %s renderer=%zu applied=%zu",
                    replacement.sourceName.c_str(), rendererIndex, applied);
            }
            return applied > 0;
        }

        bool ApplyMaterialSlotCopies(void* renderer, void* materialsObject,
            const LocalModAssetReplacement& replacement, const size_t rendererIndex) {
            const auto materials = reinterpret_cast<UnityArray<void*>*>(materialsObject);
            if (!renderer || !materials || replacement.materialCopies.empty()) {
                return false;
            }

            const auto activeRendererName = GetUnityObjectNameString(renderer);
            size_t applied = 0;
            for (const auto& materialCopy : replacement.materialCopies) {
                if (!materialCopy.rendererName.empty()
                    && !activeRendererName.empty()
                    && materialCopy.rendererName != activeRendererName) {
                    continue;
                }
                if (materialCopy.fromSlot < 0 || materialCopy.toSlot < 0
                    || static_cast<std::uintptr_t>(materialCopy.fromSlot) >= materials->max_length
                    || static_cast<std::uintptr_t>(materialCopy.toSlot) >= materials->max_length) {
                    Log::WarnFmt("[ModAsset] Material slot copy skipped out of range: %s renderer=%zu rendererName=\"%s\" from=%d to=%d materialCount=%zu",
                        replacement.sourceName.c_str(),
                        rendererIndex,
                        activeRendererName.c_str(),
                        materialCopy.fromSlot,
                        materialCopy.toSlot,
                        static_cast<size_t>(materials->max_length));
                    continue;
                }

                const auto sourceMaterial = materials->At(static_cast<unsigned int>(materialCopy.fromSlot));
                if (!sourceMaterial) {
                    Log::WarnFmt("[ModAsset] Material slot copy skipped null source: %s renderer=%zu rendererName=\"%s\" from=%d to=%d",
                        replacement.sourceName.c_str(),
                        rendererIndex,
                        activeRendererName.c_str(),
                        materialCopy.fromSlot,
                        materialCopy.toSlot);
                    continue;
                }

                materials->At(static_cast<unsigned int>(materialCopy.toSlot)) = sourceMaterial;
                ++applied;
                Log::InfoFmt("[ModAsset] Copied material slot: %s renderer=%zu rendererName=\"%s\" from=%d to=%d material=\"%s\"",
                    replacement.sourceName.c_str(),
                    rendererIndex,
                    activeRendererName.c_str(),
                    materialCopy.fromSlot,
                    materialCopy.toSlot,
                    GetUnityObjectNameString(sourceMaterial).c_str());
            }

            if (applied > 0) {
                SetRendererSharedMaterials(renderer, materialsObject);
                Log::InfoFmt("[ModAsset] Material slot copy finished: %s renderer=%zu applied=%zu",
                    replacement.sourceName.c_str(), rendererIndex, applied);
                return true;
            }
            return false;
        }

        bool MaterialHasProperty(void* material, const std::string& propertyName) {
            static auto Material_HasProperty = reinterpret_cast<bool (*)(void*, Il2cppString*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Material",
                    "HasProperty", { "System.String" }));
            return !Material_HasProperty || Material_HasProperty(material, Il2cppString::New(propertyName));
        }

        // `GakumasSdk/BodyPlaceholder` is a texture CARRIER, not a shader to render with —
        // its own header says so.  It declares `_BaseMap` / `_DefMap` / `_ShadeMap` under the
        // game shader's exact names precisely so those maps can be moved onto the game's
        // material.  Handing the renderer the placeholders instead gives up the toon shader,
        // the outline pass, the shared ramps and every render state the game sets — which is
        // why single-sided panels vanished from one side and depth stopped agreeing with the
        // rest of the actor, and why `_Cull` could not even be written (the placeholder has
        // no such property, and the log said so by staying silent).
        //
        // Cloned fresh on every application on purpose: a clone freezes `_RampMap` /
        // `_RampAddMap`, which the game swaps per scene, and a stale clone is what rendered a
        // costume black under the photo-shoot lights.
        UnityArray<void*>* AdoptGameMaterialsForModMesh(
            UnityArray<void*>* originalMaterials,
            UnityArray<void*>* modMaterials,
            const std::string& sourceName,
            const size_t rendererIndex) {
            static const char* const kCarried[] = { "_BaseMap", "_DefMap", "_ShadeMap" };
            if (!originalMaterials || originalMaterials->max_length == 0 || !modMaterials
                || modMaterials->max_length == 0) {
                return nullptr;
            }
            const auto materialClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Material");
            // ponytail: every mod submesh adopts the game's slot 0 (the opaque body
            // material).  The vanilla renderer's own slot count says nothing about how the
            // source model is cut up, and slot 1 is the transparent `co` pass — adopting it
            // by index would hand a solid panel a no-depth-write material.
            const auto templateMaterial = originalMaterials->At(0);
            if (!materialClass || !templateMaterial) return nullptr;

            const auto slots = static_cast<size_t>(modMaterials->max_length);
            auto adopted = UnityArray<void*>::New(materialClass, slots);
            std::string carriedReport;
            for (size_t index = 0; index < slots; ++index) {
                const auto clone = CloneUnityObject(templateMaterial, sourceName, rendererIndex);
                if (!clone) return nullptr;
                const auto source = modMaterials->At(static_cast<unsigned int>(index));
                size_t carried = 0;
                for (const auto property : kCarried) {
                    if (!source || !MaterialHasProperty(source, property)) continue;
                    const auto propertyId = GetShaderPropertyId(property);
                    const auto texture = GetMaterialTexture(source, propertyId);
                    if (texture && SetMaterialTexture(clone, propertyId, texture)) ++carried;
                }
                adopted->At(static_cast<unsigned int>(index)) = clone;
                carriedReport += (carriedReport.empty() ? "" : ", ")
                    + GetUnityObjectNameString(source) + ":" + std::to_string(carried);
            }
            Log::WarnFmt("[ModAsset] Adopted game material for mod submeshes: %s renderer=%zu slots=%zu template=%s carried=[%s]",
                sourceName.c_str(), rendererIndex, slots,
                GetUnityObjectNameString(templateMaterial).c_str(), carriedReport.c_str());
            return adopted;
        }


        // ---- 自建半透明材质 ----------------------------------------------------------
        //
        // 游戏自己的 Campus/Actor/Default 只有不透明与镂空（`_ALPHATEST_ON`）两档，
        // 没有可借的真半透明配方，所以 shader 是我们用 Unity 6000.0.77f1（与游戏同版本）
        // 自己编的：<游戏根>/gakumas-mod/gmi_shaders.bundle 里的 `Gmi/Transparent`。
        // 它走 URP 透明队列（Queue=Transparent + LightMode=UniversalForward），在延迟光照、
        // AO 和角色合成之后才画 —— 这正好绕开 3Dmigoto 时代那两个死结：G-buffer 合成靠
        // 深度判"这里是角色"（所以背景上的半透明会消失）、以及 A=0 缝隙被 SSAO 算出暗带。
        Il2CppGCHandle g_gmiTransparentShaderHandle{};
        Il2CppGCHandle g_gmiShaderBundleHandle{};
        bool g_gmiTransparentShaderFailed = false;

        void* EnsureGmiTransparentShader() {
            if (g_gmiTransparentShaderHandle) {
                const auto cached = UnityResolve::Invoke<void*>(
                    "il2cpp_gchandle_get_target", g_gmiTransparentShaderHandle);
                if (cached && IsNativeObjectAlive(cached)) return cached;
                UnityResolve::Invoke<void>("il2cpp_gchandle_free",
                    std::exchange(g_gmiTransparentShaderHandle, Il2CppGCHandle{}));
            }
            if (g_gmiTransparentShaderFailed) return nullptr;

            const auto bundlePath = std::filesystem::absolute(
                Paths::Root() / "gmi_shaders.bundle").lexically_normal();
            if (!std::filesystem::is_regular_file(bundlePath)) {
                g_gmiTransparentShaderFailed = true;
                Log::ErrorFmt("[ModAsset] Transparent shader bundle missing: %s", bundlePath.string().c_str());
                return nullptr;
            }

            if (!g_gmiShaderBundleHandle) {
                const auto bundle = LoadAssetBundleFromMemoryFile(bundlePath);
                if (!bundle) {
                    g_gmiTransparentShaderFailed = true;
                    Log::ErrorFmt("[ModAsset] Transparent shader bundle load failed: %s", bundlePath.string().c_str());
                    return nullptr;
                }
                g_gmiShaderBundleHandle = UnityResolve::Invoke<Il2CppGCHandle>(
                    "il2cpp_gchandle_new", bundle, false);
            }

            // 容器键在包里是小写的（assets/gmi/gmitransparent.shader）；LoadAsset 按理
            // 大小写无关，但别拿一次实机去赌，短名当兜底。
            void* shader = nullptr;
            for (const char* candidate : { "Assets/Gmi/GmiTransparent.shader",
                                           "assets/gmi/gmitransparent.shader",
                                           "GmiTransparent" }) {
                shader = LoadLocalModAssetFromBundle(
                    g_gmiShaderBundleHandle, bundlePath.string(), candidate, "Shader");
                if (shader) break;
            }
            if (!shader) {
                g_gmiTransparentShaderFailed = true;
                Log::Error("[ModAsset] Transparent shader asset not found in gmi_shaders.bundle.");
                return nullptr;
            }
            g_gmiTransparentShaderHandle = UnityResolve::Invoke<Il2CppGCHandle>(
                "il2cpp_gchandle_new", shader, false);
            Log::InfoFmt("[ModAsset] Transparent shader ready: %s shader=%p name=\"%s\"",
                bundlePath.string().c_str(), shader, GetUnityObjectNameString(shader).c_str());
            return shader;
        }

        void* CreateMaterialWithShader(void* shader) {
            const auto materialClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Material");
            if (!shader || !materialClass || !materialClass->address) return nullptr;
            // 按参数类型取 .ctor —— 参数个数匹配会撞上 Material(Material) 这个同 arity 重载。
            static auto ctor = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Material", ".ctor", { "UnityEngine.Shader" });
            if (!ctor) return nullptr;
            const auto material = UnityResolve::Invoke<void*>("il2cpp_object_new", materialClass->address);
            if (!material) return nullptr;
            ctor->Invoke<void>(material, shader);
            return material;
        }

        // 游戏 fork 出来的队列分类：VL.VLRenderQueue（Unity.RenderPipelines.Universal.Runtime.dll）
        // 里有 GBufferTransparentRange —— "在 G-buffer 阶段绘制的透明物"在他们的管线里是一等公民。
        // 别写死数值：区间是他们定的，版本之间可能变。
        struct LocalRenderQueueRange { int lowerBound; int upperBound; };

        bool GetGBufferTransparentQueue(int& outQueue) {
            static bool resolved = false;
            static bool ok = false;
            static LocalRenderQueueRange range{};
            if (!resolved) {
                resolved = true;
                if (const auto method = Il2cppUtils::GetMethod(
                        "Unity.RenderPipelines.Universal.Runtime.dll", "VL", "VLRenderQueue",
                        "get_GBufferTransparentRange", {}, true)) {
                    range = method->Invoke<LocalRenderQueueRange>();
                    ok = range.lowerBound > 0 && range.upperBound >= range.lowerBound
                        && range.upperBound <= 5000;
                    Log::InfoFmt("[ModAsset] VLRenderQueue.GBufferTransparentRange = [%d, %d] usable=%d",
                        range.lowerBound, range.upperBound, ok ? 1 : 0);
                }
                else {
                    Log::Error("[ModAsset] VLRenderQueue.GBufferTransparentRange not found; "
                        "falling back to the queue in mod.json.");
                }
            }
            if (!ok) return false;
            outQueue = range.lowerBound;
            return true;
        }

        bool SetMaterialRenderQueue(void* material, const int queue) {
            static auto Material_set_renderQueue = reinterpret_cast<void (*)(void*, int)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Material",
                    "set_renderQueue", { "System.Int32" }));
            if (!Material_set_renderQueue || !material) return false;
            Material_set_renderQueue(material, queue);
            return true;
        }

        bool SetMaterialFloatByName(void* material, const std::string& propertyName, const float value) {
            static auto Material_SetFloat = reinterpret_cast<void (*)(void*, Il2cppString*, float)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Material",
                    "SetFloat", { "System.String", "System.Single" }));
            if (!Material_SetFloat || !material) return false;
            Material_SetFloat(material, Il2cppString::New(propertyName), value);
            return true;
        }

        // 定义在下面的 VL 探针区（那里才有 CommandBuffer 相关的东西），这里先声明
        void RegisterAfterDofDraw(void* renderer, void* material, int submesh);

        // 声明了半透明段就必须整段成立：shader 缺失、贴图缺失、材质建不出来一律整体拒绝，
        // **不静默回落成不透明** —— 偷偷换渲染方式等于给作者一个"能看但不是他配的"结果。
        bool ApplyTransparentMaterials(void* renderer,
            const LocalModAssetReplacement& replacement, const size_t rendererIndex) {
            if (!renderer || replacement.transparentMaterials.empty()) return false;

            const auto activeRendererName = GetUnityObjectNameString(renderer);
            std::vector<const LocalModTransparentMaterial*> wanted;
            for (const auto& transparent : replacement.transparentMaterials) {
                if (!transparent.rendererName.empty()
                    && !activeRendererName.empty()
                    && transparent.rendererName != activeRendererName) {
                    continue;
                }
                wanted.push_back(&transparent);
            }
            if (wanted.empty()) return false;

            const auto shader = EnsureGmiTransparentShader();
            if (!shader) {
                Log::ErrorFmt("[ModAsset] Transparent materials refused (no shader): %s renderer=%zu rendererName=\"%s\" slots=%zu",
                    replacement.sourceName.c_str(), rendererIndex, activeRendererName.c_str(), wanted.size());
                return false;
            }

            const auto materialClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Material");
            const auto current = reinterpret_cast<UnityArray<void*>*>(GetRendererSharedMaterials(renderer));
            const auto currentCount = current ? static_cast<size_t>(current->max_length) : 0;
            if (!materialClass || currentCount == 0) {
                Log::ErrorFmt("[ModAsset] Transparent materials refused (renderer has no materials): %s renderer=%zu",
                    replacement.sourceName.c_str(), rendererIndex);
                return false;
            }

            size_t needed = currentCount;
            for (const auto* transparent : wanted) {
                const auto slotEnd = static_cast<size_t>(transparent->materialSlot) + 1;
                if (slotEnd > needed) needed = slotEnd;
            }

            auto expanded = UnityArray<void*>::New(materialClass, needed);
            if (!expanded) return false;
            for (size_t index = 0; index < needed; ++index) {
                // 空洞用槽 0 兜底：Unity 对 null 材质的子网格直接不画，且不报错。
                expanded->At(static_cast<unsigned int>(index)) = index < currentCount
                    ? current->At(static_cast<unsigned int>(index))
                    : current->At(0);
            }

            std::string report;
            for (const auto* transparent : wanted) {
                const auto texture = LoadLocalModAssetFromBundle(
                    replacement.bundleHandle, replacement.bundlePath,
                    transparent->assetName, transparent->typeName);
                if (!texture) {
                    Log::ErrorFmt("[ModAsset] Transparent materials refused (texture load failed): %s slot=%d asset=%s",
                        replacement.sourceName.c_str(), transparent->materialSlot, transparent->assetName.c_str());
                    return false;
                }
                void* material = nullptr;
                if (transparent->vanillaMaterial) {
                    // 槽 0 是不透明 body 材质；克隆它（每次重克隆，否则会冻结按场景换的 ramp）
                    material = CloneUnityObject(current->At(0), replacement.sourceName, rendererIndex);
                }
                else {
                    material = CreateMaterialWithShader(shader);
                }
                if (!material) {
                    Log::ErrorFmt("[ModAsset] Transparent materials refused (Material ctor failed): %s slot=%d",
                        replacement.sourceName.c_str(), transparent->materialSlot);
                    return false;
                }
                SetMaterialTexture(material, GetShaderPropertyId("_BaseMap"), texture);
                // toon 用的 t1/t4：缺了不算失败（shader 里默认黑图 = 退回接近 unlit），
                // 但要在日志里说清楚，否则"看着有点平"没人查得出原因。
                size_t toonMaps = 0;
                for (const auto& [propertyName, assetName] : {
                        std::pair<const char*, const std::string&>{ "_DefMap", transparent->defMapAsset },
                        std::pair<const char*, const std::string&>{ "_ShadeMap", transparent->shadeMapAsset } }) {
                    if (assetName.empty()) continue;
                    const auto map = LoadLocalModAssetFromBundle(
                        replacement.bundleHandle, replacement.bundlePath, assetName, transparent->typeName);
                    if (!map) {
                        Log::WarnFmt("[ModAsset] Transparent material toon map missing: %s slot=%d property=%s asset=%s",
                            replacement.sourceName.c_str(), transparent->materialSlot, propertyName, assetName.c_str());
                        continue;
                    }
                    SetMaterialTexture(material, GetShaderPropertyId(propertyName), map);
                    ++toonMaps;
                }
                SetMaterialFloatByName(material, "_Alpha", transparent->alpha);
                SetMaterialFloatByName(material, "_AlphaFromTexture", transparent->alphaFromTexture);
                SetMaterialFloatByName(material, "_Cull", transparent->cull);
                SetMaterialFloatByName(material, "_ZWriteMode", transparent->zwrite);
                SetMaterialFloatByName(material, "_Cutoff", transparent->cutoff);
                SetMaterialFloatByName(material, "_ToonStrength", toonMaps ? transparent->toonStrength : 0.0f);
                if (transparent->vanillaMaterial) {
                    Log::InfoFmt("[ModAsset] Transparent slot uses VANILLA material (control run): %s slot=%d material=%s",
                        replacement.sourceName.c_str(), transparent->materialSlot,
                        GetUnityObjectNameString(material).c_str());
                }
                SetMaterialFloatByName(material, "_ShadeDarken", transparent->shadeDarken);
                SetMaterialFloatByName(material, "_ToonSoftness", transparent->toonSoftness);
                SetMaterialFloatByName(material, "_AoStrength", transparent->aoStrength);
                for (const auto& [propertyName, value] : transparent->extraFloats) {
                    SetMaterialFloatByName(material, propertyName, value);
                }
                int queue = transparent->renderQueue;
                if (transparent->gbufferQueue && GetGBufferTransparentQueue(queue)) {
                    Log::InfoFmt("[ModAsset] Transparent slot uses native GBuffer queue: %s slot=%d queue=%d",
                        replacement.sourceName.c_str(), transparent->materialSlot, queue);
                }
                if (queue >= 0) SetMaterialRenderQueue(material, queue);
                g_runtimeMaterialHandles.emplace_back(
                    UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", material, false));
                expanded->At(static_cast<unsigned int>(transparent->materialSlot)) = material;
                RegisterAfterDofDraw(renderer, material, transparent->materialSlot);
                report += (report.empty() ? "" : ", ")
                    + std::to_string(transparent->materialSlot) + ":" + transparent->assetName
                    + " alpha=" + std::to_string(transparent->alpha)
                    + " toonMaps=" + std::to_string(toonMaps)
                    + " queue=" + std::to_string(transparent->renderQueue)
                    + " props=" + std::to_string(transparent->extraFloats.size());
            }

            if (!SetRendererSharedMaterials(renderer, expanded)) {
                Log::ErrorFmt("[ModAsset] Transparent materials refused (set_sharedMaterials failed): %s renderer=%zu",
                    replacement.sourceName.c_str(), rendererIndex);
                return false;
            }
            Log::InfoFmt("[ModAsset] Applied transparent materials: %s renderer=%zu rendererName=\"%s\" slots=%zu->%zu [%s]",
                replacement.sourceName.c_str(), rendererIndex, activeRendererName.c_str(),
                currentCount, needed, report.c_str());
            return true;
        }

        bool ApplyMaterialColorReplacements(void* renderer, void* materialsObject,
            const LocalModAssetReplacement& replacement, const size_t rendererIndex) {
            const auto materials = reinterpret_cast<UnityArray<void*>*>(materialsObject);
            if (!materials || replacement.materialColors.empty()) return false;

            static auto Material_SetColor = reinterpret_cast<void (*)(void*, Il2cppString*, LocalModUnityColor)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Material",
                    "SetColor", { "System.String", "UnityEngine.Color" }));
            if (!Material_SetColor) {
                Log::Error("[ModAsset] Cannot resolve Material.SetColor.");
                return false;
            }

            const auto activeRendererName = GetUnityObjectNameString(renderer);
            size_t applied = 0;
            for (const auto& colorReplacement : replacement.materialColors) {
                if (!colorReplacement.rendererName.empty()
                    && !activeRendererName.empty()
                    && colorReplacement.rendererName != activeRendererName) {
                    continue;
                }
                if (colorReplacement.materialSlot < 0
                    || static_cast<std::uintptr_t>(colorReplacement.materialSlot) >= materials->max_length) {
                    continue;
                }

                const auto material = materials->At(static_cast<unsigned int>(colorReplacement.materialSlot));
                if (!material || !MaterialHasProperty(material, colorReplacement.propertyName)) continue;
                Material_SetColor(material, Il2cppString::New(colorReplacement.propertyName), LocalModUnityColor{
                    colorReplacement.r,
                    colorReplacement.g,
                    colorReplacement.b,
                    colorReplacement.a,
                });
                ++applied;
                Log::InfoFmt("[ModAsset] Applied material color: %s renderer=%zu rendererName=\"%s\" slot=%d property=%s value=(%.3f,%.3f,%.3f,%.3f)",
                    replacement.sourceName.c_str(),
                    rendererIndex,
                    activeRendererName.c_str(),
                    colorReplacement.materialSlot,
                    colorReplacement.propertyName.c_str(),
                    colorReplacement.r,
                    colorReplacement.g,
                    colorReplacement.b,
                    colorReplacement.a);
            }

            if (applied > 0) {
                Log::InfoFmt("[ModAsset] Material color replacement finished: %s renderer=%zu applied=%zu",
                    replacement.sourceName.c_str(), rendererIndex, applied);
            }
            return applied > 0;
        }

        bool ApplyMaterialFloatReplacements(void* renderer, void* materialsObject,
            const LocalModAssetReplacement& replacement, const size_t rendererIndex) {
            const auto materials = reinterpret_cast<UnityArray<void*>*>(materialsObject);
            if (!materials || replacement.materialFloats.empty()) return false;

            static auto Material_SetFloat = reinterpret_cast<void (*)(void*, Il2cppString*, float)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Material",
                    "SetFloat", { "System.String", "System.Single" }));
            if (!Material_SetFloat) {
                Log::Error("[ModAsset] Cannot resolve Material.SetFloat.");
                return false;
            }

            const auto activeRendererName = GetUnityObjectNameString(renderer);
            size_t applied = 0;
            for (const auto& floatReplacement : replacement.materialFloats) {
                if (!floatReplacement.rendererName.empty()
                    && !activeRendererName.empty()
                    && floatReplacement.rendererName != activeRendererName) {
                    continue;
                }
                if (floatReplacement.materialSlot < 0
                    || static_cast<std::uintptr_t>(floatReplacement.materialSlot) >= materials->max_length) {
                    continue;
                }

                const auto material = materials->At(static_cast<unsigned int>(floatReplacement.materialSlot));
                if (!material || !MaterialHasProperty(material, floatReplacement.propertyName)) continue;
                Material_SetFloat(material, Il2cppString::New(floatReplacement.propertyName), floatReplacement.value);
                ++applied;
                Log::InfoFmt("[ModAsset] Applied material float: %s renderer=%zu rendererName=\"%s\" slot=%d property=%s value=%.3f",
                    replacement.sourceName.c_str(),
                    rendererIndex,
                    activeRendererName.c_str(),
                    floatReplacement.materialSlot,
                    floatReplacement.propertyName.c_str(),
                    floatReplacement.value);
            }

            if (applied > 0) {
                Log::InfoFmt("[ModAsset] Material float replacement finished: %s renderer=%zu applied=%zu",
                    replacement.sourceName.c_str(), rendererIndex, applied);
            }
            return applied > 0;
        }

        nlohmann::json DumpMaterial(void* material) {
            nlohmann::json result;
            result["name"] = GetUnityObjectNameString(material);

            static auto Material_get_shader = reinterpret_cast<void* (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Material", "get_shader"));
            const auto shader = material && Material_get_shader ? Material_get_shader(material) : nullptr;
            result["shader"] = GetUnityObjectNameString(shader);
            result["properties"] = nlohmann::json::array();

            static auto Shader_GetPropertyCount = reinterpret_cast<int (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Shader", "GetPropertyCount"));
            static auto Shader_GetPropertyName = reinterpret_cast<Il2cppString * (*)(void*, int)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Shader", "GetPropertyName", { "System.Int32" }));
            if (shader && Shader_GetPropertyCount && Shader_GetPropertyName) {
                const auto count = Shader_GetPropertyCount(shader);
                for (int i = 0; i < count; ++i) {
                    if (const auto propertyName = Shader_GetPropertyName(shader, i)) {
                        result["properties"].push_back(propertyName->ToString());
                    }
                }
            }
            return result;
        }

        void DumpSourceProfileIfNeeded(const std::string& sourceName, void* originalGameObject) {
            const auto profileKey = NormalizeAssetName(sourceName);
            if (g_dumpedProfiles.contains(profileKey)) return;
            g_dumpedProfiles.emplace(profileKey);

            const auto skinnedMeshRendererClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer");
            if (!skinnedMeshRendererClass || !originalGameObject) return;

            auto renderers = reinterpret_cast<UnityResolve::UnityType::GameObject*>(originalGameObject)
                ->GetComponentsInChildren<void*>(skinnedMeshRendererClass, true);
            static auto SkinnedMeshRenderer_get_sharedMesh = reinterpret_cast<void* (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer", "get_sharedMesh"));
            static auto Renderer_get_sharedMaterials = reinterpret_cast<void* (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "Renderer", "get_sharedMaterials"));

            nlohmann::json profile;
            profile["schemaVersion"] = 1;
            profile["source"] = sourceName;
            profile["part"] = InferPartFromAssetName(sourceName);
            profile["renderers"] = nlohmann::json::array();

            for (size_t rendererIndex = 0; rendererIndex < renderers.size(); ++rendererIndex) {
                const auto renderer = renderers[rendererIndex];
                const auto mesh = SkinnedMeshRenderer_get_sharedMesh ? SkinnedMeshRenderer_get_sharedMesh(renderer) : nullptr;
                const auto bones = GetSkinnedMeshRendererBones(renderer);
                const auto rootBone = GetSkinnedMeshRendererRootBone(renderer);

                nlohmann::json rendererJson;
                rendererJson["index"] = rendererIndex;
                rendererJson["name"] = GetUnityObjectNameString(renderer);
                rendererJson["type"] = "SkinnedMeshRenderer";
                rendererJson["rootBone"] = GetUnityObjectNameString(rootBone);
                rendererJson["mesh"] = {
                    {"name", GetUnityObjectNameString(mesh)},
                    {"vertexCount", GetMeshVertexCount(mesh)},
                    {"subMeshCount", GetMeshIntProperty(mesh, "get_subMeshCount")},
                    {"blendShapeCount", GetMeshIntProperty(mesh, "get_blendShapeCount")}
                };
                rendererJson["bones"] = nlohmann::json::array();
                if (bones) {
                    for (std::uintptr_t i = 0; i < bones->max_length; ++i) {
                        rendererJson["bones"].push_back(GetUnityObjectNameString(bones->At(static_cast<unsigned int>(i))));
                    }
                }

                rendererJson["materials"] = nlohmann::json::array();
                const auto materials = Renderer_get_sharedMaterials
                    ? reinterpret_cast<UnityArray<void*>*>(Renderer_get_sharedMaterials(renderer))
                    : nullptr;
                if (materials) {
                    for (std::uintptr_t i = 0; i < materials->max_length; ++i) {
                        auto materialJson = DumpMaterial(materials->At(static_cast<unsigned int>(i)));
                        materialJson["slot"] = i;
                        rendererJson["materials"].push_back(materialJson);
                    }
                }

                profile["renderers"].push_back(rendererJson);
            }

            std::error_code ec;
            const auto profileDir = Paths::Profiles();
            std::filesystem::create_directories(profileDir, ec);
            const auto outputPath = profileDir / (SanitizeFileName(sourceName) + ".profile.json");
            std::ofstream output(outputPath);
            if (!output.is_open()) {
                Log::ErrorFmt("[ModAsset] Cannot write source profile: %s", outputPath.string().c_str());
                return;
            }
            output << profile.dump(2);
            Log::InfoFmt("[ModAsset] Source profile dumped: %s renderers=%zu",
                outputPath.string().c_str(),
                renderers.size());
        }

        std::vector<void*> CopyObjectArray(UnityArray<void*>* array) {
            std::vector<void*> result;
            if (!array) return result;
            result.reserve(array->max_length);
            for (std::uintptr_t index = 0; index < array->max_length; ++index) {
                result.push_back(array->At(static_cast<unsigned int>(index)));
            }
            return result;
        }

        std::vector<std::string> CopyObjectNames(UnityArray<void*>* array) {
            std::vector<std::string> result;
            if (!array) return result;
            result.reserve(array->max_length);
            for (std::uintptr_t index = 0; index < array->max_length; ++index) {
                result.push_back(GetUnityObjectNameString(
                    array->At(static_cast<unsigned int>(index))));
            }
            return result;
        }

        void RegisterReversibleRendererPatch(
            const LocalModAssetReplacement& replacement,
            void* renderer,
            void* originalMesh,
            const int sourceRootDepth,
            const std::vector<void*>& originalMaterials,
            const std::vector<std::string>& originalBoneNames,
            const std::string& originalRootBoneName,
            void* patchedMesh,
            UnityArray<void*>* patchedMaterials) {
            if (!renderer) return;

            ReversibleRendererPatch patch{};
            patch.modId = replacement.modId;
            patch.sourceName = replacement.sourceName;
            patch.rendererName = GetUnityObjectNameString(renderer);
            patch.patchedRenderer = renderer;
            patch.patchedMesh = patchedMesh;
            patch.originalMesh = originalMesh;
            patch.sourceRootDepth = sourceRootDepth;
            patch.originalMaterials = originalMaterials;
            patch.originalBoneNames = originalBoneNames;
            patch.originalRootBoneName = originalRootBoneName;

            const auto currentMaterials = CopyObjectArray(patchedMaterials);
            patch.patchedMaterials = currentMaterials;
            for (size_t index = 0; index < currentMaterials.size(); ++index) {
                const auto original = index < patch.originalMaterials.size()
                    ? patch.originalMaterials[index]
                    : nullptr;
                if (currentMaterials[index] && currentMaterials[index] != original) {
                    patch.patchedMaterialKeys.push_back(currentMaterials[index]);
                }
            }

            if ((!patch.patchedMesh || patch.patchedMesh == patch.originalMesh)
                && patch.patchedMaterialKeys.empty()) {
                return;
            }

            std::lock_guard lock(g_reversiblePatchMutex);
            const auto duplicate = std::find_if(
                g_reversibleRendererPatches.begin(),
                g_reversibleRendererPatches.end(),
                [&patch](const auto& existing) {
                    return existing.modId == patch.modId
                        && existing.patchedRenderer == patch.patchedRenderer
                        && existing.patchedMesh == patch.patchedMesh;
                });
            if (duplicate == g_reversibleRendererPatches.end()) {
                g_reversibleRendererPatches.push_back(std::move(patch));
            }
        }

        UnityResolve::UnityType::Transform* FindTransformByName(
            void* hierarchyRoot,
            const std::string& name) {
            if (!hierarchyRoot || name.empty()) return nullptr;
            const auto transformClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
            if (!transformClass) return nullptr;
            const auto rootGameObject = reinterpret_cast<UnityResolve::UnityType::Transform*>(
                hierarchyRoot)->GetGameObject();
            if (!rootGameObject) return nullptr;
            const auto transforms = rootGameObject->GetComponentsInChildren<void*>(
                transformClass, true);
            for (const auto transform : transforms) {
                if (GetUnityObjectNameString(transform) == name) {
                    return reinterpret_cast<UnityResolve::UnityType::Transform*>(transform);
                }
            }
            return nullptr;
        }

        UnityArray<void*>* BuildRestoredBoneArray(
            void* renderer,
            const ReversibleRendererPatch& patch) {
            const auto transformClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
            if (!transformClass) return nullptr;
            if (patch.originalBoneNames.empty()) {
                return UnityArray<void*>::New(transformClass, 0);
            }
            const auto root = GetHierarchyRoot(renderer);
            if (!root) return nullptr;

            auto restored = UnityArray<void*>::New(
                transformClass, patch.originalBoneNames.size());
            for (size_t index = 0; index < patch.originalBoneNames.size(); ++index) {
                const auto bone = FindTransformByName(root, patch.originalBoneNames[index]);
                if (!bone) {
                    Log::ErrorFmt(
                        "[ModAsset] Hot restore missing original bone: mod=%s source=%s renderer=%s bone=%s",
                        patch.modId.c_str(),
                        patch.sourceName.c_str(),
                        GetUnityObjectNameString(renderer).c_str(),
                        patch.originalBoneNames[index].c_str());
                    return nullptr;
                }
                restored->At(static_cast<unsigned int>(index)) = bone;
            }
            return restored;
        }

        UnityArray<void*>* BuildRestoredMaterialArray(
            const ReversibleRendererPatch& patch) {
            const auto materialClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Material");
            if (!materialClass) return nullptr;
            auto restored = UnityArray<void*>::New(
                materialClass, patch.originalMaterials.size());
            for (size_t index = 0; index < patch.originalMaterials.size(); ++index) {
                restored->At(static_cast<unsigned int>(index)) = patch.originalMaterials[index];
            }
            return restored;
        }

        bool RendererMatchesPatch(
            void* renderer,
            const ReversibleRendererPatch& patch) {
            if (!renderer) return false;
            if (patch.patchedMesh
                && GetSkinnedMeshRendererSharedMesh(renderer) == patch.patchedMesh) {
                return true;
            }
            const auto materials = reinterpret_cast<UnityArray<void*>*>(
                GetRendererSharedMaterials(renderer));
            if (!materials || patch.patchedMaterialKeys.empty()) return false;
            for (std::uintptr_t index = 0; index < materials->max_length; ++index) {
                const auto material = materials->At(static_cast<unsigned int>(index));
                if (std::find(
                        patch.patchedMaterialKeys.begin(),
                        patch.patchedMaterialKeys.end(),
                        material) != patch.patchedMaterialKeys.end()) {
                    return true;
                }
            }
            return false;
        }

        void ClearRuntimePropertyBlocks(
            void* renderer,
            const size_t materialCount) {
            RestoreRendererPropertyBlockSnapshots(renderer, materialCount);
            std::lock_guard lock(g_materialOverrideMutex);
            g_rendererTextureOverrideCache.erase(renderer);
            g_loggedPersistentRenderers.erase(renderer);
        }

        void ReleaseRuntimeMeshClone(void* mesh) {
            if (!mesh) return;
            Il2CppGCHandle handle{};
            {
                std::lock_guard lock(g_runtimeMeshHandleMutex);
                const auto iter = g_runtimeMeshHandles.find(mesh);
                // Not ours: only clones this runtime made are destroyed here.
                if (iter == g_runtimeMeshHandles.end()) return;
                handle = iter->second;
                g_runtimeMeshHandles.erase(iter);
            }
            if (handle) UnityResolve::Invoke<void>("il2cpp_gchandle_free", handle);
            {
                std::lock_guard meshLock(g_runtimeMeshHandleMutex);
                g_transformedMeshSet.erase(mesh);
            }

            // Freeing the handle only lets the managed wrapper go.  A Mesh is a
            // native Unity object: its vertex/skin arrays stay allocated until
            // something destroys it, which is why releasing the handle alone
            // left memory climbing one ~16 MB clone per toggle.
            static auto Object_Destroy = reinterpret_cast<void (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine",
                    "Object", "Destroy", { "UnityEngine.Object" }));
            if (Object_Destroy) Object_Destroy(mesh);
            Log::InfoFmt("[ModAsset] Released hot-applied mesh clone: mesh=%p destroyed=%d",
                mesh, Object_Destroy ? 1 : 0);
        }

        // Put the vanilla textures back on whatever materials the renderer is
        // carrying now, without swapping the array.  A live actor's materials
        // are per-actor copies the game keeps writing to; replacing them with
        // the prefab's shared originals leaves the actor stripped of that state
        // and poisons the next hot ON.
        //
        // Two sources for "vanilla": the live path snapshots the texture it
        // overwrote, while the cold path never touched the original material,
        // so its untouched copy still holds the answer.
        void RestoreModTexturesOnRenderer(
            void* renderer,
            const ReversibleRendererPatch& patch) {
            const auto materials = reinterpret_cast<UnityArray<void*>*>(
                GetRendererSharedMaterials(renderer));
            if (!materials) return;
            for (std::uintptr_t i = 0; i < materials->max_length; ++i) {
                const auto live = materials->At(static_cast<unsigned int>(i));
                if (!live) continue;
                // The overrides were registered on whatever we patched, which is
                // the private clone on the cold path and the live material here.
                const auto patched = i < patch.patchedMaterials.size()
                    ? patch.patchedMaterials[i]
                    : live;
                const auto original = i < patch.originalMaterials.size()
                    ? patch.originalMaterials[i]
                    : nullptr;

                const auto entries = GetRegisteredMaterialTextureOverrides(patched);
                if (entries.empty()) continue;
                // Drop the registration first.  Material.SetTexture is hooked and
                // substitutes the registered Mod texture for whatever the caller
                // passes, so restoring while still registered writes the Mod
                // texture straight back -- the write "succeeds" and the log says
                // so, but nothing changes.
                {
                    std::lock_guard lock(g_materialOverrideMutex);
                    g_materialTextureOverrides.erase(patched);
                    g_materialTextureOverrides.erase(live);
                    g_rendererTextureOverrideCache.erase(renderer);
                }

                size_t restored = 0;
                for (const auto& entry : entries) {
                    const auto vanilla = entry.previousTexture
                        ? entry.previousTexture
                        : (original && original != patched
                            ? GetMaterialTexture(original, entry.propertyId)
                            : nullptr);
                    if (vanilla && SetMaterialTexture(live, entry.propertyId, vanilla)) {
                        ++restored;
                    }
                }
                Log::InfoFmt(
                    "[ModAsset] Restored original textures: renderer=%p slot=%u material=%p properties=%zu/%zu",
                    renderer, static_cast<unsigned>(i), live, restored, entries.size());
            }
        }

        size_t RestoreLiveModInstances(const std::string& modId) {
            // OFF 之后 renderer 的骨数组已经被还原，缓存里那份混合骨数组就是过期的：
            // 留着它，同一个 mod 原地更新后再 ON 会直接命中旧构建的骨。
            // 骨归属记录（g_createdBonesByOwner）**故意保留**：它只是一张"这个名字的骨是我
            // 建的"过滤表，key 里带 modId+指纹，别的 mod 撞不上；清掉反而会让 ON→OFF→ON
            // 每轮都重新建一套同名骨、在层级里越堆越多。骨已销毁的情况由复用处的存活检查兜。
            {
                std::lock_guard swingStateLock(g_swingStateMutex);
                const auto prefix = modId + "|";
                for (auto iter = g_hybridBonesByRenderer.begin(); iter != g_hybridBonesByRenderer.end();) {
                    iter = iter->second.ownerKey.starts_with(prefix)
                        ? g_hybridBonesByRenderer.erase(iter) : std::next(iter);
                }
                // OFF has to stop the animation bridge too, or it keeps writing rotations to
                // proxy bones nobody renders — the log showed `bound to live actor` still
                // firing after `Hot-restored renderer`.  Cleared wholesale rather than by mod:
                // a bridge is cheap to re-arm on the next ON, and the template carries no modId.
                g_sourceProxyBridges.clear();
                g_sourceProxyLiveBridges.clear();
                g_sourceProxyBridgesArmed.store(false, std::memory_order_relaxed);
            }
            std::vector<ReversibleRendererPatch> patches;
            {
                std::lock_guard lock(g_reversiblePatchMutex);
                for (auto iter = g_reversibleRendererPatches.begin();
                     iter != g_reversibleRendererPatches.end();) {
                    if (iter->modId == modId) {
                        patches.push_back(*iter);
                        iter = g_reversibleRendererPatches.erase(iter);
                    }
                    else {
                        ++iter;
                    }
                }
            }
            if (patches.empty()) return 0;

            // Keep only asset-level identities for a later re-enable.  Never carry
            // the scene renderer/GameObject pointers past this call: those objects
            // can disappear as soon as the current view changes.
            {
                std::lock_guard lock(g_reversiblePatchMutex);
                // Merged, not replaced: the load-time identities cover sources
                // this OFF had no live patch for, and the dedupe below keeps the
                // list from growing.
                auto& identities = g_reapplyRendererIdentities[modId];
                for (const auto& patch : patches) {
                    if (!patch.originalMesh) continue;
                    const ReapplyRendererIdentity identity{
                        patch.sourceName,
                        patch.originalMesh,
                        patch.sourceRootDepth,
                        patch.rendererName,
                    };
                    Detail::RememberReapplyRendererIdentity(identities, identity);
                }
            }

            // A cached renderer pointer can remain non-null (and can even pass
            // Unity's IsNativeObjectAlive check) after the actor was destroyed or
            // its native object address was reused.  Calling Renderer getters on
            // that raw pointer is a native crash boundary.  Take one current
            // SkinnedMeshRenderer snapshot first and only call Unity getters on
            // objects returned by that snapshot.  The cached pointer is used only
            // as a preference after membership in the current live set is proven.
            const auto rendererClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer");
            if (!rendererClass) return 0;
            const auto liveRenderers = rendererClass->FindObjectsByType<void*>();
            std::unordered_set<void*> liveRendererSet;
            for (const auto renderer : liveRenderers) {
                if (renderer && IsNativeObjectAlive(renderer)) liveRendererSet.emplace(renderer);
            }

            std::unordered_set<void*> seenRenderers;
            std::vector<std::pair<void*, const ReversibleRendererPatch*>> matches;
            const auto tryMatch = [&](void* renderer, const ReversibleRendererPatch& patch) {
                if (!renderer || !liveRendererSet.contains(renderer)
                    || seenRenderers.contains(renderer)
                    || !RendererMatchesPatch(renderer, patch)) {
                    return false;
                }
                seenRenderers.emplace(renderer);
                matches.emplace_back(renderer, &patch);
                return true;
            };

            // Prefer the original renderer when it is still part of the current
            // scene snapshot; otherwise resolve the patch against current objects.
            for (const auto& patch : patches) {
                if (patch.patchedRenderer) tryMatch(patch.patchedRenderer, patch);
            }
            for (const auto& patch : patches) {
                if (std::any_of(
                        matches.begin(), matches.end(),
                        [&patch](const auto& match) { return match.second == &patch; })) {
                    continue;
                }
                for (const auto renderer : liveRenderers) {
                    if (tryMatch(renderer, patch)) break;
                }
            }
            if (matches.size() < patches.size()) {
                Log::WarnFmt(
                    "[ModAsset] Hot restore skipped unmatched renderer patches: mod=%s patches=%zu matched=%zu liveRenderers=%zu",
                    modId.c_str(), patches.size(), matches.size(), liveRenderers.size());
            }

            size_t restoredCount = 0;
            for (const auto& [renderer, patch] : matches) {
                const auto restoredBones = BuildRestoredBoneArray(renderer, *patch);
                if (!restoredBones) continue;
                const auto restoredMaterials = BuildRestoredMaterialArray(*patch);
                if (!restoredMaterials) continue;

                SetSkinnedMeshRendererBones(renderer, restoredBones);
                if (!patch->originalRootBoneName.empty()) {
                    if (const auto root = GetHierarchyRoot(renderer)) {
                        SetSkinnedMeshRendererRootBone(
                            renderer,
                            FindTransformByName(root, patch->originalRootBoneName));
                    }
                }
                else {
                    SetSkinnedMeshRendererRootBone(renderer, nullptr);
                }
                // Only the renderer we actually handed a different array to gets
                // it back.  A cold-path patch is registered on the loaded
                // prefab; pushing its array onto a live instance would replace
                // the per-actor material copies the game maintains with the
                // pristine asset materials.
                // `patchedMaterialKeys` only covers per-key overrides; `replaceMaterials`
                // swaps the WHOLE array, and that undo was gated behind
                // `renderer == patch->patchedRenderer`.  Our patch is registered on the
                // PREFAB, while OFF walks the LIVE renderers — so the gate never opened, the
                // vanilla mesh came back still wearing the Mod's materials (the dark costume),
                // and the next ON cloned our own clone as its template, compounding per toggle.
                //
                // The gate exists for a real reason (pushing a prefab's array onto a live
                // instance would replace the per-actor material copies the game maintains), so
                // it stays — with a second door keyed on IDENTITY: if this renderer's slots are
                // pointer-for-pointer the array we installed, they cannot be the game's own
                // per-actor copies, and putting the originals back is exactly right.
                const auto liveMaterials = reinterpret_cast<UnityArray<void*>*>(
                    GetRendererSharedMaterials(renderer));
                const auto liveSlots = liveMaterials
                    ? static_cast<size_t>(liveMaterials->max_length) : 0;
                auto wearsOurArray = liveMaterials && !patch->patchedMaterials.empty()
                    && liveSlots == patch->patchedMaterials.size();
                for (size_t slot = 0; wearsOurArray && slot < patch->patchedMaterials.size(); ++slot) {
                    wearsOurArray = liveMaterials->At(static_cast<unsigned int>(slot))
                        == patch->patchedMaterials[slot];
                }
                // Measured, not assumed: after OFF the renderer still had `slots=5
                // expectedSlots=2`, with `isPatchTarget=0 wasWearingOurArray=0`.  Both
                // existing doors were shut — the patch is registered against the renderer of
                // an actor the game has since rebuilt, and pointer identity fails because the
                // game makes its own per-actor copies of whatever materials it finds
                // (`m_bdy(Clone) (Instance)` in the log).  The slot COUNT survives both:
                // the game never changes how many slots a renderer has, only we do.
                //
                // ponytail: a same-count array swap still needs identity, and identity still
                // loses to per-actor copies.  If that case ever appears, match on the
                // `_BaseMap` we carried in — a texture reference survives the copy.
                const auto slotCountIsOurs = !patch->patchedMaterials.empty()
                    && liveSlots == patch->patchedMaterials.size()
                    && liveSlots != patch->originalMaterials.size();
                if (wearsOurArray || slotCountIsOurs
                    || (renderer == patch->patchedRenderer
                        && !patch->patchedMaterialKeys.empty())) {
                    SetRendererSharedMaterials(renderer, restoredMaterials);
                }
                RestoreModTexturesOnRenderer(renderer, *patch);
                SetSkinnedMeshRendererSharedMesh(renderer, patch->originalMesh);
                ClearRuntimePropertyBlocks(renderer, patch->originalMaterials.size());
                RefreshSkinnedMeshRendererState(renderer);
                // The renderer is back on its original mesh, so let the clone
                // this patch installed go.  The next ON clones a fresh one; a
                // handle kept here is ~16 MB of Mod mesh leaked per toggle.
                //
                // But the clone is SHARED: the prefab and every actor instantiated from it
                // reference the same Mesh.  Any renderer still holding it is left pointing at
                // a destroyed mesh and draws nothing — the leading suspect for "the first
                // actor built after OFF has no body at all".  Measure it before acting on it.
                const auto target = patch->patchedRenderer;
                const auto targetHoldsClone = target && IsNativeObjectAlive(target)
                    && GetSkinnedMeshRendererSharedMesh(target) == patch->patchedMesh;
                ReleaseRuntimeMeshClone(patch->patchedMesh);

                ++restoredCount;
                // What the renderer actually looks like AFTER the restore.  "Hot-restored"
                // alone says the code ran, not that the renderer is whole again — and the
                // first actor rebuilt after OFF came back with no body at all, with zero log
                // lines in between, so the damage is whatever this leaves behind.
                // `isPatchTarget=1` means we just restored the PREFAB (the patch is registered
                // on it), which is what every later actor gets instantiated from.
                const auto boneArrayAfter = GetSkinnedMeshRendererBones(renderer);
                const auto slotsAfter = reinterpret_cast<UnityArray<void*>*>(
                    GetRendererSharedMaterials(renderer));
                Log::WarnFmt(
                    "[ModAsset] Hot-restored renderer: mod=%s source=%s renderer=%s mesh=%s isPatchTarget=%d wasWearingOurArray=%d targetHeldClone=%d bones=%zu expectedBones=%zu slots=%zu expectedSlots=%zu root=%s",
                    modId.c_str(),
                    patch->sourceName.c_str(),
                    GetUnityObjectNameString(renderer).c_str(),
                    GetUnityObjectNameString(patch->originalMesh).c_str(),
                    renderer == patch->patchedRenderer ? 1 : 0,
                    wearsOurArray ? 1 : 0,
                    targetHoldsClone ? 1 : 0,
                    boneArrayAfter ? static_cast<size_t>(boneArrayAfter->max_length) : 0,
                    patch->originalBoneNames.size(),
                    slotsAfter ? static_cast<size_t>(slotsAfter->max_length) : 0,
                    patch->originalMaterials.size(),
                    GetUnityObjectNameString(GetSkinnedMeshRendererRootBone(renderer)).c_str());
            }

            return restoredCount;
        }

        bool ApplySkinnedMeshReplacement(void* originalGameObject, void* modGameObject,
            const LocalModAssetReplacement& replacement,
            // A live actor's renderer carries per-actor material copies that the
            // game keeps writing to (lighting, decals, campus material setup).
            // Cloning those and installing the clone orphans the renderer from
            // every later write, which is what rendered a hot-applied costume
            // dark until a costume-page visit rebuilt the actor.  On a live
            // instance the Mod textures go straight onto the game's materials.
            const bool liveInstance = false) {
            if (!originalGameObject || !modGameObject) return false;
            const auto& sourceName = replacement.sourceName;

            DumpSourceProfileIfNeeded(sourceName, originalGameObject);

            const auto skinnedMeshRendererClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer");
            if (!skinnedMeshRendererClass) return false;

            auto originalRenderers = reinterpret_cast<UnityResolve::UnityType::GameObject*>(originalGameObject)
                ->GetComponentsInChildren<void*>(skinnedMeshRendererClass, true);
            auto modRenderers = reinterpret_cast<UnityResolve::UnityType::GameObject*>(modGameObject)
                ->GetComponentsInChildren<void*>(skinnedMeshRendererClass, true);

            if (originalRenderers.empty() || modRenderers.empty()) {
                Log::ErrorFmt("[ModAsset] SkinnedMeshRenderer not found: %s original=%zu replacement=%zu",
                    sourceName.c_str(), originalRenderers.size(), modRenderers.size());
                return false;
            }

            static auto SkinnedMeshRenderer_get_sharedMesh = reinterpret_cast<void* (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer", "get_sharedMesh"));
            static auto SkinnedMeshRenderer_set_sharedMesh = reinterpret_cast<void (*)(void*, void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer", "set_sharedMesh"));
            static auto SkinnedMeshRenderer_set_updateWhenOffscreen = reinterpret_cast<void (*)(void*, bool)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer", "set_updateWhenOffscreen"));

            if (!SkinnedMeshRenderer_get_sharedMesh || !SkinnedMeshRenderer_set_sharedMesh) {
                Log::Error("[ModAsset] Cannot resolve SkinnedMeshRenderer mesh accessors.");
                return false;
            }

            const auto rendererPairs = BuildRendererPairs(originalRenderers, modRenderers, replacement);
            size_t meshApplied = 0;
            size_t materialApplied = 0;
            size_t textureApplied = 0;
            size_t skippedMeshes = 0;

            for (size_t pairIndex = 0; pairIndex < rendererPairs.size(); ++pairIndex) {
                const auto& pair = rendererPairs[pairIndex];
                const auto originalMesh = SkinnedMeshRenderer_get_sharedMesh(pair.originalRenderer);
                const auto sourceModMesh = SkinnedMeshRenderer_get_sharedMesh(pair.modRenderer);
                const auto originalMaterials = GetRendererSharedMaterials(pair.originalRenderer);
                const auto modMaterials = GetRendererSharedMaterials(pair.modRenderer);
                const auto originalMaterialSnapshot = CopyObjectArray(
                    reinterpret_cast<UnityArray<void*>*>(originalMaterials));
                const auto originalBoneNames = CopyObjectNames(
                    GetSkinnedMeshRendererBones(pair.originalRenderer));
                const auto originalRootBoneName = GetUnityObjectNameString(
                    GetSkinnedMeshRendererRootBone(pair.originalRenderer));
                void* appliedMesh = nullptr;
                bool rendererMaterialApplied = false;

                LogSkinnedMeshRendererDiagnostics(sourceName, pair.originalIndex, pair.originalRenderer, pair.modRenderer,
                    originalMesh, sourceModMesh, "before");

                if (sourceModMesh) {
                    const auto clonedModMesh = CloneUnityObject(sourceModMesh, sourceName, pair.originalIndex);
                    const auto transformOk = clonedModMesh
                        && TransformModMeshVerticesToOriginalRendererSpace(pair.originalRenderer, pair.modRenderer,
                            clonedModMesh, sourceName, pair.originalIndex);
                    const auto skinningOk = transformOk
                        && PatchModMeshSkinningToOriginalOrder(pair.originalRenderer, pair.modRenderer, originalMesh,
                            clonedModMesh, replacement, sourceName, pair.originalIndex);

                    if (transformOk && skinningOk) {
                        SkinnedMeshRenderer_set_sharedMesh(pair.originalRenderer, clonedModMesh);
                        appliedMesh = clonedModMesh;
                        ++meshApplied;
                    }
                    else {
                        ++skippedMeshes;
                        Log::ErrorFmt("[ModAsset] Skipped mesh replacement because patch failed: %s renderer=%zu originalRenderer=\"%s\" modRenderer=\"%s\" sourceMesh=%p clonedMesh=%p transformOk=%d skinningOk=%d",
                            sourceName.c_str(),
                            pair.originalIndex,
                            GetUnityObjectNameString(pair.originalRenderer).c_str(),
                            GetUnityObjectNameString(pair.modRenderer).c_str(),
                            sourceModMesh,
                            clonedModMesh,
                            transformOk ? 1 : 0,
                            skinningOk ? 1 : 0);
                    }
                }
                else {
                    ++skippedMeshes;
                    Log::ErrorFmt("[ModAsset] Replacement renderer has no mesh: %s renderer=%zu modRenderer=\"%s\"",
                        sourceName.c_str(),
                        pair.originalIndex,
                        GetUnityObjectNameString(pair.modRenderer).c_str());
                }

                void* appliedMaterials = modMaterials;
                if (replacement.replaceMaterials && modMaterials) {
                    if (const auto adopted = AdoptGameMaterialsForModMesh(
                            reinterpret_cast<UnityArray<void*>*>(originalMaterials),
                            reinterpret_cast<UnityArray<void*>*>(modMaterials),
                            sourceName, pair.originalIndex)) {
                        appliedMaterials = adopted;
                    }
                    rendererMaterialApplied |= SetRendererSharedMaterials(
                        pair.originalRenderer, appliedMaterials);
                }
                const auto activeMaterials = replacement.replaceMaterials && modMaterials
                    ? appliedMaterials
                    : originalMaterials;
                if (!replacement.replaceMaterials
                    && !liveInstance
                    && (!replacement.materialCopies.empty()
                        || !replacement.materialTextures.empty()
                        || !replacement.materialColors.empty()
                        || !replacement.materialFloats.empty())) {
                    rendererMaterialApplied |= EnsureRendererPrivateMaterials(
                        pair.originalRenderer, activeMaterials, replacement, pair.originalIndex);
                }
                rendererMaterialApplied |= ApplyMaterialSlotCopies(
                    pair.originalRenderer, activeMaterials, replacement, pair.originalIndex);
                rendererMaterialApplied |= ApplyMaterialColorReplacements(
                    pair.originalRenderer, activeMaterials, replacement, pair.originalIndex);
                rendererMaterialApplied |= ApplyMaterialFloatReplacements(
                    pair.originalRenderer, activeMaterials, replacement, pair.originalIndex);
                if (ApplyMaterialTextureReplacements(pair.originalRenderer, activeMaterials, replacement, pair.originalIndex)) {
                    rendererMaterialApplied = true;
                    ++textureApplied;
                }
                // 半透明段放在最后：它会把 sharedMaterials 数组整个换掉（原版 body 只有
                // bdy/bdyco 两槽，我们的网格多出来的段没有槽就会被 Unity 静默丢掉）。
                rendererMaterialApplied |= ApplyTransparentMaterials(
                    pair.originalRenderer, replacement, pair.originalIndex);
                // Unity draws submesh i with materials[i] and DROPS every submesh past the
                // end of the array, so a mesh with more submeshes than the renderer has
                // slots loses geometry silently — and whatever lands on a slot authored for
                // the vanilla transparent pass (`m_bdyco`, no depth write) reads as
                // one-sided with broken depth.  Both are picture-level bugs with no error.
                if (appliedMesh) {
                    const auto slots = reinterpret_cast<UnityArray<void*>*>(
                        GetRendererSharedMaterials(pair.originalRenderer));
                    const auto slotCount = slots ? static_cast<size_t>(slots->max_length) : 0;
                    const auto subMeshes = GetMeshIntProperty(appliedMesh, "get_subMeshCount");
                    std::string names;
                    for (size_t index = 0; index < slotCount; ++index) {
                        names += (names.empty() ? "" : ", ")
                            + GetUnityObjectNameString(slots->At(static_cast<unsigned int>(index)));
                    }
                    Log::WarnFmt("[ModAsset] Renderer material slots vs mesh submeshes: %s renderer=%zu submeshes=%d slots=%zu unpainted=%d materials=[%s]",
                        sourceName.c_str(), pair.originalIndex, subMeshes, slotCount,
                        subMeshes > static_cast<int>(slotCount)
                            ? subMeshes - static_cast<int>(slotCount) : 0,
                        names.c_str());
                }
                // Property blocks only exist here to force our textures past a
                // block the game was believed to own -- a theory since
                // disproven.  On a live instance the textures are already on the
                // game's own material, so the block adds nothing and its
                // per-slot SetPropertyBlock is what rendered the hot-applied
                // costume dark (OFF cleared the blocks and the same materials
                // with the same Mod textures went back to normal).
                if (!liveInstance) ApplyPersistentTextureOverrides(pair.originalRenderer);
                if (rendererMaterialApplied) ++materialApplied;
                if (SkinnedMeshRenderer_set_updateWhenOffscreen) {
                    SkinnedMeshRenderer_set_updateWhenOffscreen(pair.originalRenderer, true);
                }

                const auto currentOriginalMesh = SkinnedMeshRenderer_get_sharedMesh(pair.originalRenderer);
                RegisterReversibleRendererPatch(
                    replacement,
                    pair.originalRenderer,
                    originalMesh,
                    GetComponentDepthFromRoot(pair.originalRenderer, originalGameObject),
                    originalMaterialSnapshot,
                    originalBoneNames,
                    originalRootBoneName,
                    currentOriginalMesh,
                    reinterpret_cast<UnityArray<void*>*>(
                        GetRendererSharedMaterials(pair.originalRenderer)));
                RefreshSkinnedMeshRendererState(pair.originalRenderer);
                LogSkinnedMeshRendererDiagnostics(sourceName, pair.originalIndex, pair.originalRenderer, pair.modRenderer,
                    currentOriginalMesh, appliedMesh ? appliedMesh : sourceModMesh, "after");

                Log::InfoFmt("[ModAsset] SkinnedMeshRenderer pair processed: %s pair=%zu originalRenderer=%zu modRenderer=%zu originalName=\"%s\" modName=\"%s\" meshApplied=%d materials=%p replaceMaterials=%d",
                    sourceName.c_str(),
                    pairIndex,
                    pair.originalIndex,
                    pair.modIndex,
                    GetUnityObjectNameString(pair.originalRenderer).c_str(),
                    GetUnityObjectNameString(pair.modRenderer).c_str(),
                    appliedMesh ? 1 : 0,
                    modMaterials,
                    replacement.replaceMaterials ? 1 : 0);
            }

            Log::InfoFmt("[ModAsset] SkinnedMeshRenderer replacement finished: %s originalRenderers=%zu replacementRenderers=%zu pairs=%zu meshApplied=%zu materialApplied=%zu textureApplied=%zu skippedMeshes=%zu",
                sourceName.c_str(),
                originalRenderers.size(),
                modRenderers.size(),
                rendererPairs.size(),
                meshApplied,
                materialApplied,
                textureApplied,
                skippedMeshes);
            return meshApplied > 0 || materialApplied > 0;
        }

        void AddUniqueLiveObject(std::vector<void*>& objects, void* object) {
            if (!object || !IsNativeObjectAlive(object)
                || std::find(objects.begin(), objects.end(), object) != objects.end()) {
                return;
            }
            objects.push_back(object);
        }

        bool IsGameObjectInsideRigRoot(
            void* gameObject,
            void* rigRootTransform) {
            if (!gameObject || !rigRootTransform) return false;
            auto current = reinterpret_cast<UnityResolve::UnityType::GameObject*>(
                gameObject)->GetTransform();
            while (current && IsNativeObjectAlive(current)) {
                if (current == rigRootTransform) return true;
                current = current->GetParent();
            }
            return false;
        }

        bool ReactivateGameObject(void* gameObject) {
            if (!gameObject || !IsNativeObjectAlive(gameObject)) return false;
            static auto getActiveSelf = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject", "get_activeSelf");
            static auto setActive = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject", "SetActive",
                { "System.Boolean" });
            if (!getActiveSelf || !getActiveSelf->function
                || !setActive || !setActive->function) {
                return false;
            }

            using GetActiveFn = bool (*)(void*, void*);
            using SetActiveFn = void (*)(void*, bool, void*);
            const auto activeSelf = reinterpret_cast<GetActiveFn>(getActiveSelf->function)(
                gameObject, getActiveSelf->address);
            if (!activeSelf) return false;
            reinterpret_cast<SetActiveFn>(setActive->function)(
                gameObject, false, setActive->address);
            reinterpret_cast<SetActiveFn>(setActive->function)(
                gameObject, true, setActive->address);
            return true;
        }

        // Do not call CampusActorModelParts.InitializeCampusMaterials() on a
        // live actor to refresh derived material state.  2026-08-09: it replaced
        // the renderer's whole material array with materials whose shader has
        // zero properties (audit printed keywords=[] floats=[]), which killed
        // both the Mod and every later toggle.
        size_t RefreshAnimationRigsAfterHotReapply(
            const std::vector<void*>& targets) {
            std::vector<ActiveAnimationRigContext> contexts;
            {
                std::lock_guard lock(g_animationRigMutex);
                std::erase_if(g_activeAnimationRigs, [](const auto& context) {
                    return !context.rig
                        || !context.rootTransform
                        || !context.rootGameObject
                        || !IsNativeObjectAlive(context.rig)
                        || !IsNativeObjectAlive(context.rootTransform)
                        || !IsNativeObjectAlive(context.rootGameObject);
                });
                contexts = g_activeAnimationRigs;
            }

            size_t refreshed = 0;
            for (const auto& context : contexts) {
                std::vector<void*> matchingTargets;
                for (const auto target : targets) {
                    if (IsGameObjectInsideRigRoot(target, context.rootTransform)) {
                        AddUniqueLiveObject(matchingTargets, target);
                    }
                }
                if (matchingTargets.empty()) continue;

                // 摇物骨和链是 graft 时长在 prefab 上、由游戏 Instantiate 后自己收走的，
                // 热切换够不着那一步：这里只刷新网格/材质并补碰撞体。新增摇物链要生效必须
                // 重新进场景。（以前这里往 initializeData 追加再重跑 RegisterBones，结果是
                // 并行表失衡抛异常，摆动一样没有，还得靠 SEH 兜住换装开关。）
                size_t reactivatedTargets = 0;
                for (const auto target : matchingTargets) {
                    if (ReactivateGameObject(target)) ++reactivatedTargets;
                }
                if (reactivatedTargets > 0) {
                    ++refreshed;
                    Log::InfoFmt(
                        "[ModAsset] Hot-refreshed active character target: root=%s reactivatedTargets=%zu (新增摇物链需重新进入场景)",
                        GetUnityObjectNameString(context.rootGameObject).c_str(),
                        reactivatedTargets);
                }
            }
            return refreshed;
        }

        std::vector<void*> CollectLiveReapplyTargets(
            const LocalModAssetReplacement& replacement,
            void* observedRenderer = nullptr) {
            std::vector<void*> targets;
            std::vector<ReapplyRendererIdentity> identities;
            {
                std::lock_guard lock(g_reversiblePatchMutex);
                if (const auto remembered = g_reapplyRendererIdentities.find(replacement.modId);
                    remembered != g_reapplyRendererIdentities.end()) {
                    for (const auto& identity : remembered->second) {
                        if (NormalizeAssetName(identity.sourceName)
                            == NormalizeAssetName(replacement.sourceName)) {
                            identities.push_back(identity);
                        }
                    }
                }
            }

            const auto rendererClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer");
            if (!rendererClass || identities.empty()) return targets;

            // Resolve only against the current renderer snapshot.  The previous
            // implementation walked GameObjects retained from earlier scenes;
            // IsNativeObjectAlive was not enough to make that raw pointer safe.
            const auto tryCollect = [&](void* renderer) {
                if (!renderer || !IsNativeObjectAlive(renderer)) return;
                const auto mesh = GetSkinnedMeshRendererSharedMesh(renderer);
                if (!mesh) return;
                const auto rendererName = GetUnityObjectNameString(renderer);
                const auto identity = std::find_if(
                    identities.begin(),
                    identities.end(),
                    [mesh, &rendererName](const auto& current) {
                        return current.originalMesh == mesh
                            && (current.rendererName.empty()
                                || current.rendererName == rendererName);
                    });
                if (identity == identities.end()) return;
                AddUniqueLiveObject(
                    targets,
                    GetSourceRootGameObject(renderer, identity->sourceRootDepth));
            };

            // Renderer lifecycle hooks can hand us a current, valid renderer
            // before FindObjectsByType starts returning inactive/initializing
            // scene objects.  It is safe to inspect for this call only; it is
            // never retained in the pending queue.
            tryCollect(observedRenderer);
            const auto liveRenderers = rendererClass->FindObjectsByType<void*>();
            for (const auto renderer : liveRenderers) {
                tryCollect(renderer);
            }
            return targets;
        }

        size_t ReapplyLiveModInstances(
            LocalModAssetReplacement& replacement,
            const bool refreshAnimationRigs = true,
            void* observedRenderer = nullptr) {
            if (replacement.replaceWholeObject || replacement.attachToOriginal) {
                Log::WarnFmt(
                    "[ModAsset] Hot reapply unsupported for whole-object/attach rule: mod=%s source=%s",
                    replacement.modId.c_str(),
                    replacement.sourceName.c_str());
                return 0;
            }

            const auto targets = CollectLiveReapplyTargets(
                replacement, observedRenderer);
            if (targets.empty()) return 0;
            const auto modAsset = LoadLocalModReplacementAsset(replacement);
            if (!modAsset) return 0;

            size_t applied = 0;
            for (const auto target : targets) {
                if (ApplySkinnedMeshReplacement(target, modAsset, replacement, true)) ++applied;
            }
            for (const auto target : targets) {
                Log::InfoFmt("[ModAsset] Hot reapply target: object=%p name=\"%s\"",
                    target,
                    GetUnityObjectNameString(target).c_str());
            }
            const auto refreshedRigs = refreshAnimationRigs
                ? RefreshAnimationRigsAfterHotReapply(targets)
                : 0;
            Log::InfoFmt(
                "[ModAsset] Hot reapply finished: mod=%s source=%s targets=%zu applied=%zu refreshedRigs=%zu",
                replacement.modId.c_str(),
                replacement.sourceName.c_str(),
                targets.size(),
                applied,
                refreshedRigs);
            return applied;
        }

        void QueuePendingLiveReapply(
            const std::string& modId,
            const std::string& sourceName) {
            const auto sourceKey = NormalizeAssetName(sourceName);
            bool queued = false;
            {
                std::lock_guard lock(g_pendingReapplyMutex);
                queued = Detail::QueuePendingReapply(
                    g_pendingReapplies, modId, sourceKey);
                g_hasPendingReapplies.store(!g_pendingReapplies.empty());
            }
            if (queued) {
                Log::InfoFmt(
                    "[ModAsset] Deferred hot reapply queued: mod=%s source=%s",
                    modId.c_str(), sourceName.c_str());
            }
        }

        void ClearPendingLiveReapply(
            const std::string& modId,
            const std::string& sourceName) {
            std::lock_guard lock(g_pendingReapplyMutex);
            Detail::ClearPendingReapply(
                g_pendingReapplies, modId, NormalizeAssetName(sourceName));
            g_hasPendingReapplies.store(!g_pendingReapplies.empty());
        }

        void ClearPendingLiveReappliesForMod(const std::string& modId) {
            std::lock_guard lock(g_pendingReapplyMutex);
            Detail::ClearPendingReappliesForMod(g_pendingReapplies, modId);
            g_hasPendingReapplies.store(!g_pendingReapplies.empty());
        }

        std::vector<void*> SnapshotPatchedMeshes(
            const Detail::PendingReapplyRequest& request) {
            std::vector<void*> patchedMeshes;
            std::lock_guard patchLock(g_reversiblePatchMutex);
            for (const auto& patch : g_reversibleRendererPatches) {
                if (patch.modId == request.modId
                    && NormalizeAssetName(patch.sourceName) == request.sourceKey
                    && patch.patchedMesh) {
                    patchedMeshes.push_back(patch.patchedMesh);
                }
            }
            return patchedMeshes;
        }

        bool HasLivePatchedRenderer(
            const Detail::PendingReapplyRequest& request,
            void* observedRenderer) {
            const auto patchedMeshes = SnapshotPatchedMeshes(request);
            if (patchedMeshes.empty()) return false;

            const auto rendererClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer");
            if (!rendererClass) return false;
            const auto matchesPatchedMesh = [&](void* renderer) {
                if (!renderer || !IsNativeObjectAlive(renderer)) return false;
                const auto mesh = GetSkinnedMeshRendererSharedMesh(renderer);
                return std::find(patchedMeshes.begin(), patchedMeshes.end(), mesh)
                    != patchedMeshes.end();
            };
            if (matchesPatchedMesh(observedRenderer)) return true;
            for (const auto renderer : rendererClass->FindObjectsByType<void*>()) {
                if (matchesPatchedMesh(renderer)) return true;
            }
            return false;
        }

        void RetryPendingLiveReapplies(
            const char* trigger,
            void* observedRenderer) {
            if (!g_hasPendingReapplies.load()) return;
            // 触发点里有 Renderer.SetPropertyBlock —— 那是**逐帧逐 renderer**的调用，而每次
            // 重试都要 FindObjectsByType 扫全场景。一条永远满足不了的请求（在主页开一个当前
            // 不在场景里的 mod，很常见）会因此把全场景扫描摊到每一帧。节流到 250ms 一次：
            // 能满足它的是"角色出现"这种秒级事件，晚一拍没有代价。
            // ponytail: 时间闸够用；要更准就改成只挂角色生命周期 hook。
            using Clock = std::chrono::steady_clock;
            static std::atomic<Clock::rep> lastAttempt{};
            static const auto minInterval = std::chrono::duration_cast<Clock::duration>(
                std::chrono::milliseconds(250)).count();
            const auto now = Clock::now().time_since_epoch().count();
            auto previous = lastAttempt.load();
            if (previous != 0 && now - previous < minInterval) return;
            if (!lastAttempt.compare_exchange_strong(previous, now)) return;
            if (g_pendingReapplyInFlight.exchange(true)) return;
            // 中途 return / 抛出都要把在飞标志放掉，否则整个进程再也不会重试。
            struct InFlightGuard {
                ~InFlightGuard() { g_pendingReapplyInFlight.store(false); }
            } inFlightGuard;

            std::vector<Detail::PendingReapplyRequest> requests;
            {
                std::lock_guard lock(g_pendingReapplyMutex);
                requests = g_pendingReapplies;
            }
            for (const auto& request : requests) {
                const auto replacement = FindLocalModAssetReplacement(request.sourceKey);
                if (!replacement || replacement->modId != request.modId) {
                    ClearPendingLiveReapply(request.modId, request.sourceKey);
                    continue;
                }

                // This retry runs from an actor/renderer lifecycle callback.  The
                // object is already being initialized, so toggling its whole root
                // inactive here would re-enter the callback.  The mesh path still
                // refreshes the renderer and bounds itself.
                const auto applied = ReapplyLiveModInstances(
                    *replacement, false, observedRenderer);
                const auto alreadyPatched = applied == 0
                    && HasLivePatchedRenderer(request, observedRenderer);
                if (applied > 0 || alreadyPatched) {
                    ClearPendingLiveReapply(request.modId, request.sourceKey);
                    Log::InfoFmt(
                        "[ModAsset] Deferred hot reapply satisfied: trigger=%s mod=%s source=%s applied=%zu alreadyPatched=%d",
                        trigger ? trigger : "unknown",
                        request.modId.c_str(),
                        replacement->sourceName.c_str(),
                        applied,
                        alreadyPatched ? 1 : 0);
                }
            }
        }

        // A Mod that was off when the game started has never been applied, so the
        // reversible registry is empty -- and until now the only writer of the
        // reapply identities was a preceding OFF.  Turning such a Mod on logged
        // "hotInstances=0" and changed nothing on screen; only a costume-page
        // visit, which reloads the asset, made it appear.  The identity a hot ON
        // needs is just the source asset's original renderer name and mesh, and
        // every load walks past that whether the Mod is enabled or not.
        void RememberSourceRendererIdentities(void* originalAsset, const std::string& sourceName) {
            if (!originalAsset
                || std::strcmp(GetUnityObjectClassName(originalAsset), "GameObject") != 0) {
                return;
            }

            std::vector<LocalModAssetReplacementPtr> candidates;
            {
                std::shared_lock replacementLock(g_replacementMutex);
                for (const auto& replacement : g_registeredReplacements) {
                    if (!replacement
                        || replacement->replaceWholeObject
                        || replacement->attachToOriginal) {
                        continue;
                    }
                    if (NormalizeAssetName(replacement->sourceName)
                        != NormalizeAssetName(sourceName)) {
                        continue;
                    }
                    candidates.push_back(replacement);
                }
            }
            if (candidates.empty()) return;

            const auto rendererClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer");
            if (!rendererClass) return;
            const auto renderers = reinterpret_cast<UnityResolve::UnityType::GameObject*>(
                originalAsset)->GetComponentsInChildren<void*>(rendererClass, true);
            if (renderers.empty()) return;

            struct ObservedRendererIdentity {
                void* mesh{};
                int sourceRootDepth{};
                std::string rendererName;
            };
            std::vector<ObservedRendererIdentity> observed;
            observed.reserve(renderers.size());
            for (const auto renderer : renderers) {
                const auto mesh = GetSkinnedMeshRendererSharedMesh(renderer);
                // A cached prefab may pass through this hook again after an
                // in-place replacement.  Recording our clone as "original"
                // would make the next ON target an already-patched renderer.
                if (!mesh || IsRuntimeOwnedMesh(mesh)) continue;
                observed.push_back({
                    mesh,
                    GetComponentDepthFromRoot(renderer, originalAsset),
                    GetUnityObjectNameString(renderer),
                });
            }
            if (observed.empty()) return;

            std::lock_guard lock(g_reversiblePatchMutex);
            for (const auto& replacement : candidates) {
                auto& identities = g_reapplyRendererIdentities[replacement->modId];
                for (const auto& current : observed) {
                    const ReapplyRendererIdentity identity{
                        replacement->sourceName,
                        current.mesh,
                        current.sourceRootDepth,
                        current.rendererName,
                    };
                    if (!Detail::RememberReapplyRendererIdentity(
                            identities, identity)) continue;
                    Log::InfoFmt(
                        "[ModAsset] Remembered hot-reapply identity: mod=%s source=%s renderer=\"%s\" mesh=%p depth=%d",
                        replacement->modId.c_str(),
                        identity.sourceName.c_str(),
                        identity.rendererName.c_str(),
                        identity.originalMesh,
                        identity.sourceRootDepth);
                }
            }
        }

        // Copy the replaced asset's layer onto every object of the replacement.
        //
        // Whole-object replacement hands the game a prefab built in another Unity project, where
        // everything sits on layer 0.  The game's actor camera culls by layer, so the body renders
        // nowhere while still casting a shadow — "built fine, draws nothing".
        void AdoptLayerFromReplacedAsset(void* originalAsset, void* modAsset,
            const std::string& sourceName) {
            static auto GameObject_get_layer = reinterpret_cast<int (*)(void*)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine",
                    "GameObject", "get_layer"));
            static auto GameObject_set_layer = reinterpret_cast<void (*)(void*, int)>(
                Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine",
                    "GameObject", "set_layer"));
            const auto transformClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
            if (!originalAsset || !modAsset || !GameObject_get_layer || !GameObject_set_layer
                || !transformClass) {
                return;
            }

            const auto targetLayer = GameObject_get_layer(originalAsset);
            size_t moved = 0;
            for (const auto transform :
                reinterpret_cast<UnityResolve::UnityType::GameObject*>(modAsset)
                    ->GetComponentsInChildren<void*>(transformClass, true)) {
                const auto gameObject = reinterpret_cast<UnityResolve::UnityType::Transform*>(
                    transform)->GetGameObject();
                if (!gameObject || GameObject_get_layer(gameObject) == targetLayer) continue;
                GameObject_set_layer(gameObject, targetLayer);
                ++moved;
            }
            Log::WarnFmt("[ModAsset] Adopted layer from replaced asset: %s layer=%d moved=%zu",
                sourceName.c_str(), targetLayer, moved);
        }

        // Same adoption as the mesh-patching path, but the renderer that keeps the result is
        // ours: under whole-object replacement the game builds the actor from OUR prefab, so
        // the game's materials have to be moved onto our renderers before it is handed over.
        void AdoptGameMaterialsForWholeObject(void* originalAsset, void* modAsset,
            const std::string& sourceName) {
            const auto rendererClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer");
            if (!originalAsset || !modAsset || !rendererClass) return;

            const auto originalRenderers = reinterpret_cast<UnityResolve::UnityType::GameObject*>(
                originalAsset)->GetComponentsInChildren<void*>(rendererClass, true);
            const auto modRenderers = reinterpret_cast<UnityResolve::UnityType::GameObject*>(
                modAsset)->GetComponentsInChildren<void*>(rendererClass, true);
            if (originalRenderers.empty() || modRenderers.empty()) {
                Log::WarnFmt("[ModAsset] Whole-object material adoption found no renderer pair: %s original=%zu mod=%zu",
                    sourceName.c_str(), originalRenderers.size(), modRenderers.size());
                return;
            }

            // ponytail: every mod renderer adopts from the FIRST vanilla renderer.  A body part
            // is one renderer in every stock package inspected so far; a package with several
            // would need a rule, and the log above is what would show it.
            const auto templateMaterials = reinterpret_cast<UnityArray<void*>*>(
                GetRendererSharedMaterials(originalRenderers.front()));
            for (const auto modRenderer : modRenderers) {
                const auto adopted = AdoptGameMaterialsForModMesh(
                    templateMaterials,
                    reinterpret_cast<UnityArray<void*>*>(GetRendererSharedMaterials(modRenderer)),
                    sourceName, 0);
                if (adopted) SetRendererSharedMaterials(modRenderer, adopted);
            }
        }

        // What the game reads off a body prefab is a CONTRACT, and every stock body satisfies it:
        // any of the 530 costumes works in any scene with any prop, which is the proof that the
        // contract is uniform rather than per-scene.  So the target is mechanical — be
        // structurally indistinguishable from the asset we stand in for — and this lists what is
        // still missing in one pass, instead of chasing one prop at a time.
        //
        // Reported in two groups because they mean opposite things: a missing `*_S` / `*_A` bone
        // is the REPLACED COSTUME's own swing rig and we are supposed not to have it, while a
        // missing structural node or component class is a contract we have not taken over yet.
        void LogWholeObjectContractGap(void* originalAsset, void* modAsset,
            const std::string& sourceName) {
            const auto transformClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
            const auto componentClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Component");
            if (!originalAsset || !modAsset || !transformClass || !componentClass) return;

            const auto names = [&](void* asset) {
                std::unordered_set<std::string> result;
                for (const auto transform :
                    reinterpret_cast<UnityResolve::UnityType::GameObject*>(asset)
                        ->GetComponentsInChildren<void*>(transformClass, true)) {
                    result.insert(GetUnityObjectNameString(transform));
                }
                return result;
            };
            const auto componentClasses = [&](void* asset) {
                std::unordered_set<std::string> result;
                for (const auto component :
                    reinterpret_cast<UnityResolve::UnityType::GameObject*>(asset)
                        ->GetComponentsInChildren<void*>(componentClass, true)) {
                    result.insert(GetUnityObjectClassName(component));
                }
                return result;
            };

            const auto ours = names(modAsset);
            std::string structural;
            size_t costumeBones = 0;
            size_t structuralCount = 0;
            for (const auto& name : names(originalAsset)) {
                if (ours.contains(name)) continue;
                // Only the replaced COSTUME's own swing rig is excusable to lack.  `_H` and `_O`
                // were in this list and should never have been: `*_H` is the game's 14-bone
                // joint-correction rig and `Skirt_*_O` is base structure — both belong to the
                // 70 base bones every stock body carries, which under whole-object replacement
                // is contract, not decoration.  Filtering them made the gap look smaller than
                // it is, which is the worst thing a ruler can do.
                if (name.ends_with("_S") || name.ends_with("_A") || name.ends_with("_S_End")) {
                    ++costumeBones;
                    continue;
                }
                ++structuralCount;
                if (structural.size() < 1200) {
                    structural += (structural.empty() ? "" : ", ") + name;
                }
            }

            const auto ourComponents = componentClasses(modAsset);
            std::string missingComponents;
            for (const auto& name : componentClasses(originalAsset)) {
                if (ourComponents.contains(name)) continue;
                missingComponents += (missingComponents.empty() ? "" : ", ") + name;
            }

            Log::WarnFmt("[ModAsset] Whole-object contract gap: %s missingNodes=%zu missingComponentClasses=[%s] nodes=[%s] costumeRigBonesSkipped=%zu",
                sourceName.c_str(), structuralCount,
                missingComponents.empty() ? "none" : missingComponents.c_str(),
                structural.empty() ? "none" : structural.c_str(), costumeBones);
        }

        // Grow the bare socket nodes the replaced body has and ours does not.
        //
        // Measured on `atbm-cstm-0140`: `Left/RightHand1_E`, `Left/RightHand{1,2}_I` and
        // `Reference{1,2}_I` — sockets on the hands and at the body root.  Props hang off these:
        // the microphone is held in a hand and the dressing-room curtain is pulled from the root,
        // and both sat at the world origin while these were missing.
        //
        // Copied from the asset being replaced rather than named in code: the positions are that
        // body's, and a hardcoded list would be wrong for the next one.  The offline skeleton dump
        // could never have found them — they carry no weights, so they are not in the bone list at
        // all, which is why the earlier "no prop socket is missing" reading was worthless.
        size_t GrowMissingSocketNodes(void* originalAsset, void* modAsset,
            const std::string& sourceName) {
            const auto transformClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
            const auto componentClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Component");
            const auto gameObjectClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
            if (!originalAsset || !modAsset || !transformClass || !componentClass || !gameObjectClass) {
                return 0;
            }

            std::unordered_map<std::string, UnityResolve::UnityType::Transform*> ours;
            for (const auto transform :
                reinterpret_cast<UnityResolve::UnityType::GameObject*>(modAsset)
                    ->GetComponentsInChildren<void*>(transformClass, true)) {
                ours.emplace(GetUnityObjectNameString(transform),
                    reinterpret_cast<UnityResolve::UnityType::Transform*>(transform));
            }

            std::string grown;
            std::string deltaReport;
            size_t count = 0;
            for (const auto candidate :
                reinterpret_cast<UnityResolve::UnityType::GameObject*>(originalAsset)
                    ->GetComponentsInChildren<void*>(transformClass, true)) {
                const auto source = reinterpret_cast<UnityResolve::UnityType::Transform*>(candidate);
                const auto name = GetUnityObjectNameString(source);
                if (name.empty() || ours.contains(name)) continue;
                // A socket is a BARE leaf: one component (its own Transform) and no children.
                // Anything else is the replaced costume's own rig — swing bones, driver hosts —
                // which we are supposed not to have.
                const auto sourceObject = source->GetGameObject();
                if (!sourceObject) continue;
                if (sourceObject->GetComponentsInChildren<void*>(componentClass, true).size() != 1) {
                    continue;
                }
                const auto parent = source->GetParent();
                if (!parent) continue;
                // The two ROOTS correspond by definition — one asset is standing in for the
                // other — but their names differ, so name matching drops exactly the sockets
                // parented at the root.  That is where `Reference{1,2}_I` live, and with them
                // missing the dressing-room curtain had nothing to hang on.
                UnityResolve::UnityType::Transform* host = nullptr;
                if (parent == reinterpret_cast<UnityResolve::UnityType::GameObject*>(
                        originalAsset)->GetTransform()) {
                    host = reinterpret_cast<UnityResolve::UnityType::GameObject*>(
                        modAsset)->GetTransform();
                }
                else if (const auto found = ours.find(GetUnityObjectNameString(parent));
                         found != ours.end()) {
                    host = found->second;
                }
                if (!host) continue;

                // The socket's local pose was authored in the VANILLA host bone's frame.  Our
                // bone's rest orientation is not the same one, so copying the local pose
                // verbatim lands the prop rotated by exactly that difference — the microphone
                // sat in the hand but pointing wrong.  Re-express it: with C the rest-rotation
                // difference between the two hosts, localPose' = C^-1 * localPose.
                const auto hostDelta = MultiplyQuaternion(
                    InvertQuaternion(parent->GetRotation()), host->GetRotation());
                const auto correction = InvertQuaternion(hostDelta);
                const auto sourceLocal = source->GetLocalPosition();
                const auto rotated = RotateVectorByQuaternion(correction, sourceLocal);

                const auto created = gameObjectClass->New<UnityResolve::UnityType::GameObject>();
                if (!created) continue;
                UnityResolve::UnityType::GameObject::Create(created, name);
                const auto transform = created->GetTransform();
                if (!transform || !SetTransformParent(transform, host)) continue;
                transform->SetLocalPosition(rotated);
                transform->SetLocalRotation(MultiplyQuaternion(correction, source->GetLocalRotation()));
                transform->SetLocalScale(source->GetLocalScale());
                const auto dot = std::fabs(hostDelta.w);
                deltaReport += (deltaReport.empty() ? "" : ", ") + name + ":"
                    + std::to_string(static_cast<int>(
                        2.0f * std::acos(dot > 1.0f ? 1.0f : dot) * 57.2957795f)) + "deg";
                ours.emplace(name, transform);
                grown += (grown.empty() ? "" : ", ") + name;
                ++count;
            }
            if (count) {
                Log::WarnFmt("[ModAsset] Grew missing socket nodes from replaced asset: %s count=%zu nodes=[%s] hostRestDelta=[%s]",
                    sourceName.c_str(), count, grown.c_str(), deltaReport.c_str());
            }
            return count;
        }

        void* ReplaceLocalModAssetIfNeeded(void* originalResult, const std::string& sourceName) {
            RememberSourceRendererIdentities(originalResult, sourceName);

            const auto replacement = FindLocalModAssetReplacement(sourceName);
            if (!replacement) return originalResult;

            Log::InfoFmt("[ModAsset] Replacement hit: %s -> %s mod=%s part=%s priority=%d",
                sourceName.c_str(),
                replacement->assetName.c_str(),
                replacement->modName.c_str(),
                replacement->part.c_str(),
                replacement->priority);
            const auto modAsset = LoadLocalModReplacementAsset(*replacement);
            if (!modAsset) {
                Log::ErrorFmt("[ModAsset] Replacement failed, keeping original asset: %s", sourceName.c_str());
                return originalResult;
            }

            if (replacement->attachToOriginal) {
                g_nativeChainValidation = true;
                replacement->attachAsset = modAsset;
                replacement->attachSourceMeshes.clear();
                const auto originalGo = reinterpret_cast<UnityResolve::UnityType::GameObject*>(originalResult);
                const auto rendererClass = Il2cppUtils::GetClass(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer");
                if (originalGo && rendererClass) {
                    for (const auto renderer : originalGo->GetComponentsInChildren<void*>(rendererClass, true)) {
                        if (const auto mesh = GetSkinnedMeshRendererSharedMesh(renderer))
                            replacement->attachSourceMeshes.emplace_back(mesh);
                    }
                }
                Log::InfoFmt("[ModAsset] Armed native chain subtree for live actor attach: %s original=%p originalName=%s subtree=%p sourceMeshes=%zu",
                    sourceName.c_str(), originalResult, GetUnityObjectNameString(originalResult).c_str(), modAsset,
                    replacement->attachSourceMeshes.size());
                return originalResult;
            }

            if (replacement->replaceWholeObject) {
                // A prefab authored anywhere else arrives on layer 0, and the actor camera does
                // not draw that layer — the body is built, animated and casting a shadow on the
                // floor while drawing nothing.  The layer number is a GAME constant, so it is
                // read off the asset being replaced rather than written into the package: the
                // thing we are standing in for is right here, and it is the ground truth.
                AdoptLayerFromReplacedAsset(originalResult, modAsset, sourceName);
                // Whole-object replacement skips the mesh-patching path entirely, and the
                // material adoption lived there — so the body arrived wearing
                // `GakumasSdk/BodyPlaceholder`, the SDK's texture CARRIER, whose single unlit
                // pass is what "blurry up close, no outline" actually looks like.  Same fix as
                // the other path, applied to the renderers of OUR prefab instead.
                AdoptGameMaterialsForWholeObject(originalResult, modAsset, sourceName);
                GrowMissingSocketNodes(originalResult, modAsset, sourceName);
                // Runs last so the gap it reports is what is left AFTER the adaptation.
                LogWholeObjectContractGap(originalResult, modAsset, sourceName);
                g_nativeChainValidation = true;
                Log::InfoFmt("[ModAsset] Replaced asset by whole-object validation path: %s original=%p replacement=%p",
                    sourceName.c_str(), originalResult, modAsset);
                // Prefab-side deserialization check: did il2cpp deserialize our chain +
                // dynamic bones WITH fields (rootBones populated, damping non-zero)?
                // Answers "component identity + field data OK" without waiting for
                // scene instantiation.
                if (const auto go = reinterpret_cast<UnityResolve::UnityType::GameObject*>(modAsset)) {
                    const auto chainClass = FindClassByName("ActorSwingChain");
                    const auto boneClass = FindClassByName("ActorSwingDynamicBone");
                    if (chainClass && boneClass) {
                        const auto chains = go->GetComponentsInChildren<void*>(chainClass, true);
                        const auto bones = go->GetComponentsInChildren<void*>(boneClass, true);
                        int rootBonesSize = -1;
                        if (!chains.empty()) {
                            if (const auto rb = chainClass->GetValue<UnityResolve::UnityType::List<void*>*>(chains.front(), "rootBones"))
                                rootBonesSize = rb->size;
                        }
                        float damping = -1.f;
                        if (!bones.empty()) {
                            if (const auto f = boneClass->Get<UnityResolve::Field>("damping"))
                                damping = *reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(bones.front()) + f->offset);
                        }
                        Log::InfoFmt("[ModAsset] whole-object prefab check: chains=%zu dynamicBones=%zu rootBones=%d damping=%.3f",
                            chains.size(), bones.size(), rootBonesSize, damping);
                    }
                }
                return modAsset;
            }

            if (ToLowerAscii(replacement->typeName) == "gameobject") {
                if (ApplySkinnedMeshReplacement(originalResult, modAsset, *replacement)) {
                    Log::InfoFmt("[ModAsset] Replaced asset in-place: %s original=%p replacementSource=%p",
                        sourceName.c_str(), originalResult, modAsset);
                    return originalResult;
                }

                Log::ErrorFmt("[ModAsset] In-place replacement failed, keeping original asset: %s", sourceName.c_str());
                return originalResult;
            }

            Log::InfoFmt("[ModAsset] Replaced asset by return value: %s original=%p replacement=%p replacementType=%s",
                sourceName.c_str(), originalResult, modAsset, GetUnityObjectClassName(modAsset));
            return modAsset;
        }

        void* AssetBundle_LoadAsset_Hook(void* self, Il2cppString* name, void* type) {
            auto result = AssetBundle_LoadAsset_Orig(self, name, type);
            if (name) {
                const auto assetName = name->ToString();
                LogAssetTrace("AssetBundle.LoadAsset_Internal", assetName, result, type);
                result = ReplaceLocalModAssetIfNeeded(result, assetName);
            }
            return result;
        }

        void* AssetBundle_LoadAssetAsync_Hook(void* self, Il2cppString* name, void* type) {
            auto result = AssetBundle_LoadAssetAsync_Orig(self, name, type);
            if (result && name) {
                const auto assetName = name->ToString();
                LogAssetTrace("AssetBundle.LoadAssetAsync_Internal", assetName, result, type);
                std::lock_guard lock(g_historyMutex);
                // 必须覆盖：request 指针会被复用，而 emplace 遇到已存在的 key 是**不写**的
                // —— 上一条没被消费掉的记录会让新请求顶着旧资源名走替换。
                // ponytail: 没人消费的记录只增不减（每条约百字节），真涨起来再加上限。
                g_loadHistory.insert_or_assign(result, assetName);
            }
            return result;
        }

        std::string TakeRequestName(void* request) {
            std::lock_guard lock(g_historyMutex);
            if (const auto iter = g_loadHistory.find(request); iter != g_loadHistory.end()) {
                auto name = iter->second;
                g_loadHistory.erase(iter);
                return name;
            }
            return {};
        }

        void* AssetBundleRequest_GetResult_Hook(void* self) {
            auto result = AssetBundleRequest_GetResult_Orig(self);
            const auto name = TakeRequestName(self);
            if (!name.empty()) {
                LogAssetTrace("AssetBundleRequest.GetResult", name, result);
                result = ReplaceLocalModAssetIfNeeded(result, name);
            }
            return result;
        }

        void* AssetBundleRequest_get_asset_Hook(void* self) {
            const auto name = TakeRequestName(self);
            auto result = AssetBundleRequest_get_asset_Orig(self);
            if (!name.empty()) {
                LogAssetTrace("AssetBundleRequest.get_asset", name, result);
                result = ReplaceLocalModAssetIfNeeded(result, name);
            }
            return result;
        }

        bool ResolvePersistentPropertyBlockMethods() {
            g_rendererSetPropertyBlockMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer", "SetPropertyBlock",
                { "UnityEngine.MaterialPropertyBlock" });
            g_rendererSetPropertyBlockMaterialIndexMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer", "SetPropertyBlock",
                { "UnityEngine.MaterialPropertyBlock", "System.Int32" });
            g_rendererGetPropertyBlockMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer", "GetPropertyBlock",
                { "UnityEngine.MaterialPropertyBlock" });
            g_rendererGetPropertyBlockMaterialIndexMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer", "GetPropertyBlock",
                { "UnityEngine.MaterialPropertyBlock", "System.Int32" });
            g_materialPropertyBlockSetTextureMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "MaterialPropertyBlock", "SetTexture",
                { "System.Int32", "UnityEngine.Texture" });
            g_materialPropertyBlockIsEmptyMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "MaterialPropertyBlock", "get_isEmpty");
            g_materialSetTextureMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Material", "SetTexture",
                { "System.Int32", "UnityEngine.Texture" });
            g_materialSetTextureStringMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Material", "SetTexture",
                { "System.String", "UnityEngine.Texture" });

            return g_rendererSetPropertyBlockMethod && g_rendererSetPropertyBlockMethod->function
                && g_rendererSetPropertyBlockMaterialIndexMethod
                && g_rendererSetPropertyBlockMaterialIndexMethod->function
                && g_rendererGetPropertyBlockMethod && g_rendererGetPropertyBlockMethod->function
                && g_rendererGetPropertyBlockMaterialIndexMethod
                && g_rendererGetPropertyBlockMaterialIndexMethod->function
                && g_materialPropertyBlockSetTextureMethod
                && g_materialPropertyBlockSetTextureMethod->function
                && g_materialPropertyBlockIsEmptyMethod
                && g_materialPropertyBlockIsEmptyMethod->function
                && g_materialSetTextureMethod && g_materialSetTextureMethod->function
                && g_materialSetTextureStringMethod && g_materialSetTextureStringMethod->function;
        }

        void* CreateMaterialPropertyBlock() {
            const auto klass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "MaterialPropertyBlock");
            const auto block = klass
                ? UnityResolve::Invoke<void*>("il2cpp_object_new", klass->address)
                : nullptr;
            const auto ctor = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "MaterialPropertyBlock", ".ctor");
            if (!block || !ctor || !ctor->address) {
                Log::Error("[ModAsset] Cannot create MaterialPropertyBlock scratch object.");
                return nullptr;
            }

            void* exception{};
            UnityResolve::Invoke<void*>(
                "il2cpp_runtime_invoke", ctor->address, block, nullptr, &exception);
            if (exception) {
                Log::Error("[ModAsset] MaterialPropertyBlock constructor threw.");
                return nullptr;
            }
            return block;
        }

        void* GetPropertyBlockScratch() {
            if (g_propertyBlockScratchHandle) {
                return UnityResolve::Invoke<void*>(
                    "il2cpp_gchandle_get_target", g_propertyBlockScratchHandle);
            }

            const auto block = CreateMaterialPropertyBlock();
            if (!block) return nullptr;
            g_propertyBlockScratchHandle = UnityResolve::Invoke<Il2CppGCHandle>(
                "il2cpp_gchandle_new", block, false);
            return g_propertyBlockScratchHandle ? block : nullptr;
        }

        void GetRendererPropertyBlock(void* renderer, void* block) {
            if (!renderer || !block || !g_rendererGetPropertyBlockMethod
                || !g_rendererGetPropertyBlockMethod->function) {
                return;
            }
            using Fn = void (*)(void*, void*, void*);
            reinterpret_cast<Fn>(g_rendererGetPropertyBlockMethod->function)(
                renderer, block, g_rendererGetPropertyBlockMethod->address);
        }

        void GetRendererPropertyBlock(void* renderer, void* block, const int materialIndex) {
            if (!renderer || !block || !g_rendererGetPropertyBlockMaterialIndexMethod
                || !g_rendererGetPropertyBlockMaterialIndexMethod->function) {
                return;
            }
            using Fn = void (*)(void*, void*, int, void*);
            reinterpret_cast<Fn>(g_rendererGetPropertyBlockMaterialIndexMethod->function)(
                renderer, block, materialIndex, g_rendererGetPropertyBlockMaterialIndexMethod->address);
        }

        bool IsMaterialPropertyBlockEmpty(void* block) {
            if (!block || !g_materialPropertyBlockIsEmptyMethod
                || !g_materialPropertyBlockIsEmptyMethod->function) {
                return true;
            }
            using Fn = bool (*)(void*, void*);
            return reinterpret_cast<Fn>(g_materialPropertyBlockIsEmptyMethod->function)(
                block, g_materialPropertyBlockIsEmptyMethod->address);
        }

        void CaptureRendererPropertyBlockSnapshot(
            void* renderer,
            const int materialIndex,
            const bool forceRefresh) {
            if (!renderer || materialIndex < 0) return;
            {
                std::lock_guard lock(g_propertyBlockSnapshotMutex);
                if (!forceRefresh) {
                    const auto rendererSnapshots = g_rendererPropertyBlockSnapshots.find(renderer);
                    if (rendererSnapshots != g_rendererPropertyBlockSnapshots.end()
                        && rendererSnapshots->second.contains(materialIndex)) {
                        return;
                    }
                }
            }

            const auto block = CreateMaterialPropertyBlock();
            if (!block) return;
            GetRendererPropertyBlock(renderer, block, materialIndex);
            const auto handle = UnityResolve::Invoke<Il2CppGCHandle>(
                "il2cpp_gchandle_new", block, false);
            if (!handle) return;

            RendererPropertyBlockSnapshot snapshot{
                block,
                handle,
                IsMaterialPropertyBlockEmpty(block),
            };
            std::lock_guard lock(g_propertyBlockSnapshotMutex);
            auto& destination = g_rendererPropertyBlockSnapshots[renderer][materialIndex];
            if (destination.handle) {
                UnityResolve::Invoke<void>("il2cpp_gchandle_free", destination.handle);
            }
            destination = snapshot;
        }

        void RestoreRendererPropertyBlockSnapshots(
            void* renderer,
            const size_t materialCount) {
            if (!renderer) return;

            std::lock_guard scratchLock(g_propertyBlockScratchMutex);
            std::unordered_set<int> runtimeOwnedSlots;
            if (const auto owned = g_runtimeOwnedPropertyBlockSlots.find(renderer);
                owned != g_runtimeOwnedPropertyBlockSlots.end()) {
                runtimeOwnedSlots = std::move(owned->second);
                g_runtimeOwnedPropertyBlockSlots.erase(owned);
            }

            std::unordered_map<int, RendererPropertyBlockSnapshot> snapshots;
            {
                std::lock_guard snapshotLock(g_propertyBlockSnapshotMutex);
                if (const auto rendererSnapshots = g_rendererPropertyBlockSnapshots.find(renderer);
                    rendererSnapshots != g_rendererPropertyBlockSnapshots.end()) {
                    snapshots = std::move(rendererSnapshots->second);
                    g_rendererPropertyBlockSnapshots.erase(rendererSnapshots);
                }
            }

            if (Renderer_SetPropertyBlockMaterialIndex_Orig
                && g_rendererSetPropertyBlockMaterialIndexMethod) {
                size_t restoredSnapshots = 0;
                size_t clearedRuntimeSlots = 0;
                for (size_t index = 0; index < materialCount; ++index) {
                    const auto materialIndex = static_cast<int>(index);
                    if (const auto snapshot = snapshots.find(materialIndex);
                        snapshot != snapshots.end()) {
                        Renderer_SetPropertyBlockMaterialIndex_Orig(
                            renderer,
                            snapshot->second.empty ? nullptr : snapshot->second.block,
                            materialIndex,
                            g_rendererSetPropertyBlockMaterialIndexMethod->address);
                        ++restoredSnapshots;
                    }
                    else if (runtimeOwnedSlots.contains(materialIndex)) {
                        Renderer_SetPropertyBlockMaterialIndex_Orig(
                            renderer,
                            nullptr,
                            materialIndex,
                            g_rendererSetPropertyBlockMaterialIndexMethod->address);
                        ++clearedRuntimeSlots;
                    }
                }
                if (restoredSnapshots > 0 || clearedRuntimeSlots > 0) {
                    Log::InfoFmt(
                        "[ModAsset] Hot-restored material property blocks: renderer=%s snapshots=%zu clearedRuntimeSlots=%zu",
                        GetUnityObjectNameString(renderer).c_str(),
                        restoredSnapshots,
                        clearedRuntimeSlots);
                }
            }

            for (auto& [materialIndex, snapshot] : snapshots) {
                (void)materialIndex;
                if (snapshot.handle) {
                    UnityResolve::Invoke<void>("il2cpp_gchandle_free", snapshot.handle);
                }
            }
        }

        void SetMaterialPropertyBlockTextures(void* block,
            const std::vector<PersistentMaterialTextureOverride>& textures) {
            if (!block || !g_materialPropertyBlockSetTextureMethod
                || !g_materialPropertyBlockSetTextureMethod->function) {
                return;
            }
            using Fn = void (*)(void*, int, void*, void*);
            const auto setTexture = reinterpret_cast<Fn>(
                g_materialPropertyBlockSetTextureMethod->function);
            for (const auto& texture : textures) {
                if (texture.propertyId < 0 || !texture.texture) continue;
                setTexture(
                    block,
                    texture.propertyId,
                    texture.texture,
                    g_materialPropertyBlockSetTextureMethod->address);
            }
        }

        void ApplyPersistentTextureOverridesToSlot(void* renderer,
            const RendererSlotTextureOverrides& slotOverrides,
            const bool rendererWideUpdated,
            const bool materialBlockSetByGame = false) {
            if (!renderer || slotOverrides.materialIndex < 0
                || !Renderer_SetPropertyBlockMaterialIndex_Orig) {
                return;
            }

            std::lock_guard lock(g_propertyBlockScratchMutex);
            const auto block = GetPropertyBlockScratch();
            if (!block) return;

            auto& runtimeOwnedSlots = g_runtimeOwnedPropertyBlockSlots[renderer];
            if (materialBlockSetByGame) {
                runtimeOwnedSlots.erase(slotOverrides.materialIndex);
            }

            bool copyRendererBlock = rendererWideUpdated
                && runtimeOwnedSlots.contains(slotOverrides.materialIndex);
            if (!copyRendererBlock) {
                // Preserve a game's existing per-material block. If the slot has none,
                // mirror the renderer-wide block before adding our texture values because
                // Unity gives per-material blocks precedence over renderer-wide blocks.
                GetRendererPropertyBlock(renderer, block, slotOverrides.materialIndex);
                copyRendererBlock = IsMaterialPropertyBlockEmpty(block);
                if (!copyRendererBlock) {
                    CaptureRendererPropertyBlockSnapshot(
                        renderer,
                        slotOverrides.materialIndex,
                        materialBlockSetByGame);
                }
            }
            if (copyRendererBlock) {
                GetRendererPropertyBlock(renderer, block);
                runtimeOwnedSlots.emplace(slotOverrides.materialIndex);
            }
            SetMaterialPropertyBlockTextures(block, slotOverrides.textures);
            Renderer_SetPropertyBlockMaterialIndex_Orig(
                renderer,
                block,
                slotOverrides.materialIndex,
                g_rendererSetPropertyBlockMaterialIndexMethod->address);
        }

        void ApplyPersistentTextureOverrides(void* renderer) {
            const auto rendererOverrides = CollectRendererTextureOverrides(renderer);
            for (const auto& slotOverrides : rendererOverrides) {
                ApplyPersistentTextureOverridesToSlot(
                    renderer, slotOverrides, true);
            }
            if (!rendererOverrides.empty()) {
                bool firstApplication = false;
                {
                    std::lock_guard lock(g_materialOverrideMutex);
                    firstApplication = g_loggedPersistentRenderers.emplace(renderer).second;
                }
                if (firstApplication) {
                    Log::InfoFmt("[ModAsset] Persistent material textures active: renderer=%p name=\"%s\" slots=%zu",
                        renderer,
                        GetUnityObjectNameString(renderer).c_str(),
                        rendererOverrides.size());
                }
                return;
            }
            // Diagnostic complement: a body renderer the game is submitting
            // property blocks for while carrying no registered overrides is a
            // different instance from the one hot reapply patched.  That single
            // fact separates "the commit landed on the wrong object" from "the
            // commit landed but its content is wrong".
            const auto name = GetUnityObjectNameString(renderer);
            if (name.rfind("Geo_", 0) != 0) return;
            bool firstReport = false;
            {
                std::lock_guard lock(g_materialOverrideMutex);
                firstReport = g_loggedUnmanagedRenderers.emplace(renderer).second;
            }
            if (firstReport) {
                Log::InfoFmt("[ModAsset] Property block on unmanaged renderer: renderer=%p name=\"%s\"",
                    renderer,
                    name.c_str());
            }
        }

        void Renderer_SetPropertyBlock_Hook(void* self, void* properties, void* methodInfo) {
            Renderer_SetPropertyBlock_Orig(self, properties, methodInfo);
            ApplyPersistentTextureOverrides(self);
            RetryPendingLiveReapplies(
                "Renderer.SetPropertyBlock",
                std::strcmp(GetUnityObjectClassName(self), "SkinnedMeshRenderer") == 0
                    ? self : nullptr);
        }

        void Renderer_SetPropertyBlockMaterialIndex_Hook(
            void* self, void* properties, const int materialIndex, void* methodInfo) {
            Renderer_SetPropertyBlockMaterialIndex_Orig(
                self, properties, materialIndex, methodInfo);
            const auto rendererOverrides = CollectRendererTextureOverrides(self);
            if (const auto iter = std::find_if(
                rendererOverrides.begin(),
                rendererOverrides.end(),
                [materialIndex](const auto& entry) {
                    return entry.materialIndex == materialIndex;
                });
                iter != rendererOverrides.end()) {
                ApplyPersistentTextureOverridesToSlot(
                    self, *iter, false, true);
            }
            RetryPendingLiveReapplies(
                "Renderer.SetPropertyBlock(materialIndex)",
                std::strcmp(GetUnityObjectClassName(self), "SkinnedMeshRenderer") == 0
                    ? self : nullptr);
        }

        // Last unexamined writer for "hot reapply looks wrong until a page
        // switch": the game reassigning the renderer's material array after we
        // patched it. Renderer.SetPropertyBlock and Material.SetTexture were
        // both ruled out by probes, so preserve the complete Mod material array
        // when the managed Renderer setters are used.
        void* (*Renderer_SetSharedMaterials_Orig)(void*, void*, void*) = nullptr;
        void* (*Renderer_SetMaterials_Orig)(void*, void*, void*) = nullptr;
        std::unordered_set<void*> g_loggedMaterialAssignments{};
        std::unordered_set<void*> g_loggedMaterialRestorations{};

        // Re-impose the mod material array the game just replaced.  Confirmed
        // 2026-08-02: after a hot reapply the game calls set_sharedMaterials on
        // the patched Geo_Body, which drops the private materials that carry the
        // mod textures.  That is why the home screen showed the mod mesh with the
        // original colours until a page switch re-ran the full asset path.
        thread_local bool t_restoringMaterials = false;

        bool MaterialArrayMatchesPatch(
            void* materialsObject,
            const ReversibleRendererPatch& patch) {
            if (!materialsObject || patch.originalMaterials.empty()) return false;
            const auto materials = reinterpret_cast<UnityArray<void*>*>(materialsObject);
            if (!materials || materials->max_length != patch.originalMaterials.size()) {
                return false;
            }
            for (size_t index = 0; index < patch.originalMaterials.size(); ++index) {
                if (materials->At(static_cast<unsigned int>(index))
                    != patch.originalMaterials[index]) {
                    return false;
                }
            }
            return true;
        }

        void RestorePatchedMaterials(
            void* renderer,
            void* assignedMaterials,
            const char* setter,
            void* (*restoreOriginal)(void*, void*, void*),
            void* methodInfo = nullptr) {
            if (!renderer || t_restoringMaterials || t_internalMaterialAssignment) return;

            const auto rendererOverrides = CollectRendererTextureOverrides(renderer);
            std::vector<ReversibleRendererPatch> patches;
            {
                std::lock_guard lock(g_reversiblePatchMutex);
                patches = g_reversibleRendererPatches;
            }

            // The renderer passed to the Unity setter can be a scene instance,
            // while the reversible registry was created from the cached prefab.
            // Prefer pointer identity, then match the material array the game
            // just submitted, and finally the still-installed patched mesh.
            const ReversibleRendererPatch* matched = nullptr;
            int bestScore = -1;
            void* currentMesh = nullptr;
            for (const auto& patch : patches) {
                int score = -1;
                if (patch.patchedRenderer == renderer) score = 300;
                if (MaterialArrayMatchesPatch(assignedMaterials, patch)) {
                    score = score > 200 ? score : 200;
                }
                if (patch.patchedMesh) {
                    if (!currentMesh) currentMesh = GetSkinnedMeshRendererSharedMesh(renderer);
                    if (currentMesh == patch.patchedMesh) {
                        score = score > 100 ? score : 100;
                    }
                }
                if (score > bestScore) {
                    bestScore = score;
                    matched = &patch;
                }
            }

            std::vector<void*> patched;
            if (matched) {
                patched = !matched->patchedMaterials.empty()
                    ? matched->patchedMaterials
                    : matched->patchedMaterialKeys;
            }

            if (!rendererOverrides.empty()) {
                bool firstReport = false;
                {
                    std::lock_guard lock(g_materialOverrideMutex);
                    firstReport = g_loggedMaterialAssignments.emplace(renderer).second;
                }
                if (firstReport) {
                    Log::InfoFmt(
                        "[ModAsset] Materials reassigned on renderer with overrides: renderer=%p name=\"%s\" via=%s registryMatch=%d matchScore=%d slots=%zu patches=%zu",
                        renderer,
                        GetUnityObjectNameString(renderer).c_str(),
                        setter,
                        matched ? 1 : 0,
                        bestScore,
                        patched.size(),
                        patches.size());
                }
            }
            if (patched.empty()) return;

            const auto materialClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Material");
            if (!materialClass) return;
            auto restored = UnityArray<void*>::New(materialClass, patched.size());
            if (!restored) return;
            for (size_t index = 0; index < patched.size(); ++index) {
                restored->At(static_cast<unsigned int>(index)) = patched[index];
            }

            t_restoringMaterials = true;
            // Go through the same original setter that the game called.  The
            // thread-local guard prevents this write from re-entering either
            // assignment hook.
            if (restoreOriginal) {
                const auto method = Il2cppUtils::GetMethod(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer",
                    setter, { "UnityEngine.Material[]" });
                restoreOriginal(
                    renderer,
                    restored,
                    methodInfo ? methodInfo : (method ? method->address : nullptr));
            }
            t_restoringMaterials = false;

            {
                std::lock_guard lock(g_materialOverrideMutex);
                g_rendererTextureOverrideCache.erase(renderer);
            }
            bool firstReport = false;
            {
                std::lock_guard lock(g_materialOverrideMutex);
                firstReport = g_loggedMaterialRestorations.emplace(renderer).second;
            }
            if (firstReport) {
                Log::InfoFmt(
                    "[ModAsset] Restored mod materials after the game reassigned them: renderer=%p name=\"%s\" via=%s slots=%zu matchScore=%d",
                    renderer,
                    GetUnityObjectNameString(renderer).c_str(),
                    setter,
                    patched.size(),
                    bestScore);
            }
        }

        void* Renderer_SetSharedMaterials_Hook(void* self, void* value, void* methodInfo) {
            const auto externalAssignment = !t_internalMaterialAssignment
                && !t_restoringMaterials;
            // Prime the per-renderer cache while the Mod material array is still
            // installed; after the game's setter runs, the array contains the
            // original materials and a fresh scan would lose the evidence.
            (void)CollectRendererTextureOverrides(self);
            const auto result = Renderer_SetSharedMaterials_Orig(self, value, methodInfo);
            RestorePatchedMaterials(
                self, value, "set_sharedMaterials", Renderer_SetSharedMaterials_Orig);
            if (externalAssignment) {
                RetryPendingLiveReapplies(
                    "Renderer.set_sharedMaterials",
                    std::strcmp(GetUnityObjectClassName(self), "SkinnedMeshRenderer") == 0
                        ? self : nullptr);
            }
            return result;
        }

        void* Renderer_SetMaterials_Hook(void* self, void* value, void* methodInfo) {
            const auto externalAssignment = !t_internalMaterialAssignment
                && !t_restoringMaterials;
            (void)CollectRendererTextureOverrides(self);
            const auto result = Renderer_SetMaterials_Orig(self, value, methodInfo);
            RestorePatchedMaterials(
                self, value, "set_materials", Renderer_SetMaterials_Orig);
            if (externalAssignment) {
                RetryPendingLiveReapplies(
                    "Renderer.set_materials",
                    std::strcmp(GetUnityObjectClassName(self), "SkinnedMeshRenderer") == 0
                        ? self : nullptr);
            }
            return result;
        }

        void Material_SetTexture_Hook(
            void* self, const int propertyId, void* texture, void* methodInfo) {
            bool overridden = false;
            for (const auto& overrideEntry : GetRegisteredMaterialTextureOverrides(self)) {
                if (overrideEntry.propertyId == propertyId) {
                    texture = overrideEntry.texture;
                    overridden = true;
                    break;
                }
            }
            // Diagnostic: the game writing textures onto a material we patched is
            // the remaining candidate for "hot reapply looks wrong until a page
            // switch".  Renderer.SetPropertyBlock was ruled out -- it never fires
            // for these renderers.
            if (overridden) {
                bool firstReport = false;
                {
                    std::lock_guard lock(g_materialOverrideMutex);
                    firstReport = g_loggedGameTextureWrites.emplace(self).second;
                }
                if (firstReport) {
                    Log::InfoFmt("[ModAsset] Game set texture on a patched material: material=%p propertyId=%d",
                        self,
                        propertyId);
                }
            }
            Material_SetTexture_Orig(self, propertyId, texture, methodInfo);
        }

        void Material_SetTextureString_Hook(
            void* self, Il2cppString* propertyName, void* texture, void* methodInfo) {
            const auto name = propertyName ? propertyName->ToString() : std::string{};
            for (const auto& overrideEntry : GetRegisteredMaterialTextureOverrides(self)) {
                if (overrideEntry.propertyName == name) {
                    texture = overrideEntry.texture;
                    break;
                }
            }
            Material_SetTextureString_Orig(self, propertyName, texture, methodInfo);
        }

        void* ResolveAssetBundleLoadAssetHookAddress() {
            if (const auto addr = Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.AssetBundle::LoadAsset_Internal(System.String,System.Type)")) {
                return addr;
            }
            return Il2cppUtils::GetMethodPointer("UnityEngine.AssetBundleModule.dll", "UnityEngine", "AssetBundle",
                "LoadAsset_Internal", { "System.String", "System.Type" });
        }

        void* ResolveAssetBundleLoadAssetAsyncHookAddress() {
            if (const auto addr = Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.AssetBundle::LoadAssetAsync_Internal(System.String,System.Type)")) {
                return addr;
            }
            return Il2cppUtils::GetMethodPointer("UnityEngine.AssetBundleModule.dll", "UnityEngine", "AssetBundle",
                "LoadAssetAsync_Internal", { "System.String", "System.Type" });
        }

        void* ResolveAssetBundleRequestResultHookAddress() {
            if (const auto addr = Il2cppUtils::il2cpp_resolve_icall("UnityEngine.AssetBundleRequest::GetResult()")) {
                return addr;
            }
            return Il2cppUtils::GetMethodPointer("UnityEngine.AssetBundleModule.dll", "UnityEngine", "AssetBundleRequest", "GetResult");
        }

        void* ResolveAssetBundleRequestAssetHookAddress() {
            if (const auto addr = Il2cppUtils::il2cpp_resolve_icall("UnityEngine.AssetBundleRequest::get_asset()")) {
                return addr;
            }
            return Il2cppUtils::GetMethodPointer("UnityEngine.AssetBundleModule.dll", "UnityEngine", "AssetBundleRequest", "get_asset");
        }

        std::atomic_bool g_bridgeTickObserved{};


        // The actor's own LateUpdate: the game's Animator, its animation jobs (IK, joint
        // limits, swing) and this method's own nod/look-at corrections have all written
        // the human bones by the time the original returns, and nothing has rendered yet.
        // Calling the original FIRST is the whole point — running before it would read a
        // pose that is still one correction short.
        void CampusActorController_LateUpdate_Hook(void* self, void* methodInfo) {
            if (CampusActorController_LateUpdate_Orig) {
                CampusActorController_LateUpdate_Orig(self, methodInfo);
            }
            if (!g_bridgeTickObserved.exchange(true)) {
                Log::InfoFmt("[ModAsset][EXPERIMENT] Animation bridge tick observed: self=%p", self);
            }
            DriveSourceProxyBridges(self);
        }

        void* ResolveCampusActorControllerLateUpdateHookAddress() {
            const auto controllerClass = FindClassByName("CampusActorController");
            const auto method = controllerClass
                ? FindMethodByNameAndArgCount(controllerClass, "LateUpdate", 0)
                : nullptr;
            return method ? method->function : nullptr;
        }

        // BUILDMODEL PROBE — observes only, changes nothing.
        //
        // `VLActorController.BuildModel(IEnumerable<GameObject>)` runs BEFORE the game builds
        // the skeleton: it is the point where `GetHumanDescription` / `InitializeData` turn
        // the given part prefabs into `_boneInfos`, a Humanoid Avatar and the swing drivers.
        // Handing it OUR prefab there means the game retargets onto our skeleton with Unity's
        // own Humanoid machinery — one skeleton instead of two, which is what would delete
        // the hand-written bridge, the head socket, the hip translation and the colliders /
        // prop anchors sitting on the wrong rig.
        //
        // The one thing that cannot be read off the dump is the CONCRETE collection handed
        // in (the signature is an interface), and whether its elements can be rewritten in
        // place.  So: log the type and the contents once, decide after.
        void CampusActorController_BuildModel_Hook(void* self, void* resources, void* methodInfo) {
            static std::atomic_bool observed{};
            if (!observed.exchange(true)) {
                std::string contents;
                if (const auto array = reinterpret_cast<UnityArray<void*>*>(resources)) {
                    // Only meaningful if it really is an array; the class name below is what
                    // says whether to trust it.
                    const auto count = static_cast<size_t>(array->max_length);
                    for (size_t index = 0; index < count && index < 16; ++index) {
                        contents += (contents.empty() ? "" : ", ")
                            + GetUnityObjectNameString(array->At(static_cast<unsigned int>(index)));
                    }
                }
                Log::WarnFmt("[ModAsset][EXPERIMENT] BuildModel observed: self=%p selfType=%s resources=%p resourcesType=%s asArray=[%s]",
                    self, GetUnityObjectClassName(self), resources,
                    GetUnityObjectClassName(resources), contents.c_str());
            }
            if (CampusActorController_BuildModel_Orig) {
                CampusActorController_BuildModel_Orig(self, resources, methodInfo);
            }
        }

        void* ResolveCampusActorControllerBuildModelHookAddress() {
            // The override lives on the GENERIC `VLDefaultActorController<TModelParts,
            // TDescriptor, TIDescriptor>`, which IL2CPP names `VLDefaultActorController` with
            // a backtick and arity — so exact-name lookup silently found the non-generic
            // subclass instead, which does not declare BuildModel, and the search fell all
            // the way through to the base.  Hooking a base whose override the game actually
            // calls installs a hook that can never fire: the first probe logged
            // "resolved on VLActorController" and then never observed a single call.
            //
            // Prefix scan, most-derived first, and every candidate is logged — the class list
            // is the ground truth about what the runtime really contains.
            UnityResolve::Method* chosen = nullptr;
            std::string chosenName;
            std::string candidates;
            for (const auto assembly : UnityResolve::assembly) {
                if (!assembly) continue;
                for (const auto klass : assembly->classes) {
                    if (!klass) continue;
                    if (!klass->name.starts_with("CampusActorController")
                        && !klass->name.starts_with("VLDefaultActorController")
                        && !klass->name.starts_with("VLActorController")) {
                        continue;
                    }
                    const auto method = FindMethodByNameAndArgCount(klass, "BuildModel", 1);
                    candidates += (candidates.empty() ? "" : ", ") + klass->name
                        + (method && method->function ? "(BuildModel)" : "(-)");
                    if (!method || !method->function) continue;
                    // Most derived wins: Campus > VLDefault > VLActor.
                    const auto rank = [](const std::string& name) {
                        if (name.starts_with("CampusActorController")) return 2;
                        if (name.starts_with("VLDefaultActorController")) return 1;
                        return 0;
                    };
                    if (!chosen || rank(klass->name) > rank(chosenName)) {
                        chosen = method;
                        chosenName = klass->name;
                    }
                }
            }
            Log::WarnFmt("[ModAsset][EXPERIMENT] BuildModel candidates: [%s] chosen=%s",
                candidates.c_str(), chosenName.empty() ? "none" : chosenName.c_str());
            return chosen ? chosen->function : nullptr;
        }

        void* ResolveCampusActorAnimationRigRegisterBonesHookAddress() {
            const auto rigClass = FindClassByName("CampusActorAnimationRig");
            const auto method = rigClass
                ? FindMethodByNameAndArgCount(rigClass, "RegisterBones", 1)
                : nullptr;
            return method ? method->function : nullptr;
        }

        // Two managed methods can resolve to one native function, and MinHook
        // reports the second as ALREADY_CREATED.  That surfaced as an error
        // every session for Renderer.set_materials while the earlier
        // set_sharedMaterials hook was already covering both callers -- the
        // shared hook body only mislabels which setter it logs.  Skipping is
        // correct; both addresses are now logged so the pairing is provable
        // from mod-plugin.log rather than assumed.
        // ---- VLActorGBuffer 探针（只读，不改任何渲染状态）--------------------------------
        //
        // VL 中间件里「G-buffer 阶段的半透明角色件」整套都在：_forwardTagId +
        // _transparentFilteringSettings + ExecuteTransparent，队列区间 GBufferTransparentRange
        // 还被特意从普通透明区间里让开（2501~2700 vs 2701 起）。但 iOS 3.2.3 那份二进制里
        // ExecuteTransparent 零调用者 —— PC 这一版是否也一样，靠这个探针实测：
        //   * ExecuteTransparent 会触发 → 这条路本来就活着，我们只要把队列和 tag 配对；
        //   * 从不触发           → 是死代码，需要在 ExecuteBase 之后转发调用它。
        // 顺便把四个 ShaderTagId 的真实字符串和透明筛选设置的头部打出来，省掉所有猜测。
        using VLExecuteBaseFn = void (*)(void*, void*, void*, bool);
        using VLExecuteTransparentFn = void (*)(void*, void*, void*);
        VLExecuteBaseFn VLActorGBuffer_ExecuteBase_Orig{};
        VLExecuteTransparentFn VLActorGBuffer_ExecuteTransparent_Orig{};
        std::atomic<int> g_vlTransparentCalls{ 0 };
        bool g_vlProbeDumped = false;

        std::string DescribeShaderTagId(void* instance, UnityResolve::Class* klass, const char* fieldName) {
            const auto field = klass ? klass->Get<UnityResolve::Field>(fieldName) : nullptr;
            if (!field || !instance) return std::string(fieldName) + "=<no field>";
            const auto slot = reinterpret_cast<int*>(
                reinterpret_cast<std::uintptr_t>(instance) + field->offset);
            static auto ShaderTagId_get_name = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "ShaderTagId", "get_name", {}, true);
            std::string text = std::string(fieldName) + "(+0x" + [&] {
                char buf[8]; snprintf(buf, sizeof(buf), "%X", static_cast<unsigned>(field->offset)); return std::string(buf);
            }() + ")=id:" + std::to_string(*slot);
            if (ShaderTagId_get_name) {
                if (const auto name = ShaderTagId_get_name->Invoke<Il2cppString*>(slot)) {
                    text += " \"" + name->ToString() + "\"";
                }
            }
            return text;
        }

        void DumpVLActorGBufferOnce(void* instance) {
            if (g_vlProbeDumped) return;
            g_vlProbeDumped = true;
            const auto klass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLActorGBuffer");
            if (!klass) {
                Log::Error("[VLProbe] VLActorGBuffer class not found.");
                return;
            }
            for (const char* name : { "_actorTagId", "_hairTagId", "_outlineTagId", "_forwardTagId" }) {
                Log::InfoFmt("[VLProbe] %s", DescribeShaderTagId(instance, klass, name).c_str());
            }
            // FilteringSettings 开头就是 RenderQueueRange{lower, upper} + layerMask + renderingLayerMask
            for (const char* name : { "_baseFilteringSettings", "_transparentFilteringSettings" }) {
                const auto field = klass->Get<UnityResolve::Field>(name);
                if (!field) { Log::InfoFmt("[VLProbe] %s=<no field>", name); continue; }
                const auto words = reinterpret_cast<int*>(
                    reinterpret_cast<std::uintptr_t>(instance) + field->offset);
                Log::InfoFmt("[VLProbe] %s(+0x%X) queue=[%d,%d] layerMask=0x%X renderingLayerMask=0x%X",
                    name, static_cast<unsigned>(field->offset), words[0], words[1],
                    static_cast<unsigned>(words[2]), static_cast<unsigned>(words[3]));
            }
            // 队列分档（VLRenderQueue 是静态类，直接调 getter）
            for (const char* getter : { "get_GBufferTransparentRange", "get_TransparentRange",
                                        "get_DownscaleTransparentRange" }) {
                if (const auto method = Il2cppUtils::GetMethod(
                        "Unity.RenderPipelines.Universal.Runtime.dll", "VL", "VLRenderQueue", getter, {}, true)) {
                    const auto range = method->Invoke<LocalRenderQueueRange>();
                    Log::InfoFmt("[VLProbe] VLRenderQueue.%s = [%d, %d]", getter + 4, range.lowerBound, range.upperBound);
                }
            }
        }

        void VLActorGBuffer_ExecuteBase_Hook(void* self, void* context, void* renderingData, bool useMotionVector) {
            DumpVLActorGBufferOnce(self);
            VLActorGBuffer_ExecuteBase_Orig(self, context, renderingData, useMotionVector);
        }

        void VLActorGBuffer_ExecuteTransparent_Hook(void* self, void* context, void* renderingData) {
            const auto count = ++g_vlTransparentCalls;
            if (count == 1 || count == 100 || count == 1000) {
                Log::InfoFmt("[VLProbe] VLActorGBuffer.ExecuteTransparent FIRED (第 %d 次) self=%p"
                    " —— 这条原生半透明通路本来就活着", count, self);
            }
            VLActorGBuffer_ExecuteTransparent_Orig(self, context, renderingData);
        }


        // VLDeferredPass —— IDA 给的静态链路是
        //   VLSRPRenderer → VLDeferredPass.Execute → RenderActor → VLActorGBuffer.ExecuteBase(UniversalGBufferActor)。
        // 上面 ExecuteBase 那个探针整局零命中，但它可能被 AOT 内联吞掉，光凭它不能定案。
        // Execute 是 override（走虚表，必有独立函数体），拿它当「与内联无关」的判据；
        // RenderActor 再把「角色那一支跑没跑」单独分出来。两个都只读、只转发。
        using VLDeferredExecuteFn = void (*)(void*, void*, void*);
        VLDeferredExecuteFn VLDeferredPass_Execute_Orig{};
        VLDeferredExecuteFn VLDeferredPass_RenderActor_Orig{};
        std::atomic<int> g_vlDeferredExecuteCalls{ 0 };
        std::atomic<int> g_vlDeferredRenderActorCalls{ 0 };

        void LogProbeFired(const char* what, std::atomic<int>& counter, void* self) {
            if (const auto n = ++counter; n == 1 || n == 300) {
                Log::InfoFmt("[VLProbe] %s FIRED（第 %d 次）self=%p", what, n, self);
            }
        }

        // 管线自己传的 renderingData 里就有当前相机 —— 从这里取才不会拿错。
        // 先把 RenderingData / CameraData 的字段布局打一次（偏移随版本变，别写死）。
        void* g_currentCamera = nullptr;
        bool g_renderingDataDumped = false;
        float g_pipelineView[16]{};
        float g_pipelineProj[16]{};
        bool g_pipelineMatricesValid = false;

        // 字段偏移按名字现查（别写死，随版本变）。注意 il2cpp 给值类型的偏移含 0x10 对象头，
        // 而 `ref RenderingData` 传进来的是裸结构体指针，所以要减掉。
        int FieldOffsetRaw(const char* ns, const char* klassName, const char* fieldName) {
            const auto klass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Universal.Runtime.dll", ns, klassName);
            const auto field = klass ? klass->Get<UnityResolve::Field>(fieldName) : nullptr;
            return field ? static_cast<int>(field->offset) - 0x10 : -1;
        }

        void CachePipelineMatrices(void* renderingData) {
            if (!renderingData) return;
            static const int camDataOff = FieldOffsetRaw("UnityEngine.Rendering.Universal", "RenderingData", "cameraData");
            static const int viewOff = FieldOffsetRaw("UnityEngine.Rendering.Universal", "CameraData", "m_ViewMatrix");
            static const int projOff = FieldOffsetRaw("UnityEngine.Rendering.Universal", "CameraData", "m_ProjectionMatrix");
            static const int cameraOff = FieldOffsetRaw("UnityEngine.Rendering.Universal", "CameraData", "camera");
            if (camDataOff < 0 || viewOff < 0 || projOff < 0) return;
            const auto camData = reinterpret_cast<std::uintptr_t>(renderingData) + camDataOff;
            std::memcpy(g_pipelineView, reinterpret_cast<void*>(camData + viewOff), sizeof(g_pipelineView));
            std::memcpy(g_pipelineProj, reinterpret_cast<void*>(camData + projOff), sizeof(g_pipelineProj));
            if (cameraOff >= 0) g_currentCamera = *reinterpret_cast<void**>(camData + cameraOff);
            if (!g_pipelineMatricesValid) {
                g_pipelineMatricesValid = true;
                Log::InfoFmt("[VLDoF] 管线相机=\"%s\"  view[12..14]=%.3f %.3f %.3f  proj[0]=%.3f",
                    g_currentCamera ? GetUnityObjectNameString(g_currentCamera).c_str() : "<null>",
                    g_pipelineView[12], g_pipelineView[13], g_pipelineView[14], g_pipelineProj[0]);
            }
        }

        void DumpRenderingDataLayoutOnce(void* renderingData) {
            if (g_renderingDataDumped) return;
            g_renderingDataDumped = true;
            for (const char* name : { "RenderingData", "CameraData" }) {
                const auto klass = Il2cppUtils::GetClass(
                    "Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal", name);
                if (!klass) { Log::ErrorFmt("[VLDoF] %s 类没找到", name); continue; }
                for (const auto field : klass->fields) {
                    if (!field || field->static_field) continue;
                    Log::InfoFmt("[VLDoF] %s.%s +0x%X", name, field->name.c_str(),
                        static_cast<unsigned>(field->offset));
                }
            }
            (void)renderingData;
        }

        // 实现在下面的后景深区（那里才有 FindMethodExact / 绘制清单），这里先声明
        void PatchDepthSnapshot(void* deferredPass, void** contextPtr);

        void VLDeferredPass_Execute_Hook(void* self, void* context, void* renderingData) {
            DumpRenderingDataLayoutOnce(renderingData);
            CachePipelineMatrices(renderingData);
            // 抓帧证实：插在 RenderActor 之后时命令落在 G-buffer 的 native render pass 内部，
            // SetRenderTarget 被静默忽略，深度写进了角色 MRT 而不是快照。改在 Execute 开头做 ——
            // 那时深度预pass 已经结束（快照内容就绪）、蒙皮结果也在，而且还没进 render pass。
            PatchDepthSnapshot(self, &context);
            LogProbeFired("VLDeferredPass.Execute", g_vlDeferredExecuteCalls, self);
            VLDeferredPass_Execute_Orig(self, context, renderingData);
        }

        // 中间件把「G-buffer 阶段的半透明角色件」整套都写好了 —— 独立 tag、独立筛选设置
        // (_transparentFilteringSettings 队列 [2501,5000])、独立执行函数 ExecuteTransparent，
        // 外加 VLRenderQueue 特意让出来的 GBufferTransparentRange=[2501,2700]（3.2.3 还没有这个档，
        // 是这一版新加的）。缺的只是没人调 ExecuteTransparent。
        //
        // ExecuteTransparent(context, ref renderingData) 的参数和 RenderActor 一模一样，
        // 而 RenderActor 每帧都跑、RT 还绑着角色 MRT、深度也在 —— 在它后面补一次调用即可。
        // （原计划是挂 ExecuteBase 转发，但 PC 这一版 ExecuteBase 的钩子零命中，挂不上。）
        //
        // 开关：<游戏目录>/gakumas-mod/vl-gbuffer-transparent.on 存在才转发，删掉重启即恢复。
        bool VLGBufferTransparentSwitchOn() {
            static const bool on = std::filesystem::exists(
                Paths::Root() / "vl-gbuffer-transparent.on");
            return on;
        }

        constexpr int kGmiTransparentQueueLow = 2400;   // 和深度认领那趟共用同一个 renderQueue
        void* g_vlExecuteTransparentEntry{};        // 已装钩子的入口，调它顺带打探针日志
        std::atomic<int> g_vlForwardedCalls{ 0 };

        void ForwardExecuteTransparentIfEnabled(void* deferredPass, void* context, void* renderingData) {
            if (!VLGBufferTransparentSwitchOn() || !deferredPass || !g_vlExecuteTransparentEntry) return;
            static const auto field = [] {
                const auto klass = Il2cppUtils::GetClass(
                    "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLDeferredPass");
                return klass ? klass->Get<UnityResolve::Field>("_actorGBuffer") : nullptr;
            }();
            if (!field) return;
            const auto gbuffer = *reinterpret_cast<void**>(
                reinterpret_cast<std::uintptr_t>(deferredPass) + field->offset);
            if (!gbuffer) return;
            // ExecuteTransparent 用的是 _forwardTagId（+0x1C，实机读到 id:60）。
            // ShaderTagId.get_name 在这一版被裁了，反查不到名字；而这个字段本来就没人用
            // （ExecuteTransparent 零调用），所以直接改写成我们自己的 tag —— 名字叫什么由我们定。
            static const int ourTagId = [] {
                const auto klass = Il2cppUtils::GetClass(
                    "UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "ShaderTagId");
                const auto ctor = klass && klass->address ? UnityResolve::Invoke<void*>(
                    "il2cpp_class_get_method_from_name", klass->address, ".ctor", 1) : nullptr;
                if (!ctor) return 0;
                int id = 0;
                void* args[1] = { Il2cppString::New("GmiGBufferTransparent") };
                void* exc = nullptr;
                UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", ctor, &id, args, &exc);
                return exc ? 0 : id;
            }();
            static const auto tagField = [] {
                const auto klass = Il2cppUtils::GetClass(
                    "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLActorGBuffer");
                return klass ? klass->Get<UnityResolve::Field>("_forwardTagId") : nullptr;
            }();
            if (ourTagId && tagField) {
                const auto tagSlot = reinterpret_cast<int*>(
                    reinterpret_cast<std::uintptr_t>(gbuffer) + tagField->offset);
                if (*tagSlot != ourTagId) {
                    Log::InfoFmt("[VLPass] _forwardTagId(+0x%X) %d → %d (GmiGBufferTransparent)",
                        static_cast<unsigned>(tagField->offset), *tagSlot, ourTagId);
                    *tagSlot = ourTagId;
                }
            }
            // 队列打架：深度认领那趟要 ≤2500（_baseFilteringSettings=[0,2500]），
            // 这趟要 ≥2501（_transparentFilteringSettings=[2501,5000]），而一个材质只有一个
            // renderQueue。把 transparent 的下界拉到 2400，材质挂 2400 就能被两趟同时收走。
            // 这个字段除了 ExecuteTransparent 没人用（本来零调用），改它不影响原版。
            static const auto filterField = [] {
                const auto klass = Il2cppUtils::GetClass(
                    "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLActorGBuffer");
                return klass ? klass->Get<UnityResolve::Field>("_transparentFilteringSettings") : nullptr;
            }();
            if (filterField) {
                // FilteringSettings 开头就是 RenderQueueRange{lowerBound, upperBound}
                const auto range = reinterpret_cast<int*>(
                    reinterpret_cast<std::uintptr_t>(gbuffer) + filterField->offset);
                if (range[0] != kGmiTransparentQueueLow) {
                    Log::InfoFmt("[VLPass] _transparentFilteringSettings 队列 [%d,%d] → [%d,%d]",
                        range[0], range[1], kGmiTransparentQueueLow, range[1]);
                    range[0] = kGmiTransparentQueueLow;
                }
            }
            if (const auto n = ++g_vlForwardedCalls; n == 1) {
                Log::InfoFmt("[VLPass] 开始向 VLActorGBuffer.ExecuteTransparent 转发 gbuffer=%p", gbuffer);
            }
            reinterpret_cast<VLExecuteTransparentFn>(g_vlExecuteTransparentEntry)(
                gbuffer, context, renderingData);
        }

        void VLDeferredPass_RenderActor_Hook(void* self, void* context, void* renderingData) {
            LogProbeFired("VLDeferredPass.RenderActor", g_vlDeferredRenderActorCalls, self);
            // ExecuteBase 被 AOT 内联的话，那边的 dump 永远不触发；从 RenderActor 手里
            // 直接取 _actorGBuffer 实例来 dump，一样能拿到 tag id 和 _baseFilteringSettings
            // 的队列区间 —— 后者直接决定 renderQueue=2400 会不会被这一趟收走。
            if (self && !g_vlProbeDumped) {
                const auto klass = Il2cppUtils::GetClass(
                    "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLDeferredPass");
                const auto field = klass ? klass->Get<UnityResolve::Field>("_actorGBuffer") : nullptr;
                if (field) {
                    if (const auto gbuffer = *reinterpret_cast<void**>(
                            reinterpret_cast<std::uintptr_t>(self) + field->offset)) {
                        Log::InfoFmt("[VLProbe] VLDeferredPass._actorGBuffer=%p —— 下面这组是它的配置", gbuffer);
                        DumpVLActorGBufferOnce(gbuffer);
                    }
                }
            }
            VLDeferredPass_RenderActor_Orig(self, context, renderingData);
            ForwardExecuteTransparentIfEnabled(self, context, renderingData);
        }

        // ---- DoF 之后画半透明件：先只读探针 -------------------------------------------
        //
        // 方案：不再跟延迟管线抢 RT0.z / RT1（那条路的代价见 research 文档 5.14/5.15），
        // 改成在**景深之后、bloom 之前**把部件画上去。那个时刻底下的场景已经被景深处理过，
        // 透过薄纱看到的地板该虚还是虚（物理正确），而纱本身不会被糊。
        // 顺带真实深度缓冲还在，能正常 ZTest —— 一直没解决的「飘带浮到人物正面」也一并解决。
        //
        // 关键：DoF 和 Bloom 在**同一个 pass 的同一次 Execute 里**，RenderPassEvent 插不进去：
        //   VLPostProcessPass.Render(cmd, ref renderingData)
        //     ├─ DoVLDOF(cmd, source, destination, ref cameraData)
        //     ├─ SetupVLDiffusion(...)
        //     ├─ SetupVLBloom(cmd, source, ...)
        //     └─ SetupVLParaffin / SetupVLVirtualEffect / RenderFinalPass
        // 所以只能钩进去。DoVLDOF 的签名正好把 cmd 和 destination（景深之后的颜色目标）都给了我们，
        // 渲染目标绑定权在自己手上 —— 之前那个 dsv=NULL 的老问题在这里不存在。
        //
        // 这一版**只打日志**，不改任何渲染。要确认三件事：
        //   ① DoVLDOF 每帧命中吗（它返回 bool，景深关掉的场景可能根本不调）；
        //   ② SetupVLBloom 是不是更可靠的兜底钩子点；
        //   ③ VLPostProcessPass 上哪个字段是深度句柄（下一步 SetRenderTarget 要用）。
        using VLDoDofFn = bool (*)(void*, void*, void*, void*, void*);
        using VLSetupBloomFn = void (*)(void*, void*, void*, void*, void*);
        VLDoDofFn VLPostProcessPass_DoVLDOF_Orig{};
        VLSetupBloomFn VLPostProcessPass_SetupVLBloom_Orig{};
        std::atomic<int> g_vlDofCalls{ 0 };
        std::atomic<int> g_vlBloomCalls{ 0 };
        bool g_vlPostFieldsDumped = false;

        void DumpVLPostProcessFieldsOnce() {
            if (g_vlPostFieldsDumped) return;
            g_vlPostFieldsDumped = true;
            const auto klass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering.Internal", "VLPostProcessPass");
            if (!klass) { Log::Error("[VLDoF] VLPostProcessPass class not found"); return; }
            // 只打和「深度 / 源 / 目标」有关的字段，全打会刷屏（这个类字段极多）
            Log::InfoFmt("[VLDoF] VLPostProcessPass parent=%s", klass->parent.c_str());
            for (const auto field : klass->fields) {
                if (!field) continue;
                const auto& n = field->name;
                if (n.find("epth") == std::string::npos && n.find("ource") == std::string::npos
                    && n.find("estination") == std::string::npos && n.find("arget") == std::string::npos
                    && n.find("amera") == std::string::npos) continue;
                Log::InfoFmt("[VLDoF] VLPostProcessPass.%s +0x%X%s", n.c_str(),
                    static_cast<unsigned>(field->offset), field->static_field ? " (static)" : "");
            }
        }

        bool VLPostProcessPass_DoVLDOF_Hook(void* self, void* cmd, void* source, void* destination, void* cameraData) {
            const auto ret = VLPostProcessPass_DoVLDOF_Orig(self, cmd, source, destination, cameraData);
            if (const auto n = ++g_vlDofCalls; n == 1 || n == 300) {
                DumpVLPostProcessFieldsOnce();
                Log::InfoFmt("[VLDoF] DoVLDOF FIRED（第 %d 次）self=%p cmd=%p source=%p destination=%p 返回=%s",
                    n, self, cmd, source, destination, ret ? "true" : "false");
            }
            return ret;
        }

        // 按参数类型精确挑重载 —— UnityResolve 的 Get<Method> 匹配失败会兜底返回第一个同名方法
        // （参数个数都不查），CoreUtils.SetRenderTarget 有七八个重载，靠不住。
        UnityResolve::Method* FindMethodExact(const char* assembly, const char* ns, const char* klassName,
            const char* methodName, const std::vector<std::string>& argTypes) {
            const auto klass = Il2cppUtils::GetClass(assembly, ns, klassName);
            if (!klass) return nullptr;
            for (const auto method : klass->methods) {
                if (!method || method->name != methodName || method->args.size() != argTypes.size()) continue;
                bool match = true;
                for (size_t i = 0; i < argTypes.size(); ++i) {
                    const auto type = method->args[i] ? method->args[i]->pType : nullptr;
                    if (!type || type->name.find(argTypes[i]) == std::string::npos) { match = false; break; }
                }
                if (match) return method;
            }
            return nullptr;
        }

        // ---- 在景深之后、bloom 之前把部件画上去 -----------------------------------------
        // 开关：<游戏目录>/gakumas-mod/vl-afterdof.on 存在才画，删掉重启即恢复。
        bool VLAfterDofSwitchOn() {
            static const bool on = std::filesystem::exists(Paths::Root() / "vl-afterdof.on");
            return on;
        }

        // shader 的 pass 顺序：0 ZPrePass / 1 ActorTransparent / 2 Forward / 3 GBufferTransparent / 4 DepthClaim。
        // 默认画 2（GmiTransparentForward）—— 0 是 ColorMask 0 的空 pass，踩过一次。
        // 开关文件里写个数字就能换，不用重编。
        int VLAfterDofPass() {
            static const int pass = [] {
                std::ifstream file(Paths::Root() / "vl-afterdof.on");
                int value = 2;
                if (file >> value && value >= 0 && value <= 8) return value;
                return 2;
            }();
            return pass;
        }

        struct GmiAfterDofDraw { Il2CppGCHandle renderer; Il2CppGCHandle material; int submesh; };
        std::vector<GmiAfterDofDraw> g_afterDofDraws;

        void RegisterAfterDofDraw(void* renderer, void* material, int submesh) {
            if (!renderer || !material) return;
            // 后景深那趟画出来是绑定姿势/位置偏 —— 已排除 VP（管线矩阵和 Camera.main 一致）。
            // 剩下的解释是顶点不是当前帧蒙皮结果。先看这个 renderer 到底是什么类型：
            // SkinnedMeshRenderer 说明蒙皮归 Unity 管，问题在绘制时机；
            // MeshRenderer 说明是 VL 自研蒙皮（VLActorSkinningSystem），这条路根本走不通。
            {
                static std::set<std::string> seen;
                const auto klass = Il2cppUtils::get_class_from_instance(renderer);
                const auto name = klass ? UnityResolve::Invoke<const char*>("il2cpp_class_get_name", klass) : nullptr;
                if (name && seen.insert(name).second) {
                    Log::InfoFmt("[VLDoF] renderer 类型 = %s（\"%s\"）", name,
                        GetUnityObjectNameString(renderer).c_str());
                }
            }
            // 材质会被重新应用（换装/重建），同一个 renderer+submesh 只保留最新一条，
            // 否则每次重建都多画一遍（实测从 2 个涨到 4 个）
            std::erase_if(g_afterDofDraws, [&](const GmiAfterDofDraw& d) {
                return d.submesh == submesh
                    && UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", d.renderer) == renderer;
            });
            g_afterDofDraws.push_back({
                UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", renderer, false),
                UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", material, false),
                submesh });
        }

        using CoreUtilsSetRTFn = void (*)(void*, void*, int, int, int, int);
        using CmdDrawRendererFn = void (*)(void*, void*, void*, int, int);

        void DrawAfterDof(void* cmd, void* source) {
            if (!VLAfterDofSwitchOn() || !cmd || !source || g_afterDofDraws.empty()) return;

            // CoreUtils.SetRenderTarget(CommandBuffer, RTHandle, ClearFlag, int miplevel, CubemapFace, int depthSlice)
            static const auto setRT = [] {
                const auto m = FindMethodExact("Unity.RenderPipelines.Core.Runtime.dll", "UnityEngine.Rendering",
                    "CoreUtils", "SetRenderTarget",
                    { "CommandBuffer", "RTHandle", "ClearFlag", "Int32", "CubemapFace", "Int32" });
                if (!m) Log::Error("[VLDoF] CoreUtils.SetRenderTarget(cmd,RTHandle,ClearFlag,…) 没找到，后景深绘制不启用");
                return m ? reinterpret_cast<CoreUtilsSetRTFn>(m->function) : nullptr;
            }();
            // CommandBuffer.DrawRenderer(Renderer, Material, int submeshIndex, int shaderPass) —— 四参唯一
            static const auto drawRenderer = [] {
                const auto m = FindMethodExact("UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                    "CommandBuffer", "DrawRenderer", { "Renderer", "Material", "Int32", "Int32" });
                if (!m) Log::Error("[VLDoF] CommandBuffer.DrawRenderer(4参) 没找到，后景深绘制不启用");
                return m ? reinterpret_cast<CmdDrawRendererFn>(m->function) : nullptr;
            }();
            // 后处理阶段 cmd 上挂的是全屏 blit 的矩阵，直接画蒙皮网格会：①世界坐标被映射到固定
            // 屏幕位置（看着像钉在场景里、不跟角色）②矩阵带 Y 翻转 → 绕序反转 → 正反面互换
            // → VFACE 判反 → 正面暗反面亮。所以画之前必须把相机的 view/proj 设上，画完还原。
            static const auto setVP = [] {
                const auto m = FindMethodExact("UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                    "CommandBuffer", "SetViewProjectionMatrices", { "Matrix4x4", "Matrix4x4" });
                if (!m) Log::Error("[VLDoF] CommandBuffer.SetViewProjectionMatrices 没找到");
                return m ? reinterpret_cast<void (*)(void*, void*, void*)>(m->function) : nullptr;
            }();
            static const auto getView = [] {
                const auto m = FindMethodExact("UnityEngine.CoreModule.dll", "UnityEngine",
                    "Camera", "get_worldToCameraMatrix", {});
                return m ? reinterpret_cast<void (*)(void*, void*)>(m->function) : nullptr;
            }();
            static const auto getProj = [] {
                const auto m = FindMethodExact("UnityEngine.CoreModule.dll", "UnityEngine",
                    "Camera", "get_projectionMatrix", {});
                return m ? reinterpret_cast<void (*)(void*, void*)>(m->function) : nullptr;
            }();
            // Camera.main 在换装间是 "Game3DManager"，未必是正在渲染的那台 —— 用别的相机的 VP
            // 画出来就是「位置偏到一侧 + 转镜头飘带不动」。优先用 Camera.current（SRP 渲染
            // 某台相机时会设它），拿不到再退回 main。
            static const auto getCurrentCamera = [] {
                const auto m = FindMethodExact("UnityEngine.CoreModule.dll", "UnityEngine",
                    "Camera", "get_current", {});
                return m ? reinterpret_cast<void* (*)()>(m->function) : nullptr;
            }();
            static const auto getMainCamera = [] {
                const auto m = FindMethodExact("UnityEngine.CoreModule.dll", "UnityEngine",
                    "Camera", "get_main", {});
                return m ? reinterpret_cast<void* (*)()>(m->function) : nullptr;
            }();
            if (!setRT || !drawRenderer || !setVP || !getView || !getProj || !getMainCamera) return;

            // 优先用管线自己那份（VLDeferredPass.Execute 里从 renderingData.cameraData 抄下来的），
            // Camera.main 只是兜底 —— 换装间里 main 是 "Game3DManager"，未必是正在渲染的那台。
            float view[16]{}, proj[16]{}, identity[16]{ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            void* camera = g_currentCamera;
            if (g_pipelineMatricesValid) {
                std::memcpy(view, g_pipelineView, sizeof(view));
                std::memcpy(proj, g_pipelineProj, sizeof(proj));
            }
            else {
                const auto current = getCurrentCamera ? getCurrentCamera() : nullptr;
                camera = current ? current : getMainCamera();
                if (!camera) {
                    static bool warned = false;
                    if (!warned) { warned = true; Log::Error("[VLDoF] 拿不到任何相机矩阵，不画"); }
                    return;
                }
                getView(view, camera);
                getProj(proj, camera);
            }

            // 矩阵读对了没有，一次看清：view 的最后一列应该是相机位置量级的数，
            // 全零 = IL2CPP 大结构体返回的 ABI 猜错了（(retBuf,this) vs (this,retBuf)）
            static bool matrixDumped = false;
            if (!matrixDumped) {
                matrixDumped = true;
                Log::InfoFmt("[VLDoF] view  = [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
                    view[0],view[1],view[2],view[3], view[4],view[5],view[6],view[7],
                    view[8],view[9],view[10],view[11], view[12],view[13],view[14],view[15]);
                Log::InfoFmt("[VLDoF] proj  = [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
                    proj[0],proj[1],proj[2],proj[3], proj[4],proj[5],proj[6],proj[7],
                    proj[8],proj[9],proj[10],proj[11], proj[12],proj[13],proj[14],proj[15]);
                Log::InfoFmt("[VLDoF] 矩阵来源=%s 相机=\"%s\"（Camera.main=%s）",
                    g_pipelineMatricesValid ? "管线 renderingData" : "Camera 兜底",
                    camera ? GetUnityObjectNameString(camera).c_str() : "<null>",
                    getMainCamera && getMainCamera() ? GetUnityObjectNameString(getMainCamera()).c_str() : "<null>");
            }

            setRT(cmd, source, 0 /*ClearFlag.None*/, 0 /*miplevel*/, -1 /*CubemapFace.Unknown*/, -1 /*depthSlice*/);
            setVP(cmd, view, proj);

            static std::atomic<int> drawn{ 0 };
            int ok = 0;
            for (const auto& d : g_afterDofDraws) {
                const auto renderer = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", d.renderer);
                const auto material = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", d.material);
                if (!renderer || !material) continue;
                drawRenderer(cmd, renderer, material, d.submesh, VLAfterDofPass());
                ++ok;
            }
            setVP(cmd, identity, identity);      // 还原，别把相机矩阵留给后面的全屏 blit
            if (const auto n = ++drawn; n == 1 || n == 300) {
                Log::InfoFmt("[VLDoF] 景深之后画了 %d 个 submesh（第 %d 次）source=%p shaderPass=%d camera=%p",
                    ok, n, source, VLAfterDofPass(), camera);
            }
        }

        // ---- 往深度快照里补一笔飘带的深度 -----------------------------------------------
        //
        // 5.14 量到：景深读的 RT0.z 由一趟全屏 resolve 从 _cameraDepthTexture（深度预pass 结束时
        // 拍的快照）重算。那是**一张独立纹理**，不是活的 DSV —— 所以只往它里面补飘带的深度，
        // resolve 自己就会算出正确的 RT0.z，而且：
        //   * 不碰活 DSV → 身体不会被飘带遮住（这是「写进深度预pass」那条路的代价）
        //   * 不用 stencil → resolve 照常回填背景色，不会白（那是 stencil 路线的代价）
        //   * 在几何阶段画 → 蒙皮和矩阵都是现成正确的（那是后景深重画那条路的代价）
        //   * 颜色完全不动 → 飘带照旧走前向透明，混合本来就对
        // 副作用：其他读这张深度图的效果（雾）会认为那块更近一点，方向对、量级小。
        //
        // 开关：<游戏目录>/gakumas-mod/vl-depthpatch.on，内容写 pass 索引（默认 0 = ZPrePass）。
        bool VLDepthPatchSwitchOn() {
            static const bool on = std::filesystem::exists(Paths::Root() / "vl-depthpatch.on");
            return on;
        }
        int VLDepthPatchPass() {
            static const int pass = [] {
                std::ifstream file(Paths::Root() / "vl-depthpatch.on");
                int value = 0;
                if (file >> value && value >= 0 && value <= 8) return value;
                return 0;
            }();
            return pass;
        }

        // 自建 CommandBuffer，用完立刻经 context.ExecuteCommandBuffer 提交 —— 这样命令一定落在
        // RenderActor 之后、resolve 之前。用 renderingData.commandBuffer 的话，那条 buffer 可能
        // 早就被这趟 pass 执行掉了，我们追加的东西会跑到 resolve 后面去，白测一次。
        void* AcquireScratchCommandBuffer() {
            static void* cached = [] () -> void* {
                const auto klass = Il2cppUtils::GetClass(
                    "UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "CommandBuffer");
                if (!klass || !klass->address) { Log::Error("[VLDepth] CommandBuffer 类没找到"); return nullptr; }
                const auto obj = UnityResolve::Invoke<void*>("il2cpp_object_new", klass->address);
                const auto ctor = UnityResolve::Invoke<void*>(
                    "il2cpp_class_get_method_from_name", klass->address, ".ctor", 0);
                if (!obj || !ctor) { Log::Error("[VLDepth] CommandBuffer 造不出来"); return nullptr; }
                void* exc = nullptr;
                UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", ctor, obj, nullptr, &exc);
                if (exc) { Log::Error("[VLDepth] CommandBuffer..ctor 抛异常"); return nullptr; }
                UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", obj, false);   // 常驻，别被回收
                return obj;
            }();
            if (!cached) return nullptr;
            static const auto clear = [] {
                const auto m = FindMethodExact("UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                    "CommandBuffer", "Clear", {});
                return m ? reinterpret_cast<void (*)(void*)>(m->function) : nullptr;
            }();
            if (clear) clear(cached);
            return cached;
        }

        void PatchDepthSnapshot(void* deferredPass, void** contextPtr) {
            if (!VLDepthPatchSwitchOn() || !deferredPass || !contextPtr || g_afterDofDraws.empty()) return;

            static const auto depthField = [] {
                const auto klass = Il2cppUtils::GetClass(
                    "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLDeferredPass");
                const auto f = klass ? klass->Get<UnityResolve::Field>("_cameraDepthTexture") : nullptr;
                if (!f) Log::Error("[VLDepth] VLDeferredPass._cameraDepthTexture 字段没找到，深度补写不启用");
                return f;
            }();
            if (!depthField) return;

            const auto depthRT = *reinterpret_cast<void**>(
                reinterpret_cast<std::uintptr_t>(deferredPass) + depthField->offset);
            const auto cmd = AcquireScratchCommandBuffer();
            if (!depthRT || !cmd) return;

            // ScriptableRenderContext 就是一个 IntPtr，按值传进寄存器；结构体实例方法的 this
            // 要的是「指向这个结构体的指针」，所以直接把参数的地址交出去。
            static const auto execute = [] {
                const auto m = FindMethodExact("UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                    "ScriptableRenderContext", "ExecuteCommandBuffer", { "CommandBuffer" });
                if (!m) Log::Error("[VLDepth] ScriptableRenderContext.ExecuteCommandBuffer 没找到，深度补写不启用");
                return m ? reinterpret_cast<void (*)(void**, void*)>(m->function) : nullptr;
            }();
            if (!execute) return;

            static const auto setRT = [] {
                const auto m = FindMethodExact("Unity.RenderPipelines.Core.Runtime.dll", "UnityEngine.Rendering",
                    "CoreUtils", "SetRenderTarget",
                    { "CommandBuffer", "RTHandle", "ClearFlag", "Int32", "CubemapFace", "Int32" });
                return m ? reinterpret_cast<CoreUtilsSetRTFn>(m->function) : nullptr;
            }();
            static const auto drawRenderer = [] {
                const auto m = FindMethodExact("UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                    "CommandBuffer", "DrawRenderer", { "Renderer", "Material", "Int32", "Int32" });
                return m ? reinterpret_cast<CmdDrawRendererFn>(m->function) : nullptr;
            }();
            if (!setRT || !drawRenderer) return;

            setRT(cmd, depthRT, 0 /*ClearFlag.None*/, 0, -1, -1);
            int ok = 0;
            for (const auto& d : g_afterDofDraws) {
                const auto renderer = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", d.renderer);
                const auto material = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", d.material);
                if (!renderer || !material) continue;
                drawRenderer(cmd, renderer, material, d.submesh, VLDepthPatchPass());
                ++ok;
            }
            execute(contextPtr, cmd);
            static std::atomic<int> patched{ 0 };
            if (const auto n = ++patched; n == 1 || n == 300) {
                Log::InfoFmt("[VLDepth] 往深度快照补写 %d 个 submesh（第 %d 次）depthRT=%p pass=%d",
                    ok, n, depthRT, VLDepthPatchPass());
            }
        }

        void VLPostProcessPass_SetupVLBloom_Hook(void* self, void* cmd, void* source, void* bloom, void* starStreak) {
            if (const auto n = ++g_vlBloomCalls; n == 1 || n == 300) {
                Log::InfoFmt("[VLDoF] SetupVLBloom FIRED（第 %d 次）self=%p cmd=%p source=%p", n, self, cmd, source);
            }
            DrawAfterDof(cmd, source);          // 先把部件画进 source，再让 bloom 从它取样
            VLPostProcessPass_SetupVLBloom_Orig(self, cmd, source, bloom, starStreak);
        }


        // 「当前渲染器是谁、它排了哪些 pass」各打一次。
        // 判 VLSRPRenderer / VLDeferredPass 在不在场，比逐个方法下钩子省一次重启；
        // 而且入队在调用链上游，内联影响不到它。
        std::set<std::string> g_vlSeenEnqueues;

        void DumpEnqueueOnce(void* renderer, const char* passName) {
            if (!renderer || !passName || g_vlSeenEnqueues.size() > 64) return;  // ponytail: 64 行只是防刷屏
            const auto klass = Il2cppUtils::get_class_from_instance(renderer);
            const auto rname = klass ? UnityResolve::Invoke<const char*>("il2cpp_class_get_name", klass) : nullptr;
            auto line = std::string(rname ? rname : "<unknown>") + " ← " + passName;
            if (!g_vlSeenEnqueues.insert(line).second) return;
            Log::InfoFmt("[VLProbe] 入队 %s", line.c_str());
        }


        // VLActorTransparentPass —— 中间件给「角色半透明件」准备的完整通路：
        // Execute 里连着三趟 DrawRenderers（_prePassTagId → _shaderTagIds(3个) → _outlineTagId），
        // 筛选队列 [0,2500]，RenderPassEvent=400（天空盒之后、普通透明之前）。
        // 那个时机场景色缓冲已经完整，混合天然正确；前置那趟又能解决景深取错深度。
        // 这个探针只读：确认它在学马跑不跑，并把五个 tag 的真实字符串打出来。
        using VLTransparentExecuteFn = void (*)(void*, void*, void*);
        VLTransparentExecuteFn VLActorTransparentPass_Execute_Orig{};
        std::atomic<int> g_vlActorTransparentCalls{ 0 };
        bool g_vlActorTransparentDumped = false;


        // tag id 是运行时按注册顺序分配的，反查名字最省事的办法：拿已知候选名各构造一个
        // ShaderTagId，看谁的 id 撞上 VLActorTransparentPass 里那几个（85~89）。
        void DumpShaderTagIdTable() {
            static bool done = false;
            if (done) return;
            done = true;
            const auto klass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "ShaderTagId");
            if (!klass || !klass->address) { Log::Error("[VLProbe] ShaderTagId class not found"); return; }
            const auto ctor = UnityResolve::Invoke<void*>(
                "il2cpp_class_get_method_from_name", klass->address, ".ctor", 1);
            if (!ctor) { Log::Error("[VLProbe] ShaderTagId..ctor(string) not found"); return; }
            static const char* const kNames[] = {
                "UniversalForward", "UniversalForwardOnly", "UniversalForwardOutline",
                "UniversalForwardPerformance", "UniversalGBuffer", "UniversalGBufferActor",
                "UniversalGBufferActorHair", "UniversalGBufferOutline", "UniversalGBufferPreDepth",
                "UniversalGBufferVirtualEffect", "UniversalGBufferVirtualHair",
                "UniversalGBufferVirtualOutline", "SRPDefaultUnlit", "DepthOnly", "DepthNormals",
                "MotionVectors",
                // VLActorTransparentPass 独占的那一族（metadata 字面量表里连着放的）
                "VLActorTransparent", "VLActorTransparentZPrePass",
                "VLActorCoverTransparent", "VLActorCoverZPrePass",
                "VLActorCoverZPrePassTransparent", "VLActorCoverAlphaFillPass",
                "VLActorTransparentOutline", "VLActorOutline", "VLActorCoverOutline",
                // _forwardTagId=id:60 还没认领，下面这几个是候选
                "VLActorForward", "UniversalGBufferActorForward", "UniversalForwardActor",
                "VLActorGBufferForward", "UniversalGBufferForward", "GBufferTransparent",
                "UniversalGBufferActorTransparent", "VLActorGBufferTransparent",
                // 从 PC 的 global-metadata 里捞出来的、字面量表里真实存在又还没试过的
                "VLActor", "VLActorCover", "Universal2D", "UniversalMaterialType",
            };
            for (const auto name : kNames) {
                int id = 0;
                void* args[1] = { Il2cppString::New(name) };
                void* exc = nullptr;
                UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", ctor, &id, args, &exc);
                if (exc) continue;
                Log::InfoFmt("[VLProbe] ShaderTagId \"%s\" = id:%d%s", name, id,
                    (id >= 85 && id <= 89) ? "   <<< 命中 VLActorTransparentPass" : "");
            }
        }

        void DumpVLActorTransparentOnce(void* instance) {
            if (g_vlActorTransparentDumped) return;
            g_vlActorTransparentDumped = true;
            DumpShaderTagIdTable();
            const auto klass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLActorTransparentPass");
            if (!klass) { Log::Error("[VLProbe] VLActorTransparentPass class not found."); return; }
            for (const char* name : { "_prePassTagId", "_outlineTagId" }) {
                Log::InfoFmt("[VLProbe] TransparentPass %s", DescribeShaderTagId(instance, klass, name).c_str());
            }
            // _shaderTagIds 是 List<ShaderTagId>：读 _items 数组 + _size
            if (const auto field = klass->Get<UnityResolve::Field>("_shaderTagIds")) {
                const auto list = *reinterpret_cast<void**>(
                    reinterpret_cast<std::uintptr_t>(instance) + field->offset);
                if (list) {
                    const auto items = *reinterpret_cast<void**>(
                        reinterpret_cast<std::uintptr_t>(list) + 0x10);
                    const auto size = *reinterpret_cast<int*>(
                        reinterpret_cast<std::uintptr_t>(list) + 0x18);
                    static auto ShaderTagId_get_name = Il2cppUtils::GetMethod(
                        "UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "ShaderTagId", "get_name", {}, true);
                    for (int i = 0; i < size && i < 8 && items; ++i) {
                        const auto slot = reinterpret_cast<int*>(
                            reinterpret_cast<std::uintptr_t>(items) + 0x20 + 4 * i);
                        std::string text = "id:" + std::to_string(*slot);
                        if (ShaderTagId_get_name) {
                            if (const auto name = ShaderTagId_get_name->Invoke<Il2cppString*>(slot)) {
                                text += " \"" + name->ToString() + "\"";
                            }
                        }
                        Log::InfoFmt("[VLProbe] TransparentPass _shaderTagIds[%d] = %s", i, text.c_str());
                    }
                }
            }
            if (const auto field = klass->Get<UnityResolve::Field>("_filteringSettings")) {
                const auto words = reinterpret_cast<int*>(
                    reinterpret_cast<std::uintptr_t>(instance) + field->offset);
                Log::InfoFmt("[VLProbe] TransparentPass _filteringSettings(+0x%X) queue=[%d,%d] layerMask=0x%X renderingLayerMask=0x%X",
                    static_cast<unsigned>(field->offset), words[0], words[1],
                    static_cast<unsigned>(words[2]), static_cast<unsigned>(words[3]));
            }
        }

        void VLActorTransparentPass_Execute_Hook(void* self, void* context, void* renderingData) {
            const auto count = ++g_vlActorTransparentCalls;
            if (count == 1) {
                Log::Info("[VLProbe] VLActorTransparentPass.Execute FIRED —— 原生角色半透明通路是活的");
                DumpVLActorTransparentOnce(self);
            }
            VLActorTransparentPass_Execute_Orig(self, context, renderingData);
        }


        // ---- 实验：把中间件的 VLActorTransparentPass 塞进当前渲染器 ----------------------
        //
        // 学马没有启用 VL 那套 actor RendererFeature（VLActorForward + VLActorTransparentPass
        // 成套创建，实测三个入口零调用），用的是 Campus 自己的渲染路径。这里不启用整套
        // feature（会和 Campus 那套重复画角色），只把「角色半透明」这一个 pass 自己 new 出来，
        // 在渲染器每次收 pass 时补塞一次。
        //
        // 它自带的规矩：筛选队列 [0,2500]、RenderPassEvent=400（天空盒之后、普通透明之前）、
        // Execute 里三趟 DrawRenderers（前置 → 颜色 → 描边）。
        //
        // **危险实验**：改的是渲染流程，出错是硬崩。所以用文件开关控制 ——
        //   <游戏目录>/gakumas-mod/vl-transparent-pass.on 存在才启用，删掉即恢复。
        void* g_vlLastRenderer = nullptr;
        Il2CppGCHandle g_vlTransparentPassHandle{};
        bool g_vlTransparentPassTried = false;
        bool g_vlTransparentPassEnabled = false;
        void* g_campusActorPassClass = nullptr;

        bool VLTransparentPassSwitchOn() {
            static const bool on = std::filesystem::is_regular_file(
                Paths::Root() / "vl-transparent-pass.on");
            return on;
        }

        void* EnsureVLActorTransparentPass() {
            if (g_vlTransparentPassHandle) {
                if (const auto cached = UnityResolve::Invoke<void*>(
                        "il2cpp_gchandle_get_target", g_vlTransparentPassHandle)) {
                    return cached;
                }
            }
            if (g_vlTransparentPassTried) return nullptr;
            g_vlTransparentPassTried = true;

            const auto klass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLActorTransparentPass");
            if (!klass || !klass->address) {
                Log::Error("[VLPass] VLActorTransparentPass class not found; 实验不启用");
                return nullptr;
            }
            const auto instance = UnityResolve::Invoke<void*>("il2cpp_object_new", klass->address);
            const auto ctor = UnityResolve::Invoke<void*>(
                "il2cpp_class_get_method_from_name", klass->address, ".ctor", 1);
            if (!instance || !ctor) {
                Log::Error("[VLPass] VLActorTransparentPass ctor not resolvable; 实验不启用");
                return nullptr;
            }
            // LayerMask 是 {int} 的结构体，按值传 = 直接传 int。-1 = Everything。
            int layerMask = -1;
            void* args[1] = { &layerMask };
            void* exc = nullptr;
            UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", ctor, instance, args, &exc);
            if (exc) {
                Log::Error("[VLPass] VLActorTransparentPass ctor threw; 实验不启用");
                return nullptr;
            }
            g_vlTransparentPassHandle = UnityResolve::Invoke<Il2CppGCHandle>(
                "il2cpp_gchandle_new", instance, false);
            // 构造函数把 renderPassEvent 设成 400（AfterRenderingSkybox）。抓帧实测：那个时机
            // 渲染器已经切到只带颜色的目标（dsv=NULL），ConfigureTarget 配什么都不生效；
            // 而不透明那一段（同帧 dsv=3bb6dc74）深度是绑着的。所以把时机提前到
            // AfterRenderingOpaques(300)：场景色已有房间与角色，深度也还在。
            static auto SetEvent = Il2cppUtils::GetMethod(
                "Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal",
                "ScriptableRenderPass", "set_renderPassEvent",
                { "UnityEngine.Rendering.Universal.RenderPassEvent" }, true);
            // 开关文件里可以直接写一个数字来指定 RenderPassEvent，免得每试一个时机就重编：
            //   250=BeforeRenderingOpaques 300=AfterRenderingOpaques 350=BeforeRenderingSkybox
            //   400=AfterRenderingSkybox(构造函数的默认值) 450=BeforeRenderingTransparents
            int passEvent = 300;
            {
                std::ifstream file(Paths::Root() / "vl-transparent-pass.on");
                int parsed = 0;
                if (file >> parsed && parsed >= 0 && parsed <= 1000) passEvent = parsed;
            }
            if (SetEvent) SetEvent->Invoke<void>(instance, passEvent);
            Log::InfoFmt("[VLPass] VLActorTransparentPass 实例已创建 instance=%p layerMask=-1 renderPassEvent=%s",
                instance, SetEvent ? std::to_string(passEvent).c_str() : "400(改不了)");
            DumpVLActorTransparentOnce(instance);
            return instance;
        }

        using EnqueuePassFn = void (*)(void*, void*);
        EnqueuePassFn ScriptableRenderer_EnqueuePass_Orig{};
        std::atomic<int> g_vlPassEnqueued{ 0 };

        void ScriptableRenderer_EnqueuePass_Hook(void* renderer, void* pass) {
            ScriptableRenderer_EnqueuePass_Orig(renderer, pass);
            if (!VLTransparentPassSwitchOn() || !renderer || !pass) return;

            // 只在 Campus 自己的角色 pass 入队之后补塞一次，避免每次调用都塞、也保证时机在角色渲染那一组里
            const auto klass = Il2cppUtils::get_class_from_instance(pass);
            const auto name = klass ? UnityResolve::Invoke<const char*>("il2cpp_class_get_name", klass) : nullptr;
            DumpEnqueueOnce(renderer, name);
            if (!name || std::string_view(name) != "CampusActorRenderPass") return;

            g_vlLastRenderer = renderer;
            const auto ours = EnsureVLActorTransparentPass();
            if (!ours) return;
            ScriptableRenderer_EnqueuePass_Orig(renderer, ours);
            if (const auto count = ++g_vlPassEnqueued; count == 1 || count == 300) {
                Log::InfoFmt("[VLPass] VLActorTransparentPass 已入队（第 %d 次）renderer=%p", count, renderer);
            }
        }


        // 相机的颜色/深度句柄在 EnqueuePass 时还是 null（实测报「取不到」），
        // 要等渲染器 Execute 时才配好。所以 ConfigureTarget 放在这里做。
        using RendererExecuteFn = void (*)(void*, void*, void*);
        RendererExecuteFn ScriptableRenderer_Execute_Orig{};

        using OnCameraSetupFn = void (*)(void*, void*, void*);
        OnCameraSetupFn ScriptableRenderPass_OnCameraSetup_Orig{};

        void ScriptableRenderPass_OnCameraSetup_Hook(void* self, void* cmd, void* renderingData) {
            ScriptableRenderPass_OnCameraSetup_Orig(self, cmd, renderingData);
            if (!VLTransparentPassSwitchOn() || !g_vlTransparentPassHandle || !g_vlLastRenderer) return;
            const auto ours = UnityResolve::Invoke<void*>(
                "il2cpp_gchandle_get_target", g_vlTransparentPassHandle);
            if (!ours || ours != self) return;   // 只管我们自己那个 pass
            static auto ConfigureTarget = Il2cppUtils::GetMethod(
                "Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal",
                "ScriptableRenderPass", "ConfigureTarget",
                { "UnityEngine.Rendering.RTHandle", "UnityEngine.Rendering.RTHandle" }, true);
            static auto GetColor = Il2cppUtils::GetMethod(
                "Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal",
                "ScriptableRenderer", "get_cameraColorTargetHandle", {}, true);
            static auto GetDepth = Il2cppUtils::GetMethod(
                "Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal",
                "ScriptableRenderer", "get_cameraDepthTargetHandle", {}, true);
            if (!ConfigureTarget || !GetColor || !GetDepth) return;
            const auto color = GetColor->Invoke<void*>(g_vlLastRenderer);
            const auto depth = GetDepth->Invoke<void*>(g_vlLastRenderer);
            static bool logged = false;
            if (color && depth) {
                // 走 native render pass 的话附件在更早阶段就算好了，晚一步 ConfigureTarget 会被无视
                // （实测：句柄非空、日志说配好了，但抓帧里 dsv 仍是 NULL）。关掉它退回经典的
                // SetRenderTarget 路径，我们配的颜色+深度才会真正绑上。
                static auto SetUseNativeRenderPass = Il2cppUtils::GetMethod(
                    "Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal",
                    "ScriptableRenderPass", "set_useNativeRenderPass", { "System.Boolean" }, true);
                if (SetUseNativeRenderPass) SetUseNativeRenderPass->Invoke<void>(ours, false);
                ConfigureTarget->Invoke<void>(ours, color, depth);
                if (!logged) {
                    logged = true;
                    Log::InfoFmt("[VLPass] OnCameraSetup 里配好目标 color=%p depth=%p useNativeRenderPass=%s",
                        color, depth, SetUseNativeRenderPass ? "已关" : "接口缺失");
                }
            }
            else if (!logged) {
                logged = true;
                Log::ErrorFmt("[VLPass] OnCameraSetup 仍取不到句柄 color=%p depth=%p", color, depth);
            }
        }

        void ScriptableRenderer_Execute_Hook_UNUSED(void* renderer, void* context, void* renderingData) {
            if (VLTransparentPassSwitchOn() && g_vlTransparentPassHandle) {
                if (const auto ours = UnityResolve::Invoke<void*>(
                        "il2cpp_gchandle_get_target", g_vlTransparentPassHandle)) {
                    static auto ConfigureTarget = Il2cppUtils::GetMethod(
                        "Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal",
                        "ScriptableRenderPass", "ConfigureTarget",
                        { "UnityEngine.Rendering.RTHandle", "UnityEngine.Rendering.RTHandle" }, true);
                    static auto GetColor = Il2cppUtils::GetMethod(
                        "Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal",
                        "ScriptableRenderer", "get_cameraColorTargetHandle", {}, true);
                    static auto GetDepth = Il2cppUtils::GetMethod(
                        "Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal",
                        "ScriptableRenderer", "get_cameraDepthTargetHandle", {}, true);
                    if (ConfigureTarget && GetColor && GetDepth) {
                        const auto color = GetColor->Invoke<void*>(renderer);
                        const auto depth = GetDepth->Invoke<void*>(renderer);
                        static bool logged = false;
                        if (color && depth) {
                            ConfigureTarget->Invoke<void>(ours, color, depth);
                            if (!logged) {
                                logged = true;
                                Log::InfoFmt("[VLPass] ConfigureTarget 已设置 color=%p depth=%p", color, depth);
                            }
                        }
                        else if (!logged) {
                            logged = true;
                            Log::ErrorFmt("[VLPass] Execute 时刻仍取不到句柄 color=%p depth=%p", color, depth);
                        }
                    }
                }
            }
            ScriptableRenderer_Execute_Orig(renderer, context, renderingData);
        }

        template <typename Fn>

        bool InstallHook(const char* name, void* target, void* hook, Fn* original) {
            if (!target) {
                Log::ErrorFmt("[ModAsset] Hook target is null: %s", name);
                return false;
            }
            if (std::find(g_hookTargets.begin(), g_hookTargets.end(), target)
                != g_hookTargets.end()) {
                Log::InfoFmt(
                    "[ModAsset] Hook target already covered, skipped: %s target=%p"
                    " (shares its native function with an earlier hook)",
                    name,
                    target);
                return true;
            }
            if (const auto status = MH_CreateHook(target, hook, reinterpret_cast<void**>(original)); status != MH_OK) {
                Log::ErrorFmt("[ModAsset] MH_CreateHook failed: %s target=%p status=%s",
                    name, target, MH_StatusToString(status));
                return false;
            }
            if (const auto status = MH_EnableHook(target); status != MH_OK) {
                Log::ErrorFmt("[ModAsset] MH_EnableHook failed: %s status=%s", name, MH_StatusToString(status));
                return false;
            }
            g_hookTargets.push_back(target);
            Log::InfoFmt("[ModAsset] Hook installed: %s target=%p", name, target);
            return true;
        }

        // 半透明路线（research/transparent-material-2026-08-18.md）的只读探针。
        // 那条路线暂停期间默认**不装**：装了它们就是 target-rig 实机里一个没人声明的变量，
        // 而"每次进游戏只改一个变量"是这条路线的贯穿规矩。
        // 该路线自己的任一开关文件在，就照常装；只想要探针就放一个空的 vl-probes.on。
        bool VLProbesSwitchOn() {
            static const bool on = VLTransparentPassSwitchOn()
                || VLGBufferTransparentSwitchOn()
                || VLAfterDofSwitchOn()
                || VLDepthPatchSwitchOn()
                || std::filesystem::exists(Paths::Root() / "vl-probes.on");
            return on;
        }

        void InstallVLProbeHooks() {
            if (!VLProbesSwitchOn()) return;
            // 只读探针，失败不影响任何既有功能
            if (const auto method = Il2cppUtils::GetMethod(
                    "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLActorGBuffer",
                    "ExecuteBase", {}, true)) {
                InstallHook("VLActorGBuffer.ExecuteBase(probe)", method->function,
                    reinterpret_cast<void*>(VLActorGBuffer_ExecuteBase_Hook),
                    &VLActorGBuffer_ExecuteBase_Orig);
            }
            else {
                Log::Error("[VLProbe] VLActorGBuffer.ExecuteBase not found —— 这一版的类名或签名变了");
            }
            if (const auto method = Il2cppUtils::GetMethod(
                    "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering.Internal", "VLPostProcessPass",
                    "DoVLDOF", {}, true)) {
                InstallHook("VLPostProcessPass.DoVLDOF(probe)", method->function,
                    reinterpret_cast<void*>(VLPostProcessPass_DoVLDOF_Hook),
                    &VLPostProcessPass_DoVLDOF_Orig);
            }
            else {
                Log::Error("[VLDoF] VLPostProcessPass.DoVLDOF not found —— 类名/命名空间可能变了");
            }
            if (const auto method = Il2cppUtils::GetMethod(
                    "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering.Internal", "VLPostProcessPass",
                    "SetupVLBloom", {}, true)) {
                InstallHook("VLPostProcessPass.SetupVLBloom(probe)", method->function,
                    reinterpret_cast<void*>(VLPostProcessPass_SetupVLBloom_Hook),
                    &VLPostProcessPass_SetupVLBloom_Orig);
            }
            else {
                Log::Error("[VLDoF] VLPostProcessPass.SetupVLBloom not found");
            }
            if (const auto method = Il2cppUtils::GetMethod(
                    "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLDeferredPass",
                    "Execute", {}, true)) {
                InstallHook("VLDeferredPass.Execute(probe)", method->function,
                    reinterpret_cast<void*>(VLDeferredPass_Execute_Hook),
                    &VLDeferredPass_Execute_Orig);
            }
            else {
                Log::Error("[VLProbe] VLDeferredPass.Execute not found");
            }
            if (const auto method = Il2cppUtils::GetMethod(
                    "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLDeferredPass",
                    "RenderActor", {}, true)) {
                InstallHook("VLDeferredPass.RenderActor(probe)", method->function,
                    reinterpret_cast<void*>(VLDeferredPass_RenderActor_Hook),
                    &VLDeferredPass_RenderActor_Orig);
            }
            else {
                Log::Error("[VLProbe] VLDeferredPass.RenderActor not found");
            }
            if (VLTransparentPassSwitchOn()) {
                if (const auto method = Il2cppUtils::GetMethod(
                        "Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal",
                        "ScriptableRenderer", "EnqueuePass", { "UnityEngine.Rendering.Universal.ScriptableRenderPass" }, true)) {
                    InstallHook("ScriptableRenderer.EnqueuePass(VL实验)", method->function,
                        reinterpret_cast<void*>(ScriptableRenderer_EnqueuePass_Hook),
                        &ScriptableRenderer_EnqueuePass_Orig);
                    Log::Info("[VLPass] 开关文件存在，VLActorTransparentPass 实验已启用");
                }
                if (const auto setup = Il2cppUtils::GetMethod(
                        "Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal",
                        "ScriptableRenderPass", "OnCameraSetup",
                        { "UnityEngine.Rendering.CommandBuffer", "UnityEngine.Rendering.Universal.RenderingData&" }, true)) {
                    InstallHook("ScriptableRenderPass.OnCameraSetup(VL实验)", setup->function,
                        reinterpret_cast<void*>(ScriptableRenderPass_OnCameraSetup_Hook),
                        &ScriptableRenderPass_OnCameraSetup_Orig);
                }
                else {
                    Log::Error("[VLPass] ScriptableRenderer.EnqueuePass 找不到，实验不启用");
                }
            }
            if (const auto method = Il2cppUtils::GetMethod(
                    "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLActorTransparentPass",
                    "Execute", {}, true)) {
                InstallHook("VLActorTransparentPass.Execute(probe)", method->function,
                    reinterpret_cast<void*>(VLActorTransparentPass_Execute_Hook),
                    &VLActorTransparentPass_Execute_Orig);
            }
            else {
                Log::Error("[VLProbe] VLActorTransparentPass.Execute not found");
            }
            if (const auto method = Il2cppUtils::GetMethod(
                    "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLActorGBuffer",
                    "ExecuteTransparent", {}, true)) {
                InstallHook("VLActorGBuffer.ExecuteTransparent(probe)", method->function,
                    reinterpret_cast<void*>(VLActorGBuffer_ExecuteTransparent_Hook),
                    &VLActorGBuffer_ExecuteTransparent_Orig);
                g_vlExecuteTransparentEntry = method->function;
            }
            else {
                Log::Error("[VLProbe] VLActorGBuffer.ExecuteTransparent not found");
            }
        }

        bool InstallHooks() {
            bool ok = true;
            InstallVLProbeHooks();
            ok &= InstallHook("AssetBundle.LoadAsset_Internal",
                ResolveAssetBundleLoadAssetHookAddress(),
                reinterpret_cast<void*>(AssetBundle_LoadAsset_Hook),
                &AssetBundle_LoadAsset_Orig);
            ok &= InstallHook("AssetBundle.LoadAssetAsync_Internal",
                ResolveAssetBundleLoadAssetAsyncHookAddress(),
                reinterpret_cast<void*>(AssetBundle_LoadAssetAsync_Hook),
                &AssetBundle_LoadAssetAsync_Orig);
            ok &= InstallHook("AssetBundleRequest.GetResult",
                ResolveAssetBundleRequestResultHookAddress(),
                reinterpret_cast<void*>(AssetBundleRequest_GetResult_Hook),
                &AssetBundleRequest_GetResult_Orig);
            ok &= InstallHook("AssetBundleRequest.get_asset",
                ResolveAssetBundleRequestAssetHookAddress(),
                reinterpret_cast<void*>(AssetBundleRequest_get_asset_Hook),
                &AssetBundleRequest_get_asset_Orig);
            if (const auto target = ResolveCampusActorAnimationRigRegisterBonesHookAddress()) {
                ok &= InstallHook("CampusActorAnimationRig.RegisterBones",
                    target,
                    reinterpret_cast<void*>(CampusActorAnimationRig_RegisterBones_Hook),
                    &CampusActorAnimationRig_RegisterBones_Orig);
            }
            if (const auto target = ResolveCampusActorControllerBuildModelHookAddress()) {
                ok &= InstallHook("CampusActorController.BuildModel",
                    target,
                    reinterpret_cast<void*>(CampusActorController_BuildModel_Hook),
                    &CampusActorController_BuildModel_Orig);
            }
            else {
                Log::Warn("[ModAsset][EXPERIMENT] BuildModel unavailable; the one-skeleton route cannot be probed here.");
            }
            if (const auto target = ResolveCampusActorControllerLateUpdateHookAddress()) {
                ok &= InstallHook("CampusActorController.LateUpdate",
                    target,
                    reinterpret_cast<void*>(CampusActorController_LateUpdate_Hook),
                    &CampusActorController_LateUpdate_Orig);
            }
            else {
                Log::Warn("[ModAsset] CampusActorController.LateUpdate unavailable; the source-proxy animation bridge cannot tick.");
            }
            if (!ResolveCampusActorAnimationRigRegisterBonesHookAddress()) {
                Log::Warn("[ModAsset] CampusActorAnimationRig.RegisterBones unavailable; ActorSwing data graft disabled.");
            }
            if (ResolvePersistentPropertyBlockMethods()) {
                ok &= InstallHook("Renderer.SetPropertyBlock(renderer)",
                    g_rendererSetPropertyBlockMethod->function,
                    reinterpret_cast<void*>(Renderer_SetPropertyBlock_Hook),
                    &Renderer_SetPropertyBlock_Orig);
                ok &= InstallHook("Renderer.SetPropertyBlock(materialIndex)",
                    g_rendererSetPropertyBlockMaterialIndexMethod->function,
                    reinterpret_cast<void*>(Renderer_SetPropertyBlockMaterialIndex_Hook),
                    &Renderer_SetPropertyBlockMaterialIndex_Orig);
                ok &= InstallHook("Material.SetTexture(propertyId)",
                    g_materialSetTextureMethod->function,
                    reinterpret_cast<void*>(Material_SetTexture_Hook),
                    &Material_SetTexture_Orig);
                ok &= InstallHook("Material.SetTexture(propertyName)",
                    g_materialSetTextureStringMethod->function,
                    reinterpret_cast<void*>(Material_SetTextureString_Hook),
                    &Material_SetTextureString_Orig);
            }
            else {
                // 以前这里没有 else：解析失败就静默跳过上面四个 hook 且 ok 仍是 true，
                // 表现是贴图覆盖整个失效而日志全绿。
                Log::Error("[ModAsset] Persistent material texture override methods unavailable.");
                ok = false;
            }

            // Reapply the complete Mod material array after the game writes its
            // original array back to a renderer during a hot toggle.
            if (const auto setShared = Il2cppUtils::GetMethod(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer",
                    "set_sharedMaterials", { "UnityEngine.Material[]" })) {
                InstallHook("Renderer.set_sharedMaterials",
                    setShared->function,
                    reinterpret_cast<void*>(Renderer_SetSharedMaterials_Hook),
                    &Renderer_SetSharedMaterials_Orig);
            }
            if (const auto setMaterials = Il2cppUtils::GetMethod(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer",
                    "set_materials", { "UnityEngine.Material[]" })) {
                InstallHook("Renderer.set_materials",
                    setMaterials->function,
                    reinterpret_cast<void*>(Renderer_SetMaterials_Hook),
                    &Renderer_SetMaterials_Orig);
            }
            else {
                Log::Error("[ModAsset] Renderer.set_materials unavailable; the game can take the mod material array back on a hot toggle.");
                ok = false;
            }
            return ok;
        }
    }

    // Diagnostic only.  Five rounds of fixes guessed at what replaces the Mod
    // colours between "hot ON" and "back on the home screen"; none of them ever
    // established what is actually attached to the renderer drawing the body at
    // that moment.  This reads it back: which live renderer carries the patched
    // mesh, whether its material array is still ours, and whether the textures
    // we wrote are still bound.  Main thread only -- called from the mod menu.
    void AuditLivePatches(const char* reason) {
        if (!Log::IsEnabled(Log::Level::Info)) return;
        const auto label = reason && *reason ? reason : "audit";

        std::vector<ReversibleRendererPatch> patches;
        {
            std::lock_guard lock(g_reversiblePatchMutex);
            patches = g_reversibleRendererPatches;
        }
        if (patches.empty()) {
            Log::InfoFmt("[ModAudit] %s: no registered renderer patches.", label);
            return;
        }

        const auto rendererClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer");
        if (!rendererClass) return;
        const auto liveRenderers = rendererClass->FindObjectsByType<void*>();

        for (const auto& patch : patches) {
            size_t withPatchedMesh = 0;
            size_t withOriginalMesh = 0;
            for (const auto renderer : liveRenderers) {
                if (!renderer || !IsNativeObjectAlive(renderer)) continue;
                const auto mesh = GetSkinnedMeshRendererSharedMesh(renderer);
                const bool patchedMesh = patch.patchedMesh && mesh == patch.patchedMesh;
                const bool originalMesh = patch.originalMesh && mesh == patch.originalMesh;
                if (patchedMesh) ++withPatchedMesh;
                if (originalMesh) ++withOriginalMesh;
                if (!patchedMesh && !originalMesh && renderer != patch.patchedRenderer) continue;

                std::string slots;
                const auto materials = reinterpret_cast<UnityArray<void*>*>(
                    GetRendererSharedMaterials(renderer));
                for (std::uintptr_t i = 0; materials && i < materials->max_length; ++i) {
                    const auto material = materials->At(static_cast<unsigned int>(i));
                    const bool isModSlot = i < patch.patchedMaterials.size()
                        && material == patch.patchedMaterials[i];
                    const bool isOriginalSlot = i < patch.originalMaterials.size()
                        && material == patch.originalMaterials[i];
                    bool isPrivate = false;
                    {
                        std::lock_guard lock(g_materialOverrideMutex);
                        isPrivate = g_privateMaterials.contains(material);
                    }
                    size_t overrides = 0;
                    size_t boundTextures = 0;
                    for (const auto& entry : GetRegisteredMaterialTextureOverrides(material)) {
                        ++overrides;
                        if (GetMaterialTexture(material, entry.propertyId) == entry.texture) {
                            ++boundTextures;
                        }
                    }
                    slots += Log::Format(
                        " [%u]=%p mod=%d original=%d private=%d textures=%zu/%zu%s",
                        static_cast<unsigned>(i), material,
                        isModSlot ? 1 : 0, isOriginalSlot ? 1 : 0, isPrivate ? 1 : 0,
                        boundTextures, overrides,
                        DescribeMaterialState(material).c_str());
                }

                Log::InfoFmt(
                    "[ModAudit] %s: mod=%s source=%s renderer=%p registered=%p same=%d name=\"%s\" mesh=%p patchedMesh=%d originalMesh=%d slots=%s",
                    label,
                    patch.modId.c_str(),
                    patch.sourceName.c_str(),
                    renderer,
                    patch.patchedRenderer,
                    renderer == patch.patchedRenderer ? 1 : 0,
                    GetUnityObjectNameString(renderer).c_str(),
                    mesh,
                    patchedMesh ? 1 : 0,
                    originalMesh ? 1 : 0,
                    slots.c_str());
            }

            // Complementary half: a patch whose mesh is nowhere in the live set
            // is the interesting case, and only a line that prints on zero hits
            // can prove it.
            Log::InfoFmt(
                "[ModAudit] %s: mod=%s source=%s renderer=%s liveRenderers=%zu withPatchedMesh=%zu withOriginalMesh=%zu",
                label,
                patch.modId.c_str(),
                patch.sourceName.c_str(),
                patch.rendererName.c_str(),
                liveRenderers.size(),
                withPatchedMesh,
                withOriginalMesh);
        }
    }

    GmrResult SetSessionModEnabled(const char* modIdUtf8, const uint8_t enabled) {
        if (!modIdUtf8 || !*modIdUtf8) return GMR_E_INVALID_ARGUMENT;

        const bool requestedEnabled = enabled != 0;
        std::vector<LocalModAssetReplacementPtr> matchingReplacements;
        bool stateChanged = false;
        std::size_t activeForMod = 0;
        std::size_t activeTotal = 0;
        std::unique_lock replacementLock(g_replacementMutex);
        bool found = false;
        for (const auto& replacement : g_registeredReplacements) {
            if (!replacement || replacement->modId != modIdUtf8) continue;
            stateChanged |= replacement->sessionEnabled != requestedEnabled;
            replacement->sessionEnabled = requestedEnabled;
            matchingReplacements.push_back(replacement);
            found = true;
        }
        if (!found) return GMR_E_MOD_NOT_FOUND;

        RebuildActiveReplacementMapLocked(true);
        for (const auto& [key, replacement] : g_replacementMap) {
            (void)key;
            if (replacement && replacement->modId == modIdUtf8) ++activeForMod;
        }
        activeTotal = g_replacementMap.size();
        replacementLock.unlock();

        size_t affectedInstances = 0;
        if (stateChanged && requestedEnabled) {
            std::unordered_set<std::string> reappliedSources;
            for (const auto& replacement : matchingReplacements) {
                if (!replacement) continue;
                const auto sourceKey = NormalizeAssetName(replacement->sourceName);
                if (!reappliedSources.emplace(sourceKey).second) continue;
                const auto applied = ReapplyLiveModInstances(*replacement);
                affectedInstances += applied;
                if (applied == 0
                    && !replacement->replaceWholeObject
                    && !replacement->attachToOriginal) {
                    QueuePendingLiveReapply(
                        replacement->modId, replacement->sourceName);
                }
                else if (applied > 0) {
                    ClearPendingLiveReapply(
                        replacement->modId, replacement->sourceName);
                }
            }
        }
        else if (stateChanged) {
            ClearPendingLiveReappliesForMod(modIdUtf8);
            affectedInstances = RestoreLiveModInstances(modIdUtf8);
        }

        Log::InfoFmt(
            "[ModAsset] Session toggle applied: modId=%s enabled=%d changed=%d activeRules=%zu activeTotal=%zu hotInstances=%zu.",
            modIdUtf8,
            requestedEnabled ? 1 : 0,
            stateChanged ? 1 : 0,
            activeForMod,
            activeTotal,
            affectedInstances);
        return GMR_OK;
    }

    bool Initialize() {
        if (g_initialized.exchange(true)) return true;

        Log::Info("[ModAsset] Standalone mod plugin initializing.");
        HMODULE gameAssembly{};
        for (int i = 0; i < 600 && !gameAssembly; ++i) {
            gameAssembly = GetModuleHandleA("GameAssembly.dll");
            if (!gameAssembly) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!gameAssembly) {
            Log::Error("[ModAsset] GameAssembly.dll not loaded; mod plugin disabled.");
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        UnityResolve::Init(gameAssembly, UnityResolve::Mode::Il2Cpp, false);
        if (!AttachIl2cppThread(gameAssembly)) {
            g_initialized = false;
            return false;
        }

        if (const auto status = MH_Initialize(); status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
            Log::ErrorFmt("[ModAsset] MH_Initialize failed: %s", MH_StatusToString(status));
            g_initialized = false;
            return false;
        }

        const auto hooksOk = InstallHooks();
        LoadLocalModManifests();
        Catalog::Refresh();
        Catalog::SetReady(true);
        std::size_t activeReplacementCount = 0;
        std::size_t candidateReplacementCount = 0;
        {
            std::shared_lock replacementLock(g_replacementMutex);
            activeReplacementCount = g_replacementMap.size();
            candidateReplacementCount = g_registeredReplacements.size();
        }
        Log::InfoFmt("[ModAsset] Standalone mod plugin initialized. hooksOk=%d candidates=%zu active=%zu",
            hooksOk ? 1 : 0,
            candidateReplacementCount,
            activeReplacementCount);
        return hooksOk;
    }

    void Shutdown() {
        if (!g_initialized.exchange(false)) return;
        Catalog::Clear();
        for (const auto target : g_hookTargets) {
            MH_DisableHook(target);
        }
        g_hookTargets.clear();
        {
            std::unique_lock replacementLock(g_replacementMutex);
            g_replacementMap.clear();
            g_registeredReplacements.clear();
        }
        if (g_propertyBlockScratchHandle) {
            UnityResolve::Invoke<void>(
                "il2cpp_gchandle_free", std::exchange(g_propertyBlockScratchHandle, nullptr));
        }
        {
            std::lock_guard lock(g_propertyBlockScratchMutex);
            g_runtimeOwnedPropertyBlockSlots.clear();
        }
        {
            std::lock_guard lock(g_propertyBlockSnapshotMutex);
            for (auto& [renderer, snapshots] : g_rendererPropertyBlockSnapshots) {
                (void)renderer;
                for (auto& [materialIndex, snapshot] : snapshots) {
                    (void)materialIndex;
                    if (snapshot.handle) {
                        UnityResolve::Invoke<void>("il2cpp_gchandle_free", snapshot.handle);
                    }
                }
            }
            g_rendererPropertyBlockSnapshots.clear();
        }
        for (const auto handle : g_runtimeMaterialHandles) {
            if (handle) UnityResolve::Invoke<void>("il2cpp_gchandle_free", handle);
        }
        g_runtimeMaterialHandles.clear();
        std::vector<Il2CppGCHandle> runtimeBoneHandles;
        {
            // 建骨那条路的全部状态：GC 句柄要放（否则每次卸载/重载插件都钉住一批
            // GameObject），三张表要清（下一轮 Initialize 会重新登记）。
            std::lock_guard lock(g_swingStateMutex);
            runtimeBoneHandles.swap(g_runtimeBoneHandles);
            g_createdActorSwingBoneNames.clear();
            g_modChainAroundByHost.clear();
            g_createdBonesByOwner.clear();
            g_hybridBonesByRenderer.clear();
            g_nativeChainAttachedRoots.clear();
        }
        for (const auto handle : runtimeBoneHandles) {
            if (handle) UnityResolve::Invoke<void>("il2cpp_gchandle_free", handle);
        }
        {
            // 资源装载那条路的句柄同样是钉住托管对象的，卸载不放就是泄漏。
            std::lock_guard lock(g_bundleMutex);
            for (const auto& [path, handle] : g_bundleHandleMap) {
                (void)path;
                if (handle) UnityResolve::Invoke<void>("il2cpp_gchandle_free", handle);
            }
            g_bundleHandleMap.clear();
            for (const auto& [key, handle] : g_loadedAssetHandleMap) {
                (void)key;
                if (handle) UnityResolve::Invoke<void>("il2cpp_gchandle_free", handle);
            }
            g_loadedAssetHandleMap.clear();
        }
        {
            std::lock_guard lock(g_runtimeMeshHandleMutex);
            for (const auto& [mesh, handle] : g_runtimeMeshHandles) {
                (void)mesh;
                if (handle) UnityResolve::Invoke<void>("il2cpp_gchandle_free", handle);
            }
            g_runtimeMeshHandles.clear();
            g_transformedMeshSet.clear();
        }
        {
            std::lock_guard lock(g_historyMutex);
            g_loadHistory.clear();
        }
        {
            // 两个原子标志也要回位：inFlight 停在 true 的话，重新 Initialize 之后
            // 这一轮的重试会被永久挡在门外。
            std::lock_guard lock(g_pendingReapplyMutex);
            g_pendingReapplies.clear();
        }
        g_hasPendingReapplies.store(false);
        g_pendingReapplyInFlight.store(false);
        {
            std::lock_guard lock(g_materialOverrideMutex);
            g_materialTextureOverrides.clear();
            g_rendererTextureOverrideCache.clear();
            g_privateMaterials.clear();
            g_loggedPersistentRenderers.clear();
            g_loggedMaterialAssignments.clear();
            g_loggedMaterialRestorations.clear();
        }
        {
            std::lock_guard lock(g_reversiblePatchMutex);
            g_reversibleRendererPatches.clear();
            g_reapplyRendererIdentities.clear();
        }
        {
            std::lock_guard lock(g_animationRigMutex);
            g_activeAnimationRigs.clear();
        }
        {
            std::lock_guard lock(g_pendingReapplyMutex);
            g_pendingReapplies.clear();
            g_hasPendingReapplies.store(false);
            g_pendingReapplyInFlight.store(false);
        }
        Log::Info("[ModAsset] Standalone mod plugin shutdown.");
    }
}
