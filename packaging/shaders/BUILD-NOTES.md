# Shader bundle build

When creating a minimal Unity project, use the accompanying `Packages/manifest.json`
as the project's package manifest. The AssetBundle engine module must be enabled.
Place GmiBuild.cs under Assets/Editor and GmiTransparent.shader under Assets/Gmi.

The first hmsz baked transparency trial used an empty dependency list. Unity
6000.0.67f1 reported a successful build but also reported that AssetBundle and
AssetBundleManifest were unsupported because the AssetBundle module was disabled.
The resulting file contained a Shader only, no AssetBundle object or container.
The game refused to load it with a generic newer-runtime compatibility message.
The mesh had four submeshes but only two material slots after shader loading failed,
so both fabric submeshes were invisible.

Enabling com.unity.modules.assetbundle and rebuilding with the SAME 6000.0.67f1
produced an AssetBundle object and an addressable shader. Actual Unity loading
succeeded, with one asset and six material passes. This establishes the malformed
bundle defect; it does not yet establish in-game pose, depth or transparency quality,
nor prove that every 67/77 asset type is interchangeable.

Do not diagnose an editor-version mismatch from the generic game error alone.
GmiBuild now actually loads its output and resolves the shader before reporting success.
