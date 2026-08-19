using System.Reflection;
using System.Runtime.InteropServices;

namespace Doorstop;

/// <summary>
/// Doorstop entrypoint used only by the capture configuration. It loads the
/// native in-process probe before handing control to the normal BepInEx
/// IL2CPP entrypoint.
/// </summary>
public static class Entrypoint
{
    [DllImport("GakumasInProcessCapture.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GakumasCaptureRun();

    private static void RestoreNativeAssemblyPath()
    {
        // Cpp2IL must read the captured runtime image, but the IL2CPP P/Invoke
        // resolver must load the module that Unity already mapped. Clearing
        // this supported BepInEx override restores its normal GameAssembly.dll
        // lookup for the native calls after RunCpp2Il returns (also on error).
        Environment.SetEnvironmentVariable("BEPINEX_GAME_ASSEMBLY_PATH", null);
    }

    private static bool SkipSecondPlatformInitialization()
    {
        return false;
    }

    private static (Assembly HarmonyAssembly, Type HarmonyType, Type HarmonyMethodType, object Harmony) LoadHarmony(string gameRoot)
    {
        var harmonyPath = Path.Combine(gameRoot, "BepInEx", "core", "0Harmony.dll");
        var harmonyAssembly = Assembly.LoadFrom(harmonyPath);
        var harmonyType = harmonyAssembly.GetType("HarmonyLib.Harmony", throwOnError: true)!;
        var harmonyMethodType = harmonyAssembly.GetType("HarmonyLib.HarmonyMethod", throwOnError: true)!;
        var harmony = Activator.CreateInstance(harmonyType, new object[] { "Gakumas.InProcessCapture" })
            ?? throw new InvalidOperationException("could not create Harmony instance");
        return (harmonyAssembly, harmonyType, harmonyMethodType, harmony);
    }

    private static void SetPlatformBeforeLoadingHarmony(string gameRoot)
    {
        var platformAssembly = Assembly.LoadFrom(Path.Combine(gameRoot, "BepInEx", "core", "BepInEx.Preloader.Core.dll"));
        var platformType = platformAssembly.GetType("BepInEx.Preloader.Core.PlatformUtils", throwOnError: true)!;
        var setPlatform = platformType.GetMethod("SetPlatform", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)
            ?? throw new MissingMethodException(platformType.FullName, "SetPlatform");
        setPlatform.Invoke(null, null);
    }

    private static void InstallPlatformSetNoopPatch(string gameRoot)
    {
        var (_, harmonyType, harmonyMethodType, harmony) = LoadHarmony(gameRoot);
        var platformAssembly = Assembly.LoadFrom(Path.Combine(gameRoot, "BepInEx", "core", "BepInEx.Preloader.Core.dll"));
        var platformType = platformAssembly.GetType("BepInEx.Preloader.Core.PlatformUtils", throwOnError: true)!;
        var setPlatform = platformType.GetMethod("SetPlatform", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)
            ?? throw new MissingMethodException(platformType.FullName, "SetPlatform");
        var prefixMethod = typeof(Entrypoint).GetMethod(nameof(SkipSecondPlatformInitialization), BindingFlags.NonPublic | BindingFlags.Static)
            ?? throw new MissingMethodException(typeof(Entrypoint).FullName, nameof(SkipSecondPlatformInitialization));
        var prefix = Activator.CreateInstance(harmonyMethodType, new object[] { prefixMethod })
            ?? throw new InvalidOperationException("could not create platform prefix");
        var patch = harmonyType.GetMethods(BindingFlags.Public | BindingFlags.Instance)
            .Single(method => method.Name == "Patch" && method.GetParameters().Length == 6);
        patch.Invoke(harmony, new object?[] { setPlatform, prefix, null, null, null, null });
    }

    private static void InstallNativePathRestorePatch(Assembly originalAssembly)
    {
        var managerType = originalAssembly.GetType("BepInEx.Unity.IL2CPP.Il2CppInteropManager", throwOnError: true)!;
        var runCpp2Il = managerType.GetMethod("RunCpp2Il", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)
            ?? throw new MissingMethodException(managerType.FullName, "RunCpp2Il");

        var gameRoot = Path.GetDirectoryName(Path.GetDirectoryName(Path.GetDirectoryName(originalAssembly.Location)!)!)!;
        var (_, harmonyType, harmonyMethodType, harmony) = LoadHarmony(gameRoot);
        var postfixMethod = typeof(Entrypoint).GetMethod(nameof(RestoreNativeAssemblyPath), BindingFlags.NonPublic | BindingFlags.Static)
            ?? throw new MissingMethodException(typeof(Entrypoint).FullName, nameof(RestoreNativeAssemblyPath));
        var postfix = Activator.CreateInstance(harmonyMethodType, new object[] { postfixMethod })
            ?? throw new InvalidOperationException("could not create Harmony postfix");
        var patch = harmonyType.GetMethods(BindingFlags.Public | BindingFlags.Instance)
            .Single(method => method.Name == "Patch" && method.GetParameters().Length == 6);
        // The platform has already been initialized by this point. The
        // finalizer restores the native path after Cpp2IL, including errors.
        patch.Invoke(harmony, new object?[] { runCpp2Il, null, null, null, postfix, null });
    }

    private static void InstallStrippedCodeRegSupport(string gameRoot, string logPath)
    {
        var corePath = Path.Combine(gameRoot, "BepInEx", "core", "Cpp2IL.Core.dll");
        var pluginPath = Path.Combine(gameRoot, "BepInEx", "gakumas-runtime-capture", "Cpp2IL.Plugin.StrippedCodeRegSupport.dll");
        var coreAssembly = Assembly.LoadFrom(corePath);
        var pluginAssembly = Assembly.LoadFrom(pluginPath);
        var pluginType = pluginAssembly.GetType("Cpp2IL.Plugin.StrippedCodeRegSupport.StrippedCodeRegSupportPlugin", throwOnError: true)!;
        var plugin = Activator.CreateInstance(pluginType)
            ?? throw new InvalidOperationException("could not create stripped code registration support plugin");
        var onLoad = pluginType.GetMethod("OnLoad", BindingFlags.Public | BindingFlags.Instance)
            ?? throw new MissingMethodException(pluginType.FullName, "OnLoad");
        onLoad.Invoke(plugin, null);
        GC.KeepAlive(coreAssembly);
        File.AppendAllText(logPath, $"loaded stripped code registration support: {pluginPath}{Environment.NewLine}");
    }

    public static void Start()
    {
        var processPath = Environment.GetEnvironmentVariable("DOORSTOP_PROCESS_PATH");
        var gameRoot = !string.IsNullOrWhiteSpace(processPath)
            ? Path.GetDirectoryName(processPath)!
            : Environment.CurrentDirectory;

        var logPath = Path.Combine(gameRoot, "BepInEx", "gakumas-runtime-capture", "capture-entrypoint.log");
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(logPath)!);
            File.AppendAllText(logPath, $"entrypoint start {DateTime.UtcNow:O}{Environment.NewLine}");

            var nativePath = Path.Combine(gameRoot, "GakumasInProcessCapture.dll");
            NativeLibrary.Load(nativePath);
            if (GakumasCaptureRun() != 1)
                throw new InvalidOperationException("in-process GameAssembly capture failed; see inprocess-capture.log");

            var capturedPath = Path.Combine(gameRoot, "BepInEx", "gakumas-runtime-capture", "GameAssembly.inprocess.dll");
            Environment.SetEnvironmentVariable("BEPINEX_GAME_ASSEMBLY_PATH", capturedPath);
            File.AppendAllText(logPath, $"native capture complete; BEPINEX_GAME_ASSEMBLY_PATH={capturedPath}{Environment.NewLine}");

            var originalPath = Path.Combine(gameRoot, "BepInEx", "core", "BepInEx.Unity.IL2CPP.dll");
            var originalAssembly = Assembly.LoadFrom(originalPath);
            SetPlatformBeforeLoadingHarmony(gameRoot);
            InstallPlatformSetNoopPatch(gameRoot);
            InstallStrippedCodeRegSupport(gameRoot, logPath);
            InstallNativePathRestorePatch(originalAssembly);
            var originalType = originalAssembly.GetType("Doorstop.Entrypoint", throwOnError: true)!;
            var start = originalType.GetMethod("Start", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)
                ?? throw new MissingMethodException(originalType.FullName, "Start");
            start.Invoke(null, null);
        }
        catch (Exception exception)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(logPath)!);
            File.AppendAllText(logPath, $"entrypoint failed: {exception}{Environment.NewLine}");
            throw;
        }
    }
}
