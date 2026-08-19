# GameAssembly runtime capture

This is an offline-built helper for the protected Windows build. It does not
start the game. It waits for a manually started `gakumas.exe`, waits until the
runtime-populated `GameAssembly.dll` code sections stabilize, then writes a
reconstructed PE image and a section map.

Default output:

```text
D:\Games\gakumas\BepInEx\gakumas-runtime-capture\capture.log
D:\Games\gakumas\BepInEx\gakumas-runtime-capture\GameAssembly.runtime.dll
D:\Games\gakumas\BepInEx\gakumas-runtime-capture\GameAssembly.runtime.map.json
```

Run the helper first, then start the game manually:

```powershell
& 'D:\Games\gakumas\GakumasRuntimeCapture-v3.exe'
```

The v3/v4 helper requests administrator elevation because the game process
rejects ordinary `OpenProcess` calls. Accept the UAC prompt. v4 additionally
scans committed memory images when both normal module enumeration APIs return
`ERROR_PARTIAL_COPY` (299). It can attach to a game process that is already
running.

The helper exits after a successful capture or a 180-second timeout. It does
not change the original `GameAssembly.dll`.

## In-process capture (current route)

`GameAssemblySelfCapture.cpp` is a native DLL that reads the already-loaded
`GameAssembly.dll` from inside the game process. It does not use
`ReadProcessMemory`, so it avoids the access-denied failure seen with the
external probes. `CaptureEntrypoint.cs` is a temporary Doorstop entrypoint: it
completes the native capture first, sets BepInEx's supported
`BEPINEX_GAME_ASSEMBLY_PATH` environment variable to the captured image, and
only then invokes the original BepInEx IL2CPP entrypoint.

The capture entrypoint first runs BepInEx's own platform initialization,
then installs the temporary hooks and loads
`Cpp2IL.Plugin.StrippedCodeRegSupport.dll`. The original BepInEx entrypoint's
second platform call is suppressed. This ordering is required because loading
Harmony or the stripped-code plugin before platform initialization makes the
preloader abort with `PlatformHelper.Current once it has been accessed`.

Build from a Visual Studio Developer PowerShell:

```powershell
cl.exe /nologo /std:c++17 /EHsc /LD /O2 /W4 /DUNICODE /D_UNICODE `
  /Fe:GameAssemblySelfCapture.dll GameAssemblySelfCapture.cpp
dotnet build .\CaptureEntrypoint.csproj --configuration Release
```

The deployed files are `GakumasInProcessCapture.dll` and
`GakumasCaptureEntrypoint.dll` in the game root, plus
`doorstop_config.capture.ini`. The active `doorstop_config.ini` is backed up
as `doorstop_config.ini.before-inprocess-capture` before switching to the
capture entrypoint. The probe writes its result and log under
`BepInEx\gakumas-runtime-capture\`.

This mode is intentionally a one-shot diagnostic. After capture, restore the
original Doorstop configuration before normal play:

```powershell
Copy-Item -LiteralPath .\doorstop_config.ini.before-inprocess-capture `
  -Destination .\doorstop_config.ini -Force
```
