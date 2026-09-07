// 镜面补绘。两处证据定的性，别再按「反射相机」那套猜：
//
// 抓帧 FrameAnalysis-2026-09-07-151234：1081x1920 的 378bc99e 是一张**图集**，上下各一格
// 反射视角，draws 47..172 全画在里面 —— 镜子是真的重画几何，不是屏幕空间贴图。mod 的不透明
// submesh（71763 索引）在里面，长纱裙不在，因为它除 baked 外所有 pass 都被关了。对比 draw
// 149 与主画面 draw 199：per-draw 常量完全一样，只有那段 VP 不同且第一行整行变号 = 镜像。
//
// il2cpp（iOS 3.3.0 UnityFramework，VL.Rendering.PlanarReflectionUtility.RenderPlanarReflection
// @0x0A2C3BA8）每面镜子跑一次，顺序是：
//   cmd: 画 stencil/mask 盒 → SetGlobalVector/Matrix → SetInvertCulling
//        → SetViewProjectionMatrices(反射 V、斜投影 P)
//   context.Cull → context.ExecuteCommandBuffer(cmd) → cmd.Clear()
//   context.DrawRenderers ×2~3（基础 tag、颜色 tag、每面镜子自带的额外 tag）
//   cmd: 恢复剔除 → 恢复主相机 VP → 画 resetPass   ← 只排进 cmd，要等下一次执行才生效
//
// 所以**原函数返回的这一刻**，GPU 上仍然是反射的 VP、反转的剔除、图集那张带深度的 RT：
// 自带一条 CommandBuffer 立刻 execute 就落在镜像里，矩阵一个都不用我们算。
// 也因此这条路不需要主画面那套编码深度采样 —— 这里有真正的深度附件，ZTest 就够。
using GmiRenderPlanarFn = void (*)(void*, void*, void*, void*, int, void*, int, int, int, void*, void*);
GmiRenderPlanarFn g_gmiRenderPlanarOrig{};

void GmiDrawIntoReflection(void** contextPtr, int mirror) {
    if (!contextPtr) return;
    static auto getFrame = GmiMethod("Time", "get_frameCount");
    static auto getEnabled = GmiMethod("Renderer", "get_enabled");
    static auto getGo = GmiMethod("Component", "get_gameObject");
    static auto getActive = GmiMethod("GameObject", "get_activeInHierarchy");
    static auto getTransform = GmiMethod("Component", "get_transform");
    static auto getWorld = GmiMethod("Transform", "get_localToWorldMatrix");
    static auto findPass = GmiMethod("Material", "FindPass", { "String" });
    static auto drawMesh = GmiMethod("CommandBuffer", "DrawMesh",
        { "Mesh", "Matrix4x4", "Material", "Int32", "Int32", "MaterialPropertyBlock" }, "UnityEngine.Rendering");
    // ScriptableRenderContext 就是一个 IntPtr，按值进寄存器；结构体实例方法要的 this 是
    // 「指向这个结构体的指针」，所以交出去的是钩子那个参数的地址。
    static const auto execute = [] {
        const auto m = FindMethodExact("UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
            "ScriptableRenderContext", "ExecuteCommandBuffer", { "CommandBuffer" });
        if (!m) Log::Error("[GmiReflection] ScriptableRenderContext.ExecuteCommandBuffer 没找到，镜面补绘不启用");
        return m ? reinterpret_cast<void (*)(void**, void*)>(m->function) : nullptr;
    }();
    if (!execute || !drawMesh || !findPass) return;
    int frame = 0;
    if (!GmiValue(getFrame, nullptr, nullptr, frame)) return;
    GmiRefreshLiveDraws(frame);          // 与主画面共用同一份清单，同帧谁先跑到都行
    if (g_gmiLiveDraws.empty()) return;
    void* cmd = AcquireScratchCommandBuffer();
    if (!cmd) return;
    int count = 0;
    for (const auto& item : g_gmiLiveDraws) {
        void* renderer = GmiHandleTarget(item.renderer);
        void* material = GmiHandleTarget(item.material);
        if (!IsNativeObjectAlive(renderer) || !IsNativeObjectAlive(material)) continue;
        if (frame - item.firstSeen < GMI_SETTLE_FRAMES) continue;      // 刚建出来的角色先别烘
        bool enabled = false, active = false;
        void* go = nullptr;
        if (!GmiValue(getEnabled, renderer, nullptr, enabled) || !enabled
            || !GmiCall(getGo, renderer, nullptr, &go) || !go
            || !GmiValue(getActive, go, nullptr, active) || !active) continue;
        // 预制体条目、已停用的 mod、被换掉的材质实例都不该画。
        const auto materials = reinterpret_cast<UnityArray<void*>*>(GetRendererSharedMaterials(renderer));
        if (!materials || item.submesh < 0 || item.submesh >= materials->max_length
            || materials->At(item.submesh) != material) continue;
        void* transform = nullptr;
        GmiMatrix world{};
        if (!GmiCall(getTransform, renderer, nullptr, &transform) || !transform
            || !GmiValue(getWorld, transform, nullptr, world)) continue;
        // 烘出来的顶点在渲染器自己的局部空间里，所以世界矩阵直接当 DrawMesh 的矩阵用，
        // shader 那边走常规 UnityObjectToClipPos，吃的就是当前这套反射 VP。
        void* mesh = GmiBake(renderer, frame);
        if (!mesh) continue;
        void* block = GmiBlock(material);
        if (!block || !GmiBindActorRamp(renderer, block)) continue;
        void* passArgs[]{ Il2cppString::New("GmiBakedReflection") };
        int pass = -1;
        if (!GmiValue(findPass, material, passArgs, pass) || pass < 0) continue;
        int submesh = item.submesh;
        void* drawArgs[]{ mesh, world.data(), material, &submesh, &pass, block };
        if (GmiCall(drawMesh, cmd, drawArgs)) ++count;
    }
    if (!count) return;
    execute(contextPtr, cmd);
    static int lastLogged = -1000;
    if (frame - lastLogged >= 300) {
        lastLogged = frame;
        Log::InfoFmt("[GmiReflection] frame=%d mirror=%d draws=%d liveSlots=%zu",
            frame, mirror, count, g_gmiLiveDraws.size());
    }
}

void GmiRenderPlanarReflectionHook(void* cmd, void* context, void* renderingData, void* entityManager,
        int index, void* material, int stencilPass, int maskPass, int resetPass,
        void* colorRenderState, void* method) {
    g_gmiRenderPlanarOrig(cmd, context, renderingData, entityManager, index, material,
        stencilPass, maskPass, resetPass, colorRenderState, method);
    GmiDrawIntoReflection(&context, index);
}
