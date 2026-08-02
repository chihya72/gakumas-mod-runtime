#include "ModRuntime.hpp"

#include "ModIl2cppUtils.hpp"
#include "ModLog.hpp"
#include "ModPaths.hpp"
#include "ModRuntimeCatalog.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
        };

        struct RendererSlotTextureOverrides {
            int materialIndex{};
            std::vector<PersistentMaterialTextureOverride> textures{};
        };

        struct ReversibleRendererPatch {
            std::string modId;
            std::string sourceName;
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
            // 碰撞：摇物骨的 dynamicCollider 与身体骨的 staticCollider 配对。身体骨
            // (Head/Neck/Spine*/Hips/Pelvis/Left|RightArm/ForeArm/Leg) 的 staticCollider 是
            // 游戏自带的，我们只需给自己的骨一个半径；不给的话 SetDefaultValues 留下的是
            // 空碰撞体 → 手臂直接穿过裙子。radius<0 表示 sidecar 没提供、不要动。
            float colliderRadius{ -1.0f };
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
        };

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
        // same names. See AddActorSwingBonesToAnimationData.
        std::unordered_set<std::string> g_createdActorSwingBoneNames{};
        // 建骨时 dynamicCollider 还不存在（SetDefaultValues 不建它，它是之后由 Awake/克隆
        // 流程建的），所以那会儿写碰撞体会被 null 检查静默跳过 —— 实测半径始终是默认的
        // 0.05。改成在活体克隆上按骨名补写（dump 证明那时 collider 非空）。指针活不过克隆，
        // 按名匹配是这个仓库既有的模式。
        std::unordered_map<std::string, LocalIpBoneSwing> g_swingParamsByBoneName{};
        // Host ActorSwingChains WE created, so a re-fired RegisterBones reuses ours instead of
        // stacking duplicates — and, critically, so we never mistake the GAME's own chain (its
        // skirt chain also lives on Hips) for one of ours and dump mod roots into it. Doing that
        // makes UpdateChainInfo truncate the shared chain to the shortest member length (skirt
        // 5 layers -> 3 when depth-3 bow roots join), which both kills the skirt's lower layers
        // and flails the bows on a solver that isn't theirs.
        std::unordered_set<void*> g_createdHostChains{};
        std::vector<Il2CppGCHandle> g_runtimeMeshHandles{};
        std::vector<Il2CppGCHandle> g_runtimeBoneHandles{};
        std::vector<Il2CppGCHandle> g_runtimeMaterialHandles{};
        std::unordered_map<void*, std::vector<void*>> g_hybridBonesByRenderer{};
        std::unordered_map<void*, std::vector<PersistentMaterialTextureOverride>> g_materialTextureOverrides{};
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
        std::unordered_map<std::string, std::vector<void*>> g_reapplyRootsByMod{};
        std::unordered_map<std::string, std::vector<void*>> g_loadedSourceGameObjects{};
        std::vector<ActiveAnimationRigContext> g_activeAnimationRigs{};
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
        std::unordered_set<void*> g_nativeChainAttachedRoots{};

        bool AttachNativeChainToLiveRoot(UnityResolve::UnityType::Transform* rootTransform);
        void RestoreRendererPropertyBlockSnapshots(void* renderer, size_t materialCount);
        void ApplyPersistentTextureOverrides(void* renderer);

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
                if (const auto f = componentClass->Get<UnityResolve::Field>("useWindGlobalForce"))
                    *reinterpret_cast<bool*>(
                        reinterpret_cast<std::uintptr_t>(component) + f->offset) = swing->useWindGlobalForce;
                // dynamicCollider 是引用字段，SetDefaultValues 已经建好实例（护士服字段 dump
                // 里非空），所以只填它的字段，不用自己 new。float_A=半径、float_B=次半径
                // (真实裙摆授权 float_A 0.024~0.03 / float_B 0.05)。
                // 碰撞体这里写不了：dynamicCollider 此刻还是 null（SetDefaultValues 不建它）。
                // 由 ApplySwingCollider 在活体克隆上按骨名补写。
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
        // 按固定 offset 直写：FindClassByName("ActorSwingDynamicCollider") 返回的类 fields 为空
        // (实测)，Get<Field>() 一律 null。布局由 base 好碰撞体的原始字节确认，与序列化结构一致：
        //   type@16 collisionMask@20 vector3_A@24 vector3_B@36 float_A@48 float_B@52
        // base/LeftBackSkirt2_S 实测 = type 0 / mask 1 / float_A 0.01 / float_B 0.05。
        size_t ApplySwingCollider(void* bone, UnityResolve::Class* dynamicBoneClass) {
            const auto it = g_swingParamsByBoneName.find(GetUnityObjectNameString(bone));
            if (it == g_swingParamsByBoneName.end() || it->second.colliderRadius < 0.0f) return 0;
            const auto cf = dynamicBoneClass->Get<UnityResolve::Field>("dynamicCollider");
            if (!cf) return 0;
            const auto collider = *reinterpret_cast<void**>(
                reinterpret_cast<std::uintptr_t>(bone) + cf->offset);
            if (!collider) return 0;
            const auto at = reinterpret_cast<std::uintptr_t>(collider);
            *reinterpret_cast<int*>(at + 16) = it->second.colliderType;
            *reinterpret_cast<int*>(at + 20) = it->second.collisionMask;
            *reinterpret_cast<float*>(at + 48) = it->second.colliderRadius;
            *reinterpret_cast<float*>(at + 52) = 0.05f;   // float_B 次半径，同 base
            return 1;
        }

        size_t AddActorSwingBonesToAnimationData(void* rootTransform, void* initializeData) {
            if (!rootTransform || !initializeData) {
                Log::Warn("[ModAsset] ActorSwing scan skipped: root or initializeData is null.");
                return 0;
            }
            const auto dynamicBoneClass = FindClassByName("ActorSwingDynamicBone");
            const auto initializeDataClass = FindClassByName("CampusActorAnimationInitializeData");
            if (!dynamicBoneClass || !initializeDataClass) {
                Log::WarnFmt("[ModAsset] ActorSwing scan skipped: dynamicBoneClass=%p initializeDataClass=%p",
                    dynamicBoneClass, initializeDataClass);
                return 0;
            }

            auto dynamicBones = initializeDataClass->GetValue<UnityResolve::UnityType::List<void*>*>(
                initializeData, "swingDynamicBones");
            if (!dynamicBones) {
                Log::Warn("[ModAsset] ActorSwing scan skipped: initializeData.swingDynamicBones is null.");
                return 0;
            }

            const auto rootGameObject = reinterpret_cast<UnityResolve::UnityType::Component*>(rootTransform)
                ->GetGameObject();
            const auto allBones = rootGameObject && IsNativeObjectAlive(rootGameObject)
                ? rootGameObject->GetComponentsInChildren<void*>(dynamicBoneClass, true)
                : std::vector<void*>{};
            Log::InfoFmt("[ModAsset] ActorSwing scan: root=%p rootGameObject=%p name=%s components=%zu dataList=%d",
                rootTransform, rootGameObject,
                rootGameObject ? GetUnityObjectNameString(rootGameObject).c_str() : "(null)",
                allBones.size(), dynamicBones->size);
            size_t added = 0;
            for (const auto bone : allBones) {
                if (!bone || !IsNativeObjectAlive(bone)) continue;
                bool exists = false;
                if (dynamicBones->pList) {
                    for (int i = 0; i < dynamicBones->size; ++i) {
                        if (dynamicBones->pList->At(static_cast<unsigned int>(i)) == bone) {
                            exists = true;
                            break;
                        }
                    }
                }
                if (!exists && ListAddManaged(dynamicBones, bone)) {
                    ++added;
                }
            }

            if (const auto chainClass = FindClassByName("ActorSwingChain")) {
                const auto chains = rootGameObject && IsNativeObjectAlive(rootGameObject)
                    ? rootGameObject->GetComponentsInChildren<void*>(chainClass, true)
                    : std::vector<void*>{};
                // Select the current character's mod-created bones by NAME on the live
                // instance. The graft ran on the loaded prefab, then the game Instantiated
                // it — so pointer-based tracking (the old g_createdActorSwingBones set, and
                // the manual Transform.GetParent walk that crashed at ModRuntime.cpp:419)
                // never matched the scene clone. allBones is this root's live dynamic-bone
                // snapshot; the clone preserves GameObject names, so name matching finds them.
                std::vector<void*> currentRootCreatedBones;
                size_t collidersApplied = 0;
                for (const auto bone : allBones) {
                    if (!bone || !IsNativeObjectAlive(bone)) continue;
                    if (g_createdActorSwingBoneNames.count(GetUnityObjectNameString(bone))) {
                        currentRootCreatedBones.emplace_back(bone);
                        collidersApplied += ApplySwingCollider(bone, dynamicBoneClass);
                    }
                }
                if (collidersApplied)
                    Log::InfoFmt("[ModAsset] ActorSwing colliders applied: %zu/%zu",
                        collidersApplied, currentRootCreatedBones.size());
                // ponytail: un-modded character has no mod-created bones here — don't touch
                // its swing chains (OnEnable/UpdateChainInfo) at all, restore vanilla behavior.
                if (currentRootCreatedBones.empty()) return added;
                // The mod's bones (nurse-dress ribbons/wings/stethoscope) belong to NO existing
                // chain (topology: their roots hang off LeftShoulder/RightShoulder/Spine2, not
                // under any base-costume swing chain). Source rui-nurs drives them with a single
                // ActorSwingChain on Pelvis whose rootBones lists each sub-chain top. Reproduce
                // that here: host one ActorSwingChain on Pelvis, add the mod's chain-root bones
                // (those whose parent is NOT a dynamic bone) to its rootBones, register it into
                // initializeData.swingChains, then UpdateChainInfo. Uses ListAddManaged because
                // UnityResolve's List::Add faults on the un-inflated generic method.
                std::vector<void*> chainRootBones;  // mod bones that top their own sub-chain
                for (const auto bone : currentRootCreatedBones) {
                    const auto transform = reinterpret_cast<UnityResolve::UnityType::Component*>(bone)->GetTransform();
                    const auto parent = transform ? transform->GetParent() : nullptr;
                    const auto parentGo = parent ? parent->GetGameObject() : nullptr;
                    const bool parentDynamic = parentGo && parentGo->GetComponent<void*>(dynamicBoneClass);
                    Log::InfoFmt("[ModAsset] ActorSwing created-bone topology: name=%s parent=%s parentDynamic=%d",
                        GetUnityObjectNameString(bone).c_str(),
                        parent ? GetUnityObjectNameString(parent).c_str() : "(null)", parentDynamic ? 1 : 0);
                    if (!parentDynamic) chainRootBones.emplace_back(bone);
                }

                // Host the chain on the skeleton root. Source (偶像荣耀) names it "Pelvis";
                // the mod grafts onto Gakumas' base skeleton whose root is "Hips" — so accept
                // either, and fall back to the RegisterBones root GameObject so we never no-op.
                UnityResolve::UnityType::GameObject* pelvisGo = nullptr;
                if (!chainRootBones.empty()) {
                    const auto t0 = reinterpret_cast<UnityResolve::UnityType::Component*>(chainRootBones.front())->GetTransform();
                    for (auto p = t0; p && IsNativeObjectAlive(p); p = p->GetParent()) {
                        const auto n = GetUnityObjectNameString(p);
                        if (n == "Pelvis" || n == "Hips") { pelvisGo = p->GetGameObject(); break; }
                    }
                }
                if (!pelvisGo) pelvisGo = rootGameObject;
                // Template List type from any existing chain's rootBones (List<ActorSwingDynamicBone>).
                void* templateRootList = nullptr;
                for (const auto chain : chains) {
                    if (const auto rb = chainClass->GetValue<void*>(chain, "rootBones")) { templateRootList = rb; break; }
                }

                // Walk a root's chain and return its length (bones, tip included).
                const auto chainDepth = [&](void* root) {
                    int depth = 0;
                    void* bone = root;
                    auto t = reinterpret_cast<UnityResolve::UnityType::Component*>(root)->GetTransform();
                    while (bone && t && IsNativeObjectAlive(t)) {
                        ++depth;
                        UnityResolve::UnityType::Transform* next = nullptr;
                        void* nextBone = nullptr;
                        const int cc = t->GetChildCount();
                        for (int i = 0; i < cc; ++i) {
                            const auto child = t->GetChild(i);
                            const auto go = child ? child->GetGameObject() : nullptr;
                            if (const auto c = go ? go->GetComponent<void*>(dynamicBoneClass) : nullptr) {
                                next = child;
                                nextBone = c;
                                break;
                            }
                        }
                        if (!next) break;
                        t = next;
                        bone = nextBone;
                    }
                    return depth;
                };
                // One chain per chain-length. UpdateChainInfo gives a chain only as many layers
                // as its SHORTEST member chain (measured: nurse 4/4/3 -> 2 layers; chisaki with
                // 17 single-bone chains mixed in -> 1 layer, killing all 28 dress chains; after
                // dropping those -> 7 layers). Mixing lengths therefore truncates the long chains,
                // so group by length — every group is uniform and each chain keeps its full depth.
                std::map<int, std::vector<void*>> rootsByDepth;
                for (const auto root : chainRootBones) rootsByDepth[chainDepth(root)].emplace_back(root);
                std::string groupStat;
                for (const auto& [len, roots] : rootsByDepth)
                    groupStat += std::to_string(roots.size()) + "x" + std::to_string(len) + " ";
                Log::InfoFmt("[ModAsset] ActorSwing chain groups (roots x length): %s", groupStat.c_str());

                // Mod chains we already put on Pelvis, in component order == creation order, so a
                // re-fired RegisterBones reuses them instead of stacking duplicates. ONLY chains
                // we created (g_createdHostChains) — never the game's own chain on Hips, or we
                // truncate it (see g_createdHostChains comment).
                std::vector<void*> existingHostChains;
                for (const auto chain : chains) {
                    const auto go = reinterpret_cast<UnityResolve::UnityType::Component*>(chain)->GetGameObject();
                    if (go == pelvisGo && g_createdHostChains.count(chain)) existingHostChains.emplace_back(chain);
                }

                size_t chainRootsAdded = 0, swingChainRegistered = 0, groupIndex = 0;
                for (const auto& [chainLen, groupRoots] : rootsByDepth) {
                    if (!pelvisGo || !templateRootList || groupRoots.empty()) break;
                    void* hostChain = groupIndex < existingHostChains.size()
                        ? existingHostChains[groupIndex] : nullptr;
                    ++groupIndex;
                    const bool createdChain = hostChain == nullptr;
                    if (!hostChain) hostChain = AddComponentByClass(pelvisGo, chainClass);
                    if (hostChain && createdChain) g_createdHostChains.insert(hostChain);
                    if (hostChain) {
                        auto rootBones = chainClass->GetValue<void*>(hostChain, "rootBones");
                        if (!rootBones) {
                            rootBones = CreateObjectLike(templateRootList);
                            if (rootBones) {
                                if (const auto f = chainClass->Get<UnityResolve::Field>("rootBones"))
                                    *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(hostChain) + f->offset) = rootBones;
                            }
                        }
                        if (rootBones) {
                            const auto rbList = reinterpret_cast<UnityResolve::UnityType::List<void*>*>(rootBones);
                            // UpdateChainInfo (RE'd from the unpacked iOS binary; findings §9)
                            // builds layers by walking transform.GetChild(0) from each ROOT and
                            // requiring an ActorSwingDynamicBone component on every step — so
                            // rootBones takes only the sub-chain tops; the rest of each chain is
                            // discovered by that walk. (The old all-bones fill made every bone a
                            // 1-layer chain root.)
                            for (const auto bone : groupRoots) {
                                bool exists = false;
                                if (rbList->pList) {
                                    for (int i = 0; i < rbList->size; ++i)
                                        if (rbList->pList->At(static_cast<unsigned int>(i)) == bone) { exists = true; break; }
                                }
                                if (!exists && ListAddManaged(rootBones, bone)) ++chainRootsAdded;
                            }
                        }
                        // Register the chain so the rig actually drives it. A reused host
                        // (createdChain==false) from a prior RegisterBones fire may not be in
                        // swingChains — if it isn't, the rig never drives it and its whole
                        // chain stays active=0. So register any host not already present, not
                        // just freshly-created ones (dedup by scanning the list first).
                        if (const auto initDataClass = FindClassByName("CampusActorAnimationInitializeData")) {
                            if (const auto swingChains = initDataClass->GetValue<void*>(initializeData, "swingChains")) {
                                bool alreadyRegistered = false;
                                if (const auto scList = reinterpret_cast<UnityResolve::UnityType::List<void*>*>(swingChains);
                                    scList && scList->pList) {
                                    for (int i = 0; i < scList->size; ++i)
                                        if (scList->pList->At(static_cast<unsigned int>(i)) == hostChain) { alreadyRegistered = true; break; }
                                }
                                if (!alreadyRegistered && ListAddManaged(swingChains, hostChain)) ++swingChainRegistered;
                            }
                        }
                        if (createdChain) {
                            if (const auto onEnable = chainClass->Get<UnityResolve::Method>("OnEnable"))
                                onEnable->Invoke<void>(hostChain);
                        }
                        // Diagnostic: which bones did UpdateChainInfo put in which layer, vs a base
                        // chain? It omits each chain's last bone (the tip only defines the final
                        // segment and must not be simulated), so a complete chain of depth N yields
                        // N-1 layers.
                        const auto layerStats = [&](void* ch) -> std::string {
                            const auto chainInfoClass = FindClassByName("ChainInfo");
                            const auto layerClass = FindClassByName("ChainLayerInfo");
                            if (!chainInfoClass || !layerClass) return "n/a";
                            const auto ci = chainClass->GetValue<void*>(ch, "chains");
                            if (!ci) return "noChains";
                            const auto ly = chainInfoClass->GetValue<UnityResolve::UnityType::List<void*>*>(ci, "layers");
                            if (!ly || !ly->pList) return "noLayers";
                            int total = 0;
                            for (int i = 0; i < ly->size; ++i) {
                                if (const auto layer = ly->pList->At(static_cast<unsigned int>(i)))
                                    if (const auto bl = layerClass->GetValue<UnityResolve::UnityType::List<void*>*>(layer, "bones"))
                                        total += bl->size;
                            }
                            return std::to_string(ly->size) + "layers/" + std::to_string(total) + "bones";
                        };
                        // Name every bone per layer: a bone appearing twice means it gets simulated
                        // twice and the chain explodes, which is exactly what hand-built layers did.
                        const auto layerNames = [&](void* ch) {
                            const auto chainInfoClass = FindClassByName("ChainInfo");
                            const auto layerClass = FindClassByName("ChainLayerInfo");
                            const auto ci = chainInfoClass && layerClass ? chainClass->GetValue<void*>(ch, "chains") : nullptr;
                            const auto ly = ci ? chainInfoClass->GetValue<UnityResolve::UnityType::List<void*>*>(ci, "layers") : nullptr;
                            if (!ly || !ly->pList) return;
                            for (int i = 0; i < ly->size; ++i) {
                                const auto layer = ly->pList->At(static_cast<unsigned int>(i));
                                const auto bl = layer ? layerClass->GetValue<UnityResolve::UnityType::List<void*>*>(layer, "bones") : nullptr;
                                std::string names;
                                if (bl && bl->pList)
                                    for (int b = 0; b < bl->size; ++b)
                                        names += GetUnityObjectNameString(bl->pList->At(static_cast<unsigned int>(b))) + " ";
                                Log::InfoFmt("[ModAsset] ActorSwing layer[%d] active=%d bones=%s", i,
                                    layer ? layerClass->GetValue<bool>(layer, "active") : 0, names.c_str());
                            }
                        };
                        // UpdateChainInfo builds the layers; nothing here should touch them. It looked
                        // broken while it produced a single layer for us, but the chains were simply
                        // missing their tips: it omits each chain's last bone, so the tipless
                        // Wing1->Wing2 lost Wing2 (the bone that should swing) and yielded 1 layer.
                        // With the tips supplied it yields the expected depth-1 layers on its own.
                        const std::string beforeStat = layerStats(hostChain);
                        if (const auto updateInfo = chainClass->Get<UnityResolve::Method>("UpdateChainInfo")) {
                            static bool loggedAddr = false;
                            if (!loggedAddr) {
                                loggedAddr = true;
                                const auto ga = reinterpret_cast<void*>(GetModuleHandleW(L"GameAssembly.dll"));
                                const auto ud = chainClass->Get<UnityResolve::Method>("UpdateHierarchyDepth");
                                Log::InfoFmt("[ModAsset] ActorSwing method addrs: gaBase=%p UpdateChainInfo=%p UpdateHierarchyDepth=%p",
                                    ga, updateInfo->function, ud ? ud->function : nullptr);
                            }
                            updateInfo->Invoke<void>(hostChain);
                        }
                        Log::InfoFmt("[ModAsset] ActorSwing chain[len=%d]: roots=%zu created=%d before=%s after=%s",
                            chainLen, groupRoots.size(), createdChain ? 1 : 0,
                            beforeStat.c_str(), layerStats(hostChain).c_str());
                        if (chainLen >= 3) layerNames(hostChain);
                    }
                }
                Log::InfoFmt("[ModAsset] ActorSwing new chain: pelvis=%p chainRoots=%zu groups=%zu added=%zu registered=%zu createdBones=%zu",
                    pelvisGo, chainRootBones.size(), rootsByDepth.size(), chainRootsAdded,
                    swingChainRegistered, currentRootCreatedBones.size());
            }
            return added;
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
            const auto added = AddActorSwingBonesToAnimationData(rootTransform, initializeData);
            if (added) {
                const auto dynamicBones = initializeDataClass
                    ? initializeDataClass->GetValue<UnityResolve::UnityType::List<void*>*>(
                        initializeData, "swingDynamicBones")
                    : nullptr;
                Log::InfoFmt("[ModAsset] ActorSwing data grafted before CampusActorAnimationRig.RegisterBones: added=%zu total=%d",
                    added, dynamicBones ? dynamicBones->size : 0);
            }
            if (nativeChainAttached) {
                const auto chainsAdded = AddActorSwingChainsToAnimationData(rootTransform, initializeData);
                Log::InfoFmt("[ModAsset] Native ActorSwing chain registered before CampusActorAnimationRig.RegisterBones: added=%zu",
                    chainsAdded);
            }
            CampusActorAnimationRig_RegisterBones_Orig(self, initializeData);
            if (g_nativeChainValidation) {
                LogActorSwingChainStats(rootTransform, "native");
            }
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
            if (const auto iter = g_bundleHandleMap.find(normalizedPath); iter != g_bundleHandleMap.end()) {
                return iter->second;
            }

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

            g_bundleHandleMap.emplace(normalizedPath, bundleHandle);
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
            if (const auto iter = g_loadedAssetHandleMap.find(cacheKey); iter != g_loadedAssetHandleMap.end()) {
                auto cachedAsset = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", iter->second);
                if (cachedAsset && IsNativeObjectAlive(cachedAsset)) {
                    return cachedAsset;
                }
                UnityResolve::Invoke<void>("il2cpp_gchandle_free", std::exchange(iter->second, nullptr));
                g_loadedAssetHandleMap.erase(iter);
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

            g_loadedAssetHandleMap[cacheKey] = UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", modAsset, false);
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

            g_runtimeMeshHandles.emplace_back(UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", clone, false));
            Log::InfoFmt("[ModAsset] Cloned mod mesh before patch: %s renderer=%zu sourceMesh=%p clonedMesh=%p",
                sourceName.c_str(),
                rendererIndex,
                obj,
                clone);
            return clone;
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
            static auto resetBounds = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer", "ResetBounds");
            static auto resetLocalBounds = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer", "ResetLocalBounds");

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
            std::vector<LocalIpExtraBone>& extraBones) {
            if (replacement.skeletonAssetName.empty()) return false;

            const auto asset = LoadLocalModAssetFromBundle(
                replacement.bundleHandle,
                replacement.bundlePath,
                replacement.skeletonAssetName,
                "TextAsset");
            const auto text = GetTextAssetText(asset);
            if (text.empty()) {
                Log::ErrorFmt("[ModAsset] IP skeleton sidecar is empty or unavailable: %s asset=%s",
                    replacement.sourceName.c_str(), replacement.skeletonAssetName.c_str());
                return false;
            }

            try {
                const auto document = nlohmann::json::parse(text);
                constexpr int kAbRuntimeProtocol = 1;
                if (!document.contains("runtimeProtocol") || !document["runtimeProtocol"].is_number_integer()) {
                    throw std::runtime_error("runtimeProtocol is required (exporter/runtime mismatch)");
                }
                const auto runtimeProtocol = document["runtimeProtocol"].get<int>();
                if (runtimeProtocol != kAbRuntimeProtocol) {
                    throw std::runtime_error(
                        "unsupported runtimeProtocol=" + std::to_string(runtimeProtocol)
                        + ", expected=" + std::to_string(kAbRuntimeProtocol));
                }
                if (!document.contains("buildId") || !document["buildId"].is_string()
                    || document["buildId"].get<std::string>().empty()) {
                    throw std::runtime_error("buildId is required for bundle/log correlation");
                }
                Log::InfoFmt("[ModAsset] IP skeleton sidecar protocol=%d buildId=%s source=%s",
                    runtimeProtocol, document["buildId"].get<std::string>().c_str(),
                    replacement.sourceName.c_str());
                if (!document.contains("bones") || !document["bones"].is_array()) {
                    throw std::runtime_error("bones array is required");
                }

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
                    LocalIpBoneSwing swing{
                        s.value("damping", 0.0f), s.value("stiffness", 0.0f),
                        s.value("spring", 0.0f), s.value("mass", 0.0f),
                        s.value("useWindGlobalForce", false) };
                    swing.rootWeight = s.value("rootWeight", -1.0f);
                    swing.pendulum = s.value("pendulum", -1.0f);
                    // collider 嵌在 swing 里，不是 bone 对象顶层 —— 从 item 找会永远落空、
                    // 静默跳过写入（rootWeight 从 s 读所以一直是对的，只有 collider 中招）。
                    if (s.contains("collider") && s["collider"].is_object()) {
                        const auto& c = s["collider"];
                        swing.colliderRadius = c.value("radius", -1.0f);
                        swing.colliderType = c.value("type", 0);
                        swing.collisionMask = c.value("collisionMask", -1);
                    }
                    return swing;
                };

                bones.clear();
                bones.reserve(document["bones"].size());
                for (const auto& item : document["bones"]) {
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
                    bones.emplace_back(std::move(bone));
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
                        extraBones.emplace_back(std::move(bone));
                    }
                }
                return !bones.empty();
            }
            catch (const std::exception& e) {
                Log::ErrorFmt("[ModAsset] Cannot parse IP skeleton sidecar: %s asset=%s error=%s",
                    replacement.sourceName.c_str(), replacement.skeletonAssetName.c_str(), e.what());
                bones.clear();
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
            if (g_nativeChainAttachedRoots.contains(rootGameObject)) return true;

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
                g_nativeChainAttachedRoots.emplace(rootGameObject);
                Log::InfoFmt("[ModAsset] Attached native chain subtree to live actor: root=%s source=%s matchedMesh=%s clone=%p",
                    GetUnityObjectNameString(rootGameObject).c_str(), replacement.sourceName.c_str(),
                    GetUnityObjectNameString(matchedMesh).c_str(), subtreeClone);
                return true;
            }
            return false;
        }

        UnityArray<void*>* BuildHybridBoneArray(void* originalRenderer,
            UnityArray<void*>* originalBones,
            UnityArray<void*>* modBones,
            const std::vector<LocalIpBone>& sidecarBones,
            const std::vector<LocalIpExtraBone>& extraBones,
            const std::string& sourceName,
            const size_t rendererIndex,
            size_t& matchedBones,
            size_t& createdBones,
            std::vector<void*>& createdDynamicBones) {
            if (!originalRenderer || !originalBones || !modBones || sidecarBones.size() != modBones->max_length) return nullptr;
            if (const auto cached = g_hybridBonesByRenderer.find(originalRenderer); cached != g_hybridBonesByRenderer.end()
                && cached->second.size() == sidecarBones.size()
                && std::all_of(cached->second.begin(), cached->second.end(), [](const void* bone) { return bone && IsNativeObjectAlive(const_cast<void*>(bone)); })) {
                const auto transformClass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
                if (!transformClass) return nullptr;
                auto result = UnityArray<void*>::New(transformClass, cached->second.size());
                for (size_t i = 0; i < cached->second.size(); ++i) result->At(static_cast<unsigned int>(i)) = cached->second[i];
                matchedBones = 0;
                for (const auto& bone : sidecarBones) if (BuildBoneNameIndexMap(originalBones).contains(bone.name)) ++matchedBones;
                createdBones = sidecarBones.size() - matchedBones;
                return result;
            }

            const auto transformClass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
            const auto gameObjectClass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
            if (!transformClass || !gameObjectClass) return nullptr;

            const auto originalBoneIndexMap = BuildBoneNameIndexMap(originalBones);
            std::unordered_map<std::string, UnityResolve::UnityType::Transform*> existingCreatedBones;
            if (const auto hierarchyRoot = GetHierarchyRootGameObject(originalRenderer)) {
                const auto hierarchyTransforms = reinterpret_cast<UnityResolve::UnityType::GameObject*>(
                    hierarchyRoot)->GetComponentsInChildren<void*>(transformClass, true);
                for (const auto item : hierarchyTransforms) {
                    const auto name = GetUnityObjectNameString(item);
                    if (!g_createdActorSwingBoneNames.contains(name)) continue;
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

            const auto createBone = [&](const std::string& name, UnityResolve::UnityType::Transform* parent,
                const UnityResolve::UnityType::Vector3& localPosition,
                const UnityResolve::UnityType::Quaternion& localRotation,
                const UnityResolve::UnityType::Vector3& localScale,
                const std::optional<LocalIpBoneSwing>& swing) -> UnityResolve::UnityType::Transform* {
                auto gameObject = gameObjectClass->New<UnityResolve::UnityType::GameObject>();
                if (!gameObject) return nullptr;
                UnityResolve::UnityType::GameObject::Create(gameObject, name);
                auto transform = gameObject->GetTransform();
                if (!transform || !SetTransformParent(transform, parent)) return nullptr;
                transform->SetLocalPosition(localPosition);
                transform->SetLocalRotation(localRotation);
                transform->SetLocalScale(localScale);
                if (const auto dynamicBoneClass = FindClassByName("ActorSwingDynamicBone")) {
                    const auto dynamicBone = AddComponentByClass(gameObject, dynamicBoneClass);
                    if (dynamicBone && InitializeActorSwingDynamicBone(dynamicBone, dynamicBoneClass, swing)) {
                        g_createdActorSwingBoneNames.emplace(name);
                        if (swing) g_swingParamsByBoneName[name] = *swing;
                        createdDynamicBones.emplace_back(dynamicBone);
                    }
                }
                g_runtimeBoneHandles.emplace_back(UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", gameObject, false));
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
                        sidecarBone.localRotation, sidecarBone.localScale, sidecarBone.swing);
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
                    extra.localPosition, extra.localRotation, extra.localScale, extra.swing) : nullptr;
                if (createdTransform) {
                    extraTransforms[extra.name] = createdTransform;
                    ++extraCreated;
                }
            }
            if (!extraBones.empty()) {
                Log::InfoFmt("[ModAsset] Chain tips attached: %zu/%zu renderer=%zu",
                    extraCreated, extraBones.size(), rendererIndex);
            }

            g_hybridBonesByRenderer[originalRenderer] = hybridBones;
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
            if (g_transformedMeshSet.contains(modMesh)) return true;

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

            SetMeshVertices(modMesh, vertices);
            RecalculateMeshBounds(modMesh);
            g_transformedMeshSet.emplace(modMesh);
            Log::InfoFmt("[ModAsset] Transformed mod mesh vertices to original renderer space: %s renderer=%zu vertices=%zu originalRenderer=\"%s\" modRenderer=\"%s\"",
                sourceName.c_str(),
                rendererIndex,
                static_cast<size_t>(vertices->max_length),
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

            const auto originalRootName = GetUnityObjectNameString(GetSkinnedMeshRendererRootBone(originalRenderer));
            const auto modRootName = GetUnityObjectNameString(GetSkinnedMeshRendererRootBone(modRenderer));
            if (originalRootName.empty() || originalRootName != modRootName
                || modBindposes->max_length != modBones->max_length) {
                Log::ErrorFmt("[ModAsset] Lossless IP skeleton requires matching roots and bone/bindpose counts: %s renderer=%zu originalRoot=\"%s\" modRoot=\"%s\" modBones=%zu modBindposes=%zu",
                    sourceName.c_str(), rendererIndex, originalRootName.c_str(), modRootName.c_str(),
                    static_cast<size_t>(modBones->max_length), static_cast<size_t>(modBindposes->max_length));
                return false;
            }

            std::vector<LocalIpBone> sidecarBones;
            std::vector<LocalIpExtraBone> extraSwingBones;
            if (!LoadIpBoneSidecar(replacement, sidecarBones, extraSwingBones) || sidecarBones.size() != modBones->max_length) {
                Log::ErrorFmt("[ModAsset] Lossless IP skeleton sidecar count mismatch: %s renderer=%zu sidecar=%zu modBones=%zu",
                    sourceName.c_str(), rendererIndex, sidecarBones.size(), static_cast<size_t>(modBones->max_length));
                return false;
            }

            // The graft (BuildHybridBoneArray) builds the whole skeleton from the sidecar
            // JSON by name/order and creates missing bones live; it never reads the mod
            // SMR's bone names. Requiring modBones[i].name == sidecar[i].name only forced
            // the exporter to embed synthesized Transforms into the bundle, which Unity 6
            // native LoadAsset crashes on. Validate the sidecar hierarchy alone.
            for (size_t i = 0; i < sidecarBones.size(); ++i) {
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
            const auto hybridBones = BuildHybridBoneArray(
                originalRenderer, originalBones, modBones, sidecarBones, extraSwingBones, sourceName, rendererIndex,
                matchedBones, createdBones, createdDynamicBones);
            if (!hybridBones) return false;

            auto adjustedBindposes = UnityArray<UnityResolve::UnityType::Matrix4x4>::New(matrixClass, modBindposes->max_length);
            const auto bindposeSpaceAdjustment = GetBindposeRendererSpaceAdjustment(originalRenderer, modRenderer);
            for (std::uintptr_t i = 0; i < modBindposes->max_length; ++i) {
                adjustedBindposes->At(static_cast<unsigned int>(i)) = MultiplyMatrix4x4(
                    modBindposes->At(static_cast<unsigned int>(i)), bindposeSpaceAdjustment);
            }

            SetMeshBindposes(modMesh, adjustedBindposes);
            SetSkinnedMeshRendererBones(originalRenderer, hybridBones);
            RecalculateMeshBounds(modMesh);
            Log::InfoFmt("[ModAsset] Applied lossless IP skeleton graft: %s renderer=%zu matchedBones=%zu createdBones=%zu bones=%zu boneWeights=%zu swingPrepared=%zu droppedInfluences=0 fallbackVertices=0",
                sourceName.c_str(), rendererIndex, matchedBones, createdBones,
                static_cast<size_t>(hybridBones->max_length), static_cast<size_t>(modBoneWeights->max_length),
                createdDynamicBones.size());
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

        void RegisterPersistentMaterialTextureOverride(void* material, const int propertyId,
            const std::string& propertyName, void* texture) {
            if (!material || propertyId < 0 || !texture) return;
            std::lock_guard lock(g_materialOverrideMutex);
            auto& overrides = g_materialTextureOverrides[material];
            if (const auto iter = std::find_if(overrides.begin(), overrides.end(),
                [propertyId](const auto& entry) { return entry.propertyId == propertyId; });
                iter != overrides.end()) {
                iter->propertyName = propertyName;
                iter->texture = texture;
            }
            else {
                overrides.emplace_back(PersistentMaterialTextureOverride{
                    propertyId,
                    propertyName,
                    texture,
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
                if (!SetMaterialTexture(material, propertyId, textureAsset)) {
                    Log::ErrorFmt("[ModAsset] Material.SetTexture failed: %s renderer=%zu slot=%d property=%s",
                        replacement.sourceName.c_str(),
                        rendererIndex,
                        textureReplacement.materialSlot,
                        textureReplacement.propertyName.c_str());
                    continue;
                }
                RegisterPersistentMaterialTextureOverride(
                    material, propertyId, textureReplacement.propertyName, textureAsset);
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

        void RememberLoadedSourceGameObject(
            const std::string& sourceName,
            void* gameObject) {
            if (!gameObject || std::strcmp(GetUnityObjectClassName(gameObject), "GameObject") != 0) {
                return;
            }
            const auto key = NormalizeAssetName(sourceName);
            bool isRegisteredSource = false;
            {
                std::shared_lock replacementLock(g_replacementMutex);
                isRegisteredSource = std::any_of(
                    g_registeredReplacements.begin(),
                    g_registeredReplacements.end(),
                    [&key](const auto& replacement) {
                        return replacement
                            && NormalizeAssetName(replacement->sourceName) == key;
                    });
            }
            if (!isRegisteredSource) return;

            std::lock_guard lock(g_reversiblePatchMutex);
            auto& objects = g_loadedSourceGameObjects[key];
            if (std::find(objects.begin(), objects.end(), gameObject) == objects.end()) {
                objects.push_back(gameObject);
            }
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

        size_t RestoreLiveModInstances(const std::string& modId) {
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

            // Matching reads sharedMesh/sharedMaterials off a renderer list that
            // was snapshotted before anything was touched.  Restoring writes
            // bones, mesh, materials and toggles enabled on those same objects.
            // Interleaving the two meant later iterations kept reading the stale
            // snapshot through Unity after it had already been mutated, which
            // crashed inside UnityPlayer.  Match everything first, then write.
            std::unordered_set<void*> seenRenderers;
            std::vector<std::pair<void*, const ReversibleRendererPatch*>> matches;
            for (const auto& patch : patches) {
                if (!patch.patchedRenderer || !IsNativeObjectAlive(patch.patchedRenderer)
                    || !RendererMatchesPatch(patch.patchedRenderer, patch)) continue;
                if (!seenRenderers.emplace(patch.patchedRenderer).second) continue;
                matches.emplace_back(patch.patchedRenderer, &patch);
            }
            // Only sweep the scene for patches whose recorded renderer is gone or
            // no longer carries the Mod mesh -- a re-instantiated actor.
            if (matches.size() < patches.size()) {
                const auto rendererClass = Il2cppUtils::GetClass(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer");
                if (!rendererClass) return 0;
                for (const auto renderer : rendererClass->FindObjectsByType<void*>()) {
                    if (!renderer || !IsNativeObjectAlive(renderer)
                        || !seenRenderers.emplace(renderer).second) continue;
                    const auto patch = std::find_if(
                        patches.begin(), patches.end(), [renderer](const auto& candidate) {
                            return RendererMatchesPatch(renderer, candidate);
                        });
                    if (patch == patches.end()) continue;
                    matches.emplace_back(renderer, &*patch);
                }
            }

            std::unordered_set<void*> restoredSourceRoots;
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
                SetRendererSharedMaterials(renderer, restoredMaterials);
                SetSkinnedMeshRendererSharedMesh(renderer, patch->originalMesh);
                ClearRuntimePropertyBlocks(renderer, patch->originalMaterials.size());
                RefreshSkinnedMeshRendererState(renderer);

                if (const auto sourceRoot = GetSourceRootGameObject(
                        renderer, patch->sourceRootDepth)) {
                    restoredSourceRoots.emplace(sourceRoot);
                }
                ++restoredCount;
                Log::InfoFmt(
                    "[ModAsset] Hot-restored renderer: mod=%s source=%s renderer=%s mesh=%s",
                    modId.c_str(),
                    patch->sourceName.c_str(),
                    GetUnityObjectNameString(renderer).c_str(),
                    GetUnityObjectNameString(patch->originalMesh).c_str());
            }

            {
                std::lock_guard lock(g_reversiblePatchMutex);
                auto& targets = g_reapplyRootsByMod[modId];
                for (const auto root : restoredSourceRoots) {
                    if (std::find(targets.begin(), targets.end(), root) == targets.end()) {
                        targets.push_back(root);
                    }
                }
            }
            return restoredCount;
        }

        bool ApplySkinnedMeshReplacement(void* originalGameObject, void* modGameObject,
            const LocalModAssetReplacement& replacement) {
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

                if (replacement.replaceMaterials && modMaterials) {
                    rendererMaterialApplied |= SetRendererSharedMaterials(
                        pair.originalRenderer, modMaterials);
                }
                const auto activeMaterials = replacement.replaceMaterials && modMaterials
                    ? modMaterials
                    : originalMaterials;
                if (!replacement.replaceMaterials
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
                // Commit persistent texture overrides immediately.  The game's
                // later SetMaterialArray/Renderer.set_sharedMaterials call is
                // handled by the material-assignment hook below and will restore
                // this complete Mod material array after its write.
                ApplyPersistentTextureOverrides(pair.originalRenderer);
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

                const auto addedBones = AddActorSwingBonesToAnimationData(
                    context.rootTransform, context.initializeData);
                const auto addedChains = AddActorSwingChainsToAnimationData(
                    context.rootTransform, context.initializeData);
                if (addedBones > 0 || addedChains > 0) {
                    CampusActorAnimationRig_RegisterBones_Orig(
                        context.rig, context.initializeData);
                }

                size_t reactivatedTargets = 0;
                for (const auto target : matchingTargets) {
                    if (ReactivateGameObject(target)) ++reactivatedTargets;
                }
                if (reactivatedTargets > 0 || addedBones > 0 || addedChains > 0) {
                    ++refreshed;
                    Log::InfoFmt(
                        "[ModAsset] Hot-refreshed active character target: root=%s reactivatedTargets=%zu addedBones=%zu addedChains=%zu",
                        GetUnityObjectNameString(context.rootGameObject).c_str(),
                        reactivatedTargets,
                        addedBones,
                        addedChains);
                }
            }
            return refreshed;
        }

        std::vector<void*> CollectLiveReapplyTargets(
            const LocalModAssetReplacement& replacement) {
            struct SourceRendererIdentity {
                void* mesh{};
                int depthFromSourceRoot{};
                std::string rendererName;
            };

            std::vector<void*> targets;
            std::vector<void*> rememberedSources;
            {
                std::lock_guard lock(g_reversiblePatchMutex);
                if (const auto restored = g_reapplyRootsByMod.find(replacement.modId);
                    restored != g_reapplyRootsByMod.end()) {
                    for (const auto root : restored->second) AddUniqueLiveObject(targets, root);
                }
                if (const auto remembered = g_loadedSourceGameObjects.find(
                        NormalizeAssetName(replacement.sourceName));
                    remembered != g_loadedSourceGameObjects.end()) {
                    rememberedSources = remembered->second;
                }
            }

            const auto rendererClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer");
            if (!rendererClass) return targets;

            std::vector<SourceRendererIdentity> sourceRenderers;
            for (const auto source : rememberedSources) {
                if (!source || !IsNativeObjectAlive(source)) continue;
                AddUniqueLiveObject(targets, source);
                const auto renderers = reinterpret_cast<UnityResolve::UnityType::GameObject*>(
                    source)->GetComponentsInChildren<void*>(rendererClass, true);
                for (const auto renderer : renderers) {
                    const auto mesh = GetSkinnedMeshRendererSharedMesh(renderer);
                    if (!mesh) continue;
                    const SourceRendererIdentity identity{
                        mesh,
                        GetComponentDepthFromRoot(renderer, source),
                        GetUnityObjectNameString(renderer),
                    };
                    const auto duplicate = std::find_if(
                        sourceRenderers.begin(), sourceRenderers.end(),
                        [&identity](const auto& current) {
                            return current.mesh == identity.mesh
                                && current.depthFromSourceRoot == identity.depthFromSourceRoot
                                && current.rendererName == identity.rendererName;
                        });
                    if (duplicate == sourceRenderers.end()) sourceRenderers.push_back(identity);
                }
            }

            if (!sourceRenderers.empty()) {
                const auto renderers = rendererClass->FindObjectsByType<void*>();
                for (const auto renderer : renderers) {
                    if (!renderer || !IsNativeObjectAlive(renderer)) continue;
                    const auto mesh = GetSkinnedMeshRendererSharedMesh(renderer);
                    const auto rendererName = GetUnityObjectNameString(renderer);
                    const auto identity = std::find_if(
                        sourceRenderers.begin(), sourceRenderers.end(),
                        [mesh, &rendererName](const auto& current) {
                            return current.mesh == mesh
                                && (current.rendererName.empty()
                                    || current.rendererName == rendererName);
                        });
                    if (identity == sourceRenderers.end()) continue;
                    AddUniqueLiveObject(
                        targets,
                        GetSourceRootGameObject(renderer, identity->depthFromSourceRoot));
                }
            }
            return targets;
        }

        size_t ReapplyLiveModInstances(LocalModAssetReplacement& replacement) {
            if (replacement.replaceWholeObject || replacement.attachToOriginal) {
                Log::WarnFmt(
                    "[ModAsset] Hot reapply unsupported for whole-object/attach rule: mod=%s source=%s",
                    replacement.modId.c_str(),
                    replacement.sourceName.c_str());
                return 0;
            }

            const auto targets = CollectLiveReapplyTargets(replacement);
            if (targets.empty()) return 0;
            const auto modAsset = LoadLocalModReplacementAsset(replacement);
            if (!modAsset) return 0;

            size_t applied = 0;
            for (const auto target : targets) {
                if (ApplySkinnedMeshReplacement(target, modAsset, replacement)) ++applied;
            }
            for (const auto target : targets) {
                Log::InfoFmt("[ModAsset] Hot reapply target: object=%p name=\"%s\"",
                    target,
                    GetUnityObjectNameString(target).c_str());
            }
            const auto refreshedRigs = RefreshAnimationRigsAfterHotReapply(targets);
            Log::InfoFmt(
                "[ModAsset] Hot reapply finished: mod=%s source=%s targets=%zu applied=%zu refreshedRigs=%zu",
                replacement.modId.c_str(),
                replacement.sourceName.c_str(),
                targets.size(),
                applied,
                refreshedRigs);
            return applied;
        }

        void* ReplaceLocalModAssetIfNeeded(void* originalResult, const std::string& sourceName) {
            RememberLoadedSourceGameObject(sourceName, originalResult);
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
                g_loadHistory.emplace(result, assetName);
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
            // Prime the per-renderer cache while the Mod material array is still
            // installed; after the game's setter runs, the array contains the
            // original materials and a fresh scan would lose the evidence.
            (void)CollectRendererTextureOverrides(self);
            const auto result = Renderer_SetSharedMaterials_Orig(self, value, methodInfo);
            RestorePatchedMaterials(
                self, value, "set_sharedMaterials", Renderer_SetSharedMaterials_Orig);
            return result;
        }

        void* Renderer_SetMaterials_Hook(void* self, void* value, void* methodInfo) {
            (void)CollectRendererTextureOverrides(self);
            const auto result = Renderer_SetMaterials_Orig(self, value, methodInfo);
            RestorePatchedMaterials(
                self, value, "set_materials", Renderer_SetMaterials_Orig);
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

        bool InstallHooks() {
            bool ok = true;
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
            else {
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
                Log::Error("[ModAsset] Persistent material texture override methods unavailable.");
                ok = false;
            }
            return ok;
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
                affectedInstances += ReapplyLiveModInstances(*replacement);
            }
        }
        else if (stateChanged) {
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
            g_reapplyRootsByMod.clear();
            g_loadedSourceGameObjects.clear();
        }
        {
            std::lock_guard lock(g_animationRigMutex);
            g_activeAnimationRigs.clear();
        }
        Log::Info("[ModAsset] Standalone mod plugin shutdown.");
    }
}
