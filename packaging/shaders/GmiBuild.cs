using System.IO;
using UnityEditor;
using UnityEngine;

public static class GmiBuild
{
    // Unity.exe -batchmode -quit -nographics -projectPath <proj> -executeMethod GmiBuild.Build
    public static void Build()
    {
        const string shaderPath = "Assets/Gmi/GmiTransparent.shader";
        var shader = AssetDatabase.LoadAssetAtPath<Shader>(shaderPath);
        if (shader == null)
        {
            Debug.LogError("GMI_BUILD_FAIL shader not found: " + shaderPath);
            EditorApplication.Exit(2);
            return;
        }
        Debug.Log("GMI_SHADER name=" + shader.name + " isSupported=" + shader.isSupported);

        var importer = AssetImporter.GetAtPath(shaderPath);
        importer.assetBundleName = "gmi_shaders";
        importer.SaveAndReimport();
        AssetDatabase.SaveAssets();

        var outDir = Path.Combine(Directory.GetCurrentDirectory(), "out");
        Directory.CreateDirectory(outDir);
        var manifest = BuildPipeline.BuildAssetBundles(
            outDir, BuildAssetBundleOptions.None, BuildTarget.StandaloneWindows64);
        if (manifest == null)
        {
            Debug.LogError("GMI_BUILD_FAIL BuildAssetBundles returned null");
            EditorApplication.Exit(3);
            return;
        }
        var bundle = Path.Combine(outDir, "gmi_shaders");
        // Building can report success even when the AssetBundle module is disabled.
        // In that case the file contains a Shader but no AssetBundle/container.
        var loaded = AssetBundle.LoadFromFile(bundle);
        if (loaded == null || loaded.LoadAsset<Shader>(shaderPath) == null)
        {
            Debug.LogError("GMI_BUILD_FAIL bundle cannot load its shader; ensure com.unity.modules.assetbundle is enabled");
            if (loaded != null) loaded.Unload(true);
            EditorApplication.Exit(4);
            return;
        }
        loaded.Unload(true);
        Debug.Log("GMI_BUILD_OK " + bundle + " size=" + new FileInfo(bundle).Length);
        EditorApplication.Exit(0);
    }
}
