// Included inside ModRuntime.cpp's anonymous namespace. Experimental PC path.
// All struct-valued managed calls use runtime_invoke, not guessed native ABIs.
using GmiMatrix = std::array<float, 16>;
// 刚出现的渲染器：RegisterBones 那一帧 BakeMesh 会原生崩，隔几帧再烘；同时要求蒙皮数据自洽。
// 发现靠 RegisterBones 事件触发重扫（不再等 20 帧），所以整件衣服基本同时出现。
constexpr int GMI_SETTLE_FRAMES = 3;

// 没有全局开关：材质自己声明 _GmiBakedAfterDof 就画，没人声明就什么都不做。

UnityResolve::Method* GmiMethod(const char* klass, const char* name,
        const std::vector<std::string>& args = {}, const char* ns = "UnityEngine",
        const char* assembly = "UnityEngine.CoreModule.dll") {
    return FindMethodExact(assembly, ns, klass, name, args);
}

bool GmiCall(UnityResolve::Method* method, void* instance, void** args, void** result = nullptr) {
    if (!method || !method->address) return false;
    void* exception = nullptr;
    void* value = UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", method->address, instance, args, &exception);
    if (exception) {
        static std::set<std::string> reported;
        if (reported.insert(method->name).second) Log::ErrorFmt("[GmiBaked] Managed exception in %s", method->name.c_str());
        return false;
    }
    if (result) *result = value;
    return true;
}

template<class T> bool GmiValue(UnityResolve::Method* method, void* instance, void** args, T& value) {
    void* box = nullptr;
    if (!GmiCall(method, instance, args, &box) || !box) return false;
    const auto data = UnityResolve::Invoke<void*>("il2cpp_object_unbox", box);
    if (!data) return false;
    std::memcpy(&value, data, sizeof(T));
    return true;
}

