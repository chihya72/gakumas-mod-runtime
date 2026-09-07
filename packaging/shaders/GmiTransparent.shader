// 学马半透明 shader —— 走中间件原生的「角色半透明」通路。
//
// 2026-08-18 实机确认：VL 中间件带一个 VLActorTransparentPass（学马没启用，runtime 把它
// 挂进渲染器后 Execute 正常触发）。它自带全套规矩：
//   筛选队列 [0, 2500]，layerMask/renderingLayerMask 全开
//   RenderPassEvent = 400（天空盒之后、普通透明之前）→ 混合时场景色已经完整
//   Execute 里三趟 DrawRenderers：前置 → 颜色 → 描边
// tag 由 ShaderTagId 反查确认（id 85~89 是这个 pass 独占的，全游戏别处不用）：
//   85 VLActorTransparentZPrePass   86 VLActorTransparent
//   87 VLActorCoverZPrePassTransparent   88 VLActorCoverTransparent
//
// 用这两个 tag 的好处：不会被任何其它 pass 误收（之前撞过的白块、多趟同画全部消失）。
//
// 不 include URP 的 ShaderLibrary：游戏用的是 fork 过的 URP，include 版本对不上就是粉屏。
// 只用引擎自带的 UnityCG.cginc；蒙皮由引擎在 VS 之前完成。
//
// toon 用本项目实测过的贴图语义：t1.r=阴影阈值 t1.a=AO；t4.rgb=暗面色 t4.a=分支 mask。
Shader "Gmi/Transparent"
{
    Properties
    {
        _BaseMap ("Base Map (t0)", 2D) = "white" {}
        _DefMap ("Packed Mask (t1)", 2D) = "black" {}
        _ShadeMap ("Shade Color (t4)", 2D) = "black" {}
        _BaseColor ("Tint", Color) = (1,1,1,1)
        _Alpha ("Alpha", Range(0,1)) = 0.5
        _GmiBakedAfterDof ("Opt into explicit baked transparency", Float) = 0
        _AlphaFromTexture ("Use t0 alpha", Float) = 1
        _Cutoff ("Discard below", Range(0,1)) = 0.004
        _DepthCutoff ("ZPrePass writes above", Range(0,1)) = 0.1
        _ToonStrength ("Toon strength", Range(0,1)) = 1
        _ShadeDarken ("Cloth darken", Range(0,1)) = 0.45
        _ToonSoftness ("Toon edge softness", Range(0.001,0.5)) = 0.08
        _AoStrength ("AO strength", Range(0,1)) = 0.5
        _Cull ("Cull", Float) = 0        // 0=Off 1=Front 2=Back
        _ZWriteMode ("ZWrite (color pass)", Float) = 0
        _ZPrePassEnable ("Draw native Z pre-pass", Float) = 1
        _ActorTransparentEnable ("Draw native actor transparent", Float) = 1
        _ForwardEnable ("Fallback: forward transparent stage", Float) = 0
        _SelfBlendEnable ("Self-blend in actor MRT pass", Float) = 0
        _DepthClaimEnable ("Claim depth in actor MRT pass", Float) = 0
        _SceneColorDebug ("Probe: output sampled scene color", Float) = 0
        _MaterialId ("GBuffer material id (RT0.a)", Float) = 256
        // 抓帧实测：角色 MRT 段结束前有一趟全屏 resolve（ps 24c7e9bfbe2d4b81，StencilRef 64），
        // 把真实 DSV 的深度重新编码进 RT0.B。它按 stencil 跳过角色像素；我们不写 stencil，
        // 于是「压背景」那半边的深度认领被它整片刷掉（实测存活率 0.0%，压角色那半边 98.4%）。
        // 写上和原版角色一样的 stencil 64，这趟就会绕开我们。
        _StencilRef ("Actor stencil ref", Float) = 64
        _StencilWriteMask ("Actor stencil write mask", Float) = 64
        // 场景染色（只作用于两条 baked 通路）。默认全开，作者可在 mod JSON 里关掉或调曝光。
        _GmiSceneTint ("Scene light tint", Range(0,1)) = 1
        _GmiExposure ("Baked exposure", Range(0,2)) = 1
        _GmiToonThreshold ("Vanilla toon threshold", Range(0,1)) = 0.3
        _GmiToonFloor ("Vanilla toon floor", Range(0,1)) = 0.745
    }

    SubShader
    {
        Tags
        {
            "RenderPipeline" = "UniversalPipeline"
            "RenderType" = "Transparent"
            "Queue" = "Geometry+400"     // 落在 [0,2500] 里，由 VLActorTransparentPass 收
            "IgnoreProjector" = "True"
        }

        CGINCLUDE
        #include "UnityCG.cginc"

        sampler2D _BaseMap;
        sampler2D _DefMap;
        sampler2D _ShadeMap;
        float4 _BaseMap_ST;
        fixed4 _BaseColor;
        float _Alpha;
        float _AlphaFromTexture;
        float _Cutoff;
        float _DepthCutoff;
        float _ToonStrength;
        float _ShadeDarken;
        float _ToonSoftness;
        float _AoStrength;
        float _ZPrePassEnable;
        float _ActorTransparentEnable;
        float _ForwardEnable;
        float _SelfBlendEnable;
        float _DepthClaimEnable;
        float _SceneColorDebug;
        float _MaterialId;
        sampler2D _CameraOpaqueTexture;
        float4 _MainLightPosition;
        half4 _MainLightColor;

        struct appdata { float4 vertex : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
        struct v2f { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float3 worldNormal : TEXCOORD1;
                     float3 worldPos : TEXCOORD2; };

        v2f vertCommon(appdata v)
        {
            v2f o;
            o.pos = UnityObjectToClipPos(v.vertex);
            o.uv = TRANSFORM_TEX(v.uv, _BaseMap);
            o.worldNormal = UnityObjectToWorldNormal(v.normal);
            o.worldPos = mul(unity_ObjectToWorld, v.vertex).xyz;
            return o;
        }

        float SampleAlpha(float2 uv)
        {
            return lerp(1.0, tex2D(_BaseMap, uv).a, _AlphaFromTexture) * _Alpha;
        }

        // Actor cloth lighting prototype, based on body PS f872756c910a6eb7.
        // The previous luminance-normalized light tint missed scene shadow color.
        // Global names are present in PC metadata; their register mapping still
        // needs a live capture comparison. No SH, shadow-map or specular parity yet.
        // Campus.Rendering.Shaders.ShaderVariablesActorLighting, exact 528-byte
        // layout from ActorCBuffer.cs metadata, confirmed against native PS cb2.
        // URP _AdditionalLights* is a DIFFERENT light list (185607 capture).
        CBUFFER_START(ShaderVariablesActorLighting)
            uint4 _LightData;
            float4 _LightPositions[8];
            float4 _LightColors[8];
            float4 _LightAttenuations[8];
            float4 _LightDirections[8];
        CBUFFER_END
        float _GmiActorIndex;
        float _GmiSceneTint, _GmiExposure, _GmiToonThreshold, _GmiToonFloor;

        // Names exist in the PC metadata and CampusActorParameterPass. These are
        // scene globals, deliberately not material Properties (which would shadow them).
        float4 _MatCapMainLight, _MatCapParam, _ShadeMultiplyColor, _GlobalLightParameter;
        float4 _MatCapLightColor, _ShadeAdditiveColor;
        sampler2D _GmiActorRamp;
        float _GmiActorRampReady;
        float4x4 _GmiLightingView;

        float3 GmiActorAdditionalLight(float3 worldPos, float3 n)
        {
            float3 sum = 0;
            uint actor = (uint)clamp(_GmiActorIndex, 0, 15);
            uint mask = (_LightData[actor >> 2] >> ((actor & 3) * 8)) & 255;
            [loop] for (int i = 0; i < 8; ++i)
            {
                if ((mask & (1u << i)) == 0) continue;
                float3 toLight = _LightPositions[i].xyz - worldPos * _LightPositions[i].w;
                float distSqr = max(dot(toLight, toLight), 6.103515625e-05);
                float3 dir = toLight * rsqrt(distSqr);
                float spot = saturate(dot(_LightDirections[i].xyz, dir)
                    * _LightAttenuations[i].z + _LightAttenuations[i].w);
                float atten = spot * spot;
                if (_LightDirections[i].w != 1)      // 平行光那支不吃距离衰减
                {
                    float f = _LightAttenuations[i].x * distSqr;
                    f = max(0, 1 - f * f); f *= f;
                    atten *= f / distSqr;
                    float h = saturate(dot(n, dir)) * 0.5 + 0.5; h *= h;
                    float t = saturate(1024.17041 * (h * 2.3561945 - (_MatCapParam.x - 0.0004883)));
                    atten *= saturate((3 - 2 * t) * t * t + _MatCapParam.z);
                }
                sum += _LightColors[i].rgb * atten;
            }
            return sum * _GlobalLightParameter.y;
        }

        // Shared by main and mirror. Preserve actual HDR intensity and alpha;
        // do not normalize the stage light into an arbitrary tint.
        float3 GmiActorCloth(float2 uv, float3 worldPos, float3 normal, float3 viewNormal)
        {
            float3 base = tex2D(_BaseMap, uv).rgb * _BaseColor.rgb;
            float4 def = tex2D(_DefMap, uv);
            float4 shade = tex2D(_ShadeMap, uv);
            float3 lightNormal = _MatCapMainLight.w > 0.5 ? normal : viewNormal;
            float rampU = saturate(dot(lightNormal, _MatCapMainLight.xyz) * 0.5 + 0.5
                - 0.5 * (_MatCapParam.x - (def.r * 2 - 1)));
            float4 ramp = tex2D(_GmiActorRamp, float2(rampU, 0));
            if (_GmiActorRampReady < 0.5) ramp = float4(1,1,1,1-rampU);
            // Body PS: lerp(base, shade * sceneShadow, sceneWeight * ramp.a),
            // followed by its shade-alpha branch for the multiplicative ramp.
            float weight = saturate(_MatCapParam.z);
            float3 shaded = lerp(base, shade.rgb * _ShadeMultiplyColor.rgb, weight * ramp.a);
            float3 rampTint = lerp(ramp.rgb, ramp.rgb * _ShadeMultiplyColor.rgb, ramp.a);
            float3 color = lerp(shaded, base * lerp(1.0, rampTint, weight), shade.a);
            color = color * (_MatCapLightColor.rgb + GmiActorAdditionalLight(worldPos, normal))
                + _ShadeAdditiveColor.rgb * ramp.a;
            return lerp(base, color, _GmiSceneTint) * _GmiExposure;
        }

        // 顶点色故意不采样：mod 网格的 COLOR 是描边参数（布料预设 (0,0,255,0)），
        // 乘进来会把飘带染蓝并把 alpha 干掉。
        fixed4 ShadeToon(v2f i, fixed facing)
        {
            fixed4 base = tex2D(_BaseMap, i.uv) * _BaseColor;
            fixed4 packed = tex2D(_DefMap, i.uv);
            fixed4 shade = tex2D(_ShadeMap, i.uv);

            // 双面片：背面法线要翻，否则背面永远算成暗面
            float3 n = normalize(i.worldNormal) * (facing >= 0 ? 1.0 : -1.0);
            float3 lightDir = _MainLightPosition.xyz;
            float3 lightColor = _MainLightColor.rgb;
            if (dot(lightDir, lightDir) < 1e-6) lightDir = float3(0.35, 0.75, -0.56);
            if (dot(lightColor, lightColor) < 1e-6) lightColor = float3(1, 1, 1);
            lightDir = normalize(lightDir);

            float threshold = lerp(0.35, packed.r, step(0.001, packed.r));
            float ndl = saturate(dot(n, lightDir) * 0.5 + 0.5);
            float lit = smoothstep(threshold - _ToonSoftness, threshold + _ToonSoftness, ndl);
            float3 dark = lerp(base.rgb * _ShadeDarken, shade.rgb, saturate(shade.a));
            float3 col = lerp(dark, base.rgb, lit);
            col = lerp(base.rgb, col, _ToonStrength);
            col *= lerp(1.0, 1.0 - packed.a, _AoStrength);
            col *= lightColor;
            return fixed4(col, SampleAlpha(i.uv));
        }
        ENDCG

        // ① 前置：中间件在颜色之前先跑这一趟（id 85）。只写深度、不写颜色。
        Pass
        {
            Name "GmiActorTransparentZPrePass"
            Tags { "LightMode" = "VLActorTransparentZPrePass" }

            ZWrite On
            ZTest LEqual
            ColorMask 0
            Cull [_Cull]

            CGPROGRAM
            #pragma vertex vertZ
            #pragma fragment fragZ
            v2f vertZ(appdata v) { return vertCommon(v); }
            float4 fragZ(v2f i) : SV_Target
            {
                clip(_ZPrePassEnable - 0.5);
                clip(SampleAlpha(i.uv) - _DepthCutoff);   // 镂空处不写深度
                return 0;
            }
            ENDCG
        }

        // ② 颜色：真 alpha 混合。此时天空盒已画完，底下是完整场景（id 86）。
        Pass
        {
            Name "GmiActorTransparent"
            Tags { "LightMode" = "VLActorTransparent" }

            Blend SrcAlpha OneMinusSrcAlpha
            ZWrite [_ZWriteMode]
            ZTest LEqual
            Cull [_Cull]

            CGPROGRAM
            #pragma vertex vertA
            #pragma fragment fragA
            v2f vertA(appdata v) { return vertCommon(v); }
            fixed4 fragA(v2f i, fixed facing : VFACE) : SV_Target
            {
                clip(_ActorTransparentEnable - 0.5);
                clip(SampleAlpha(i.uv) - _Cutoff);
                return ShadeToon(i, facing);
            }
            ENDCG
        }

        // ③ 退路：原来那条前向透明（队列 3000 时用），默认关。
        Pass
        {
            Name "GmiTransparentForward"
            Tags { "LightMode" = "UniversalForwardOnly" }

            Blend SrcAlpha OneMinusSrcAlpha
            ZWrite [_ZWriteMode]
            ZTest LEqual
            Cull [_Cull]

            CGPROGRAM
            #pragma vertex vertF
            #pragma fragment fragF
            v2f vertF(appdata v) { return vertCommon(v); }
            fixed4 fragF(v2f i, fixed facing : VFACE) : SV_Target
            {
                clip(_ForwardEnable - 0.5);
                clip(SampleAlpha(i.uv) - _Cutoff);
                return ShadeToon(i, facing);
            }
            ENDCG
        }


        // ④ 中间件自带的「G-buffer 阶段半透明角色件」通路（VLActorGBuffer.ExecuteTransparent）。
        //    这条 pass 是给它准备的：runtime 把 _forwardTagId 改写成下面这个自定义 tag，
        //    就不必知道原版那个 id:60 到底叫什么名字（ShaderTagId.get_name 被裁了，反查不到）。
        //    队列要落进 GBufferTransparentRange = [2501, 2700]，这一版特意让出来的那 200 格。
        //    此刻 RT 还绑着角色 MRT：RT0 写全（含材质 ID），RT1 写颜色 + alpha 由中间件合成。
        Pass
        {
            Name "GmiGBufferTransparent"
            Tags { "LightMode" = "GmiGBufferTransparent" }

            // 实测：这趟的 SV_Target0 就是颜色缓冲（按 MRT 写两路，rt0 的 16376·√z 直接
            // 变成爆亮纯蓝糊在画面上）。所以它是前向 pass，单目标。
            Blend SrcAlpha OneMinusSrcAlpha
            ZWrite [_ZWriteMode]
            ZTest LEqual
            Cull [_Cull]

            CGPROGRAM
            #pragma vertex vertGT
            #pragma fragment fragGT
            v2f vertGT(appdata v) { return vertCommon(v); }

            fixed4 fragGT(v2f i, fixed facing : VFACE) : SV_Target
            {
                clip(SampleAlpha(i.uv) - _Cutoff);
                return ShadeToon(i, facing);
            }
            ENDCG
        }


        // ---- 只认领深度：颜色交给 VL 那趟，这里只把飘带自己的深度写进景深读的那张图 ------
        //
        // 分工的理由（都由抓帧坐实）：
        //   * VLActorTransparentPass 画在颜色阶段，混合正确，但那个时机**没有深度附件**
        //     （dsv=NULL，试过 ConfigureTarget、useNativeRenderPass、renderPassEvent 250~450 都改不动）；
        //   * 角色 MRT 那一趟（tag=UniversalGBufferActor，队列 ≤2500）深度绑着，RT0 就是
        //     景深唯一读的那张（编码 16376·√z）。
        // 所以同一个材质挂两个 tag，两趟各取所需，不用复制几何。
        //
        // 只写 RT0 的 B 通道：.w 是材质 ID，一旦认领成角色，合成会把（我们没写的）角色颜色
        // 缓冲贴上来 —— 那就是之前那片白。.xy 是运动矢量，同样不碰。
        Pass
        {
            Name "GmiActorDepthClaim"
            Tags { "LightMode" = "UniversalGBufferActor" }

            Blend Off
            ColorMask B 0
            ColorMask 0 1
            ZWrite On
            ZTest LEqual
            Cull [_Cull]
            Stencil
            {
                Ref [_StencilRef]
                WriteMask [_StencilWriteMask]
                Comp Always
                Pass Replace
            }

            CGPROGRAM
            #pragma vertex vertDC
            #pragma fragment fragDC
            struct v2fDC { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
            struct fragOutDC { float4 rt0 : SV_Target0; float4 rt1 : SV_Target1; };

            v2fDC vertDC(appdata v)
            {
                v2fDC o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv = TRANSFORM_TEX(v.uv, _BaseMap);
                return o;
            }

            fragOutDC fragDC(v2fDC i)
            {
                clip(_DepthClaimEnable - 0.5);
                clip(SampleAlpha(i.uv) - _DepthCutoff);   // 镂空处不认领
                fragOutDC o;
                o.rt0 = float4(0.0, 0.0, 16376.0 * sqrt(saturate(i.pos.z)), 0.0);
                o.rt1 = 0;                                 // 被 ColorMask 屏蔽，写什么都不进去
                return o;
            }
            ENDCG
        }
        // Selected explicitly by Material.FindPass, never by the game's renderer lists.
        // CPU-baked vertices and private matrices avoid stale SRP object/skinning state.
        Pass
        {
            Name "GmiBakedAfterDof"
            Tags { "LightMode" = "GmiBakedAfterDof" }
            Blend One OneMinusSrcAlpha
            ZWrite Off
            ZTest Always
            Cull [_Cull]
            CGPROGRAM
            #pragma target 3.5
            #pragma vertex vertBaked
            #pragma fragment fragBaked
            float4x4 _GmiMVP, _GmiMV, _GmiModel;
            float4 _GmiViewport, _GmiDepthProjection, _GmiDepthSettings;
            sampler2D _GmiSceneDepth;
            struct bakedVaryings {
                float4 position : SV_POSITION;
                float2 uv : TEXCOORD0;
                float eyeDepth : TEXCOORD1;
                float3 normal : TEXCOORD2;
                float3 worldPos : TEXCOORD3;
            };
            bakedVaryings vertBaked(appdata v) {
                bakedVaryings o;
                o.position = mul(_GmiMVP, v.vertex);
                o.eyeDepth = -mul(_GmiMV, v.vertex).z;
                o.uv = TRANSFORM_TEX(v.uv, _BaseMap);
                // The hmsz prototype uses uniform object scale.
                o.normal = normalize(mul((float3x3)_GmiModel, v.normal));
                o.worldPos = mul(_GmiModel, v.vertex).xyz;
                return o;
            }
            float4 fragBaked(bakedVaryings i, float facing : VFACE) : SV_Target {
                if (_GmiViewport.w > 0.5) return float4(1,0,1,1);
                float alpha = SampleAlpha(i.uv);
                clip(alpha - _Cutoff);
                float2 uv = i.position.xy / _GmiViewport.xy;
                if (_GmiViewport.z > 0.5) uv.y = 1 - uv.y;
                if (_GmiDepthSettings.z > 0.5) {
                    float4 sampleDepth = tex2Dlod(_GmiSceneDepth, float4(uv,0,0));
                    float z = sampleDepth.r;
                    if (_GmiDepthSettings.x > 0.5) {
                        z = sampleDepth.b / 16376.0;
                        z *= z;
                    }
                    float denominator = z * _GmiDepthProjection.z - _GmiDepthProjection.x;
                    float sceneEye = abs((_GmiDepthProjection.y - z * _GmiDepthProjection.w) /
                        (abs(denominator) > 1e-8 ? denominator : 1e-8));
                    clip(sceneEye + _GmiDepthSettings.y - i.eyeDepth);
                }
                float3 normal = normalize(i.normal) * (facing >= 0 ? 1 : -1);
                float3 color = GmiActorCloth(i.uv, i.worldPos, normal,
                    normalize(mul((float3x3)_GmiLightingView, normal)));
                return float4(color * alpha, alpha);
            }
            ENDCG
        }

        // 镜面补绘。同样由 FindPass 显式选中，不进任何 draw list。
        // 运行时把这一笔接在 PlanarReflectionUtility.RenderPlanarReflection 返回之后，
        // 那时 GPU 上还是反射的 VP、反转的剔除、图集那张**带深度附件**的 RT：
        // 所以顶点走常规变换、遮挡交给 ZTest，不需要后景深那套编码深度采样。
        // 与 GmiBakedAfterDof 共用 GmiActorCloth，使用当前镜面视图。
        Pass
        {
            Name "GmiBakedReflection"
            Tags { "LightMode" = "GmiBakedReflection" }
            Blend One OneMinusSrcAlpha
            ZWrite Off
            ZTest LEqual
            Cull [_Cull]
            CGPROGRAM
            #pragma target 3.5
            #pragma vertex vertReflect
            #pragma fragment fragReflect
            v2f vertReflect(appdata v) { return vertCommon(v); }
            float4 fragReflect(v2f i, float facing : VFACE) : SV_Target {
                float alpha = SampleAlpha(i.uv);
                clip(alpha - _Cutoff);
                float3 normal = normalize(i.worldNormal) * (facing >= 0 ? 1 : -1);
                float3 color = GmiActorCloth(i.uv, i.worldPos, normal,
                    normalize(mul((float3x3)UNITY_MATRIX_V, normal)));
                return float4(color * alpha, alpha);
            }
            ENDCG
        }
    }

    Fallback Off
}