GmiMatrix GmiMultiply(const GmiMatrix& a, const GmiMatrix& b) {
    GmiMatrix result{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                result[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
    return result;
}

void* GmiRtTexture(void* handle) {
    static auto get = GmiMethod("RTHandle", "get_rt", {}, "UnityEngine.Rendering", "Unity.RenderPipelines.Core.Runtime.dll");
    void* texture = nullptr;
    return handle && GmiCall(get, handle, nullptr, &texture) ? texture : nullptr;
}

void* GmiPostTexture(void* post, const std::string& name) {
    const auto klass = Il2cppUtils::GetClass("Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "PostProcessPass");
    const auto field = klass ? klass->Get<UnityResolve::Field>(name) : nullptr;
    return field ? GmiRtTexture(*reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(post) + field->offset)) : nullptr;
}

struct GmiBakedMesh {
    Il2CppGCHandle owner{};
    Il2CppGCHandle mesh{};
    int frame = -1;
};
std::vector<GmiBakedMesh> g_gmiBakedMeshes;
struct GmiBakedBlock { Il2CppGCHandle material{}; Il2CppGCHandle block{}; };
std::vector<GmiBakedBlock> g_gmiBakedBlocks;

void* GmiHandleTarget(Il2CppGCHandle handle) {
    return handle ? UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", handle) : nullptr;
}

void GmiRetireDeadBakedObjects() {
    std::erase_if(g_gmiBakedMeshes, [](const auto& item) {
        if (IsNativeObjectAlive(GmiHandleTarget(item.owner))) return false;
        if (const auto mesh = GmiHandleTarget(item.mesh); IsNativeObjectAlive(mesh)) DestroyComponentImmediate(mesh);
        UnityResolve::Invoke<void>("il2cpp_gchandle_free", item.owner);
        UnityResolve::Invoke<void>("il2cpp_gchandle_free", item.mesh);
        return true;
    });
    std::erase_if(g_gmiBakedBlocks, [](const auto& item) {
        if (IsNativeObjectAlive(GmiHandleTarget(item.material))) return false;
        UnityResolve::Invoke<void>("il2cpp_gchandle_free", item.material);
        UnityResolve::Invoke<void>("il2cpp_gchandle_free", item.block);
        return true;
    });
}

void* GmiBake(void* renderer, int frame) {
    auto found = std::find_if(g_gmiBakedMeshes.begin(), g_gmiBakedMeshes.end(),
        [renderer](const auto& item) { return GmiHandleTarget(item.owner) == renderer; });
    if (found == g_gmiBakedMeshes.end()) {
        static auto klass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Mesh");
        static auto ctor = GmiMethod("Mesh", ".ctor");
        void* mesh = klass ? UnityResolve::Invoke<void*>("il2cpp_object_new", klass->address) : nullptr;
        if (!mesh || !GmiCall(ctor, mesh, nullptr)) return nullptr;
        g_gmiBakedMeshes.push_back({
            UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", renderer, false),
            UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", mesh, false), -1 });
        found = std::prev(g_gmiBakedMeshes.end());
    }
    void* mesh = GmiHandleTarget(found->mesh);
    if (found->frame != frame) {
        static auto bake = GmiMethod("SkinnedMeshRenderer", "BakeMesh", { "Mesh", "Boolean" });
        bool useScale = false;
        void* args[]{ mesh, &useScale };
        if (!GmiCall(bake, renderer, args)) return nullptr;
        found->frame = frame;
    }
    return mesh;
}

void* GmiBlock(void* material) {
    for (const auto& item : g_gmiBakedBlocks)
        if (GmiHandleTarget(item.material) == material) return GmiHandleTarget(item.block);
    void* block = CreateMaterialPropertyBlock();
    if (!block) return nullptr;
    g_gmiBakedBlocks.push_back({
        UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", material, false),
        UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", block, false) });
    return block;
}

// Read the current native cloth ramp on each submission, never freeze a scene texture.
bool GmiBindActorRamp(void* renderer, void* block) {
    static auto setTexture = GmiMethod("MaterialPropertyBlock", "SetTexture", { "Int32", "Texture" });
    static auto setFloat = GmiMethod("MaterialPropertyBlock", "SetFloat", { "Int32", "Single" });
    static auto hasProperty = GmiMethod("Material", "HasProperty", { "Int32" });
    const auto materials = reinterpret_cast<UnityArray<void*>*>(GetRendererSharedMaterials(renderer));
    void* source = materials && materials->max_length > 0 ? materials->At(0) : nullptr;
    // Actor light masks index 16 actors, packed as four bytes in each uint.
    // Preserve renderer/per-material MPB precedence when copying the native index.
    static auto getMaterialFloat = GmiMethod("Material", "GetFloat", { "Int32" });
    static auto getBlockFloat = GmiMethod("MaterialPropertyBlock", "GetFloat", { "Int32" });
    static auto hasBlockFloat = GmiMethod("MaterialPropertyBlock", "HasFloat", { "Int32" });
    static auto getRendererBlock = GmiMethod("Renderer", "GetPropertyBlock", { "MaterialPropertyBlock" });
    static auto getSlotBlock = GmiMethod("Renderer", "GetPropertyBlock", { "MaterialPropertyBlock", "Int32" });
    int actorId = GetShaderPropertyId("_ActorIndex");
    void* actorArgs[]{ &actorId };
    float actorIndex = 0;
    bool hasActor = false;
    if (IsNativeObjectAlive(source) && GmiValue(hasProperty, source, actorArgs, hasActor) && hasActor)
        GmiValue(getMaterialFloat, source, actorArgs, actorIndex);
    if (void* scratch = GetPropertyBlockScratch()) {
        void* args[]{ scratch };
        hasActor = false;
        if (GmiCall(getRendererBlock, renderer, args)
            && GmiValue(hasBlockFloat, scratch, actorArgs, hasActor) && hasActor)
            GmiValue(getBlockFloat, scratch, actorArgs, actorIndex);
        int slot = 0;
        void* slotArgs[]{ scratch, &slot };
        hasActor = false;
        if (GmiCall(getSlotBlock, renderer, slotArgs)
            && GmiValue(hasBlockFloat, scratch, actorArgs, hasActor) && hasActor)
            GmiValue(getBlockFloat, scratch, actorArgs, actorIndex);
    }
    int actorTarget = GetShaderPropertyId("_GmiActorIndex");
    void* actorSetArgs[]{ &actorTarget, &actorIndex };
    if (!GmiCall(setFloat, block, actorSetArgs)) return false;
    int rampId = GetShaderPropertyId("_RampMap");
    bool has = false;
    void* hasArgs[]{ &rampId };
    void* ramp = IsNativeObjectAlive(source) && GmiValue(hasProperty, source, hasArgs, has) && has
        ? GetMaterialTexture(source, rampId) : nullptr;
    float ready = IsNativeObjectAlive(ramp) ? 1.0f : 0.0f;
    if (ready > 0) {
        int id = GetShaderPropertyId("_GmiActorRamp");
        void* args[]{ &id, ramp };
        if (!GmiCall(setTexture, block, args)) ready = 0;
    }
    int id = GetShaderPropertyId("_GmiActorRampReady");
    void* args[]{ &id, &ready };
    return GmiCall(setFloat, block, args);
}

// Asset replacement patches a prefab. Its scene clones are different renderers;
// retaining the prefab renderer cannot supply the visible character's current pose.
std::vector<GmiAfterDofDraw> g_gmiLiveDraws;
// 蒙皮数据自洽才烘：sharedMesh 在、bones 数 == bindposes 数。角色刚建时游戏还在换 mesh / 装骨，
// 这两个数会有一瞬不相等，那一瞬 BakeMesh 就是换装页那次原生崩溃最可能的形态。
bool GmiRendererReady(void* renderer) {
    static auto getMesh = GmiMethod("SkinnedMeshRenderer", "get_sharedMesh");
    static auto getBones = GmiMethod("SkinnedMeshRenderer", "get_bones");
    static auto getBindposes = GmiMethod("Mesh", "get_bindposes");
    void* mesh = nullptr; void* bones = nullptr; void* bindposes = nullptr;
    if (!GmiCall(getMesh, renderer, nullptr, &mesh) || !IsNativeObjectAlive(mesh)) return false;
    if (!GmiCall(getBones, renderer, nullptr, &bones) || !bones) return false;
    if (!GmiCall(getBindposes, mesh, nullptr, &bindposes) || !bindposes) return false;
    const auto boneCount = reinterpret_cast<UnityArray<void*>*>(bones)->max_length;
    const auto poseCount = reinterpret_cast<UnityArray<void*>*>(bindposes)->max_length;
    return boneCount > 0 && boneCount == poseCount;
}

void GmiRefreshLiveDraws(int frame) {
    static int lastScan = -60;
    const bool requested = g_gmiRescanRequested.exchange(false);
    if (!requested && frame >= lastScan && frame - lastScan < 20) return;
    lastScan = frame;
    // 换装页实机：角色当场新建（RegisterBones 刚打出来、摇物表刚收集），镜面钩子同一帧就找到它并
    // BakeMesh → 原生崩在 UnityEngine 包装层。刚出现的渲染器先放 GMI_SETTLE_FRAMES 帧再烘，
    // 所以要记住每个渲染器第一次被看到的帧，跨次扫描保留。
    std::unordered_map<void*, int> firstSeen;
    for (const auto& item : g_gmiLiveDraws) {
        if (void* r = GmiHandleTarget(item.renderer)) firstSeen.emplace(r, item.firstSeen);
        UnityResolve::Invoke<void>("il2cpp_gchandle_free", item.renderer);
        UnityResolve::Invoke<void>("il2cpp_gchandle_free", item.material);
    }
    g_gmiLiveDraws.clear();
    static auto klass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer");
    static auto hasProperty = GmiMethod("Material", "HasProperty", { "Int32" });
    static auto getFloat = GmiMethod("Material", "GetFloat", { "Int32" });
    if (!klass) return;
    int id = GetShaderPropertyId("_GmiBakedAfterDof");
    void* args[]{ &id };
    for (void* renderer : klass->FindObjectsByType<void*>()) {
        if (!IsNativeObjectAlive(renderer)) continue;
        const auto materials = reinterpret_cast<UnityArray<void*>*>(GetRendererSharedMaterials(renderer));
        if (!materials) continue;
        for (int slot = 0; slot < materials->max_length; ++slot) {
            void* material = materials->At(slot);
            bool has = false;
            float enabled = 0;
            if (!IsNativeObjectAlive(material)
                || !GmiValue(hasProperty, material, args, has) || !has
                || !GmiValue(getFloat, material, args, enabled) || enabled < 0.5f) continue;
            const auto seen = firstSeen.find(renderer);
            g_gmiLiveDraws.push_back({
                UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", renderer, false),
                UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", material, false), slot,
                seen == firstSeen.end() ? frame : seen->second });
        }
    }
}

void DrawGmiBakedTransparency(void* post, void* cmd, void* source) {
    if (!post || !cmd || !source || !g_currentCamera
        || !g_pipelineMatricesValid || g_afterDofDraws.empty()) return;
    static auto getFrame = GmiMethod("Time", "get_frameCount");
    static auto getFloat = GmiMethod("Material", "GetFloat", { "Int32" });
    static auto findPass = GmiMethod("Material", "FindPass", { "String" });
    static auto getEnabled = GmiMethod("Renderer", "get_enabled");
    static auto getGo = GmiMethod("Component", "get_gameObject");
    static auto getActive = GmiMethod("GameObject", "get_activeInHierarchy");
    static auto getTransform = GmiMethod("Component", "get_transform");
    static auto getWorld = GmiMethod("Transform", "get_localToWorldMatrix");
    static auto gpuProjection = GmiMethod("GL", "GetGPUProjectionMatrix", { "Matrix4x4", "Boolean" });
    static auto setMatrix = GmiMethod("MaterialPropertyBlock", "SetMatrix", { "Int32", "Matrix4x4" });
    static auto setVector = GmiMethod("MaterialPropertyBlock", "SetVector", { "Int32", "Vector4" });
    static auto setTexture = GmiMethod("MaterialPropertyBlock", "SetTexture", { "Int32", "Texture" });
    static auto drawMesh = GmiMethod("CommandBuffer", "DrawMesh",
        { "Mesh", "Matrix4x4", "Material", "Int32", "Int32", "MaterialPropertyBlock" }, "UnityEngine.Rendering");
    static auto setRT = GmiMethod("CoreUtils", "SetRenderTarget",
        { "CommandBuffer", "RTHandle", "ClearFlag", "Int32", "CubemapFace", "Int32" },
        "UnityEngine.Rendering", "Unity.RenderPipelines.Core.Runtime.dll");
    static auto getWidth = GmiMethod("Texture", "get_width");
    static auto getHeight = GmiMethod("Texture", "get_height");
    static auto getFormat = GmiMethod("RenderTexture", "get_graphicsFormat");
    static int lastLoggedFrame = -1000;
    static bool methodsLogged = false;
    if (!methodsLogged) {
        methodsLogged = true;
        Log::InfoFmt("[GmiBaked] API bake=%d drawMesh=%d gpuProjection=%d setMatrix=%d setRT=%d",
            GmiMethod("SkinnedMeshRenderer", "BakeMesh", { "Mesh", "Boolean" }) != nullptr,
            drawMesh != nullptr, gpuProjection != nullptr, setMatrix != nullptr, setRT != nullptr);
        // 崩溃栈只给 GameAssembly 内的绝对地址；这里把我们会调的几个方法的 RVA 打出来，对号用。
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("GameAssembly.dll"));
        auto rva = [base](UnityResolve::Method* method) -> unsigned long long {
            return method && base ? reinterpret_cast<std::uintptr_t>(method->function) - base : 0; };
        Log::InfoFmt("[GmiBaked] RVA bake=%llx drawMesh=%llx execute=%llx findPass=%llx bakeMeshGetSharedMesh=%llx",
            rva(GmiMethod("SkinnedMeshRenderer", "BakeMesh", { "Mesh", "Boolean" })), rva(drawMesh),
            rva(FindMethodExact("UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "ScriptableRenderContext", "ExecuteCommandBuffer", { "CommandBuffer" })),
            rva(findPass), rva(GmiMethod("SkinnedMeshRenderer", "get_sharedMesh")));
    }
    int frame = 0;
    if (!GmiValue(getFrame, nullptr, nullptr, frame)) return;
    GmiRefreshLiveDraws(frame);
    GmiRetireDeadBakedObjects();
    // 2026-09-07 实机定案：遮挡深度取 m_MotionVectors（_AdditionalInfoTexture）的编码深度。
    const std::string depthField = "m_MotionVectors";
    void* depth = GmiPostTexture(post, depthField);
    void* targetTexture = GmiRtTexture(source);
    int width = 0, height = 0;
    if (!targetTexture || !GmiValue(getWidth, targetTexture, nullptr, width)
        || !GmiValue(getHeight, targetTexture, nullptr, height) || width < 1 || height < 1) return;
    const bool diagnostic = false;
    if (!depth && !diagnostic) {
        if (frame - lastLoggedFrame >= 300) {
            lastLoggedFrame = frame;
            Log::ErrorFmt("[GmiBaked] No depth texture %s; skipped rather than drawing through the body.", depthField.c_str());
        }
        return;
    }
    if (frame - lastLoggedFrame >= 300) {
        for (const char* field : { "m_Depth", "m_MotionVectors" }) {
            auto tex = GmiPostTexture(post, field);
            int w = 0, h = 0, format = 0;
            if (tex) { GmiValue(getWidth, tex, nullptr, w); GmiValue(getHeight, tex, nullptr, h); GmiValue(getFormat, tex, nullptr, format); }
            Log::InfoFmt("[GmiBaked] %s=%p name=%s size=%dx%d graphicsFormat=%d",
                field, tex, tex ? GetUnityObjectNameString(tex).c_str() : "null", w, h, format);
        }
    }
    GmiMatrix view{}, projection{}, gpu{};
    std::memcpy(view.data(), g_pipelineView, sizeof(view));
    std::memcpy(projection.data(), g_pipelineProj, sizeof(projection));
    bool renderIntoTexture = true;
    void* gpuArgs[]{ projection.data(), &renderIntoTexture };
    if (!GmiValue(gpuProjection, nullptr, gpuArgs, gpu)) return;
    const auto vp = GmiMultiply(gpu, view);
    int clear = 0, mip = 0, face = -1, slice = -1;
    void* rtArgs[]{ cmd, source, &clear, &mip, &face, &slice };
    if (!GmiCall(setRT, nullptr, rtArgs)) return;
    const GmiMatrix identity{ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    const auto start = std::chrono::steady_clock::now();
    int count = 0;
    const bool logFrame = frame - lastLoggedFrame >= 300;
    auto skipped = [&](const GmiAfterDofDraw& item, const char* reason) {
        if (logFrame) Log::InfoFmt("[GmiBaked] skip renderer=%p slot=%d reason=%s",
            GmiHandleTarget(item.renderer), item.submesh, reason);
    };
    for (const auto& item : g_gmiLiveDraws) {
        void* renderer = GmiHandleTarget(item.renderer);
        void* material = GmiHandleTarget(item.material);
        if (!IsNativeObjectAlive(renderer) || !IsNativeObjectAlive(material)) { skipped(item, "destroyed"); continue; }
        if (frame - item.firstSeen < GMI_SETTLE_FRAMES) { skipped(item, "settling"); continue; }
        if (!GmiRendererReady(renderer)) { skipped(item, "skin-not-ready"); continue; }
        int enabledId = GetShaderPropertyId("_GmiBakedAfterDof");
        void* floatArgs[]{ &enabledId };
        float enabled = 0;
        if (!GmiValue(getFloat, material, floatArgs, enabled) || enabled < 0.5f) { skipped(item, "material-disabled"); continue; }
        bool rendererEnabled = false, active = false;
        void* go = nullptr;
        if (!GmiValue(getEnabled, renderer, nullptr, rendererEnabled) || !rendererEnabled
            || !GmiCall(getGo, renderer, nullptr, &go) || !go
            || !GmiValue(getActive, go, nullptr, active) || !active) { skipped(item, "renderer-inactive"); continue; }
        // Prefab entries, disabled mods, and retired material instances must not draw.
        const auto materials = reinterpret_cast<UnityArray<void*>*>(GetRendererSharedMaterials(renderer));
        if (!materials || item.submesh < 0 || item.submesh >= materials->max_length
            || materials->At(item.submesh) != material) { skipped(item, "material-replaced"); continue; }
        void* transform = nullptr;
        GmiMatrix world{};
        if (!GmiCall(getTransform, renderer, nullptr, &transform) || !transform
            || !GmiValue(getWorld, transform, nullptr, world)) { skipped(item, "transform"); continue; }
        void* mesh = GmiBake(renderer, frame);
        void* block = GmiBlock(material);
        if (!mesh || !block) { skipped(item, "bake-or-property-block"); continue; }
        auto mvp = GmiMultiply(vp, world);
        auto mv = GmiMultiply(view, world);
        auto matrix = [&](const char* name, GmiMatrix& value) {
            int id = GetShaderPropertyId(name); void* args[]{ &id, value.data() };
            return GmiCall(setMatrix, block, args);
        };
        auto vector = [&](const char* name, std::array<float, 4> value) {
            int id = GetShaderPropertyId(name); void* args[]{ &id, value.data() };
            return GmiCall(setVector, block, args);
        };
        if (!matrix("_GmiMVP", mvp) || !matrix("_GmiMV", mv) || !matrix("_GmiModel", world)
            || !matrix("_GmiLightingView", view) || !GmiBindActorRamp(renderer, block)) continue;
        if (!vector("_GmiViewport", { float(width), float(height), 0.0f, diagnostic ? 1.0f : 0.0f })
            || !vector("_GmiDepthProjection", { gpu[10], gpu[14], gpu[11], gpu[15] })
            || !vector("_GmiDepthSettings", { 1.0f /*encoded*/, 0.005f /*epsilon*/, depth ? 1.0f : 0.0f, 0.0f })) continue;
        if (depth) {
            int id = GetShaderPropertyId("_GmiSceneDepth"); void* args[]{ &id, depth };
            if (!GmiCall(setTexture, block, args)) continue;
        }
        auto name = Il2cppString::New("GmiBakedAfterDof"); void* passArgs[]{ name };
        int pass = -1;
        if (!GmiValue(findPass, material, passArgs, pass) || pass < 0) { skipped(item, "missing-pass"); continue; }
        int submesh = item.submesh;
        void* drawArgs[]{ mesh, const_cast<float*>(identity.data()), material, &submesh, &pass, block };
        if (GmiCall(drawMesh, cmd, drawArgs)) ++count;
    }
    if (frame - lastLoggedFrame >= 300) {
        lastLoggedFrame = frame;
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        Log::InfoFmt("[GmiBaked] frame=%d camera=%s draws=%d cpuBakeAndSubmitMs=%.3f depth=%s target=%dx%d diagnostic=%d",
            frame, GetUnityObjectNameString(g_currentCamera).c_str(), count, ms, depthField.c_str(), width, height, diagnostic);
        Log::InfoFmt("[GmiBaked] registeredPrefabSlots=%zu liveSceneSlots=%zu", g_afterDofDraws.size(), g_gmiLiveDraws.size());
    }
}
